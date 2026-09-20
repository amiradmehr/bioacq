SUMMARY = "BrainFlow: one API for biosensor data acquisition"
DESCRIPTION = "BrainFlow 5.23.0 with the EmotiBit ancillary-upsampling fix BioAcq needs \
(third_party/brainflow/emotibit-ancillary-upsample.patch in the bioacq repository): without it \
the EmotiBit temperature repeats stale samples."
HOMEPAGE = "https://brainflow.org"
BUGTRACKER = "https://github.com/brainflow-dev/brainflow/issues"
LICENSE = "MIT"
LIC_FILES_CHKSUM = "file://LICENSE;md5=79b3d736c93ef8022f9b1b2826a71a5f"

SRC_URI = "git://github.com/brainflow-dev/brainflow.git;protocol=https;nobranch=1 \
           file://emotibit-ancillary-upsample.patch \
           "
# tag 5.23.0
SRCREV = "7994b54b4ee23898456d96446dc7e51ebfa18840"
S = "${WORKDIR}/git"

inherit cmake

# BRAINFLOW_COPY_TO_PACKAGE_DIRS copies the built libraries into the Python and
# Java packages in the source tree, which a Yocto build has no use for.
# CMAKE_POLICY_VERSION_MINIMUM: 5.23.0 predates CMake 4's policy floor.
EXTRA_OECMAKE = " \
    -DBRAINFLOW_VERSION=${PV} \
    -DBRAINFLOW_COPY_TO_PACKAGE_DIRS=OFF \
    -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
"

# Upstream installs headers into <prefix>/inc and libraries into <prefix>/lib,
# which is neither ${includedir} nor a multilib ${libdir}. Move both into the
# usual places; CMakeLists.txt in bioacq looks for include/brainflow as well as
# inc, so BRAINFLOW_ROOT=${prefix} finds them either way.
do_install:append() {
    if [ -d ${D}${prefix}/inc ]; then
        install -d ${D}${includedir}/brainflow
        mv ${D}${prefix}/inc/* ${D}${includedir}/brainflow/
        rmdir ${D}${prefix}/inc
    fi
    if [ "${prefix}/lib" != "${libdir}" ] && [ -d ${D}${prefix}/lib ]; then
        install -d ${D}${libdir}
        mv ${D}${prefix}/lib/* ${D}${libdir}/
        rmdir ${D}${prefix}/lib
    fi
}

# The libraries carry no SONAME version (libBoardController.so and friends are
# what the application links against and loads), so they belong in the runtime
# package, not in -dev.
FILES:${PN} += "${libdir}/lib*.so"
FILES:${PN}-dev = "${includedir}"
INSANE_SKIP:${PN} += "dev-so"

BBCLASSEXTEND = "native nativesdk"
