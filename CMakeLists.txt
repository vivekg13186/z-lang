cmake_minimum_required(VERSION 3.10)
project(z LANGUAGES C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

add_executable(z z.c)

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

set_target_properties(z PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/dist/${Z_PLATFORM}_${Z_ARCH}")

# Optional image module — set -DZ_WITH_IMAGE=ON when configuring.
option(Z_WITH_IMAGE "Build with image manipulation builtins (requires ImageMagick at runtime)" OFF)
if(Z_WITH_IMAGE)
    target_compile_definitions(z PRIVATE Z_WITH_IMAGE)
endif()

if(MSVC)
    target_compile_definitions(z PRIVATE _CRT_SECURE_NO_WARNINGS)
    target_compile_options(z PRIVATE /W3)
else()
    target_compile_options(z PRIVATE
        -O2 -Wall -Wextra
        -Wno-unused-parameter -Wno-unused-result)
    target_link_libraries(z PRIVATE m)
endif()

# Allow `cmake --install .` to drop the binary on POSIX systems.
include(GNUInstallDirs)
install(TARGETS z RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
