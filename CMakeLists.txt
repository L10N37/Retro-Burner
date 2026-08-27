cmake_minimum_required(VERSION 3.24)

project(RetroBurner VERSION 0.4.0 LANGUAGES C CXX)

# The Windows resource compiler is only required for the native Win32 GUI.
# Keeping RC out of the project language list lets the same top-level CMake
# configure on Linux without changing the Windows build.
if(WIN32)
    enable_language(RC)
endif()

if(NOT WIN32)
    # Native Linux application bring-up.
    #
    # Windows continues through the original Win32/DX11 build below. Linux
    # uses SDL3 + OpenGL3 and the sysfs optical drive-manager implementation.
    set(IMGUI_DIR "${CMAKE_CURRENT_SOURCE_DIR}/external/imgui")

    foreach(required_imgui_file
            imgui.cpp
            imgui_draw.cpp
            imgui_tables.cpp
            imgui_widgets.cpp
            backends/imgui_impl_sdl3.cpp
            backends/imgui_impl_sdl3.h
            backends/imgui_impl_opengl3.cpp
            backends/imgui_impl_opengl3.h
            backends/imgui_impl_opengl3_loader.h)
        if(NOT EXISTS "${IMGUI_DIR}/${required_imgui_file}")
            message(FATAL_ERROR
                "Missing Dear ImGui Linux file: ${IMGUI_DIR}/${required_imgui_file}")
        endif()
    endforeach()

    find_package(OpenGL REQUIRED)
    find_package(SDL3_image CONFIG REQUIRED)

    if(TARGET SDL3_image::SDL3_image)
        set(RB_SDL3_IMAGE_TARGET SDL3_image::SDL3_image)
    elseif(TARGET SDL3_image::SDL3_image-shared)
        set(RB_SDL3_IMAGE_TARGET SDL3_image::SDL3_image-shared)
    else()
        message(FATAL_ERROR "SDL3_image CMake target not found")
    endif()
    find_package(Threads REQUIRED)

    find_package(SDL3 CONFIG QUIET)
    if(TARGET SDL3::SDL3)
        set(RB_SDL3_TARGET SDL3::SDL3)
    else()
        find_package(PkgConfig REQUIRED)
        pkg_check_modules(SDL3 REQUIRED IMPORTED_TARGET sdl3)
        set(RB_SDL3_TARGET PkgConfig::SDL3)
    endif()

    add_library(imgui_linux STATIC
        "${IMGUI_DIR}/imgui.cpp"
        "${IMGUI_DIR}/imgui_draw.cpp"
        "${IMGUI_DIR}/imgui_tables.cpp"
        "${IMGUI_DIR}/imgui_widgets.cpp"
        "${IMGUI_DIR}/backends/imgui_impl_sdl3.cpp"
        "${IMGUI_DIR}/backends/imgui_impl_opengl3.cpp"
    )

    target_include_directories(imgui_linux PUBLIC
        "${IMGUI_DIR}"
        "${IMGUI_DIR}/backends"
    )

    target_link_libraries(imgui_linux PUBLIC
        ${RB_SDL3_TARGET}
        ${RB_SDL3_IMAGE_TARGET}
        OpenGL::GL
    )

    target_compile_features(imgui_linux PUBLIC cxx_std_20)

    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(imgui_linux PRIVATE
            -Wall
            -Wextra
            -Wpedantic
        )
    endif()

    add_library(retroburner_drive_manager STATIC
        src/drive_manager_linux.cpp
        src/drive_manager.h
    )

    target_include_directories(retroburner_drive_manager PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    target_compile_features(retroburner_drive_manager PUBLIC cxx_std_20)

    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(retroburner_drive_manager PRIVATE
            -Wall
            -Wextra
            -Wpedantic
        )
    endif()

    add_executable(RetroBurner
        src/main_linux.cpp
        src/process_runner_linux.cpp
        src/process_runner.h
        src/retrobeam_linux.cpp
        src/retrobeam_linux.h
        src/optical_verify_linux.cpp
        src/optical_verify_linux.h
        src/burn_engine_linux.cpp
        src/burnermax_linux.cpp
        src/burnermax.h
        src/burn_engine.h
        src/texture_loader_linux.cpp
        src/texture_loader_linux.h
    
    src/retrobeam_progress.cpp
    src/retrobeam_progress.h)

    target_link_libraries(RetroBurner PRIVATE
        imgui_linux
        retroburner_drive_manager
        ${RB_SDL3_TARGET}
        OpenGL::GL
        Threads::Threads
    )

    target_compile_features(RetroBurner PRIVATE cxx_std_20)

    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(RetroBurner PRIVATE
            -Wall
            -Wextra
            -Wpedantic
        )
    endif()

    set_target_properties(RetroBurner PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
        OUTPUT_NAME "RetroBurner"
    )

    add_custom_command(TARGET RetroBurner POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_SOURCE_DIR}/assets"
            "$<TARGET_FILE_DIR:RetroBurner>/assets"
        COMMENT "Copying Retro Burner artwork/assets for Linux runtime"
    )

    # Keep the text probe available as a small diagnostics target, but the
    # native GUI is now the primary Linux application target.
    add_executable(RetroBurnerDriveProbe EXCLUDE_FROM_ALL
        cmake/retroburner-drive-probe/drive_probe_main.cpp
    )

    target_link_libraries(RetroBurnerDriveProbe PRIVATE
        retroburner_drive_manager
    )

    set_target_properties(RetroBurnerDriveProbe PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
        OUTPUT_NAME "RetroBurnerDriveProbe"
    )

    install(TARGETS RetroBurner RUNTIME DESTINATION bin)

    message(STATUS
        "Retro Burner Linux: native SDL3/OpenGL3 application target enabled; "
        "Windows continues to use Win32/DX11.")

    # Do not parse the Windows-only resources, embedded PE tools, SPTI probes,
    # or Direct3D targets below when configuring on Linux.

# BEGIN RETROBURNER_LINUX_SINGLE_FILE
if(UNIX AND NOT APPLE)
    if(NOT DEFINED RETROBURNER_EMBED_RETROBEAM OR
       NOT EXISTS "${RETROBURNER_EMBED_RETROBEAM}")
        message(FATAL_ERROR
            "RETROBURNER_EMBED_RETROBEAM must point to the freshly-built native Linux RetroBeam binary")
    endif()

    if(NOT DEFINED RETROBURNER_EMBED_ABGX360 OR
       NOT EXISTS "${RETROBURNER_EMBED_ABGX360}")
        message(FATAL_ERROR
            "RETROBURNER_EMBED_ABGX360 must point to the freshly-built native Linux ABGX360 binary")
    endif()

    if(NOT DEFINED RETROBURNER_EMBED_CDIRIP OR
       NOT EXISTS "${RETROBURNER_EMBED_CDIRIP}")
        message(FATAL_ERROR
            "RETROBURNER_EMBED_CDIRIP must point to the freshly-built native Linux cdirip binary")
    endif()

    find_package(Python3 REQUIRED COMPONENTS Interpreter)

    set(RETROBURNER_EMBED_GENERATED_DIR
        "${CMAKE_CURRENT_BINARY_DIR}/generated")
    set(RETROBURNER_EMBED_HEADER
        "${RETROBURNER_EMBED_GENERATED_DIR}/embedded_bundle_linux.h")
    set(RETROBURNER_EMBED_SOURCE
        "${RETROBURNER_EMBED_GENERATED_DIR}/embedded_bundle_linux.cpp")

    add_custom_command(
        OUTPUT
            "${RETROBURNER_EMBED_HEADER}"
            "${RETROBURNER_EMBED_SOURCE}"
        COMMAND
            "${Python3_EXECUTABLE}"
            "${CMAKE_SOURCE_DIR}/scripts/internal/generate-linux-embedded-bundle.py"
            --header "${RETROBURNER_EMBED_HEADER}"
            --source "${RETROBURNER_EMBED_SOURCE}"
            --asset "BurningDC.png=${CMAKE_SOURCE_DIR}/assets/BurningDC.png"
            --asset "BurningPS1.png=${CMAKE_SOURCE_DIR}/assets/BurningPS1.png"
            --asset "BurningPS2.png=${CMAKE_SOURCE_DIR}/assets/BurningPS2.png"
            --asset "BurningSaturn.png=${CMAKE_SOURCE_DIR}/assets/BurningSaturn.png"
            --asset "BurningXB360.png=${CMAKE_SOURCE_DIR}/assets/BurningXB360.png"
            --asset "makeSpinDC.png=${CMAKE_SOURCE_DIR}/assets/makeSpinDC.png"
            --asset "RetroBurner.png=${CMAKE_SOURCE_DIR}/assets/RetroBurner.png"
            --asset "BurnComplete.wav=${CMAKE_SOURCE_DIR}/assets/mixkit-cartoon-monkey-mocking-laugh-107.wav"
            --tool "retrobeam=${RETROBURNER_EMBED_RETROBEAM}"
            --tool "abgx360=${RETROBURNER_EMBED_ABGX360}"
            --tool "cdirip=${RETROBURNER_EMBED_CDIRIP}"
        DEPENDS
            "${CMAKE_SOURCE_DIR}/scripts/internal/generate-linux-embedded-bundle.py"
            "${CMAKE_SOURCE_DIR}/assets/BurningDC.png"
            "${CMAKE_SOURCE_DIR}/assets/BurningPS1.png"
            "${CMAKE_SOURCE_DIR}/assets/BurningPS2.png"
            "${CMAKE_SOURCE_DIR}/assets/BurningSaturn.png"
            "${CMAKE_SOURCE_DIR}/assets/BurningXB360.png"
            "${CMAKE_SOURCE_DIR}/assets/makeSpinDC.png"
            "${CMAKE_SOURCE_DIR}/assets/RetroBurner.png"
            "${CMAKE_SOURCE_DIR}/assets/mixkit-cartoon-monkey-mocking-laugh-107.wav"
            "${RETROBURNER_EMBED_RETROBEAM}"
            "${RETROBURNER_EMBED_ABGX360}"
            "${RETROBURNER_EMBED_CDIRIP}"
        VERBATIM
        COMMENT
            "Embedding Retro Burner Linux assets and native helper executables")

    target_sources(
        RetroBurner
        PRIVATE
            "${RETROBURNER_EMBED_SOURCE}")

    target_include_directories(
        RetroBurner
        PRIVATE
            "${RETROBURNER_EMBED_GENERATED_DIR}")
endif()
# END RETROBURNER_LINUX_SINGLE_FILE

    return()
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# /MT and /MTd keep the finished program independent of the VC++ redistributable.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

set(IMGUI_DIR "${CMAKE_CURRENT_SOURCE_DIR}/external/imgui")
set(CDIRIP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/external/cdirip")
if(NOT EXISTS "${IMGUI_DIR}/imgui.cpp")
    message(FATAL_ERROR
        "Dear ImGui is missing. Run scripts/bootstrap-deps.ps1 before configuring.")
endif()

add_library(imgui STATIC
    "${IMGUI_DIR}/imgui.cpp"
    "${IMGUI_DIR}/imgui_draw.cpp"
    "${IMGUI_DIR}/imgui_tables.cpp"
    "${IMGUI_DIR}/imgui_widgets.cpp"
    "${IMGUI_DIR}/backends/imgui_impl_dx11.cpp"
    "${IMGUI_DIR}/backends/imgui_impl_win32.cpp"
)

set_target_properties(imgui PROPERTIES
    ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"
    ARCHIVE_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}/lib"
    ARCHIVE_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/lib"
    ARCHIVE_OUTPUT_DIRECTORY_RELWITHDEBINFO "${CMAKE_BINARY_DIR}/lib"
    ARCHIVE_OUTPUT_DIRECTORY_MINSIZEREL "${CMAKE_BINARY_DIR}/lib"
)

target_include_directories(imgui PUBLIC
    "${IMGUI_DIR}"
    "${IMGUI_DIR}/backends"
)

target_compile_definitions(imgui PRIVATE
    UNICODE
    _UNICODE
    _WIN32_WINNT=0x0A00
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)

add_executable(cdirip
    "${CDIRIP_DIR}/audio.c"
    "${CDIRIP_DIR}/buffer.c"
    "${CDIRIP_DIR}/cdi.c"
    "${CDIRIP_DIR}/cdirip.c"
    "${CDIRIP_DIR}/common.c"
)

target_include_directories(cdirip PRIVATE "${CDIRIP_DIR}")
target_compile_definitions(cdirip PRIVATE
    _CRT_SECURE_NO_WARNINGS
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)
target_link_libraries(cdirip PRIVATE comdlg32 shell32 ole32)
if(MSVC)
    target_compile_options(cdirip PRIVATE /W3 /utf-8)
endif()


set(CDIRIP_EMBED_DIR "${CMAKE_CURRENT_SOURCE_DIR}/tools/.embedded")
file(MAKE_DIRECTORY "${CDIRIP_EMBED_DIR}")
set_target_properties(cdirip PROPERTIES
    OUTPUT_NAME "cdirip"
    RUNTIME_OUTPUT_DIRECTORY "${CDIRIP_EMBED_DIR}"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CDIRIP_EMBED_DIR}"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CDIRIP_EMBED_DIR}"
    RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO "${CDIRIP_EMBED_DIR}"
    RUNTIME_OUTPUT_DIRECTORY_MINSIZEREL "${CDIRIP_EMBED_DIR}"
)
add_executable(RetroBurner WIN32
    src/main.cpp
    src/burn_engine.cpp
    src/burn_engine.h
    src/burnermax.cpp
    src/burnermax.h
    src/drive_manager.cpp
    src/drive_manager.h
    src/embedded_tools.cpp
    src/embedded_tools.h
    src/texture_loader.cpp
    src/texture_loader.h
    src/resource.h
    src/RetroBurner.rc
)


set_source_files_properties(src/RetroBurner.rc PROPERTIES
    OBJECT_DEPENDS
        "${CDIRIP_EMBED_DIR}/cdirip.exe;${CMAKE_CURRENT_SOURCE_DIR}/tools/retrobeam-dev/retrobeam.exe;${CMAKE_CURRENT_SOURCE_DIR}/tools/growisofs.exe;${CMAKE_CURRENT_SOURCE_DIR}/tools/dvd+rw-mediainfo.exe;${CMAKE_CURRENT_SOURCE_DIR}/tools/abgx360.exe;${CMAKE_CURRENT_SOURCE_DIR}/assets/BurningXB360.png;${CMAKE_CURRENT_SOURCE_DIR}/assets/mixkit-cartoon-monkey-mocking-laugh-107.wav;${CMAKE_CURRENT_SOURCE_DIR}/THIRD_PARTY.md;${CDIRIP_DIR}/LICENSE;${CMAKE_CURRENT_SOURCE_DIR}/licenses/cdrecord-cddl.txt;${CMAKE_CURRENT_SOURCE_DIR}/licenses/cdrecord-gpl3.txt;${CMAKE_CURRENT_SOURCE_DIR}/licenses/cygwin-GPL-2.0.txt;${CMAKE_CURRENT_SOURCE_DIR}/licenses/dvd-rw-tools-GPL-2.0.txt;${CMAKE_CURRENT_SOURCE_DIR}/licenses/ABGX360-GPL-2.0.txt;${CMAKE_CURRENT_SOURCE_DIR}/licenses/Mixkit-Sound-Effects-NOTICE.txt;${CMAKE_CURRENT_SOURCE_DIR}/licenses/BurnerMAX-NOTICE.txt;${IMGUI_DIR}/LICENSE.txt"
)
target_compile_definitions(RetroBurner PRIVATE
    UNICODE
    _UNICODE
    _WIN32_WINNT=0x0A00
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)

if(MSVC)
    target_compile_options(RetroBurner PRIVATE
        /W4
/WX
        /permissive-
        /utf-8
        /EHsc
        /Zc:__cplusplus
    )
    # RetroBurner.rc already embeds the requested-execution and DPI manifest.
    target_link_options(RetroBurner PRIVATE /MANIFEST:NO)
endif()

target_link_libraries(RetroBurner PRIVATE
    imgui
    d3d11
    dxgi
    windowscodecs
    ole32
    shell32
    winmm
)

add_dependencies(RetroBurner cdirip)
set_property(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" PROPERTY VS_STARTUP_PROJECT RetroBurner)

install(TARGETS RetroBurner RUNTIME DESTINATION .)
# RB_BURNERMAX_READONLY_LAB_35
# Read-only vendor-register research utility. This target is deliberately
# separate from RetroBurner and contains no vendor-register write path.
add_executable(RetroBurnerBurnerMaxProbe EXCLUDE_FROM_ALL
    src/burnermax_probe_cli.cpp
)

target_compile_definitions(RetroBurnerBurnerMaxProbe PRIVATE
    UNICODE
    _UNICODE
    _WIN32_WINNT=0x0A00
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)

if(MSVC)
    target_compile_options(RetroBurnerBurnerMaxProbe PRIVATE
        /W4
        /permissive-
        /utf-8
        /EHsc
        /Zc:__cplusplus
    )
endif()

set_target_properties(RetroBurnerBurnerMaxProbe PROPERTIES
    OUTPUT_NAME "retroburner-burnermax-probe"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/tools/burnermax-dev"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_CURRENT_SOURCE_DIR}/tools/burnermax-dev"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_CURRENT_SOURCE_DIR}/tools/burnermax-dev"
    RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO "${CMAKE_CURRENT_SOURCE_DIR}/tools/burnermax-dev"
    RUNTIME_OUTPUT_DIRECTORY_MINSIZEREL "${CMAKE_CURRENT_SOURCE_DIR}/tools/burnermax-dev"
)
# RB_ASUS_VENDOR_FAMILY_PROBE_36
# Finite DATA-IN-only command-family probe based on published QPxTool
# ASUS/Lite-On query commands. No vendor write path exists in this target.
add_executable(RetroBurnerAsusFamilyProbe EXCLUDE_FROM_ALL
    src/asus_vendor_family_probe_cli.cpp
)

target_compile_definitions(RetroBurnerAsusFamilyProbe PRIVATE
    UNICODE
    _UNICODE
    _WIN32_WINNT=0x0A00
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)

if(MSVC)
    target_compile_options(RetroBurnerAsusFamilyProbe PRIVATE
        /W4
        /permissive-
        /utf-8
        /EHsc
        /Zc:__cplusplus
    )
endif()

set_target_properties(RetroBurnerAsusFamilyProbe PROPERTIES
    OUTPUT_NAME "retroburner-asus-family-probe"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/tools/burnermax-dev"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_CURRENT_SOURCE_DIR}/tools/burnermax-dev"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_CURRENT_SOURCE_DIR}/tools/burnermax-dev"
    RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO "${CMAKE_CURRENT_SOURCE_DIR}/tools/burnermax-dev"
    RUNTIME_OUTPUT_DIRECTORY_MINSIZEREL "${CMAKE_CURRENT_SOURCE_DIR}/tools/burnermax-dev"
)
# RB_ASUS_SHARED_BUFFER_PROBE_79
# Read-only discriminator: tests whether DF/02/09 exposes a shared firmware
# response buffer by bracketing it with standard MMC DATA-IN queries.
add_executable(RetroBurnerAsusSharedBufferProbe EXCLUDE_FROM_ALL
    src/asus_shared_buffer_probe_cli.cpp
)

target_compile_definitions(RetroBurnerAsusSharedBufferProbe PRIVATE
    UNICODE
    _UNICODE
    _WIN32_WINNT=0x0A00
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)

if(MSVC)
    target_compile_options(RetroBurnerAsusSharedBufferProbe PRIVATE
        /W4
        /permissive-
        /utf-8
        /EHsc
        /Zc:__cplusplus
    )
endif()

set_target_properties(RetroBurnerAsusSharedBufferProbe PROPERTIES
    OUTPUT_NAME "retroburner-asus-shared-buffer-probe"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/tools/burnermax-dev"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_CURRENT_SOURCE_DIR}/tools/burnermax-dev"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_CURRENT_SOURCE_DIR}/tools/burnermax-dev"
    RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO "${CMAKE_CURRENT_SOURCE_DIR}/tools/burnermax-dev"
    RUNTIME_OUTPUT_DIRECTORY_MINSIZEREL "${CMAKE_CURRENT_SOURCE_DIR}/tools/burnermax-dev"
)
# RB_LTNFLASH_READ_PROBE_81
# Exact DATA-IN-only vendor reads recovered statically from
# LtnFlash 2.6.11 / embedded MFlashPI.class.
add_executable(RetroBurnerLtnFlashReadProbe EXCLUDE_FROM_ALL
    src/ltnflash_read_probe_cli.cpp
)

target_compile_definitions(RetroBurnerLtnFlashReadProbe PRIVATE
    UNICODE
    _UNICODE
    _WIN32_WINNT=0x0A00
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)

if(MSVC)
    target_compile_options(RetroBurnerLtnFlashReadProbe PRIVATE
        /W4
        /permissive-
        /utf-8
        /EHsc
        /Zc:__cplusplus
    )
endif()

set_target_properties(RetroBurnerLtnFlashReadProbe PROPERTIES
    OUTPUT_NAME "retroburner-ltnflash-read-probe"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/tools/burnermax-dev"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_CURRENT_SOURCE_DIR}/tools/burnermax-dev"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_CURRENT_SOURCE_DIR}/tools/burnermax-dev"
    RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO "${CMAKE_CURRENT_SOURCE_DIR}/tools/burnermax-dev"
    RUNTIME_OUTPUT_DIRECTORY_MINSIZEREL "${CMAKE_CURRENT_SOURCE_DIR}/tools/burnermax-dev"
)
# RB_LTNFLASH_CHALLENGE_PROBE_82
# DATA-IN-only PLDS/LtnFlash registration challenge discriminator.
add_executable(RetroBurnerLtnFlashChallengeProbe EXCLUDE_FROM_ALL
    src/ltnflash_challenge_probe_cli.cpp
)

target_compile_definitions(RetroBurnerLtnFlashChallengeProbe PRIVATE
    UNICODE
    _UNICODE
    _WIN32_WINNT=0x0A00
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)

if(MSVC)
    target_compile_options(RetroBurnerLtnFlashChallengeProbe PRIVATE
        /W4
        /permissive-
        /utf-8
        /EHsc
        /Zc:__cplusplus
    )
endif()

set_target_properties(RetroBurnerLtnFlashChallengeProbe PROPERTIES
    OUTPUT_NAME "retroburner-ltnflash-challenge-probe"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/tools/burnermax-dev"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_CURRENT_SOURCE_DIR}/tools/burnermax-dev"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_CURRENT_SOURCE_DIR}/tools/burnermax-dev"
    RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO "${CMAKE_CURRENT_SOURCE_DIR}/tools/burnermax-dev"
    RUNTIME_OUTPUT_DIRECTORY_MINSIZEREL "${CMAKE_CURRENT_SOURCE_DIR}/tools/burnermax-dev"
)
# RB_STRICT_MSVC_WARNINGS_V84R
# Project-owned C++ and bundled cdirip must compile cleanly.
if(MSVC)
    target_compile_options(RetroBurner PRIVATE
        /W4
        /WX
        /permissive-
        /Zc:__cplusplus
        /utf-8
    )

    target_compile_options(cdirip PRIVATE
        /W4
        /WX
    )
endif()