# Writes OUT, a header defining CONKER_BUILD_VERSION as the version tools/version.sh prints for SOURCE, run
# on every build (the build_version target in CMakeLists.txt). The file is only rewritten when the
# version changes, so nothing is recompiled for nothing.
#
# A CONKER_BUILD_VERSION environment variable, when set, is used instead: tools/linux/build.sh sets it
# from the host, because git cannot describe the source inside its container (the bind mount is owned
# by another user, and in a worktree .git points at a host path the container does not have).
if(DEFINED ENV{CONKER_BUILD_VERSION} AND NOT "$ENV{CONKER_BUILD_VERSION}" STREQUAL "")
    set(version "$ENV{CONKER_BUILD_VERSION}")
else()
    # As tools/version.sh: "-dirty" for changed tracked files here, not for the patched submodules.
    execute_process(COMMAND git describe --always WORKING_DIRECTORY "${SOURCE}"
        OUTPUT_VARIABLE version OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    execute_process(COMMAND git status --porcelain --untracked-files=no --ignore-submodules=dirty
        WORKING_DIRECTORY "${SOURCE}" OUTPUT_VARIABLE changes OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    if(version AND changes)
        set(version "${version}-dirty")
    endif()
endif()
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
