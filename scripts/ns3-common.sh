#!/usr/bin/env bash
# ns3-common.sh — Shared functions for ns-3 simulation runners.
# Source this file; do not execute directly.

NS3_DIR="${NS3_DIR:-$HOME/ns-3}"
PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

# Copy a simulation source file into ns-3 scratch.
#   ns3_copy_sim <filename>   e.g. ns3_copy_sim ecmp-leaf-spine.cc
ns3_copy_sim() {
    local sim_file="$1"
    echo "--- Copying $sim_file to ns-3 scratch ---"
    cp "$PROJECT_DIR/ns3-scratch/$sim_file" "$NS3_DIR/scratch/"
}

# Ensure the combined ns-3 patch (ECMP flow-hash + Hedera override) is applied.
# This patch is a superset: it works for both ECMP-only and Hedera simulations.
ns3_ensure_patch() {
    echo "--- Checking ns-3 patch ---"
    cd "$NS3_DIR"
    if grep -q "g_flowOverrideTable" src/internet/model/ipv4-global-routing.cc 2>/dev/null; then
        echo "Patch already applied."
    elif grep -q "FlowEcmpRouting" src/internet/model/ipv4-global-routing.h 2>/dev/null; then
        echo "Old ECMP-only patch detected, upgrading to combined patch..."
        git checkout -- src/internet/model/ipv4-global-routing.h \
                        src/internet/model/ipv4-global-routing.cc
        git apply "$PROJECT_DIR/patches/hedera-override.patch"
    else
        echo "Applying combined patch..."
        git apply "$PROJECT_DIR/patches/hedera-override.patch"
    fi
}

# Build ns-3.
ns3_build() {
    echo "--- Building ns-3 ---"
    cd "$NS3_DIR"
    ./ns3 build
}
