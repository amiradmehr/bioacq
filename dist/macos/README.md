# BioAcq for macOS

Double-click `BioAcq.app` to start the GUI.

- Built from `main` at commit 6f02284 with Homebrew Qt 6.10.2 and BrainFlow 5.23.0 (patched), Apple silicon (arm64), macOS 26 or newer.
- Ad-hoc signed, not notarised. A copy cloned with git opens directly. A copy downloaded as a zip from GitHub is quarantined: after the first double-click is refused, open System Settings → Privacy & Security and click Open Anyway next to the BioAcq message (or run `xattr -dr com.apple.quarantine BioAcq.app` before opening it).
- On the first Connect macOS asks for Bluetooth access (an EmotiBit on the bioacq Bluetooth firmware streams over Bluetooth). Click Allow; a newly built copy asks again.
- On the first EmotiBit connect over Wi-Fi macOS asks for Local Network access. Click Allow, or the EmotiBit can't be reached on Wi-Fi.
- Recordings go to `~/Documents/BioAcq Recordings`.
