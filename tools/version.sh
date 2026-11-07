#!/usr/bin/env bash
# Prints the build's version: CONKER_BUILD_VERSION when set, else `git describe --always`, with
# "-dirty" when this repository's tracked files have changed. The submodules' own changes (the
# patches tools/apply_patches.sh applies in every build) do not count, though `git describe --dirty`
# counts them. tools/build_version.cmake and tools/package_notices.py do the same.
set -euo pipefail
if [ -n "${CONKER_BUILD_VERSION:-}" ]; then
    echo "$CONKER_BUILD_VERSION"
    exit 0
fi
here="$(cd "$(dirname "$0")/.." && pwd)"
version="$(git -C "$here" describe --always 2>/dev/null || echo unknown)"
if [ "$version" != unknown ] && [ -n "$(git -C "$here" status --porcelain --untracked-files=no --ignore-submodules=dirty)" ]; then
    version="$version-dirty"
fi
echo "$version"
