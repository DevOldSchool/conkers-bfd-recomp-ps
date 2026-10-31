# CMake toolchain for cross-compiling the Windows (x64) build on macOS or Linux with clang-cl, lld-link
# and the MSVC CRT and Windows SDK that xwin downloads (tools/windows/cross_build.sh).
#
#   XWIN_DIR  folder xwin splatted into (holds crt/ and sdk/)
#   LLVM_DIR  folder with clang-cl, llvm-lib, llvm-rc and llvm-mt; lld-link there or on the PATH
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_VERSION 10.0)
set(CMAKE_SYSTEM_PROCESSOR AMD64)

set(XWIN_DIR "$ENV{XWIN_DIR}" CACHE PATH "xwin splat folder")
set(LLVM_DIR "$ENV{LLVM_DIR}" CACHE PATH "LLVM tools folder")
if(NOT XWIN_DIR OR NOT LLVM_DIR)
    message(FATAL_ERROR "Set XWIN_DIR and LLVM_DIR (see tools/windows/cross_build.sh)")
endif()

set(CMAKE_C_COMPILER "${LLVM_DIR}/clang-cl")
set(CMAKE_CXX_COMPILER "${LLVM_DIR}/clang-cl")
find_program(CMAKE_LINKER lld-link HINTS "${LLVM_DIR}" REQUIRED)
set(CMAKE_AR "${LLVM_DIR}/llvm-lib")
set(CMAKE_RC_COMPILER "${LLVM_DIR}/llvm-rc")
set(CMAKE_MT "${LLVM_DIR}/llvm-mt")

set(_includes "")
foreach(dir crt/include sdk/include/ucrt sdk/include/um sdk/include/shared sdk/include/winrt)
    string(APPEND _includes " /imsvc${XWIN_DIR}/${dir}")
endforeach()
set(CMAKE_C_FLAGS_INIT "--target=x86_64-pc-windows-msvc -fuse-ld=lld-link${_includes}")
set(CMAKE_CXX_FLAGS_INIT "--target=x86_64-pc-windows-msvc -fuse-ld=lld-link${_includes}")
set(_libs "/libpath:${XWIN_DIR}/crt/lib/x86_64 /libpath:${XWIN_DIR}/sdk/lib/um/x86_64 /libpath:${XWIN_DIR}/sdk/lib/ucrt/x86_64")
set(CMAKE_EXE_LINKER_FLAGS_INIT "${_libs}")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "${_libs}")
set(CMAKE_MODULE_LINKER_FLAGS_INIT "${_libs}")

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
