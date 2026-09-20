#!/usr/bin/env bash
# Build a relocatable BioAcq folder for desktop Linux and tar it.
#
#   scripts/package_linux.sh
#
# Output:
#   dist/linux/BioAcq/BioAcq            start the GUI (launcher next to bin/, lib/, plugins/)
#   dist/BioAcq-linux-<arch>.tar.gz     the same folder, tarred
#
# This is the desktop counterpart of scripts/package_macos.sh. For an embedded
# image, build the recipe in yocto/meta-bioacq instead: there Qt and BrainFlow
# come from the image, so nothing is bundled.
#
# Steps: configure + build with -DBIOACQ_PACKAGED=ON (records into <Documents>/
# BioAcq Recordings) and -DBIOACQ_LINUX_BUNDLE=ON (rpath $ORIGIN/../lib) ->
# cmake --install into dist/linux/BioAcq, which is the same install() the Yocto
# recipe uses (binary, desktop entry, icons, licences) -> the Qt and BrainFlow
# shared libraries the build-tree binary and the Qt plugins pull in -> the Qt
# platform plugins (xcb, wayland, eglfs, linuxfb, offscreen, minimal, vnc) ->
# bin/qt.conf so the binary finds those plugins without environment variables ->
# verify (--selftest and a --screenshot from inside the folder with Qt taken off
# the environment, and no library loaded from the Qt prefix) -> tar.gz.
#
# Environment:
#   QT_PREFIX          Qt 6 install (default $QT_ROOT_DIR, else qmake6/qmake on PATH)
#   BRAINFLOW_ROOT     patched BrainFlow install (default ~/.local/brainflow)
#   BIOACQ_BUILD_DIR   build tree (default ~/.local/build/bioacq-package-linux)
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BRAINFLOW_ROOT="${BRAINFLOW_ROOT:-$HOME/.local/brainflow}"
BUILD_DIR="${BIOACQ_BUILD_DIR:-$HOME/.local/build/bioacq-package-linux}"
DIST="$ROOT_DIR/dist"
PKG="$DIST/linux/BioAcq"
ARCH="$(uname -m)"
TARBALL="$DIST/BioAcq-linux-$ARCH.tar.gz"

step () { printf '\n==> %s\n' "$*"; }
fail () { printf 'error: %s\n' "$*" >&2; exit 1; }

[[ "$(uname -s)" == "Linux" ]] || fail "this script builds the Linux package; run it on Linux"

# ------------------------------------------------------------------ Qt prefix
QT_PREFIX="${QT_PREFIX:-${QT_ROOT_DIR:-}}"
if [[ -z "$QT_PREFIX" ]]; then
    for qmake in qmake6 qmake; do
        if command -v "$qmake" >/dev/null; then
            QT_PREFIX="$("$qmake" -query QT_INSTALL_PREFIX 2>/dev/null || true)"
            [[ -n "$QT_PREFIX" ]] && break
        fi
    done
fi
[[ -n "$QT_PREFIX" && -d "$QT_PREFIX" ]] || fail "Qt 6 not found (set QT_PREFIX, or put qmake6 on PATH)"
QT_LIBS="$QT_PREFIX/lib"
[[ -d "$QT_LIBS" ]] || QT_LIBS="$QT_PREFIX/lib64"
QT_PLUGINS="$QT_PREFIX/plugins"
for q in qtpaths6 qtpaths; do
    if command -v "$QT_PREFIX/bin/$q" >/dev/null; then
        p="$("$QT_PREFIX/bin/$q" --query QT_INSTALL_PLUGINS 2>/dev/null || true)"
        [[ -d "$p" ]] && QT_PLUGINS="$p"
        break
    fi
done
[[ -d "$QT_PLUGINS/platforms" ]] || fail "no Qt platform plugins under $QT_PLUGINS"
[[ -f "$BRAINFLOW_ROOT/inc/board_shim.h" || -f "$BRAINFLOW_ROOT/include/brainflow/board_shim.h" ]] ||
    fail "BrainFlow not found in $BRAINFLOW_ROOT (set BRAINFLOW_ROOT, or run scripts/build_brainflow.sh)"

# --------------------------------------------------------------------- build
step "building (packaged Release) in $BUILD_DIR"
generator=()
command -v ninja >/dev/null && generator=(-G Ninja)
cmake -S "$ROOT_DIR" -B "$BUILD_DIR" ${generator[@]+"${generator[@]}"} \
    -DCMAKE_BUILD_TYPE=Release \
    -DBIOACQ_PACKAGED=ON \
    -DBIOACQ_LINUX_BUNDLE=ON \
    -DBRAINFLOW_ROOT="$BRAINFLOW_ROOT" \
    -DQT_PREFIX="$QT_PREFIX" \
    -DCMAKE_PREFIX_PATH="$QT_PREFIX;$BRAINFLOW_ROOT"
cmake --build "$BUILD_DIR" --parallel
[[ -x "$BUILD_DIR/bioacq" ]] || fail "no binary at $BUILD_DIR/bioacq"

# --------------------------------------------------------------------- stage
step "installing into $PKG"
rm -rf "$DIST/linux" "$TARBALL"
mkdir -p "$PKG"
cmake --install "$BUILD_DIR" --prefix "$PKG"
[[ -x "$PKG/bin/bioacq" ]] || fail "cmake --install left no binary at $PKG/bin/bioacq"
for licence in Inter-OFL.txt JetBrainsMono-OFL.txt LICENSE LGPL-3.0.txt GPL-3.0.txt; do
    [[ -f "$PKG/share/doc/bioacq/licenses/$licence" ]] || fail "licence file missing: $licence"
done
mkdir -p "$PKG/lib" "$PKG/plugins"

# Shared libraries the app must carry: everything ldd resolves inside the Qt
# prefix or the BrainFlow install. The walk runs on the build-tree binary, which
# still has the rpath into both; the installed one points at lib/, which is
# empty at this point. System libraries (glibc, libstdc++, libGL, X11, Wayland,
# D-Bus, ...) stay on the host: bundling them is what breaks a folder on the
# next distro.
copied_libs=()
# The walk always runs ldd on the library where it was installed, never on the
# copy in lib/: Qt's own libraries carry an $ORIGIN rpath, so a copy resolves
# its siblings inside the half-filled package and the walk stops early.
copy_deps () # copy_deps <elf file>...
{
    local queue=("$@") file dep base
    while ((${#queue[@]})); do
        file="${queue[0]}"
        queue=("${queue[@]:1}")
        while read -r dep; do
            [[ -f "$dep" ]] || continue
            case "$dep" in
                "$QT_LIBS"/* | "$QT_PREFIX"/* | "$BRAINFLOW_ROOT"/*) ;;
                *) continue ;;
            esac
            base="$(basename "$dep")"
            [[ -f "$PKG/lib/$base" ]] && continue
            cp -L "$dep" "$PKG/lib/$base"
            chmod 644 "$PKG/lib/$base"
            copied_libs+=("$base")
            queue+=("$dep")
        done < <(ldd "$file" 2>/dev/null | awk '$2 == "=>" && $3 ~ /^\// { print $3 } $1 ~ /^\// { print $1 }')
    done
}

step "collecting Qt and BrainFlow libraries"
copy_deps "$BUILD_DIR/bioacq"

# Qt plugins: the platform plugins an image might use, plus the integrations the
# xcb and wayland ones load themselves. Only what exists in this Qt is copied.
QT_PLUGIN_FILES=(
    platforms/libqxcb.so
    platforms/libqwayland-generic.so
    platforms/libqwayland-egl.so
    platforms/libqeglfs.so
    platforms/libqlinuxfb.so
    platforms/libqminimal.so
    platforms/libqoffscreen.so
    platforms/libqvnc.so
)
QT_PLUGIN_DIRS=(xcbglintegrations egldeviceintegrations wayland-shell-integration wayland-decoration-client
    wayland-graphics-integration-client)

step "collecting Qt plugins"
plugin_sources=()
for rel in "${QT_PLUGIN_FILES[@]}"; do
    [[ -f "$QT_PLUGINS/$rel" ]] || continue
    mkdir -p "$PKG/plugins/$(dirname "$rel")"
    cp -L "$QT_PLUGINS/$rel" "$PKG/plugins/$rel"
    plugin_sources+=("$QT_PLUGINS/$rel")
done
for dir in "${QT_PLUGIN_DIRS[@]}"; do
    [[ -d "$QT_PLUGINS/$dir" ]] || continue
    mkdir -p "$PKG/plugins/$dir"
    for so in "$QT_PLUGINS/$dir"/*.so; do
        [[ -f "$so" ]] || continue
        cp -L "$so" "$PKG/plugins/$dir/"
        plugin_sources+=("$so")
    done
done
[[ -f "$PKG/plugins/platforms/libqoffscreen.so" ]] || fail "the offscreen platform plugin is missing from $QT_PLUGINS"
copy_deps "${plugin_sources[@]}"

# The binary sits in bin/ with rpath $ORIGIN/../lib, so qt.conf only has to
# point Qt at the plugin folder: no launcher environment, and a plain
# ./bin/bioacq works as well as the launcher.
cat > "$PKG/bin/qt.conf" <<'EOF'
[Paths]
Prefix = ..
Plugins = plugins
Libraries = lib
EOF

cat > "$PKG/BioAcq" <<'EOF'
#!/bin/sh
# BioAcq launcher. The binary in bin/ finds its libraries through its rpath and
# its Qt plugins through bin/qt.conf, so there is nothing to set up here:
# bin/bioacq works just as well, and this is only the friendlier name.
here=$(cd "$(dirname "$0")" && pwd)
exec "$here/bin/bioacq" "$@"
EOF
chmod 755 "$PKG/BioAcq"

# -------------------------------------------------------------------- verify
# Nothing outside the folder may fill a gap: Qt off LD_LIBRARY_PATH, no
# QT_PLUGIN_PATH, and the loader's own report checked for stray Qt libraries.
run_packaged () # run_packaged <timeout s> <args...>
{
    local seconds="$1"
    shift
    env -u QT_PLUGIN_PATH -u QML2_IMPORT_PATH -u QT_QPA_PLATFORM_PLUGIN_PATH LD_LIBRARY_PATH= \
        timeout "$seconds" "$PKG/BioAcq" "$@"
}

step "verify: selftest from the package"
QT_QPA_PLATFORM=offscreen run_packaged 300 --selftest | tail -n 3

step "verify: screenshot from the package"
shot="$BUILD_DIR/package-check-live.png"
rm -f "$shot"
QT_QPA_PLATFORM=offscreen run_packaged 120 --screenshot "$shot" --state live --size 1280x800 >/dev/null
[[ -s "$shot" ]] || fail "no screenshot at $shot"

step "verify: no library loaded from $QT_PREFIX or $BRAINFLOW_ROOT"
loaded="$(env -u QT_PLUGIN_PATH LD_LIBRARY_PATH= LD_DEBUG=libs QT_QPA_PLATFORM=offscreen \
    timeout 120 "$PKG/bin/bioacq" --list-ports 2>&1 >/dev/null | grep -F 'calling init' || true)"
if grep -qF "$QT_PREFIX/" <<<"$loaded" || grep -qF "$BRAINFLOW_ROOT/" <<<"$loaded"; then
    grep -F -e "$QT_PREFIX/" -e "$BRAINFLOW_ROOT/" <<<"$loaded" >&2
    fail "the package loaded a library from outside dist/linux/BioAcq"
fi

# ----------------------------------------------------------------- tar it up
step "tarring $TARBALL"
tar -C "$DIST/linux" -czf "$TARBALL" BioAcq

printf '\nPackaged: %s\n' "$PKG"
printf 'Tarball:  %s (%s)\n' "$TARBALL" "$(du -h "$TARBALL" | cut -f1)"
printf 'Bundled:  %d shared libraries, %d Qt plugins\n' \
    "${#copied_libs[@]}" "$(find "$PKG/plugins" -name '*.so' | wc -l | tr -d ' ')"
