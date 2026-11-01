cmake_minimum_required(VERSION 3.20)

project(ConkersBadFurDayRecompiled LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)

# Where N64Recomp wrote the recompiled functions (see tools/regenerate.sh).
set(CONKER_RECOMPILED_DIR "${CMAKE_SOURCE_DIR}/build/us/RecompiledFuncs" CACHE PATH
    "Directory containing funcs.h, recomp_overlays.inl and funcs_*.c produced by N64Recomp")

# The recompiled code is ~54 MB of C. Keep its optimization level separate from the rest of the
# build so that a Debug build of the runtime does not mean an unoptimized game.
set(CONKER_RECOMPILED_OPT "-O1" CACHE STRING "Optimization flag for the recompiled game code")
option(CONKER_TRACE "Record recompiled function entries per thread for crash reports (slower)" ON)
set(CONKER_RENDERER "null" CACHE STRING
    "Renderer: null (headless, logs graphics tasks) or rt64 (window, needs tools/patches/rt64-f3dex2cbfd.patch)")
set_property(CACHE CONKER_RENDERER PROPERTY STRINGS null rt64)
if(NOT CONKER_RENDERER MATCHES "^(null|rt64)$")
    message(FATAL_ERROR "CONKER_RENDERER must be null or rt64, not '${CONKER_RENDERER}'")
endif()

# The RSP vector unit code (librecomp's rsp_vu_impl.hpp) uses SSE4.1 on x86-64. Every 64-bit x86 CPU
# that can run the game has it.
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(AMD64|x86_64)$")
    add_compile_options(-msse4.1)
endif()
# N64ModernRuntime brings ultramodern (libultra reimplementation), librecomp (glue for N64Recomp
# output, ROM/PI, saves), the N64Recomp library used for live recompilation, and miniz.
add_subdirectory(lib/N64ModernRuntime)

# ---------------------------------------------------------------------------------------------
# Recompiled game code
# ---------------------------------------------------------------------------------------------
file(GLOB CONKER_RECOMPILED_SOURCES CONFIGURE_DEPENDS "${CONKER_RECOMPILED_DIR}/funcs_*.c")
if(NOT CONKER_RECOMPILED_SOURCES)
    message(FATAL_ERROR
        "No recompiled sources in ${CONKER_RECOMPILED_DIR}. Run tools/regenerate.sh first "
        "(it needs the decomp checkout and your US ROM).")
endif()

add_library(ConkerRecompiled STATIC ${CONKER_RECOMPILED_SOURCES})
target_include_directories(ConkerRecompiled PUBLIC
    "${CONKER_RECOMPILED_DIR}"
    "${CMAKE_SOURCE_DIR}/lib/N64ModernRuntime/N64Recomp/include"
    "${CMAKE_SOURCE_DIR}/src"
)
# trace_mode makes N64Recomp include "trace.h" by that name (src/runtime/trace.h), in funcs.h too,
# so everything that includes funcs.h needs it.
target_include_directories(ConkerRecompiled PUBLIC "${CMAKE_SOURCE_DIR}/src/runtime")
target_compile_options(ConkerRecompiled PRIVATE ${CONKER_RECOMPILED_OPT} -w)
if(CONKER_TRACE)
    target_compile_definitions(ConkerRecompiled PRIVATE CONKER_TRACE=1)
endif()

# The recompiled code must come from this checkout's config/us.toml, or its hooks are silently
# missing: checked on every build (tools/check_recompiled.cmake).
option(CONKER_ALLOW_STALE_RECOMPILED "Build even when config/us.toml changed since tools/regenerate.sh ran" OFF)
if(NOT CONKER_ALLOW_STALE_RECOMPILED)
    add_custom_target(recompiled_check
        COMMAND ${CMAKE_COMMAND} -DSOURCE=${CMAKE_SOURCE_DIR} -DRECOMPILED=${CONKER_RECOMPILED_DIR}
                -P ${CMAKE_SOURCE_DIR}/tools/check_recompiled.cmake)
    add_dependencies(ConkerRecompiled recompiled_check)
endif()

# ---------------------------------------------------------------------------------------------
# Runtime executable
# ---------------------------------------------------------------------------------------------
# Audio microcode recompiled by RSPRecomp (config/aspMain.toml, run by tools/regenerate.sh).
set(CONKER_RSP_DIR "${CMAKE_SOURCE_DIR}/build/us/rsp" CACHE PATH "Directory containing aspMain.cpp produced by RSPRecomp")
if(NOT EXISTS "${CONKER_RSP_DIR}/aspMain.cpp")
    message(FATAL_ERROR "No ${CONKER_RSP_DIR}/aspMain.cpp. Run tools/regenerate.sh first.")
endif()

# src/runtime: boot, memory layout, overlays and the libultra pieces the recompiled code needs.
# src/platform: host backends (audio, input, window, renderer); the renderer-specific ones are
# added below. src/fixes: hooks that make the recompiled game behave as it does on hardware.
# src/credits: the port's own name in the ending credits.
# src/enhancements: one folder per feature the console does not have, each with its own switch.
add_executable(ConkersBadFurDayRecompiled
    src/main.cpp
    src/runtime/frame_audio.cpp
    src/runtime/frame_input.cpp
    src/runtime/game.cpp
    src/runtime/heap_check.cpp
    src/runtime/memory.cpp
    src/runtime/net_play.cpp
    src/runtime/net_codes.cpp
    src/runtime/net_lobby.cpp
    src/runtime/net_portmap.cpp
    src/runtime/net_socket.cpp
    src/runtime/snapshot.cpp
    src/runtime/overlays.cpp
    src/runtime/rsp.cpp
    src/runtime/rzip.cpp
    src/runtime/state_hash.cpp
    src/runtime/trace.cpp
    src/runtime/ultra_extras.cpp
    "${CONKER_RSP_DIR}/aspMain.cpp"
    src/platform/audio.cpp
    src/platform/display_mode.cpp
    src/platform/gpu_faults.cpp
    src/platform/process.cpp
    src/credits/credits.cpp
    src/fixes/geometry_escape.cpp
    src/fixes/frame_effects.cpp
    src/fixes/level_seams.cpp
    src/fixes/music_start.cpp
    src/fixes/rumble_banner.cpp
    src/fixes/scheduler_yield.cpp
    src/enhancements/cutscene_skip/cutscene_skip.cpp
    src/enhancements/draw_distance/draw_distance.cpp
    src/enhancements/debug_menu/debug_menu.cpp
    src/enhancements/object_inspector/object_inspector.cpp
    src/enhancements/pad_icons/choice.cpp
    src/enhancements/status.cpp
    src/enhancements/switches.cpp
    src/enhancements/display_options/display_options.cpp
    src/enhancements/display_options/widescreen_intro.cpp
    src/enhancements/display_options/widescreen_copies.cpp
    src/enhancements/display_options/widescreen_culling.cpp
    src/enhancements/display_options/widescreen_frame.cpp
    src/enhancements/display_options/widescreen_out.cpp
    src/enhancements/display_options/widescreen_timer.cpp
    src/enhancements/frame_rate/frame_rate.cpp
    src/enhancements/intro_skip/intro_skip.cpp
    src/enhancements/max_detail/max_detail.cpp
    src/enhancements/pause_save/pause_save.cpp
    src/enhancements/pause_save/resume_point.cpp
    src/enhancements/pause_save/save_check.cpp
    src/enhancements/haptics/haptics.cpp
    src/enhancements/adaptive_triggers/adaptive_triggers.cpp
    src/enhancements/consistent_controls/consistent_controls.cpp
    src/enhancements/consistent_controls/mouse_look.cpp
    src/enhancements/sharp_menus/menu_art.cpp
    src/enhancements/sharp_menus/pause_signs.cpp
    src/enhancements/surround_default/surround_default.cpp
)
target_include_directories(ConkersBadFurDayRecompiled PRIVATE
    "${CMAKE_SOURCE_DIR}/src"
    "${CMAKE_SOURCE_DIR}/lib/N64ModernRuntime/N64Recomp/include"
    "${CMAKE_SOURCE_DIR}/lib/N64ModernRuntime/thirdparty/miniz"
)
target_link_libraries(ConkersBadFurDayRecompiled PRIVATE ConkerRecompiled librecomp ultramodern miniz)
# The build a debug report names (src/enhancements/debug_menu): build_version.h, from git describe
# on every build (tools/build_version.cmake).
set(CONKER_GENERATED_DIR "${CMAKE_BINARY_DIR}/generated")
add_custom_target(build_version
    COMMAND ${CMAKE_COMMAND} -DSOURCE=${CMAKE_SOURCE_DIR} -DOUT=${CONKER_GENERATED_DIR}/build_version.h
            -P ${CMAKE_SOURCE_DIR}/tools/build_version.cmake
    BYPRODUCTS ${CONKER_GENERATED_DIR}/build_version.h)
add_dependencies(ConkersBadFurDayRecompiled build_version)
# The JSON library the runtime ships (nlohmann/json), for debug reports.
target_include_directories(ConkersBadFurDayRecompiled PRIVATE "${CONKER_GENERATED_DIR}"
    "${CMAKE_SOURCE_DIR}/lib/N64ModernRuntime/thirdparty")
# The microcode runs once per audio frame on the audio thread; keep it optimised in Debug builds.
set_source_files_properties("${CONKER_RSP_DIR}/aspMain.cpp" PROPERTIES COMPILE_OPTIONS "-O2;-w")

# ---------------------------------------------------------------------------------------------
# Renderer
# ---------------------------------------------------------------------------------------------
if(CONKER_RENDERER STREQUAL "rt64")
    if(APPLE)
        # RT64 compiles its shaders with Xcode's metal. Without it the build fails hundreds of steps in,
        # so check here, and pass on xcrun's own message (license, Metal toolchain, no Xcode selected).
        execute_process(COMMAND xcrun -sdk macosx metal --version
            RESULT_VARIABLE CONKER_METAL_RESULT OUTPUT_QUIET ERROR_VARIABLE CONKER_METAL_ERROR)
        if(NOT CONKER_METAL_RESULT EQUAL 0)
            string(STRIP "${CONKER_METAL_ERROR}" CONKER_METAL_ERROR)
            message(FATAL_ERROR
                "The rt64 renderer needs Xcode's metal compiler, and 'xcrun -sdk macosx metal --version' "
                "failed:\n  ${CONKER_METAL_ERROR}\n"
                "See step 1 of docs/building-macos.md.")
        endif()
    endif()
    set(RT64_STATIC ON CACHE BOOL "" FORCE)
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        # On Linux RT64 takes the SDL window and makes its Vulkan surface through SDL (X11 or Wayland).
        set(RT64_SDL_WINDOW_VULKAN ON CACHE BOOL "" FORCE)
    endif()
    add_subdirectory(lib/rt64 EXCLUDE_FROM_ALL)
    if(APPLE)
        # RT64 turns plume's Retina support off, which sizes the swap chain in points and leaves macOS
        # to upscale it on a Retina screen. The window is sized in pixels (src/platform/window_sdl.cpp),
        # so the swap chain should be too.
        target_compile_definitions(plume PRIVATE PLUME_APPLE_RETINA_ENABLED)
    endif()
    target_sources(ConkersBadFurDayRecompiled PRIVATE
        src/platform/renderer_rt64.cpp src/platform/object_picker_rt64.cpp src/platform/lobby_ui_rt64.cpp src/platform/lobby_style_rt64.cpp src/platform/font_rects.cpp src/enhancements/sharp_font/sharp_font.cpp src/enhancements/sharp_menus/sharp_menus.cpp src/platform/window_sdl.cpp src/platform/hang_watch.cpp src/platform/input_sdl.cpp
        src/enhancements/pad_icons/pad_icons.cpp src/enhancements/pad_icons/b_pad.cpp src/enhancements/pad_icons/stone_pad.cpp src/enhancements/pad_icons/hex_pad.cpp src/enhancements/pad_icons/square_pad.cpp src/enhancements/pad_icons/oval_pad.cpp src/enhancements/pad_icons/rock_pad.cpp src/enhancements/pad_icons/rug_pad.cpp src/enhancements/pad_icons/plate_pad.cpp src/enhancements/pad_icons/crate_pad.cpp src/enhancements/pad_icons/block_pad.cpp src/enhancements/pad_icons/disc_pad.cpp src/enhancements/pad_icons/rust_pad.cpp src/enhancements/pad_icons/riveted_pad.cpp src/enhancements/pad_icons/panel_pad.cpp
        src/enhancements/pad_icons/button_art.cpp)
    if(WIN32)
        # RT64 brings SDL2 for Windows, and sets these for its own folder only.
        set(CONKER_SDL2_DIR "${CMAKE_SOURCE_DIR}/lib/rt64/src/contrib/mupen64plus-win32-deps/SDL2-2.26.3")
        set(SDL2_INCLUDE_DIRS "${CONKER_SDL2_DIR}/include")
        target_link_directories(ConkersBadFurDayRecompiled PRIVATE "${CONKER_SDL2_DIR}/lib/x64")
        # The DLLs the game needs to start, next to it: SDL2, and DXC for RT64's D3D12 shaders.
        add_custom_command(TARGET ConkersBadFurDayRecompiled POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${CONKER_SDL2_DIR}/lib/x64/SDL2.dll"
                "${CMAKE_SOURCE_DIR}/lib/rt64/src/contrib/dxc/bin/x64/dxcompiler.dll"
                "${CMAKE_SOURCE_DIR}/lib/rt64/src/contrib/dxc/bin/x64/dxil.dll"
                "$<TARGET_FILE_DIR:ConkersBadFurDayRecompiled>")
    endif()
    target_include_directories(ConkersBadFurDayRecompiled PRIVATE
        ${SDL2_INCLUDE_DIRS}
        "${CMAKE_SOURCE_DIR}/lib/rt64/src"
        "${CMAKE_SOURCE_DIR}/lib/rt64/src/contrib"
        "${CMAKE_SOURCE_DIR}/lib/rt64/src/contrib/plume"
        "${CMAKE_SOURCE_DIR}/lib/rt64/src/contrib/hlslpp/include"
        "${CMAKE_SOURCE_DIR}/lib/rt64/src/contrib/imgui"
    )
    target_compile_definitions(ConkersBadFurDayRecompiled PRIVATE CONKER_RENDERER_RT64=1 CONKER_AUDIO_SDL=1 HLSL_CPU FFX_GCC)
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        # RT64 defines these for itself only; its headers need them here too (an SDL_Window* window).
        target_compile_definitions(ConkersBadFurDayRecompiled PRIVATE RT64_SDL_WINDOW_VULKAN PLUME_SDL_VULKAN_ENABLED)
    endif()
    target_link_libraries(ConkersBadFurDayRecompiled PRIVATE rt64 ${SDL2_LIBRARIES})
    if(APPLE)
        # DualSense haptic effects and adaptive triggers through Apple's GameController framework
        # and Core Haptics.
        enable_language(OBJCXX)
        target_sources(ConkersBadFurDayRecompiled PRIVATE src/platform/haptics_apple.mm src/platform/adaptive_triggers_apple.mm)
        set_source_files_properties(src/platform/haptics_apple.mm PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
        target_link_libraries(ConkersBadFurDayRecompiled PRIVATE "-framework GameController" "-framework CoreHaptics")
        # Started from the desktop: data folder, log and ROM panel (src/platform/launch.hpp).
        target_sources(ConkersBadFurDayRecompiled PRIVATE src/platform/launch_apple.mm)
        set_source_files_properties(src/platform/launch_apple.mm PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
        target_compile_definitions(ConkersBadFurDayRecompiled PRIVATE CONKER_LAUNCH_UI=1)
        target_link_libraries(ConkersBadFurDayRecompiled PRIVATE "-framework AppKit")
    elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux" OR WIN32)
        # Started from the desktop: data folder, log and the ROM dialog RT64 builds (nfd: GTK on
        # Linux, the common item dialog on Windows).
        if(WIN32)
            target_sources(ConkersBadFurDayRecompiled PRIVATE src/platform/launch_windows.cpp src/platform/launch_nfd.cpp)
        else()
            target_sources(ConkersBadFurDayRecompiled PRIVATE src/platform/launch_linux.cpp src/platform/launch_nfd.cpp)
        endif()
        target_include_directories(ConkersBadFurDayRecompiled PRIVATE
            "${CMAKE_SOURCE_DIR}/lib/rt64/src/contrib/nativefiledialog-extended/src/include")
        target_compile_definitions(ConkersBadFurDayRecompiled PRIVATE CONKER_LAUNCH_UI=1)
        target_link_libraries(ConkersBadFurDayRecompiled PRIVATE nfd)
    endif()
else()
    target_sources(ConkersBadFurDayRecompiled PRIVATE
        src/platform/renderer_null.cpp src/platform/window_headless.cpp src/platform/input_headless.cpp)
endif()

# ---------------------------------------------------------------------------------------------
# Opening an online host's port on the router (src/runtime/net_portmap.cpp): miniupnpc for UPnP IGD
# and libnatpmp for NAT-PMP, both BSD-3-Clause, fetched at configure time at pinned versions.
# ---------------------------------------------------------------------------------------------
option(CONKER_PORT_MAPPING "Open an online host's port on the router with UPnP and NAT-PMP" ON)
# Internet play is off unless the game starts with --net-unsafe-internet. Test builds may start with it
# on instead (--net-lan-only turns it off); the lobby still says UNSAFE either way.
option(CONKER_NET_INTERNET_DEFAULT "Start with internet play on, as with --net-unsafe-internet (test builds)" OFF)
target_compile_definitions(ConkersBadFurDayRecompiled PRIVATE CONKER_NET_INTERNET_DEFAULT=$<BOOL:${CONKER_NET_INTERNET_DEFAULT}>)
if(CONKER_PORT_MAPPING)
    include(FetchContent)
    set(UPNPC_BUILD_STATIC ON CACHE BOOL "" FORCE)
    set(UPNPC_BUILD_SHARED OFF CACHE BOOL "" FORCE)
    set(UPNPC_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(UPNPC_BUILD_SAMPLE OFF CACHE BOOL "" FORCE)
    set(UPNPC_NO_INSTALL ON CACHE BOOL "" FORCE)
    FetchContent_Declare(miniupnpc
        URL https://github.com/miniupnp/miniupnp/archive/refs/tags/miniupnpc_2_3_3.tar.gz
        URL_HASH SHA256=8cf2c833b3e76fc4893ff29c2a376e3394962449e5970e373c0a91421724d222
        SOURCE_SUBDIR miniupnpc
        EXCLUDE_FROM_ALL)
    # libnatpmp is built here rather than by its own CMakeLists: its folder holds a file named VERSION,
    # which a case-insensitive disk finds for C++'s <version> once that folder is on the include path.
    # Only its headers are copied out to be included.
    FetchContent_Declare(libnatpmp
        URL https://github.com/miniupnp/libnatpmp/archive/134fc89e2781e154e40042641f4d8bcbe42579f1.tar.gz
        URL_HASH SHA256=be1864ed4e54a122590fd2de3824b085386b6e4aa9a91738c277c280ab8bf188
        SOURCE_SUBDIR no-cmake-build
        EXCLUDE_FROM_ALL)
    FetchContent_MakeAvailable(miniupnpc libnatpmp)
    add_library(conker_natpmp STATIC "${libnatpmp_SOURCE_DIR}/natpmp.c" "${libnatpmp_SOURCE_DIR}/getgateway.c")
    if(WIN32)
        target_sources(conker_natpmp PRIVATE "${libnatpmp_SOURCE_DIR}/wingettimeofday.c")
    endif()
    target_compile_definitions(conker_natpmp PUBLIC ENABLE_STRNATPMPERR NATPMP_STATICLIB)
    target_compile_options(conker_natpmp PRIVATE -w)
    file(COPY "${libnatpmp_SOURCE_DIR}/natpmp.h" "${libnatpmp_SOURCE_DIR}/natpmp_declspec.h"
         DESTINATION "${CMAKE_BINARY_DIR}/natpmp-include")
    target_include_directories(conker_natpmp PUBLIC "${CMAKE_BINARY_DIR}/natpmp-include")
    target_link_libraries(ConkersBadFurDayRecompiled PRIVATE libminiupnpc-static conker_natpmp)
    target_compile_definitions(ConkersBadFurDayRecompiled PRIVATE CONKER_PORT_MAPPING=1)
endif()

if(WIN32)
    # The icon for Explorer, the window and the taskbar.
    target_sources(ConkersBadFurDayRecompiled PRIVATE assets/app-icon.rc)
    # windows.h (included by librecomp's headers too) would otherwise define min and max macros, which
    # break std::min and std::max.
    target_compile_definitions(ConkersBadFurDayRecompiled PRIVATE NOMINMAX)
    # And would include winsock.h, which clashes with the winsock2.h online play uses (empty, as
    # winsock2.h defines it).
    target_compile_definitions(ConkersBadFurDayRecompiled PRIVATE _WINSOCKAPI_=)
    target_compile_definitions(ConkerRecompiled PRIVATE NOMINMAX)
    # The portable C library calls (getenv, fopen, sscanf, inet_ntoa) without the Microsoft-only
    # replacements the C runtime suggests.
    target_compile_definitions(ConkersBadFurDayRecompiled PRIVATE _CRT_SECURE_NO_WARNINGS _WINSOCK_DEPRECATED_NO_WARNINGS)
    # M_PI and friends from <cmath>; and, as RT64 does for itself, SDL's prefetch intrinsic left to
    # SDL, which clang-cl's own header would define a second time.
    target_compile_definitions(ConkersBadFurDayRecompiled PRIVATE _USE_MATH_DEFINES __PRFCHWINTRIN_H)
    # Sockets and the network adapter list (src/runtime/net_*.cpp), crash backtraces (src/main.cpp).
    target_link_libraries(ConkersBadFurDayRecompiled PRIVATE ws2_32 iphlpapi dbghelp)
elseif(APPLE)
    target_compile_definitions(ConkersBadFurDayRecompiled PRIVATE CONKER_PLATFORM_POSIX=1)
elseif(UNIX)
    target_compile_definitions(ConkersBadFurDayRecompiled PRIVATE CONKER_PLATFORM_POSIX=1)
    target_link_libraries(ConkersBadFurDayRecompiled PRIVATE rt)
endif()
