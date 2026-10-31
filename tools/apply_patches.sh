#!/usr/bin/env bash
# Apply this port's patches to the submodules, in order. Each submodule's patches are first applied
# to a scratch index, which checks that they all apply to the pinned commit. A submodule whose files
# already match that result is skipped, so running the script again is safe; one with other changes
# in those files stops it.
#
# Needs the submodules checked out: git submodule update --init --recursive
set -euo pipefail

here="$(cd "$(dirname "$0")/.." && pwd)"
patches="$here/tools/patches"

# submodule (relative to the repository) and its patches, in the order they apply
submodules=(
    "lib/N64Recomp"
    "lib/N64ModernRuntime"
    "lib/rt64"
    "lib/rt64/src/contrib/plume"
)
patches_for() {
    case "$1" in
        lib/N64Recomp) echo n64recomp.patch ;;
        lib/N64ModernRuntime)
            echo n64modernruntime-running-thread.patch n64modernruntime-pi-dma-message.patch \
                n64modernruntime-keep-rdram.patch n64modernruntime-count-override.patch \
                n64modernruntime-game-speed.patch n64modernruntime-timer-snapshot.patch \
                n64modernruntime-untrusted-rdram.patch n64modernruntime-rdram-guard.patch \
                n64modernruntime-pi-bounds.patch n64modernruntime-audio-buffer.patch \
                n64modernruntime-function-exit.patch n64modernruntime-isolated-pass.patch ;;
        lib/rt64)
            echo rt64-f3dex2cbfd.patch rt64-native-size.patch rt64-edge-rects.patch \
                rt64-memory-packs.patch rt64-wireframe.patch rt64-wide-draw-rect.patch \
                rt64-queue-names.patch rt64-metal-float-to-int.patch rt64-object-picking.patch \
                rt64-near-draw-rect.patch rt64-split-screen-wide.patch rt64-grey-from-green.patch \
                rt64-frame-effects.patch rt64-dl-command-limit.patch rt64-thread-exit.patch rt64-hidden-rows.patch rt64-forget-framebuffers.patch \
                rt64-vertex-slots.patch rt64-extended-gbi.patch rt64-dl-stack.patch rt64-texture-loads.patch \
                rt64-vi-copy.patch rt64-rsp-indices.patch rt64-online-writeback.patch rt64-metal-version.patch \
                rt64-cross-dxc.patch rt64-present-focus.patch ;;
        lib/rt64/src/contrib/plume) echo plume-wireframe.patch plume-gpu-errors.patch ;;
    esac
}

# Every patch in tools/patches must be listed, so a new one is not left out.
listed=" $(for sub in "${submodules[@]}"; do patches_for "$sub"; done | tr '\n' ' ') "
for file in "$patches"/*.patch; do
    name="$(basename "$file")"
    if [[ "$listed" != *" $name "* ]]; then
        echo "error: tools/patches/$name is not listed in tools/apply_patches.sh" >&2
        exit 1
    fi
done

scratch="$(mktemp -d)"
trap 'rm -rf "$scratch"' EXIT

for sub in "${submodules[@]}"; do
    dir="$here/$sub"
    read -r -a names <<< "$(patches_for "$sub")"

    # The tree the patches make from the pinned commit, built in a scratch index.
    index="$scratch/index"
    rm -f "$index"
    GIT_INDEX_FILE="$index" git -C "$dir" read-tree HEAD
    for name in "${names[@]}"; do
        if ! GIT_INDEX_FILE="$index" git -C "$dir" apply --cached "$patches/$name"; then
            echo "error: $name does not apply to $sub at $(git -C "$dir" rev-parse --short HEAD)" >&2
            exit 1
        fi
    done
    expected="$(GIT_INDEX_FILE="$index" git -C "$dir" write-tree)"

    # Compare only the files the patches touch; build output elsewhere does not matter.
    paths=()
    for name in "${names[@]}"; do
        while IFS=$'\t' read -r _ _ path; do paths+=("$path"); done \
            < <(git -C "$dir" apply --numstat "$patches/$name")
    done

    # Those files as they are now (new files a patch made are untracked), in a second scratch index.
    rm -f "$index"
    GIT_INDEX_FILE="$index" git -C "$dir" read-tree HEAD
    present=()  # git add refuses a path that is neither on disk nor in the commit
    for path in "${paths[@]}"; do
        if [ -e "$dir/$path" ] || git -C "$dir" cat-file -e "HEAD:$path" 2>/dev/null; then
            present+=("$path")
        fi
    done
    if [ ${#present[@]} -gt 0 ]; then
        GIT_INDEX_FILE="$index" git -C "$dir" add -A -f -- "${present[@]}"
    fi
    current="$(GIT_INDEX_FILE="$index" git -C "$dir" write-tree)"

    if git -C "$dir" diff --quiet "$expected" "$current" -- "${paths[@]}"; then
        echo "skipped  $sub (already patched)"
    elif git -C "$dir" diff --quiet HEAD "$current" -- "${paths[@]}"; then
        for name in "${names[@]}"; do
            git -C "$dir" apply "$patches/$name"
            echo "applied  $name"
        done
    else
        echo "error: $sub has other changes in the files its patches touch;" \
            "undo them (git -C $sub checkout -- .) and run this again" >&2
        exit 1
    fi
done
