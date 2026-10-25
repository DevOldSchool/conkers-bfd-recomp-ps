#!/usr/bin/env bash
# Build ConkerRecomp for Linux in a Docker container (tools/linux/Dockerfile) and package it as
# dist/Conkers-Bad-Fur-Day-<version>-linux-<arch>.tar.gz.
#
#   tools/linux/build.sh [platform]    (default: the Docker host's, linux/arm64 on Apple silicon)
#
# Needs Docker and the recompiled code from tools/regenerate.sh (build/us), which is plain C and the
# same for every platform. The build goes to build/linux-<arch>. The binary needs SDL2, GTK 3 and a
# Vulkan driver on the machine it runs on; the tarball's README says how to install them on Ubuntu.
set -euo pipefail

here="$(cd "$(dirname "$0")/../.." && pwd)"
platform="${1:-linux/$(docker info --format '{{.Architecture}}' | sed 's/aarch64/arm64/; s/x86_64/amd64/')}"
arch="${platform#linux/}"
image="conker-recomp-linux:$arch"
build="build/linux-$arch"

echo "==> build image $image"
docker build --platform "$platform" -t "$image" "$here/tools/linux"

# The recompiled code is large: a job per 2 GB of the container's memory keeps the compiler alive.
memory_gb=$(( $(docker info --format '{{.MemTotal}}') / 1024 / 1024 / 1024 ))
jobs=$(( memory_gb / 2 > 0 ? memory_gb / 2 : 1 ))

echo "==> building in $build ($jobs jobs)"
docker run --rm --platform "$platform" -v "$here:/src" -w /src "$image" bash -c "
    set -e
    cmake -S . -B '$build' -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCONKER_RENDERER=rt64
    cmake --build '$build' --target ConkerRecomp -j $jobs
"

version="$(git -C "$here" describe --always --dirty 2>/dev/null || echo unknown)"
name="Conkers-Bad-Fur-Day-$version-linux-$arch"
stage="$here/dist/$name"
echo "==> packaging dist/$name.tar.gz"
rm -rf "$stage" "$stage.tar.gz"
mkdir -p "$stage"
cp "$here/$build/ConkerRecomp" "$stage/ConkerRecomp"
cp "$here/assets/app-icon.png" "$stage/conker-recomp.png"
cat > "$stage/README.txt" <<'EOF'
Conker's Bad Fur Day: Recompiled, a test build for Linux.

Needs SDL2, GTK 3 and a Vulkan driver. On Ubuntu 24.04 or newer:

    sudo apt install libsdl2-2.0-0 libgtk-3-0t64 mesa-vulkan-drivers

No ROM is included. Run it from this folder with your own US ROM the first time:

    ./ConkerRecomp /path/to/conker-us.z64

A copy of the ROM is kept in conker-recomp-data in the folder you run it from, with settings and
saves, so later runs need no argument: ./ConkerRecomp
EOF
tar -C "$here/dist" -czf "$stage.tar.gz" "$name"
rm -rf "$stage"
echo "==> dist/$name.tar.gz ($(du -h "$stage.tar.gz" | cut -f1))"
