#!/usr/bin/env bash
# Build Conker's Bad Fur Day Recompiled as a PS5 homebrew title in a Docker container
# (tools/ps5/Dockerfile), into dist/PPSA92064 (docs/building-ps5.md).
#
#   tools/ps5/build.sh [deps|configure|build|package|all]    (default: all)
#
#   deps       once per dependency volume: Mihawk's PS5_Vulkan, PS5_Mesa and PS5_PayloadSDK at their
#              pins, the payload SDK, RADV (release), ps5-native-tool, libc.prx, PacBrew's sysroot
#   configure  RT64's file_to_c for the build machine, then CMake with tools/ps5/toolchain.cmake
#   build      the ConkersBadFurDayRecompiled target (the signed title, tools/ps5/link-title.sh)
#   package    dist/PPSA92064: eboot.bin, sce_sys (param.json, icon, home-screen pictures and music),
#              sce_module/libc.prx
#
# Runs on an Apple silicon Mac and on an x86-64 Linux machine alike. Environment:
#   CONKER_GENERATED=<dir>        the recompiled code (tools/regenerate.sh's build/us, default build/us)
#   CONKER_PS5_DEPS_VOLUME=<name> the dependency volume (default conker-ps5-deps)
#   CONKER_PS5_BUILD_VOLUME=<name> the build folder's volume (default conker-ps5-build)
#   CONKER_BUILD_ARGS=<args>      passed on to the build tool (ninja)
#   CONKER_PS5_SND0=<file.at9>    the home screen's music (default tools/ps5/sce_sys/snd0.at9)
#   CONKER_PS5_BACKGROUND=<file>  a 16:9 picture of your own for the home screen's background
#
# Docker volumes hold the dependencies and the build folder, so macOS file sharing is not in their
# way: the dependency volume at /deps, the build volume at build/ps5. Never packages a ROM.
set -euo pipefail

title_id=PPSA92064
image=conker-ps5-build

# Mihawk's repositories and PacBrew, at the revisions this port was built and tested with.
ps5_vulkan_url=https://github.com/mihawk-99/PS5_Vulkan.git
ps5_vulkan_rev=5b5e4fc2d80fdb67a4f61ba9e6a8a424026bb8a5
ps5_mesa_url=https://github.com/mihawk-99/PS5_Mesa.git
ps5_mesa_rev=7b59ef27c1b09b9671bc4153c41940c3155c3af2
ps5_sdk_url=https://github.com/mihawk-99/PS5_PayloadSDK.git
ps5_sdk_rev=b83202be73e930050e00de0f1b4d9ec46c0391bf
pacbrew_url=https://github.com/ps5-payload-dev/pacbrew-repo/releases/download/v0.40.2/ps5-payload-dev.tar.gz
pacbrew_hash=a85f65de418a8e6a898c6c3e3c870d50fff7618a200e4dd59ea9692af6ecec4d

# Where things are inside the container.
deps=/deps
vulkan=$deps/PS5_Vulkan
sdk=$vulkan/.deps/native/ps5-payload-sdk
radv=$vulkan/.deps/native/radv-release
native_tool=$vulkan/build/host/ps5-native-tool
pacbrew=$deps/pacbrew/sysroot
build=/src/build/ps5

# ---------------------------------------------------------------------------------------------
# Inside the container
# ---------------------------------------------------------------------------------------------

# A repository at one commit: fetched alone (the forks' histories are large) and checked out.
fetch_at() {
    local url=$1 rev=$2 dir=$3
    if [ "$(git -C "$dir" rev-parse HEAD 2>/dev/null || true)" = "$rev" ]; then
        return
    fi
    echo "==> [deps] $(basename "$dir") at ${rev:0:12}"
    if [ ! -d "$dir/.git" ]; then
        rm -rf "$dir"
        git init -q "$dir"
        git -C "$dir" remote add origin "$url"
    fi
    git -C "$dir" fetch -q --depth 1 origin "$rev" || { sleep 30; git -C "$dir" fetch -q --depth 1 origin "$rev"; }
    git -C "$dir" checkout -q --force "$rev"
}

# Mesa's wrap files fetch their meson patches from WrapDB's server, which answers with errors for hours
# at a time. The same files are release assets of WrapDB's GitHub repository: put there into the
# package cache (which tools/build-radv.sh keeps), checked against the wrap's hash, meson does not ask.
fetch_wrap_patches() {
    local subprojects=$1 wrap name version file hash
    [ -d "$subprojects" ] || return 0
    mkdir -p "$subprojects/packagecache"
    for wrap in "$subprojects"/*.wrap; do
        [ -f "$wrap" ] || continue
        name=$(basename "$wrap" .wrap)
        version=$(awk -F' *= *' '$1 == "wrapdb_version" { print $2 }' "$wrap")
        file=$(awk -F' *= *' '$1 == "patch_filename" { print $2 }' "$wrap")
        hash=$(awk -F' *= *' '$1 == "patch_hash" { print $2 }' "$wrap")
        if [ -z "$version" ] || [ -z "$file" ] || [ -z "$hash" ] || [ -f "$subprojects/packagecache/$file" ]; then
            continue
        fi
        echo "==> [deps] $file from WrapDB's GitHub releases"
        if curl -fsSL --retry 3 "https://github.com/mesonbuild/wrapdb/releases/download/${name}_$version/$file" \
                -o "$subprojects/packagecache/$file.download" &&
            echo "$hash  $subprojects/packagecache/$file.download" | sha256sum --check --strict --status; then
            mv "$subprojects/packagecache/$file.download" "$subprojects/packagecache/$file"
        else
            rm -f "$subprojects/packagecache/$file.download"
        fi
    done
}

radv_built() {
    grep -qx "revision: $ps5_mesa_rev" "$radv/PROVENANCE.txt" 2>/dev/null &&
        grep -qx "sdk: $ps5_sdk_rev" "$radv/PROVENANCE.txt" 2>/dev/null &&
        [ -f "$radv/lib/libvulkan_radeon.ps5.a" ]
}

in_deps() {
    mkdir -p "$deps"
    fetch_at "$ps5_vulkan_url" "$ps5_vulkan_rev" "$vulkan"
    fetch_at "$ps5_sdk_url" "$ps5_sdk_rev" "$deps/PS5_PayloadSDK"

    # The payload SDK (the v0.42 release with Mihawk's platform layer) and the zlib ps5-native-tool
    # links, by PS5_Vulkan's own setup. It skips what is there, so a download GitHub failed is simply
    # tried again.
    local attempt
    for attempt in 1 2 3; do
        if PS5_PAYLOAD_SDK_FORK="$deps/PS5_PayloadSDK" bash "$vulkan/tools/setup-native-dependencies.sh" > /dev/null; then
            break
        fi
        [ "$attempt" -lt 3 ] || { echo "the payload SDK's setup failed" >&2; exit 1; }
        echo "==> [deps] the payload SDK's setup failed; again in 30 s"
        sleep 30
    done

    # RADV, the release build titles ship (Mesa's assertions cost 5 to 6 times the shader compile time).
    if radv_built; then
        echo "==> [deps] RADV at ${ps5_mesa_rev:0:12} is built"
    else
        fetch_at "$ps5_mesa_url" "$ps5_mesa_rev" "$deps/PS5_Mesa"
        echo "==> [deps] building RADV (release); this takes a while"
        # Meson downloads a subproject while it configures, and a server's hiccup fails that: the
        # half-configured folder is removed and the build tried again.
        for attempt in 1 2 3; do
            if PS5_MESA_FORK="$deps/PS5_Mesa" bash "$vulkan/tools/build-radv.sh" release; then
                break
            fi
            [ "$attempt" -lt 3 ] || { echo "RADV did not build" >&2; exit 1; }
            [ -f "$vulkan/.deps/work/radv-build-ps5-release/build.ninja" ] ||
                rm -rf "$vulkan/.deps/work/radv-build-ps5-release"
            fetch_wrap_patches "$vulkan/.deps/work/radv-src/subprojects"
            echo "==> [deps] RADV's build failed; again in 60 s"
            sleep 60
        done
        radv_built || { echo "RADV did not build" >&2; exit 1; }
    fi

    # The host tool that converts and signs the title, built as PS5_Vulkan's tools/build.sh builds it.
    local zlib_root="$vulkan/.deps/native/zlib/root" zlib_archive
    zlib_archive=$(find "$zlib_root" -type f -name libz.a -print -quit)
    local native="$vulkan/tooling/native"
    if [ ! -x "$native_tool" ] || [ -n "$(find "$native" "$zlib_archive" -newer "$native_tool" -type f -print -quit)" ]; then
        echo "==> [deps] ps5-native-tool"
        mkdir -p "$(dirname "$native_tool")"
        clang++ -std=c++20 -O2 -Wall -Wextra -Werror -I "$zlib_root/usr/include" \
            "$native/native_app_builder.cpp" "$native/self_container.cpp" "$native/elf_object.cpp" \
            "$native/sce_module_writer.cpp" "$zlib_archive" -o "$native_tool"
    fi

    # sce_module/libc.prx, generated and checked against its recorded digest by PS5_Vulkan.
    if ! (cd "$vulkan/runtime" && sha256sum --check --strict --quiet libc.prx.sha256 > /dev/null 2>&1); then
        echo "==> [deps] runtime/libc.prx"
        bash "$vulkan/tools/rebuild-libc.sh" > /dev/null
    fi

    # PacBrew's prebuilt ports: the game takes SDL2 (and what it links) from its homebrew prefix. A
    # sysroot already there is kept.
    if [ ! -f "$pacbrew/user/homebrew/lib/libSDL2.a" ]; then
        echo "==> [deps] PacBrew v0.40.2"
        mkdir -p "$deps/pacbrew"
        local archive="$deps/pacbrew/ps5-payload-dev.tar.gz"
        if ! echo "$pacbrew_hash  $archive" | sha256sum --check --strict --status 2>/dev/null; then
            curl -fsSL --retry 5 --retry-all-errors "$pacbrew_url" -o "$archive.download"
            echo "$pacbrew_hash  $archive.download" | sha256sum --check --strict --status ||
                { echo "PacBrew's archive does not have the expected SHA-256" >&2; exit 1; }
            mv "$archive.download" "$archive"
        fi
        rm -rf "$pacbrew" "$deps/pacbrew/.extract"
        mkdir -p "$deps/pacbrew/.extract"
        tar -xzf "$archive" -C "$deps/pacbrew/.extract" --strip-components=3 opt/ps5-payload-sdk/target/user/homebrew
        mkdir -p "$pacbrew/user"
        mv "$deps/pacbrew/.extract/user/homebrew" "$pacbrew/user/homebrew"
        rm -rf "$deps/pacbrew/.extract" "$archive"
    fi
    [ -f "$pacbrew/user/homebrew/lib/libSDL2.a" ] || { echo "PacBrew's sysroot has no SDL2" >&2; exit 1; }
    echo "==> [deps] ready"
}

check_deps() {
    for file in "$sdk/toolchain/prospero.cmake" "$radv/lib/libvulkan_radeon.ps5.a" "$native_tool" \
            "$vulkan/runtime/libc.prx" "$pacbrew/user/homebrew/lib/libSDL2.a"; do
        [ -e "$file" ] || { echo "missing $file: run tools/ps5/build.sh deps" >&2; exit 1; }
    done
}

in_configure() {
    check_deps
    # The build machine's file_to_c: RT64 turns its shaders into C with it, and a cross build cannot
    # run the console's.
    mkdir -p "$build/host-tools"
    clang++ -std=c++17 -O2 /src/lib/rt64/src/tools/file_to_c/file_to_c.cpp -o "$build/host-tools/file_to_c"
    cmake -S /src -B "$build" -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo \
        -DCMAKE_TOOLCHAIN_FILE=/src/tools/ps5/toolchain.cmake \
        -DPS5_PAYLOAD_SDK="$sdk" -DPACBREW="$pacbrew" -DPS5_VULKAN="$vulkan" -DCONKER_HOST_TOOLS="$build/host-tools" \
        -DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache \
        -DCONKER_RENDERER=rt64 -DCONKER_RECOMPILED_DIR=/gen/RecompiledFuncs -DCONKER_RSP_DIR=/gen/rsp \
        -DCONKER_PORT_MAPPING=OFF -DCONKER_EMBEDDED_MODS=
}

in_build() {
    check_deps
    [ -f "$build/build.ninja" ] || { echo "$build is not configured: run tools/ps5/build.sh configure" >&2; exit 1; }
    # The recompiled code is large: a compile job per 2 GB of the container's memory.
    local memory_kb jobs
    memory_kb=$(awk '/^MemTotal:/ { print $2 }' /proc/meminfo)
    jobs=$(( memory_kb / 1024 / 1024 / 2 ))
    [ "$jobs" -gt 0 ] || jobs=1
    local args=()
    if [ -n "${CONKER_BUILD_ARGS:-}" ]; then
        read -r -a args <<< "$CONKER_BUILD_ARGS"
    fi
    echo "==> [build] $jobs jobs"
    cmake --build "$build" --target ConkersBadFurDayRecompiled -j "$jobs" -- "${args[@]}"
}

in_package() {
    check_deps
    local eboot="$build/ConkersBadFurDayRecompiled"
    [ -f "$eboot" ] || { echo "no $eboot: run tools/ps5/build.sh build" >&2; exit 1; }
    local snd0=/src/tools/ps5/sce_sys/snd0.at9
    [ -z "${CONKER_PS5_SND0:-}" ] || snd0=/snd0.at9
    [ -f "$snd0" ] || { echo "no home-screen music ($snd0)" >&2; exit 1; }

    # The home-screen pictures, made again only when the script or its picture changed.
    # The project's background (assets/background.webp, the one the desktop builds use), unless
    # CONKER_PS5_BACKGROUND names another; the icon on a dark field if there is neither.
    local art="$build/home-art" picture=/src/assets/app-icon.png art_args=()
    if [ -n "${CONKER_PS5_BACKGROUND:-}" ]; then
        picture=/background
        art_args=(--image "$picture")
    elif [ -f /src/assets/background.webp ]; then
        picture=/src/assets/background.webp
        art_args=(--image "$picture")
    fi
    local stamp
    stamp="$(cat /src/tools/ps5/make_home_art.py "$picture" | sha256sum | cut -d' ' -f1) ${art_args[*]}"
    if [ ! -f "$art/pic0.dds" ] || [ ! -f "$art/pic1.dds" ] || [ "$(cat "$art/.stamp" 2>/dev/null)" != "$stamp" ]; then
        echo "==> [package] home-screen pictures"
        python3 /src/tools/ps5/make_home_art.py "$art" "${art_args[@]}"
        echo "$stamp" > "$art/.stamp"
    fi

    local app="/src/dist/$title_id"
    rm -rf "$app"
    mkdir -p "$app/sce_sys" "$app/sce_module"
    cp "$eboot" "$app/eboot.bin"
    cp /src/tools/ps5/sce_sys/param.json /src/tools/ps5/sce_sys/icon0.png "$app/sce_sys/"
    cp "$art/pic0.dds" "$art/pic1.dds" "$app/sce_sys/"
    cp "$snd0" "$app/sce_sys/snd0.at9"
    cp "$vulkan/runtime/libc.prx" "$app/sce_module/libc.prx"
    # The console's own checks of what it reads: PS5_Vulkan's asset validation and the converter's.
    bash "$vulkan/tools/validate-assets.sh" "$app/sce_sys"
    "$native_tool" self --inspect --file "$app/eboot.bin" > /dev/null
    "$native_tool" self --inspect --file "$app/sce_module/libc.prx" > /dev/null
    chown -R "$HOST_UID:$HOST_GID" /src/dist
    echo "==> [package] dist/$title_id ($(du -sh "$app" | cut -f1))"
}

# ---------------------------------------------------------------------------------------------
# On the host
# ---------------------------------------------------------------------------------------------

# A function, so the whole of it is read before it runs: the script may change while a long step runs.
on_host() {
    here="$(cd "$(dirname "$0")/../.." && pwd)"
    step="${1:-all}"
    case "$step" in
        deps|configure|build|package|all) ;;
        *) echo "usage: tools/ps5/build.sh [deps|configure|build|package|all]" >&2; exit 2 ;;
    esac

    deps_volume="${CONKER_PS5_DEPS_VOLUME:-conker-ps5-deps}"
    build_volume="${CONKER_PS5_BUILD_VOLUME:-conker-ps5-build}"
    generated="${CONKER_GENERATED:-$here/build/us}"
    if [ ! -f "$generated/RecompiledFuncs/funcs.h" ] || [ ! -f "$generated/rsp/aspMain.cpp" ]; then
        echo "error: no recompiled code in $generated (RecompiledFuncs/funcs.h and rsp/aspMain.cpp):" \
            "run tools/regenerate.sh, or point CONKER_GENERATED at another checkout's build/us" >&2
        exit 1
    fi
    generated="$(cd "$generated" && pwd)"

    mounts=(-v "$here:/src" -v "$generated:/gen:ro" -v "$deps_volume:/deps" -v "$build_volume:/src/build/ps5")
    if [ -n "${CONKER_PS5_SND0:-}" ]; then
        [ -f "$CONKER_PS5_SND0" ] || { echo "error: no $CONKER_PS5_SND0" >&2; exit 1; }
        mounts+=(-v "$(cd "$(dirname "$CONKER_PS5_SND0")" && pwd)/$(basename "$CONKER_PS5_SND0"):/snd0.at9:ro")
    fi
    if [ -n "${CONKER_PS5_BACKGROUND:-}" ]; then
        [ -f "$CONKER_PS5_BACKGROUND" ] || { echo "error: no $CONKER_PS5_BACKGROUND" >&2; exit 1; }
        mounts+=(-v "$(cd "$(dirname "$CONKER_PS5_BACKGROUND")" && pwd)/$(basename "$CONKER_PS5_BACKGROUND"):/background:ro")
    fi

    echo "==> build image $image"
    docker build -q -t "$image" "$here/tools/ps5" > /dev/null

    # git cannot describe the source inside the container (in a worktree .git points at a host path), so
    # the version the game reports comes from here.
    version="$("$here/tools/version.sh")"

    in_container() {
        docker run --rm "${mounts[@]}" -w /src -e CONKER_BUILD_VERSION="$version" -e HOST_UID="$(id -u)" \
            -e HOST_GID="$(id -g)" -e CONKER_BUILD_ARGS="${CONKER_BUILD_ARGS:-}" -e CONKER_PS5_SND0="${CONKER_PS5_SND0:-}" \
            -e CONKER_PS5_BACKGROUND="${CONKER_PS5_BACKGROUND:-}" "$image" bash /src/tools/ps5/build.sh --in-container "$1"
    }

    if [ "$step" = deps ] || [ "$step" = all ]; then
        in_container deps
    fi
    if [ "$step" = configure ] || [ "$step" = build ] || [ "$step" = all ]; then
        # The submodules' patches (tools/patches), applied here: git in the container cannot see a
        # worktree's repository.
        "$here/tools/apply_patches.sh"
    fi
    if [ "$step" = configure ] || [ "$step" = all ]; then
        in_container configure
    fi
    if [ "$step" = build ] || [ "$step" = all ]; then
        in_container build
    fi
    if [ "$step" = package ] || [ "$step" = all ]; then
        in_container package
    fi
}

if [ "${1:-}" = "--in-container" ]; then
    export CCACHE_DIR=$deps/ccache HOME=$deps/home
    case "$2" in
        deps) in_deps ;;
        configure) in_configure ;;
        build) in_build ;;
        package) in_package ;;
    esac
else
    on_host "$@"
fi
