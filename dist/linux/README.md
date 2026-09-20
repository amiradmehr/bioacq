# BioAcq for Linux

Run `BioAcq/bin/bioacq` (or `BioAcq/BioAcq`) and keep the `BioAcq` folder together.

- x86_64, glibc 2.35+ (Ubuntu 22.04 and newer). Built from `linux` at 02ec559 by GitHub Actions; Qt and BrainFlow are inside the folder.
- The Cyton dongle needs the `dialout` group: `sudo usermod -aG dialout $USER`, then log in again.
- The EmotiBit connects over Bluetooth and needs BlueZ: `bluetoothd` running and the adapter unblocked (`rfkill unblock bluetooth`).
- Recordings go to `Documents/BioAcq Recordings`.
- For an embedded image build the Yocto layer in `yocto/` instead; nothing is bundled there.
