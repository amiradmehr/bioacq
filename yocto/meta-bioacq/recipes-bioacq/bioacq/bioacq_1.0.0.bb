SUMMARY = "BioAcq: real-time ECG (OpenBCI Cyton) and EmotiBit viewer"
DESCRIPTION = "Qt 6 Widgets viewer that streams an OpenBCI Cyton over a serial dongle and an EmotiBit \
over Bluetooth or Wi-Fi through BrainFlow, plots them and records BrainFlow CSV files."
HOMEPAGE = "https://github.com/amiradmehr/bioacq"
# In-house software, no public licence text in the repository. The third-party
# licences it ships (fonts, BrainFlow, Qt) are installed with the application.
LICENSE = "CLOSED"

# The repository is private, so the default fetch is over SSH with the builder's
# own key. For day-to-day work point the build at a checkout instead:
#   devtool modify bioacq
# or, in local.conf:
#   INHERIT += "externalsrc"
#   EXTERNALSRC:pn-bioacq = "/path/to/bioacq"
BIOACQ_GIT ?= "github.com/amiradmehr/bioacq.git;protocol=ssh;user=git"
BIOACQ_BRANCH ?= "main"
SRC_URI = "git://${BIOACQ_GIT};branch=${BIOACQ_BRANCH} \
           file://bioacq.service \
           file://99-bioacq-openbci.rules \
           "
SRCREV ?= "${AUTOREV}"
PV = "1.0.0+git"
S = "${WORKDIR}/git"

DEPENDS = "qtbase qtserialport qtconnectivity brainflow"

# UNPACKDIR is where file:// sources land from styhead on; older releases
# unpack them straight into WORKDIR.
UNPACKDIR ?= "${WORKDIR}"

inherit cmake_qt6 systemd

# BIOACQ_PACKAGED: recordings go to <Documents>/BioAcq Recordings instead of a
# folder inside the source tree, and no rpath is baked in (everything is in the
# image's own library path). BRAINFLOW_ROOT is the sysroot prefix the brainflow
# recipe installed into. CMAKE_INSTALL_DOCDIR keeps the third-party licence
# texts in the runtime package rather than in bioacq-doc.
EXTRA_OECMAKE = " \
    -DBIOACQ_PACKAGED=ON \
    -DBRAINFLOW_ROOT=${STAGING_DIR_HOST}${prefix} \
    -DCMAKE_INSTALL_DOCDIR=${datadir}/${BPN} \
"

do_install:append() {
    install -d ${D}${nonarch_base_libdir}/udev/rules.d
    install -m 0644 ${UNPACKDIR}/99-bioacq-openbci.rules ${D}${nonarch_base_libdir}/udev/rules.d/

    if ${@bb.utils.contains('DISTRO_FEATURES', 'systemd', 'true', 'false', d)}; then
        install -d ${D}${systemd_system_unitdir}
        install -m 0644 ${UNPACKDIR}/bioacq.service ${D}${systemd_system_unitdir}/
    fi
}

# Not enabled by default: a kiosk image turns it on with
# SYSTEMD_AUTO_ENABLE:pn-bioacq = "enable" (see ../../../README.md).
SYSTEMD_SERVICE:${PN} = "${@bb.utils.contains('DISTRO_FEATURES', 'systemd', 'bioacq.service', '', d)}"
SYSTEMD_AUTO_ENABLE ?= "disable"

FILES:${PN} += " \
    ${datadir}/icons \
    ${nonarch_base_libdir}/udev/rules.d \
"

# Qt platform plugins (xcb / wayland / eglfs / linuxfb live in qtbase-plugins),
# the serial-port backend's udev, and BlueZ when the image has Bluetooth.
RDEPENDS:${PN} += "qtbase-plugins brainflow"
RRECOMMENDS:${PN} += " \
    ${@bb.utils.contains('DISTRO_FEATURES', 'wayland', 'qtwayland', '', d)} \
    ${@bb.utils.contains('DISTRO_FEATURES', 'bluetooth', 'bluez5', '', d)} \
"
