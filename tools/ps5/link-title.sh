#!/usr/bin/env bash
# Links the PS5 title: CMake's rule for every executable on the console (tools/ps5/toolchain.cmake).
#
#   link-title.sh <output> <objects and libraries, as CMake passes them>
#
# Writes <output>, the signed title (eboot.bin), and <output>.elf, the same program unsigned and with
# its symbols, for reading crash reports. Needs PS5_PAYLOAD_SDK, PACBREW and PS5_VULKAN (the
# toolchain sets them); RADV_ARCHIVE names another RADV archive than PS5_Vulkan's release build.
#
# The link is PS5_Vulkan's recipe for a title with RADV built in (its tools/build-radv-title.sh and
# tools/radv-link.sh): its entry point and C++ runtime glue (tooling/native), stub libraries naming the
# AGC imports, its linker script and version script, RADV linked whole with the SDK's libc++ and
# platform layer, and ps5-native-tool to convert and sign. On top of it come this port's own
# pieces: the path layer (ps5_paths.c), the C library additions (ps5_libc.c) and the import table.
set -euo pipefail

output=$1
shift
here="$(cd "$(dirname "$0")" && pwd)"
sdk=${PS5_PAYLOAD_SDK:?PS5_PAYLOAD_SDK is not set}
homebrew=${PACBREW:?PACBREW is not set}/user/homebrew
vulkan=${PS5_VULKAN:?PS5_VULKAN is not set}
radv_archive=${RADV_ARCHIVE:-$vulkan/.deps/native/radv-release/lib/libvulkan_radeon.ps5.a}
native="$vulkan/tooling/native"
tool="$vulkan/build/host/ps5-native-tool"
work="$output.link"
mkdir -p "$work"

# The loader and container constants of PS5_Vulkan's titles (its tools/build-radv-title.sh).
module_sdk=0x02000009
companion_sdk=0x08050001
fself_magic=0x1D3D154F

# The C library functions whose paths the path layer resolves (ps5_paths.c). opendir and access are
# not among them: the recipe binds both to the SDK platform layer's own (ps5_opendir, ps5_access),
# which open and stat the path, and those are wrapped.
path_functions=(chdir getcwd stat lstat open fopen mkdir rmdir unlink remove rename truncate chmod realpath)

# ---------------------------------------------------------------------------------------------
# What CMake passes: objects and archives as they are; -l names that are PacBrew's become its archives;
# the system's and the C runtime's come from the recipe below.
# ---------------------------------------------------------------------------------------------
inputs=()
uses_freetype=false
add_library_name() {
    local name=$1
    case "$name" in
        # The C runtime, threads and C++ runtime: the recipe's (the SDK's libc++, libc++abi, libunwind).
        c|m|pthread|dl|rt|stdc++|c++|c++abi|unwind|gcc|gcc_s) return ;;
    esac
    if [ -f "$homebrew/lib/lib$name.a" ]; then
        inputs+=("$homebrew/lib/lib$name.a")
        [ "$name" != freetype ] || uses_freetype=true
    elif [ -f "$sdk/target/lib/lib$name.so" ]; then
        :  # a system module: every SDK stub is linked below, as needed
    else
        echo "link-title.sh: no library for -l$name (neither PacBrew's nor the SDK's)" >&2
        exit 1
    fi
}
for arg in "$@"; do
    case "$arg" in
        -l*) add_library_name "${arg#-l}" ;;
        -L*|-Wl,-rpath*|-Wl,--rpath*|-pthread|-rdynamic|-fPIC|-fPIE|-pie|-O*|-g*|-m*) ;;
        -Wl,*)
            # Linker options from the build (none are needed today): passed on to lld.
            IFS=, read -r -a options <<< "${arg#-Wl,}"
            inputs+=("${options[@]}")
            ;;
        -*) echo "link-title.sh: ignoring $arg" >&2 ;;
        *) inputs+=("$arg") ;;
    esac
done
if $uses_freetype; then
    # PacBrew's FreeType is built with PNG and bzip2 support, and libpng needs zlib.
    inputs+=("$homebrew/lib/libpng16.a" "$homebrew/lib/libbz2.a" "$homebrew/lib/libz.a")
fi

for file in "$radv_archive" "$tool" "$sdk/bin/prospero-lld" "$native/app_crt.cpp"; do
    [ -e "$file" ] || { echo "link-title.sh: missing $file (tools/ps5/build.sh deps)" >&2; exit 1; }
done

# ---------------------------------------------------------------------------------------------
# The objects of the title's own: the recipe's start-up and C++ glue, the AGC import stubs, the path
# layer and the C library additions. Rebuilt only when their sources change.
# ---------------------------------------------------------------------------------------------
cc() { PS5_PAYLOAD_SDK="$sdk" sh "$vulkan/tooling/prospero-clang18" "$@"; }
build_object() {
    local source=$1 object=$2
    shift 2
    if [ ! -f "$object" ] || [ "$source" -nt "$object" ] || [ "$0" -nt "$object" ]; then
        cc "$@" -c "$source" -o "$object"
    fi
}
cxx_flags=(-std=c++20 -O2 -fno-exceptions -fno-rtti -ffunction-sections -fdata-sections)
c_flags=(-std=c11 -O2 -fPIC -ffunction-sections -fdata-sections -Wall -Wextra)
build_object "$native/app_crt.cpp" "$work/app_crt.o" "${cxx_flags[@]}"
build_object "$native/app_cpp_runtime.cpp" "$work/app_cpp_runtime.o" "${cxx_flags[@]}"
build_object "$here/ps5_paths.c" "$work/ps5_paths.o" "${c_flags[@]}"
build_object "$here/ps5_libc.c" "$work/ps5_libc.o" "${c_flags[@]}"
# AGC comes from system modules; these stubs only name the imports (RADV's winsys calls them).
stub_library() {
    local library=$1 source=$2
    if [ ! -f "$work/$library.so" ] || [ "$source" -nt "$work/$library.so" ]; then
        cc -std=c11 -O2 -fPIC -c "$source" -o "$work/${library}_stub.o"
        "$sdk/bin/prospero-lld" --shared -soname "$library.prx" -o "$work/$library.so" "$work/${library}_stub.o"
    fi
}
stub_library libSceAgc "$vulkan/vendor/ps5/sdk/stubs/agc_canary_link_stub.c"
stub_library libSceAgcDriver "$vulkan/vendor/ps5/sdk/stubs/agc_driver_canary_link_stub.c"

# The recipe's RADV inputs and flags, which radv_link_recipe sets.
radv_linker_script=()
radv_link_inputs=()
radv_link_flags=()
# shellcheck source=/dev/null
source "$vulkan/tools/radv-link.sh"
radv_link_recipe "$vulkan" "$sdk" "$radv_archive"

# The path layer's wraps, and a version script keeping everything this port defines local: a symbol
# a system module's stub names would otherwise become an export.
flags=("${radv_link_flags[@]}")
local_names=(___mb_cur_max posix_madvise timegm conker_ps5_import_addrs conker_ps5_import_names conker_ps5_import_count)
for name in "${path_functions[@]}"; do
    flags+=("--wrap=$name")
    local_names+=("__wrap_$name")
done
{
    printf '{\n    local:\n'
    printf '        %s;\n' "${local_names[@]}"
    printf '};\n'
} > "$work/title-local.map"

link_pie() {
    local table=$1 out=$2
    "$sdk/bin/prospero-lld" "${radv_linker_script[@]}" --eh-frame-hdr "${flags[@]}" \
        --version-script "$native/app-symbols.map" --version-script "$work/title-local.map" \
        --exclude-libs=ALL -e _start -o "$out" \
        "$work/app_crt.o" "$work/app_cpp_runtime.o" "$work/ps5_paths.o" "$work/ps5_libc.o" "$table" \
        "${inputs[@]}" "$work/libSceAgc.so" "$work/libSceAgcDriver.so" \
        "${radv_link_inputs[@]}" \
        --as-needed "$sdk"/target/lib/*.so
}

# Every undefined dynamic symbol of a linked program: its imports. "U name" or "w name" (weak).
imports_of() {
    "$sdk/bin/llvm-nm" -D --undefined-only "$1" | awk 'NF == 2 { print $1, $2 }' | sort -k2
}

# ---------------------------------------------------------------------------------------------
# The import table: every import by name and by its address once loaded, which is 0 for one the
# console lacks (src/platform/ps5_platform.cpp logs those). Its names are only known once the title
# is linked, so it is linked twice: first with an empty table, then with the table of the imports
# that link has. The table takes no import of its own, so the second link has the same imports.
# Many of the names are compiler built-ins (memcpy, strlen...), which C cannot declare under their own
# names with another type: each is declared under a name of ours bound to the import's with an asm
# label. Every reference is weak: a weak reference pulls no archive member in, so the table cannot
# turn an import into a definition linked from an archive (the SDK's libunwind has _Unwind_Backtrace).
# ---------------------------------------------------------------------------------------------
declare -A wrapped=()
for flag in "${flags[@]}"; do
    [[ "$flag" != --wrap=* ]] || wrapped[${flag#--wrap=}]=1
done

write_table() {
    local list=$1 source=$2
    {
        echo "// Generated by tools/ps5/link-title.sh: the title's imports."
        local i=0 name symbol
        while read -r _ name; do
            # A wrapped name (--wrap) reaches the import as __real_<name>; <name> itself is the wrapper.
            symbol=$name
            [ -z "${wrapped[$name]:-}" ] || symbol=__real_$name
            echo "extern char conker_import_$i __asm__(\"$symbol\") __attribute__((weak));"
            i=$((i + 1))
        done < "$list"
        echo "const void* const conker_ps5_import_addrs[] = {"
        for ((j = 0; j < i; j++)); do echo "    &conker_import_$j,"; done
        echo "    0,"
        echo "};"
        echo "const char* const conker_ps5_import_names[] = {"
        while read -r _ name; do echo "    \"$name\","; done < "$list"
        echo "    0,"
        echo "};"
        echo "const unsigned int conker_ps5_import_count = $i;"
    } > "$source"
}

: > "$work/no-imports.txt"
write_table "$work/no-imports.txt" "$work/import_table_empty.c"
cc "${c_flags[@]}" -c "$work/import_table_empty.c" -o "$work/import_table_empty.o"
link_pie "$work/import_table_empty.o" "$work/first.elf"
imports_of "$work/first.elf" > "$work/imports.txt"

write_table "$work/imports.txt" "$work/import_table.c"
cc "${c_flags[@]}" -c "$work/import_table.c" -o "$work/import_table.o"
link_pie "$work/import_table.o" "$work/title.elf"
imports_of "$work/title.elf" > "$work/imports-final.txt"
if ! cmp -s "$work/imports.txt" "$work/imports-final.txt"; then
    echo "link-title.sh: the import table changed the title's imports:" >&2
    diff "$work/imports.txt" "$work/imports-final.txt" >&2 || true
    exit 1
fi
echo "link-title.sh: $(wc -l < "$work/imports.txt" | tr -d ' ') imports"

# ---------------------------------------------------------------------------------------------
# Converted to the console's executable layout and signed, as PS5_Vulkan's titles are.
# ---------------------------------------------------------------------------------------------
"$tool" link --in "$work/title.elf" --out "$work/eboot.elf" \
    --stub-dir "$sdk/target/lib" --stub "$work/libSceAgc.so" --stub "$work/libSceAgcDriver.so" \
    --module-sdk "$module_sdk" --companion-sdk "$companion_sdk" --file-name eboot.elf
"$tool" self --sign --in "$work/eboot.elf" --out "$output" --magic "$fself_magic"
cp "$work/title.elf" "$output.elf"
rm -f "$work/first.elf"
