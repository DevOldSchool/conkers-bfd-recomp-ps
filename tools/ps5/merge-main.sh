#!/usr/bin/env bash
# Takes in the main project's changes and records which version of it this repository is level with,
# in tools/ps5/main-version. The title plays online as that version (CONKER_NET_VERSION, read by
# tools/ps5/build.sh), so it plays with that version's desktop builds.
#
#   tools/ps5/merge-main.sh v1.2.0           merge the main project's release v1.2.0 (for a PS5 release)
#   tools/ps5/merge-main.sh                  merge the main project's main (between releases)
#   tools/ps5/merge-main.sh --record [REF]   only record, after resolving a merge's conflicts and committing
#
# CONKER_UPSTREAM_REMOTE names the main project's remote (default upstream; docs/building-ps5.md).
set -euo pipefail

here="$(cd "$(dirname "$0")/../.." && pwd)"
remote="${CONKER_UPSTREAM_REMOTE:-upstream}"
file="$here/tools/ps5/main-version"

record_only=0
if [ "${1:-}" = "--record" ]; then
    record_only=1
    shift
fi
ref="${1:-main}"

git -C "$here" fetch --tags "$remote" main
# A release tag as it is; a branch as the remote has it.
if git -C "$here" rev-parse -q --verify "refs/tags/$ref^{commit}" > /dev/null; then
    target="refs/tags/$ref"
else
    target="$remote/$ref"
fi
git -C "$here" rev-parse -q --verify "$target^{commit}" > /dev/null || { echo "merge-main.sh: no $ref in $remote" >&2; exit 1; }

if [ "$record_only" = 0 ] && ! git -C "$here" merge --no-edit "$target"; then
    echo "merge-main.sh: resolve the conflicts (docs/building-ps5.md says how), commit, then run" \
        "tools/ps5/merge-main.sh --record $ref" >&2
    exit 1
fi
git -C "$here" merge-base --is-ancestor "$target" HEAD || { echo "merge-main.sh: $ref is not merged here" >&2; exit 1; }

# That commit, described as the main project describes its own builds (tools/version.sh): its release
# tag, or its commit between releases.
commit="$(git -C "$here" rev-parse "$target^{commit}")"
version="$(git -C "$here" describe --tags --always "$commit")"
printf '%s %s\n' "$version" "$commit" > "$file"
if git -C "$here" diff --quiet -- "$file" && git -C "$here" ls-files --error-unmatch "$file" > /dev/null 2>&1; then
    echo "==> level with the main project's $version (unchanged)"
    exit 0
fi
git -C "$here" add "$file"
git -C "$here" commit -q -m "Level with the main project's $version" \
    -m "tools/ps5/merge-main.sh recorded it: the title plays online as $version."
echo "==> level with the main project's $version: recorded in tools/ps5/main-version"
