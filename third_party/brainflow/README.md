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

## Build and install (macOS)

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
