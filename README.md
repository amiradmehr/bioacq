# bioacq — real-time Cyton ECG + EmotiBit viewer

`bioacq` is a native C++17 / Qt 6 Widgets application that streams an OpenBCI Cyton (USB serial dongle) and an EmotiBit (WiFi) through the BrainFlow C++ API and plots the signals live, with filtering, stream-health readouts and CSV recording.

## Repository layout

| Path | Contents |
|---|---|
| `CMakeLists.txt` | Build definition (target `bioacq`) |
| `src/app/` | `main.cpp` (command line, modes), `BuildConfig.h.in` |
| `src/core/` | Header-only building blocks: ring buffer, rate meter, readout rules, decimation |
| `src/dsp/` | Biquad filters |
| `src/devices/` | `DeviceWorker` (BrainFlow thread per device), EmotiBit discovery, signal resolution, packet re-timing |
| `src/ui/` | `MainWindow`, `PlotWidget`, widgets, theme |
| `src/tools/` | `Headless` (`--selftest`, `--probe`) |
| `src/platform/` | Portability layer: UDP sockets (POSIX / Winsock), serial port listing and OpenBCI dongle detection, default folders, Windows console attach; `compat/win32/` holds a `<sys/resource.h>` stand-in |
| `resources/fonts/` | Embedded JetBrains Mono + Inter (SIL OFL 1.1, licence texts included) |
| `resources/macos/`, `resources/windows/` | `Info.plist` template and icon for `BioAcq.app`; version resource and icon for `BioAcq.exe` |
| `scripts/` | `build.sh`, `run.sh` (development build); `build_brainflow.sh` / `.ps1`; `package_macos.sh`, `package_windows.ps1`; `run_cli_windows.ps1` |
| `.github/workflows/` | `windows.yml` (every push: build, selftest, package), `macos.yml` (manual) |
| `third_party/brainflow/` | The local BrainFlow 5.23.0 patch, BrainFlow's licence, and how to build BrainFlow with it |
| `design/` | Claude Design export used as the visual spec |
| `prototype/` | The superseded PySide6 prototype (`stream_gui.py`) |

## Branches

* `main`: source code.
* `mac` / `windows`: double-clickable builds for each platform.

## Signals

| Plot | Source | Units |
|---|---|---|
| OpenBCI Cyton EXG channel 1 (SRB / AGND / N1P, single-ended) | Cyton, 250 Hz, `get_exg_channels()[0]` | µV |
| EmotiBit temperature (T1) | ANCILLARY preset | °C |
| EmotiBit PPG **green** | AUXILIARY preset, `get_ppg_channels()[2]` | a.u. |
| EmotiBit accelerometer / gyroscope / magnetometer X,Y,Z | DEFAULT preset | g, °/s, µT |

All rows are resolved at runtime from BrainFlow's board descriptors, never hard-coded.

## Build & run

```bash
scripts/build.sh          # configure (first time) + incremental Release build
scripts/build.sh --clean  # wipe the build tree and reconfigure
scripts/run.sh            # build if needed, then launch the GUI (extra args are passed through)
```

* The scripts locate the repository root from their own location, so they work from any directory.
* The build tree is `~/.local/build/bioacq`, outside Google Drive. Override it with `BIOACQ_BUILD_DIR` (`STREAM_GUI_BUILD_DIR` is still accepted as a fallback alias).
* Binary: `~/.local/build/bioacq/bioacq`. It's a plain executable, not a `.app`, and not on your `PATH`, so use `scripts/run.sh …` or the full binary path. Its rpath points at `~/.local/brainflow/lib` and Homebrew Qt, so it runs without `DYLD_*` variables.
* Requirements: Homebrew Qt 6 (`/opt/homebrew/opt/qt`, modules Core/Gui/Widgets/Network), CMake ≥ 3.21, Ninja, and BrainFlow 5.23.0 installed to `~/.local/brainflow` with the patch in `third_party/brainflow/` (see *Local BrainFlow patch* below). You can override these with `BRAINFLOW_ROOT` or `QT_PREFIX` (together with `--clean`).
* Fonts: JetBrains Mono and Inter (`resources/fonts/`, SIL Open Font License 1.1, licence texts next to them) are compiled into the binary as Qt resources and registered at startup. If registration fails the UI falls back to Menlo / the system font (printed on stderr).

## Packages: BioAcq.app and BioAcq.exe

The development build above is a plain `bioacq` binary tied to this machine's Qt and BrainFlow. The packaged builds are self-contained and start with a double-click. They are built with `-DBIOACQ_PACKAGED=ON`, which also changes the default recording folder to `Documents/BioAcq Recordings` (development builds record into `recordings/` in the repository).

**macOS** (`scripts/package_macos.sh`, Apple Silicon):

```bash
scripts/build_brainflow.sh ~/.local/brainflow        # once: clone 5.23.0, patch, build, install
scripts/package_macos.sh                             # -> dist/macos/BioAcq.app, dist/BioAcq-macos-arm64.zip
```

* The script builds `BioAcq.app`, deploys Qt with `macdeployqt` (plus the offscreen platform plugin, so `BioAcq.app/Contents/MacOS/BioAcq --selftest` and `--screenshot` work from inside the bundle), copies BrainFlow's `libBoardController` / `libDataHandler` / `libMLModule` into `Contents/Frameworks`, rewrites every install name and rpath to point inside the bundle, sets `LSMinimumSystemVersion` to the highest minimum macOS of the bundled binaries, and signs ad hoc.
* It then verifies the result and stops on the first failure: `codesign --verify --deep --strict`; no `/opt/homebrew`, `/usr/local` or `/Users` path in any load command or rpath; `--selftest` and `--screenshot` run from the bundle with every image dyld loads inside the bundle or the OS; the embedded fonts register; and `open BioAcq.app` stays up for 5 s without opening the dongle or an EmotiBit socket, then quits.
* Environment: `QT_PREFIX` (default `$QT_ROOT_DIR`, else Homebrew), `BRAINFLOW_ROOT`, `BIOACQ_BUILD_DIR` (default `~/.local/build/bioacq-package-macos`), `BIOACQ_SKIP_OPEN_TEST=1`. With Homebrew Qt the bundle needs the macOS version Homebrew's bottles were built for (currently 26); build BrainFlow with `MACOSX_DEPLOYMENT_TARGET=13.0` and use the official Qt (as `macos.yml` does) for macOS 13+.
* The app is signed ad hoc, not notarised. A copy downloaded from GitHub is quarantined by Gatekeeper: right-click → *Open* the first time, or run `xattr -dr com.apple.quarantine BioAcq.app`.
* `Info.plist` carries `NSLocalNetworkUsageDescription`. macOS 15+ asks once for Local Network access on the first EmotiBit connect; without that permission the discovery packet cannot be sent.

**Windows 10 / 11 x64** (`scripts\package_windows.ps1`), from a *x64 Native Tools* / Developer PowerShell for VS 2022, with CMake, Ninja and Qt 6.10 for `msvc2022_64` including Qt Serial Port:

```powershell
scripts\build_brainflow.ps1                                      # once: -> %USERPROFILE%\brainflow
scripts\package_windows.ps1 -QtPrefix C:\Qt\6.10.2\msvc2022_64    # -> dist\windows\BioAcq\BioAcq.exe, dist\BioAcq-windows-x64.zip
```

* The folder contains `BioAcq.exe`, the Qt DLLs and plugins from `windeployqt` (plus `platforms\qoffscreen.dll`), BrainFlow's `BoardController.dll` / `DataHandler.dll` / `MLModule.dll`, and the MSVC runtime DLLs, so nothing needs to be installed. Unzip it anywhere and double-click `BioAcq.exe`.
* BrainFlow is built with the dynamic MSVC runtime (`-DMSVC_RUNTIME=dynamic`), because its static C++ binding is linked into the same executable as Qt.
* `BioAcq.exe` uses the GUI subsystem, so a double-click opens no console window. Started with arguments from cmd or PowerShell, it attaches to that console and prints there. The shell does not wait for a GUI program, though, so for exit codes use `start /wait BioAcq.exe --selftest` in cmd, or `scripts\run_cli_windows.ps1 -Exe <path>\BioAcq.exe -Arguments '--selftest'`, which waits, prints stdout/stderr and returns the exit code.
* Windows Firewall may ask to allow `BioAcq.exe` on private networks the first time the EmotiBit is contacted.

**Cyton port.** Without `--port`, the app looks for the OpenBCI dongle (FTDI FT231X, USB VID `0403` / PID `6015`, or a port description naming FT231X / OpenBCI) and preselects it: `COM3`-style names on Windows, `/dev/cu.usbserial-…` on macOS (the `tty.` twin is skipped). `--list-ports` prints every serial port with its VID:PID, dongles first. Listing never opens a port.

**CI.** `.github/workflows/windows.yml` runs on every push to `main`, `windows` and `feat/**` (and manually): MSVC 2022 + Qt 6.10 via `install-qt-action`, the patched BrainFlow (cached on the patch and script hash), `package_windows.ps1`, then `--selftest` and a `--screenshot --state live --size 1600x1000` run on the packaged folder with Qt taken off `PATH`. Artifacts: `BioAcq-windows-x64` (the zip) and `bioacq-windows-screenshot`. `.github/workflows/macos.yml` runs `package_macos.sh` on `macos-14` with the official Qt and a BrainFlow built for macOS 13; it is manual only (`gh workflow run macos.yml --ref <branch>`), because macOS minutes cost 10× on a private repository.

## Modes

Run these from the repository root (`scripts/run.sh` builds if needed and passes the arguments through):

| Command | What it does |
|---|---|
| `scripts/run.sh` | Interactive GUI |
| `scripts/run.sh --synthetic` | GUI with both devices simulated by BrainFlow's synthetic board |
| `scripts/run.sh --selftest [s]` | Headless end-to-end test on the synthetic board through the same worker/ring code. Covers filters, decimation, EmotiBit packet re-timing, the EmotiBit discovery (against a local fake device), cancel while `prepare_session` is still blocking, recording started/stopped on a running session, the readout rules (rail headroom, rate colour, stall detection, dropped packets, number formatting, CPU meter), the test hooks, clean shutdown and reconnect. Exits 0 on PASS |
| `scripts/run.sh --probe [s]` | Headless check of the **real** devices. Prints per-signal sample counts, rates and latest values. Won't crash if a device is absent. Add `--no-cyton` / `--no-emotibit` to probe one device only |
| `QT_QPA_PLATFORM=offscreen scripts/run.sh --screenshot out.png [--state S] [--size WxH]` | Renders one UI state through the real code paths, saves a PNG and quits (also prints the process CPU use over t = 2–4 s) |

Screenshot states (`--state`, default `live`): `live` (both slots synthetic), `idle` (nothing connected), `connecting` (Cyton synthetic + EmotiBit discovery toward the unanswered TEST-NET address 192.0.2.1, 20 s timeout, captured mid-discovery), `error` (same with a 2 s timeout, captured after the failure), `recording` (both synthetic, the record button pressed after 1.5 s; files go to `--record-dir`, or to a temporary folder that is deleted afterwards), `warning` (both synthetic with the two test hooks below).

Options: `--port <port>` (`COM3`, `/dev/cu.usbserial-XXXX`; default: the detected OpenBCI dongle), `--list-ports`, `--ip <emotibit ip>` (default `192.168.1.12`), `--discover` (blank IP: broadcast discovery), `--timeout <s>` (2–20), `--record` (arm recording), `--record-dir <dir>`, `--window <s>` (1–60), `--bf-discovery` (see below), `--verbose` (BrainFlow INFO log).

Test hooks (command line only, never in the UI; a warning is printed on stderr): `--test-stall-emotibit [s]` stops polling the EmotiBit after *s* seconds of streaming (default 1.5; the session stays open, so the stalled path renders; `0` freezes before the first sample, which renders the *no samples since connect* path), `--test-rail-offset-uv <µV>` adds a DC offset to the Cyton's raw Ch1 **display** value (the near-rail path). Recordings are not affected by either.

## Using the GUI

The window is an instrument panel: a 30 px session bar (session label = start time + streaming devices, REC chip while recording, local clock), a 264 px control rail on the left, the plots, and a 26 px status bar (device states, aggregate rate, window, recording state, the current message, Cyton dropped packets, process CPU). A banner above the plots reports a stalled stream / near-rail input (amber), a failed connection (red) or a running recording (red outline). With nothing connected, the hero plot shows a call to action: both connect buttons, the shortcut hints and the *simulate devices* switch. Every value on screen comes from real state; what can't be measured is omitted or shown as `—`.

* **Window size.** The layout fits 1280 × 800 without hiding plots. The rail scrolls internally when its modules are taller than the window (e.g. with an error module open). Like the design's 1280 × 800 artboard, an overflow of at most the last module's bottom padding is clipped rather than adding a scroll bar.
* **Before connecting, close the OpenBCI GUI and EmotiBit Oscilloscope.** Only one program can own the Cyton dongle, and only one host can own the EmotiBit stream.
* **Cyton.** Choose the port (default `/dev/cu.usbserial-DP04W4GA`; the rescan button or **⌘R** rescans `/dev/cu.usbserial-*`) and press `[ connect cyton ]`. BrainFlow soft-resets the board, which takes a few seconds; the button reads `[ cancel ]` meanwhile. While a device is in use, its port / IP / timeout fields are locked (read-only; the port list and rescan are inert) but drawn at rest.
  * *Remove DC offset*: display only, subtracts the mean of the visible window.
  * *High-pass 1 Hz* and *Notch 60 Hz*: RBJ biquads run sample-by-sample in the worker thread. Filter state resets whenever you toggle them. The hero plot header shows the three filter states as chips.
  * *Rail headroom* = 1 − max|raw Ch1| / 187,500 µV over the last window-length of samples, read straight from the Cyton's ring buffer on the raw value before filters. A DC offset near the rail therefore shows even with DC removal on, and the readout keeps following the live data while the display is paused. The percentage is floored to 0.1 % (0.0996 reads `9.9 %`, never `10 %`). Below 10 % it turns amber (green again above 12 %), and the hero plot switches to the raw value on a fixed ±187,500 µV range: dashed ±FS rail lines near the top and bottom, labelled `+187,500 µV` / `−187,500 µV`, with LATEST reading `LATEST RAW`. The hero's LED stays the Cyton link state.
  * *DROPPED* in the status bar counts packets missing from the Cyton's package-number sequence since connect. It appears once the Cyton has delivered packets (not while it is still connecting).
* **EmotiBit.** The IP field defaults to `192.168.1.12`. Clear it for broadcast discovery (same subnet only, see below). `[ connect emotibit ]` or **⌘⇧D** starts the search; the amber module shows elapsed / timeout, the targets (unicast address or broadcast addresses) and the probes sent, with `[ cancel discovery ]`. If nothing answers, the red module explains why and shows this Mac's actual IPv4 subnet(s).
* **Plot readouts.** Every header shows `measured / nominal Hz` over the last 2 s (amber when more than 5 % below nominal, `— / nominal` before the first sample) and the latest value(s); the hero also shows the window's peak-to-peak. A stream with no new samples for more than 1 s is *stalled*: amber values and LED, `0.0 / nominal`, and a `LAST SAMPLE x.x s AGO` overlay (plain amber text). A stream that never delivered a sample is stalled too, 1 s (Cyton) / 3 s (EmotiBit, whose worker skips leading all-zero samples for up to 3 s) after its device reached *streaming*; its overlay reads `NO SAMPLES SINCE CONNECT · x.x s`. When every stream of a device is stalled, the device chip reads `STALLED`, the amber banner shows, and the overlay appears once, on the device's last plot (MAG for the EmotiBit). A partial stall labels each stale plot and is named in the status bar. If the cause is BrainFlow's lock (the other device is inside `prepare_session`), the overlay and chip say `HELD` instead; those samples are buffered and the plot catches up. `All streams nominal` is printed only when every channel of every streaming device has data at a nominal rate.
* **Record.** `[ record to csv ]` starts recording immediately on every streaming device and `[ stop recording ]` closes the files — BrainFlow's file streamers can be added to and removed from a running session (`Board::add_streamer` / `delete_streamer` take the same board lock the data thread holds while it pushes samples, and deleting a file streamer closes the file). Pressed with nothing connected, the button *arms* the recording (amber): every device then records from its first sample when it connects, as `--record` does. Devices that connect while a recording runs join it.
  * Files go to `recordings/<device>_<preset>_<yyyyMMdd_HHmmss>.csv`, one file per BrainFlow preset: Cyton has `default`; EmotiBit has `default`, `auxiliary` and `ancillary`. A second recording started within the same second gets a `_2` suffix instead of overwriting.
  * BrainFlow writes every row of the preset tab-separated with no header, despite the `.csv` name. A `*_columns.json` next to each file maps row numbers to channels.
  * Timestamps are UNIX seconds, BrainFlow's raw values.
  * The record module shows the folder, the elapsed time and the files' actual size on disk (polled once per second).
  * Recording falls back to `~/bioacq_recordings` if the folder can't be created, is too long for BrainFlow's 512-byte limit, or contains a `:` (other than a Windows drive letter's). BrainFlow splits `file://path:mode` at the last `:`.
* **Display.** *Time window* sets the visible span, 1–60 s. *Pause display* freezes the plots only; streaming, recording and the rail-headroom / near-rail warning continue.
* **Simulate devices.** The *simulate devices* switch sits in the idle call to action, next to the shortcut hints. It can only change while nothing is connected, which is exactly when that overlay shows. It connects the slots to BrainFlow's synthetic board; a synthetic session is labelled `synthetic` in the session bar, the Cyton meta line and the hero subtitle.
* **Remembered settings.** The GUI stores the last successfully used Cyton port and EmotiBit IP, plus the discovery timeout, window length, filter toggles and the armed/recording state of the record button. Settings are saved on each successful connect and on close, and restored at start. The store is `QSettings`: `~/Library/Preferences/com.bioacq.bioacq.plist` on macOS, `HKEY_CURRENT_USER\Software\bioacq\bioacq` on Windows. Command-line `--port` / `--ip` / `--timeout` / `--window` / `--record` override the stored values. `--screenshot` neither reads nor writes them.

## Networking / EmotiBit not found

* **Same WiFi isn't the same subnet.** The Mac and the EmotiBit can join the same SSID and still land on different subnets, e.g. Mac on `172.31.x.x/16` and EmotiBit on `192.168.1.12`. Broadcast discovery (blank IP) then fails with "no EmotiBit answered". A typed IP always works, because it is reached by unicast across subnets. That is why the field defaults to `192.168.1.12`.
* **Finding the IP.** The EmotiBit prints its IP in its serial boot log (Feather USB serial monitor). EmotiBit Oscilloscope also shows it. Type it into the IP field. After one successful connect the app remembers it.
* If discovery with a blank IP fails, the error module says so, shows the Mac's subnet, and asks for the IP.
* **Close EmotiBit Oscilloscope and the OpenBCI GUI** before connecting. Only one host can own the EmotiBit stream, and only one program the Cyton dongle.
* **macOS Local Network permission.** If sending the discovery packet fails ("DISCOVERY BLOCKED"), macOS may be blocking Local Network access for the app that launched this program. Allow it under System Settings → Privacy & Security → Local Network, for Terminal, iTerm or VS Code.
* **How the connect works.** The app first finds the EmotiBit with its own UDP handshake: HELLO_EMOTIBIT to port 3131, waiting for HELLO_HOST, identical to BrainFlow's. This runs for a typed IP as well as for broadcast, can be cancelled, and does not hold BrainFlow's global lock. Only then does it call BrainFlow's `prepare_session` with the found IP. That holds the lock for about 1–7 s: a 1 s fixed sleep plus the TCP handshake. `--bf-discovery` switches back to BrainFlow's own discovery. It holds the lock for the whole search, pausing the Cyton, and is kept only as an escape hatch.

## Architecture

* `DeviceWorker`: one `std::thread` per device. It runs discovery (EmotiBit), `prepare_session`, `add_streamer` / `delete_streamer` and `start_stream`, then polls `get_board_data(preset)` every 10–15 ms. Only the plotted rows go into `SignalRing`s, which are fixed-capacity and mutex-guarded. The GUI thread never calls a blocking BrainFlow function; recording requests are applied by the worker within one poll interval.
  * EmotiBit timestamps: BrainFlow stamps every sample of a packet (up to ~9) with the same host time. For display, `PacketRetimer` spreads each packet evenly over the interval since the previous one, so PPG and IMU draw as waveforms, not combs. Recordings keep BrainFlow's raw timestamps.
  * Cancel / Disconnect / close while `prepare_session` is still blocking: the worker releases the session as soon as BrainFlow returns. The EmotiBit search itself stops within ~0.1 s.
  * Closing hides the window at once and waits for both workers. A safety watchdog forces the exit if release takes longer than the configured worst case (≈ 15 s + per-device budget). It then prints which device was still busy, flushes stdio, including BrainFlow's recording files, and exits with status 1.
* `PlotWidget`: a custom QPainter strip chart drawn as one panel section (header, optional X/Y/Z legend strip, recessed plot on a dot raster), redrawn at up to 60 Hz only when data or time has moved.
  * The static recess is cached in two layers. The dot raster and border are rasterised only when the size / DPR change; each dot is a crisp 2 device-px block in the composited colour, which is how a browser renders the design's `radial-gradient` tile. On top of one blit of that layer, the zero / rail lines and the axis and time labels are redrawn only when the snapped Y range, the window or the rail view change. Trace frames repaint only the recess, and header text is refreshed at ≤ 8 Hz and repaints only the header.
  * Per-pixel min/max decimation keeps it to ≤ 2 points per column at any sample rate. Bins are anchored in absolute time, so peaks don't shimmer while scrolling, and switching between exact and decimated drawing has hysteresis.
  * Traces use cosmetic 1 device-px pens: Qt's fast line path. Exact segments are antialiased and keep the design's dash patterns (X solid, Y dashed 7 4, Z dotted 2 4); min/max-decimated segments are solid and aliased. The design's 1.4 / 1.3 px widths would need Qt's stroker, which measured at ~100 % of one core with every segment sparse (offscreen 1600×1000, both slots synthetic, 1 s window) versus ~20 % with cosmetic pens.
  * Gaps and NaN/Inf values are drawn as gaps.
  * Y autoscale has hysteresis (expands immediately, shrinks slowly), is snapped outwards to a round step (stable axis labels), and has a minimum span of one display digit. With DC removal or the high-pass on, the hero's range is symmetric around 0.
  * Time axis runs from −window to 0 with x = ts − host now, so a stalled stream visibly scrolls away.
* `Theme` (tokens, fonts, global stylesheet), `Widgets` (toggle switch, status chip, meter bar, banner, stepper, record / rescan buttons, session and status bars) and `Readouts` (headroom, rate tone, stall rule, package-number gaps, formatting, CPU meter; header-only and covered by `--selftest`).
* `SignalSpec`: each plotted signal is described as {preset, rows, timestamp row, label, units}, resolved via the BoardShim getters. The same pipeline therefore runs on `SYNTHETIC_BOARD`. The synthetic board has no magnetometer, so that plot uses its auxiliary accelerometer instead. Each plot's tooltip names its source rows; on a real device, a channel resolved through a fallback also gets a faint `SUBST` tag in its header.

## Local BrainFlow patch

The BrainFlow build in `~/.local/brainflow` was compiled from `~/.local/src/brainflow` with one fix in `src/board_controller/emotibit/emotibit.cpp`, in the ancillary 2× upsampling around line 376. Upstream writes the duplicate to `anc_packages[i + 1]`, which overwrites newer temperature values with older ones. The local copy writes `anc_packages[i * 2 + 1]` (marked `patched (stream_gui_cpp)`; the diff is `third_party/brainflow/emotibit-ancillary-upsample.patch`). **If you rebuild or update BrainFlow, re-apply this patch**, or the EmotiBit temperature trace shows stale, duplicated values. `scripts/build_brainflow.sh` (macOS) and `scripts\build_brainflow.ps1` (Windows) clone 5.23.0, apply the patch and install in one step.

## Known limitations

* BrainFlow serialises every session call behind one global lock. While one device is inside `prepare_session` (Cyton soft reset: a few s; EmotiBit after discovery: ~1–7 s), the other device can't be polled. Its plot shows "HELD" and catches up afterwards, because the samples are buffered in BrainFlow. A Cancel pressed during that phase takes effect when BrainFlow returns.
* BrainFlow labels EmotiBit's nominal rates as placeholders (25 / 25 / 15 Hz). The rate readouts compare the measured rate against them.
* EmotiBit temperature is sample-and-hold at the EDA push rate, which is how BrainFlow packs the ancillary preset.
* Traces are one device pixel wide (thin on a Retina display, 1 px at DPR 1 instead of the design's 1.4 / 1.3 px) — that is what keeps rendering on Qt's fast path.
* Windows: recording paths reach `std::filesystem` and BrainFlow's file streamer as 8-bit strings, which Windows reads in the ANSI code page. A recording folder with non-ASCII characters in its path (e.g. an accented user name under `Documents`) therefore ends up with a garbled name; use `--record-dir` with an ASCII path.
* The dropped-packet counter only exists for the Cyton (its package number increments once per sample); the EmotiBit has no equivalent in BrainFlow's rows.
