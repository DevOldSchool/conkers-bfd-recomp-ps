# Mod sandbox tests: no ROM, builds only the patched N64Recomp (for LiveRecomp) and headers.
#   cmake -S tests/mod_sandbox -B build/mod_sandbox -G Ninja && cmake --build build/mod_sandbox
#   build/mod_sandbox/mod_sandbox_test
# Run tools/apply_patches.sh first: the tests check what the patches do.
cmake_minimum_required(VERSION 3.20)
project(mod_sandbox_test LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(runtime "${CMAKE_CURRENT_SOURCE_DIR}/../../lib/N64ModernRuntime")
add_subdirectory("${runtime}/N64Recomp" N64Recomp EXCLUDE_FROM_ALL)

add_executable(mod_sandbox_test main.cpp address_window.cpp to_ptr.cpp save_file_name.cpp)
target_include_directories(mod_sandbox_test PRIVATE
    "${runtime}/N64Recomp/include"
    "${runtime}/ultramodern/include"
    "${runtime}/librecomp/include"
)
target_link_libraries(mod_sandbox_test PRIVATE LiveRecomp)
