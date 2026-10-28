# The Linux build environment for tools/linux/build.sh: Ubuntu 24.04, so the binary runs on Ubuntu
# 24.04 and newer (and other distributions with glibc 2.39 or later). Built for the host's
# architecture, arm64 on Apple silicon.
FROM ubuntu:24.04

RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
        clang lld cmake ninja-build git pkg-config python3 file patchelf \
        libsdl2-dev libgtk-3-dev libx11-dev libxrandr-dev libvulkan-dev zlib1g-dev \
    && rm -rf /var/lib/apt/lists/* \
    && git config --system --add safe.directory '*'

ENV CC=clang CXX=clang++
