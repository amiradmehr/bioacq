# BioAcq for macOS

Double-click `BioAcq.app` to start the GUI.

- Built from `main` at commit 18eeb04 with Homebrew Qt 6.10.2 and BrainFlow 5.23.0 (patched), Apple silicon (arm64), macOS 26 or newer.
- Ad-hoc signed, not notarised. A copy cloned with git opens directly. A copy downloaded as a zip from GitHub is quarantined: right-click the app, choose Open, then Open again (or run `xattr -dr com.apple.quarantine BioAcq.app`).
- On the first EmotiBit connect macOS asks for Local Network access. Click Allow, or the EmotiBit can't be reached.
- Recordings go to `~/Documents/BioAcq Recordings`.
