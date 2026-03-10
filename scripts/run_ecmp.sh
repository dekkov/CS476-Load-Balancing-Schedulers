#!/usr/bin/env bash
# run_ecmp.sh — Build, run ECMP simulation, and analyze results.
set -euo pipefail

source "$(dirname "$0")/ns3-common.sh"

RESULTS_DIR="${PROJECT_DIR}/results-ecmp"
ECMP_MODE="${1:-2}"  # Default: per-flow hash

echo "=== ECMP Simulation Runner ==="
echo "ns-3 dir:    $NS3_DIR"
echo "Project dir: $PROJECT_DIR"
echo "ECMP mode:   $ECMP_MODE (0=none, 1=random, 2=flow-hash)"
echo ""

ns3_copy_sim ecmp-leaf-spine.cc
ns3_ensure_patch
ns3_build

mkdir -p "$RESULTS_DIR"

echo "--- Running simulation (ecmpMode=$ECMP_MODE) ---"
cd "$NS3_DIR"
./ns3 run "ecmp-leaf-spine --ecmpMode=$ECMP_MODE --outputDir=$RESULTS_DIR"

echo "--- Running analysis ---"
python3 "$PROJECT_DIR/analysis/metrics.py" \
    "$RESULTS_DIR/flowmon-ecmp.xml" \
    "$RESULTS_DIR"

echo ""
echo "=== Done ==="
echo "Results in: $RESULTS_DIR/"
