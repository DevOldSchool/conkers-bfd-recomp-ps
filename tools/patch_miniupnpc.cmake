# Run as miniupnpc's FetchContent PATCH_COMMAND (CMakeLists.txt): cmake -DSOURCE=<its source> -P this.
# Under MSVC, src/addr_is_reserved.c declares its own `typedef unsigned long uint32_t`, which clashes
# with the Windows SDK's stdint.h on arm64, where winsock2.h already brings it in. Every compiler we
# build with has <stdint.h>, so the file includes it instead. Safe to run again.
set(file "${SOURCE}/miniupnpc/src/addr_is_reserved.c")
file(READ "${file}" text)
string(REPLACE "typedef unsigned long uint32_t;" "#include <stdint.h>" text "${text}")
file(WRITE "${file}" "${text}")
