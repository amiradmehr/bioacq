# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Layout

This folder is the `bioacq` repository (GitHub `amiradmehr/bioacq`, private). `bioacq` is a C++17 / Qt6 Widgets real-time viewer that streams two devices through the BrainFlow C++ API: an OpenBCI Cyton (serial dongle, EXG channel 1) and an EmotiBit (WiFi: temperature, PPG green, accel/gyro/mag). `README.md` specifies the UI behaviour, modes, networking and known limitations in detail. Read the relevant section before changing behaviour, and update it when behaviour changes.

- `CMakeLists.txt`: the build (target `bioacq`).
- `src/app/`: `main.cpp`, `BuildConfig.h.in`. `src/core/`: `RingBuffer.h`, `RateMeter.h`, `Readouts.h`, `Decimate.h`. `src/dsp/`: `Biquad.h`. `src/devices/`: `DeviceWorker`, `EmotiBitDiscovery`, `SignalSpec`, `Retimer.h`. `src/ui/`: `MainWindow`, `PlotWidget`, `Widgets`, `Theme`. `src/tools/`: `Headless` (selftest, probe). Every `src/` subfolder is an include directory, so includes are plain `#include "X.h"`.
- `resources/fonts/`: the embedded TTFs and OFL texts (served at `:/fonts/...`).
- `scripts/`: `build.sh`, `run.sh`.
- `third_party/brainflow/`: the local BrainFlow patch and its build instructions.
- `prototype/stream_gui.py` is the original PySide6 + pyqtgraph prototype, now superseded. The user rejected Python for real-time streaming UIs as too slow, so new work goes into the C++/Qt code.
- `design/Biosignal streaming GUI system.zip` is the Claude Design export that serves as the visual spec (`Instrument Screen.dc.html`, `Component Sheet.dc.html`, `Biosignal GUI.dc.html`). The `src/ui/Theme.h` tokens and the pixel sizes in `src/ui/Widgets.cpp` / `src/ui/PlotWidget.cpp` mirror it. Read it without extracting: `unzip -p "design/Biosignal streaming GUI system.zip" "Instrument Screen.dc.html"`. Its `_ds/` folder is the author's website design system and only supplies the font families. The instrument colours are inline hex in the `.dc.html` files.
- Branches: `main` holds the source; `mac` / `windows` hold double-clickable builds.
- The folder lives in Google Drive under a path with spaces, and one folder name starts with a space (`" demo Columbia"`), so quote every path. The git database is kept outside Drive at `~/.local/git/bioacq.git` (the `.git` here is a gitdir file pointing to it).

## Build and run

The scripts find their own location, so they work from any directory:

```bash
scripts/build.sh           # configure on first run, then incremental Release build (Ninja)
scripts/build.sh --clean   # wipe the build tree and reconfigure
scripts/run.sh [args]      # build if needed, then exec the binary with args
```

- The build tree is kept outside Google Drive on purpose, at `~/.local/build/bioacq` (override with `BIOACQ_BUILD_DIR`; `STREAM_GUI_BUILD_DIR` is still accepted as a fallback). The binary and `compile_commands.json` are there. clangd won't find `compile_commands.json` on its own.
- `build.sh` only runs the CMake configure step when there's no cache, so `BRAINFLOW_ROOT` / `QT_PREFIX` overrides need `--clean`. The default recording folder is `BIOACQ_RECORD_DIR`: `recordings/` at the repo root, inside Drive and gitignored. It's a CMake cache variable baked in through `src/app/BuildConfig.h.in`, and `--record-dir` overrides it at runtime.
- Dependencies: Homebrew Qt 6 (`/opt/homebrew/opt/qt`), CMake ≥ 3.21, Ninja, and BrainFlow 5.23.0 installed in `~/.local/brainflow` (patched, see below).
- `CMakeLists.txt` lists every source and header explicitly in `add_executable`, so new files must be added there. AUTOMOC is on, and the fonts are compiled in with `qt_add_resources`.

## Testing

There is no unit-test framework. A single headless end-to-end suite runs the GUI's own worker and ring code on BrainFlow's synthetic board:

```bash
scripts/run.sh --selftest                               # ~15 s, exit 0 on PASS
scripts/run.sh --selftest 2>&1 | grep -E "FAIL|RESULT"   # only failures + the summary
```

- Output: each check prints `[ OK ]` or `[FAIL]`, and the run ends with `RESULT: PASS (n/n checks passed)`. A single check can't be run on its own.
- Stages:
  1. Unit checks: ring, decimation, retimer, biquads, board-row mapping, `Readouts` rules, CPU meter.
  2. Cancel while `prepare_session` is still blocking.
  3. EmotiBit discovery against a loopback fake device.
  4. Synthetic streaming, filters, recording and the drop counter.
  5. Shutdown.
  6. Reconnect.
  7. Test hooks.
- The optional seconds argument (minimum 2, default 6) only sets the length of stage 4.
- New checks are `check (cond, "description")` calls in `src/tools/Headless.cpp`, inside `unitChecks`, `discoveryChecks` or `runSelftest`.

To check the UI visually without hardware or a window, render one UI state through the real code paths:

```bash
QT_QPA_PLATFORM=offscreen scripts/run.sh --screenshot /path/out.png --state live --size 1280x800
```

- States: `live`, `idle`, `connecting`, `error`, `recording`, `warning`. Open the PNG with Read afterwards.
- The command also prints process CPU over t = 2–4 s. `live` at 1280×800 costs about 15–20 % of one core, so a jump points to a rendering regression.
- The `propagateSizeHints()` warning under offscreen is harmless.

The interactive GUI and `run.sh --probe [s]` (add `--no-cyton` / `--no-emotibit` to probe one device) use the real devices: the Cyton dongle at `/dev/cu.usbserial-DP04W4GA` and the EmotiBit at `192.168.1.12`, reached by unicast. Keep in mind:
- Only one program can own each device, so close the OpenBCI GUI and EmotiBit Oscilloscope first.
- Broadcast discovery fails when the Mac and the EmotiBit land on different subnets of the same WiFi, which happens in this lab.
- For anything that doesn't need the hardware, use `--synthetic`, `--selftest` or `--screenshot`.

## Architecture

**Data path.** `DeviceWorker` runs one `std::thread` per device. It polls BrainFlow `get_board_data (count, preset)` every 10 ms (Cyton) or 15 ms (EmotiBit), processes each signal, and appends to a `SignalRing`. The ring is fixed-capacity and mutex-guarded, and the worker shares it with the GUI through a `shared_ptr`. In `MainWindow`, three timers drive the display:
- A 16 ms timer calls `PlotWidget::tick (now)` on all six plots. Each plot copies its window out of the ring and rebuilds min/max-decimated polylines (`Decimate.h`).
- A 250 ms timer updates the rate meters and stream health (stalled / held). The rules live in `Readouts.h`.
- A 1 s timer handles the slow updates, such as recording file sizes.

**Threading invariants.**
- The GUI thread never calls a blocking BrainFlow function. `prepare_session`, `start_stream`, `add_streamer` / `delete_streamer`, polling, `stop_stream` and `release_session` all run on the worker. `BoardShim`'s static descriptor getters don't take BrainFlow's lock, so they're safe on the GUI thread (`resolveSignals`).
- `DeviceWorker` is a `QObject` that lives on the GUI thread (no `moveToThread`), but it emits its signals from the worker thread. `MainWindow::wireWorker` passes `this` as the context object, so AutoConnection queues those signals onto the GUI thread. New connections need a GUI-thread context too, and the worker must never touch widgets.
- The GUI reaches the worker through atomics (stop, filter toggles, a generation counter that resets filter state) and through mutex-guarded requests such as `requestRecording`, which the poll loop applies within one interval.
- BrainFlow puts every session call of every board behind one process-wide mutex. While one device is inside `prepare_session`, the other can't be polled; the UI calls this "HELD". To keep that window short, `EmotiBitDiscovery` reimplements BrainFlow's UDP handshake (a byte-identical HELLO_EMOTIBIT to port 3131, answered by HELLO_HOST). It runs before `prepare_session`, holds no lock and can be cancelled. `--bf-discovery` switches back to BrainFlow's own discovery.
- BrainFlow calls can't be interrupted, so a stop only takes effect once the current call returns. `DeviceWorker::start` computes `worstCaseStopSeconds` from the config. The watchdog in `MainWindow::closeEvent` calls `std::_Exit (1)` once that budget runs out, so any new blocking step in the worker needs budget there.

**Signal resolution.** BrainFlow row numbers are never hard-coded. `signalDefsFor` in `SignalSpec.cpp` lists ordered candidates (channel kind, preset, index), and `resolveSignals` takes the first one that exists on the board descriptor. That lets the same pipeline run on `SYNTHETIC_BOARD`, which substitutes some channels; for example, its auxiliary accelerometer stands in for the magnetometer. On a real board, a channel resolved through a fallback gets a `SUBST` tag. Plots are looked up by `SignalKeys` strings.

**Display vs. recording.** Only the display ring is changed by:
- the Biquad high-pass and notch (filterable signals only)
- DC removal
- EmotiBit packet re-timing (`Retimer.h`)
- the EmotiBit leading-zero skip
- both `--test-*` hooks

Filterable signals also keep their unfiltered value in an extra ring channel, which the rail-headroom readout uses.

Recordings are BrainFlow file streamers added to the running session:
- They keep BrainFlow's raw rows and timestamps.
- They're tab-separated with no header, despite the `.csv` name, and each file comes with a `*_columns.json` sidecar.
- `DeviceWorker.cpp` falls back to `~/bioacq_recordings` when the folder can't be created, is longer than BrainFlow's 512-byte streamer path, or contains `:`.

**Defined in more than one place (keep in sync).**
- `DeviceConfig` is built in `MainWindow::connectDeviceWith` (GUI) and in `makeConfig` in `Headless.cpp` (selftest and probe). Change both, or the selftest stops testing what the GUI actually does.
- The Cyton port, EmotiBit IP and discovery-timeout defaults are in `main.cpp` (`Args` and the `usage ()` text), in `LaunchOptions` in `MainWindow.h`, and in `ProbeOptions` in `Headless.h`.
- Readout thresholds and formatting live in `Readouts.h` (header-only, no widgets). `unitChecks` in `Headless.cpp` asserts them, and the README's "Using the GUI" section restates them.

**Rendering performance.** `PlotWidget` depends on four performance choices:
- Cosmetic 1-device-px pens, which stay on Qt's fast line path. Wider pens go through the stroker, measured at about 100 % of a core against about 20 %.
- A cached two-layer recess.
- Header repaints capped at 8 Hz.
- At most 2 points per pixel column, with bins anchored in absolute time.

Re-measure with `--screenshot` before trading any of these for looks.

## Local BrainFlow patch

`~/.local/brainflow` is built from `~/.local/src/brainflow` (v5.23.0) with one fix in `src/board_controller/emotibit/emotibit.cpp`, around line 376 (diff and build steps in `third_party/brainflow/`). The ancillary 2× upsampling writes `anc_packages[i * 2 + 1]`, and the change is marked `patched (stream_gui_cpp)`. Re-apply it after any BrainFlow rebuild or update, or the EmotiBit temperature shows stale duplicate values. To check that it's in place:

```bash
grep -n "patched (stream_gui_cpp)" ~/.local/src/brainflow/src/board_controller/emotibit/emotibit.cpp
```

## Code style

There's no formatter config and clang-format isn't installed, so match the existing code by hand:
- Allman braces, 4-space indent.
- A space before every parenthesis: `f (x)`, `if (x)`, `QStringLiteral ("…")`.
- No indentation inside namespaces.
- `T *p` and `const T &r`.
- Member names end in an underscore.
- Lines up to about 120 columns.
