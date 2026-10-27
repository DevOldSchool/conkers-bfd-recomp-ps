cmake_minimum_required(VERSION 3.20)

project(ConkerRecomp LANGUAGES C CXX)

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
# src/enhancements: one folder per feature the console does not have, each with its own switch.
add_executable(ConkerRecomp
    src/main.cpp
    src/runtime/game.cpp
    src/runtime/heap_check.cpp
    src/runtime/memory.cpp
    src/runtime/overlays.cpp
    src/runtime/rsp.cpp
    src/runtime/rzip.cpp
    src/runtime/trace.cpp
    src/runtime/ultra_extras.cpp
    "${CONKER_RSP_DIR}/aspMain.cpp"
    src/platform/audio.cpp
    src/platform/display_mode.cpp
    src/platform/gpu_faults.cpp
    src/fixes/geometry_escape.cpp
    src/fixes/frame_effects.cpp
    src/fixes/level_seams.cpp
    src/fixes/music_start.cpp
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
    src/enhancements/display_options/widescreen_timer.cpp
    src/enhancements/frame_rate/frame_rate.cpp
    src/enhancements/intro_skip/intro_skip.cpp
    src/enhancements/max_detail/max_detail.cpp
    src/enhancements/pause_save/pause_save.cpp
    src/enhancements/pause_save/resume_point.cpp
    src/enhancements/pause_save/save_check.cpp
    src/enhancements/rumble_banner/rumble_banner.cpp
    src/enhancements/haptics/haptics.cpp
    src/enhancements/adaptive_triggers/adaptive_triggers.cpp
    src/enhancements/consistent_controls/consistent_controls.cpp
    src/enhancements/consistent_controls/mouse_look.cpp
    src/enhancements/sharp_menus/menu_art.cpp
    src/enhancements/sharp_menus/pause_signs.cpp
    src/enhancements/surround_default/surround_default.cpp
)
target_include_directories(ConkerRecomp PRIVATE
    "${CMAKE_SOURCE_DIR}/src"
    "${CMAKE_SOURCE_DIR}/lib/N64ModernRuntime/N64Recomp/include"
    "${CMAKE_SOURCE_DIR}/lib/N64ModernRuntime/thirdparty/miniz"
)
target_link_libraries(ConkerRecomp PRIVATE ConkerRecompiled librecomp ultramodern miniz)
# The build a debug report names (src/enhancements/debug_menu): build_version.h, from git describe
# on every build (tools/build_version.cmake).
set(CONKER_GENERATED_DIR "${CMAKE_BINARY_DIR}/generated")
add_custom_target(build_version
    COMMAND ${CMAKE_COMMAND} -DSOURCE=${CMAKE_SOURCE_DIR} -DOUT=${CONKER_GENERATED_DIR}/build_version.h
            -P ${CMAKE_SOURCE_DIR}/tools/build_version.cmake
    BYPRODUCTS ${CONKER_GENERATED_DIR}/build_version.h)
add_dependencies(ConkerRecomp build_version)
# The JSON library the runtime ships (nlohmann/json), for debug reports.
target_include_directories(ConkerRecomp PRIVATE "${CONKER_GENERATED_DIR}"
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
    add_subdirectory(lib/rt64 EXCLUDE_FROM_ALL)
    if(APPLE)
        # RT64 turns plume's Retina support off, which sizes the swap chain in points and leaves macOS
        # to upscale it on a Retina screen. The window is sized in pixels (src/platform/window_sdl.cpp),
        # so the swap chain should be too.
        target_compile_definitions(plume PRIVATE PLUME_APPLE_RETINA_ENABLED)
    endif()
    target_sources(ConkerRecomp PRIVATE
        src/platform/renderer_rt64.cpp src/platform/object_picker_rt64.cpp src/platform/font_rects.cpp src/enhancements/sharp_font/sharp_font.cpp src/enhancements/sharp_menus/sharp_menus.cpp src/platform/window_sdl.cpp src/platform/hang_watch.cpp src/platform/input_sdl.cpp
        src/enhancements/pad_icons/pad_icons.cpp src/enhancements/pad_icons/b_pad.cpp src/enhancements/pad_icons/stone_pad.cpp src/enhancements/pad_icons/hex_pad.cpp src/enhancements/pad_icons/square_pad.cpp src/enhancements/pad_icons/oval_pad.cpp src/enhancements/pad_icons/rock_pad.cpp src/enhancements/pad_icons/rug_pad.cpp src/enhancements/pad_icons/plate_pad.cpp src/enhancements/pad_icons/crate_pad.cpp src/enhancements/pad_icons/block_pad.cpp src/enhancements/pad_icons/disc_pad.cpp src/enhancements/pad_icons/rust_pad.cpp src/enhancements/pad_icons/riveted_pad.cpp
        src/enhancements/pad_icons/button_art.cpp)
    target_include_directories(ConkerRecomp PRIVATE
        ${SDL2_INCLUDE_DIRS}
        "${CMAKE_SOURCE_DIR}/lib/rt64/src"
        "${CMAKE_SOURCE_DIR}/lib/rt64/src/contrib"
        "${CMAKE_SOURCE_DIR}/lib/rt64/src/contrib/plume"
        "${CMAKE_SOURCE_DIR}/lib/rt64/src/contrib/hlslpp/include"
        "${CMAKE_SOURCE_DIR}/lib/rt64/src/contrib/imgui"
    )
    target_compile_definitions(ConkerRecomp PRIVATE CONKER_RENDERER_RT64=1 CONKER_AUDIO_SDL=1 HLSL_CPU FFX_GCC)
    target_link_libraries(ConkerRecomp PRIVATE rt64 ${SDL2_LIBRARIES})
    if(APPLE)
        # DualSense haptic effects and adaptive triggers through Apple's GameController framework
        # and Core Haptics.
        enable_language(OBJCXX)
        target_sources(ConkerRecomp PRIVATE src/platform/haptics_apple.mm src/platform/adaptive_triggers_apple.mm)
        set_source_files_properties(src/platform/haptics_apple.mm PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
        target_link_libraries(ConkerRecomp PRIVATE "-framework GameController" "-framework CoreHaptics")
    endif()
else()
    target_sources(ConkerRecomp PRIVATE
        src/platform/renderer_null.cpp src/platform/window_headless.cpp src/platform/input_headless.cpp)
endif()

if(APPLE)
    target_compile_definitions(ConkerRecomp PRIVATE CONKER_PLATFORM_POSIX=1)
elseif(UNIX)
    target_compile_definitions(ConkerRecomp PRIVATE CONKER_PLATFORM_POSIX=1)
    target_link_libraries(ConkerRecomp PRIVATE rt)
endif()
