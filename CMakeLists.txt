cmake_minimum_required(VERSION 3.10)
project(z LANGUAGES C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

add_executable(z z.c)
add_executable(zide zide.c)

# Match the Makefile's dist/<os>_<arch>/ layout.
if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Windows")
    set(Z_PLATFORM "windows")
elseif(CMAKE_HOST_SYSTEM_NAME STREQUAL "Darwin")
    set(Z_PLATFORM "macos")
else()
    set(Z_PLATFORM "linux")
endif()

if(CMAKE_HOST_SYSTEM_PROCESSOR MATCHES "(x86_64|amd64|AMD64)")
    set(Z_ARCH "x86")
elseif(CMAKE_HOST_SYSTEM_PROCESSOR MATCHES "(aarch64|arm64|ARM64)")
    set(Z_ARCH "arm64")
elseif(CMAKE_HOST_SYSTEM_PROCESSOR MATCHES "(i686|i386)")
    set(Z_ARCH "x86_32")
else()
    set(Z_ARCH "${CMAKE_HOST_SYSTEM_PROCESSOR}")
endif()

set_target_properties(z zide PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/dist/${Z_PLATFORM}_${Z_ARCH}")

# Optional image module — set -DZ_WITH_IMAGE=ON when configuring.
option(Z_WITH_IMAGE "Build with image manipulation builtins (requires ImageMagick at runtime)" OFF)
if(Z_WITH_IMAGE)
    target_compile_definitions(z    PRIVATE Z_WITH_IMAGE)
    target_compile_definitions(zide PRIVATE Z_WITH_IMAGE)
endif()

foreach(tgt z zide)
    if(MSVC)
        target_compile_definitions(${tgt} PRIVATE _CRT_SECURE_NO_WARNINGS)
        target_compile_options(${tgt} PRIVATE /W3)
    else()
        target_compile_options(${tgt} PRIVATE
            -O2 -Wall -Wextra
            -Wno-unused-parameter -Wno-unused-result)
        target_link_libraries(${tgt} PRIVATE m)
    endif()
endforeach()

# Allow `cmake --install .` to drop both binaries on POSIX systems.
include(GNUInstallDirs)
install(TARGETS z zide RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
