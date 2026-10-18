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

add_executable(ConkerRecomp
    src/main.cpp
    src/audio.cpp
    "${CONKER_RSP_DIR}/aspMain.cpp"
    src/game.cpp
    src/memory.cpp
    src/overlays.cpp
    src/rsp.cpp
    src/rzip.cpp
    src/trace.cpp
    src/hooks.cpp
    src/pause_save.cpp
    src/resume_point.cpp
    src/ultra_extras.cpp
)
target_include_directories(ConkerRecomp PRIVATE
    "${CMAKE_SOURCE_DIR}/src"
    "${CMAKE_SOURCE_DIR}/lib/N64ModernRuntime/N64Recomp/include"
    "${CMAKE_SOURCE_DIR}/lib/N64ModernRuntime/thirdparty/miniz"
)
target_link_libraries(ConkerRecomp PRIVATE ConkerRecompiled librecomp ultramodern miniz)
# The microcode runs once per audio frame on the audio thread; keep it optimised in Debug builds.
set_source_files_properties("${CONKER_RSP_DIR}/aspMain.cpp" PROPERTIES COMPILE_OPTIONS "-O2;-w")

# ---------------------------------------------------------------------------------------------
# Renderer
# ---------------------------------------------------------------------------------------------
if(CONKER_RENDERER STREQUAL "rt64")
    set(RT64_STATIC ON CACHE BOOL "" FORCE)
    add_subdirectory(lib/rt64 EXCLUDE_FROM_ALL)
    target_sources(ConkerRecomp PRIVATE src/renderer_rt64.cpp src/window_sdl.cpp src/input_sdl.cpp)
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
else()
    target_sources(ConkerRecomp PRIVATE src/renderer_null.cpp src/window_headless.cpp src/input_headless.cpp)
endif()

if(APPLE)
    target_compile_definitions(ConkerRecomp PRIVATE CONKER_PLATFORM_POSIX=1)
elseif(UNIX)
    target_compile_definitions(ConkerRecomp PRIVATE CONKER_PLATFORM_POSIX=1)
    target_link_libraries(ConkerRecomp PRIVATE rt)
endif()
