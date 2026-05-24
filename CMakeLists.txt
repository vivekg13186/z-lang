cmake_minimum_required(VERSION 3.10)
project(z LANGUAGES C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

add_executable(z z.c)

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
