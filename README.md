# CS476 — Load-Balancing Schedulers in Cloud Datacenters

ns-3 simulation comparing load-balancing strategies on a leaf-spine datacenter topology. Implements **ECMP (Equal-Cost Multi-Path)** routing with per-flow hashing and **Hedera** with a centralized controller that detects elephant flows and reroutes them via Global First Fit.

## Problem

Static hash-based load balancing (ECMP) can cause **hash collisions** where multiple elephant flows map to the same spine link, starving co-located mice flows. Hedera addresses this by periodically detecting elephant flows and rerouting them to less-loaded spines. This simulation measures both approaches and quantifies the impact on flow completion time (FCT), throughput, and path distribution.

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

## Prerequisites

- **OS**: Ubuntu/Debian (tested on WSL2)
- **Build tools**: g++, cmake, ninja-build, git
- **Python 3** with pip (pandas, matplotlib)
- **Disk**: ~2 GB for ns-3 build

## Quick Start

```bash
# 1. Install ns-3 and dependencies (one-time)
bash scripts/setup.sh

# 2. Run ECMP vs Hedera comparison (same workload, side-by-side results)
bash scripts/run_comparison.sh
```

## Scripts

All scripts share common setup logic via `scripts/ns3-common.sh`, which handles patch application and ns-3 builds. Each script copies its simulation file to ns-3 scratch, ensures the combined patch is applied, builds, runs, and analyzes.

| Script | Description | Usage |
|--------|-------------|-------|
| `scripts/setup.sh` | One-time setup: install ns-3.43, apply patch, install Python deps | `bash scripts/setup.sh` |
| `scripts/ns3-common.sh` | Shared functions sourced by all run scripts (not run directly) | — |
| `scripts/run_ecmp.sh` | ECMP simulation (2 elephants + 8 mice, 12s) | `bash scripts/run_ecmp.sh [ecmpMode]` |
| `scripts/run_ecmp_scaled.sh` | Scaled ECMP simulation (52 flows) | `bash scripts/run_ecmp_scaled.sh [ecmpMode]` |
| `scripts/run_hedera.sh` | Hedera simulation (6 elephants + 8 mice, 15s) | `bash scripts/run_hedera.sh [enableHedera]` |
| `scripts/run_comparison.sh` | Run ECMP-only and Hedera back-to-back with identical workload | `bash scripts/run_comparison.sh` |

### `run_ecmp.sh` — ECMP Simulation

Runs the basic ECMP simulation with 2 elephant flows and 8 mice flows.

```bash
bash scripts/run_ecmp.sh        # default: per-flow hash (mode 2)
bash scripts/run_ecmp.sh 0      # single-path (no ECMP)
bash scripts/run_ecmp.sh 1      # random per-packet
bash scripts/run_ecmp.sh 2      # per-flow hash
```

- **Simulation**: `ns3-scratch/ecmp-leaf-spine.cc`
- **Analysis**: `analysis/metrics.py`
- **Results**: `results-ecmp/`

### `run_ecmp_scaled.sh` — Scaled ECMP Simulation

Runs a larger ECMP simulation with 52 flows to demonstrate hash distribution effects at scale.

```bash
bash scripts/run_ecmp_scaled.sh      # default: per-flow hash (mode 2)
bash scripts/run_ecmp_scaled.sh 0    # single-path
```

- **Simulation**: `ns3-scratch/ecmp-leaf-spine-scaled.cc`
- **Analysis**: `analysis/metrics.py`
- **Results**: `results-ecmp-scaled/`

### `run_hedera.sh` — Hedera Simulation

Runs the Hedera simulation with 6 elephant flows and 8 mice flows. A centralized controller periodically detects elephants (>1 MB) and reroutes them using Global First Fit to balance spine utilization.

```bash
bash scripts/run_hedera.sh       # default: Hedera enabled
bash scripts/run_hedera.sh 1     # Hedera enabled (same as default)
bash scripts/run_hedera.sh 0     # ECMP-only baseline (controller disabled)
```

- **Simulation**: `ns3-scratch/hedera-leaf-spine.cc`
- **Analysis**: `analysis/metrics_hedera.py`
- **Results**: `results-hedera/`

### `run_comparison.sh` — ECMP vs Hedera Comparison

Runs both modes back-to-back using the **same simulation file** (`hedera-leaf-spine.cc`) with identical topology and traffic. The only difference is whether the Hedera controller is enabled. This ensures a fair comparison.

```bash
bash scripts/run_comparison.sh
```

- Builds ns-3 once, then runs two simulations sequentially
- **ECMP baseline** (`enableHedera=0`) → `results-comparison/ecmp/`
- **Hedera** (`enableHedera=1`) → `results-comparison/hedera/`

Both directories contain the same output files, making it easy to compare metrics side by side.

## Traffic Patterns

### ECMP Simulation (2 elephants + 8 mice)

| Flow | Source | Destination | Port | Size | Start |
|------|--------|-------------|------|------|-------|
| E1 | host0 (leaf0) | host4 (leaf2) | 9 | 100 MB | 0.5s |
| E2 | host1 (leaf0) | host6 (leaf3) | 9 | 100 MB | 0.5s |
| M1-M8 | various | various | 11-18 | 100 KB | 1.0-1.2s |

### Hedera / Comparison Simulation (6 elephants + 8 mice)

| Flow | Source | Destination | Port | Size | Start |
|------|--------|-------------|------|------|-------|
| E1 | host0 (leaf0) | host4 (leaf2) | 9001 | 100 MB | 0.5s |
| E2 | host0 (leaf0) | host5 (leaf2) | 9002 | 100 MB | 0.5s |
| E3 | host1 (leaf0) | host6 (leaf3) | 9003 | 100 MB | 0.5s |
| E4 | host1 (leaf0) | host7 (leaf3) | 9004 | 100 MB | 0.5s |
| E5 | host2 (leaf1) | host4 (leaf2) | 9005 | 100 MB | 0.5s |
| E6 | host3 (leaf1) | host7 (leaf3) | 9006 | 100 MB | 0.5s |
| M1-M8 | various | various | 11-18 | 100 KB | 1.0-1.2s |

With 6 elephants on 2 spines, pigeonhole guarantees at least 4 on one spine under ECMP. Hedera's Global First Fit should achieve a 3-3 split.

All flows use TCP (Cubic, MSS=1448) via `BulkSendApplication`.

## Simulation Parameters

### ECMP Parameters (`ecmp-leaf-spine.cc`)

| Argument | Default | Description |
|----------|---------|-------------|
| `--ecmpMode` | `2` | ECMP routing mode: 0=none, 1=random, 2=flow-hash |
| `--outputDir` | `results-ecmp` | Output directory |

### Hedera Parameters (`hedera-leaf-spine.cc`)

| Argument | Default | Description |
|----------|---------|-------------|
| `--enableHedera` | `1` | 0=ECMP-only baseline, 1=Hedera controller enabled |
| `--hederaEpoch` | `2.0` | Controller polling interval in seconds |
| `--elephantThreshold` | `1048576` | Elephant detection threshold in bytes (1 MB) |
| `--outputDir` | `results-hedera` | Output directory |

### How Flow-Hash ECMP Works

The patch (`patches/hedera-override.patch`) modifies ns-3's `Ipv4GlobalRouting` to:

1. Extract the **5-tuple** from each packet: source IP, destination IP, protocol, source port, destination port
2. Compute an asymmetric hash: `srcIP * 2654435761 ^ dstIP * 2246822519 ^ (protocol << 16) ^ (srcPort << 8) ^ dstPort`
3. Check the **flow override table** — if the controller has placed this flow, use the assigned spine
4. Otherwise select route index: `hash % num_equal_cost_routes`

### How Hedera Works

The Hedera controller runs as a periodic callback inside the simulation:

1. **Detect**: Every epoch (2s), read FlowMonitor stats and identify flows exceeding the elephant threshold (1 MB)
2. **Sort**: Rank detected elephants by throughput rate (bytes/sec) in descending order
3. **Place**: Global First Fit — assign each elephant to the least-loaded spine
4. **Override**: Call `Ipv4GlobalRouting::SetFlowOverride(hash, spineIdx)` to reroute the flow

Default traffic still uses ECMP mode 2 (per-flow hash). Only detected elephants get overridden.

## Environment Variables

| Variable | Default | Description |
|----------|---------|-------------|
| `NS3_DIR` | `$HOME/ns-3` | Path to ns-3 installation directory |

```bash
NS3_DIR=/opt/ns-3 bash scripts/setup.sh
NS3_DIR=/opt/ns-3 bash scripts/run_comparison.sh
```

## Output Files

### ECMP (`results-ecmp/`)

| File | Description |
|------|-------------|
| `flowmon-ecmp.xml` | FlowMonitor per-flow stats (bytes, packets, timestamps, delays) |
| `spine-trace.csv` | Per-packet spine traversal log (timestamp, 5-tuple, spine ID) |
| `routing-tables.txt` | Routing table dump from all nodes at t=0.4s |
| `ecmp-metrics.png` | Bar charts: mice FCT, elephant throughput, path distribution |

### Hedera (`results-hedera/` or `results-comparison/{ecmp,hedera}/`)

| File | Description |
|------|-------------|
| `flowmon-hedera.xml` | FlowMonitor per-flow stats |
| `spine-trace.csv` | Per-packet spine traversal log |
| `hedera-controller.csv` | Controller decisions: epoch, flow, old/new spine, rate (Hedera mode only) |
| `routing-tables.txt` | Routing table dump |
| `hedera-metrics.png` | 4-panel plot: elephant throughput, mice FCT, path distribution, reroute timeline |

### Metrics

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
│   ├── ecmp-leaf-spine.cc             # ECMP simulation (2 elephants + 8 mice)
│   ├── ecmp-leaf-spine-scaled.cc      # Scaled ECMP simulation (52 flows)
│   └── hedera-leaf-spine.cc           # Hedera simulation (6 elephants + 8 mice)
├── patches/
│   ├── ecmp-flow-hash.patch           # Original ECMP-only patch (superseded)
│   └── hedera-override.patch          # Combined ECMP + Hedera override patch
├── analysis/
│   ├── metrics.py                     # ECMP analysis: parse FlowMonitor → stats + plots
│   └── metrics_hedera.py             # Hedera analysis: adds controller log + 4-panel plots
├── scripts/
│   ├── setup.sh                       # One-time: install ns-3, apply patch, install deps
│   ├── ns3-common.sh                  # Shared functions (patch, build, copy)
│   ├── run_ecmp.sh                    # Run ECMP simulation
│   ├── run_ecmp_scaled.sh             # Run scaled ECMP simulation
│   ├── run_hedera.sh                  # Run Hedera simulation
│   └── run_comparison.sh              # Run ECMP vs Hedera side-by-side
├── results-ecmp/                      # ECMP output (gitignored)
├── results-ecmp-scaled/               # Scaled ECMP output (gitignored)
├── results-hedera/                    # Hedera output (gitignored)
├── results-comparison/                # Comparison output (gitignored)
│   ├── ecmp/                          #   ECMP-only baseline
│   └── hedera/                        #   Hedera with controller
├── requirements.txt
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
| FlowMonitor shows many tiny flows | ACK-only reverse flows are expected; analysis scripts filter them out |
| Old ECMP-only patch applied | `run_*.sh` scripts auto-detect and upgrade to combined patch |
