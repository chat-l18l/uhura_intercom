cmake_minimum_required(VERSION 3.25)

project(uhura_intercom
    VERSION 0.1.0
    DESCRIPTION "Embedded-grade C23 Opus UDP Intercom (Linux, ESP32 & Windows 11)"
    LANGUAGES C
)

set(CMAKE_C_STANDARD 23)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

option(UHURA_BUILD_APP "Build the platform intercom executable" ON)
option(UHURA_BUILD_TESTS "Build deterministic core unit tests" ON)

# Strict NASA Power of 10 / Embedded C23 compiler warnings
add_library(uhura_strict_warnings INTERFACE)
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(uhura_strict_warnings INTERFACE
        -Wall
        -Wextra
        -Werror
        -Wpedantic
        -Wconversion
        -Wsign-conversion
        -Wshadow
        -Wundef
        -Wstrict-prototypes
        -Wmissing-prototypes
        -Wvla
        -fno-common
    )
endif()

# ---------------------------------------------------------------------------
# Opus Codec Dependency:
# - Native Linux: fast system/Pixi libopus via pkg-config
# - Windows Cross-Compile: static zero-DLL libopus built from official source
# ---------------------------------------------------------------------------
if(CMAKE_SYSTEM_NAME STREQUAL "Windows")
    include(FetchContent)
    set(OPUS_BUILD_SHARED_LIBRARY OFF CACHE BOOL "" FORCE)
    set(OPUS_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
    set(OPUS_BUILD_TESTING OFF CACHE BOOL "" FORCE)
    set(OPUS_FORTIFY_SOURCE OFF CACHE BOOL "" FORCE)
    set(OPUS_STACK_PROTECTOR OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(
        opus_static
        URL https://downloads.xiph.org/releases/opus/opus-1.5.2.tar.gz
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    )
    FetchContent_MakeAvailable(opus_static)
    set(UHURA_OPUS_TARGET Opus::opus)
else()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(OPUS REQUIRED IMPORTED_TARGET opus)
    set(UHURA_OPUS_TARGET PkgConfig::OPUS)
endif()

# ---------------------------------------------------------------------------
# Pure Platform-Independent Core Library (0 OS / socket / audio-driver headers)
# ---------------------------------------------------------------------------
add_library(uhura_core STATIC
    src/core/types.c
    src/core/audio_dsp.c
    src/core/wire_proto.c
    src/core/spsc_pcm_ring.c
    src/core/opus_codec.c
    src/core/jitter_buffer.c
    src/core/duplex_gate.c
    src/core/intercom_core.c
)

target_include_directories(uhura_core
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

if(CMAKE_SYSTEM_NAME STREQUAL "Windows")
    # Provide <opus.h> include path matching pkg-config opus layout
    target_include_directories(uhura_core SYSTEM PUBLIC ${opus_static_SOURCE_DIR}/include)
endif()

target_link_libraries(uhura_core
    PUBLIC
        ${UHURA_OPUS_TARGET}
    PRIVATE
        uhura_strict_warnings
)

# ---------------------------------------------------------------------------
# Platform 1: Thin Linux Adapter & CLI Executable (ALSA + POSIX UDP)
# ---------------------------------------------------------------------------
if(UHURA_BUILD_APP AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
    pkg_check_modules(ALSA REQUIRED IMPORTED_TARGET alsa)
    find_package(Threads REQUIRED)

    add_executable(uhura
        src/platform/linux/net_udp.c
        src/platform/linux/audio_alsa.c
        src/platform/linux/control_cli.c
        src/platform/linux/main_linux.c
    )

    target_compile_definitions(uhura PRIVATE _POSIX_C_SOURCE=200809L)

    target_include_directories(uhura
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/include
    )

    target_link_libraries(uhura
        PRIVATE
            uhura_core
            PkgConfig::ALSA
            Threads::Threads
            uhura_strict_warnings
    )
endif()

# ---------------------------------------------------------------------------
# Platform 2: Thin Windows 11 Native Standalone GUI Executable (uhura.exe)
# Zero external DLLs (-static), 4 MB stack headroom, Win32 + WinMM + Winsock2
# ---------------------------------------------------------------------------
if(UHURA_BUILD_APP AND CMAKE_SYSTEM_NAME STREQUAL "Windows")
    add_executable(uhura WIN32
        src/platform/win32/net_winsock.c
        src/platform/win32/audio_winmm.c
        src/platform/win32/main_win32.c
    )

    target_compile_definitions(uhura PRIVATE
        WIN32_LEAN_AND_MEAN
        _WIN32_WINNT=0x0A00
    )

    target_include_directories(uhura
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/include
    )

    target_link_options(uhura PRIVATE
        -static
        -Wl,--stack,4194304
    )

    target_link_libraries(uhura
        PRIVATE
            uhura_core
            ws2_32
            winmm
            gdi32
            user32
            uhura_strict_warnings
    )
endif()

# ---------------------------------------------------------------------------
# Deterministic Core Unit Tests (compile & run on both Linux and Windows!)
# ---------------------------------------------------------------------------
if(UHURA_BUILD_TESTS)
    enable_testing()

    function(uhura_add_test test_name test_src)
        add_executable(${test_name} ${test_src})
        target_link_libraries(${test_name}
            PRIVATE
                uhura_core
                uhura_strict_warnings
        )
        if(CMAKE_SYSTEM_NAME STREQUAL "Windows")
            target_link_options(${test_name} PRIVATE -static -Wl,--stack,4194304)
        endif()
        add_test(NAME ${test_name} COMMAND ${test_name})
    endfunction()

    uhura_add_test(test_wire_proto        tests/test_wire_proto.c)
    uhura_add_test(test_spsc_pcm_ring     tests/test_spsc_pcm_ring.c)
    uhura_add_test(test_jitter_buffer     tests/test_jitter_buffer.c)
    uhura_add_test(test_opus_codec        tests/test_opus_codec.c)
    uhura_add_test(test_audio_dsp         tests/test_audio_dsp.c)
    uhura_add_test(test_intercom_loopback tests/test_intercom_loopback.c)
    uhura_add_test(test_care_listen       tests/test_care_listen.c)
endif()
