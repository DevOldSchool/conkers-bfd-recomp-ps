#!/usr/bin/env bash
# Adds the game to Steam as a non-Steam game, with its library artwork (macOS and Linux, the Steam Deck
# included; Windows has add-to-steam.ps1).
#
#   ./add-to-steam.sh              add it, or point Steam at it again after it moved
#   ./add-to-steam.sh --remove     take it out again
#   ./add-to-steam.sh --dry-run    say what it would do, change nothing
#   options: --game <the game, or its .app>  --user <Steam account name or number>  --steam <Steam's folder>
#
# Quit Steam first: it rewrites its list of non-Steam games as it quits, so a change made while it runs
# is lost. The next time Steam starts the game is in the library, with its grid tile, banner and icon.
#
# It changes one entry in userdata/<account>/config/shortcuts.vdf, Steam's list of non-Steam games (a copy
# of the file as it was is kept beside it, shortcuts.vdf.bak), and puts the artwork in grid/, named after
# the entry's id. Other games in the list are left as they are. The game and the artwork are found next to
# this script, as the release packages put them, or in a checkout (assets/steam, dist/ and build*/).
#
# Needs only what macOS and Linux have: bash, od, gzip (for the id's CRC-32) and printf.
set -euo pipefail

app_name="Conker's Bad Fur Day Recompiled"
binary=ConkersBadFurDayRecompiled
here="$(cd "$(dirname "$0")" && pwd)"

die() { echo "$*" >&2; exit 1; }

remove=0 dry=0 game="" user="" steam=""
while [[ $# -gt 0 ]]; do
    case "$1" in
        --remove) remove=1 ;;
        --dry-run) dry=1 ;;
        --game) game="$2"; shift ;;
        --user) user="$2"; shift ;;
        --steam) steam="$2"; shift ;;
        -h|--help) sed -n '2,19p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) die "unknown option $1 (--help lists them)" ;;
    esac
    shift
done

# --- Steam, the account ---------------------------------------------------------------------------

if [[ -z $steam ]]; then
    for folder in "$HOME/Library/Application Support/Steam" "$HOME/.steam/steam" "$HOME/.local/share/Steam" \
                  "$HOME/.var/app/com.valvesoftware.Steam/.local/share/Steam"; do
        [[ -d $folder/userdata ]] && { steam="$folder"; break; }
    done
    [[ -n $steam ]] || die "Steam was not found: start it once, or give --steam <its folder>"
fi
[[ -d $steam/userdata ]] || die "No Steam in $steam"

if [[ $dry == 0 && -z ${CONKER_STEAM_TEST:-} ]]; then
    process=steam; [[ $(uname) == Darwin ]] && process=steam_osx
    if pgrep -x "$process" > /dev/null 2>&1; then
        die "Steam is running: quit it first (Steam > Exit, or Quit on a Mac), then run this again."
    fi
fi

# The accounts: their numbers under userdata, their names and the last one signed in, from
# config/loginusers.vdf (a SteamID64 less 76561197960265728 is the number).
login_users() {  # "SteamID64 name most-recent" a line
    [[ -f $steam/config/loginusers.vdf ]] || return 0
    awk '
        /^[ \t]*"[0-9]+"[ \t]*$/ { if (id != "") print id, name, recent; gsub(/[ \t"]/, ""); id = $0; name = "-"; recent = 0 }
        /"AccountName"/ { split($0, f, "\""); name = f[4] }
        /"MostRecent"/ { split($0, f, "\""); recent = f[4] }
        END { if (id != "") print id, name, recent }' "$steam/config/loginusers.vdf"
}
users=$(login_users)
accounts=() names=() last=""
for dir in "$steam/userdata"/*/; do
    number=$(basename "$dir")
    [[ $number =~ ^[0-9]+$ && $number != 0 ]] || continue
    name=$number
    while read -r steam_id account_name most_recent; do
        [[ -n $steam_id ]] || continue
        # bash does the 64-bit subtraction (awk's numbers are doubles)
        if [[ $(( steam_id - 76561197960265728 )) == "$number" ]]; then
            name=$account_name
            [[ $most_recent == 1 ]] && last=$number
        fi
    done <<< "$users"
    accounts+=("$number") names+=("$name")
done
[[ ${#accounts[@]} -gt 0 ]] || die "No Steam account here yet: sign in to Steam once, quit it, then run this again."
chosen=""
if [[ -n $user ]]; then
    for i in "${!accounts[@]}"; do
        [[ ${accounts[$i]} == "$user" || ${names[$i]} == "$user" ]] && chosen=$i
    done
    [[ -n $chosen ]] || die "No Steam account '$user' here: ${names[*]}"
else
    chosen=0
    for i in "${!accounts[@]}"; do [[ ${accounts[$i]} == "$last" ]] && chosen=$i; done
    if [[ ${#accounts[@]} -gt 1 ]]; then
        echo "Several Steam accounts here; using ${names[$chosen]} (the last signed in). --user <name> picks another."
    fi
fi
account="${names[$chosen]}"
config="$steam/userdata/${accounts[$chosen]}/config"
vdf="$config/shortcuts.vdf"
grid="$config/grid"

# --- shortcuts.vdf, as hex ------------------------------------------------------------------------
# Steam's binary VDF: a map is a run of entries ended by 08; an entry is a type byte, a key ending in 00
# and a value: 00 a map, 01 a string ending in 00, 02 four bytes, 07 eight. The file is held as text, two
# hex digits and a space a byte, so bash can walk and cut it.

hex() { printf '%s' "$1" | od -An -v -tx1 | tr -s ' \n' '  ' | sed 's/^ //; s/ *$/ /'; }
le32() { printf '%02x %02x %02x %02x ' $(( $1 & 255 )) $(( $1 >> 8 & 255 )) $(( $1 >> 16 & 255 )) $(( $1 >> 24 & 255 )); }
str_field() { printf '01 %s00 %s00 ' "$(hex "$1")" "$(hex "$2")"; }
int_field() { printf '02 %s00 %s' "$(hex "$1")" "$(le32 "$2")"; }

if [[ -f $vdf ]]; then
    data=$(od -An -v -tx1 "$vdf" | tr -s ' \n' '  ' | sed 's/^ //; s/ *$/ /')
else
    data="00 $(hex shortcuts)00 08 08 "
fi
# Positions count characters: 3 a byte.
pos=0
byte() { echo "${data:$pos:2}"; }
skip_cstring() { while [[ ${data:$pos:2} != 00 ]]; do pos=$((pos + 3)); [[ $pos -lt ${#data} ]] || die "$vdf is damaged: not touching it"; done; pos=$((pos + 3)); }
skip_map() {
    local kind
    while true; do
        kind=$(byte); pos=$((pos + 3))
        case $kind in
            08) return ;;
            00) skip_cstring; skip_map ;;
            01) skip_cstring; skip_cstring ;;
            02) skip_cstring; pos=$((pos + 12)) ;;
            07) skip_cstring; pos=$((pos + 24)) ;;
            *) die "$vdf has an entry of a kind this script does not know (0x$kind): not touching it" ;;
        esac
        [[ $pos -lt ${#data} ]] || die "$vdf is damaged: not touching it"
    done
}

header="00 $(hex shortcuts)00 "
[[ ${data:0:${#header}} == "$header" ]] || die "$vdf does not start with a shortcuts list: not touching it"
pos=${#header}
games=()  # each game's map, its key left out: from its first field to its closing 08
while [[ $(byte) == 00 ]]; do
    pos=$((pos + 3)); skip_cstring
    start=$pos; skip_map
    games+=("${data:$start:$((pos - start))}")
done
[[ $(byte) == 08 ]] || die "$vdf is damaged: not touching it"

# Ours: named as this game is, or running its program.
is_ours() {
    local entry="$1"
    [[ $entry == *"01 $(hex AppName)00 $(hex "$app_name")00 "* || $entry == *"01 $(hex appname)00 $(hex "$app_name")00 "* ||
       $entry == *"$(hex "$binary\"")00 "* || $entry == *"$(hex "$binary.exe\"")00 "* ]]
}
# Its id (a 32-bit value after "appid"), to find its artwork.
entry_id() {
    local entry="$1" key mark rest
    for key in appid appID AppID; do
        mark="02 $(hex "$key")00 "
        if [[ $entry == *"$mark"* ]]; then
            rest="${entry#*"$mark"}"
            read -r a b c d _ <<< "$rest"
            echo $(( 0x$d << 24 | 0x$c << 16 | 0x$b << 8 | 0x$a ))
            return
        fi
    done
}

others=() previous=()
for entry in ${games[@]+"${games[@]}"}; do
    if is_ours "$entry"; then previous+=("$entry"); else others+=("$entry"); fi
done
old_ids=()
for entry in ${previous[@]+"${previous[@]}"}; do
    id=$(entry_id "$entry"); [[ -n $id ]] && old_ids+=("$id")
done

write_vdf() {  # the list: its games numbered 0, 1, 2...
    local out="$header" i=0 entry
    for entry in "$@"; do
        out+="00 $(hex "$i")00 $entry"
        i=$((i + 1))
    done
    out+="08 08 "
    mkdir -p "$config"
    [[ -f $vdf ]] && cp "$vdf" "$vdf.bak"
    # Two hex digits and a space a byte, to printf's \x escapes.
    printf "$(sed 's/\([0-9a-f][0-9a-f]\) /\\x\1/g' <<< "$out")" > "$vdf.new"
    mv "$vdf.new" "$vdf"
}
remove_art() {
    local id suffix
    for id in ${old_ids[@]+"${old_ids[@]}"}; do
        for suffix in p.jpg .jpg _hero.jpg _icon.png p.png .png _hero.png; do rm -f "$grid/$id$suffix"; done
    done
}

if [[ $remove == 1 ]]; then
    [[ ${#previous[@]} -gt 0 ]] || { echo "$app_name is not in $account's Steam library."; exit 0; }
    echo "Taking $app_name out of $account's Steam library."
    [[ $dry == 1 ]] && exit 0
    write_vdf ${others[@]+"${others[@]}"}
    remove_art
    echo "Done. Start Steam again."
    exit 0
fi

# --- the game and its artwork ----------------------------------------------------------------------

macos_app="Conker's Bad Fur Day Recompiled.app"
if [[ -n $game ]]; then
    [[ $game == *.app || $game == *.app/ ]] && game="${game%/}/Contents/MacOS/$binary"
else
    for candidate in "$here/$binary" "$here/$macos_app/Contents/MacOS/$binary" \
                     "$here/../../dist/$macos_app/Contents/MacOS/$binary" "$here"/../../build*/"$binary"; do
        [[ -f $candidate ]] && { game="$candidate"; break; }
    done
    [[ -n $game ]] || die "The game was not found next to this script: give --game <the game, or its .app>"
fi
[[ -f $game ]] || die "The game is not at $game"
game="$(cd "$(dirname "$game")" && pwd)/$(basename "$game")"
art=""
for candidate in "$here/steam" "$(dirname "$game")/../Resources/steam" "$here/../../assets/steam"; do
    [[ -f $candidate/hero.jpg ]] && { art="$(cd "$candidate" && pwd)"; break; }
done
[[ -n $art ]] || die "The Steam artwork (steam/) was not found next to this script"

# Steam's id for a non-Steam game: the CRC-32 of its quoted program and its name, top bit set (gzip's
# trailer holds that CRC-32, low byte first).
exe="\"$game\""
crc=$(printf '%s' "$exe$app_name" | gzip -c | tail -c 8 | od -An -tu4 -N4 | tr -d ' ')
id=$(( crc | 0x80000000 ))

if [[ ${#previous[@]} -gt 0 ]]; then
    echo "Updating $app_name in $account's Steam library: $game"
else
    echo "Adding $app_name to $account's Steam library: $game"
fi
if [[ $dry == 1 ]]; then
    echo "Would write $vdf and the artwork in $grid (id $id)."
    exit 0
fi
entry="$(int_field appid "$id")$(str_field AppName "$app_name")$(str_field Exe "$exe")"
entry+="$(str_field StartDir "\"$(dirname "$game")\"")$(str_field icon "$grid/${id}_icon.png")"
entry+="$(str_field ShortcutPath "")$(str_field LaunchOptions "")$(int_field IsHidden 0)$(int_field AllowDesktopConfig 1)"
entry+="$(int_field AllowOverlay 1)$(int_field OpenVR 0)$(int_field Devkit 0)$(str_field DevkitGameID "")"
entry+="$(int_field DevkitOverrideAppID 0)$(int_field LastPlayTime 0)$(str_field FlatpakAppID "")"
entry+="00 $(hex tags)00 08 08 "
write_vdf ${others[@]+"${others[@]}"} "$entry"
remove_art
mkdir -p "$grid"
cp "$art/capsule_portrait.jpg" "$grid/${id}p.jpg"
cp "$art/capsule_wide.jpg" "$grid/${id}.jpg"
cp "$art/hero.jpg" "$grid/${id}_hero.jpg"
cp "$art/icon.png" "$grid/${id}_icon.png"
echo "Done. Start Steam: $app_name is in the library with its artwork."
