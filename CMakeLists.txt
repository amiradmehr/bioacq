cmake_minimum_required(VERSION 3.21)
project(bioacq VERSION 1.0.0 LANGUAGES CXX)

if(POLICY CMP0144)
    cmake_policy(SET CMP0144 NEW) # honour BRAINFLOW_ROOT as a search root
endif()

# ---------------------------------------------------------------------------
# Native real-time streaming GUI: OpenBCI Cyton (serial) + EmotiBit (WiFi)
# via BrainFlow, rendered with plain QPainter widgets (Qt6 Widgets).
# ---------------------------------------------------------------------------

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_CXX_FLAGS_RELEASE "-O2 -DNDEBUG")
set(CMAKE_AUTOMOC ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(BRAINFLOW_ROOT "$ENV{HOME}/.local/brainflow" CACHE PATH "BrainFlow install prefix")
set(QT_PREFIX "/opt/homebrew/opt/qt" CACHE PATH "Qt6 install prefix")
list(APPEND CMAKE_PREFIX_PATH "${QT_PREFIX}" "${BRAINFLOW_ROOT}")

find_package(Qt6 6.2 REQUIRED COMPONENTS Core Gui Widgets Network)
find_package(brainflow CONFIG REQUIRED)

if(NOT BoardControllerPath OR NOT BrainflowPath)
    message(FATAL_ERROR "BrainFlow libraries not found under ${BRAINFLOW_ROOT}/lib")
endif()
message(STATUS "BrainFlow include: ${brainflow_INCLUDE_DIRS}")
message(STATUS "BrainFlow libs:    ${brainflow_LIBRARY_DIRS}")

# Default recording folder: <repo root>/recordings (gitignored)
get_filename_component(_default_record_dir "${CMAKE_CURRENT_SOURCE_DIR}/recordings" ABSOLUTE)
set(BIOACQ_RECORD_DIR "${_default_record_dir}" CACHE PATH "Default folder for CSV recordings")
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
    "${CMAKE_CURRENT_SOURCE_DIR}/src/ui"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/tools"
    "${CMAKE_CURRENT_BINARY_DIR}/generated"
)
# BrainFlow headers are third-party: mark as system to silence their warnings.
target_include_directories(bioacq SYSTEM PRIVATE ${brainflow_INCLUDE_DIRS})

target_link_libraries(bioacq PRIVATE
    Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Network
    # order matters on some platforms (mirrors BrainFlow's own examples)
    ${BrainflowPath}
    ${MLModulePath}
    ${DataHandlerPath}
    ${BoardControllerPath}
)

target_compile_options(bioacq PRIVATE -Wall -Wextra -Wno-unused-parameter)

# Run straight from the build tree without DYLD_* variables.
set_target_properties(bioacq PROPERTIES
    BUILD_RPATH "${brainflow_LIBRARY_DIRS};${QT_PREFIX}/lib"
    INSTALL_RPATH "${brainflow_LIBRARY_DIRS};${QT_PREFIX}/lib"
)
