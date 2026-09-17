# BrainFlow patch

BioAcq builds against BrainFlow **5.23.0** (commit `7994b54b`) with one fix, `emotibit-ancillary-upsample.patch`.

BrainFlow upsamples the EmotiBit ancillary preset 2× in `src/board_controller/emotibit/emotibit.cpp` (`read_thread`, around line 376), but writes each duplicate to `anc_packages[i + 1]` instead of `anc_packages[i * 2 + 1]`. Newer temperature values are overwritten with older ones, so the trace shows stale, repeated samples. The fixed line is marked `patched (stream_gui_cpp)`.

## Build

```bash
scripts/build_brainflow.sh [prefix]            # macOS / Linux; default ~/.local/brainflow
```

```powershell
scripts\build_brainflow.ps1 [-Prefix <dir>]    # Windows, VS 2022 x64 developer PowerShell; default %USERPROFILE%\brainflow
```

* Both clone tag 5.23.0, apply the patch, build Release and install `inc/` and `lib/` into the prefix.
* They set `-DBRAINFLOW_VERSION=5.23.0`; otherwise BrainFlow reports version 0.0.1.
* On Windows the script also sets `-DMSVC_RUNTIME=dynamic`, which linking BrainFlow next to Qt requires.
* On macOS, set `MACOSX_DEPLOYMENT_TARGET` (e.g. `13.0`) to run on older macOS than the build machine.

Check an install with `grep -n "patched (stream_gui_cpp)" <checkout>/src/board_controller/emotibit/emotibit.cpp`. To use another install, set `BRAINFLOW_ROOT` and run `scripts/build.sh --clean`. Re-apply the patch after every BrainFlow update.

`LICENSE` is BrainFlow's MIT licence; the packaged apps ship BrainFlow under it.
