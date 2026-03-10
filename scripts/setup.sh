#!/usr/bin/env bash
# setup.sh — Install ns-3.43 and dependencies for ECMP simulation
set -euo pipefail

NS3_DIR="${NS3_DIR:-$HOME/ns-3}"
PATCH_DIR="$(cd "$(dirname "$0")/.." && pwd)/patches"

echo "=== Step 1: Install build dependencies ==="
sudo apt-get update
sudo apt-get install -y g++ cmake ninja-build python3 python3-pip git

echo "=== Step 2: Clone ns-3.43 ==="
if [ -d "$NS3_DIR" ]; then
    echo "ns-3 directory already exists at $NS3_DIR, skipping clone."
else
    git clone --branch ns-3.43 --depth 1 \
        https://gitlab.com/nsnam/ns-3-dev.git "$NS3_DIR"
fi

echo "=== Step 3: Configure ns-3 ==="
cd "$NS3_DIR"
./ns3 configure --build-profile=optimized --enable-examples

echo "=== Step 4: Build ns-3 ==="
./ns3 build

echo "=== Step 5: Verify ns-3 ==="
./ns3 run hello-simulator

echo "=== Step 6: Apply per-flow ECMP patch ==="
if git diff --quiet HEAD 2>/dev/null; then
    echo "Applying ECMP flow-hash patch..."
    git apply "$PATCH_DIR/ecmp-flow-hash.patch"
    echo "Rebuilding ns-3 with patch..."
    ./ns3 build
else
    echo "Working tree has changes; patch may already be applied. Skipping."
fi

echo "=== Step 7: Install Python dependencies ==="
pip install --user pandas matplotlib 2>/dev/null \
    || pip install --break-system-packages pandas matplotlib 2>/dev/null \
    || echo "WARNING: Could not install Python packages. Install manually: pip install pandas matplotlib"

echo ""
echo "=== Setup complete ==="
echo "ns-3 installed at: $NS3_DIR"
echo "Verify with: cd $NS3_DIR && ./ns3 run hello-simulator"
