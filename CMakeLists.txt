cmake_minimum_required(VERSION 3.18)
project(PrimeSenseOpenNI2WinUSB NONE)

if(NOT WIN32)
    message(FATAL_ERROR "The WinUSB transport build is supported on Windows only.")
endif()

find_program(POWERSHELL_EXECUTABLE NAMES powershell.exe powershell)
if(NOT POWERSHELL_EXECUTABLE)
    message(FATAL_ERROR "PowerShell is required to build the WinUSB plugin.")
endif()

set(OPENNI2_BUILD_DIR "${CMAKE_BINARY_DIR}" CACHE PATH
    "Build directory used for the pinned OpenNI2 checkout")
set(OPENNI2_DRIVER_OUTPUT_DIR "${CMAKE_BINARY_DIR}/OpenNI2/Drivers" CACHE PATH
    "Destination for the custom PS1080 plugin")

add_custom_target(build_winusb_openni
    COMMAND "${POWERSHELL_EXECUTABLE}" -NoProfile -ExecutionPolicy Bypass
        -File "${CMAKE_CURRENT_SOURCE_DIR}/Build-OpenNI2-WinUSB.ps1"
        -BuildDir "${OPENNI2_BUILD_DIR}"
        -OutputDriverDir "${OPENNI2_DRIVER_OUTPUT_DIR}"
    VERBATIM
)
