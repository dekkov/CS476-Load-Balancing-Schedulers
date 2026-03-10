#!/usr/bin/env bash
# run_ecmp.sh — Build, run ECMP simulation, and analyze results.
set -euo pipefail

NS3_DIR="${NS3_DIR:-$HOME/ns-3}"
PROJECT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
RESULTS_DIR="${PROJECT_DIR}/results"
ECMP_MODE="${1:-2}"  # Default: per-flow hash

echo "=== ECMP Simulation Runner ==="
echo "ns-3 dir:    $NS3_DIR"
echo "Project dir: $PROJECT_DIR"
echo "ECMP mode:   $ECMP_MODE (0=none, 1=random, 2=flow-hash)"
echo ""

# Step 1: Copy simulation to ns-3 scratch
echo "--- Copying simulation to ns-3 scratch ---"
cp "$PROJECT_DIR/ns3-scratch/ecmp-leaf-spine.cc" "$NS3_DIR/scratch/"

# Step 2: Apply patch if not already applied
echo "--- Checking ECMP patch ---"
cd "$NS3_DIR"
if grep -q "FlowEcmpRouting" src/internet/model/ipv4-global-routing.h 2>/dev/null; then
    echo "Patch already applied."
else
    echo "Applying ECMP flow-hash patch..."
    git apply "$PROJECT_DIR/patches/ecmp-flow-hash.patch"
fi

# Step 3: Build
echo "--- Building ns-3 ---"
./ns3 build

# Step 4: Create results directory
mkdir -p "$RESULTS_DIR"

# Step 5: Run simulation
echo "--- Running simulation (ecmpMode=$ECMP_MODE) ---"
./ns3 run "ecmp-leaf-spine --ecmpMode=$ECMP_MODE --outputDir=$RESULTS_DIR"

# Step 6: Run analysis
echo "--- Running analysis ---"
python3 "$PROJECT_DIR/analysis/metrics.py" \
    "$RESULTS_DIR/flowmon-ecmp.xml" \
    "$RESULTS_DIR"

echo ""
echo "=== Done ==="
echo "Results in: $RESULTS_DIR/"
