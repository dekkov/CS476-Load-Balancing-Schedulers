#!/usr/bin/env python3
"""metrics.py — Parse FlowMonitor XML and spine trace CSV to compute
ECMP load-balancing metrics: FCT, throughput, path distribution.

Usage:
    python3 metrics.py <flowmon.xml> <results_dir>
"""

import sys
import os
import xml.etree.ElementTree as ET

import pandas as pd
import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

ELEPHANT_THRESHOLD = 50 * 1024 * 1024  # 50 MB — actual elephants are ~108 MB


def parse_ns3_time(time_str):
    """Parse ns-3 time string like '+1.23456e+09ns' to seconds."""
    s = time_str.strip().lstrip("+")
    if s.endswith("ns"):
        return float(s[:-2]) / 1e9
    elif s.endswith("us"):
        return float(s[:-2]) / 1e6
    elif s.endswith("ms"):
        return float(s[:-2]) / 1e3
    elif s.endswith("s"):
        return float(s[:-1])
    return float(s) / 1e9  # assume nanoseconds


# ---------------------------------------------------------------------------
# Parse FlowMonitor XML
# ---------------------------------------------------------------------------


def parse_flowmon(xml_path):
    """Return list of dicts with per-flow stats and 5-tuple info."""
    tree = ET.parse(xml_path)
    root = tree.getroot()

    # Build classifier map: flowId -> {src, dst, srcPort, dstPort, protocol}
    classifier = {}
    for fc in root.iter("Ipv4FlowClassifier"):
        for flow in fc.iter("Flow"):
            fid = flow.get("flowId")
            classifier[fid] = {
                "srcAddr": flow.get("sourceAddress"),
                "dstAddr": flow.get("destinationAddress"),
                "srcPort": int(flow.get("sourcePort", 0)),
                "dstPort": int(flow.get("destinationPort", 0)),
                "protocol": int(flow.get("protocol", 0)),
            }

    # Parse flow stats
    flows = []
    for fs in root.iter("FlowStats"):
        for flow in fs.iter("Flow"):
            fid = flow.get("flowId")
            tx_bytes = int(flow.get("txBytes", 0))
            rx_bytes = int(flow.get("rxBytes", 0))
            tx_packets = int(flow.get("txPackets", 0))
            rx_packets = int(flow.get("rxPackets", 0))

            time_first_tx = flow.get("timeFirstTxPacket", "0ns")
            time_last_rx = flow.get("timeLastRxPacket", "0ns")

            first_tx = parse_ns3_time(time_first_tx)
            last_rx = parse_ns3_time(time_last_rx)
            fct = last_rx - first_tx if last_rx > first_tx else 0

            info = classifier.get(fid, {})
            flows.append(
                {
                    "flowId": fid,
                    "srcAddr": info.get("srcAddr", "?"),
                    "dstAddr": info.get("dstAddr", "?"),
                    "srcPort": info.get("srcPort", 0),
                    "dstPort": info.get("dstPort", 0),
                    "protocol": info.get("protocol", 0),
                    "txBytes": tx_bytes,
                    "rxBytes": rx_bytes,
                    "txPackets": tx_packets,
                    "rxPackets": rx_packets,
                    "fct_s": fct,
                    "firstTx_s": first_tx,
                    "lastRx_s": last_rx,
                }
            )

    return pd.DataFrame(flows)


# ---------------------------------------------------------------------------
# Classify and compute metrics
# ---------------------------------------------------------------------------


def classify_flows(df):
    """Add 'type' column: elephant / mouse / ack-only."""
    def label(row):
        if row["txBytes"] < 10_000:
            return "ack-only"
        elif row["txBytes"] > ELEPHANT_THRESHOLD:
            return "elephant"
        elif row["fct_s"] > 0.4:
            # Medium-sized, long-running flow = reverse TCP ACK stream for an
            # elephant (e.g. ~1.88 MB over ~0.87s). Exclude from both plots.
            return "ack-only"
        else:
            return "mouse"

    df["type"] = df.apply(label, axis=1)
    return df


def compute_metrics(df):
    """Compute and print summary metrics."""
    mice = df[df["type"] == "mouse"]
    elephants = df[df["type"] == "elephant"]

    print("\n" + "=" * 60)
    print("ECMP SIMULATION RESULTS")
    print("=" * 60)

    # --- Mice FCT ---
    if not mice.empty:
        fcts = mice["fct_s"] * 1000  # convert to ms
        print(f"\nMice flows ({len(mice)}):")
        print(f"  FCT mean:  {fcts.mean():.2f} ms")
        print(f"  FCT p95:   {fcts.quantile(0.95):.2f} ms")
        print(f"  FCT p99:   {fcts.quantile(0.99):.2f} ms")
    else:
        print("\nNo mice flows found.")

    # --- Elephant throughput ---
    if not elephants.empty:
        elephants = elephants.copy()
        elephants["throughput_mbps"] = elephants.apply(
            lambda r: (r["rxBytes"] * 8) / (r["fct_s"] * 1e6) if r["fct_s"] > 0 else 0,
            axis=1,
        )
        print(f"\nElephant flows ({len(elephants)}):")
        print(f"  Throughput mean: {elephants['throughput_mbps'].mean():.2f} Mbps")
        for _, row in elephants.iterrows():
            print(
                f"    Flow {row['flowId']}: {row['srcAddr']}:{row['srcPort']} → "
                f"{row['dstAddr']}:{row['dstPort']}  "
                f"{row['throughput_mbps']:.2f} Mbps  FCT={row['fct_s']:.3f}s"
            )
    else:
        print("\nNo elephant flows found.")

    # --- All flows summary ---
    print(f"\nAll flows ({len(df)}, excluding {len(df[df['type'] == 'ack-only'])} ACK-only):")
    for _, row in df[df["type"] != "ack-only"].iterrows():
        print(
            f"  [{row['type']:>8}] Flow {row['flowId']}: "
            f"{row['srcAddr']}:{row['srcPort']} → {row['dstAddr']}:{row['dstPort']}  "
            f"tx={row['txBytes']}B rx={row['rxBytes']}B FCT={row['fct_s']*1000:.2f}ms"
        )

    return mice, elephants


# ---------------------------------------------------------------------------
# Spine trace analysis
# ---------------------------------------------------------------------------


def analyze_spine_trace(results_dir):
    """Parse spine-trace.csv and compute path distribution."""
    csv_path = os.path.join(results_dir, "spine-trace.csv")
    if not os.path.exists(csv_path):
        print("\nNo spine-trace.csv found, skipping path analysis.")
        return None

    df = pd.read_csv(csv_path)
    if df.empty:
        print("\nSpine trace is empty.")
        return None

    # Count bytes per spine (approximate: count packets, assume ~1500B each)
    spine_counts = df.groupby("spine_id").size()
    total_packets = spine_counts.sum()

    print(f"\nPath distribution (spine trace, {total_packets} packets):")
    for spine_id, count in spine_counts.items():
        pct = 100.0 * count / total_packets
        print(f"  Spine {spine_id}: {count} packets ({pct:.1f}%)")

    if len(spine_counts) > 1:
        imbalance = spine_counts.max() / spine_counts.mean()
        print(f"  Imbalance ratio: {imbalance:.2f}")

    return df


# ---------------------------------------------------------------------------
# Plots
# ---------------------------------------------------------------------------


def generate_plots(mice, elephants, spine_df, results_dir):
    """Generate bar charts for FCT, throughput, and path distribution."""
    fig, axes = plt.subplots(1, 3, figsize=(18, 5))

    # Plot 1: Mice FCT
    if not mice.empty:
        fcts = mice["fct_s"] * 1000
        ax = axes[0]
        x = range(len(fcts))
        ax.bar(x, fcts.values)
        ax.set_ylabel("FCT (ms)")
        ax.set_xlabel("Flow index (sorted by flow ID)")
        ax.set_title("Mice Flow Completion Times")
        ax.set_xticks([])  # too many bars for individual labels
        ax.set_title(f"Mice Flow Completion Times (n={len(mice)})")
    else:
        axes[0].text(0.5, 0.5, "No mice flows", ha="center", va="center")
        axes[0].set_title("Mice FCT")

    # Plot 2: Elephant throughput
    if not elephants.empty and "throughput_mbps" in elephants.columns:
        ax = axes[1]
        labels = [f"F{fid}" for fid in elephants["flowId"]]
        ax.bar(labels, elephants["throughput_mbps"].values)
        ax.set_ylabel("Throughput (Mbps)")
        ax.set_title(f"Elephant Throughput (n={len(elephants)})")
    else:
        axes[1].text(0.5, 0.5, "No elephant flows", ha="center", va="center")
        axes[1].set_title("Elephant Throughput")

    # Plot 3: Path distribution
    if spine_df is not None and not spine_df.empty:
        ax = axes[2]
        max_spine = max(spine_df["spine_id"].max() + 1, 2)  # always show at least 2 spines
        spine_counts = spine_df.groupby("spine_id").size().reindex(
            range(max_spine), fill_value=0
        )
        total_pkts = spine_counts.sum()
        ax.bar([f"Spine {s}" for s in spine_counts.index], spine_counts.values)
        ax.set_ylabel("Packets")
        ax.set_title(f"Path Distribution (Spine, {total_pkts:,} pkts)")
    else:
        axes[2].text(0.5, 0.5, "No spine trace", ha="center", va="center")
        axes[2].set_title("Path Distribution")

    plt.tight_layout(pad=2.0)
    plot_path = os.path.join(results_dir, "ecmp-metrics.png")
    plt.savefig(plot_path, dpi=150, bbox_inches="tight")
    print(f"\nPlots saved to: {plot_path}")
    plt.close()


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------


def main():
    if len(sys.argv) < 3:
        print(f"Usage: {sys.argv[0]} <flowmon.xml> <results_dir>")
        sys.exit(1)

    xml_path = sys.argv[1]
    results_dir = sys.argv[2]

    if not os.path.exists(xml_path):
        print(f"Error: {xml_path} not found")
        sys.exit(1)

    # Parse and analyze
    df = parse_flowmon(xml_path)
    df = classify_flows(df)
    mice, elephants = compute_metrics(df)
    spine_df = analyze_spine_trace(results_dir)
    generate_plots(mice, elephants, spine_df, results_dir)


if __name__ == "__main__":
    main()
