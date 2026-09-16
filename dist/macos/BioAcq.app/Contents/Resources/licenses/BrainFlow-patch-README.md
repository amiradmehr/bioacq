# BrainFlow (local patch)

bioacq builds against BrainFlow **5.23.0** (tag `5.23.0`, commit `7994b54b`) with one local fix.

## What the patch fixes

`emotibit-ancillary-upsample.patch` changes `src/board_controller/emotibit/emotibit.cpp`
(`Emotibit::read_thread`, around line 376). BrainFlow upsamples the EmotiBit ancillary preset 2×:
every parsed value is stored at `anc_packages[i * 2]` and should be duplicated into
`anc_packages[i * 2 + 1]`. Upstream writes the duplicate to `anc_packages[i + 1]` instead, which
overwrites newer temperature values with older ones, so the temperature trace shows stale,
duplicated samples. The patched line is marked `patched (stream_gui_cpp)` (the marker text is kept
as-is so existing installs can still be checked with the grep below).

## Build and install with the scripts

```bash
scripts/build_brainflow.sh [prefix]            # macOS / Linux, default prefix ~/.local/brainflow
```

```powershell
scripts\build_brainflow.ps1 [-Prefix <dir>]    # Windows, VS 2022 x64 developer PowerShell, default %USERPROFILE%\brainflow
```

Both clone tag `5.23.0` (checkout in `~/.local/src/brainflow-5.23.0` by default), check the commit, apply the
patch unless the marker is already there, build Release and install `inc/` + `lib/` into the prefix.

* macOS: set `MACOSX_DEPLOYMENT_TARGET` (e.g. `13.0`) for a BrainFlow that runs on older macOS than the build
  machine; `scripts/package_macos.sh` derives the app's `LSMinimumSystemVersion` from the bundled binaries.
* Windows: BrainFlow defaults to the static MSVC runtime (`/MT`). The script passes `-DMSVC_RUNTIME=dynamic`
  (`/MD`), because BrainFlow's static C++ binding (`Brainflow.lib`) is linked into `BioAcq.exe` together with Qt,
  which uses `/MD`. The DLLs are installed into `lib\` (`BoardController.dll`, `DataHandler.dll`, `MLModule.dll`).
* Both pass `-DBRAINFLOW_COPY_TO_PACKAGE_DIRS=OFF` (no copies into BrainFlow's language bindings) and
  `-DBRAINFLOW_VERSION=5.23.0` (otherwise `BoardShim::get_version ()` reports `0.0.1`).

`LICENSE` is BrainFlow's MIT licence; the packaged apps ship BrainFlow's libraries under it.

## Build and install by hand (macOS)

```bash
git clone https://github.com/brainflow-dev/brainflow.git ~/.local/src/brainflow
cd ~/.local/src/brainflow
git checkout 5.23.0                      # commit 7994b54b
git apply "<repo>/third_party/brainflow/emotibit-ancillary-upsample.patch"
grep -n "patched (stream_gui_cpp)" src/board_controller/emotibit/emotibit.cpp   # verify

cmake -S . -B build -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="$HOME/.local/brainflow"
cmake --build build --parallel
cmake --install build
```

bioacq's build looks for BrainFlow in `~/.local/brainflow` by default; point `BRAINFLOW_ROOT` elsewhere
(and run `scripts/build.sh --clean`) if you install it somewhere else.

Re-apply the patch after every BrainFlow rebuild or update.
