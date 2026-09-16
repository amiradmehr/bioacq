# Qt

BioAcq is built with the Qt 6 libraries, which are used under the **GNU Lesser General Public License version 3**
(`LGPL-3.0.txt`; the LGPL incorporates the GNU GPL version 3, `GPL-3.0.txt`). Copyright (C) The Qt Company Ltd. and
other contributors.

* Modules: Qt Core, Gui, Widgets, Network and Serial Port, plus Qt D-Bus as a dependency on macOS, and the platform and
  style plugins the app loads.
* Qt is linked dynamically and shipped unmodified: the `Qt6*.dll` files and `plugins` folders next to `BioAcq.exe`, and
  the `Qt*.framework` bundles and `PlugIns` in `BioAcq.app/Contents`. They can be replaced with another build of the
  same Qt version.
* Source code: the Qt release the package was built with, from <https://download.qt.io/official_releases/qt/> or
  <https://code.qt.io/>. The Windows CI package and `macos.yml` use the official Qt 6.10 binaries; a local
  `scripts/package_macos.sh` build uses Homebrew's Qt (<https://formulae.brew.sh/formula/qt>).
* Qt contains third-party components under their own licences, listed at
  <https://doc.qt.io/qt-6/licenses-used-in-qt.html>. A Homebrew-based `BioAcq.app` also bundles the Homebrew libraries
  that Qt links against; their licence files and SPDX identifiers are copied into
  `BioAcq.app/Contents/Resources/licenses/homebrew/`.

The packaging scripts copy this file (as `Qt-NOTICE.md`) and both licence texts into the package's `licenses` folder.
