cmake_minimum_required(VERSION 3.25)

project(uhura_intercom
    VERSION 0.1.0
    DESCRIPTION "Embedded-grade C23 Opus UDP Intercom"
    LANGUAGES C
)

set(CMAKE_C_STANDARD 23)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

option(UHURA_BUILD_LINUX_APP "Build the Linux ALSA/UDP intercom executable" ON)
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

find_package(PkgConfig REQUIRED)
pkg_check_modules(OPUS REQUIRED IMPORTED_TARGET opus)

# ---------------------------------------------------------------------------
# Pure Platform-Independent Core Library (0 OS / socket / audio-driver headers)
# ---------------------------------------------------------------------------
add_library(uhura_core STATIC
    src/core/types.c
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

target_link_libraries(uhura_core
    PUBLIC
        PkgConfig::OPUS
    PRIVATE
        uhura_strict_warnings
)

# ---------------------------------------------------------------------------
# Thin Linux Platform Adapter & Intercom Executable
# ---------------------------------------------------------------------------
if(UHURA_BUILD_LINUX_APP AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
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
# Deterministic Core Unit Tests (no network card or sound card required)
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
        add_test(NAME ${test_name} COMMAND ${test_name})
    endfunction()

    uhura_add_test(test_wire_proto        tests/test_wire_proto.c)
    uhura_add_test(test_spsc_pcm_ring     tests/test_spsc_pcm_ring.c)
    uhura_add_test(test_jitter_buffer     tests/test_jitter_buffer.c)
    uhura_add_test(test_opus_codec        tests/test_opus_codec.c)
    uhura_add_test(test_intercom_loopback tests/test_intercom_loopback.c)
endif()
