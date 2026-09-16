#!/usr/bin/env bash
# Build and install the patched BrainFlow that bioacq links against (macOS / Linux).
#
#   scripts/build_brainflow.sh [install-prefix]      default prefix: ~/.local/brainflow
#
# Clones BrainFlow tag 5.23.0 (commit 7994b54b), applies
# third_party/brainflow/emotibit-ancillary-upsample.patch, builds Release and
# installs to the prefix (inc/ + lib/). Re-running reuses the checkout.
#
# Environment:
#   BRAINFLOW_SRC_DIR          checkout to use/create   (default ~/.local/src/brainflow-5.23.0)
#   BRAINFLOW_BUILD_DIR        build tree               (default <src>/build-bioacq)
#   BRAINFLOW_REPO             clone URL                (default https://github.com/brainflow-dev/brainflow.git)
#   MACOSX_DEPLOYMENT_TARGET   oldest macOS to support  (default: the build machine's, via CMake)
set -euo pipefail

TAG="5.23.0"
COMMIT="7994b54b"
ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PATCH="$ROOT_DIR/third_party/brainflow/emotibit-ancillary-upsample.patch"
MARKER="patched (stream_gui_cpp)"
PREFIX="${1:-$HOME/.local/brainflow}"
SRC="${BRAINFLOW_SRC_DIR:-$HOME/.local/src/brainflow-$TAG}"
BUILD="${BRAINFLOW_BUILD_DIR:-$SRC/build-bioacq}"
REPO="${BRAINFLOW_REPO:-https://github.com/brainflow-dev/brainflow.git}"
if [[ -d /opt/homebrew/bin ]]; then
    export PATH="/opt/homebrew/bin:$PATH"
fi

for tool in git cmake; do
    command -v "$tool" >/dev/null || { echo "error: $tool not found" >&2; exit 1; }
done

if [[ ! -d "$SRC/.git" ]]; then
    echo "==> cloning BrainFlow $TAG into $SRC"
    mkdir -p "$(dirname "$SRC")"
    git -c advice.detachedHead=false clone --depth 1 --branch "$TAG" "$REPO" "$SRC"
fi

head="$(git -C "$SRC" rev-parse HEAD)"
if [[ "$head" != "$COMMIT"* ]]; then
    echo "error: $SRC is at $head, expected tag $TAG ($COMMIT)" >&2
    exit 1
fi

EMOTIBIT_CPP="$SRC/src/board_controller/emotibit/emotibit.cpp"
if grep -q "$MARKER" "$EMOTIBIT_CPP"; then
    echo "==> patch already applied"
else
    echo "==> applying $(basename "$PATCH")"
    git -C "$SRC" apply --ignore-whitespace "$PATCH"
fi
grep -n "$MARKER" "$EMOTIBIT_CPP"

generator=()
if command -v ninja >/dev/null; then
    generator=(-G Ninja)
fi
osx=()
if [[ -n "${MACOSX_DEPLOYMENT_TARGET:-}" ]]; then
    osx=(-DCMAKE_OSX_DEPLOYMENT_TARGET="$MACOSX_DEPLOYMENT_TARGET")
fi

echo "==> configuring ($BUILD)"
cmake -S "$SRC" -B "$BUILD" ${generator[@]+"${generator[@]}"} ${osx[@]+"${osx[@]}"} \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="$PREFIX" \
    -DBRAINFLOW_VERSION="$TAG" \
    -DBRAINFLOW_COPY_TO_PACKAGE_DIRS=OFF \
    -DCMAKE_POLICY_VERSION_MINIMUM=3.5

echo "==> building"
cmake --build "$BUILD" --config Release --parallel

echo "==> installing to $PREFIX"
cmake --install "$BUILD" --config Release

if [[ ! -f "$PREFIX/inc/board_shim.h" ]]; then
    echo "error: install incomplete: $PREFIX/inc/board_shim.h missing" >&2
    exit 1
fi
echo "BrainFlow $TAG (patched) installed in $PREFIX"
