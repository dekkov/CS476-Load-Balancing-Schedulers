/* hedera-leaf-spine.cc
 * CS476 — Hedera load-balancing simulation on a leaf-spine topology.
 *
 * Topology: 8 hosts, 4 leaves, 2 spines (full mesh leaf-spine).
 * Traffic:  6 elephant flows + 8 mice flows.
 * Output:   FlowMonitor XML, spine trace CSV, controller log, routing tables.
 *
 * Extends ECMP with a centralized Hedera controller that detects elephant
 * flows and reroutes them via Global First Fit to balance spine utilization.
 */

#include "ns3/applications-module.h"
#include "ns3/core-module.h"
#include "ns3/flow-monitor-helper.h"
#include "ns3/flow-monitor.h"
#include "ns3/ipv4-flow-classifier.h"
#include "ns3/ipv4-global-routing.h"
#include "ns3/internet-module.h"
#include "ns3/network-module.h"
#include "ns3/point-to-point-module.h"

#include <algorithm>
#include <fstream>
#include <iomanip>
#include <map>
#include <sstream>
#include <vector>

using namespace ns3;

NS_LOG_COMPONENT_DEFINE("HederaLeafSpine");

// ---------- Spine trace callback ----------

static std::ofstream g_spineTraceFile;

void
SpineTxCallback(uint32_t spineId,
                Ptr<const Packet> packet,
                Ptr<Ipv4> ipv4,
                uint32_t interface)
{
    Ipv4Header ipHeader;
    Ptr<Packet> copy = packet->Copy();
    copy->RemoveHeader(ipHeader);

    uint16_t srcPort = 0;
    uint16_t dstPort = 0;
    uint8_t protocol = ipHeader.GetProtocol();

    if (protocol == 6) // TCP
    {
        TcpHeader tcpHeader;
        if (copy->PeekHeader(tcpHeader))
        {
            srcPort = tcpHeader.GetSourcePort();
            dstPort = tcpHeader.GetDestinationPort();
        }
    }
    else if (protocol == 17) // UDP
    {
        UdpHeader udpHeader;
        if (copy->PeekHeader(udpHeader))
        {
            srcPort = udpHeader.GetSourcePort();
            dstPort = udpHeader.GetDestinationPort();
        }
    }

    g_spineTraceFile << Simulator::Now().GetNanoSeconds() << ","
                     << ipHeader.GetSource() << ","
                     << ipHeader.GetDestination() << ","
                     << srcPort << ","
                     << dstPort << ","
                     << spineId << "\n";
}

// ---------- Helper: install BulkSend + PacketSink ----------

void
InstallFlow(Ptr<Node> srcNode,
            Ptr<Node> dstNode,
            Ipv4Address dstAddr,
            uint16_t port,
            uint32_t maxBytes,
            double startTime)
{
    // Sink on destination
    PacketSinkHelper sinkHelper("ns3::TcpSocketFactory",
                                InetSocketAddress(Ipv4Address::GetAny(), port));
    ApplicationContainer sinkApp = sinkHelper.Install(dstNode);
    sinkApp.Start(Seconds(0.0));

    // BulkSend on source
    BulkSendHelper sourceHelper("ns3::TcpSocketFactory",
                                InetSocketAddress(dstAddr, port));
    sourceHelper.SetAttribute("MaxBytes", UintegerValue(maxBytes));
    sourceHelper.SetAttribute("SendSize", UintegerValue(1448));
    ApplicationContainer sourceApp = sourceHelper.Install(srcNode);
    sourceApp.Start(Seconds(startTime));
}

// ---------- Hash helper (must match LookupGlobal exactly) ----------

uint32_t
ComputeFlowHash(uint32_t srcAddr, uint32_t dstAddr,
                uint8_t protocol, uint16_t srcPort, uint16_t dstPort)
{
    return srcAddr * 2654435761u ^ dstAddr * 2246822519u
           ^ (static_cast<uint32_t>(protocol) << 16)
           ^ (static_cast<uint32_t>(srcPort) << 8)
           ^ static_cast<uint32_t>(dstPort);
}

// ---------- Controller state ----------

static Ptr<FlowMonitor> g_flowMonitor;
static Ptr<Ipv4FlowClassifier> g_classifier;
static uint32_t g_numSpines = 2;
static std::map<uint32_t, uint64_t> g_prevFlowBytes; // hash -> bytes at last epoch
static std::ofstream g_controllerLog;
static double g_hederaEpoch = 1.0;
static uint64_t g_elephantThreshold = 10 * 1024 * 1024; // 10 MB detection threshold

// ---------- Hedera Controller Epoch ----------

struct ElephantInfo
{
    uint32_t hash;
    FlowMonitor::FlowStats stats;
    Ipv4FlowClassifier::FiveTuple tuple;
    double rate; // bytes per second
    uint32_t currentSpine;
};

void
HederaControllerEpoch()
{
    double now = Simulator::Now().GetSeconds();
    NS_LOG_INFO("Hedera controller epoch at t=" << now << "s");

    FlowMonitor::FlowStatsContainer flowStats = g_flowMonitor->GetFlowStats();

    // Step 1: Detect elephants and compute rates
    std::vector<ElephantInfo> elephants;

    for (auto it = flowStats.begin(); it != flowStats.end(); ++it)
    {
        Ipv4FlowClassifier::FiveTuple t = g_classifier->FindFlow(it->first);
        uint32_t hash = ComputeFlowHash(
            t.sourceAddress.Get(), t.destinationAddress.Get(),
            t.protocol, t.sourcePort, t.destinationPort);

        if (it->second.txBytes > g_elephantThreshold)
        {
            uint64_t prevBytes = 0;
            auto prevIt = g_prevFlowBytes.find(hash);
            if (prevIt != g_prevFlowBytes.end())
            {
                prevBytes = prevIt->second;
            }

            double rate = 0.0;
            if (g_hederaEpoch > 0)
            {
                rate = static_cast<double>(it->second.txBytes - prevBytes) / g_hederaEpoch;
            }

            // Determine current spine assignment from hash
            uint32_t currentSpine = hash % g_numSpines;

            ElephantInfo info;
            info.hash = hash;
            info.stats = it->second;
            info.tuple = t;
            info.rate = rate;
            info.currentSpine = currentSpine;
            elephants.push_back(info);
        }

        g_prevFlowBytes[hash] = it->second.txBytes;
    }

    if (elephants.empty())
    {
        NS_LOG_INFO("No elephants detected this epoch.");
        Simulator::Schedule(Seconds(g_hederaEpoch), &HederaControllerEpoch);
        return;
    }

    // Step 2: Sort elephants by rate descending
    std::sort(elephants.begin(), elephants.end(),
              [](const ElephantInfo& a, const ElephantInfo& b) {
                  return a.rate > b.rate;
              });

    // Step 3: Global First Fit placement
    std::vector<double> spineLoad(g_numSpines, 0.0);

    // First pass: account for non-elephant background load (not modeled, assume 0)
    // Place elephants using first-fit
    for (auto& e : elephants)
    {
        uint32_t bestSpine = 0;
        double bestLoad = spineLoad[0];

        // Find spine with least load (first fit on least-loaded)
        for (uint32_t s = 0; s < g_numSpines; ++s)
        {
            if (spineLoad[s] < bestLoad)
            {
                bestLoad = spineLoad[s];
                bestSpine = s;
            }
        }

        uint32_t oldSpine = e.currentSpine;
        e.currentSpine = bestSpine;
        spineLoad[bestSpine] += e.rate;

        // Set override
        Ipv4GlobalRouting::SetFlowOverride(e.hash, bestSpine);

        double rateMbps = (e.rate * 8.0) / 1e6;

        NS_LOG_INFO("  Elephant: " << e.tuple.sourceAddress << ":" << e.tuple.sourcePort
                    << " -> " << e.tuple.destinationAddress << ":" << e.tuple.destinationPort
                    << " rate=" << rateMbps << "Mbps"
                    << " spine " << oldSpine << " -> " << bestSpine);

        g_controllerLog << std::fixed << std::setprecision(3)
                        << now << ","
                        << e.hash << ","
                        << e.tuple.sourceAddress << ","
                        << e.tuple.destinationAddress << ","
                        << e.tuple.sourcePort << ","
                        << e.tuple.destinationPort << ","
                        << oldSpine << ","
                        << bestSpine << ","
                        << rateMbps << "\n";
    }

    g_controllerLog.flush();

    NS_LOG_INFO("Spine loads after placement:");
    for (uint32_t s = 0; s < g_numSpines; ++s)
    {
        NS_LOG_INFO("  Spine " << s << ": " << (spineLoad[s] * 8.0 / 1e6) << " Mbps");
    }

    // Reschedule next epoch
    Simulator::Schedule(Seconds(g_hederaEpoch), &HederaControllerEpoch);
}

// ---------- Main ----------

int
main(int argc, char* argv[])
{
    // --- Command-line arguments ---
    uint32_t enableHedera = 1; // 0=ECMP-only baseline, 1=Hedera
    double hederaEpoch = 1.0;
    uint64_t elephantThreshold = 10 * 1024 * 1024; // 10 MB
    std::string outputDir = "results-hedera";

    CommandLine cmd;
    cmd.AddValue("enableHedera", "0=ECMP-only baseline, 1=Hedera", enableHedera);
    cmd.AddValue("hederaEpoch", "Controller epoch in seconds", hederaEpoch);
    cmd.AddValue("elephantThreshold", "Elephant detection threshold in bytes", elephantThreshold);
    cmd.AddValue("outputDir", "Output directory for results", outputDir);
    cmd.Parse(argc, argv);

    g_hederaEpoch = hederaEpoch;
    g_elephantThreshold = elephantThreshold;

    // --- TCP configuration ---
    Config::SetDefault("ns3::TcpSocket::SegmentSize", UintegerValue(1448));
    Config::SetDefault("ns3::TcpL4Protocol::SocketType", StringValue("ns3::TcpCubic"));

    // --- ECMP: always use per-flow hash (mode 2) ---
    Config::SetDefault("ns3::Ipv4GlobalRouting::FlowEcmpRouting", BooleanValue(true));

    // Clear any previous overrides
    Ipv4GlobalRouting::ClearAllOverrides();

    // ========== TOPOLOGY ==========

    // Create nodes
    NodeContainer hosts;
    hosts.Create(8);
    NodeContainer leaves;
    leaves.Create(4);
    NodeContainer spines;
    spines.Create(2);

    // Install Internet stack on all nodes
    InternetStackHelper internet;
    internet.Install(hosts);
    internet.Install(leaves);
    internet.Install(spines);

    // --- Point-to-point helpers ---
    PointToPointHelper hostLeafLink;
    hostLeafLink.SetDeviceAttribute("DataRate", StringValue("1Gbps"));
    hostLeafLink.SetChannelAttribute("Delay", StringValue("5us"));

    PointToPointHelper leafSpineLink;
    leafSpineLink.SetDeviceAttribute("DataRate", StringValue("1Gbps"));
    leafSpineLink.SetChannelAttribute("Delay", StringValue("5us"));

    Ipv4AddressHelper address;

    // --- Host-Leaf links ---
    // hosts[0-1] -> leaf0, hosts[2-3] -> leaf1, hosts[4-5] -> leaf2, hosts[6-7] -> leaf3
    NetDeviceContainer hostLeafDevices[8];
    Ipv4InterfaceContainer hostLeafInterfaces[8];

    for (uint32_t leaf = 0; leaf < 4; ++leaf)
    {
        for (uint32_t h = 0; h < 2; ++h)
        {
            uint32_t hostIdx = leaf * 2 + h;
            hostLeafDevices[hostIdx] =
                hostLeafLink.Install(hosts.Get(hostIdx), leaves.Get(leaf));

            std::ostringstream subnet;
            subnet << "10." << leaf << "." << h << ".0";
            address.SetBase(subnet.str().c_str(), "255.255.255.0");
            hostLeafInterfaces[hostIdx] = address.Assign(hostLeafDevices[hostIdx]);
        }
    }

    // --- Leaf-Spine links (full mesh: 4 leaves x 2 spines = 8 links) ---
    NetDeviceContainer leafSpineDevices[8];
    Ipv4InterfaceContainer leafSpineInterfaces[8];
    uint32_t linkId = 0;

    for (uint32_t leaf = 0; leaf < 4; ++leaf)
    {
        for (uint32_t spine = 0; spine < 2; ++spine)
        {
            leafSpineDevices[linkId] =
                leafSpineLink.Install(leaves.Get(leaf), spines.Get(spine));

            std::ostringstream subnet;
            subnet << "10.10." << linkId << ".0";
            address.SetBase(subnet.str().c_str(), "255.255.255.0");
            leafSpineInterfaces[linkId] = address.Assign(leafSpineDevices[linkId]);
            ++linkId;
        }
    }

    // --- Populate routing tables ---
    Ipv4GlobalRoutingHelper::PopulateRoutingTables();

    // --- Dump routing tables for diagnostics ---
    std::string routeFile = outputDir + "/routing-tables.txt";
    Ptr<OutputStreamWrapper> routeStream = Create<OutputStreamWrapper>(routeFile, std::ios::out);
    Ipv4GlobalRoutingHelper::PrintRoutingTableAllAt(Seconds(0.4), routeStream);

    // ========== TRAFFIC ==========

    Ipv4Address hostAddr[8];
    for (uint32_t i = 0; i < 8; ++i)
    {
        hostAddr[i] = hostLeafInterfaces[i].GetAddress(0);
    }

    // --- 6 Elephant flows: 500 MB each, start at 0.5s ---
    // All-odd destination ports force all 6 flows to hash to spine 1 under ECMP,
    // creating a 6-0 collision. Hedera redistributes to 3-3.
    uint32_t elephantBytes = 500 * 1024 * 1024; // 500 MB

    // E1: host0 (leaf0) -> host4 (leaf2), port 9001
    InstallFlow(hosts.Get(0), hosts.Get(4), hostAddr[4], 9001, elephantBytes, 0.5);
    // E2: host0 (leaf0) -> host5 (leaf2), port 9003
    InstallFlow(hosts.Get(0), hosts.Get(5), hostAddr[5], 9003, elephantBytes, 0.5);
    // E3: host1 (leaf0) -> host6 (leaf3), port 9005
    InstallFlow(hosts.Get(1), hosts.Get(6), hostAddr[6], 9005, elephantBytes, 0.5);
    // E4: host1 (leaf0) -> host7 (leaf3), port 9007
    InstallFlow(hosts.Get(1), hosts.Get(7), hostAddr[7], 9007, elephantBytes, 0.5);
    // E5: host2 (leaf1) -> host4 (leaf2), port 9009
    InstallFlow(hosts.Get(2), hosts.Get(4), hostAddr[4], 9009, elephantBytes, 0.5);
    // E6: host3 (leaf1) -> host7 (leaf3), port 9011
    InstallFlow(hosts.Get(3), hosts.Get(7), hostAddr[7], 9011, elephantBytes, 0.5);

    // --- 8 Mice flows: 100 KB each ---
    uint32_t mouseBytes = 100 * 1024; // 100 KB

    // M1: host2 (leaf1) -> host5 (leaf2), port 11, t=1.0s
    InstallFlow(hosts.Get(2), hosts.Get(5), hostAddr[5], 11, mouseBytes, 1.0);
    // M2: host3 (leaf1) -> host7 (leaf3), port 12, t=1.0s
    InstallFlow(hosts.Get(3), hosts.Get(7), hostAddr[7], 12, mouseBytes, 1.0);
    // M3: host4 (leaf2) -> host1 (leaf0), port 13, t=1.0s
    InstallFlow(hosts.Get(4), hosts.Get(1), hostAddr[1], 13, mouseBytes, 1.0);
    // M4: host5 (leaf2) -> host0 (leaf0), port 14, t=1.0s
    InstallFlow(hosts.Get(5), hosts.Get(0), hostAddr[0], 14, mouseBytes, 1.0);
    // M5: host6 (leaf3) -> host3 (leaf1), port 15, t=1.1s
    InstallFlow(hosts.Get(6), hosts.Get(3), hostAddr[3], 15, mouseBytes, 1.1);
    // M6: host7 (leaf3) -> host2 (leaf1), port 16, t=1.1s
    InstallFlow(hosts.Get(7), hosts.Get(2), hostAddr[2], 16, mouseBytes, 1.1);
    // M7: host0 (leaf0) -> host7 (leaf3), port 17, t=1.2s
    InstallFlow(hosts.Get(0), hosts.Get(7), hostAddr[7], 17, mouseBytes, 1.2);
    // M8: host1 (leaf0) -> host5 (leaf2), port 18, t=1.2s
    InstallFlow(hosts.Get(1), hosts.Get(5), hostAddr[5], 18, mouseBytes, 1.2);

    // ========== MEASUREMENT ==========

    // --- FlowMonitor ---
    FlowMonitorHelper flowHelper;
    g_flowMonitor = flowHelper.InstallAll();
    g_classifier = DynamicCast<Ipv4FlowClassifier>(flowHelper.GetClassifier());

    // --- Spine trace ---
    std::string spineTraceFile = outputDir + "/spine-trace.csv";
    g_spineTraceFile.open(spineTraceFile);
    g_spineTraceFile << "timestamp_ns,src_ip,dst_ip,src_port,dst_port,spine_id\n";

    for (uint32_t s = 0; s < spines.GetN(); ++s)
    {
        Ptr<Ipv4> ipv4 = spines.Get(s)->GetObject<Ipv4>();
        ipv4->TraceConnectWithoutContext(
            "Tx",
            MakeBoundCallback(&SpineTxCallback, s));
    }

    // --- Hedera controller ---
    if (enableHedera)
    {
        std::string controllerLogFile = outputDir + "/hedera-controller.csv";
        g_controllerLog.open(controllerLogFile);
        g_controllerLog << "epoch_s,flowHash,srcAddr,dstAddr,srcPort,dstPort,oldSpine,newSpine,rate_mbps\n";

        // Schedule first controller epoch
        Simulator::Schedule(Seconds(hederaEpoch + 0.5), &HederaControllerEpoch);
        NS_LOG_INFO("Hedera controller enabled, epoch=" << hederaEpoch << "s, threshold=" << elephantThreshold << "B");
    }
    else
    {
        NS_LOG_INFO("Hedera disabled, running ECMP-only baseline");
    }

    // ========== RUN ==========

    Simulator::Stop(Seconds(30.0));
    Simulator::Run();

    // --- Save FlowMonitor results ---
    std::string flowmonFile = outputDir + "/flowmon-hedera.xml";
    g_flowMonitor->SerializeToXmlFile(flowmonFile, true, true);

    g_spineTraceFile.close();
    if (enableHedera)
    {
        g_controllerLog.close();
    }

    Simulator::Destroy();

    std::cout << "Simulation complete. Results in: " << outputDir << "/" << std::endl;
    return 0;
}
