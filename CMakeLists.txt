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
add_executable(ConkerRecomp
    src/main.cpp
    src/game.cpp
    src/memory.cpp
    src/overlays.cpp
    src/renderer_null.cpp
    src/rsp.cpp
    src/rzip.cpp
    src/trace.cpp
    src/ultra_extras.cpp
)
target_include_directories(ConkerRecomp PRIVATE
    "${CMAKE_SOURCE_DIR}/src"
    "${CMAKE_SOURCE_DIR}/lib/N64ModernRuntime/N64Recomp/include"
    "${CMAKE_SOURCE_DIR}/lib/N64ModernRuntime/thirdparty/miniz"
)
target_link_libraries(ConkerRecomp PRIVATE ConkerRecompiled librecomp ultramodern miniz)
if(APPLE)
    target_compile_definitions(ConkerRecomp PRIVATE CONKER_PLATFORM_POSIX=1)
elseif(UNIX)
    target_compile_definitions(ConkerRecomp PRIVATE CONKER_PLATFORM_POSIX=1)
    target_link_libraries(ConkerRecomp PRIVATE rt)
endif()
