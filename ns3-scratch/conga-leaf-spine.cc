/* conga-leaf-spine.cc
 * CS476 — CONGA (Congestion-Aware) load-balancing simulation on a leaf-spine topology.
 *
 * Topology: 8 hosts, 4 leaves, 2 spines (full mesh leaf-spine).
 * Traffic:  6 elephant flows + 8 mice flows (same as Hedera).
 * Output:   FlowMonitor XML, spine trace CSV, CONGA log CSV, routing tables.
 *
 * Implements a simulation-level CONGA:
 *   - Flowlet-based switching: per-flow last-seen timestamps; if inter-arrival
 *     gap > flowletGap (default 300us), the next packet starts a new flowlet
 *     and can be reassigned to a different spine.
 *   - Congestion metric: CE = max(link_utilization, queue_depth_normalized)
 *     measured on each leaf->spine uplink via TrafficControlHelper queue discs.
 *   - Distributed: each leaf independently picks the least-congested spine for
 *     each new flowlet.
 *   - Feedback delay: congestion estimates are aged by a configurable RTT
 *     (default 100us) to model piggybacking delay.
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
#include "ns3/traffic-control-module.h"

#include <algorithm>
#include <fstream>
#include <iomanip>
#include <map>
#include <sstream>
#include <vector>

using namespace ns3;

NS_LOG_COMPONENT_DEFINE("CongaLeafSpine");

// ========== Constants ==========

static const uint32_t NUM_HOSTS  = 8;
static const uint32_t NUM_LEAVES = 4;
static const uint32_t NUM_SPINES = 2;
static const uint32_t HOSTS_PER_LEAF = 2;

// ========== Spine trace callback ==========

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

// ========== Helper: install BulkSend + PacketSink ==========

void
InstallFlow(Ptr<Node> srcNode,
            Ptr<Node> dstNode,
            Ipv4Address dstAddr,
            uint16_t port,
            uint32_t maxBytes,
            double startTime)
{
    PacketSinkHelper sinkHelper("ns3::TcpSocketFactory",
                                InetSocketAddress(Ipv4Address::GetAny(), port));
    ApplicationContainer sinkApp = sinkHelper.Install(dstNode);
    sinkApp.Start(Seconds(0.0));

    BulkSendHelper sourceHelper("ns3::TcpSocketFactory",
                                InetSocketAddress(dstAddr, port));
    sourceHelper.SetAttribute("MaxBytes", UintegerValue(maxBytes));
    sourceHelper.SetAttribute("SendSize", UintegerValue(1448));
    ApplicationContainer sourceApp = sourceHelper.Install(srcNode);
    sourceApp.Start(Seconds(startTime));
}

// ========== Hash helper (must match LookupGlobal exactly) ==========

uint32_t
ComputeFlowHash(uint32_t srcAddr, uint32_t dstAddr,
                uint8_t protocol, uint16_t srcPort, uint16_t dstPort)
{
    return srcAddr * 2654435761u ^ dstAddr * 2246822519u
           ^ (static_cast<uint32_t>(protocol) << 16)
           ^ (static_cast<uint32_t>(srcPort) << 8)
           ^ static_cast<uint32_t>(dstPort);
}

// ========== CONGA State ==========

// Flowlet table entry: tracks last-seen time and assigned spine per flow
struct FlowletEntry
{
    int64_t  lastSeenNs;    // last packet timestamp (ns)
    uint32_t assignedSpine; // current spine assignment
};

// Congestion estimate for one leaf->spine uplink
struct CongestionEstimate
{
    double utilization;     // bytes_last_interval / (bandwidth * interval)
    double queueDepth;      // queue_packets / queue_max_packets
    double ce;              // max(utilization, queueDepth)
    int64_t lastUpdateNs;   // when this estimate was last updated
};

// Per-leaf CONGA state
struct LeafCongaState
{
    // CE table: indexed by spine id
    CongestionEstimate ceTable[NUM_SPINES];
    // Byte counters for utilization measurement per uplink (spine index)
    uint64_t bytesSent[NUM_SPINES];
    uint64_t bytesSentPrev[NUM_SPINES];
};

// Global CONGA state
static std::map<uint32_t, FlowletEntry> g_flowletTable;
static LeafCongaState g_leafState[NUM_LEAVES];

// Queue disc pointers for each leaf->spine link: [leaf][spine]
static Ptr<QueueDisc> g_leafSpineQueueDisc[NUM_LEAVES][NUM_SPINES];
// NetDevice pointers for byte counting: [leaf][spine]
static Ptr<NetDevice> g_leafSpineDevice[NUM_LEAVES][NUM_SPINES];

// CONGA parameters
static double   g_congaTickInterval = 100e-6;  // 100 us tick (must be < flowlet gap)
static double   g_flowletGap        = 300e-6;  // 300 us flowlet gap
static double   g_feedbackDelay     = 100e-6;  // 100 us RTT feedback delay
static double   g_linkBandwidthBps  = 1e9;     // 1 Gbps leaf-spine links
static uint32_t g_queueMaxPackets   = 100;     // queue disc max size

// FlowMonitor for flow tracking
static Ptr<FlowMonitor> g_flowMonitor;
static Ptr<Ipv4FlowClassifier> g_classifier;

// CONGA log
static std::ofstream g_congaLog;
static uint64_t g_congaTicks = 0;

// ========== Leaf uplink byte counter callback ==========

// Track bytes sent on each leaf->spine uplink via Tx trace
void
LeafSpineTxByteCallback(uint32_t leafId, uint32_t spineId,
                        Ptr<const Packet> packet)
{
    g_leafState[leafId].bytesSent[spineId] += packet->GetSize();
}

// ========== CONGA Tick ==========

void
CongaTick()
{
    int64_t nowNs = Simulator::Now().GetNanoSeconds();
    double nowS = Simulator::Now().GetSeconds();
    double interval = g_congaTickInterval;
    g_congaTicks++;

    // --- Step 1: Update congestion estimates for each leaf ---
    for (uint32_t leaf = 0; leaf < NUM_LEAVES; ++leaf)
    {
        for (uint32_t spine = 0; spine < NUM_SPINES; ++spine)
        {
            // Utilization: bytes sent in last interval / (bandwidth * interval)
            uint64_t bytesDelta = g_leafState[leaf].bytesSent[spine]
                                  - g_leafState[leaf].bytesSentPrev[spine];
            double util = (bytesDelta * 8.0) / (g_linkBandwidthBps * interval);
            util = std::min(util, 1.0);

            // Queue depth: current queue occupancy / max
            double qDepth = 0.0;
            if (g_leafSpineQueueDisc[leaf][spine])
            {
                uint32_t nPkts = g_leafSpineQueueDisc[leaf][spine]->GetNPackets();
                qDepth = static_cast<double>(nPkts) / g_queueMaxPackets;
                qDepth = std::min(qDepth, 1.0);
            }

            // CE = max(utilization, queue_depth)
            double ce = std::max(util, qDepth);

            // Apply feedback delay: only update if enough time has passed
            int64_t delayNs = static_cast<int64_t>(g_feedbackDelay * 1e9);
            if ((nowNs - g_leafState[leaf].ceTable[spine].lastUpdateNs) >= delayNs)
            {
                g_leafState[leaf].ceTable[spine].utilization = util;
                g_leafState[leaf].ceTable[spine].queueDepth = qDepth;
                g_leafState[leaf].ceTable[spine].ce = ce;
                g_leafState[leaf].ceTable[spine].lastUpdateNs = nowNs;
            }

            // Save counters for next interval
            g_leafState[leaf].bytesSentPrev[spine] = g_leafState[leaf].bytesSent[spine];
        }
    }

    // --- Step 2: Process flowlets ---
    FlowMonitor::FlowStatsContainer flowStats = g_flowMonitor->GetFlowStats();

    for (auto it = flowStats.begin(); it != flowStats.end(); ++it)
    {
        if (it->second.txBytes < 1000)
        {
            continue; // skip tiny flows / ACK-only
        }

        Ipv4FlowClassifier::FiveTuple t = g_classifier->FindFlow(it->first);
        uint32_t hash = ComputeFlowHash(
            t.sourceAddress.Get(), t.destinationAddress.Get(),
            t.protocol, t.sourcePort, t.destinationPort);

        // Determine source leaf from IP: 10.<leaf>.<h>.1
        // Source address format: 10.L.H.1 where L is the leaf index
        // Ipv4Address::Get() returns host-byte-order: (10<<24)|(L<<16)|(H<<8)|1
        uint32_t srcAddrRaw = t.sourceAddress.Get();
        // Extract second octet (leaf index) from bits 16-23
        uint32_t srcLeaf = (srcAddrRaw >> 16) & 0xFF;
        if (srcLeaf >= NUM_LEAVES)
        {
            continue; // skip non-host flows (e.g. 10.10.x.x leaf-spine subnets)
        }

        // Get last packet time from FlowMonitor
        int64_t lastRxNs = it->second.timeLastTxPacket.GetNanoSeconds();

        auto fIt = g_flowletTable.find(hash);

        if (fIt == g_flowletTable.end())
        {
            // New flow: assign to least-congested spine
            uint32_t bestSpine = 0;
            double bestCE = g_leafState[srcLeaf].ceTable[0].ce;
            for (uint32_t s = 1; s < NUM_SPINES; ++s)
            {
                if (g_leafState[srcLeaf].ceTable[s].ce < bestCE)
                {
                    bestCE = g_leafState[srcLeaf].ceTable[s].ce;
                    bestSpine = s;
                }
            }

            FlowletEntry entry;
            entry.lastSeenNs = lastRxNs;
            entry.assignedSpine = bestSpine;
            g_flowletTable[hash] = entry;

            Ipv4GlobalRouting::SetFlowOverride(hash, bestSpine);
        }
        else
        {
            // Existing flow: check for flowlet gap
            int64_t gapNs = lastRxNs - fIt->second.lastSeenNs;
            double gapS = gapNs / 1e9;

            if (gapS > g_flowletGap)
            {
                // Flowlet boundary detected: reassign to least-congested spine
                uint32_t oldSpine = fIt->second.assignedSpine;
                uint32_t bestSpine = 0;
                double bestCE = g_leafState[srcLeaf].ceTable[0].ce;
                for (uint32_t s = 1; s < NUM_SPINES; ++s)
                {
                    if (g_leafState[srcLeaf].ceTable[s].ce < bestCE)
                    {
                        bestCE = g_leafState[srcLeaf].ceTable[s].ce;
                        bestSpine = s;
                    }
                }

                fIt->second.assignedSpine = bestSpine;
                fIt->second.lastSeenNs = lastRxNs;

                Ipv4GlobalRouting::SetFlowOverride(hash, bestSpine);

                // Log reassignment (only for flows > 100KB to keep log manageable)
                if (oldSpine != bestSpine && it->second.txBytes > 100 * 1024)
                {
                    double rateMbps = 0.0;
                    if (it->second.txBytes > 0 && it->second.timeLastTxPacket > it->second.timeFirstTxPacket)
                    {
                        double duration = (it->second.timeLastTxPacket - it->second.timeFirstTxPacket).GetSeconds();
                        if (duration > 0)
                        {
                            rateMbps = (it->second.txBytes * 8.0) / (duration * 1e6);
                        }
                    }

                    g_congaLog << std::fixed << std::setprecision(6)
                               << nowS << ","
                               << hash << ","
                               << t.sourceAddress << ","
                               << t.destinationAddress << ","
                               << t.sourcePort << ","
                               << t.destinationPort << ","
                               << oldSpine << ","
                               << bestSpine << ","
                               << std::setprecision(3) << rateMbps << ","
                               << std::setprecision(4)
                               << g_leafState[srcLeaf].ceTable[oldSpine].ce << ","
                               << g_leafState[srcLeaf].ceTable[bestSpine].ce << "\n";
                }
            }
            else
            {
                // Same flowlet: update timestamp, keep spine
                fIt->second.lastSeenNs = lastRxNs;
            }
        }
    }

    // --- Step 3: Periodic log of congestion state (every 1000 ticks ~ 0.3s) ---
    if (g_congaTicks % 1000 == 0)
    {
        NS_LOG_INFO("CONGA t=" << nowS << "s  flows=" << g_flowletTable.size());
        for (uint32_t leaf = 0; leaf < NUM_LEAVES; ++leaf)
        {
            for (uint32_t spine = 0; spine < NUM_SPINES; ++spine)
            {
                NS_LOG_INFO("  Leaf" << leaf << "->Spine" << spine
                            << " util=" << g_leafState[leaf].ceTable[spine].utilization
                            << " qDepth=" << g_leafState[leaf].ceTable[spine].queueDepth
                            << " CE=" << g_leafState[leaf].ceTable[spine].ce);
            }
        }
    }

    // Reschedule
    Simulator::Schedule(Seconds(g_congaTickInterval), &CongaTick);
}

// ========== Main ==========

int
main(int argc, char* argv[])
{
    // --- Command-line arguments ---
    uint32_t enableConga = 1;          // 0=ECMP-only baseline, 1=CONGA
    double flowletGap = 300e-6;        // 300 us
    double feedbackDelay = 100e-6;     // 100 us
    double congaTickInterval = 100e-6; // 100 us (must be < flowlet gap)
    uint32_t queueMaxPackets = 100;
    std::string outputDir = "results-conga";

    CommandLine cmd;
    cmd.AddValue("enableConga", "0=ECMP-only baseline, 1=CONGA", enableConga);
    cmd.AddValue("flowletGap", "Flowlet gap threshold in seconds", flowletGap);
    cmd.AddValue("feedbackDelay", "RTT feedback delay in seconds", feedbackDelay);
    cmd.AddValue("congaTickInterval", "CONGA tick interval in seconds", congaTickInterval);
    cmd.AddValue("queueMaxPackets", "Queue disc max packets", queueMaxPackets);
    cmd.AddValue("outputDir", "Output directory for results", outputDir);
    cmd.Parse(argc, argv);

    g_flowletGap = flowletGap;
    g_feedbackDelay = feedbackDelay;
    g_congaTickInterval = congaTickInterval;
    g_queueMaxPackets = queueMaxPackets;

    // --- TCP configuration ---
    Config::SetDefault("ns3::TcpSocket::SegmentSize", UintegerValue(1448));
    Config::SetDefault("ns3::TcpL4Protocol::SocketType", StringValue("ns3::TcpCubic"));

    // --- ECMP: per-flow hash ---
    Config::SetDefault("ns3::Ipv4GlobalRouting::FlowEcmpRouting", BooleanValue(true));

    // Clear any previous overrides
    Ipv4GlobalRouting::ClearAllOverrides();

    // ========== TOPOLOGY ==========

    NodeContainer hosts;
    hosts.Create(NUM_HOSTS);
    NodeContainer leaves;
    leaves.Create(NUM_LEAVES);
    NodeContainer spines;
    spines.Create(NUM_SPINES);

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
    NetDeviceContainer hostLeafDevices[NUM_HOSTS];
    Ipv4InterfaceContainer hostLeafInterfaces[NUM_HOSTS];

    for (uint32_t leaf = 0; leaf < NUM_LEAVES; ++leaf)
    {
        for (uint32_t h = 0; h < HOSTS_PER_LEAF; ++h)
        {
            uint32_t hostIdx = leaf * HOSTS_PER_LEAF + h;
            hostLeafDevices[hostIdx] =
                hostLeafLink.Install(hosts.Get(hostIdx), leaves.Get(leaf));

            std::ostringstream subnet;
            subnet << "10." << leaf << "." << h << ".0";
            address.SetBase(subnet.str().c_str(), "255.255.255.0");
            hostLeafInterfaces[hostIdx] = address.Assign(hostLeafDevices[hostIdx]);
        }
    }

    // --- Leaf-Spine links (full mesh: 4 leaves x 2 spines = 8 links) ---
    NetDeviceContainer leafSpineDevices[NUM_LEAVES * NUM_SPINES];
    Ipv4InterfaceContainer leafSpineInterfaces[NUM_LEAVES * NUM_SPINES];
    uint32_t linkId = 0;

    // Queue disc helpers for congestion measurement on leaf->spine uplinks
    TrafficControlHelper tchUninstall;
    TrafficControlHelper tch;
    tch.SetRootQueueDisc("ns3::FifoQueueDisc", "MaxSize",
                         QueueSizeValue(QueueSize(QueueSizeUnit::PACKETS, queueMaxPackets)));

    for (uint32_t leaf = 0; leaf < NUM_LEAVES; ++leaf)
    {
        for (uint32_t spine = 0; spine < NUM_SPINES; ++spine)
        {
            leafSpineDevices[linkId] =
                leafSpineLink.Install(leaves.Get(leaf), spines.Get(spine));

            std::ostringstream subnet;
            subnet << "10.10." << linkId << ".0";
            address.SetBase(subnet.str().c_str(), "255.255.255.0");
            leafSpineInterfaces[linkId] = address.Assign(leafSpineDevices[linkId]);

            // Replace default queue disc with our FIFO on the leaf-side device
            Ptr<NetDevice> leafDev = leafSpineDevices[linkId].Get(0);
            tchUninstall.Uninstall(leafDev);
            QueueDiscContainer qdc = tch.Install(leafDev);
            g_leafSpineQueueDisc[leaf][spine] = qdc.Get(0);
            g_leafSpineDevice[leaf][spine] = leafDev;

            ++linkId;
        }
    }

    // --- Populate routing tables ---
    Ipv4GlobalRoutingHelper::PopulateRoutingTables();

    // --- Dump routing tables ---
    std::string routeFile = outputDir + "/routing-tables.txt";
    Ptr<OutputStreamWrapper> routeStream = Create<OutputStreamWrapper>(routeFile, std::ios::out);
    Ipv4GlobalRoutingHelper::PrintRoutingTableAllAt(Seconds(0.4), routeStream);

    // ========== TRAFFIC (same as Hedera) ==========

    Ipv4Address hostAddr[NUM_HOSTS];
    for (uint32_t i = 0; i < NUM_HOSTS; ++i)
    {
        hostAddr[i] = hostLeafInterfaces[i].GetAddress(0);
    }

    // --- 6 Elephant flows: 500 MB each, start at 0.5s ---
    uint32_t elephantBytes = 500 * 1024 * 1024;

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
    uint32_t mouseBytes = 100 * 1024;

    InstallFlow(hosts.Get(2), hosts.Get(5), hostAddr[5], 11, mouseBytes, 1.0);
    InstallFlow(hosts.Get(3), hosts.Get(7), hostAddr[7], 12, mouseBytes, 1.0);
    InstallFlow(hosts.Get(4), hosts.Get(1), hostAddr[1], 13, mouseBytes, 1.0);
    InstallFlow(hosts.Get(5), hosts.Get(0), hostAddr[0], 14, mouseBytes, 1.0);
    InstallFlow(hosts.Get(6), hosts.Get(3), hostAddr[3], 15, mouseBytes, 1.1);
    InstallFlow(hosts.Get(7), hosts.Get(2), hostAddr[2], 16, mouseBytes, 1.1);
    InstallFlow(hosts.Get(0), hosts.Get(7), hostAddr[7], 17, mouseBytes, 1.2);
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

    // --- Leaf->Spine byte counter traces ---
    linkId = 0;
    for (uint32_t leaf = 0; leaf < NUM_LEAVES; ++leaf)
    {
        for (uint32_t spine = 0; spine < NUM_SPINES; ++spine)
        {
            // Trace on the leaf-side device (index 0)
            Ptr<NetDevice> dev = leafSpineDevices[linkId].Get(0);
            dev->TraceConnectWithoutContext(
                "PhyTxEnd",
                MakeBoundCallback(&LeafSpineTxByteCallback, leaf, spine));
            ++linkId;
        }
    }

    // --- Initialize CONGA state ---
    for (uint32_t leaf = 0; leaf < NUM_LEAVES; ++leaf)
    {
        for (uint32_t spine = 0; spine < NUM_SPINES; ++spine)
        {
            g_leafState[leaf].bytesSent[spine] = 0;
            g_leafState[leaf].bytesSentPrev[spine] = 0;
            g_leafState[leaf].ceTable[spine].utilization = 0.0;
            g_leafState[leaf].ceTable[spine].queueDepth = 0.0;
            g_leafState[leaf].ceTable[spine].ce = 0.0;
            g_leafState[leaf].ceTable[spine].lastUpdateNs = 0;
        }
    }

    // --- CONGA controller ---
    if (enableConga)
    {
        std::string congaLogFile = outputDir + "/conga-log.csv";
        g_congaLog.open(congaLogFile);
        g_congaLog << "epoch_s,flowHash,srcAddr,dstAddr,srcPort,dstPort,"
                      "oldSpine,newSpine,rate_mbps,oldCE,newCE\n";

        // Schedule first CONGA tick after flows start
        Simulator::Schedule(Seconds(0.5 + g_congaTickInterval), &CongaTick);
        NS_LOG_INFO("CONGA enabled: flowletGap=" << flowletGap * 1e6 << "us"
                    << " feedbackDelay=" << feedbackDelay * 1e6 << "us"
                    << " tick=" << congaTickInterval * 1e6 << "us");
    }
    else
    {
        NS_LOG_INFO("CONGA disabled, running ECMP-only baseline");
    }

    // ========== RUN ==========

    Simulator::Stop(Seconds(30.0));
    Simulator::Run();

    // --- Save FlowMonitor results ---
    std::string flowmonFile = outputDir + "/flowmon-conga.xml";
    g_flowMonitor->SerializeToXmlFile(flowmonFile, true, true);

    g_spineTraceFile.close();
    if (enableConga)
    {
        g_congaLog.close();
    }

    Simulator::Destroy();

    std::cout << "Simulation complete. Results in: " << outputDir << "/" << std::endl;
    return 0;
}
