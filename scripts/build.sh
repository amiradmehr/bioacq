#!/usr/bin/env bash
# Configure + build bioacq. The build tree lives OUTSIDE Google Drive.
#   scripts/build.sh            incremental Release build
#   scripts/build.sh --clean    wipe the build dir first
# Build dir: $BIOACQ_BUILD_DIR (legacy alias STREAM_GUI_BUILD_DIR), default ~/.local/build/bioacq
# This is the development build (plain `bioacq` binary). For the double-clickable
# BioAcq.app use scripts/package_macos.sh; on Windows scripts/package_windows.ps1.
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${BIOACQ_BUILD_DIR:-${STREAM_GUI_BUILD_DIR:-$HOME/.local/build/bioacq}}"
BRAINFLOW_ROOT="${BRAINFLOW_ROOT:-$HOME/.local/brainflow}"
QT_PREFIX="${QT_PREFIX:-/opt/homebrew/opt/qt}"
export PATH="/opt/homebrew/bin:$PATH"

if [[ "${1:-}" == "--clean" ]]; then
    rm -rf "$BUILD_DIR"
fi

if [[ ! -f "$BRAINFLOW_ROOT/inc/board_shim.h" ]]; then
    echo "error: BrainFlow not found at $BRAINFLOW_ROOT (set BRAINFLOW_ROOT, or run scripts/build_brainflow.sh)" >&2
    exit 1
fi

mkdir -p "$BUILD_DIR"
# Configure on first use, and again if a packaging script configured this tree
# (BIOACQ_PACKAGED=ON builds BioAcq.app, not the bioacq binary run.sh starts).
if [[ ! -f "$BUILD_DIR/CMakeCache.txt" || ! -f "$BUILD_DIR/build.ninja" ]] ||
    grep -q '^BIOACQ_PACKAGED:BOOL=ON$' "$BUILD_DIR/CMakeCache.txt"; then
    cmake -S "$ROOT_DIR" -B "$BUILD_DIR" -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DBIOACQ_PACKAGED=OFF \
        -DBRAINFLOW_ROOT="$BRAINFLOW_ROOT" \
        -DQT_PREFIX="$QT_PREFIX" \
        -DCMAKE_PREFIX_PATH="$QT_PREFIX;$BRAINFLOW_ROOT"
fi

cmake --build "$BUILD_DIR" --parallel
echo "Built: $BUILD_DIR/bioacq"
