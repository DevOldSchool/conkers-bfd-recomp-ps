# CMake toolchain for the PS5 title (tools/ps5/build.sh passes it): the payload SDK's own toolchain,
# PacBrew's prebuilt ports for the libraries, and the title link (tools/ps5/link-title.sh) as the rule
# for every executable.
#
#   PS5_PAYLOAD_SDK  the payload SDK (PS5_Vulkan's .deps/native/ps5-payload-sdk)
#   PACBREW          PacBrew's sysroot, holding its prefix user/homebrew
#   PS5_VULKAN       the PS5_Vulkan checkout: the title link's recipe, RADV and ps5-native-tool
#   CONKER_HOST_TOOLS the folder of tools built for the build machine (RT64's file_to_c)
#
# Each can come from the environment instead of -D.
foreach(name PS5_PAYLOAD_SDK PACBREW PS5_VULKAN CONKER_HOST_TOOLS)
    if(NOT ${name} AND DEFINED ENV{${name}})
        set(${name} "$ENV{${name}}")
    endif()
    if(NOT ${name})
        message(FATAL_ERROR "tools/ps5/toolchain.cmake needs ${name} (tools/ps5/build.sh sets it)")
    endif()
    set(${name} "${${name}}" CACHE PATH "" FORCE)
endforeach()
# try_compile projects read this file again, without the cache.
list(APPEND CMAKE_TRY_COMPILE_PLATFORM_VARIABLES PS5_PAYLOAD_SDK PACBREW PS5_VULKAN CONKER_HOST_TOOLS)

include("${PS5_PAYLOAD_SDK}/toolchain/prospero.cmake")
if(NOT PS5)
    set(PS5 TRUE)
endif()
# The SDK turns on every command's echo; the recompiled code is hundreds of them.
set(CMAKE_VERBOSE_MAKEFILE OFF CACHE BOOL "" FORCE)
set(CONKER_PS5_DIR "${CMAKE_CURRENT_LIST_DIR}" CACHE PATH "" FORCE)

# PacBrew's ports are installed under /user/homebrew of their own sysroot: found there, and before the
# SDK's (CMAKE_SYSROOT, which prospero.cmake sets).
list(PREPEND CMAKE_FIND_ROOT_PATH "${PACBREW}")
list(APPEND CMAKE_PREFIX_PATH "/user/homebrew")

# SDL2's headers as <SDL.h>, which RT64 and the game include, and the definitions RT64's and plume's
# headers need wherever they are included: the SDL window for Vulkan as on Linux. ZSTD_TRACE=0: zstd's
# tracing hooks are weak references nothing defines, which would become imports no system module has.
set(conker_ps5_flags "-isystem ${PACBREW}/user/homebrew/include/SDL2 -DPLUME_SDL_VULKAN_ENABLED -DRT64_SDL_WINDOW_VULKAN -DZSTD_TRACE=0")
set(CMAKE_C_FLAGS_INIT "${conker_ps5_flags}")
set(CMAKE_CXX_FLAGS_INIT "${conker_ps5_flags}")

# CMake's own checks build a library, not a program: every program is a title, linked below.
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

# The title link: link-title.sh <output> <objects and libraries>, with RADV and the SDK's runtime.
set(conker_ps5_link "${CMAKE_COMMAND} -E env PS5_PAYLOAD_SDK=${PS5_PAYLOAD_SDK} PACBREW=${PACBREW} PS5_VULKAN=${PS5_VULKAN} ${CMAKE_CURRENT_LIST_DIR}/link-title.sh <TARGET> <OBJECTS> <LINK_LIBRARIES> <LINK_FLAGS>")
set(CMAKE_C_LINK_EXECUTABLE "${conker_ps5_link}")
set(CMAKE_CXX_LINK_EXECUTABLE "${conker_ps5_link}")
