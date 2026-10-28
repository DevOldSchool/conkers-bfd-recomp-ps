#!/usr/bin/env bash
# Build ConkersBadFurDayRecompiled for Linux in a Docker container (tools/linux/Dockerfile) and package it as
# dist/Conkers-Bad-Fur-Day-Recompiled-<version>-linux-<arch>.tar.gz.
#
#   tools/linux/build.sh [platform]    (default: the Docker host's, linux/arm64 on Apple silicon)
#
# Needs Docker and the recompiled code from tools/regenerate.sh (build/us), which is plain C and the
# same for every platform. The build goes to build/linux-<arch>. The binary needs SDL2, GTK 3 and a
# Vulkan driver on the machine it runs on; the tarball's README says how to install them on Ubuntu,
# and its install.sh adds the game, with its icon, to the desktop's applications.
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
    cmake --build '$build' --target ConkersBadFurDayRecompiled -j $jobs
"

version="$(git -C "$here" describe --always --dirty 2>/dev/null || echo unknown)"
name="Conkers-Bad-Fur-Day-Recompiled-$version-linux-$arch"
stage="$here/dist/$name"
echo "==> packaging dist/$name.tar.gz"
rm -rf "$stage" "$stage.tar.gz"
mkdir -p "$stage"
cp "$here/$build/ConkersBadFurDayRecompiled" "$stage/ConkersBadFurDayRecompiled"
# The window and launcher icon, at 256 px (sips on macOS; the full 1024 px elsewhere).
if command -v sips > /dev/null; then
    sips -z 256 256 "$here/assets/app-icon.png" --out "$stage/conkers-bad-fur-day-recompiled.png" > /dev/null
else
    cp "$here/assets/app-icon.png" "$stage/conkers-bad-fur-day-recompiled.png"
fi
cat > "$stage/install.sh" <<'EOF'
#!/bin/sh
# Adds Conker's Bad Fur Day Recompiled to the desktop's applications, with its icon, run from this folder.
# Run it again after moving the folder. Remove ~/.local/share/applications/conkers-bad-fur-day-recompiled.desktop
# to take it out.
set -e
here="$(cd "$(dirname "$0")" && pwd)"
apps="${XDG_DATA_HOME:-$HOME/.local/share}/applications"
mkdir -p "$apps"
cat > "$apps/conkers-bad-fur-day-recompiled.desktop" <<DESKTOP
[Desktop Entry]
Type=Application
Name=Conker's Bad Fur Day Recompiled
Comment=A native port of Conker's Bad Fur Day for Nintendo 64
Exec="$here/ConkersBadFurDayRecompiled"
Icon=$here/conkers-bad-fur-day-recompiled.png
Terminal=false
Categories=Game;
StartupWMClass=ConkersBadFurDayRecompiled
DESKTOP
chmod +x "$apps/conkers-bad-fur-day-recompiled.desktop"
command -v update-desktop-database > /dev/null && update-desktop-database "$apps" || true
echo "Added Conker's Bad Fur Day Recompiled to your applications."
EOF
chmod +x "$stage/install.sh"
cat > "$stage/README.txt" <<'EOF'
Conker's Bad Fur Day Recompiled, a test build for Linux.

Needs SDL2, GTK 3 and a Vulkan driver. On Ubuntu 24.04 or newer:

    sudo apt install libsdl2-2.0-0 libgtk-3-0t64 mesa-vulkan-drivers

Then add the game to your applications, with its icon:

    ./install.sh

and start it from there. No ROM is included: the first time, it asks for your own US ROM (.z64)
and keeps a copy, so it only asks once. Settings, saves, the ROM copy and last-run.log (the log
when it was started from the desktop) are kept in ~/.local/share/ConkersBadFurDayRecompiled/conker-recomp-data.

From a terminal, ./ConkersBadFurDayRecompiled prints its log there instead, and ./ConkersBadFurDayRecompiled /path/to/rom.z64
uses that ROM (and keeps it in place of the stored copy).
EOF
tar -C "$here/dist" -czf "$stage.tar.gz" "$name"
rm -rf "$stage"
echo "==> dist/$name.tar.gz ($(du -h "$stage.tar.gz" | cut -f1))"
