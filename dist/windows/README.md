# BioAcq for Windows

Double-click `BioAcq\BioAcq.exe` to start the GUI. Keep the whole `BioAcq` folder together; the exe needs the DLLs and plugin folders next to it.

- Built from `main` at commit 951c04f by the Windows GitHub Actions workflow (MSVC 2022, Qt 6, BrainFlow 5.23.0 patched), Windows 10/11 x64. The Visual C++ runtime is included.
- Unsigned. A copy downloaded as a zip may show SmartScreen: click More info, then Run anyway. A copy cloned with git runs directly.
- Windows Firewall may ask on the first EmotiBit connect. Allow BioAcq on private networks.
- The Cyton dongle is found automatically on its COM port (FTDI driver required).
- Recordings go to `Documents\BioAcq Recordings`.
