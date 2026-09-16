cmake_minimum_required(VERSION 3.21)
project(bioacq VERSION 1.0.0 LANGUAGES CXX)

# ---------------------------------------------------------------------------
# Native real-time streaming GUI: OpenBCI Cyton (serial) + EmotiBit (WiFi)
# via BrainFlow, rendered with plain QPainter widgets (Qt6 Widgets).
# Builds on macOS (Apple clang; Homebrew or official Qt) and Windows (MSVC
# 2022; official Qt). See README.md "Building and packaging".
# ---------------------------------------------------------------------------

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# Packaged builds (scripts/package_macos.sh, scripts/package_windows.ps1):
# record into <Documents>/BioAcq Recordings instead of the source tree, and on
# macOS build BioAcq.app. Development builds keep the plain `bioacq` binary.
option(BIOACQ_PACKAGED "Build for distribution (recordings in Documents, macOS app bundle)" OFF)
if(APPLE)
    option(BIOACQ_MACOS_BUNDLE "Build BioAcq.app instead of the plain bioacq binary" ${BIOACQ_PACKAGED})
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT MSVC)
    set(CMAKE_CXX_FLAGS_RELEASE "-O2 -DNDEBUG")
endif()
set(CMAKE_AUTOMOC ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ---------------------------------------------------------------- BrainFlow
# Patched 5.23.0 install (third_party/brainflow, scripts/build_brainflow.*),
# located directly under BRAINFLOW_ROOT: the installed brainflowConfig.cmake
# hard-codes the absolute prefix it was built for, which breaks relocated or
# cached installs. Library names: libBoardController.dylib on macOS,
# BoardController.lib + BoardController.dll on Windows.
if(WIN32)
    set(_bf_default "$ENV{USERPROFILE}/brainflow")
else()
    set(_bf_default "$ENV{HOME}/.local/brainflow")
endif()
file(TO_CMAKE_PATH "${_bf_default}" _bf_default)
set(BRAINFLOW_ROOT "${_bf_default}" CACHE PATH "BrainFlow install prefix")

if(APPLE AND EXISTS "/opt/homebrew/opt/qt")
    set(_qt_default "/opt/homebrew/opt/qt")
else()
    set(_qt_default "")
endif()
set(QT_PREFIX "${_qt_default}" CACHE PATH "Qt6 install prefix (empty: use CMAKE_PREFIX_PATH / Qt6_DIR)")
if(QT_PREFIX)
    list(APPEND CMAKE_PREFIX_PATH "${QT_PREFIX}")
endif()

find_package(Qt6 6.2 REQUIRED COMPONENTS Core Gui Widgets Network SerialPort)

set(brainflow_INCLUDE_DIRS "${BRAINFLOW_ROOT}/inc")
set(brainflow_LIBRARY_DIRS "${BRAINFLOW_ROOT}/lib")
if(NOT EXISTS "${brainflow_INCLUDE_DIRS}/board_shim.h")
    message(FATAL_ERROR "BrainFlow not found: no ${brainflow_INCLUDE_DIRS}/board_shim.h "
        "(set BRAINFLOW_ROOT, or build it with scripts/build_brainflow.sh / scripts/build_brainflow.ps1)")
endif()
foreach(_bf_lib BoardController DataHandler MLModule Brainflow)
    find_library(${_bf_lib}Path NAMES ${_bf_lib} lib${_bf_lib}
        PATHS "${brainflow_LIBRARY_DIRS}" NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH)
    if(NOT ${_bf_lib}Path)
        message(FATAL_ERROR "BrainFlow library ${_bf_lib} not found under ${brainflow_LIBRARY_DIRS}")
    endif()
endforeach()
message(STATUS "BrainFlow include: ${brainflow_INCLUDE_DIRS}")
message(STATUS "BrainFlow libs:    ${brainflow_LIBRARY_DIRS}")

# Default recording folder for development builds: <repo root>/recordings
# (gitignored). Packaged builds bake in nothing (see src/platform/AppPaths.cpp).
get_filename_component(_default_record_dir "${CMAKE_CURRENT_SOURCE_DIR}/recordings" ABSOLUTE)
set(BIOACQ_RECORD_DIR "${_default_record_dir}" CACHE PATH "Default folder for CSV recordings")
if(BIOACQ_PACKAGED)
    set(BIOACQ_RECORD_DIR "") # normal variable: shadows the cache entry for configure_file
endif()
configure_file(src/app/BuildConfig.h.in "${CMAKE_CURRENT_BINARY_DIR}/generated/BuildConfig.h" @ONLY)

add_executable(bioacq
    src/app/main.cpp
    src/core/Decimate.h
    src/core/RateMeter.h
    src/core/Readouts.h
    src/core/RingBuffer.h
    src/dsp/Biquad.h
    src/devices/DeviceWorker.cpp
    src/devices/DeviceWorker.h
    src/devices/EmotiBitDiscovery.cpp
    src/devices/EmotiBitDiscovery.h
    src/devices/Retimer.h
    src/devices/SignalSpec.cpp
    src/devices/SignalSpec.h
    src/platform/AppPaths.cpp
    src/platform/AppPaths.h
    src/platform/ProcessSetup.cpp
    src/platform/ProcessSetup.h
    src/platform/SerialPorts.cpp
    src/platform/SerialPorts.h
    src/platform/Sockets.h
    src/ui/MainWindow.cpp
    src/ui/MainWindow.h
    src/ui/PlotWidget.cpp
    src/ui/PlotWidget.h
    src/ui/Theme.cpp
    src/ui/Theme.h
    src/ui/Widgets.cpp
    src/ui/Widgets.h
    src/tools/Headless.cpp
    src/tools/Headless.h
)

# Embedded UI fonts (SIL Open Font License 1.1; the licence texts ship in the
# same resource bundle): JetBrains Mono + Inter, registered at startup.
# BASE keeps the resource paths at :/fonts/... (not :/resources/fonts/...).
qt_add_resources(bioacq "ui_fonts"
    PREFIX "/"
    BASE resources
    FILES
        resources/fonts/JetBrainsMono-Regular.ttf
        resources/fonts/JetBrainsMono-Medium.ttf
        resources/fonts/JetBrainsMono-SemiBold.ttf
        resources/fonts/JetBrainsMono-Bold.ttf
        resources/fonts/JetBrainsMono-OFL.txt
        resources/fonts/Inter-Regular.ttf
        resources/fonts/Inter-Medium.ttf
        resources/fonts/Inter-SemiBold.ttf
        resources/fonts/Inter-Bold.ttf
        resources/fonts/Inter-OFL.txt
)

target_include_directories(bioacq PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src/app"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/core"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/dsp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/devices"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/platform"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/ui"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/tools"
    "${CMAKE_CURRENT_BINARY_DIR}/generated"
)
# BrainFlow headers are third-party: mark as system to silence their warnings.
target_include_directories(bioacq SYSTEM PRIVATE ${brainflow_INCLUDE_DIRS})

target_link_libraries(bioacq PRIVATE
    Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Network Qt6::SerialPort
    # order matters on some platforms (mirrors BrainFlow's own examples)
    ${BrainflowPath}
    ${MLModulePath}
    ${DataHandlerPath}
    ${BoardControllerPath}
)

if(MSVC)
    # /utf-8: the sources contain UTF-8 literals (U+2212 minus, em dash, ...).
    # C4244 / C4267: implicit double->int and size_t->int narrowing (clang's -Wall is silent there too).
    # (CXX only: target options also reach rc.exe for the .rc file, which rejects them.)
    target_compile_options(bioacq PRIVATE
        "$<$<COMPILE_LANGUAGE:CXX>:/W3;/utf-8;/EHsc;/Zc:__cplusplus;/permissive-;/wd4244;/wd4267>")
else()
    target_compile_options(bioacq PRIVATE -Wall -Wextra -Wno-unused-parameter)
endif()

# ------------------------------------------------------------------ Windows
if(WIN32)
    configure_file(resources/windows/bioacq.rc.in "${CMAKE_CURRENT_BINARY_DIR}/generated/bioacq.rc" @ONLY)
    target_sources(bioacq PRIVATE
        src/platform/compat/win32/sys/resource.cpp
        src/platform/compat/win32/sys/resource.h
        "${CMAKE_CURRENT_BINARY_DIR}/generated/bioacq.rc"
    )
    # <sys/resource.h> stand-in for Readouts.h (getrusage via GetProcessTimes)
    target_include_directories(bioacq PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src/platform/compat/win32")
    target_compile_definitions(bioacq PRIVATE
        _USE_MATH_DEFINES NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS _WINSOCK_DEPRECATED_NO_WARNINGS)
    target_link_libraries(bioacq PRIVATE ws2_32 iphlpapi winmm)
    # GUI subsystem: a double-click opens no console window; the command-line
    # modes attach to the calling console (src/platform/ProcessSetup.cpp).
    set_target_properties(bioacq PROPERTIES
        OUTPUT_NAME BioAcq
        WIN32_EXECUTABLE ON
    )
    # BrainFlow's core DLLs next to BioAcq.exe so the build tree runs as is.
    # (Board-specific DLLs -- Ganglion, Muse, BrainBit, ... -- are loaded only
    # for those boards; Cyton, EmotiBit and the synthetic board need none.)
    foreach(_bf_dll BoardController DataHandler MLModule)
        add_custom_command(TARGET bioacq POST_BUILD
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "${brainflow_LIBRARY_DIRS}/${_bf_dll}.dll" "$<TARGET_FILE_DIR:bioacq>"
            VERBATIM)
    endforeach()
endif()

# -------------------------------------------------------------------- macOS
if(APPLE)
    # Run straight from the build tree without DYLD_* variables.
    set(_bioacq_rpath "${brainflow_LIBRARY_DIRS}")
    if(QT_PREFIX)
        list(APPEND _bioacq_rpath "${QT_PREFIX}/lib")
    endif()
    set_target_properties(bioacq PROPERTIES
        BUILD_RPATH "${_bioacq_rpath}"
        INSTALL_RPATH "${_bioacq_rpath}"
    )
    if(BIOACQ_MACOS_BUNDLE)
        # LSMinimumSystemVersion: the deployment target if one is set, else
        # Qt 6.10's floor. scripts/package_macos.sh rewrites it with the highest
        # minimum macOS of every binary it bundles.
        if(CMAKE_OSX_DEPLOYMENT_TARGET)
            set(BIOACQ_MACOS_MIN_VERSION "${CMAKE_OSX_DEPLOYMENT_TARGET}")
        else()
            set(BIOACQ_MACOS_MIN_VERSION "13.0")
        endif()
        set(BIOACQ_BUNDLE_ID "edu.umass.wssl.bioacq")
        set_target_properties(bioacq PROPERTIES
            OUTPUT_NAME BioAcq
            MACOSX_BUNDLE ON
            MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/resources/macos/Info.plist.in"
        )
        set(_icns "${CMAKE_CURRENT_SOURCE_DIR}/resources/macos/BioAcq.icns")
        if(EXISTS "${_icns}")
            target_sources(bioacq PRIVATE "${_icns}")
            set_source_files_properties("${_icns}" PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
        endif()
    endif()
endif()
