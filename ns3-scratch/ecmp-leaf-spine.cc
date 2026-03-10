/* ecmp-leaf-spine.cc
 * CS476 — ECMP load-balancing simulation on a leaf-spine topology.
 *
 * Topology: 8 hosts, 4 leaves, 2 spines (full mesh leaf↔spine).
 * Traffic:  2 elephant flows + 8 mice flows.
 * Output:   FlowMonitor XML, spine trace CSV, routing tables.
 */

#include "ns3/applications-module.h"
#include "ns3/core-module.h"
#include "ns3/flow-monitor-helper.h"
#include "ns3/internet-module.h"
#include "ns3/network-module.h"
#include "ns3/point-to-point-module.h"

#include <fstream>
#include <iomanip>
#include <sstream>

using namespace ns3;

NS_LOG_COMPONENT_DEFINE("EcmpLeafSpine");

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

// ---------- Main ----------

int
main(int argc, char* argv[])
{
    // --- Command-line arguments ---
    uint32_t ecmpMode = 2; // 0=none, 1=random, 2=flow-hash
    std::string outputDir = "results-ecmp";

    CommandLine cmd;
    cmd.AddValue("ecmpMode", "ECMP mode: 0=none, 1=random, 2=flow-hash", ecmpMode);
    cmd.AddValue("outputDir", "Output directory for results", outputDir);
    cmd.Parse(argc, argv);

    // --- TCP configuration ---
    Config::SetDefault("ns3::TcpSocket::SegmentSize", UintegerValue(1448));
    Config::SetDefault("ns3::TcpL4Protocol::SocketType", StringValue("ns3::TcpCubic"));

    // --- ECMP configuration ---
    if (ecmpMode == 1)
    {
        Config::SetDefault("ns3::Ipv4GlobalRouting::RandomEcmpRouting", BooleanValue(true));
        NS_LOG_INFO("ECMP mode: random (per-packet)");
    }
    else if (ecmpMode == 2)
    {
        Config::SetDefault("ns3::Ipv4GlobalRouting::FlowEcmpRouting", BooleanValue(true));
        NS_LOG_INFO("ECMP mode: per-flow hash");
    }
    else
    {
        NS_LOG_INFO("ECMP mode: none (single path)");
    }

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
    leafSpineLink.SetDeviceAttribute("DataRate", StringValue("10Gbps"));
    leafSpineLink.SetChannelAttribute("Delay", StringValue("5us"));

    Ipv4AddressHelper address;

    // --- Host-Leaf links ---
    // hosts[0-1] → leaf0, hosts[2-3] → leaf1, hosts[4-5] → leaf2, hosts[6-7] → leaf3
    // IP: 10.<leaf>.<host_local_index>.0/24  (host=.1, leaf=.2)
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

    // --- Leaf-Spine links (full mesh: 4 leaves × 2 spines = 8 links) ---
    // IP: 10.10.<link_id>.0/24
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

    // Destination IP addresses (host side of each host-leaf link, index 0 = host)
    // hostLeafInterfaces[hostIdx].GetAddress(0) is the host's IP
    Ipv4Address hostAddr[8];
    for (uint32_t i = 0; i < 8; ++i)
    {
        hostAddr[i] = hostLeafInterfaces[i].GetAddress(0);
    }

    // Elephant flows: 100 MB each, start at 0.5s
    uint32_t elephantBytes = 100 * 1024 * 1024; // 100 MB

    // E1: host0 (leaf0) → host4 (leaf2), port 9
    InstallFlow(hosts.Get(0), hosts.Get(4), hostAddr[4], 9, elephantBytes, 0.5);
    // E2: host1 (leaf0) → host6 (leaf3), port 9
    InstallFlow(hosts.Get(1), hosts.Get(6), hostAddr[6], 9, elephantBytes, 0.5);

    // Mice flows: 100 KB each
    uint32_t mouseBytes = 100 * 1024; // 100 KB

    // M1: host2 (leaf1) → host5 (leaf2), port 11, t=1.0s
    InstallFlow(hosts.Get(2), hosts.Get(5), hostAddr[5], 11, mouseBytes, 1.0);
    // M2: host3 (leaf1) → host7 (leaf3), port 12, t=1.0s
    InstallFlow(hosts.Get(3), hosts.Get(7), hostAddr[7], 12, mouseBytes, 1.0);
    // M3: host4 (leaf2) → host1 (leaf0), port 13, t=1.0s
    InstallFlow(hosts.Get(4), hosts.Get(1), hostAddr[1], 13, mouseBytes, 1.0);
    // M4: host5 (leaf2) → host0 (leaf0), port 14, t=1.0s
    InstallFlow(hosts.Get(5), hosts.Get(0), hostAddr[0], 14, mouseBytes, 1.0);
    // M5: host6 (leaf3) → host3 (leaf1), port 15, t=1.1s
    InstallFlow(hosts.Get(6), hosts.Get(3), hostAddr[3], 15, mouseBytes, 1.1);
    // M6: host7 (leaf3) → host2 (leaf1), port 16, t=1.1s
    InstallFlow(hosts.Get(7), hosts.Get(2), hostAddr[2], 16, mouseBytes, 1.1);
    // M7: host0 (leaf0) → host7 (leaf3), port 17, t=1.2s
    InstallFlow(hosts.Get(0), hosts.Get(7), hostAddr[7], 17, mouseBytes, 1.2);
    // M8: host1 (leaf0) → host5 (leaf2), port 18, t=1.2s
    InstallFlow(hosts.Get(1), hosts.Get(5), hostAddr[5], 18, mouseBytes, 1.2);

    // ========== MEASUREMENT ==========

    // --- FlowMonitor ---
    FlowMonitorHelper flowHelper;
    Ptr<FlowMonitor> flowMonitor = flowHelper.InstallAll();

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

    // ========== RUN ==========

    Simulator::Stop(Seconds(12.0));
    Simulator::Run();

    // --- Save FlowMonitor results ---
    std::string flowmonFile = outputDir + "/flowmon-ecmp.xml";
    flowMonitor->SerializeToXmlFile(flowmonFile, true, true);

    g_spineTraceFile.close();

    Simulator::Destroy();

    std::cout << "Simulation complete. Results in: " << outputDir << "/" << std::endl;
    return 0;
}
