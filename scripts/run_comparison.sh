#!/usr/bin/env bash
# run_comparison.sh — Run ECMP-only and Hedera simulations with the same
# traffic workload (6 elephants + 8 mice) for side-by-side comparison.
#
# Usage:
#   bash scripts/run_comparison.sh
#
# Both runs use hedera-leaf-spine.cc with the same topology and traffic.
# The only difference is enableHedera=0 (ECMP baseline) vs enableHedera=1.
#
# Output:
#   results-comparison/ecmp/    — ECMP-only baseline
#   results-comparison/hedera/  — Hedera with Global First Fit
set -euo pipefail

source "$(dirname "$0")/ns3-common.sh"

COMPARISON_DIR="${PROJECT_DIR}/results-comparison"
ECMP_DIR="${COMPARISON_DIR}/ecmp"
HEDERA_DIR="${COMPARISON_DIR}/hedera"

echo "=== ECMP vs Hedera Comparison ==="
echo "ns-3 dir:       $NS3_DIR"
echo "Comparison dir: $COMPARISON_DIR"
echo ""

# --- Build once ---
ns3_copy_sim hedera-leaf-spine.cc
ns3_ensure_patch
ns3_build

mkdir -p "$ECMP_DIR" "$HEDERA_DIR"

# --- Run 1: ECMP-only baseline (enableHedera=0) ---
echo ""
echo "=========================================="
echo "  Run 1/2: ECMP-only baseline"
echo "=========================================="
cd "$NS3_DIR"
./ns3 run "hedera-leaf-spine --enableHedera=0 --outputDir=$ECMP_DIR"

echo "--- ECMP analysis ---"
python3 "$PROJECT_DIR/analysis/metrics_hedera.py" \
    "$ECMP_DIR/flowmon-hedera.xml" \
    "$ECMP_DIR"

# --- Run 2: Hedera (enableHedera=1) ---
echo ""
echo "=========================================="
echo "  Run 2/2: Hedera (Global First Fit)"
echo "=========================================="
cd "$NS3_DIR"
./ns3 run "hedera-leaf-spine --enableHedera=1 --outputDir=$HEDERA_DIR"

echo "--- Hedera analysis ---"
python3 "$PROJECT_DIR/analysis/metrics_hedera.py" \
    "$HEDERA_DIR/flowmon-hedera.xml" \
    "$HEDERA_DIR"

echo ""
echo "=========================================="
echo "  Comparison complete"
echo "=========================================="
echo "ECMP results:   $ECMP_DIR/"
echo "Hedera results: $HEDERA_DIR/"
echo ""
echo "To compare plots side by side:"
echo "  open $ECMP_DIR/hedera-metrics.png"
echo "  open $HEDERA_DIR/hedera-metrics.png"
