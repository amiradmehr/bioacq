# EmotiBit Bluetooth firmware

`bioacq-ble.patch` makes the EmotiBit stream over Bluetooth Low Energy by default, at the same rates and in the same packet format as over Wi-Fi. BioAcq connects to it with **Connect**.

**Base:** [EmotiBit_FeatherWing](https://github.com/EmotiBit/EmotiBit_FeatherWing) branch `feat-blePrototype-Example` at `abdec15`, EmotiBit's unreleased Bluetooth prototype (MIT). Its service `6E400001-B5A3-F393-E0A9-E50E24DCCA9E` sends the data packets as notifications on `…0003` and takes host writes on `…0002`.

**What the patch changes:**
* Raises the BLE MTU to 517; the prototype's default of 23 truncated the data.
* Sends each 100 ms batch in chunks of (MTU − 3) bytes, split between packets, 2 ms apart.
* Starts in Bluetooth mode (PlatformIO environment `bioacq_ble_feather_esp32`); holding the button during the boot prompt selects Wi-Fi.
* Uploads at 115200 baud; the Feather's CP2104 corrupted faster transfers on this Mac.

## Verified

On EmotiBit MD-V7-0001421 with a Mac, over 60 s: PPG 24.7 Hz, IMU 24.8 Hz, EDA 15.0 Hz, temperature 7.5 Hz (Wi-Fi: 25 / 25 / 15 / 7.5 Hz); 9,691 packets with none missing; 5.7 kB/s. Through BioAcq: connected in 3 s, then 25.2 Hz PPG, 25.3 Hz IMU and 15.1 Hz temperature over 20 s.

## Build and flash

```bash
git clone https://github.com/EmotiBit/EmotiBit_FeatherWing && cd EmotiBit_FeatherWing
git checkout abdec15 && git am /path/to/bioacq/third_party/emotibit-firmware/bioacq-ble.patch
./download_dependencies.sh                 # clones the libraries next to it (needs jq)
cd EmotiBit_stock_firmware
python3 -m pip install platformio esptool
python3 -m platformio run -e bioacq_ble_feather_esp32
```

Back up the flash first (EmotiBit on USB, EmotiBit Oscilloscope and serial monitors closed), then flash:

```bash
python3 -m esptool --port /dev/cu.usbserial-XXXX --baud 115200 read-flash 0 ALL emotibit-backup.bin   # ~7 min
python3 -m platformio run -e bioacq_ble_feather_esp32 -t upload --upload-port /dev/cu.usbserial-XXXX  # ~2 min
```

* **Restore:** `python3 -m esptool --port /dev/cu.usbserial-XXXX --baud 115200 write-flash 0 emotibit-backup.bin`, or EmotiBit's Firmware Installer.
* Two reads of the same flash differ only in the NVS partition (0x9000–0xDFFF); a difference anywhere else means a bad read.
* The lab EmotiBit's stock 1.14.0 backup is in `emotibit-firmware-backup/` (gitignored) and `~/.local/share/emotibit/backup/`, with `RESTORE.txt` and a SHA-256.
* Flashing keeps the device ID (EEPROM) and the saved Wi-Fi networks (SD card).

## Test tool

`tools/ble_stream_test.py [seconds]` (`pip install bleak`) prints throughput, per-type rates and packet gaps. macOS lets only an app that declares `NSBluetoothAlwaysUsageDescription` use Bluetooth, so run it from a small `.app` wrapper rather than from Terminal.

## Limitations

* No pairing or encryption: add LE Secure Connections bonding before using it with participants.
* No time sync with the computer; timestamps are the EmotiBit's own.
* Range is about 10 m. Data sent while the link is down is lost; the SD card recording is not affected.
* `firmware_variant` shows the build path (cosmetic).
