#!/usr/bin/env bash
# Configure + build bioacq. The build tree lives OUTSIDE Google Drive.
#   scripts/build.sh            incremental Release build
#   scripts/build.sh --clean    wipe the build dir first
# Build dir: $BIOACQ_BUILD_DIR (legacy alias STREAM_GUI_BUILD_DIR), default ~/.local/build/bioacq
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${BIOACQ_BUILD_DIR:-${STREAM_GUI_BUILD_DIR:-$HOME/.local/build/bioacq}}"
BRAINFLOW_ROOT="${BRAINFLOW_ROOT:-$HOME/.local/brainflow}"
QT_PREFIX="${QT_PREFIX:-/opt/homebrew/opt/qt}"
export PATH="/opt/homebrew/bin:$PATH"

if [[ "${1:-}" == "--clean" ]]; then
    rm -rf "$BUILD_DIR"
fi

if [[ ! -f "$BRAINFLOW_ROOT/lib/cmake/brainflow/brainflowConfig.cmake" ]]; then
    echo "error: BrainFlow not found at $BRAINFLOW_ROOT (set BRAINFLOW_ROOT)" >&2
    exit 1
fi

mkdir -p "$BUILD_DIR"
if [[ ! -f "$BUILD_DIR/CMakeCache.txt" || ! -f "$BUILD_DIR/build.ninja" ]]; then
    cmake -S "$ROOT_DIR" -B "$BUILD_DIR" -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DBRAINFLOW_ROOT="$BRAINFLOW_ROOT" \
        -DQT_PREFIX="$QT_PREFIX" \
        -DCMAKE_PREFIX_PATH="$QT_PREFIX;$BRAINFLOW_ROOT"
fi

cmake --build "$BUILD_DIR" --parallel
echo "Built: $BUILD_DIR/bioacq"
