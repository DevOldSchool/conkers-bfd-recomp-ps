#!/usr/bin/env bash
# Package a built ConkersBadFurDayRecompiled as "dist/Conker's Bad Fur Day Recompiled.app" and
# dist/Conkers-Bad-Fur-Day-Recompiled-<version>-macos.zip, to run on another Mac with no Homebrew or checkout.
#
#   tools/package_macos.sh [build dir]
#
# Without a build dir, the newer of build/rt64 and cmake-build-relwithdebinfo-rt64 is packaged.
# Build the RT64 renderer first. The app carries SDL2 (sdl2-compat) and the SDL3 it loads in
# Contents/Frameworks and is signed ad hoc, not notarised: the first time, the tester opens it with
# right click > Open, or clears the quarantine with `xattr -dr com.apple.quarantine "Conker's Bad Fur Day Recompiled.app"`.
# No ROM is packaged. Opened from Finder the app asks for the tester's own ROM once and keeps its data
# and last-run.log in ~/Library/Application Support/ConkersBadFurDayRecompiled (src/platform/launch.hpp).
# Contents/Resources/Licenses holds the licenses and SOURCE.txt (tools/package_notices.py); attach
# tools/source_archive.sh's tarball to the release beside the zip.
# dist/ConkersBadFurDayRecompiled.dSYM holds the debug information, for tools/symbolicate.sh on a tester's crash.
set -euo pipefail

here="$(cd "$(dirname "$0")/.." && pwd)"
if [ -n "${1:-}" ]; then
    build="$1"
else
    build="$here/build/rt64"
    if [ "$here/cmake-build-relwithdebinfo-rt64/ConkersBadFurDayRecompiled" -nt "$build/ConkersBadFurDayRecompiled" ]; then
        build="$here/cmake-build-relwithdebinfo-rt64"
    fi
fi
binary="$build/ConkersBadFurDayRecompiled"
dist="$here/dist"
app="$dist/Conker's Bad Fur Day Recompiled.app"

if [ ! -x "$binary" ]; then
    echo "no $binary: build the RT64 renderer first" >&2
    exit 1
fi
# A build from before the packaged-app support would quit at once when opened from Finder.
if ! grep -q "Choose your Conker" "$binary"; then
    echo "$binary predates the packaged-app support (src/platform/launch_apple.mm): rebuild it" >&2
    exit 1
fi
# The built-in mods (tools/build_mods.sh, CONKER_EMBEDDED_MODS): a release always carries them.
if ! grep -q '^CONKER_EMBEDDED_MOD_IDS:INTERNAL=.*conker_speedrun' "$build/CMakeCache.txt" 2>/dev/null; then
    echo "$build has no built-in speedrun mod: run tools/build_mods.sh, then build again" >&2
    exit 1
fi
echo "==> packaging $binary ($(stat -f '%Sm' "$binary"))"

version="$(git -C "$here" describe --always --dirty 2>/dev/null || echo unknown)"
minos="$(otool -l "$binary" | awk '/LC_BUILD_VERSION/ {found = 1} found && $1 == "minos" {print $2; exit}')"
arch="$(lipo -archs "$binary")"

# The SDL2 the binary links, wherever Homebrew put it.
sdl2_ref="$(otool -L "$binary" | awk '/libSDL2-2\.0\.0\.dylib/ {print $1; exit}')"
if [ -z "$sdl2_ref" ]; then
    echo "$binary does not link libSDL2-2.0.0.dylib" >&2
    exit 1
fi
if [[ "$sdl2_ref" == @rpath/* ]]; then
    # An SDL from a CMake package (an SDL built for an older macOS): the first rpath that has it.
    for rpath in $(otool -l "$binary" | awk '$1 == "cmd" {cmd = $2} cmd == "LC_RPATH" && $1 == "path" {print $2}'); do
        if [ -f "$rpath/${sdl2_ref#@rpath/}" ]; then
            sdl2="$(realpath "$rpath/${sdl2_ref#@rpath/}")"
            break
        fi
    done
    if [ -z "${sdl2:-}" ]; then
        echo "$binary links $sdl2_ref, found in none of its rpaths" >&2
        exit 1
    fi
else
    sdl2="$(realpath "$sdl2_ref")"
fi
# sdl2-compat loads SDL3 at run time, first from @loader_path/libSDL3.dylib: the one next to that SDL2
# (an SDL built for an older macOS, see docs/building-macos.md), else Homebrew's.
if [ -f "$(dirname "$sdl2")/libSDL3.0.dylib" ]; then
    sdl3="$(realpath "$(dirname "$sdl2")/libSDL3.0.dylib")"
else
    sdl3="$(realpath "$(brew --prefix sdl3)/lib/libSDL3.0.dylib")"
fi
# Each library must run wherever the game does: Homebrew's are built for the macOS they were built on.
minos_of() { otool -l "$1" | awk '/LC_BUILD_VERSION/ {found = 1} found && $1 == "minos" {print $2; exit}'; }
for lib in "$sdl2" "$sdl3"; do
    if [ "$(printf '%s\n%s\n' "$minos" "$(minos_of "$lib")" | sort -V | tail -1)" != "$minos" ]; then
        echo "$lib needs macOS $(minos_of "$lib"), the game $minos: build SDL for $minos (docs/building-macos.md)" >&2
        exit 1
    fi
done

echo "==> $app ($version, $arch, macOS $minos or later)"
rm -rf "$app" "$dist/ConkersBadFurDayRecompiled.dSYM" "$dist/Conker's Bad Fur Day.app" "$dist/ConkersBadFurDay.dSYM"
mkdir -p "$app/Contents/MacOS" "$app/Contents/Frameworks"
cp "$binary" "$app/Contents/MacOS/ConkersBadFurDayRecompiled"
cp "$sdl2" "$app/Contents/Frameworks/libSDL2-2.0.0.dylib"
cp "$sdl3" "$app/Contents/Frameworks/libSDL3.dylib"
chmod u+w "$app/Contents/Frameworks/"*.dylib

cat > "$app/Contents/Info.plist" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>CFBundleName</key><string>Conker's Bad Fur Day Recompiled</string>
    <key>CFBundleDisplayName</key><string>Conker's Bad Fur Day Recompiled</string>
    <key>CFBundleExecutable</key><string>ConkersBadFurDayRecompiled</string>
    <key>CFBundleIdentifier</key><string>org.conkerrecomp.ConkersBadFurDayRecompiled</string>
    <key>CFBundlePackageType</key><string>APPL</string>
    <key>CFBundleShortVersionString</key><string>0.1.0</string>
    <key>CFBundleVersion</key><string>$version</string>
    <key>LSMinimumSystemVersion</key><string>$minos</string>
    <key>LSApplicationCategoryType</key><string>public.app-category.games</string>
    <key>NSHighResolutionCapable</key><true/>
    <key>GCSupportsControllerUserInteraction</key><true/>
    <key>CFBundleIconFile</key><string>AppIcon</string>
</dict>
</plist>
EOF

# The icon: assets/app-icon.png, 1024 x 1024 with the tile in the middle 824, at every size an .icns holds.
echo "==> making the icon"
iconset="$(mktemp -d)/AppIcon.iconset"
mkdir -p "$iconset" "$app/Contents/Resources"
retina="@2x"  # kept apart from the file names, which tools/check_tree.py would read as email addresses
cp "$here/assets/app-icon.png" "$iconset/icon_512x512${retina}.png"
for size in 16 32 128 256 512; do
    sips -z "$size" "$size" "$here/assets/app-icon.png" --out "$iconset/icon_${size}x${size}.png" > /dev/null
    if [ "$size" != 512 ]; then
        double=$((size * 2))
        sips -z "$double" "$double" "$here/assets/app-icon.png" --out "$iconset/icon_${size}x${size}${retina}.png" > /dev/null
    fi
done
iconutil -c icns "$iconset" -o "$app/Contents/Resources/AppIcon.icns"
rm -rf "$(dirname "$iconset")"

# The licenses the app must carry (the GPL-3.0 among them) and where its source is.
python3 "$here/tools/package_notices.py" "$build" "$app/Contents/Resources/Licenses" macos

echo "==> pointing the binary at the bundled SDL"
exe="$app/Contents/MacOS/ConkersBadFurDayRecompiled"
install_name_tool -id @rpath/libSDL2-2.0.0.dylib "$app/Contents/Frameworks/libSDL2-2.0.0.dylib"
install_name_tool -id @rpath/libSDL3.dylib "$app/Contents/Frameworks/libSDL3.dylib"
install_name_tool -change "$sdl2_ref" @rpath/libSDL2-2.0.0.dylib "$exe"
for rpath in $(otool -l "$exe" | awk '$1 == "cmd" {cmd = $2} cmd == "LC_RPATH" && $1 == "path" {print $2}'); do
    install_name_tool -delete_rpath "$rpath" "$exe"
done
install_name_tool -add_rpath @executable_path/../Frameworks "$exe"

# Nothing may still point into Homebrew or this machine.
leaks="$(for f in "$exe" "$app/Contents/Frameworks/"*.dylib; do otool -L "$f" | tail -n +2; done \
    | awk '{print $1}' | grep -vE '^(/System/Library/|/usr/lib/|@rpath/)' || true)"
if [ -n "$leaks" ]; then
    echo "still linked outside the app:" >&2
    echo "$leaks" >&2
    exit 1
fi

echo "==> signing ad hoc"
codesign --force --sign - "$app/Contents/Frameworks/"*.dylib
codesign --force --sign - "$app"
codesign --verify --strict "$app"

dsymutil "$binary" -o "$dist/ConkersBadFurDayRecompiled.dSYM"
zip="$dist/Conkers-Bad-Fur-Day-Recompiled-$version-macos.zip"
rm -f "$zip"
ditto -c -k --keepParent "$app" "$zip"
echo "==> $zip ($(du -h "$zip" | cut -f1))"
