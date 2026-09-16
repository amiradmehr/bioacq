#!/usr/bin/env bash
# Build (incrementally, fast when nothing changed) and launch bioacq.
# Any arguments are passed through, e.g.:
#   scripts/run.sh                     interactive GUI
#   scripts/run.sh --synthetic         GUI with both devices simulated
#   scripts/run.sh --probe 10          headless check of the real devices
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${BIOACQ_BUILD_DIR:-${STREAM_GUI_BUILD_DIR:-$HOME/.local/build/bioacq}}"
BIN="$BUILD_DIR/bioacq"

"$SCRIPT_DIR/build.sh" >/dev/null || "$SCRIPT_DIR/build.sh"

exec "$BIN" "$@"
