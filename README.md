# BioAcq

A native C++ / Qt 6 app that streams an **OpenBCI Cyton** (one ECG channel) and an **EmotiBit** (PPG, temperature, IMU) through BrainFlow and plots them live, with ECG filters, a heart rate from the PPG, stream-health readouts and CSV recording. One Connect button opens both devices.

## Get the app

Download from the [releases](https://github.com/amiradmehr/bioacq/releases) (or check out the `mac` / `windows` / `linux` branch, which holds the same build under `dist/`).

* **macOS** (Apple silicon, macOS 26+): unzip `BioAcq-macos-arm64.zip`, move `BioAcq.app` to Applications and open it. The app is not notarised: if macOS refuses to open it, go to System Settings → Privacy & Security and click **Open Anyway**, or run `xattr -dr com.apple.quarantine BioAcq.app` first.
* **Windows** (10/11 x64): unzip `BioAcq-windows-x64.zip`, keep the `BioAcq` folder together and run `BioAcq.exe`. If SmartScreen appears: More info → Run anyway.
* **Linux** (x86_64, glibc 2.35+): untar `BioAcq-linux-x86_64.tar.gz`, keep the `BioAcq` folder together and run `./BioAcq`. Add yourself to the `dialout` group for the Cyton dongle (`sudo usermod -aG dialout $USER`, then log in again). For an embedded image, build the Yocto layer instead ([yocto/README.md](yocto/README.md)).

## Connect

Close the OpenBCI GUI and EmotiBit Oscilloscope, then press **Connect** (⌘K / Ctrl+K). Both devices start at once; a missing device doesn't stop the other. While connecting the button cancels, while streaming it disconnects.

**Cyton (ECG).** Plug in the dongle (switch on GPIO6) and set the board to PC. The dongle's port is found automatically; pick a port in the list to override it, and use ⟳ (⌘R / Ctrl+R) to rescan. Channel 1 is single-ended: electrode on N1P, reference on SRB, bias on AGND. BrainFlow resets the board on connect, which takes a few seconds.

**EmotiBit (Bluetooth).** The EmotiBit needs the bioacq Bluetooth firmware ([third_party/emotibit-firmware](third_party/emotibit-firmware/README.md)). Switch it on, keep it near the computer and press Connect.
* macOS asks for Bluetooth access on the first Connect, and again after a new version is installed. Click Allow. If it was refused, turn BioAcq on under System Settings → Privacy & Security → Bluetooth.
* Linux needs BlueZ: `bluetoothd` running and the adapter unblocked (`rfkill unblock bluetooth`).
* If the link drops, the plots show `STALLED` and BioAcq reconnects on its own; a recording continues with a gap.
* With several EmotiBits around, the one used last is preferred.

**EmotiBit on Wi-Fi** (stock firmware, or the bioacq firmware started with its button held): start BioAcq with `--emotibit-link wifi` (or `auto` to try Bluetooth and Wi-Fi at once), e.g. `open -a BioAcq --args --emotibit-link wifi`. The EmotiBit panel then shows a search timeout and **wi-fi setup**, which saves Wi-Fi networks to the EmotiBit over USB. Connect tries the last EmotiBit that answered, then broadcasts on this computer's networks (same subnet only); `--ip <address>` gives an address to try first. macOS asks for Local Network access and Windows Firewall may ask too: allow both.

## The screen

* **Plots:** ECG (Cyton channel 1, 250 Hz, µV), the largest, across the top; PPG green, red and infrared (25 Hz); heart rate, temperature and motion (the IMU's accelerometer, gyroscope and magnetometer in three lanes, 25 Hz).
* **Left rail:** device status and a detail line per device, the ECG display filters, rail headroom, time window, pause, and record.
* **ECG filters** (display only, all on): Remove DC, high-pass 0.5 Hz, notch 60 Hz, low-pass 40 Hz.
* **EmotiBit filters** (display only, both on): *PPG DC removal* shows each PPG signal minus its 2 s moving average, so the pulses sit around 0 without the DC level or slow drift; *Temperature moving average* smooths the temperature over 3 s.
* **Rail headroom** is how far the raw ECG of the last 2 s stays from the ±187.5 mV input range. Below 10 % it turns amber and the plot shows the raw signal; at full scale it reads `SATURATED` (check electrode contact).
* **Stream health:** each plot header shows the measured / nominal rate. `STALLED` means no samples for over a second; `HELD` means the device waits while the other one connects (BrainFlow does one device setup at a time) and catches up afterwards. `DROPPED` in the status bar counts lost Cyton packets.
* **Heart rate** comes from the PPG and is display-only (not recorded). It shows the rate, the PPG channel used and a quality score; otherwise `ACQUIRING`, `NO PULSE`, `IRREGULAR` or `NO DATA` says why there is no value. The first value takes about 5 s.
* **Simulate devices** (switch on the start screen) runs both slots on BrainFlow's synthetic board, without hardware.
* Settings (port choice, filters, time window, record arm, last EmotiBit) are remembered between runs.

## Recording

**[ record to csv ]** starts and stops recording on every streaming device. Pressed while nothing is connected, it arms recording for the next Connect.

* Files go to `Documents/BioAcq Recordings` (change with `--record-dir`), one per device and BrainFlow preset: `<device>_<preset>_<yyyyMMdd_HHmmss>.csv`. The Cyton writes `default`; the EmotiBit writes `default` (motion), `auxiliary` (PPG) and `ancillary` (EDA, temperature).
* Despite the `.csv` name the files are tab-separated with no header. A `*_columns.json` next to each file names the columns.
* Rows are BrainFlow's raw data with UNIX timestamps; the filters and the heart rate are not applied.
* If the folder can't be used, recordings go to `~/bioacq_recordings`.

## Command line

On macOS run the binary inside the app (`/Applications/BioAcq.app/Contents/MacOS/BioAcq`); for Bluetooth start it with `open -a BioAcq --args …`, since macOS only allows Bluetooth for the app itself. On Windows use `start /wait BioAcq.exe …` or `scripts\run_cli_windows.ps1` to see output and exit codes. On Linux run `bioacq` (or `./BioAcq` in the unpacked folder) like any other program. `--help` lists everything.

| Command | What it does |
|---|---|
| (no arguments) | the app |
| `--synthetic` | the app with simulated devices |
| `--selftest [s]` | end-to-end test on the synthetic board; exit code 0 on PASS |
| `--probe [s]` | connects to the real devices and prints sample counts and rates (`--no-cyton` / `--no-emotibit` to skip one) |
| `--screenshot out.png [--state S] [--size WxH]` | renders a UI state to a PNG (`live`, `idle`, `connecting`, `error`, `recording`, `warning`, `wifi`) |
| `--list-ports` | serial ports, OpenBCI dongles first |
| `--emotibit-wifi-list` | the Wi-Fi networks saved on the EmotiBit, read over USB (restarts it) |

Options: `--port <port>`, `--emotibit-link bluetooth|wifi|auto`, `--ip <address>`, `--timeout <s>`, `--record`, `--record-dir <dir>`, `--window <s>`, `--verbose`.

## Build from source

macOS, with Homebrew Qt 6 (including Qt Serial Port and Qt Bluetooth), CMake 3.21+ and Ninja:

```bash
scripts/build_brainflow.sh      # once: BrainFlow 5.23.0 with the local patch, into ~/.local/brainflow
scripts/run.sh                  # build (in ~/.local/build/bioacq) and start
scripts/run.sh --selftest       # run the tests
scripts/package_macos.sh        # dist/macos/BioAcq.app and dist/BioAcq-macos-arm64.zip
```

Windows, in a VS 2022 x64 developer PowerShell with CMake, Ninja and Qt 6.10 `msvc2022_64` (Serial Port and Connectivity):

```powershell
scripts\build_brainflow.ps1
scripts\package_windows.ps1 -QtPrefix C:\Qt\6.10.2\msvc2022_64    # dist\windows\BioAcq\BioAcq.exe
```

Linux, with Qt 6 (Base, Serial Port, Connectivity), CMake 3.21+, Ninja and a C++17 compiler — on Debian/Ubuntu `qt6-base-dev qt6-serialport-dev qt6-connectivity-dev libgl1-mesa-dev libudev-dev`:

```bash
scripts/build_brainflow.sh      # same patched BrainFlow, into ~/.local/brainflow
scripts/run.sh                  # build and start
scripts/package_linux.sh        # dist/linux/BioAcq and dist/BioAcq-linux-x86_64.tar.gz
```

For a Yocto image use the layer in [`yocto/`](yocto/README.md): Qt and BrainFlow come from the image, so BioAcq is a normal package and nothing is bundled.

* BrainFlow needs one local fix for the EmotiBit temperature ([third_party/brainflow](third_party/brainflow/README.md)); the scripts apply it.
* The development binary started from a terminal can't use Bluetooth on macOS. Use `--synthetic`, `--emotibit-link wifi`, or package the app and `open` it.
* The packages are self-contained: Qt, BrainFlow and their licences are inside. The macOS package is signed ad hoc and needs macOS 26 when built with Homebrew Qt.

## Repository

* `src/`: `app` (entry point), `core` (buffers, readout rules), `dsp` (filters, heart rate), `devices` (BrainFlow worker, EmotiBit Bluetooth bridge and Wi-Fi discovery), `ui` (window, plots, theme), `tools` (selftest, probe), `platform` (sockets, serial ports, permissions).
* `scripts/` builds, runs and packages; `resources/` holds fonts and icons; `third_party/` the BrainFlow patch, the EmotiBit firmware patch and the Qt licence texts; `yocto/` the `meta-bioacq` layer for embedded images; `design/` the visual spec.
* Branches: `main` is the source; `mac`, `windows` and `linux` are the source plus the packaged app in `dist/`; features are developed on `feat/*` and merged into `main`.
* CI builds, tests and packages on pushes to `main`, the platform branches and `feat/**`: `windows.yml` and `linux.yml`; `macos.yml` runs only on demand.

## Known limitations

* The EmotiBit's Bluetooth link is not paired or encrypted: while no other computer is connected, anyone nearby running BioAcq can connect. Range is about 10 m. Bluetooth has not been tried with the EmotiBit on Windows, and neither device has been tried with real hardware on Windows.
* BrainFlow sets up one device at a time, so the other device shows `HELD` for a few seconds during a connect.
* BrainFlow's nominal EmotiBit rates (25 / 25 / 15 Hz) are placeholders, and the temperature is sample-and-hold.
* The heart rate is not validated against ECG on people; sustained movement reads `NO PULSE` or `IRREGULAR`.
* Windows: a recording folder with non-ASCII characters gets a garbled name; use `--record-dir` with an ASCII path.
* Linux is built and tested in CI (selftest and screenshots), but neither device has been tried on it with real hardware, and the Yocto recipes have not been run through a build yet.
