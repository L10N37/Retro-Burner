cmake_minimum_required(VERSION 3.24)
project(RetroBurnerLinuxDriveProbe LANGUAGES CXX)

if(WIN32)
    message(FATAL_ERROR "This probe validates only the Linux drive-manager implementation.")
endif()

add_executable(retroburner_drive_probe
    drive_probe_main.cpp
    ../../src/drive_manager_linux.cpp
)

target_include_directories(retroburner_drive_probe PRIVATE ../../src)
target_compile_features(retroburner_drive_probe PRIVATE cxx_std_20)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(retroburner_drive_probe PRIVATE
        -Wall -Wextra -Wpedantic
    )
endif()
