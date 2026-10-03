cmake_minimum_required(VERSION 3.20)
project(AuraUI VERSION 0.2.2 LANGUAGES CXX RC)

# ---------------------------------------------------------------------------
# AuraUI - native Win32 desktop HUD (C++20 / Direct2D / DirectWrite)
#
# Toolchain: MinGW-w64 (winlibs). MSVC is intentionally NOT required.
# ---------------------------------------------------------------------------

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})

# windres needs to find icon.ico / app.manifest next to the .rc file
set(CMAKE_RC_FLAGS "${CMAKE_RC_FLAGS} -I\"${CMAKE_CURRENT_SOURCE_DIR}/assets\" -I\"${CMAKE_CURRENT_SOURCE_DIR}\"")

set(SOURCES
    src/main.cpp

    app/app.cpp

    core/log.cpp
    core/paths.cpp
    core/win_util.cpp

    config/config.cpp

    desktop/desktop.cpp

    hud/hud.cpp
    hud/hud_render.cpp

    monitor/cpu.cpp
    monitor/memory.cpp
    monitor/disk.cpp
    monitor/network.cpp
    monitor/sampler.cpp
    monitor/sysinfo.cpp

    system/gpu.cpp

    settings/settings.cpp

    tray/tray.cpp
)

set(HEADERS
    app/app.h

    core/log.h
    core/paths.h
    core/win_util.h

    config/config.h

    desktop/desktop.h

    hud/hud.h
    hud/theme.h

    monitor/cpu.h
    monitor/memory.h
    monitor/disk.h
    monitor/network.h
    monitor/snapshot.h
    monitor/sampler.h
    monitor/sysinfo.h

    system/gpu.h
    system/nvml_api.h

    settings/settings.h
    settings/settings_ids.h

    tray/tray.h
)

add_executable(AuraUI WIN32 ${SOURCES} ${HEADERS} assets/app.rc)

target_include_directories(AuraUI PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

target_compile_definitions(AuraUI PRIVATE
    UNICODE
    _UNICODE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    # MinGW defaults to 0x0602 (Win8); IDesktopWallpaper (per-monitor wallpaper
    # state for the underlay) needs 0x0603+. Runtime presence is checked via
    # CoCreateInstance failing on older systems - the registry fallback covers
    # them.
    _WIN32_WINNT=0x0603
    WINVER=0x0603
)

if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    target_compile_options(AuraUI PRIVATE
        -Wall
        -Wextra
        -Wno-unused-parameter
        -Wno-cast-function-type
        -fno-strict-aliasing
    )
    # wWinMain + statically linked runtime => standalone .exe, no DLL deps
    target_link_options(AuraUI PRIVATE
        -municode
        -static
        -static-libgcc
        -static-libstdc++
        -Wl,--gc-sections
    )
    target_compile_options(AuraUI PRIVATE -ffunction-sections -fdata-sections)
endif()

target_link_libraries(AuraUI PRIVATE
    user32
    gdi32
    shell32
    advapi32
    ole32
    oleaut32
    comctl32
    d2d1
    dwrite
    dwmapi
    dxgi
    iphlpapi
    ws2_32
    psapi
    windowscodecs
    wtsapi32
    uuid
)

set_target_properties(AuraUI PROPERTIES
    OUTPUT_NAME "AuraUI"
    WIN32_EXECUTABLE TRUE
)

message(STATUS "AuraUI: compiler = ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
message(STATUS "AuraUI: build type = ${CMAKE_BUILD_TYPE}")
