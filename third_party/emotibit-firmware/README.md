# EmotiBit firmware with Bluetooth (BLE) streaming

The stock EmotiBit firmware (1.14) streams only over Wi-Fi. This folder holds a small patch that makes the EmotiBit stream over **Bluetooth Low Energy by default**. Sampling rates and data packets stay exactly as they are over Wi-Fi.

* **Base:** [EmotiBit_FeatherWing](https://github.com/EmotiBit/EmotiBit_FeatherWing) branch `feat-blePrototype-Example` at commit `abdec15` (library version 1.14.4, firmware version string 1.14.3). This is EmotiBit's own, not yet released, Bluetooth prototype (MIT licence). It adds a BLE GATT server with a Nordic-UART-style service:
  * `6E400001-B5A3-F393-E0A9-E50E24DCCA9E`, the service;
  * `…0003`, the TX characteristic: the EmotiBit's data packets as notifications;
  * `…0002`, the RX characteristic: control packets written by the host.
* **`bioacq-ble.patch`**: one commit on that base.
  1. **Raises the BLE MTU to 517.** The prototype left it at the default of 23, so every notification carried 20 bytes and `BLECharacteristic::notify()` truncated each data chunk of up to 512 bytes, which lost most of the data.
  2. **Sends in chunks that fit.** Each 100 ms batch goes out in chunks of at most (negotiated MTU − 3) bytes, cut after a packet delimiter, with a 2 ms pause between notifications.
  3. **Bluetooth is the default mode.** A new PlatformIO environment, `bioacq_ble_feather_esp32`, defines `EMOTIBIT_BLUETOOTH_DEFAULT`. The prototype needed the button held at boot to get Bluetooth; now holding the button during the boot prompt selects **Wi-Fi** instead.
  4. **Uploads at 115200 baud.** The Feather HUZZAH32's CP2104 USB-serial chip corrupted reads at 460800 and 921600 baud on this Mac.

## Verified

On EmotiBit MD-V7-0001421 (HW V07a, Feather HUZZAH32, ESP32-D0WD-V3, 4 MB flash), with a Mac as the Bluetooth host (negotiated ATT MTU 515), over 60 s:

| | Over Bluetooth | Stock over Wi-Fi |
|---|---|---|
| PPG IR / red / green | 24.72 Hz | 25 Hz |
| Accelerometer / gyroscope / magnetometer | 24.84 Hz | 25 Hz |
| EDA | 14.96 Hz | 15 Hz |
| Temperature T1 / thermopile TH | 7.48 Hz | 7.5 Hz |
| Packets | 9691, **0 missing** (continuous packet counter), 0 malformed | |
| Throughput | 5.7 kB/s in 19 notifications/s of ~300 bytes | |

The stream also carries the firmware's own derived values (`HR`, `BI`, `SF`, battery `BV` / `B%`).

## Build, flash, restore

```bash
git clone https://github.com/EmotiBit/EmotiBit_FeatherWing && cd EmotiBit_FeatherWing
git checkout abdec15 && git am /path/to/bioacq/third_party/emotibit-firmware/bioacq-ble.patch
./download_dependencies.sh            # clones the libraries next to EmotiBit_FeatherWing (needs jq)
cd EmotiBit_stock_firmware
python3 -m pip install platformio esptool
python3 -m platformio run -e bioacq_ble_feather_esp32
```

**Back up the EmotiBit's flash first**, so you can restore it exactly. The EmotiBit must be on USB, with EmotiBit Oscilloscope and any serial monitor closed.

```bash
python3 -m esptool --port /dev/cu.usbserial-XXXX --baud 115200 read-flash 0 ALL emotibit-backup.bin   # ~7 min for 4 MB
python3 -m platformio run -e bioacq_ble_feather_esp32 -t upload --upload-port /dev/cu.usbserial-XXXX  # ~2 min
```

* **Restore:** `python3 -m esptool --port /dev/cu.usbserial-XXXX --baud 115200 write-flash 0 emotibit-backup.bin`, or install stock firmware with EmotiBit's Firmware Installer.
* **Reading the backup twice** gives a few different bytes inside the NVS partition (0x9000–0xDFFF), because the ESP32 rewrites its Wi-Fi calibration data at boot. Differences anywhere else point to a bad read.
* **The lab EmotiBit's backup:** its stock 1.14.0 image from 2026-09-16 is in `emotibit-firmware-backup/` in the project folder (gitignored) and in `~/.local/share/emotibit/backup/`, together with `RESTORE.txt` and a SHA-256.

The EmotiBit's device ID lives on the FeatherWing's EEPROM and its Wi-Fi networks on the SD card. Flashing touches neither, and BioAcq's *wi-fi setup* dialog keeps working, because the config edit mode is unchanged.

## Test tool

`tools/ble_stream_test.py [seconds]` (Python, `pip install bleak`) scans for `EmotiBit: <id>`, subscribes to the TX characteristic, and prints throughput, per-type sample rates and packet-counter gaps.

On macOS a process may use Bluetooth only if the app responsible for it declares `NSBluetoothAlwaysUsageDescription`; otherwise macOS kills it (a TCC crash). Terminal.app and IDEs that don't declare it can't run the tool directly. Run it from a minimal `.app` bundle whose `Info.plist` has the key and whose executable starts the script, and allow Bluetooth when macOS asks.

## Limitations

* **No pairing or encryption yet:** anyone in range can connect and read the data. Add LE Secure Connections bonding before using it with participants.
* **BioAcq can't read the Bluetooth stream yet.** Until it can, boot the EmotiBit in Wi-Fi mode (hold the button during the boot prompt) or restore the stock firmware.
* **No time sync with the computer** (the Wi-Fi mode's `TL` / `TU` packets): timestamps are the EmotiBit's own milliseconds.
* **Range:** Bluetooth reaches about 10 m, and the body can block it when the EmotiBit is worn. Data sent while the link is down is lost; the SD card recording is unaffected.
* **Cosmetic:** `firmware_variant` shows the build path, because the `.ino` splits `__FILE__` on backslashes only.
