# Writes OUT, a header defining CONKER_BUILD_VERSION as `git describe --always --dirty` of SOURCE, run
# on every build (the build_version target in CMakeLists.txt). The file is only rewritten when the
# version changes, so nothing is recompiled for nothing.
execute_process(COMMAND git describe --always --dirty WORKING_DIRECTORY "${SOURCE}"
    OUTPUT_VARIABLE version OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
if(NOT version)
    set(version "unknown")
endif()
set(content "#pragma once\n#define CONKER_BUILD_VERSION \"${version}\"\n")
if(EXISTS "${OUT}")
    file(READ "${OUT}" previous)
endif()
if(NOT previous STREQUAL content)
    file(WRITE "${OUT}" "${content}")
endif()
