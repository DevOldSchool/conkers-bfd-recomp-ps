#!/usr/bin/env bash
# Copy the PS5 title (dist/PPSA21064, tools/ps5/build.sh) to a console over FTP: etaHEN's FTP server.
#
#   PS5_HOST=<ip>[:port] tools/ps5/upload.sh [path inside the title folder ...]
#
# With no paths, the whole title folder; with paths (eboot.bin, sce_sys/param.json...), only those.
#   PS5_HOST  the console, port 2121 when none is given
#   PS5_DIR   the folder the title folder goes in (default /data/homebrew)
#   PS5_ROM   also copy this ROM into the title folder, its extension lower-cased (the game looks for
#             .z64, .n64 or .v64 there)
#
# The server can refuse to overwrite a file, so each goes up under another name, the old one is deleted
# and the new one renamed. The home screen's own copies of the icon, pictures and music,
# /user/appmeta/PPSA21064 and the console's install copy /user/app/PPSA21064/sce_sys (where it reads the
# background), are updated with them; the console shows the new ones after a restart.
set -euo pipefail

title_id=PPSA21064
here="$(cd "$(dirname "$0")/../.." && pwd)"
app="$here/dist/$title_id"

[ -n "${PS5_HOST:-}" ] || { echo "usage: PS5_HOST=<ip>[:port] tools/ps5/upload.sh [path ...]" >&2; exit 2; }
host="$PS5_HOST"
[[ "$host" == *:* ]] || host="$host:2121"
remote_dir="${PS5_DIR:-/data/homebrew}/$title_id"
appmeta="/user/appmeta/$title_id"
installed="/user/app/$title_id"
[ -d "$app" ] || { echo "no $app: run tools/ps5/build.sh" >&2; exit 1; }

# put <local file> <remote path>: up as <remote path>.uploading, then into place.
put() {
    local file=$1 remote=$2
    echo "==> $remote"
    curl -sS --fail --ftp-create-dirs -T "$file" "ftp://$host$remote.uploading"
    # A listing of / carries the commands: delete the old file (it may not be there, hence the *),
    # then rename the new one.
    curl -sS --fail -o /dev/null "ftp://$host/" \
        -Q "*DELE $remote" -Q "RNFR $remote.uploading" -Q "RNTO $remote"
}

# same <local file> <remote path>: whether the console already has this file there. The home screen
# holds on to its copies, and one replaced, even by the same bytes, can go silent until the console restarts.
same() {
    local copy
    copy="$(mktemp)"
    if curl -sS --fail -o "$copy" "ftp://$host$2" 2>/dev/null && cmp -s "$1" "$copy"; then
        rm -f "$copy"
        return 0
    fi
    rm -f "$copy"
    return 1
}

files=()
if [ $# -gt 0 ]; then
    for path in "$@"; do
        [ -f "$app/$path" ] || { echo "no $path in dist/$title_id" >&2; exit 1; }
        files+=("$path")
    done
else
    while IFS= read -r path; do
        files+=("${path#"$app"/}")
    done < <(find "$app" -type f | sort)
fi

for path in "${files[@]}"; do
    put "$app/$path" "$remote_dir/$path"
    case "$path" in
        sce_sys/icon0.png|sce_sys/pic0.dds|sce_sys/pic1.dds|sce_sys/snd0.at9)
            for copy in "$appmeta/${path#sce_sys/}" "$installed/$path"; do
                if same "$app/$path" "$copy"; then
                    echo "==> $copy is unchanged"
                else
                    put "$app/$path" "$copy"
                fi
            done ;;
    esac
done

if [ -n "${PS5_ROM:-}" ]; then
    [ -f "$PS5_ROM" ] || { echo "no $PS5_ROM" >&2; exit 1; }
    name="$(basename "$PS5_ROM")"
    stem="${name%.*}"
    extension="$(printf '%s' "${name##*.}" | tr '[:upper:]' '[:lower:]')"
    put "$PS5_ROM" "$remote_dir/$stem.$extension"
fi
echo "==> $title_id is on $host in $remote_dir"
