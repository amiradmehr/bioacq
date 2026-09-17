#!/usr/bin/env bash
# Build BioAcq.app (packaged Release), make it self-contained, sign it ad hoc,
# verify it and zip it.
#
#   scripts/package_macos.sh
#
# Output:
#   dist/macos/BioAcq.app              double-click to start the GUI
#   dist/BioAcq-macos-<arch>.zip       the app, zipped with ditto (signature intact after any unzip)
#
# Steps: configure + build with -DBIOACQ_PACKAGED=ON -> the Qt plugins the app
# needs (cocoa + offscreen platforms for the GUI and headless --selftest /
# --screenshot, macOS style) -> macdeployqt (Qt frameworks and their
# dependencies) -> BrainFlow core dylibs into Contents/Frameworks -> install
# names and rpaths relative to the bundle -> LSMinimumSystemVersion from the
# bundled binaries -> licences into Contents/Resources/licenses (fonts,
# BrainFlow, Qt, and the Homebrew libraries a Homebrew Qt pulls in) ->
# ad-hoc codesign -> verify (signature, no /opt/homebrew or /Users paths in any
# load command or rpath, --selftest and --screenshot from inside the bundle
# with every loaded image inside the bundle, fonts registered, a launch via
# `open` that stays up for 5 s without connecting) -> zip.
#
# Environment:
#   QT_PREFIX             Qt install (default $QT_ROOT_DIR, else Homebrew /opt/homebrew/opt/qt)
#   BRAINFLOW_ROOT        patched BrainFlow install (default ~/.local/brainflow)
#   BIOACQ_BUILD_DIR      build tree (default ~/.local/build/bioacq-package-macos)
#   BIOACQ_SKIP_OPEN_TEST=1   skip the `open` launch test (e.g. CI without a login session)
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
QT_PREFIX="${QT_PREFIX:-${QT_ROOT_DIR:-/opt/homebrew/opt/qt}}"
BRAINFLOW_ROOT="${BRAINFLOW_ROOT:-$HOME/.local/brainflow}"
BUILD_DIR="${BIOACQ_BUILD_DIR:-$HOME/.local/build/bioacq-package-macos}"
DIST="$ROOT_DIR/dist"
APP="$DIST/macos/BioAcq.app"
EXE="$APP/Contents/MacOS/BioAcq"
FRAMEWORKS="$APP/Contents/Frameworks"
ARCH="$(uname -m)"
ZIP="$DIST/BioAcq-macos-$ARCH.zip"
CHECK_DIR="$BUILD_DIR/package-check"
BRAINFLOW_LIBS=(libBoardController.dylib libDataHandler.dylib libMLModule.dylib)
# Only what the app uses: it draws everything with QPainter (no image formats,
# SVG icons, TLS or input-method plugins).
QT_PLUGIN_SET=(platforms/libqcocoa.dylib platforms/libqoffscreen.dylib styles/libqmacstyle.dylib)
if [[ -d /opt/homebrew/bin ]]; then
    export PATH="/opt/homebrew/bin:$PATH"
fi

step () { printf '\n==> %s\n' "$*"; }
fail () { printf 'error: %s\n' "$*" >&2; exit 1; }
# run with a watchdog: timeout <seconds> cmd...
watchdog () { local s="$1"; shift; perl -e 'alarm shift; exec @ARGV' "$s" "$@"; }
# same, printing every image dyld loads to stderr (set inside perl: SIP strips
# DYLD_* variables when a protected binary such as /usr/bin/perl is launched)
watchdog_dyld () { local s="$1"; shift; perl -e '$ENV{DYLD_PRINT_LIBRARIES} = 1; alarm shift; exec @ARGV' "$s" "$@"; }

[[ -x "$QT_PREFIX/bin/macdeployqt" ]] || fail "macdeployqt not found under $QT_PREFIX/bin (set QT_PREFIX)"
[[ -f "$BRAINFLOW_ROOT/inc/board_shim.h" ]] || fail "BrainFlow not found in $BRAINFLOW_ROOT (set BRAINFLOW_ROOT)"
for lib in "${BRAINFLOW_LIBS[@]}"; do
    [[ -f "$BRAINFLOW_ROOT/lib/$lib" ]] || fail "$BRAINFLOW_ROOT/lib/$lib missing"
done

QT_PLUGINS="$("$QT_PREFIX/bin/qtpaths" --query QT_INSTALL_PLUGINS 2>/dev/null || true)"
[[ -d "$QT_PLUGINS" ]] || QT_PLUGINS="$QT_PREFIX/plugins"
for plugin in "${QT_PLUGIN_SET[@]}"; do
    [[ -f "$QT_PLUGINS/$plugin" ]] || fail "Qt plugin $plugin not found in $QT_PLUGINS"
done

is_macho () { file -b "$1" | grep -q 'Mach-O'; }
macho_files () { find "$APP" -type f | while IFS= read -r f; do if is_macho "$f"; then printf '%s\n' "$f"; fi; done; }
rpaths_of () { otool -l "$1" | awk '$1 == "cmd" && $2 == "LC_RPATH" { r = 1 } r && $1 == "path" { print $2; r = 0 }'; }
# dylibs this file loads (not its own install name)
deps_of () { otool -l "$1" | awk '$1 == "cmd" { d = ($2 ~ /^LC_(LOAD|LOAD_WEAK|REEXPORT|LAZY_LOAD|LOAD_UPWARD)_DYLIB$/) } d && $1 == "name" { print $2; d = 0 }'; }
install_id_of () { otool -D "$1" | tail -n +2; }

# ---------------------------------------------------------------- build
step "building (packaged Release) in $BUILD_DIR"
generator=()
if command -v ninja >/dev/null; then
    generator=(-G Ninja)
fi
cmake -S "$ROOT_DIR" -B "$BUILD_DIR" ${generator[@]+"${generator[@]}"} \
    -DCMAKE_BUILD_TYPE=Release \
    -DBIOACQ_PACKAGED=ON \
    -DBRAINFLOW_ROOT="$BRAINFLOW_ROOT" \
    -DQT_PREFIX="$QT_PREFIX" \
    -DCMAKE_PREFIX_PATH="$QT_PREFIX"
cmake --build "$BUILD_DIR" --config Release --parallel
[[ -x "$BUILD_DIR/BioAcq.app/Contents/MacOS/BioAcq" ]] || fail "build produced no BioAcq.app"

rm -rf "$APP" "$ZIP"
mkdir -p "$DIST/macos"
ditto "$BUILD_DIR/BioAcq.app" "$APP"

# ---------------------------------------------------------------- deploy
step "Qt plugins"
deploy_args=()
for plugin in "${QT_PLUGIN_SET[@]}"; do
    mkdir -p "$APP/Contents/PlugIns/$(dirname "$plugin")"
    cp "$QT_PLUGINS/$plugin" "$APP/Contents/PlugIns/$plugin"
    deploy_args+=("-executable=$APP/Contents/PlugIns/$plugin")
    echo "  $plugin"
done
if [[ ! -f "$APP/Contents/Resources/qt.conf" ]]; then
    printf '[Paths]\nPlugins = PlugIns\n' > "$APP/Contents/Resources/qt.conf"
fi

step "macdeployqt"
"$QT_PREFIX/bin/macdeployqt" "$APP" -no-plugins -always-overwrite -verbose=1 "${deploy_args[@]}"
if [[ ! -f "$APP/Contents/Resources/qt.conf" ]]; then
    printf '[Paths]\nPlugins = PlugIns\n' > "$APP/Contents/Resources/qt.conf"
fi

step "BrainFlow dylibs"
mkdir -p "$FRAMEWORKS"
for lib in "${BRAINFLOW_LIBS[@]}"; do
    cp -f "$BRAINFLOW_ROOT/lib/$lib" "$FRAMEWORKS/$lib"
    chmod u+w "$FRAMEWORKS/$lib"
    install_name_tool -id "@rpath/$lib" "$FRAMEWORKS/$lib" 2>/dev/null
    echo "  $lib"
done

step "rpaths and install names"
while IFS= read -r f; do
    chmod u+w "$f"
    id="$(install_id_of "$f")"
    case "$id" in
        "" | @*) ;;
        *)
            rel="${f#"$FRAMEWORKS"/}"
            [[ "$rel" == "$f" ]] && rel="$(basename "$f")"
            install_name_tool -id "@rpath/$rel" "$f" 2>/dev/null
            echo "  = id $id -> @rpath/$rel"
            ;;
    esac
    while IFS= read -r rp; do
        case "$rp" in
            @*) ;;
            *) install_name_tool -delete_rpath "$rp" "$f" 2>/dev/null && echo "  - rpath $rp  (${f#"$APP"/})" ;;
        esac
    done < <(rpaths_of "$f")
    while IFS= read -r dep; do
        case "$dep" in
            /System/* | /usr/lib/* | @*) ;;
            *)
                base="$(basename "$dep")"
                if [[ -f "$FRAMEWORKS/$base" ]]; then
                    install_name_tool -change "$dep" "@executable_path/../Frameworks/$base" "$f" 2>/dev/null
                    echo "  ~ $dep -> @executable_path/../Frameworks/$base  (${f#"$APP"/})"
                fi
                ;;
        esac
    done < <(deps_of "$f")
done < <(macho_files)
if ! rpaths_of "$EXE" | grep -qx '@executable_path/../Frameworks'; then
    install_name_tool -add_rpath "@executable_path/../Frameworks" "$EXE"
fi
echo "  executable rpaths: $(rpaths_of "$EXE" | tr '\n' ' ')"

step "LSMinimumSystemVersion"
min_os="$(macho_files | while IFS= read -r f; do
    otool -l "$f" | awk '($1 == "minos" || ($1 == "version" && prev ~ /LC_VERSION_MIN_MACOSX/)) { print $2 } $1 == "cmd" { prev = $2 }'
done | sort -t. -k1,1n -k2,2n -k3,3n | tail -1)"
[[ -n "$min_os" ]] || fail "could not read the minimum macOS version of the bundled binaries"
/usr/libexec/PlistBuddy -c "Set :LSMinimumSystemVersion $min_os" "$APP/Contents/Info.plist"
echo "  $min_os (highest minimum of all bundled binaries)"

# ---------------------------------------------------------------- licences
# Before signing: Contents/Resources is sealed by the signature.
step "licences"
LICENSES="$APP/Contents/Resources/licenses"
rm -rf "$LICENSES"
mkdir -p "$LICENSES"
cp "$ROOT_DIR"/resources/fonts/*-OFL.txt "$LICENSES/"
cp "$ROOT_DIR/third_party/brainflow/LICENSE" "$LICENSES/BrainFlow-LICENSE.txt"
cp "$ROOT_DIR/third_party/brainflow/README.md" "$LICENSES/BrainFlow-patch-README.md"
cp "$ROOT_DIR/third_party/qt/README.md" "$LICENSES/Qt-NOTICE.md"
cp "$ROOT_DIR/third_party/qt/LGPL-3.0.txt" "$LICENSES/Qt-LGPL-3.0.txt"
cp "$ROOT_DIR/third_party/qt/GPL-3.0.txt" "$LICENSES/Qt-GPL-3.0.txt"
echo "  fonts (OFL), BrainFlow (MIT), Qt (LGPL-3.0 + GPL-3.0 texts, notice)"
# Libraries macdeployqt copied from outside Qt (a Homebrew Qt pulls in ICU,
# OpenSSL, GLib, HarfBuzz, ...): each one's licence files from its Homebrew keg,
# and a summary with the formula version and SPDX licence.
brew_prefix=""
if command -v brew >/dev/null; then
    brew_prefix="$(brew --prefix 2>/dev/null || true)"
fi
extra_libs=()
for f in "$FRAMEWORKS"/*.dylib; do
    [[ -e "$f" ]] || continue
    base="$(basename "$f")"
    case " ${BRAINFLOW_LIBS[*]} " in *" $base "*) continue ;; esac
    extra_libs+=("$base")
done
if [[ ${#extra_libs[@]} -gt 0 ]]; then
    [[ -n "$brew_prefix" ]] || fail "bundled libraries ${extra_libs[*]} come from outside Qt, but brew is not available to find their licences"
    mkdir -p "$LICENSES/homebrew"
    summary="$LICENSES/homebrew/THIRD-PARTY.txt"
    formulae=()
    rows=()
    for base in "${extra_libs[@]}"; do
        src="$(ls -d "$brew_prefix"/opt/*/lib/"$base" 2>/dev/null | head -1 || true)"
        [[ -n "$src" ]] || fail "no Homebrew keg provides $base: cannot ship its licence"
        keg="$(cd "$(dirname "$src")/.." && pwd -P)"
        formula="$(basename "$(dirname "$keg")")"
        version="$(basename "$keg")"
        if [[ ! -d "$LICENSES/homebrew/$formula" ]]; then
            mkdir -p "$LICENSES/homebrew/$formula"
            find "$keg" -maxdepth 1 -type f \( -iname 'LICEN[CS]E*' -o -iname 'COPYING*' -o -iname 'NOTICE*' \
                -o -iname 'COPYRIGHT*' -o -iname '*GPL*' \) -exec cp {} "$LICENSES/homebrew/$formula/" \;
            formulae+=("$formula")
        fi
        rows+=("$base $formula $version")
    done
    spdx_json="$(HOMEBREW_NO_AUTO_UPDATE=1 HOMEBREW_NO_ANALYTICS=1 watchdog 120 brew info --json=v2 --formula "${formulae[@]}" 2>/dev/null || true)"
    {
        echo "Libraries bundled in BioAcq.app/Contents/Frameworks besides Qt and BrainFlow."
        echo "macdeployqt copied them from Homebrew as dependencies of Homebrew's Qt. Each formula's licence files"
        echo "are in the folder of the same name; the licence column is Homebrew's SPDX expression."
        echo
        printf '%-32s %-20s %-14s %s\n' library formula version licence
        for row in "${rows[@]}"; do
            read -r base formula version <<<"$row"
            spdx="$(printf '%s' "$spdx_json" | /usr/bin/python3 -c 'import json, sys
try:
    data = json.load(sys.stdin)
    print(next((f.get("license") or "?") for f in data["formulae"] if f["name"] == sys.argv[1]))
except Exception:
    print("?")' "$formula" 2>/dev/null || echo "?")"
            printf '%-32s %-20s %-14s %s\n' "$base" "$formula" "$version" "$spdx"
        done
    } > "$summary"
    for formula in "${formulae[@]}"; do
        if [[ -z "$(ls -A "$LICENSES/homebrew/$formula")" ]]; then
            grep -E "^[^ ]+ +$formula " "$summary" | grep -qv ' ?$' ||
                fail "no licence file and no SPDX licence for Homebrew formula $formula"
            echo "  $formula: no licence file in the keg (SPDX licence in THIRD-PARTY.txt)"
            rmdir "$LICENSES/homebrew/$formula"
        fi
    done
    echo "  ${#extra_libs[@]} Homebrew libraries from ${#formulae[@]} formulae: licences in Resources/licenses/homebrew"
fi

# ---------------------------------------------------------------- sign
step "ad-hoc codesign"
xattr -cr "$APP"
codesign --force --deep --sign - --timestamp=none "$APP"

# ---------------------------------------------------------------- verify
step "verify: signature"
codesign --verify --deep --strict --verbose=2 "$APP"

step "verify: load commands and rpaths"
bad=0
while IFS= read -r f; do
    rel="${f#"$APP"/}"
    while IFS= read -r dep; do
        case "$dep" in
            /opt/homebrew/* | /usr/local/* | /Users/* | "$HOME"/*) echo "  BAD load command in $rel: $dep"; bad=1 ;;
        esac
    done < <(otool -L "$f" | tail -n +2 | awk '{ print $1 }')
    while IFS= read -r rp; do
        case "$rp" in
            @*) ;;
            *) echo "  BAD rpath in $rel: $rp"; bad=1 ;;
        esac
    done < <(rpaths_of "$f")
done < <(macho_files)
[[ "$bad" == 0 ]] || fail "the bundle still references files outside itself"
echo "  $(macho_files | wc -l | tr -d ' ') Mach-O files, no /opt/homebrew, /usr/local or /Users paths"
for req in PlugIns/platforms/libqcocoa.dylib PlugIns/platforms/libqoffscreen.dylib Resources/qt.conf \
    Frameworks/QtCore.framework Frameworks/QtWidgets.framework Frameworks/QtSerialPort.framework \
    Frameworks/QtBluetooth.framework; do
    [[ -e "$APP/Contents/$req" ]] || fail "missing Contents/$req"
done
for key in NSLocalNetworkUsageDescription NSBluetoothAlwaysUsageDescription; do
    /usr/libexec/PlistBuddy -c "Print :$key" "$APP/Contents/Info.plist" >/dev/null ||
        fail "Info.plist has no $key"
done
for req in Inter-OFL.txt JetBrainsMono-OFL.txt BrainFlow-LICENSE.txt Qt-NOTICE.md Qt-LGPL-3.0.txt Qt-GPL-3.0.txt; do
    [[ -s "$APP/Contents/Resources/licenses/$req" ]] || fail "missing licence Contents/Resources/licenses/$req"
done
echo "  licences present (fonts, BrainFlow, Qt$([[ -d "$APP/Contents/Resources/licenses/homebrew" ]] && echo ', Homebrew libraries'))"

rm -rf "$CHECK_DIR"
mkdir -p "$CHECK_DIR"
# Every image the process loads must come from the bundle or the OS.
check_loaded_images () {
    local log="$1" outside
    grep -qE '^dyld\[[0-9]+\]: <' "$log" || { echo "  no dyld image list in $log"; return 1; }
    outside="$(grep -E '^dyld\[[0-9]+\]: <[^>]+> ' "$log" | sed -E 's/^dyld\[[0-9]+\]: <[^>]+> //' |
        grep -vF "$APP/" | grep -vE '^(/System/|/usr/lib/|/Library/Apple/|/private/preboot/|/System/Volumes/Preboot/)' ||
        true)"
    if [[ -n "$outside" ]]; then
        echo "$outside" | sed 's/^/  loaded from outside the bundle: /'
        return 1
    fi
    echo "  $(grep -cE '^dyld\[[0-9]+\]: <' "$log") images loaded, all from the bundle or the OS"
}

step "verify: --selftest from inside the bundle"
set +e
(cd "$CHECK_DIR" && watchdog_dyld 240 "$EXE" --selftest > selftest.out 2> selftest.err)
rc=$?
set -e
grep -E '^\s*\[FAIL\]|^RESULT' "$CHECK_DIR/selftest.out" || true
[[ "$rc" == 0 ]] && grep -q '^RESULT: PASS' "$CHECK_DIR/selftest.out" ||
    { tail -40 "$CHECK_DIR/selftest.out"; grep -v '^dyld\[' "$CHECK_DIR/selftest.err" | tail -20; fail "selftest failed (exit $rc)"; }
check_loaded_images "$CHECK_DIR/selftest.err" || fail "selftest loaded libraries from outside the bundle"

step "verify: --screenshot from inside the bundle"
set +e
(cd "$CHECK_DIR" && QT_QPA_PLATFORM=offscreen watchdog_dyld 90 \
    "$EXE" --screenshot "$CHECK_DIR/live.png" --state live --size 1280x800 > shot.out 2> shot.err)
rc=$?
set -e
cat "$CHECK_DIR/shot.out"
[[ "$rc" == 0 && -s "$CHECK_DIR/live.png" ]] ||
    { grep -v '^dyld\[' "$CHECK_DIR/shot.err" | tail -20; fail "screenshot failed (exit $rc)"; }
if grep -q 'could not be registered' "$CHECK_DIR/shot.err"; then
    grep 'could not be registered' "$CHECK_DIR/shot.err"
    fail "embedded fonts did not load"
fi
echo "  fonts: JetBrains Mono and Inter registered (no fallback warning)"
check_loaded_images "$CHECK_DIR/shot.err" || fail "screenshot loaded libraries from outside the bundle"
grep -q "$APP/Contents/PlugIns/platforms/libqoffscreen.dylib" "$CHECK_DIR/shot.err" ||
    fail "the offscreen platform plugin was not loaded from the bundle"
echo "  screenshot: $CHECK_DIR/live.png"

if [[ "${BIOACQ_SKIP_OPEN_TEST:-0}" != 1 ]]; then
    step "verify: double-click launch (open), no auto-connect"
    touch "$CHECK_DIR/.open-start"
    open "$APP"
    pid=""
    for _ in $(seq 1 50); do
        pid="$(pgrep -f "^$EXE" | head -1 || true)"
        [[ -n "$pid" ]] && break
        sleep 0.2
    done
    [[ -n "$pid" ]] || fail "BioAcq did not start via open"
    echo "  running as pid $pid; watching for 5 s"
    sleep 5
    kill -0 "$pid" 2>/dev/null || fail "BioAcq exited or crashed within 5 s of launch"
    open_files="$(lsof -p "$pid" 2>/dev/null || true)"
    if grep -qE 'usbserial|COM[0-9]|:3131' <<<"$open_files"; then
        grep -E 'usbserial|:3131' <<<"$open_files"
        fail "BioAcq opened a device on launch (it must not auto-connect)"
    fi
    echo "  still running, no serial port or EmotiBit socket open"
    watchdog 10 osascript -e 'quit app "BioAcq"' >/dev/null 2>&1 || true
    for _ in $(seq 1 50); do
        kill -0 "$pid" 2>/dev/null || break
        sleep 0.2
    done
    if kill -0 "$pid" 2>/dev/null; then
        echo "  quit via AppleScript did not finish; sending SIGTERM"
        kill -TERM "$pid" 2>/dev/null || true
        sleep 3
        kill -0 "$pid" 2>/dev/null && kill -KILL "$pid" 2>/dev/null || true
    fi
    crash="$(find "$HOME/Library/Logs/DiagnosticReports" -name 'BioAcq*' -newer "$CHECK_DIR/.open-start" 2>/dev/null |
        head -1 || true)"
    [[ -z "$crash" ]] || fail "crash report written: $crash"
    echo "  quit cleanly, no crash report"
fi

# ---------------------------------------------------------------- zip
step "zip"
# No extended attributes (macOS tags every file with com.apple.provenance): as
# ._* AppleDouble entries they would break the signature seal for anyone who
# unpacks the zip with a plain unzip tool.
ditto -c -k --norsrc --noextattr --noacl --keepParent "$APP" "$ZIP"
if unzip -l "$ZIP" | grep -q '/\._'; then
    fail "the zip contains AppleDouble (._*) entries"
fi
rm -rf "$CHECK_DIR/unzipped"
mkdir -p "$CHECK_DIR/unzipped"
unzip -q "$ZIP" -d "$CHECK_DIR/unzipped"
codesign --verify --deep --strict "$CHECK_DIR/unzipped/BioAcq.app" || fail "the app unpacked with unzip fails codesign --verify"
echo "  unpacked with unzip: signature valid"
echo "  $ZIP ($(du -h "$ZIP" | cut -f1 | tr -d ' '))"
echo "  $APP ($(du -sh "$APP" | cut -f1 | tr -d ' '))"
