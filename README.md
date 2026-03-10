# CS476 — Load-Balancing Schedulers in Cloud Datacenters

ns-3 simulation comparing load-balancing strategies on a leaf-spine datacenter topology. Currently implements **ECMP (Equal-Cost Multi-Path)** routing with per-flow hashing. CONGA will be added in a future phase.

## Problem

Static hash-based load balancing (ECMP) can cause **hash collisions** where multiple elephant flows map to the same spine link, starving co-located mice flows. This simulation measures that effect and quantifies the impact on flow completion time (FCT) and throughput.

## Topology

```
  host0 ─┐           ┌─ host4
  host1 ─┤leaf0──┬───┤leaf2─ host5
         │       │   │
  host2 ─┤leaf1──┼───┤leaf3─ host6
  host3 ─┘   │   │   └─ host7
              │   │
           spine0 spine1
```

- **8 hosts**, **4 leaf switches**, **2 spine switches** (14 nodes total)
- Host-Leaf links: **1 Gbps**, 5 us delay
- Leaf-Spine links: **10 Gbps**, 5 us delay (full mesh: 4 leaves x 2 spines = 8 links)
- Host assignment: hosts[0-1] on leaf0, hosts[2-3] on leaf1, hosts[4-5] on leaf2, hosts[6-7] on leaf3

### IP Addressing

| Link type | Subnet pattern | Example |
|-----------|---------------|---------|
| Host-Leaf | `10.<leaf>.<host_local_idx>.0/24` | host0-leaf0: `10.0.0.0/24` (host=.1, leaf=.2) |
| Leaf-Spine | `10.10.<link_id>.0/24` | leaf0-spine0: `10.10.0.0/24` |

## Traffic Pattern

### Elephant Flows (2 flows, 100 MB each, start at t=0.5s)

| Flow | Source | Destination | Port |
|------|--------|-------------|------|
| E1 | host0 (leaf0) | host4 (leaf2) | 9 |
| E2 | host1 (leaf0) | host6 (leaf3) | 9 |

### Mice Flows (8 flows, 100 KB each, staggered start)

| Flow | Source | Destination | Port | Start |
|------|--------|-------------|------|-------|
| M1 | host2 (leaf1) | host5 (leaf2) | 11 | 1.0s |
| M2 | host3 (leaf1) | host7 (leaf3) | 12 | 1.0s |
| M3 | host4 (leaf2) | host1 (leaf0) | 13 | 1.0s |
| M4 | host5 (leaf2) | host0 (leaf0) | 14 | 1.0s |
| M5 | host6 (leaf3) | host3 (leaf1) | 15 | 1.1s |
| M6 | host7 (leaf3) | host2 (leaf1) | 16 | 1.1s |
| M7 | host0 (leaf0) | host7 (leaf3) | 17 | 1.2s |
| M8 | host1 (leaf0) | host5 (leaf2) | 18 | 1.2s |

All flows use TCP (Cubic, MSS=1448) via `BulkSendApplication`. Simulation runs for **12 seconds**.

## Prerequisites

- **OS**: Ubuntu/Debian (tested on WSL2)
- **Build tools**: g++, cmake, ninja-build, git
- **Python 3** with pip
- **Disk**: ~2 GB for ns-3 build

## Quick Start

```bash
# 1. Install ns-3 and dependencies
bash scripts/setup.sh

# 2. Run simulation + analysis
bash scripts/run_ecmp.sh
```

Results are written to `results/`.

## Simulation Parameters

### Command-Line Arguments

Pass these to the ns-3 simulation via `run_ecmp.sh` or directly:

| Argument | Default | Description |
|----------|---------|-------------|
| `--ecmpMode` | `2` | ECMP routing mode (see below) |
| `--outputDir` | `results` | Directory for output files (must exist) |

### ECMP Modes (`--ecmpMode`)

| Value | Mode | Behavior |
|-------|------|----------|
| `0` | None | Single path — all flows use the first available route. Worst-case baseline. |
| `1` | Random | Per-packet random — each packet independently picks a random equal-cost route. Can cause packet reordering. |
| `2` | Flow-hash | Per-flow hash — packets from the same 5-tuple always follow the same path. Different flows may use different paths. **Default.** |

To compare modes:

```bash
bash scripts/run_ecmp.sh 0   # single-path baseline
bash scripts/run_ecmp.sh 1   # random per-packet
bash scripts/run_ecmp.sh 2   # per-flow hash (default)
```

### How Flow-Hash ECMP Works

The per-flow hash patch (`patches/ecmp-flow-hash.patch`) modifies ns-3's `Ipv4GlobalRouting` to:

1. Extract the **5-tuple** from each packet: source IP, destination IP, protocol, source port, destination port
2. Compute an asymmetric hash: `srcIP * 2654435761 ^ dstIP * 2246822519 ^ (protocol << 16) ^ (srcPort << 8) ^ dstPort`
3. Select route index: `hash % num_equal_cost_routes`

This ensures all packets in a flow take the same spine, while different flows may take different spines. The asymmetric hash means forward and reverse flows (A->B vs B->A) can be assigned to different paths.

### Simulation Variables and What They Affect

| Variable | Location | Value | Effect |
|----------|----------|-------|--------|
| Host-Leaf bandwidth | `ecmp-leaf-spine.cc` | 1 Gbps | Access link capacity; bottleneck for individual flows |
| Leaf-Spine bandwidth | `ecmp-leaf-spine.cc` | 10 Gbps | Fabric link capacity; shared by all flows traversing that spine |
| Link delay | `ecmp-leaf-spine.cc` | 5 us | Propagation delay per hop; affects RTT and TCP ramp-up |
| Elephant size | `ecmp-leaf-spine.cc` | 100 MB | Large enough to saturate links and create congestion |
| Mouse size | `ecmp-leaf-spine.cc` | 100 KB | Small transfers sensitive to queuing delay from elephants |
| TCP variant | `ecmp-leaf-spine.cc` | Cubic | Congestion control algorithm; affects throughput dynamics |
| TCP MSS | `ecmp-leaf-spine.cc` | 1448 B | Maximum segment size; standard for 1500B MTU |
| Simulation duration | `ecmp-leaf-spine.cc` | 12 s | Must be long enough for 100 MB elephants to complete at 1 Gbps (~0.8s) |
| Elephant threshold | `metrics.py` | 1 MB | Flows with `txBytes > 1MB` classified as elephant; below as mouse |

## Environment Variables

| Variable | Default | Used by | Description |
|----------|---------|---------|-------------|
| `NS3_DIR` | `$HOME/ns-3` | `setup.sh`, `run_ecmp.sh` | Path to ns-3 installation directory |

Example:

```bash
NS3_DIR=/opt/ns-3 bash scripts/setup.sh
NS3_DIR=/opt/ns-3 bash scripts/run_ecmp.sh
```

## Output Files

After running the simulation, the `results/` directory contains:

| File | Format | Description |
|------|--------|-------------|
| `flowmon-ecmp.xml` | XML | ns-3 FlowMonitor output with per-flow byte/packet counts, timestamps, delays |
| `spine-trace.csv` | CSV | Per-packet log of which spine each packet traversed (timestamp, 5-tuple, spine ID) |
| `routing-tables.txt` | Text | Full routing table dump from all nodes at t=0.4s for debugging |
| `ecmp-metrics.png` | PNG | Bar charts: mice FCT, elephant throughput, path distribution across spines |

### Metrics Computed by `analysis/metrics.py`

| Metric | Formula | What it shows |
|--------|---------|---------------|
| Mice FCT (mean, p95, p99) | `timeLastRxPacket - timeFirstTxPacket` | How long small flows take; affected by queuing behind elephants |
| Elephant throughput | `rxBytes * 8 / FCT` (Mbps) | Sustained bandwidth of large flows |
| Path distribution | Packets per spine from `spine-trace.csv` | Whether traffic is balanced across spines |
| Imbalance ratio | `max_packets / mean_packets` across spines | 1.0 = perfect balance; higher = worse |

## Project Structure

```
CS476-Load-Balancing-Schedulers/
├── ns3-scratch/
│   └── ecmp-leaf-spine.cc          # Main simulation (~280 lines C++)
├── patches/
│   └── ecmp-flow-hash.patch        # Per-flow ECMP patch for ns-3
├── analysis/
│   └── metrics.py                  # Parse FlowMonitor XML → stats + plots
├── results/                        # Output directory (gitignored)
├── scripts/
│   ├── setup.sh                    # Install ns-3 + dependencies + apply patch
│   └── run_ecmp.sh                 # Build, run simulation, run analysis
├── requirements.txt                # Python dependencies (pandas, matplotlib)
├── .gitignore
└── README.md
```

## Troubleshooting

| Problem | Solution |
|---------|----------|
| `setup.sh` fails at `apt-get` | Run with `sudo` or install deps manually |
| `pip install` blocked by PEP 668 | Use `pip install --break-system-packages` or create a venv |
| ns-3 build fails after patch | Check patch applied cleanly: `cd $NS3_DIR && git diff` |
| Simulation produces empty XML | Ensure `--outputDir` points to an existing directory |
| All flows on one spine | Verify `--ecmpMode=2` is set and patch is applied |
| FlowMonitor shows many tiny flows | ACK-only reverse flows are expected; `metrics.py` filters them out (`txBytes < 10KB`) |
