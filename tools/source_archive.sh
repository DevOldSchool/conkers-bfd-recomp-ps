#!/usr/bin/env bash
# Writes dist/Conkers-Bad-Fur-Day-Recompiled-<version>-source.tar.gz: the committed source at HEAD with
# every submodule at its committed revision, unpatched (tools/apply_patches.sh patches them, as in any
# checkout). Attach it to each release beside the builds: with it, the release carries the complete
# source the GPL-3.0 asks for, whatever happens to the submodules' own repositories.
#
#   tools/source_archive.sh
#
# Only committed files go in; the ROM, the recompiled game code (build/us) and other untracked files
# never do.
set -euo pipefail

here="$(cd "$(dirname "$0")/.." && pwd)"
version="$(git -C "$here" describe --tags --always 2>/dev/null)"
name="Conkers-Bad-Fur-Day-Recompiled-$version-source"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

git -C "$here" archive --prefix="$name/" HEAD | tar -x -C "$work"
# Each submodule at the revision HEAD records, recursively, from its own git data.
git -C "$here" submodule foreach --quiet --recursive 'echo "$displaypath $sha1"' | while read -r path sha; do
    git -C "$here/$path" archive --prefix="$name/$path/" "$sha" | tar -x -C "$work"
done

mkdir -p "$here/dist"
# macOS's tar puts each file's extended attributes and resource fork in the archive, and GNU tar on
# Linux then warns about every file it unpacks. Neither belongs in the package.
tar_flags=""
if [ "$(uname)" = Darwin ] && tar --version 2> /dev/null | grep -q bsdtar; then
    tar_flags="--no-xattrs --no-mac-metadata"
fi
# shellcheck disable=SC2086
COPYFILE_DISABLE=1 tar $tar_flags -C "$work" -czf "$here/dist/$name.tar.gz" "$name"
echo "==> dist/$name.tar.gz ($(du -h "$here/dist/$name.tar.gz" | cut -f1))"
