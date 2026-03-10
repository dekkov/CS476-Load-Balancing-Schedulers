/* ecmp-leaf-spine-scaled.cc
 * CS476 — Scaled ECMP simulation: same 4-leaf/2-spine/8-host topology but
 * with 52 concurrent flows (4 elephants + 48 mice) to demonstrate that
 * per-flow hash ECMP (mode 2) distributes correctly with sufficient flow count.
 *
 * The original ecmp-leaf-spine.cc uses only 10 flows, which is too few for
 * the hash function to statistically spread across 2 spines. Here, all 12
 * ordered cross-leaf pairs are used, giving the hash enough 5-tuple diversity
 * to achieve ~50/50 balance — without the per-packet reordering risk of mode 1.
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

NS_LOG_COMPONENT_DEFINE("EcmpLeafSpineScaled");

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

    g_spineTraceFile << Simulator::Now().GetNanoSeconds() << ","
                     << ipHeader.GetSource() << ","
                     << ipHeader.GetDestination() << ","
                     << srcPort << ","
                     << dstPort << ","
                     << spineId << "\n";
}

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

int
main(int argc, char* argv[])
{
    uint32_t ecmpMode = 2;
    std::string outputDir = "results-ecmp-scaled";

    CommandLine cmd;
    cmd.AddValue("ecmpMode", "ECMP mode: 0=none, 1=random, 2=flow-hash", ecmpMode);
    cmd.AddValue("outputDir", "Output directory for results", outputDir);
    cmd.Parse(argc, argv);

    Config::SetDefault("ns3::TcpSocket::SegmentSize", UintegerValue(1448));
    Config::SetDefault("ns3::TcpL4Protocol::SocketType", StringValue("ns3::TcpCubic"));

    if (ecmpMode == 1)
        Config::SetDefault("ns3::Ipv4GlobalRouting::RandomEcmpRouting", BooleanValue(true));
    else if (ecmpMode == 2)
        Config::SetDefault("ns3::Ipv4GlobalRouting::FlowEcmpRouting", BooleanValue(true));

    // ========== TOPOLOGY (identical to ecmp-leaf-spine.cc) ==========
    // 8 hosts, 4 leaves, 2 spines — hosts[0-1] on leaf0, [2-3] on leaf1,
    // [4-5] on leaf2, [6-7] on leaf3.

    NodeContainer hosts;  hosts.Create(8);
    NodeContainer leaves; leaves.Create(4);
    NodeContainer spines; spines.Create(2);

    InternetStackHelper internet;
    internet.Install(hosts);
    internet.Install(leaves);
    internet.Install(spines);

    PointToPointHelper hostLeafLink;
    hostLeafLink.SetDeviceAttribute("DataRate", StringValue("1Gbps"));
    hostLeafLink.SetChannelAttribute("Delay", StringValue("5us"));

    PointToPointHelper leafSpineLink;
    leafSpineLink.SetDeviceAttribute("DataRate", StringValue("10Gbps"));
    leafSpineLink.SetChannelAttribute("Delay", StringValue("5us"));

    Ipv4AddressHelper address;

    // hostAddr[leaf][local_h]: IP of host h on the given leaf
    Ipv4Address hostAddr[4][2];

    for (uint32_t leaf = 0; leaf < 4; ++leaf)
    {
        for (uint32_t h = 0; h < 2; ++h)
        {
            uint32_t hostIdx = leaf * 2 + h;
            NetDeviceContainer devs = hostLeafLink.Install(hosts.Get(hostIdx), leaves.Get(leaf));
            std::ostringstream subnet;
            subnet << "10." << leaf << "." << h << ".0";
            address.SetBase(subnet.str().c_str(), "255.255.255.0");
            Ipv4InterfaceContainer ifaces = address.Assign(devs);
            hostAddr[leaf][h] = ifaces.GetAddress(0);
        }
    }

    uint32_t linkId = 0;
    for (uint32_t leaf = 0; leaf < 4; ++leaf)
    {
        for (uint32_t spine = 0; spine < 2; ++spine)
        {
            NetDeviceContainer devs = leafSpineLink.Install(leaves.Get(leaf), spines.Get(spine));
            std::ostringstream subnet;
            subnet << "10.10." << linkId << ".0";
            address.SetBase(subnet.str().c_str(), "255.255.255.0");
            address.Assign(devs);
            ++linkId;
        }
    }

    Ipv4GlobalRoutingHelper::PopulateRoutingTables();

    std::string routeFile = outputDir + "/routing-tables.txt";
    Ptr<OutputStreamWrapper> routeStream = Create<OutputStreamWrapper>(routeFile, std::ios::out);
    Ipv4GlobalRoutingHelper::PrintRoutingTableAllAt(Seconds(0.4), routeStream);

    // ========== TRAFFIC ==========

    uint32_t elephantBytes = 100 * 1024 * 1024; // 100 MB
    uint32_t mouseBytes    = 100 * 1024;         // 100 KB
    uint16_t nextPort      = 9000;

    // --- Elephant flows (4): spread across different source/destination leaves
    // so that each elephant has a unique 5-tuple and crosses the fabric.
    InstallFlow(hosts.Get(0), hosts.Get(4), hostAddr[2][0], nextPort++, elephantBytes, 0.5); // leaf0→leaf2
    InstallFlow(hosts.Get(1), hosts.Get(6), hostAddr[3][0], nextPort++, elephantBytes, 0.5); // leaf0→leaf3
    InstallFlow(hosts.Get(2), hosts.Get(5), hostAddr[2][1], nextPort++, elephantBytes, 0.5); // leaf1→leaf2
    InstallFlow(hosts.Get(3), hosts.Get(7), hostAddr[3][1], nextPort++, elephantBytes, 0.5); // leaf1→leaf3

    // --- Mice flows (48): all 12 ordered cross-leaf pairs × 4 host combos.
    // Staggered by 0.02s each to avoid a simultaneous SYN storm while keeping
    // most flows active concurrently (mice complete in ~1ms so stagger is fine).
    double startTime = 1.0;
    for (uint32_t srcLeaf = 0; srcLeaf < 4; ++srcLeaf)
    {
        for (uint32_t dstLeaf = 0; dstLeaf < 4; ++dstLeaf)
        {
            if (srcLeaf == dstLeaf)
                continue;
            for (uint32_t sh = 0; sh < 2; ++sh)
            {
                for (uint32_t dh = 0; dh < 2; ++dh)
                {
                    uint32_t srcIdx = srcLeaf * 2 + sh;
                    uint32_t dstIdx = dstLeaf * 2 + dh;
                    InstallFlow(hosts.Get(srcIdx),
                                hosts.Get(dstIdx),
                                hostAddr[dstLeaf][dh],
                                nextPort++,
                                mouseBytes,
                                startTime);
                    startTime += 0.02;
                }
            }
        }
    }

    // ========== MEASUREMENT ==========

    FlowMonitorHelper flowHelper;
    Ptr<FlowMonitor> flowMonitor = flowHelper.InstallAll();

    std::string spineTraceFile = outputDir + "/spine-trace.csv";
    g_spineTraceFile.open(spineTraceFile);
    g_spineTraceFile << "timestamp_ns,src_ip,dst_ip,src_port,dst_port,spine_id\n";

    for (uint32_t s = 0; s < spines.GetN(); ++s)
    {
        Ptr<Ipv4> ipv4 = spines.Get(s)->GetObject<Ipv4>();
        ipv4->TraceConnectWithoutContext("Tx", MakeBoundCallback(&SpineTxCallback, s));
    }

    // 15s: enough for 100MB elephants + all delayed mice to complete
    Simulator::Stop(Seconds(15.0));
    Simulator::Run();

    std::string flowmonFile = outputDir + "/flowmon-ecmp.xml";
    flowMonitor->SerializeToXmlFile(flowmonFile, true, true);

    g_spineTraceFile.close();
    Simulator::Destroy();

    std::cout << "Simulation complete. Results in: " << outputDir << "/" << std::endl;
    return 0;
}
