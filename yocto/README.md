# BioAcq on Yocto

`meta-bioacq` builds BioAcq and its patched BrainFlow into a Yocto image. Qt
comes from meta-qt6, so nothing is bundled: the application is a normal package.

Tested layer series: scarthgap and newer (the recipes also declare kirkstone).
The recipes have not been run through CI yet — build them once for your machine
before relying on them in a release image.

## Add the layer

```sh
bitbake-layers add-layer /path/to/bioacq/yocto/meta-bioacq
```

It needs `meta` (poky) and [`meta-qt6`](https://code.qt.io/yocto/meta-qt6.git)
on a matching branch. In `local.conf`:

```
DISTRO_FEATURES:append = " bluetooth wayland opengl"
IMAGE_INSTALL:append = " bioacq"
```

`bluetooth` is what the EmotiBit's default link needs (it pulls in BlueZ and
qtconnectivity's BlueZ backend). Drop it if you only use the EmotiBit over
Wi-Fi. `wayland` / `opengl` decide which Qt platform plugins exist; a plain
framebuffer image needs neither.

## Build

```sh
bitbake bioacq          # the package
bitbake core-image-weston   # or whatever image you install it into
```

The repository is private, so the default fetch is `git://…;protocol=ssh` with
the builder's own key. To build from a checkout instead:

```
INHERIT += "externalsrc"
EXTERNALSRC:pn-bioacq = "/path/to/bioacq"
```

or `devtool modify bioacq`, which is the better choice while you are changing
the source.

## Run it on the board

```sh
bioacq                       # under a desktop session
QT_QPA_PLATFORM=eglfs bioacq # no session, straight to the display
QT_QPA_PLATFORM=linuxfb bioacq
bioacq --selftest            # headless end-to-end test, no hardware
bioacq --list-ports          # serial ports, OpenBCI dongles first
```

The package ships a systemd unit for single-purpose images. It is installed but
not enabled:

```sh
systemctl enable --now bioacq
```

Enable it in the image instead with `SYSTEMD_AUTO_ENABLE:pn-bioacq = "enable"`,
and change the platform plugin or the user with `systemctl edit bioacq.service`.

## Devices

- **Cyton dongle** — the udev rule in the package (`99-bioacq-openbci.rules`)
  gives the FTDI FT231X mode 0660, group `dialout`, and the stable name
  `/dev/openbci-dongle`. Run BioAcq as root or as a user in `dialout`.
- **EmotiBit over Bluetooth** — needs `bluetoothd` running and the adapter
  unblocked (`rfkill unblock bluetooth`), plus the bioacq BLE firmware on the
  EmotiBit.
- **EmotiBit over Wi-Fi** — `bioacq --emotibit-link wifi`. The board and the
  EmotiBit must reach each other; broadcast discovery only works on one subnet.

## Recordings

`<Documents>/BioAcq Recordings` for the user running it (`/home/root/Documents/…`
under the shipped unit), or `--record-dir <dir>`. On an image with a read-only
or tiny rootfs, point it at the data partition.
