cmake_minimum_required(VERSION 3.24)

project(RetroBeamOpticalEngine VERSION 0.6 LANGUAGES C)
# RETROBEAM_SUPPORTED_PLATFORM_CONTRACT
#
# RetroBurner is not targeting unknown 1990s Unix variants.  The optical
# backend has an explicit modern platform contract:
#
#   Windows -> MinGW-w64 GCC
#   Linux   -> GCC/Clang POSIX
#   macOS   -> Apple Clang/POSIX
#
# Standard C11 headers/functions are requirements.  OS-specific inherited
# compatibility macros are selected by platform instead of rediscovered by
# compile probes on every clean configure.
set(HAVE_STDINT_H 1)
set(HAVE_INTTYPES_H 1)
set(HAVE_STDDEF_H 1)
set(HAVE_PUTENV 1)
set(HAVE_STRERROR 1)

if(WIN32)
    if(NOT MINGW)
        message(FATAL_ERROR
            "RetroBeam Windows backend currently requires MinGW-w64 GCC")
    endif()

    # MinGW-w64 compatibility surface used by the inherited cdrecord headers.
    set(HAVE_SYS_TYPES_H 1)
    set(HAVE_UNISTD_H 1)
    set(HAVE_SYS_TIME_H 1)
    set(HAVE_SYS_RESOURCE_H 0)

    set(HAVE_DIRECT_H 1)
    set(HAVE_IO_H 1)
    set(HAVE_PROCESS_H 1)
    set(HAVE_WINDOWS_H 1)

    set(HAVE_GETTIMEOFDAY 1)
    set(HAVE_USLEEP 1)

elseif(UNIX)
    # Linux and macOS supported POSIX contract.
    set(HAVE_SYS_TYPES_H 1)
    set(HAVE_UNISTD_H 1)
    set(HAVE_SYS_TIME_H 1)
    set(HAVE_SYS_RESOURCE_H 1)

    set(HAVE_DIRECT_H 0)
    set(HAVE_IO_H 0)
    set(HAVE_PROCESS_H 0)
    set(HAVE_WINDOWS_H 0)

    set(HAVE_GETTIMEOFDAY 1)
    set(HAVE_USLEEP 1)

    # RetroBeam Linux/macOS replacement for the legacy Schily xconfig
    # architecture and libc probes.
    set(HAVE_C_BITFIELDS 1)
    include(TestBigEndian)
    test_big_endian(WORDS_BIGENDIAN)
    set(HAVE_C_BIGENDIAN 1)
    if(WORDS_BIGENDIAN)
        set(BITFIELDS_HTOL 1)
    endif()

    set(HAVE_STRUCT_RUSAGE 1)
    set(HAVE_TYPE_RLIM_T 1)

    # POSIX/glibc already owns fexecve(int,...). Schily's historical
    # fexecve() has a different signature, so do not redirect that name.
    set(NO_SCHILY_FEXEC 1)

else()
    message(FATAL_ERROR
        "Unsupported RetroBeam platform: ${CMAKE_SYSTEM_NAME}")
endif()
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/generated/retroburner")

configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/include/retroburner/config.h.in"
    "${CMAKE_CURRENT_BINARY_DIR}/generated/retroburner/config.h"
    @ONLY
)


# RetroBurner-owned build.  Do not include Schilling RULES/SMakefile here.
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)

set(SCHILY_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../external/schilytools")

set(RETROBEAM_SOURCES
    "${SCHILY_ROOT}/cdrecord/audiosize.c"
    "${SCHILY_ROOT}/cdrecord/auinfo.c"
    "${SCHILY_ROOT}/cdrecord/cd_misc.c"
    "${SCHILY_ROOT}/cdrecord/cdr_drv.c"
    "${SCHILY_ROOT}/cdrecord/cdrecord.c"
    "${SCHILY_ROOT}/cdrecord/cdtext.c"
    "${SCHILY_ROOT}/cdrecord/clone.c"
    "${SCHILY_ROOT}/cdrecord/crc16.c"
    "${SCHILY_ROOT}/cdrecord/cue.c"
    "${SCHILY_ROOT}/cdrecord/diskid.c"
    "${SCHILY_ROOT}/cdrecord/drv_7501.c"
    "${SCHILY_ROOT}/cdrecord/drv_bd.c"
    "${SCHILY_ROOT}/cdrecord/drv_dvd.c"
    "${SCHILY_ROOT}/cdrecord/drv_dvdplus.c"
    "${SCHILY_ROOT}/cdrecord/drv_jvc.c"
    "${SCHILY_ROOT}/cdrecord/drv_mmc.c"
    "${SCHILY_ROOT}/cdrecord/drv_philips.c"
    "${SCHILY_ROOT}/cdrecord/drv_simul.c"
    "${SCHILY_ROOT}/cdrecord/drv_sony.c"
    "${SCHILY_ROOT}/cdrecord/fifo.c"
    "${SCHILY_ROOT}/cdrecord/isosize.c"
    "${SCHILY_ROOT}/cdrecord/misc.c"
    "${SCHILY_ROOT}/cdrecord/movesect.c"
    "${SCHILY_ROOT}/cdrecord/priv.c"
    "${SCHILY_ROOT}/cdrecord/scsi_cdr.c"
    "${SCHILY_ROOT}/cdrecord/scsi_cdr_mmc4.c"
    "${SCHILY_ROOT}/cdrecord/scsi_mmc.c"
    "${SCHILY_ROOT}/cdrecord/scsi_mmc4.c"
    "${SCHILY_ROOT}/cdrecord/scsi_scan.c"
    "${SCHILY_ROOT}/cdrecord/scsilog.c"
    "${SCHILY_ROOT}/cdrecord/sector.c"
    "${SCHILY_ROOT}/cdrecord/subchan.c"
    "${SCHILY_ROOT}/cdrecord/vendor.c"
    "${SCHILY_ROOT}/cdrecord/wm_packet.c"
    "${SCHILY_ROOT}/cdrecord/wm_session.c"
    "${SCHILY_ROOT}/cdrecord/wm_track.c"
    "${SCHILY_ROOT}/cdrecord/xio.c"
)
# RETROBURNER_WINDOWS_EXCLUDE_POSIX_PRIV
if(WIN32)
    list(FILTER RETROBEAM_SOURCES
         EXCLUDE REGEX "[/\\]priv[.]c$")
endif()


add_executable(retrobeam_record
    ${RETROBEAM_SOURCES}
)

set_target_properties(retrobeam_record PROPERTIES
    OUTPUT_NAME "retrobeam"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

target_include_directories(retrobeam_record PRIVATE
    "${CMAKE_CURRENT_BINARY_DIR}/generated"
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "${SCHILY_ROOT}/include"
    "${SCHILY_ROOT}/cdrecord"
    "${SCHILY_ROOT}/libscg"
    "${SCHILY_ROOT}/libedc"
    "${SCHILY_ROOT}/libcdrdeflt"
    "${SCHILY_ROOT}/libdeflt"
)

target_compile_definitions(retrobeam_record PRIVATE
    CDRECORD
    DVD_MULTI
    DRV_DVD
    DRV_BD
    FIFO
    AUINFO
    HAVE_LIB_EDC_ECC
    CLONE_WRITE
    USE_LARGEFILES
    SCHILY_PRINT
)

if(UNIX)
    target_compile_definitions(retrobeam_record PRIVATE
        _FILE_OFFSET_BITS=64
    )
endif()

# RETROBEAM_LINUX_GNU_SOURCE_V5
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_compile_definitions(retrobeam_record PRIVATE
        _GNU_SOURCE
    )
endif()

if(MINGW)
    target_compile_options(retrobeam_record PRIVATE
        "-Wno-old-style-definition"
        "-Wno-deprecated-declarations"
    )
endif()

# Platform support libraries.
#
# Windows keeps the last proven i686 bridge archives unchanged.
# Native POSIX builds compile the RetroBeam support code directly with
# CMake and select the Linux libscg SG transport.
if(WIN32)
    set(RB_LIB_SCGCMD "${SCHILY_ROOT}/libs/i686-mingw32_nt-gcc/libscgcmd.a")
    set(RB_LIB_RSCG "${SCHILY_ROOT}/libs/i686-mingw32_nt-gcc/librscg.a")
    set(RB_LIB_SCG "${SCHILY_ROOT}/libs/i686-mingw32_nt-gcc/libscg.a")
    set(RB_LIB_EDC_ECC "${SCHILY_ROOT}/libs/i686-mingw32_nt-gcc/libedc_ecc.a")
    set(RB_LIB_CDRDEFLT "${SCHILY_ROOT}/libs/i686-mingw32_nt-gcc/libcdrdeflt.a")
    set(RB_LIB_DEFLT "${SCHILY_ROOT}/libs/i686-mingw32_nt-gcc/libdeflt.a")
    set(RB_LIB_SCHILY "${SCHILY_ROOT}/libs/i686-mingw32_nt-gcc/libschily.a")
    set(RB_LIB_INTL "C:/msys64/mingw32/lib/libintl.dll.a")
    set(RB_LIB_ICONV "C:/msys64/mingw32/lib/libiconv.dll.a")

    target_link_libraries(retrobeam_record PRIVATE
        "-Wl,--start-group"
        ${RB_LIB_SCGCMD}
        ${RB_LIB_RSCG}
        ${RB_LIB_SCG}
        ${RB_LIB_EDC_ECC}
        ${RB_LIB_CDRDEFLT}
        ${RB_LIB_DEFLT}
        ${RB_LIB_SCHILY}
        "-Wl,--end-group"
        ${RB_LIB_INTL}
        ${RB_LIB_ICONV}
        ws2_32
        winmm
        advapi32
    )

    message(STATUS "RetroBeam optical engine: proven Win32 bridge libraries")
else()
    include("${CMAKE_CURRENT_SOURCE_DIR}/retrobeam-native-libs.cmake")

    find_package(Threads REQUIRED)
    find_package(Iconv)
    find_package(Intl)

    target_link_libraries(retrobeam_record PRIVATE
        "-Wl,--start-group"
        retrobeam_scgcmd
        retrobeam_rscg
        retrobeam_scg
        retrobeam_edc_ecc
        retrobeam_cdrdeflt
        retrobeam_deflt
        retrobeam_schily
        "-Wl,--end-group"
        Threads::Threads
        m
        dl
    )

    if(Iconv_FOUND)
        target_link_libraries(retrobeam_record PRIVATE Iconv::Iconv)
    endif()

    if(Intl_FOUND)
        target_link_libraries(retrobeam_record PRIVATE Intl::Intl)
    endif()

    message(STATUS "RetroBeam optical engine: native POSIX CMake libraries")
endif()

message(STATUS "Schilling RULES/SMakefile/configure are NOT invoked")
