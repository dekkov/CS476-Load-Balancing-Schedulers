#!/usr/bin/env bash
# run_conga.sh — Build, run CONGA simulation, and analyze results.
set -euo pipefail

source "$(dirname "$0")/ns3-common.sh"

RESULTS_DIR="${PROJECT_DIR}/results-conga"
ENABLE_CONGA="${1:-1}"  # 0=ECMP-only baseline, 1=CONGA

echo "=== CONGA Simulation Runner ==="
echo "ns-3 dir:      $NS3_DIR"
echo "Project dir:   $PROJECT_DIR"
echo "Enable CONGA:  $ENABLE_CONGA (0=ECMP-only, 1=CONGA)"
echo ""

ns3_copy_sim conga-leaf-spine.cc
ns3_ensure_patch
ns3_build

mkdir -p "$RESULTS_DIR"

echo "--- Running simulation (enableConga=$ENABLE_CONGA) ---"
cd "$NS3_DIR"
./ns3 run "conga-leaf-spine --enableConga=$ENABLE_CONGA --outputDir=$RESULTS_DIR"

echo "--- Running analysis ---"
python3 "$PROJECT_DIR/analysis/metrics_conga.py" \
    "$RESULTS_DIR/flowmon-conga.xml" \
    "$RESULTS_DIR"

echo ""
echo "=== Done ==="
echo "Results in: $RESULTS_DIR/"
