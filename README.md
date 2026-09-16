# bioacq — real-time Cyton ECG + EmotiBit viewer

`bioacq` is a native C++17 / Qt 6 Widgets application that streams an OpenBCI Cyton (USB serial dongle: one ECG channel) and an EmotiBit (WiFi: PPG green / red / infrared, temperature, IMU) through the BrainFlow C++ API and plots the signals live, with ECG filtering, a PPG heart rate, stream-health readouts and CSV recording. One Connect button opens both devices.

## Repository layout

| Path | Contents |
|---|---|
| `CMakeLists.txt` | Build definition (target `bioacq`) |
| `src/app/` | `main.cpp` (command line, modes), `BuildConfig.h.in` |
| `src/core/` | Header-only building blocks: ring buffer, rate meter, readout rules, decimation |
| `src/dsp/` | Biquad filters, PPG heart-rate tracker (`HeartRate`) |
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

* `main`: the source, documentation and CI. It never contains build output (`dist/` is gitignored here).
* `mac` / `windows`: `main` plus the packaged, double-clickable build for that platform committed under `dist/` (`BioAcq.app` for macOS on Apple Silicon; the `BioAcq` folder with `BioAcq.exe` for Windows x64). They are refreshed from `main` with the packaging scripts (the Windows package comes from the `windows.yml` artifact) and are never merged back into `main`.
* `feat/*`: feature branches, merged into `main` with merge commits (`feat/platform`: Windows port, dongle detection, packaging and CI; `feat/features`: ECG / PPG / heart rate / IMU panels and the single Connect button).

## Signals

| Panel | Source | Units |
|---|---|---|
| **ECG · Cyton Ch1** (hero; single-ended N1P against SRB, AGND bias) | Cyton, 250 Hz, `get_exg_channels()[0]` | µV |
| **PPG green / PPG red / PPG IR** | EmotiBit AUXILIARY preset, `get_ppg_channels()[2]` / `[1]` / `[0]` (BrainFlow's `emotibit.cpp` stores infrared, red, green in that order) | a.u. (counts) |
| **Heart rate** | Derived in the EmotiBit worker from the three PPG channels (display only, see *Heart rate*) | bpm |
| **IMU**: three lanes on one time axis, ACC / GYR / MAG, each X / Y / Z | DEFAULT preset, `get_accel_channels()` / `get_gyro_channels()` / `get_magnetometer_channels()` | g, °/s, µT |
| **Temperature** (T1) | ANCILLARY preset, `get_temperature_channels()[0]` | °C |

All rows are resolved at runtime from BrainFlow's board descriptors, never hard-coded. EDA is not shown.

## Build & run

```bash
scripts/build.sh          # configure (first time) + incremental Release build
scripts/build.sh --clean  # wipe the build tree and reconfigure
scripts/run.sh            # build if needed, then launch the GUI (extra args are passed through)
```

* The scripts locate the repository root from their own location, so they work from any directory.
* The build tree is `~/.local/build/bioacq`, outside Google Drive. Override it with `BIOACQ_BUILD_DIR` (`STREAM_GUI_BUILD_DIR` is still accepted as a fallback alias).
* Binary: `~/.local/build/bioacq/bioacq`. It's a plain executable, not a `.app`, and not on your `PATH`, so use `scripts/run.sh …` or the full binary path. Its rpath points at `~/.local/brainflow/lib` and Homebrew Qt, so it runs without `DYLD_*` variables.
* Requirements: Homebrew Qt 6 (`/opt/homebrew/opt/qt`, modules Core/Gui/Widgets/Network/SerialPort), CMake ≥ 3.21, Ninja, and BrainFlow 5.23.0 installed to `~/.local/brainflow` with the patch in `third_party/brainflow/` (see *Local BrainFlow patch* below). You can override these with `BRAINFLOW_ROOT` or `QT_PREFIX` (together with `--clean`).
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
* `BioAcq.exe` is not code-signed. SmartScreen may show *Windows protected your PC* for a downloaded copy: *More info* → *Run anyway*.

**Cyton port.** Without `--port`, the Cyton port is *Auto*: at every connect the app looks for the OpenBCI dongle (FTDI FT231X, USB VID `0403` / PID `6015`, or a port description naming FT231X / OpenBCI), `COM3`-style names on Windows and `/dev/cu.usbserial-…` on macOS (the `tty.` twin is skipped); see *Using the GUI*. `--probe` without `--port` uses the first dongle found. `--list-ports` prints every serial port with its VID:PID, dongles first. Listing never opens a port.

**CI.** `.github/workflows/windows.yml` runs on every push to `main`, `windows` and `feat/**` (and manually): MSVC 2022 + Qt 6.10 via `install-qt-action`, the patched BrainFlow (cached on the patch and script hash), `package_windows.ps1`, then `--selftest`, a `--screenshot --state live --size 1600x1000` run (offscreen) and a `--screenshot --state idle --size 1280x800` run through the native `windows` platform plugin (the one a double-click loads), all on the packaged folder with Qt taken off `PATH`. The selftest log includes the cost of the serial port scan on Windows. Artifacts: `BioAcq-windows-x64` (the zip) and `bioacq-windows-screenshot`. `.github/workflows/macos.yml` runs `package_macos.sh` on `macos-14` with the official Qt and a BrainFlow built for macOS 13; it is manual only (`gh workflow run macos.yml --ref <branch>`), because macOS minutes cost 10× on a private repository.

## Modes

Run these from the repository root (`scripts/run.sh` builds if needed and passes the arguments through):

| Command | What it does |
|---|---|
| `scripts/run.sh` | Interactive GUI |
| `scripts/run.sh --synthetic` | GUI with both devices simulated by BrainFlow's synthetic board |
| `scripts/run.sh --selftest [s]` | Headless end-to-end test on the synthetic board through the same worker/ring code. Covers the ECG filters (high-pass 0.5 Hz, notch 60 Hz, low-pass 40 Hz), the heart-rate detector (synthetic PPG at 60 / 72 / 120 / 180 bpm, beats on the inverted counts, no HR from noise or a flat signal, source selection, the worker path with the PPG hook), the channel map (PPG order), decimation, EmotiBit packet re-timing, the EmotiBit discovery (against a local fake device), cancel while `prepare_session` is still blocking, recording started/stopped on a running session, the readout rules (rail headroom over 2 s and its clearing, saturation wording, heart-rate readout, rate colour, stall detection, dropped packets, number formatting, CPU meter), serial port enumeration (dongles first, BrainFlow port names, cost of the 1 Hz auto-detect scan) and the default recording folders, the test hooks, clean shutdown and reconnect. Exits 0 on PASS |
| `scripts/run.sh --probe [s]` | Headless check of the **real** devices. Prints per-signal sample counts, rates and latest values. Won't crash if a device is absent. Add `--no-cyton` / `--no-emotibit` to probe one device only |
| `QT_QPA_PLATFORM=offscreen scripts/run.sh --screenshot out.png [--state S] [--size WxH]` | Renders one UI state through the real code paths, saves a PNG and quits (also prints the process CPU use over t = 2–4 s) |

Screenshot states (`--state`, default `live`); every state except `idle` presses Connect (both devices at once) and is captured at t = 6.3 s: `live` (both slots synthetic, with `--test-ppg-bpm 72` so the heart-rate panel has a pulse), `idle` (nothing connected), `connecting` (both still connecting: the synthetic Cyton held 8 s before `prepare_session`, the EmotiBit discovering the unanswered TEST-NET address 192.0.2.1 with a 20 s timeout), `error` (EmotiBit not found at 192.0.2.1 after a 2 s timeout while the synthetic Cyton streams), `recording` (as `live`, the record button pressed after 1.5 s; files go to `--record-dir`, or to a temporary folder that is deleted afterwards), `warning` (both synthetic with the stall and rail-offset test hooks below).

Options: `--port <port>` (`COM3`, `/dev/cu.usbserial-XXXX`; default: the detected OpenBCI dongle), `--list-ports`, `--ip <emotibit ip>` (default `192.168.1.12`), `--discover` (blank IP: the GUI tries the last EmotiBit that answered, then broadcast; `--probe` broadcasts), `--timeout <s>` (2–20), `--record` (arm recording), `--record-dir <dir>`, `--window <s>` (1–60), `--bf-discovery` (see below), `--verbose` (BrainFlow INFO log).

Test hooks (command line only, never in the UI; a warning is printed on stderr): `--test-stall-emotibit [s]` stops polling the EmotiBit after *s* seconds of streaming (default 1.5; the session stays open, so the stalled path renders; `0` freezes before the first sample, which renders the *no samples since connect* path), `--test-rail-offset-uv <µV>` adds a DC offset to the Cyton's raw Ch1 **display** value (the near-rail path), `--test-ppg-bpm <bpm>` replaces the EmotiBit's PPG **display** values with a synthetic pulse at that rate (the heart-rate path; the synthetic board's own PPG is noise, which correctly yields no heart rate). Recordings are not affected by any of them.

## Using the GUI

The window is an instrument panel: a 30 px session bar (session label = start time + streaming devices, REC chip while recording, local clock), a 264 px control rail on the left, the plots, and a 26 px status bar (device states, aggregate rate, window, recording state, the current message, Cyton dropped packets, process CPU). A banner above the plots reports a stalled stream / near-rail or saturated ECG (amber), a failed connection (red) or a running recording (red outline). With nothing connected, the hero plot shows a call to action: the Connect button, the shortcut hints and the *simulate devices* switch. Every value on screen comes from real state; what can't be measured is omitted or shown as `—`.

* **Layout.** The ECG across the top; below it PPG green | PPG red | PPG IR | Heart rate (the three PPG channels and the rate they produce, in signal-flow order); at the bottom the IMU panel over three columns (its three lanes need the width and the height) and the temperature (a slow, sample-and-hold channel) in the fourth. Columns line up across both rows. The layout fits 1280 × 800 without hiding plots: a panel too narrow for its whole header drops the units first, then the rate, before the title is elided, and the IMU readout strip drops its legend glyphs. The rail fits 1280 × 800 even with the EmotiBit discovery module open; it scrolls internally only with an error module open. Like the design's 1280 × 800 artboard, an overflow of at most the last module's bottom padding is clipped rather than adding a scroll bar.
* **Before connecting, close the OpenBCI GUI and EmotiBit Oscilloscope.** Only one program can own the Cyton dongle, and only one host can own the EmotiBit stream.
* **Connect (one button).** `[ connect ]` at the top of the rail (or in the idle call to action, **⌘K** / **Ctrl+K**) starts both devices at once. While any attempt is still in progress it reads `[ cancel ]` and cancels the attempts in progress (a device that already streams keeps streaming; ⌘K does the same). Once a device streams it reads `[ disconnect ]` and stops everything (⌘K never disconnects). Each device module keeps its state chip and a one-line detail: port or IP, device serial, rate and link time, or the reason it is not streaming.
  * **Not available is not an error.** No Cyton dongle on USB marks the Cyton `NOT FOUND` with `no dongle on USB · plug it in, then connect` and never raises a dialog or, while the EmotiBit streams, a banner. An EmotiBit that does not answer is `NOT FOUND` with the design's inline error module (unicast / subnet explanation) and the red banner. A modal dialog appears only for errors that need action outside the app: the Cyton port cannot be opened or the board does not answer (another program such as the OpenBCI GUI holds the dongle, or the board is off), or the EmotiBit discovery packet cannot be sent (Local Network permission on macOS, the firewall on Windows).
  * The device settings are locked (read-only, drawn at rest) while that device is in use, and editable again once it has stopped, even if the other device still streams. To retry one device, disconnect and connect again.
* **Cyton (ECG).** The port field defaults to **Auto**: at every connect it takes the last port that connected if that port is a detected OpenBCI dongle, else the first dongle found (FTDI FT231X, VID:PID `0403:6015`, or a description naming FT231X / OpenBCI), so a saved port never hides a dongle that moved to another port. While the Cyton is idle, the detail line follows plugging and unplugging (`auto · COM3`, `auto · no dongle detected`; rescanned once per second, which only reads the OS device list). The drop-down lists every USB serial port, dongles first (hover an entry for its description and VID:PID). Pick or type a port (`COM3`, `/dev/cu.usbserial-…`) to override Auto; a missing override is `NOT FOUND`, never a fallback to another port. The rescan button or **⌘R** / **Ctrl+R** rescans. BrainFlow soft-resets the board on connect, which takes a few seconds.
  * *ECG display filters* (2 × 2, all on by default): *Remove DC* (display only, subtracts the mean of the visible window), *HP 0.5 Hz*, *Notch 60 Hz* and *LP 40 Hz*: RBJ biquads run sample-by-sample in the worker thread in that order (high-pass, notch, low-pass). Filter state resets whenever you toggle one. The hero plot header shows the four filter states as chips.
  * *Rail headroom · 2 s* = 1 − max|raw ECG| / 187,500 µV over the **last 2 s**, read straight from the Cyton's ring buffer on the raw value before filters. A DC offset near the rail therefore shows even with DC removal on, the readout keeps following the live data while the display is paused, and the warning clears within ~2 s after the input recovers. The percentage is floored to 0.1 % (0.0996 reads `9.9 %`, never `10 %`). Below 10 % it turns amber (green again above 12 %), and the hero plot switches to the raw value on a fixed ±187,500 µV range: dashed ±FS rail lines, labelled `+187,500 µV` / `−187,500 µV`, LATEST reading `LATEST RAW`, and a single amber `RAW` chip instead of the filter chips (none of them applies to the raw view). The hero's LED stays the Cyton link state.
  * *Saturated.* When |raw| reaches full scale (≥ 187,499 µV) the module reads `SATURATED`, and the banner says *ECG saturated at ±187,500 µV — reseat the electrode or check contact* instead of a meaningless *within 0.0 %*.
  * *DROPPED* in the status bar counts packets missing from the Cyton's package-number sequence since connect. It appears once the Cyton has delivered packets (not while it is still connecting).
* **EmotiBit.** The IP field defaults to `192.168.1.12`: a typed IP is reached by unicast (works across subnets). **Blank** means *auto*: the last EmotiBit that answered is tried by unicast first and, if it does not answer, broadcast discovery runs (same subnet only, see below). The amber module shows elapsed / timeout, the target (unicast address, or broadcast after the fallback) and the probes sent. If nothing answers, the red module explains why and shows this computer's actual IPv4 subnet(s).
* **Heart rate.** The panel shows the rate as a large readout (`72 bpm`), a trend of the rate over the plot window (one point per beat), and two chips: the PPG channel it comes from (`SOURCE GREEN` / `RED` / `IR`, picked automatically) and `QUALITY n %`. Without a valid rate the value is `—` and the second chip says why: `ACQUIRING` (the first 8 s after the stream starts), `NO PULSE` (fewer than 4 beats, or no periodic pulse), `IRREGULAR · Q n %` (fewer than 70 % of the beat intervals within ±20 % of the median) or `NO DATA` (no heart-rate update for more than max(3 s, 1 s + 1.5 beat intervals), e.g. the PPG stalled). The rate is display-only: it is not recorded. See *Heart rate* below.
* **IMU.** One panel, one shared time axis, three stacked lanes (ACC g, GYR °/s, MAG µT), each with its own Y autoscale and range labels; X solid, Y dashed, Z dotted in the design's trace colours. The strip under the header shows all nine latest values, grouped by lane.
* **Plot readouts.** Every BrainFlow panel header shows `measured / nominal Hz` over the last 2 s (amber when more than 5 % below nominal, `— / nominal` before the first sample) and the latest value(s); the hero also shows the window's peak-to-peak. A stream with no new samples for more than 1 s is *stalled*: amber values and LED, `0.0 / nominal`, and a `LAST SAMPLE x.x s AGO` overlay (plain amber text). A stream that never delivered a sample is stalled too, 1 s (Cyton) / 3 s (EmotiBit, whose worker skips leading all-zero samples for up to 3 s) after its device reached *streaming*; its overlay reads `NO SAMPLES SINCE CONNECT · x.x s`. When every stream of a device is stalled, the device chip reads `STALLED`, the amber banner shows, and the overlay appears once, on the device's largest panel (the IMU for the EmotiBit). A partial stall labels each stale plot and is named in the status bar. If the cause is BrainFlow's lock (the other device is inside `prepare_session`), the overlay and chip say `HELD` instead; those samples are buffered and the plot catches up. `All streams nominal` is printed only when every channel of every streaming device has data at a nominal rate.
* **Record.** `[ record to csv ]` starts recording immediately on every streaming device and `[ stop recording ]` closes the files — BrainFlow's file streamers can be added to and removed from a running session (`Board::add_streamer` / `delete_streamer` take the same board lock the data thread holds while it pushes samples, and deleting a file streamer closes the file). Pressed with nothing connected, the button *arms* the recording (amber): every device then records from its first sample when it connects, as `--record` does. Devices that connect while a recording runs join it.
  * Files go to `<folder>/<device>_<preset>_<yyyyMMdd_HHmmss>.csv`, where the folder is `--record-dir` or the default: `recordings/` in the repository for development builds, `Documents/BioAcq Recordings` for `BioAcq.app` / `BioAcq.exe` (and for a development build whose `recordings/` is not writable); one file per BrainFlow preset: Cyton has `default`; EmotiBit has `default`, `auxiliary` and `ancillary`. A second recording started within the same second gets a `_2` suffix instead of overwriting.
  * BrainFlow writes every row of the preset tab-separated with no header, despite the `.csv` name. A `*_columns.json` next to each file maps row numbers to channels. The rows are raw: the ECG filters, the EmotiBit re-timing and the heart rate are display-only.
  * Timestamps are UNIX seconds, BrainFlow's raw values.
  * The record module shows the folder, the elapsed time and the files' actual size on disk (polled once per second).
  * Recording falls back to `~/bioacq_recordings` if the folder can't be created, is too long for BrainFlow's 512-byte limit, or contains a `:` (other than a Windows drive letter's). BrainFlow splits `file://path:mode` at the last `:`.
* **Display.** *Time window* sets the visible span, 1–60 s. *Pause display* freezes the plots only; streaming, recording, the heart rate and the rail-headroom / near-rail warning continue.
* **Simulate devices.** The *simulate devices* switch sits in the idle call to action, next to the shortcut hints. It can only change while nothing is connected, which is exactly when that overlay shows. It connects both slots to BrainFlow's synthetic board; a synthetic session is labelled `synthetic` in the session bar, the device detail lines and the hero subtitle. The synthetic board's PPG is noise, so its heart rate reads `NO PULSE` (use `--test-ppg-bpm` to see a pulse).
* **Remembered settings.** The GUI stores the Cyton port choice (`Auto` or an override) and the last port that connected, the EmotiBit IP field and the last EmotiBit that answered, the discovery timeout, window length, the four ECG filter toggles and the armed/recording state of the record button. The ECG filter keys are new (`ecg/*`): values saved by older versions under `cyton/*` (high-pass at 1 Hz, filters off by default) are ignored. Settings are saved on each successful connect and on close, and restored at start. The store is `QSettings`: `~/Library/Preferences/com.bioacq.bioacq.plist` on macOS, `HKEY_CURRENT_USER\Software\bioacq\bioacq` on Windows. Command-line `--port` (becomes the port override) / `--ip` / `--timeout` / `--window` / `--record` override the stored values. `--screenshot` neither reads nor writes them.

## Heart rate

Computed in the EmotiBit worker thread from the display samples of all three PPG channels, after the packet re-timing; the ring buffer `emotibit.hr` gets one sample per beat of the source channel (bpm, quality, source, beats, periodicity) or, while there is no valid rate, a status sample about once a second. Implementation: `src/dsp/HeartRate.{h,cpp}`.

1. **Band-pass 0.5–4 Hz** per channel: one RBJ high-pass and one RBJ low-pass biquad (Q = 1/√2), designed at the nominal PPG rate. The rate is re-measured over the first 4 s of every uninterrupted stretch of samples (gaps of more than three nominal sample periods are left out, a gap over 1 s starts a new stretch) and the filters are redesigned if it is more than 15 % off, within 0.6–1.6 × nominal. (It used to be measured once over everything since the first sample: a 1.5 s WiFi dropout in the first 4 s redesigned the filters for about half the real rate and blanked the heart rate for the rest of the session. A second high-pass stage made the filter ring: at 30–45 bpm its rebound in the long diastole reached half a beat's height and counted as a beat.)
2. **Inverted.** The MAX30101 measures reflected light, and more blood absorbs more light, so the counts **drop** at systole: the detector works on −bandpass(counts). The selftest checks that the beats land just after the count minima of a synthetic pulse, not half a beat away.
3. **Beats** after Elgendi et al. (2013): the clipped square of the signal is averaged over 111 ms (a systolic peak) and 667 ms (a beat); a block where the first exceeds the second (plus a small offset) and lasts at least 111 ms yields one beat at its largest sample, timed by parabolic interpolation between samples. A beat must reach 35 % of the largest beat of the last 3 s and come at least 0.29 s after the previous one (200 bpm with 5 % slack); a larger peak inside that refractory period replaces the previous beat.
4. **Estimate** over the last 8 s: inter-beat intervals outside 30–200 bpm are outliers; the median of the others anchors the estimate; **quality** = share of all intervals within ±20 % of the median; **HR = 60 / mean of those inlier intervals** (the mean of the inliers averages the sub-sample timing error that a single median interval keeps: at 25 Hz this halved the error at 120–180 bpm). The rate is valid only with ≥ 4 beats, quality ≥ 70 %, a recent beat and **periodicity ≥ 0.5**: the autocorrelation of the band-passed signal at the beat interval minus its (positive) autocorrelation at half that interval. Band-passed noise also produces fairly regular "beats", but it does not repeat itself, and slow respiratory wander stays correlated at half a beat as well; without this check about 3 % of noise-only time read as a heart rate in simulation, and with the plain autocorrelation noise riding on strong wander still passed at 250 Hz.
5. **Source**: the valid channel with the best quality, switched only when another channel is better by 15 points for 3 s.

Accuracy on the selftest's synthetic PPG (25 Hz, ±2 ms timing jitter, noise at 8 % of the pulse amplitude, respiratory and slow baseline wander): within 0.3 bpm at 60, 72, 120 and 180 bpm; noise-only and flat signals give no rate.

Limitations:
* Display-only and not validated against an ECG-derived rate on people; no motion-artefact rejection beyond the quality and periodicity gates. Wrist PPG during movement will mostly read `NO PULSE` / `IRREGULAR`, which is the intended failure mode.
* The EmotiBit delivers PPG at 25 Hz in packets; timing within a packet comes from the re-timing, so single intervals carry a few ms of error. The 8 s window makes the rate lag real changes by a few seconds, and the first value appears about 4 beats after the stream starts.
* Below ~40 bpm an 8 s window holds only 4–5 beats, so the rate can drop out.

## Networking / EmotiBit not found

* **Same WiFi isn't the same subnet.** The computer and the EmotiBit can join the same SSID and still land on different subnets, e.g. the computer on `172.31.x.x/16` and the EmotiBit on `192.168.1.12`. Broadcast discovery then fails with "no EmotiBit answered". A typed IP always works, because it is reached by unicast across subnets. That is why the field defaults to `192.168.1.12`. With the field blank the app first tries the last EmotiBit that answered (unicast), so a blank field keeps working across subnets once one connect has succeeded.
* **Finding the IP.** The EmotiBit prints its IP in its serial boot log (Feather USB serial monitor). EmotiBit Oscilloscope also shows it. Type it into the IP field. After one successful connect the app remembers the field and the address that answered.
* If discovery with a blank IP fails, the error module says so, shows this computer's subnet, and asks for the IP.
* **Close EmotiBit Oscilloscope and the OpenBCI GUI** before connecting. Only one host can own the EmotiBit stream, and only one program the Cyton dongle.
* **macOS Local Network permission.** If sending the discovery packet fails ("DISCOVERY BLOCKED"), macOS may be blocking Local Network access for the app that launched this program. Allow it under System Settings → Privacy & Security → Local Network: for `BioAcq.app`, or for Terminal, iTerm or VS Code when the development build is started from there.
* **Windows firewall.** Windows Defender Firewall asks the first time `BioAcq.exe` contacts the EmotiBit; allow it on private networks. If the prompt was dismissed, discovery replies are dropped (EmotiBit `NOT FOUND`) or sending fails (`DISCOVERY BLOCKED`): allow `BioAcq.exe` under Windows Security → Firewall & network protection → Allow an app through firewall.
* **How the connect works.** The app first finds the EmotiBit with its own UDP handshake: HELLO_EMOTIBIT to port 3131, waiting for HELLO_HOST, identical to BrainFlow's. This runs for a typed IP as well as for broadcast (and for the blank field's unicast-then-broadcast fallback), can be cancelled, and does not hold BrainFlow's global lock. Only then does it call BrainFlow's `prepare_session` with the found IP. That holds the lock for about 1–7 s: a 1 s fixed sleep plus the TCP handshake. `--bf-discovery` switches back to BrainFlow's own discovery. It holds the lock for the whole search, pausing the Cyton, and is kept only as an escape hatch.

## Architecture

* `DeviceWorker`: one `std::thread` per device. It runs discovery (EmotiBit), `prepare_session`, `add_streamer` / `delete_streamer` and `start_stream`, then polls `get_board_data(preset)` every 10–15 ms. Only the plotted rows go into `SignalRing`s, which are fixed-capacity and mutex-guarded. The GUI thread never calls a blocking BrainFlow function; recording requests are applied by the worker within one poll interval.
  * EmotiBit timestamps: BrainFlow stamps every sample of a packet (up to ~9) with the same host time. For display, `PacketRetimer` spreads each packet evenly over the interval since the previous one, so PPG and IMU draw as waveforms, not combs. Recordings keep BrainFlow's raw timestamps.
  * Cancel / Disconnect / close while `prepare_session` is still blocking: the worker releases the session as soon as BrainFlow returns. The EmotiBit search itself stops within ~0.1 s.
  * Closing hides the window at once and waits for both workers. A safety watchdog forces the exit if release takes longer than the configured worst case (≈ 15 s + per-device budget). It then prints which device was still busy, flushes stdio, including BrainFlow's recording files, and exits with status 1.
* `DeviceWorker` also runs the heart-rate tracker (`HeartRate::Tracker`) on the re-timed PPG display samples and appends to the derived `emotibit.hr` ring; the ECG filter chain (high-pass, notch, low-pass) runs there too.
* `PlotWidget`: a custom QPainter strip chart drawn as one panel section (header, optional readout strip, recessed plot on a dot raster), redrawn at up to 60 Hz only when data or time has moved. Kinds: `Hero` (ECG), `Scalar` (PPG, temperature), `Lanes` (IMU: stacked lanes on one time axis, each with its own ring, units and Y autoscale; the strip shows every value) and `Vital` (heart rate: large value, chip strip, trend). Every panel is one or more lanes; decimation, pens and the recess cache work per lane and trace.
  * The static recess is cached in two layers. The dot raster and border are rasterised only when the size / DPR change; each dot is a crisp 2 device-px block in the composited colour, which is how a browser renders the design's `radial-gradient` tile. On top of one blit of that layer, the zero / rail lines and the axis and time labels are redrawn only when the snapped Y range, the window or the rail view change. Trace frames repaint only the recess, and header text is refreshed at ≤ 8 Hz and repaints only the header.
  * Per-pixel min/max decimation keeps it to ≤ 2 points per column at any sample rate. Bins are anchored in absolute time, so peaks don't shimmer while scrolling, and switching between exact and decimated drawing has hysteresis.
  * Traces use cosmetic 1 device-px pens: Qt's fast line path. Exact segments are antialiased and keep the design's dash patterns (X solid, Y dashed 7 4, Z dotted 2 4); min/max-decimated segments are solid and aliased. The design's 1.4 / 1.3 px widths would need Qt's stroker, which measured at ~100 % of one core with every segment sparse (offscreen 1600×1000, both slots synthetic, 1 s window) versus ~20 % with cosmetic pens.
  * Gaps and NaN/Inf values are drawn as gaps.
  * Y autoscale has hysteresis (expands immediately, shrinks slowly), is snapped outwards to a round step (stable axis labels), and has a minimum span of one display digit. With DC removal or the high-pass on, the hero's range is symmetric around 0.
  * Time axis runs from −window to 0 with x = ts − host now, so a stalled stream visibly scrolls away.
* `Theme` (tokens, fonts, global stylesheet; PPG ink follows the LED colour: green #7FBF7F as in the design, red #C07A6E, infrared the design's teal trace ink, heart rate its cream ink), `Widgets` (toggle switch, status chip, meter bar, banner, stepper, record / rescan buttons, session and status bars) and `Readouts` (headroom over 2 s, saturation wording, rate tone, stall rule, package-number gaps, heart-rate readout, formatting, CPU meter; header-only and covered by `--selftest`).
* `SignalSpec`: each plotted signal is described as {preset, rows, timestamp row, label, units}, resolved via the BoardShim getters. The same pipeline therefore runs on `SYNTHETIC_BOARD`. The synthetic board has no magnetometer, so that lane uses its auxiliary accelerometer instead, and only two PPG rows, so PPG green and red share one. A *derived* signal (the heart rate) has no rows: it resolves when at least one of its inputs does. Each plot's tooltip names its source rows; on a real device, a channel resolved through a fallback also gets a faint `SUBST` tag in its header.

## Local BrainFlow patch

The BrainFlow build in `~/.local/brainflow` was compiled from `~/.local/src/brainflow` with one fix in `src/board_controller/emotibit/emotibit.cpp`, in the ancillary 2× upsampling around line 376. Upstream writes the duplicate to `anc_packages[i + 1]`, which overwrites newer temperature values with older ones. The local copy writes `anc_packages[i * 2 + 1]` (marked `patched (stream_gui_cpp)`; the diff is `third_party/brainflow/emotibit-ancillary-upsample.patch`). **If you rebuild or update BrainFlow, re-apply this patch**, or the EmotiBit temperature trace shows stale, duplicated values. `scripts/build_brainflow.sh` (macOS) and `scripts\build_brainflow.ps1` (Windows) clone 5.23.0, apply the patch and install in one step.

## Known limitations

* BrainFlow serialises every session call behind one global lock. While one device is inside `prepare_session` (Cyton soft reset: a few s; EmotiBit after discovery: ~1–7 s), the other device can't be polled. Its plot shows "HELD" and catches up afterwards, because the samples are buffered in BrainFlow. A Cancel pressed during that phase takes effect when BrainFlow returns.
* BrainFlow labels EmotiBit's nominal rates as placeholders (25 / 25 / 15 Hz). The rate readouts compare the measured rate against them.
* EmotiBit temperature is sample-and-hold at the EDA push rate, which is how BrainFlow packs the ancillary preset.
* Traces are one device pixel wide (thin on a Retina display, 1 px at DPR 1 instead of the design's 1.4 / 1.3 px) — that is what keeps rendering on Qt's fast path.
* Windows: recording paths reach `std::filesystem` and BrainFlow's file streamer as 8-bit strings, which Windows reads in the ANSI code page. A recording folder with non-ASCII characters in its path (e.g. an accented user name under `Documents`) therefore ends up with a garbled name; use `--record-dir` with an ASCII path.
* The dropped-packet counter only exists for the Cyton (its package number increments once per sample); the EmotiBit has no equivalent in BrainFlow's rows.
* The heart rate is display-only and not recorded; see *Heart rate* for its limits.
* A device that failed while the other streams can only be retried with *disconnect* then *connect* (one button for both devices).
* Windows builds are verified by CI on the synthetic board only (selftest, offscreen and native screenshots of the packaged folder); the Cyton dongle and the EmotiBit have not been tried on Windows yet.
* `BioAcq.app` built locally with Homebrew Qt needs the macOS version Homebrew's bottles target (currently 26). For older macOS (13+), build with the official Qt and a BrainFlow built for 13.0, as `macos.yml` does (not yet run).
