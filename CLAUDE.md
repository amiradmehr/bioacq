# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Layout

This folder is the `bioacq` repository (GitHub `amiradmehr/bioacq`, private). `bioacq` is a C++17 / Qt6 Widgets real-time viewer that streams two devices through the BrainFlow C++ API: an OpenBCI Cyton (serial dongle, one ECG channel on EXG channel 1) and an EmotiBit (WiFi: PPG green / red / IR, a heart rate derived from them, temperature, accel/gyro/mag in one IMU panel). One Connect button opens both devices. `README.md` specifies the UI behaviour, modes, networking and known limitations in detail. Read the relevant section before changing behaviour, and update it when behaviour changes.

- `CMakeLists.txt`: the build (target `bioacq`).
- `src/app/`: `main.cpp`, `BuildConfig.h.in`. `src/core/`: `RingBuffer.h`, `RateMeter.h`, `Readouts.h`, `Decimate.h`. `src/dsp/`: `Biquad.h`, `HeartRate` (PPG beat detector + tracker). `src/devices/`: `DeviceWorker`, `EmotiBitDiscovery`, `SignalSpec`, `Retimer.h`. `src/ui/`: `MainWindow`, `PlotWidget`, `Widgets`, `Theme`. `src/tools/`: `Headless` (selftest, probe). `src/platform/`: `Sockets.h`, `SerialPorts`, `AppPaths`, `ProcessSetup`, `compat/win32/sys/resource.h`. Every `src/` subfolder is an include directory, so includes are plain `#include "X.h"`.
- `resources/fonts/`: the embedded TTFs and OFL texts (served at `:/fonts/...`). `resources/macos/` (`Info.plist.in`, `BioAcq.icns`) and `resources/windows/` (`bioacq.rc.in`, `BioAcq.ico`) are the package resources.
- `scripts/`: `build.sh`, `run.sh` (development build); `build_brainflow.sh` / `.ps1`; `package_macos.sh`, `package_windows.ps1`; `run_cli_windows.ps1`.
- `.github/workflows/`: `windows.yml` (runs on push), `macos.yml` (manual only, never trigger it casually: macOS minutes cost 10x on this private repo).
- `third_party/brainflow/`: the local BrainFlow patch, BrainFlow's licence and the build instructions.
- `prototype/stream_gui.py` is the original PySide6 + pyqtgraph prototype, now superseded. The user rejected Python for real-time streaming UIs as too slow, so new work goes into the C++/Qt code.
- `design/Biosignal streaming GUI system.zip` is the Claude Design export that serves as the visual spec (`Instrument Screen.dc.html`, `Component Sheet.dc.html`, `Biosignal GUI.dc.html`). The `src/ui/Theme.h` tokens and the pixel sizes in `src/ui/Widgets.cpp` / `src/ui/PlotWidget.cpp` mirror it. Read it without extracting: `unzip -p "design/Biosignal streaming GUI system.zip" "Instrument Screen.dc.html"`. Its `_ds/` folder is the author's website design system and only supplies the font families. The instrument colours are inline hex in the `.dc.html` files.
- Branches: `main` holds the source, docs and CI and never build output (`dist/` is gitignored). `mac` / `windows` are `main` plus the packaged build committed under `dist/` (`BioAcq.app`; the `BioAcq` folder with `BioAcq.exe`), refreshed from `main` and never merged back. `feat/platform` (Windows port, packaging, CI) and `feat/features` (channels, heart rate, one Connect button) are merged into `main`; new work goes on `feat/*` branches merged with merge commits.
- The folder lives in Google Drive under a path with spaces, and one folder name starts with a space (`" demo Columbia"`), so quote every path. The git database is kept outside Drive at `~/.local/git/bioacq.git` (the `.git` here is a gitdir file pointing to it).

## Build and run

The scripts find their own location, so they work from any directory:

```bash
scripts/build.sh           # configure on first run, then incremental Release build (Ninja)
scripts/build.sh --clean   # wipe the build tree and reconfigure
scripts/run.sh [args]      # build if needed, then exec the binary with args
```

- The build tree is kept outside Google Drive on purpose, at `~/.local/build/bioacq` (override with `BIOACQ_BUILD_DIR`; `STREAM_GUI_BUILD_DIR` is still accepted as a fallback). The binary and `compile_commands.json` are there. clangd won't find `compile_commands.json` on its own.
- `build.sh` only runs the CMake configure step when there's no cache (or the cache says `BIOACQ_PACKAGED=ON`, i.e. a packaging script configured the tree; it then configures it back with `-DBIOACQ_PACKAGED=OFF`), so `BRAINFLOW_ROOT` / `QT_PREFIX` overrides need `--clean`. The default recording folder is `defaultRecordDir ()` (`src/platform/AppPaths.cpp`): in development builds `BIOACQ_RECORD_DIR`, i.e. `recordings/` at the repo root (inside Drive, gitignored; a CMake cache variable baked in through `src/app/BuildConfig.h.in`) if it's writable; in packaged builds and otherwise `Documents/BioAcq Recordings`. `--record-dir` overrides it at runtime.
- Dependencies: Homebrew Qt 6 (`/opt/homebrew/opt/qt`, including Qt Serial Port), CMake ≥ 3.21, Ninja, and BrainFlow 5.23.0 installed in `~/.local/brainflow` (patched, see below).
- `CMakeLists.txt` lists every source and header explicitly in `add_executable`, so new files must be added there (Windows-only files go in the `if(WIN32)` `target_sources`). AUTOMOC is on, and the fonts are compiled in with `qt_add_resources`. BrainFlow is found directly under `BRAINFLOW_ROOT` (`inc/`, `lib/`), not through its `brainflowConfig.cmake`, which hard-codes the install prefix.
- Packaged builds: `-DBIOACQ_PACKAGED=ON` (records into `Documents/BioAcq Recordings`; on macOS builds `BioAcq.app`, target output name `BioAcq`). `scripts/package_macos.sh` builds, deploys, signs and verifies `dist/macos/BioAcq.app` + `dist/BioAcq-macos-arm64.zip`; `scripts\package_windows.ps1` produces `dist\windows\BioAcq\BioAcq.exe` + `dist\BioAcq-windows-x64.zip`. `dist/` is gitignored on `main`. Use a separate `BIOACQ_BUILD_DIR` for packaging (a shared tree works but rebuilds everything on each switch). On macOS `BIOACQ_MACOS_BUNDLE` is a plain variable derived from `BIOACQ_PACKAGED` on every configure, not an option: an `option()` default only applied to a new tree and left reused trees building the wrong target.

## Testing

There is no unit-test framework. A single headless end-to-end suite runs the GUI's own worker and ring code on BrainFlow's synthetic board:

```bash
scripts/run.sh --selftest                               # ~15 s, exit 0 on PASS
scripts/run.sh --selftest 2>&1 | grep -E "FAIL|RESULT"   # only failures + the summary
```

- Output: each check prints `[ OK ]` or `[FAIL]`, and the run ends with `RESULT: PASS (n/n checks passed)`. A single check can't be run on its own.
- Stages:
  1. Unit checks: ring, decimation, retimer, biquads (ECG high-pass 0.5 Hz, notch, low-pass 40 Hz), heart rate (synthetic PPG at 60/72/120/180 bpm, inverted-count alignment, noise / flat, 45 / 60 bpm under 1.5 × respiratory wander, a 2 s dropout during the rate check and a real 21 Hz stream, a 15 × motion artefact, source selection), board-row mapping (incl. PPG order), `Readouts` rules (2 s rail window clearing, saturation wording, heart-rate readout), CPU meter, serial port enumeration (dongles first, BrainFlow port names, under 250 ms because the GUI rescans at 1 Hz), typed ports in the OS spelling (`matchPortName`) and the default recording folders.
  2. Cancel while `prepare_session` is still blocking.
  3. EmotiBit discovery against a loopback fake device.
  4. Synthetic streaming, ECG filters, heart rate through the worker (a third session with `testPpgBpm`), recording and the drop counter.
  5. Shutdown.
  6. Reconnect.
  7. Test hooks.
- The optional seconds argument (minimum 2, default 6) only sets the length of stage 4.
- New checks are `check (cond, "description")` calls in `src/tools/Headless.cpp`, inside `unitChecks`, `discoveryChecks` or `runSelftest`.

To check the UI visually without hardware or a window, render one UI state through the real code paths:

```bash
QT_QPA_PLATFORM=offscreen scripts/run.sh --screenshot /path/out.png --state live --size 1280x800
```

- States: `live`, `idle`, `connecting`, `error`, `recording`, `warning`. Every state but `idle` goes through `MainWindow::connectAllWith` (the Connect button) and is captured at t = 6.3 s. `live` / `recording` enable the `--test-ppg-bpm 72` hook so the heart-rate panel has a pulse (the first value appears ≈ 5.5 s after the stream starts, because the detector lags ≈ 1.2 s and needs 4 beats: a detector change that delays it pushes the capture to `ACQUIRING`); `connecting` holds the synthetic Cyton 8 s before `prepare_session` (`LaunchOptions::testCytonPrepareDelayMs`) so both devices are still connecting. Open the PNG with Read afterwards.
- The command also prints process CPU over t = 2–4 s. `live` costs roughly 10–20 % of one core at 1280×800 and 1600×1000 (it varies run to run), so a jump well past that points to a rendering regression.
- The `propagateSizeHints()` warning under offscreen is harmless.

The interactive GUI and `run.sh --probe [s]` (add `--no-cyton` / `--no-emotibit` to probe one device) use the real devices: the Cyton dongle, auto-detected (on this Mac `/dev/cu.usbserial-DP04W4GA`; `--list-ports` shows it without opening it), and the EmotiBit at `192.168.1.12`, reached by unicast. Keep in mind:
- Only one program can own each device, so close the OpenBCI GUI and EmotiBit Oscilloscope first.
- Broadcast discovery fails when the computer and the EmotiBit land on different subnets of the same WiFi, which happens in this lab.
- For anything that doesn't need the hardware, use `--synthetic`, `--selftest` or `--screenshot`.

## Architecture

**Data path.** `DeviceWorker` runs one `std::thread` per device. It polls BrainFlow `get_board_data (count, preset)` every 10 ms (Cyton) or 15 ms (EmotiBit), processes each signal, and appends to a `SignalRing`. The ring is fixed-capacity and mutex-guarded, and the worker shares it with the GUI through a `shared_ptr`. In `MainWindow`, three timers drive the display:
- A 16 ms timer calls `PlotWidget::tick (now)` on all seven panels. Each panel is one or more lanes (the IMU has three: ACC / GYR / MAG); each lane copies its window out of its ring and rebuilds min/max-decimated polylines per trace (`Decimate.h`).
- A 250 ms timer updates the rate meters, stream health (stalled / held), the rail headroom (raw ECG over the last 2 s) and the heart-rate readout. The rules live in `Readouts.h`. Several channels can share one panel, so `updateStreamHealth` aggregates per panel (worst state wins); derived channels (the heart rate) are left to `updateHeartRate`.
- A 1 s timer handles the slow updates, such as recording file sizes.

**Threading invariants.**
- The GUI thread never calls a blocking BrainFlow function. `prepare_session`, `start_stream`, `add_streamer` / `delete_streamer`, polling, `stop_stream` and `release_session` all run on the worker. `BoardShim`'s static descriptor getters don't take BrainFlow's lock, so they're safe on the GUI thread (`resolveSignals`).
- `DeviceWorker` is a `QObject` that lives on the GUI thread (no `moveToThread`), but it emits its signals from the worker thread. `MainWindow::wireWorker` passes `this` as the context object, so AutoConnection queues those signals onto the GUI thread. New connections need a GUI-thread context too, and the worker must never touch widgets.
- The GUI reaches the worker through atomics (stop, filter toggles, a generation counter that resets filter state) and through mutex-guarded requests such as `requestRecording`, which the poll loop applies within one interval.
- BrainFlow puts every session call of every board behind one process-wide mutex. While one device is inside `prepare_session`, the other can't be polled; the UI calls this "HELD". To keep that window short, `EmotiBitDiscovery` reimplements BrainFlow's UDP handshake (a byte-identical HELLO_EMOTIBIT to port 3131, answered by HELLO_HOST). It runs before `prepare_session`, holds no lock and can be cancelled. `--bf-discovery` switches back to BrainFlow's own discovery.
- BrainFlow calls can't be interrupted, so a stop only takes effect once the current call returns. `DeviceWorker::start` computes `worstCaseStopSeconds` from the config. The watchdog in `MainWindow::closeEvent` calls `std::_Exit (1)` once that budget runs out, so any new blocking step in the worker needs budget there.

**Signal resolution.** BrainFlow row numbers are never hard-coded. `signalDefsFor` in `SignalSpec.cpp` lists ordered candidates (channel kind, preset, index), and `resolveSignals` takes the first one that exists on the board descriptor. That lets the same pipeline run on `SYNTHETIC_BOARD`, which substitutes some channels; for example, its auxiliary accelerometer stands in for the magnetometer and it has only two PPG rows. On a real board, a channel resolved through a fallback gets a `SUBST` tag. EmotiBit PPG order is `ppg_channels = [IR, red, green]` (BrainFlow's `emotibit.cpp`; asserted by the selftest). A `SignalDef` with `derivedFrom` (the heart rate, `emotibit.hr`) has no rows: it resolves when one of its inputs does, the worker computes it (`HeartRate::Tracker` on the PPG channels flagged `heartRateInput`) and its ring holds `HeartRateRing` channels. Plots are looked up by `SignalKeys` strings (`plotForKey` also returns the IMU lane).

**Display vs. recording.** Only the display ring is changed by:
- the ECG Biquad high-pass 0.5 Hz, notch 60 Hz and low-pass 40 Hz (filterable signals only)
- DC removal
- EmotiBit packet re-timing (`Retimer.h`)
- the EmotiBit leading-zero skip
- the heart rate (a derived ring; never recorded)
- the three `--test-*` hooks (stall, rail offset, PPG pulse)

Filterable signals also keep their unfiltered value in an extra ring channel, which the rail-headroom readout uses.

Recordings are BrainFlow file streamers added to the running session:
- They keep BrainFlow's raw rows and timestamps.
- They're tab-separated with no header, despite the `.csv` name, and each file comes with a `*_columns.json` sidecar.
- `DeviceWorker.cpp` falls back to `~/bioacq_recordings` (`fallbackRecordDir ()`) when the folder can't be created, is longer than BrainFlow's 512-byte streamer path, or contains `:` other than a Windows drive letter's.

**Defined in more than one place (keep in sync).**
- `DeviceConfig` is built in `MainWindow::connectDeviceWith` (GUI) and in `makeConfig` in `Headless.cpp` (selftest and probe). Change both, or the selftest stops testing what the GUI actually does.
- The Cyton port, EmotiBit IP and discovery-timeout defaults are in `main.cpp` (`Args` and the `usage ()` text), in `LaunchOptions` in `MainWindow.h`, and in `ProbeOptions` in `Headless.h`. The Cyton port default is empty, meaning auto-detect: `MainWindow::cytonPortFor` for the GUI (at every connect, and at 1 Hz for the idle detail line) and `findCytonDongle ()` in `runProbe`, both through `SerialPorts.h`.
- The default recording folder is `defaultRecordDir ()` (`AppPaths.h`) in `main.cpp` `Args` (and so `LaunchOptions` / `ProbeOptions`), the `usage ()` text and the `MainWindow` constructor's fallback; the fallback folder is `fallbackRecordDir ()` in `DeviceWorker.cpp`.
- Readout thresholds and formatting live in `Readouts.h` (header-only, no widgets), including the 2 s rail window, the saturation wording and the heart-rate readout. `unitChecks` in `Headless.cpp` asserts them, and the README's "Using the GUI" section restates them.
- The heart-rate constants and algorithm (`HeartRate.h`) are restated in the README's "Heart rate" section, including the simulation numbers quoted there (wander, artefact gap, noise false positives, first value, dropout recovery); re-measure them when the detector changes. The `HeartRateRing` channel layout (`SignalSpec.h`) is written by `DeviceWorker::pollLoop` and read by `MainWindow::updateHeartRate` and the selftest.
- The ECG filter corners (0.5 / 60 / 40 Hz) are `kEcg*` in `MainWindow.cpp`; the hero chips, the rail toggle labels, stage 4 of the selftest and the README repeat them. The QSettings keys for them are `ecg/*` (renamed when the defaults and the high-pass corner changed); rename again if their meaning changes.
- Cyton port choice: `cytonPortFor` (connect, idle detail line) and `refreshPorts` (the drop-down: USB serial ports, dongles first) share `pickCytonDongle` in `MainWindow.cpp`, so Auto and the list agree. Auto means the last port that connected (`cyton/port`) if it is a detected dongle, else the first dongle; an override (`cyton/portOverride`, or `--port`) is used if it exists, in the OS spelling (`existingSerialPort ()`: `com12` → `COM12` on Windows, where BrainFlow adds the `\\.\` prefix COM10+ need only after an upper-case `COM`). `DeviceWorker::run` applies the same to every serial port it opens, so `--probe --port` gets it too.

**Rendering performance.** `PlotWidget` (kinds `Hero`, `Scalar`, `Lanes`, `Vital`; every panel is one or more lanes, and the rules below hold per lane and trace) depends on four performance choices:
- Cosmetic 1-device-px pens, which stay on Qt's fast line path. Wider pens go through the stroker, measured at about 100 % of a core against about 20 %.
- A cached two-layer recess.
- Header repaints capped at 8 Hz.
- At most 2 points per pixel column, with bins anchored in absolute time.

Re-measure with `--screenshot` before trading any of these for looks.

## Platforms (macOS, Windows)

- Keep platform `#ifdef`s in `src/platform/`. The few elsewhere are deliberate and small: the broadcast-address lookup in `EmotiBitDiscovery.cpp`, the drive-letter `:` check and `networkPermissionHint ()` in `DeviceWorker.cpp`, and the selftest's port-name checks. Plain UDP code uses `netsock::` from `Sockets.h` (POSIX sockets / Winsock with `WSAStartup`, `closesocket`, `ioctlsocket`, `WSAPoll`); `Readouts.h` keeps calling `getrusage`, which Windows gets from `compat/win32/sys/resource.h` (on the include path of Windows builds only). `M_PI` comes from `_USE_MATH_DEFINES`, set with `NOMINMAX` and `WIN32_LEAN_AND_MEAN` for the whole target on Windows.
- MSVC builds with `/utf-8` (the sources contain UTF-8 literals) and `/permissive-`. Don't include `<windows.h>` from headers that UI code includes. Files that do see it (through `Sockets.h`: `Headless.cpp`, `EmotiBitDiscovery.cpp`) get its macros, so don't name anything `near` or `far` there (a local `near` broke the Windows build once).
- Serial ports: `listSerialPorts ()` / `findCytonDongle ()` (Qt SerialPort, FTDI 0403:6015), `existingSerialPort ()` (the port BrainFlow must get, or "") / `serialPortExists ()`, and `matchPortName ()` (the Windows matching rules on a given list; pure, so the selftest runs it everywhere). Port strings are BrainFlow's: `COM3` on Windows, `/dev/cu.*` on macOS; compare them with `sameSerialPort ()` (case-insensitive on Windows). `--list-ports` prints them. Enumeration reads the OS device registry and never opens a port, so it is safe while the lab's dongle is in use; it runs on the GUI thread at 1 Hz while the Cyton is idle, which the selftest's timing check guards.
- OS-specific user-facing text (Local Network permission on macOS, the firewall on Windows) goes through `networkPermissionHint ()` in `DeviceWorker.cpp`. Shortcut hints use `QKeySequence::NativeText` (`⌘K` / `Ctrl+K`), never literal glyphs.
- `BioAcq.exe` is a GUI-subsystem program (`WIN32_EXECUTABLE`): `attachParentConsole ()` in `main ()` makes the CLI modes print to the calling console. cmd / PowerShell don't wait for it; use `scripts\run_cli_windows.ps1` (or `start /wait`) for exit codes. `raiseTimerResolution ()` sets 1 ms timers (otherwise the 10 / 15 ms polls stretch to 15.6 ms).
- On Windows, BrainFlow must be built with `-DMSVC_RUNTIME=dynamic` (`build_brainflow.ps1` does), or linking its static C++ binding next to Qt fails on the runtime mismatch.
- CI: `windows.yml` (pushes to `main`, `windows`, `feat/**`, and manual) builds BrainFlow (cached), runs `package_windows.ps1`, then `--selftest`, `--screenshot --state live --size 1600x1000` (offscreen) and `--screenshot --state idle --size 1280x800` through the native `windows` platform plugin, all on the packaged folder with Qt removed from `PATH`. Check a run with `gh run list --workflow windows.yml` / `gh run view <id> --log-failed`; artifacts `BioAcq-windows-x64` and `bioacq-windows-screenshot`.

## Local BrainFlow patch

`~/.local/brainflow` is built from `~/.local/src/brainflow` (v5.23.0) with one fix in `src/board_controller/emotibit/emotibit.cpp`, around line 376 (diff and build steps in `third_party/brainflow/`). The ancillary 2× upsampling writes `anc_packages[i * 2 + 1]`, and the change is marked `patched (stream_gui_cpp)`. Re-apply it after any BrainFlow rebuild or update, or the EmotiBit temperature shows stale duplicate values. To check that it's in place:

```bash
grep -n "patched (stream_gui_cpp)" ~/.local/src/brainflow/src/board_controller/emotibit/emotibit.cpp
```

`scripts/build_brainflow.sh [prefix]` / `scripts\build_brainflow.ps1 -Prefix <dir>` clone 5.23.0 into a separate checkout (`~/.local/src/brainflow-5.23.0`), apply the patch and install; they don't touch `~/.local/src/brainflow`. Don't run them with the default prefix unless you mean to replace `~/.local/brainflow`.

## Code style

There's no formatter config and clang-format isn't installed, so match the existing code by hand:
- Allman braces, 4-space indent.
- A space before every parenthesis: `f (x)`, `if (x)`, `QStringLiteral ("…")`.
- No indentation inside namespaces.
- `T *p` and `const T &r`.
- Member names end in an underscore.
- Lines up to about 120 columns.
