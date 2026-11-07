cmake_minimum_required(VERSION 3.20)

project(ConkersBadFurDayRecompiled LANGUAGES C CXX)

# Windows: the C++ runtime is linked into the game and every library built with it, so a PC needs no
# Visual C++ Redistributable. The policy default makes submodules with older CMake minimums follow.
if(WIN32)
    set(CMAKE_POLICY_DEFAULT_CMP0091 NEW)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

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
# Online play runs the same game on every player's machine, Macs, Linux and Windows PCs together, and
# their games must stay the same to the bit. A compiler may fuse a multiply and an add into one
# instruction with a single rounding (arm64 clang does within an expression, GCC anywhere), which
# x86-64 without FMA never does: so never, here and in every library built with the game.
#
# The recompiled code (and the native code that reads game memory) reaches the same RDRAM through
# 32-, 16- and 8-bit pointers (recomp.h's MEM_W, MEM_H, MEM_B). Under C's strict aliasing a compiler
# may take a 16-bit load and a 32-bit store as different memory and swap them: a Linux clang read
# the stack word __n_vsVol had just stored before the store, so the title music's notes came out
# louder there than on a Mac, and online the two machines' games parted at once. So no
# type-based aliasing anywhere either.
if(CMAKE_C_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
    add_compile_options(/clang:-ffp-contract=off /clang:-fno-strict-aliasing)
else()
    add_compile_options(-ffp-contract=off -fno-strict-aliasing)
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
# The game switches the FPU's rounding mode (CTC1, set_cop1_cs) hundreds of times, to truncate for a
# conversion and back. Without this a compiler may fold or move float arithmetic across the switch as
# if it always rounded to nearest, and two compilers (Apple's clang, a Linux clang, clang-cl) do so
# differently: online, a Mac and a Linux machine placed a vertex on and off the screen from the same
# memory. With it every machine does each operation in the mode the game set.
if(CMAKE_C_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
    target_compile_options(ConkerRecompiled PRIVATE /clang:-frounding-math)
else()
    target_compile_options(ConkerRecompiled PRIVATE -frounding-math)
endif()
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
# src/net: online play (lockstep session, lobby, join codes, port mapping); the lobby's page (SDL's
# keyboard) is added with the RT64 renderer below.
# src/credits: the port's own name in the ending credits.
# src/enhancements: one folder per feature the console does not have, each with its own switch.
add_executable(ConkersBadFurDayRecompiled
    src/main.cpp
    src/runtime/frame_audio.cpp
    src/runtime/frame_input.cpp
    src/runtime/game.cpp
    src/runtime/heap_check.cpp
    src/runtime/memory.cpp
    src/runtime/snapshot.cpp
    src/runtime/overlays.cpp
    src/runtime/mod_data.cpp
    src/runtime/rsp.cpp
    src/runtime/rzip.cpp
    src/runtime/state_hash.cpp
    src/runtime/trace.cpp
    src/runtime/ultra_extras.cpp
    src/net/net_play.cpp
    src/net/net_codes.cpp
    src/net/net_lobby.cpp
    src/net/net_portmap.cpp
    src/net/net_seal.cpp
    src/net/net_stats.cpp
    src/net/net_keys.cpp
    src/net/net_socket.cpp
    "${CONKER_RSP_DIR}/aspMain.cpp"
    src/platform/audio.cpp
    src/platform/display_mode.cpp
    src/platform/gpu_faults.cpp
    src/platform/input_config.cpp
    src/platform/process.cpp
    src/credits/credits.cpp
    src/fixes/alien_shadow.cpp
    src/fixes/display_list_room.cpp
    src/fixes/geometry_escape.cpp
    src/fixes/heist_alcove.cpp
    src/fixes/frame_effects.cpp
    src/fixes/level_seams.cpp
    src/fixes/level_visibility.cpp
    src/fixes/music_start.cpp
    src/fixes/music_skip.cpp
    src/fixes/projected_shadow.cpp
    src/fixes/rumble_banner.cpp
    src/fixes/scheduler_yield.cpp
    src/fixes/weapon_rack.cpp
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
    src/enhancements/display_options/edge_text.cpp
    src/enhancements/display_options/widescreen_bubbles.cpp
    src/enhancements/display_options/widescreen_frame.cpp
    src/enhancements/display_options/widescreen_out.cpp
    src/enhancements/display_options/widescreen_timer.cpp
    src/enhancements/frame_rate/frame_rate.cpp
    src/enhancements/intro_skip/intro_skip.cpp
    src/enhancements/max_detail/max_detail.cpp
    src/enhancements/my_view/my_view.cpp
    src/enhancements/pause_save/pause_save.cpp
    src/enhancements/pause_save/resume_point.cpp
    src/enhancements/pause_save/save_check.cpp
    src/enhancements/quit_game/quit_game.cpp
    src/enhancements/haptics/haptics.cpp
    src/enhancements/adaptive_triggers/adaptive_triggers.cpp
    src/enhancements/consistent_controls/consistent_controls.cpp
    src/enhancements/consistent_controls/mouse_look.cpp
    src/enhancements/sharp_menus/menu_art.cpp
    src/enhancements/sharp_menus/pause_signs.cpp
    src/enhancements/surround_default/surround_default.cpp
    src/enhancements/mp_characters/mp_characters.cpp
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
# The functions mods may not hook or replace, because config/us.toml changes them: mod_locks.h,
# read by src/runtime/overlays.cpp (tools/gen_mod_locks.py; tomllib needs Python 3.11).
find_package(Python3 3.11 REQUIRED COMPONENTS Interpreter)
add_custom_command(
    OUTPUT ${CONKER_GENERATED_DIR}/mod_locks.h
    COMMAND Python3::Interpreter ${CMAKE_SOURCE_DIR}/tools/gen_mod_locks.py
            --config ${CMAKE_SOURCE_DIR}/config/us.toml --syms ${CMAKE_SOURCE_DIR}/config/us.syms.toml
            --out ${CONKER_GENERATED_DIR}/mod_locks.h
    DEPENDS ${CMAKE_SOURCE_DIR}/tools/gen_mod_locks.py ${CMAKE_SOURCE_DIR}/config/us.toml ${CMAKE_SOURCE_DIR}/config/us.syms.toml)
# And every recompiled function by name, for the replacement lookup on function entry
# (src/runtime/trace.h).
add_custom_command(
    OUTPUT ${CONKER_GENERATED_DIR}/func_names.inl
    COMMAND Python3::Interpreter ${CMAKE_SOURCE_DIR}/tools/gen_func_names.py ${CONKER_RECOMPILED_DIR}
            --out ${CONKER_GENERATED_DIR}/func_names.inl
    DEPENDS ${CMAKE_SOURCE_DIR}/tools/gen_func_names.py ${CONKER_RECOMPILED_SOURCES})
target_sources(ConkersBadFurDayRecompiled PRIVATE ${CONKER_GENERATED_DIR}/mod_locks.h ${CONKER_GENERATED_DIR}/func_names.inl)
# Mods built into the game, off until the player turns them on (debug menu > MODS): each a .nrm its
# folder's Makefile makes (tools/build_mods.sh), named <mod id>.nrm. One not built yet is left out with a
# warning; the packaging scripts build them first. A copy in the data folder's mods/ wins over the
# built-in one (librecomp opens the folder first).
set(CONKER_EMBEDDED_MODS "${CMAKE_SOURCE_DIR}/mods/speedrun/build/conker_speedrun.nrm"
    CACHE STRING "Built .nrm files to build into the game (a list)")
set(embedded_arrays "")
set(embedded_calls "")
set(embedded_ids "")
foreach(nrm IN LISTS CONKER_EMBEDDED_MODS)
    if(NOT EXISTS "${nrm}")
        message(WARNING "${nrm} is not built (tools/build_mods.sh): the game is built without it")
        continue()
    endif()
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${nrm}")
    get_filename_component(mod_id "${nrm}" NAME_WE)
    file(READ "${nrm}" mod_hex HEX)
    string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," mod_bytes "${mod_hex}")
    string(APPEND embedded_arrays "const uint8_t mod_${mod_id}[] = {${mod_bytes}};\n")
    string(APPEND embedded_calls "    recomp::mods::register_embedded_mod(\"${mod_id}\", mod_${mod_id});\n")
    list(APPEND embedded_ids "${mod_id}")
endforeach()
# Read by the packaging scripts, which refuse a build without them.
set(CONKER_EMBEDDED_MOD_IDS "${embedded_ids}" CACHE INTERNAL "")
file(CONFIGURE OUTPUT "${CONKER_GENERATED_DIR}/embedded_mods.cpp" @ONLY CONTENT
"// Generated by CMakeLists.txt from CONKER_EMBEDDED_MODS.
#include <cstdint>
#include \"librecomp/mods.hpp\"

namespace {
@embedded_arrays@}

namespace conker {
void register_embedded_mods() {
@embedded_calls@}
}
")
target_sources(ConkersBadFurDayRecompiled PRIVATE ${CONKER_GENERATED_DIR}/embedded_mods.cpp)
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
    if(WIN32 AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(ARM64|arm64|aarch64)$")
        set(CONKER_WIN_ARM64 ON)
    endif()
    if(WIN32 AND (CONKER_WIN_ARM64 OR NOT CMAKE_HOST_WIN32))
        # DXC's own release, in the layout RT64_WIN_DXC_DIR wants (tools/patches/rt64-windows-arm64.patch):
        # it has the arm64 DLLs RT64's copy lacks, and its Windows dxc.exe for a cross build. RT64 links
        # shader libraries compiled at build time with entry points compiled at run time, and DXC only
        # links pieces from one version, so the DLLs shipped and the dxc.exe that compiles the shaders
        # come from this one release.
        include(FetchContent)
        FetchContent_Declare(dxc_release
            URL https://github.com/microsoft/DirectXShaderCompiler/releases/download/v1.8.2403.2/dxc_2024_03_29.zip
            URL_HASH SHA256=74874e9741f027d4321263af58d24ae0f6dde2351230680b151c87144cc0a02a)
        FetchContent_MakeAvailable(dxc_release)
        set(RT64_WIN_DXC_DIR "${dxc_release_SOURCE_DIR}")
    endif()
    option(CONKER_SIGNED_SHADERS "Cross build: compile D3D12 shaders with the release's dxc.exe under Wine in Docker" ON)
    if(WIN32 AND NOT CMAKE_HOST_WIN32 AND CONKER_SIGNED_SHADERS)
        set(RT64_CROSS_DXC "${CMAKE_SOURCE_DIR}/tools/windows/dxc_signed.sh" "${dxc_release_SOURCE_DIR}/bin/x64/dxc.exe")
    endif()
    # The Linux tarball carries its own SDL2 (tools/linux/build.sh), built here so that it needs only
    # the C library: its video and audio backends (X11, Wayland, PulseAudio, ALSA...) are loaded at run
    # time. A distribution's own libSDL2 links them all directly.
    option(CONKER_BUNDLED_SDL2 "Linux: build SDL2 from source and load it from libs/ next to the game" OFF)
    if(CONKER_WIN_ARM64 OR (CONKER_BUNDLED_SDL2 AND CMAKE_SYSTEM_NAME STREQUAL "Linux"))
        # SDL2 has no arm64 build for Windows: built here as a DLL.
        include(FetchContent)
        set(SDL_SHARED ON CACHE BOOL "" FORCE)
        set(SDL_STATIC OFF CACHE BOOL "" FORCE)
        set(SDL_TEST OFF CACHE BOOL "" FORCE)
        # With the C runtime: built without it (SDL's default for MSVC), clang-cl's arm64 calls to
        # strlen and wcslen have nothing to link to.
        set(SDL_LIBC ON CACHE BOOL "" FORCE)
        set(SDL2_DISABLE_INSTALL ON CACHE BOOL "" FORCE)
        FetchContent_Declare(sdl2
            URL https://github.com/libsdl-org/SDL/releases/download/release-2.32.10/SDL2-2.32.10.tar.gz
            URL_HASH SHA256=5f5993c530f084535c65a6879e9b26ad441169b3e25d789d83287040a9ca5165)
        FetchContent_MakeAvailable(sdl2)
        set(RT64_SDL2_FROM_PARENT ON)
        set(SDL2_INCLUDE_DIRS "${sdl2_SOURCE_DIR}/include")
        set(SDL2_LIBRARIES SDL2::SDL2 SDL2::SDL2main)
    endif()
    add_subdirectory(lib/rt64 EXCLUDE_FROM_ALL)
    if(APPLE)
        # RT64 turns plume's Retina support off, which sizes the swap chain in points and leaves macOS
        # to upscale it on a Retina screen. The window is sized in pixels (src/platform/window_sdl.cpp),
        # so the swap chain should be too.
        target_compile_definitions(plume PRIVATE PLUME_APPLE_RETINA_ENABLED)
    endif()
    target_sources(ConkersBadFurDayRecompiled PRIVATE
        src/platform/renderer_rt64.cpp src/platform/object_picker_rt64.cpp src/net/lobby_ui.cpp src/platform/font_rects.cpp src/enhancements/sharp_font/sharp_font.cpp src/enhancements/sharp_menus/sharp_menus.cpp src/platform/window_sdl.cpp src/platform/hang_watch.cpp src/platform/input_sdl.cpp
        src/enhancements/pad_icons/pad_icons.cpp src/enhancements/pad_icons/b_pad.cpp src/enhancements/pad_icons/stone_pad.cpp src/enhancements/pad_icons/hex_pad.cpp src/enhancements/pad_icons/square_pad.cpp src/enhancements/pad_icons/oval_pad.cpp src/enhancements/pad_icons/rock_pad.cpp src/enhancements/pad_icons/rug_pad.cpp src/enhancements/pad_icons/plate_pad.cpp src/enhancements/pad_icons/crate_pad.cpp src/enhancements/pad_icons/block_pad.cpp src/enhancements/pad_icons/disc_pad.cpp src/enhancements/pad_icons/rust_pad.cpp src/enhancements/pad_icons/riveted_pad.cpp src/enhancements/pad_icons/panel_pad.cpp src/enhancements/pad_icons/tile_pad.cpp
        src/enhancements/pad_icons/button_art.cpp)
    if(WIN32 AND CONKER_WIN_ARM64)
        # The DLLs the game needs to start, next to it: SDL2 (built above), and DXC for RT64's D3D12
        # shaders.
        add_custom_command(TARGET ConkersBadFurDayRecompiled POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "$<TARGET_FILE:SDL2::SDL2>"
                "${RT64_WIN_DXC_DIR}/bin/arm64/dxcompiler.dll"
                "${RT64_WIN_DXC_DIR}/bin/arm64/dxil.dll"
                "$<TARGET_FILE_DIR:ConkersBadFurDayRecompiled>")
    elseif(WIN32)
        # RT64 brings SDL2 for Windows, and sets these for its own folder only.
        set(CONKER_SDL2_DIR "${CMAKE_SOURCE_DIR}/lib/rt64/src/contrib/mupen64plus-win32-deps/SDL2-2.26.3")
        set(SDL2_INCLUDE_DIRS "${CONKER_SDL2_DIR}/include")
        target_link_directories(ConkersBadFurDayRecompiled PRIVATE "${CONKER_SDL2_DIR}/lib/x64")
        # The DLLs the game needs to start, next to it: SDL2, and DXC for RT64's D3D12 shaders (the same
        # DXC that compiled them, RT64_WIN_DXC_DIR).
        if(NOT RT64_WIN_DXC_DIR)
            set(RT64_WIN_DXC_DIR "${CMAKE_SOURCE_DIR}/lib/rt64/src/contrib/dxc")
        endif()
        add_custom_command(TARGET ConkersBadFurDayRecompiled POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${CONKER_SDL2_DIR}/lib/x64/SDL2.dll"
                "${RT64_WIN_DXC_DIR}/bin/x64/dxcompiler.dll"
                "${RT64_WIN_DXC_DIR}/bin/x64/dxil.dll"
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
    if(CONKER_BUNDLED_SDL2 AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
        # The game finds its SDL2 in libs/ beside it, in the build folder as in the tarball (not lib/,
        # which holds the submodules' build folders here).
        set_target_properties(ConkersBadFurDayRecompiled PROPERTIES
            BUILD_WITH_INSTALL_RPATH ON INSTALL_RPATH "\$ORIGIN/libs")
        add_custom_command(TARGET ConkersBadFurDayRecompiled POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:ConkersBadFurDayRecompiled>/libs"
            COMMAND ${CMAKE_COMMAND} -E copy "$<TARGET_FILE:SDL2::SDL2>"
                "$<TARGET_FILE_DIR:ConkersBadFurDayRecompiled>/libs/$<TARGET_SONAME_FILE_NAME:SDL2::SDL2>")
    endif()
    if(APPLE)
        # DualSense haptic effects and adaptive triggers through Apple's GameController framework
        # and Core Haptics.
        enable_language(OBJCXX)
        target_sources(ConkersBadFurDayRecompiled PRIVATE src/platform/haptics_apple.mm src/platform/adaptive_triggers_apple.mm)
        set_source_files_properties(src/platform/haptics_apple.mm PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
        target_link_libraries(ConkersBadFurDayRecompiled PRIVATE "-framework GameController" "-framework CoreHaptics")
        # Started from the desktop: data folder, log and ROM panel (src/platform/launch.hpp).
        target_sources(ConkersBadFurDayRecompiled PRIVATE src/platform/launch_apple.mm src/platform/crash_log.cpp)
        set_source_files_properties(src/platform/launch_apple.mm PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
        target_compile_definitions(ConkersBadFurDayRecompiled PRIVATE CONKER_LAUNCH_UI=1)
        target_link_libraries(ConkersBadFurDayRecompiled PRIVATE "-framework AppKit")
    elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux" OR WIN32)
        # Started from the desktop: data folder, log and the ROM dialog RT64 builds (nfd: GTK on
        # Linux, the common item dialog on Windows).
        if(WIN32)
            target_sources(ConkersBadFurDayRecompiled PRIVATE src/platform/launch_windows.cpp src/platform/launch_nfd.cpp src/platform/crash_log.cpp)
        else()
            target_sources(ConkersBadFurDayRecompiled PRIVATE src/platform/launch_linux.cpp src/platform/launch_nfd.cpp src/platform/crash_log.cpp)
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
# Sealing the lobby's and the session's packets (src/net/net_seal.cpp): Monocypher's X25519,
# BLAKE2b and XChaCha20-Poly1305, BSD-2-Clause or CC0, fetched at configure time at a pinned version.
# ---------------------------------------------------------------------------------------------
include(FetchContent)
FetchContent_Declare(monocypher
    URL https://github.com/LoupVaillant/Monocypher/archive/refs/tags/4.0.2.tar.gz
    URL_HASH SHA256=bc1ca30b1b2654e4e7daf2492c0d204200e55137f23fda6b7142fd7d523bd6b4
    SOURCE_SUBDIR no-cmake-build
    EXCLUDE_FROM_ALL)
FetchContent_MakeAvailable(monocypher)
add_library(conker_monocypher STATIC "${monocypher_SOURCE_DIR}/src/monocypher.c")
target_include_directories(conker_monocypher PUBLIC "${monocypher_SOURCE_DIR}/src")
target_compile_options(conker_monocypher PRIVATE -w)
target_link_libraries(ConkersBadFurDayRecompiled PRIVATE conker_monocypher)

# ---------------------------------------------------------------------------------------------
# Opening an online host's port on the router (src/net/net_portmap.cpp): miniupnpc for UPnP IGD
# and libnatpmp for NAT-PMP, both BSD-3-Clause, fetched at configure time at pinned versions.
# ---------------------------------------------------------------------------------------------
option(CONKER_PORT_MAPPING "Open an online host's port on the router with UPnP and NAT-PMP" ON)
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
        PATCH_COMMAND ${CMAKE_COMMAND} -DSOURCE=<SOURCE_DIR> -P ${CMAKE_SOURCE_DIR}/tools/patch_miniupnpc.cmake
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
    # Sockets and the network adapter list (src/net/net_*.cpp), random numbers for keys (net_seal.cpp),
    # crash backtraces (src/main.cpp).
    target_link_libraries(ConkersBadFurDayRecompiled PRIVATE ws2_32 iphlpapi bcrypt dbghelp)
elseif(APPLE)
    target_compile_definitions(ConkersBadFurDayRecompiled PRIVATE CONKER_PLATFORM_POSIX=1)
elseif(UNIX)
    target_compile_definitions(ConkersBadFurDayRecompiled PRIVATE CONKER_PLATFORM_POSIX=1)
    target_link_libraries(ConkersBadFurDayRecompiled PRIVATE rt)
endif()
