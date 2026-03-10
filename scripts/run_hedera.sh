#!/usr/bin/env bash
# run_hedera.sh — Build, run Hedera simulation, and analyze results.
set -euo pipefail

source "$(dirname "$0")/ns3-common.sh"

RESULTS_DIR="${PROJECT_DIR}/results-hedera"
ENABLE_HEDERA="${1:-1}"  # 0=ECMP-only baseline, 1=Hedera

echo "=== Hedera Simulation Runner ==="
echo "ns-3 dir:      $NS3_DIR"
echo "Project dir:   $PROJECT_DIR"
echo "Enable Hedera: $ENABLE_HEDERA (0=ECMP-only, 1=Hedera)"
echo ""

ns3_copy_sim hedera-leaf-spine.cc
ns3_ensure_patch
ns3_build

mkdir -p "$RESULTS_DIR"

echo "--- Running simulation (enableHedera=$ENABLE_HEDERA) ---"
cd "$NS3_DIR"
./ns3 run "hedera-leaf-spine --enableHedera=$ENABLE_HEDERA --outputDir=$RESULTS_DIR"

echo "--- Running analysis ---"
python3 "$PROJECT_DIR/analysis/metrics_hedera.py" \
    "$RESULTS_DIR/flowmon-hedera.xml" \
    "$RESULTS_DIR"

echo ""
echo "=== Done ==="
echo "Results in: $RESULTS_DIR/"
