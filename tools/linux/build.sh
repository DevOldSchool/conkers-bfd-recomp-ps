#!/usr/bin/env bash
# Build ConkersBadFurDayRecompiled for Linux in a Docker container (tools/linux/Dockerfile) and package it as
# dist/Conkers-Bad-Fur-Day-Recompiled-<version>-linux-<arch>.tar.gz.
#
#   tools/linux/build.sh [platform]    (default: the Docker host's, linux/arm64 on Apple silicon)
#
# Needs Docker and the recompiled code from tools/regenerate.sh (build/us), which is plain C and the
# same for every platform. The build goes to build/linux-<arch>. The tarball carries its own SDL2 in
# libs/ (CONKER_BUNDLED_SDL2); GTK 3 and a Vulkan driver come with a desktop install, and the README says
# how to add them to a minimal one,
# its install.sh adds the game, with its icon, to the desktop's applications, and licenses/ holds the
# licenses and SOURCE.txt (tools/package_notices.py). Attach tools/source_archive.sh's tarball to the
# release beside it.
set -euo pipefail

here="$(cd "$(dirname "$0")/../.." && pwd)"
platform="${1:-linux/$(docker info --format '{{.Architecture}}' | sed 's/aarch64/arm64/; s/x86_64/amd64/')}"
arch="${platform#linux/}"
image="conker-recomp-linux:$arch"
build="build/linux-$arch"

"$here/tools/build_mods.sh"

echo "==> build image $image"
docker build --platform "$platform" -t "$image" "$here/tools/linux"

# The recompiled code is large: a job per 2 GB of the container's memory keeps the compiler alive.
memory_gb=$(( $(docker info --format '{{.MemTotal}}') / 1024 / 1024 / 1024 ))
jobs=$(( memory_gb / 2 > 0 ? memory_gb / 2 : 1 ))

# git cannot describe the source inside the container, so the binary's version (tools/build_version.cmake),
# which online play compares, comes from here; the tarball is named with the same text.
version="$("$here/tools/version.sh")"

echo "==> building $version in $build ($jobs jobs)"
docker run --rm --platform "$platform" -v "$here:/src" -w /src -e CONKER_BUILD_VERSION="$version" "$image" bash -c "
    set -e
    cmake -S . -B '$build' -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCONKER_RENDERER=rt64 -DCONKER_BUNDLED_SDL2=ON
    cmake --build '$build' --target ConkersBadFurDayRecompiled -j $jobs
"

# The built-in mods (tools/build_mods.sh, CONKER_EMBEDDED_MODS): a release always carries them.
if ! grep -q '^CONKER_EMBEDDED_MOD_IDS:INTERNAL=.*conker_speedrun' "$here/$build/CMakeCache.txt" 2>/dev/null; then
    echo "$build has no built-in speedrun mod: run tools/build_mods.sh, then build again" >&2
    exit 1
fi

name="Conkers-Bad-Fur-Day-Recompiled-$version-linux-$arch"
stage="$here/dist/$name"
echo "==> packaging dist/$name.tar.gz"
rm -rf "$stage" "$stage.tar.gz"
mkdir -p "$stage"
cp "$here/$build/ConkersBadFurDayRecompiled" "$stage/ConkersBadFurDayRecompiled"
# Its own SDL2 (CONKER_BUNDLED_SDL2), found through the game's $ORIGIN/libs run path.
cp -R "$here/$build/libs" "$stage/libs"
# The window and launcher icon, at 256 px (sips on macOS; the full 1024 px elsewhere).
if command -v sips > /dev/null; then
    sips -z 256 256 "$here/assets/app-icon.png" --out "$stage/conkers-bad-fur-day-recompiled.png" > /dev/null
else
    cp "$here/assets/app-icon.png" "$stage/conkers-bad-fur-day-recompiled.png"
fi
python3 "$here/tools/package_notices.py" "$here/$build" "$stage/licenses" linux
# Adding it to Steam as a non-Steam game, with its artwork (tools/steam).
cp "$here/tools/steam/add-to-steam.sh" "$stage/add-to-steam.sh"
cp -R "$here/assets/steam" "$stage/steam"
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
Conker's Bad Fur Day Recompiled for Linux.

Runs on Ubuntu 24.04 or newer, and other distributions as new, with a desktop and a Vulkan
driver, which a desktop install has; SDL2 comes in libs/. A minimal install without them needs:

    sudo apt install libgtk-3-0t64 mesa-vulkan-drivers

Add the game to your applications, with its icon:

    ./install.sh

and start it from there. No ROM is included: the first time, it asks for your own US ROM (.z64)
and keeps a copy, so it only asks once. Settings, saves, the ROM copy and last-run.log (the log
when it was started from the desktop) are kept in ~/.local/share/ConkersBadFurDayRecompiled/conker-recomp-data.
If the game crashes or will not start, open it again: it says so and shows the log to attach to a
bug report (crash-reports/crash-<date>_<time>.log there).

From a terminal, ./ConkersBadFurDayRecompiled prints its log there instead, and ./ConkersBadFurDayRecompiled /path/to/rom.z64
uses that ROM (and keeps it in place of the stored copy).

The licenses are in licenses/: the game as a whole is passed on under the GPL-3.0 (COPYING.txt),
and licenses/SOURCE.txt says where its source code is.
EOF
# macOS's tar puts each file's extended attributes and resource fork in the archive, and GNU tar on
# Linux then warns about every file it unpacks. Neither belongs in the package.
tar_flags=""
if [ "$(uname)" = Darwin ] && tar --version 2> /dev/null | grep -q bsdtar; then
    tar_flags="--no-xattrs --no-mac-metadata"
fi
# shellcheck disable=SC2086
COPYFILE_DISABLE=1 tar $tar_flags -C "$here/dist" -czf "$stage.tar.gz" "$name"
rm -rf "$stage"
echo "==> dist/$name.tar.gz ($(du -h "$stage.tar.gz" | cut -f1))"
