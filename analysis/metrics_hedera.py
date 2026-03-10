#!/usr/bin/env python3
"""metrics_hedera.py — Parse FlowMonitor XML, spine trace CSV, and Hedera
controller log to compute load-balancing metrics for the Hedera simulation.

Usage:
    python3 metrics_hedera.py <flowmon-hedera.xml> <results_dir>
"""

import sys
import os

import pandas as pd
import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt

# Import shared helpers from metrics.py
sys.path.insert(0, os.path.dirname(__file__))
from metrics import parse_flowmon, parse_ns3_time, classify_flows, analyze_spine_trace

ELEPHANT_THRESHOLD = 50 * 1024 * 1024  # 50 MB for classification


# ---------------------------------------------------------------------------
# Controller log parsing
# ---------------------------------------------------------------------------


def parse_controller_log(results_dir):
    """Parse hedera-controller.csv into a DataFrame."""
    csv_path = os.path.join(results_dir, "hedera-controller.csv")
    if not os.path.exists(csv_path):
        print("\nNo hedera-controller.csv found, skipping controller analysis.")
        return None

    df = pd.read_csv(csv_path)
    if df.empty:
        print("\nController log is empty.")
        return None

    print(f"\nController log: {len(df)} reroute decisions")
    for _, row in df.iterrows():
        print(
            f"  t={row['epoch_s']:.1f}s: {row['srcAddr']}:{row['srcPort']} -> "
            f"{row['dstAddr']}:{row['dstPort']}  "
            f"spine {int(row['oldSpine'])}->{int(row['newSpine'])}  "
            f"{row['rate_mbps']:.1f} Mbps"
        )

    return df


# ---------------------------------------------------------------------------
# Metrics
# ---------------------------------------------------------------------------


def compute_hedera_metrics(df):
    """Compute and print summary metrics for Hedera simulation."""
    mice = df[df["type"] == "mouse"]
    elephants = df[df["type"] == "elephant"]

    print("\n" + "=" * 60)
    print("HEDERA SIMULATION RESULTS")
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
                f"    Flow {row['flowId']}: {row['srcAddr']}:{row['srcPort']} -> "
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
            f"{row['srcAddr']}:{row['srcPort']} -> {row['dstAddr']}:{row['dstPort']}  "
            f"tx={row['txBytes']}B rx={row['rxBytes']}B FCT={row['fct_s']*1000:.2f}ms"
        )

    return mice, elephants


# ---------------------------------------------------------------------------
# Plots
# ---------------------------------------------------------------------------


def generate_hedera_plots(mice, elephants, spine_df, controller_df, results_dir):
    """Generate plots for Hedera simulation results."""
    n_panels = 4 if controller_df is not None else 3
    fig, axes = plt.subplots(1, n_panels, figsize=(6 * n_panels, 5))

    # Panel 1: Elephant throughput
    ax = axes[0]
    if not elephants.empty and "throughput_mbps" in elephants.columns:
        labels = [
            f"F{fid}\n{row['srcAddr'].split('.')[-1]}:{row['srcPort']}"
            for fid, row in elephants.iterrows()
        ]
        bars = ax.bar(range(len(elephants)), elephants["throughput_mbps"].values)
        ax.set_xticks(range(len(elephants)))
        ax.set_xticklabels(
            [f"F{fid}" for fid in elephants["flowId"]],
            rotation=45,
            ha="right",
            fontsize=8,
        )
        ax.set_ylabel("Throughput (Mbps)")
        ax.set_title(f"Elephant Throughput (n={len(elephants)})")
    else:
        ax.text(0.5, 0.5, "No elephant flows", ha="center", va="center",
                transform=ax.transAxes)
        ax.set_title("Elephant Throughput")

    # Panel 2: Mice FCT
    ax = axes[1]
    if not mice.empty:
        fcts = mice["fct_s"] * 1000
        ax.bar(range(len(fcts)), fcts.values)
        ax.set_ylabel("FCT (ms)")
        ax.set_xlabel("Flow index")
        ax.set_title(f"Mice Flow Completion Times (n={len(mice)})")
    else:
        ax.text(0.5, 0.5, "No mice flows", ha="center", va="center",
                transform=ax.transAxes)
        ax.set_title("Mice FCT")

    # Panel 3: Path distribution
    ax = axes[2]
    if spine_df is not None and not spine_df.empty:
        max_spine = max(spine_df["spine_id"].max() + 1, 2)
        spine_counts = spine_df.groupby("spine_id").size().reindex(
            range(max_spine), fill_value=0
        )
        total_pkts = spine_counts.sum()
        ax.bar([f"Spine {s}" for s in spine_counts.index], spine_counts.values)
        ax.set_ylabel("Packets")
        ax.set_title(f"Path Distribution ({total_pkts:,} pkts)")
    else:
        ax.text(0.5, 0.5, "No spine trace", ha="center", va="center",
                transform=ax.transAxes)
        ax.set_title("Path Distribution")

    # Panel 4: Controller rerouting timeline
    if controller_df is not None and n_panels == 4:
        ax = axes[3]
        if not controller_df.empty:
            # Color by new spine assignment
            colors = ["#1f77b4", "#ff7f0e"]
            for _, row in controller_df.iterrows():
                c = colors[int(row["newSpine"]) % len(colors)]
                ax.scatter(
                    row["epoch_s"],
                    row["rate_mbps"],
                    c=c,
                    s=60,
                    edgecolors="black",
                    linewidths=0.5,
                    zorder=3,
                )

            # Legend
            for s in range(2):
                ax.scatter([], [], c=colors[s], label=f"Spine {s}", edgecolors="black",
                           linewidths=0.5)
            ax.legend(fontsize=8)
            ax.set_xlabel("Time (s)")
            ax.set_ylabel("Flow Rate (Mbps)")
            ax.set_title("Controller Reroute Decisions")
        else:
            ax.text(0.5, 0.5, "No reroutes", ha="center", va="center",
                    transform=ax.transAxes)
            ax.set_title("Controller Reroutes")

    plt.tight_layout(pad=2.0)
    plot_path = os.path.join(results_dir, "hedera-metrics.png")
    plt.savefig(plot_path, dpi=150, bbox_inches="tight")
    print(f"\nPlots saved to: {plot_path}")
    plt.close()


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------


def main():
    if len(sys.argv) < 3:
        print(f"Usage: {sys.argv[0]} <flowmon-hedera.xml> <results_dir>")
        sys.exit(1)

    xml_path = sys.argv[1]
    results_dir = sys.argv[2]

    if not os.path.exists(xml_path):
        print(f"Error: {xml_path} not found")
        sys.exit(1)

    # Parse and analyze
    df = parse_flowmon(xml_path)
    df = classify_flows(df)
    mice, elephants = compute_hedera_metrics(df)
    spine_df = analyze_spine_trace(results_dir)
    controller_df = parse_controller_log(results_dir)
    generate_hedera_plots(mice, elephants, spine_df, controller_df, results_dir)


if __name__ == "__main__":
    main()
