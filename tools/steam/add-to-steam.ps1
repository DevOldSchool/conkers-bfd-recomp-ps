# Adds the game to Steam as a non-Steam game, with its library artwork (Windows; macOS and Linux have
# add-to-steam.sh). Double-click "Add to Steam.bat" next to it, or from PowerShell:
#
#   .\add-to-steam.ps1              add it, or point Steam at it again after it moved
#   .\add-to-steam.ps1 -Remove      take it out again
#   .\add-to-steam.ps1 -DryRun      say what it would do, change nothing
#   options: -Game <ConkersBadFurDayRecompiled.exe>  -User <Steam account name or number>  -Steam <Steam's folder>
#
# Quit Steam first: it rewrites its list of non-Steam games as it quits, so a change made while it runs
# is lost. The next time Steam starts the game is in the library, with its grid tile, banner and icon.
#
# It changes one entry in userdata\<account>\config\shortcuts.vdf, Steam's list of non-Steam games (a copy
# of the file as it was is kept beside it, shortcuts.vdf.bak), and puts the artwork in grid\, named after
# the entry's id. Other games in the list are left as they are. The game and the artwork are found next to
# this script, as the release package puts them, or in a checkout (assets\steam and build*\).
#
# Needs only Windows PowerShell 5.1, which every Windows 10 and 11 has.
param(
    [switch]$Remove,
    [switch]$DryRun,
    [string]$Game = "",
    [string]$User = "",
    [string]$Steam = ""
)
$ErrorActionPreference = "Stop"
$AppName = "Conker's Bad Fur Day Recompiled"
$Binary = "ConkersBadFurDayRecompiled.exe"
$Here = $PSScriptRoot

function Fail([string]$message) { Write-Host $message -ForegroundColor Red; exit 1 }

# --- Steam, the account ---------------------------------------------------------------------------

if (-not $Steam) {
    $candidates = @()
    $key = Get-ItemProperty -Path "HKCU:\Software\Valve\Steam" -Name SteamPath -ErrorAction SilentlyContinue
    if ($key) { $candidates += $key.SteamPath }
    $candidates += Join-Path ${env:ProgramFiles(x86)} "Steam"
    foreach ($folder in $candidates) {
        if (Test-Path (Join-Path $folder "userdata")) { $Steam = $folder; break }
    }
    if (-not $Steam) { Fail "Steam was not found: start it once, or give -Steam <its folder>" }
}
if (-not (Test-Path (Join-Path $Steam "userdata"))) { Fail "No Steam in $Steam" }
$Steam = (Resolve-Path $Steam).Path

if (-not $DryRun -and -not $env:CONKER_STEAM_TEST -and (Get-Process -Name steam -ErrorAction SilentlyContinue)) {
    Fail "Steam is running: quit it first (Steam > Exit), then run this again."
}

# The accounts: their numbers under userdata, their names and the last one signed in, from
# config\loginusers.vdf (a SteamID64 less 76561197960265728 is the number).
$names = @{}
$last = ""
$users = Join-Path $Steam "config\loginusers.vdf"
if (Test-Path $users) {
    $text = [IO.File]::ReadAllText($users)
    foreach ($m in [regex]::Matches($text, '"(\d{17})"\s*\{(.*?)\}', "Singleline")) {
        $number = ([UInt64]$m.Groups[1].Value - 76561197960265728).ToString()
        $body = $m.Groups[2].Value
        $account = [regex]::Match($body, '"AccountName"\s*"([^"]*)"').Groups[1].Value
        $names[$number] = if ($account) { $account } else { $number }
        if ([regex]::Match($body, '"MostRecent"\s*"1"').Success) { $last = $number }
    }
}
$accounts = @(Get-ChildItem (Join-Path $Steam "userdata") -Directory | Where-Object { $_.Name -match '^\d+$' -and $_.Name -ne "0" } |
    ForEach-Object { $_.Name } | Sort-Object)
if ($accounts.Count -eq 0) { Fail "No Steam account here yet: sign in to Steam once, quit it, then run this again." }
function NameOf([string]$number) { if ($names.ContainsKey($number)) { $names[$number] } else { $number } }
$chosen = $null
if ($User) {
    foreach ($number in $accounts) { if ($number -eq $User -or (NameOf $number) -eq $User) { $chosen = $number } }
    if (-not $chosen) { Fail "No Steam account '$User' here: $(($accounts | ForEach-Object { NameOf $_ }) -join ', ')" }
} else {
    $chosen = if ($accounts -contains $last) { $last } else { $accounts[0] }
    if ($accounts.Count -gt 1) {
        Write-Host "Several Steam accounts here; using $(NameOf $chosen) (the last signed in). -User <name> picks another."
    }
}
$AccountName = NameOf $chosen
$Config = Join-Path $Steam "userdata\$chosen\config"
$Vdf = Join-Path $Config "shortcuts.vdf"
$Grid = Join-Path $Config "grid"

# --- shortcuts.vdf ---------------------------------------------------------------------------------
# Steam's binary VDF: a map is a run of entries ended by 0x08; an entry is a type byte, a key ending in 0
# and a value: 0x00 a map, 0x01 a string ending in 0, 0x02 four bytes, 0x07 eight. Each game in the list
# is kept as its bytes (its key left out), so the others are written back exactly as they were.

$utf8 = New-Object Text.UTF8Encoding($false)
# The leading comma keeps a function's array whole (PowerShell would otherwise unroll it, and an empty
# one would come back as nothing at all).
function Bytes([string]$text) { ,$utf8.GetBytes($text) }
function Concat($parts) {
    $out = New-Object Collections.Generic.List[byte]
    foreach ($p in $parts) { if ($null -ne $p) { $out.AddRange([byte[]]$p) } }
    ,$out.ToArray()
}
function StrField([string]$key, [string]$value) { Concat @(@([byte]1), (Bytes $key), @([byte]0), (Bytes $value), @([byte]0)) }
function IntField([string]$key, [UInt32]$value) { Concat @(@([byte]2), (Bytes $key), @([byte]0), [BitConverter]::GetBytes($value)) }
$Header = Concat @(@([byte]0), (Bytes "shortcuts"), @([byte]0))

$data = if (Test-Path $Vdf) { [IO.File]::ReadAllBytes($Vdf) } else { Concat @($Header, @([byte]8, [byte]8)) }
$script:pos = 0
function Damaged { Fail "$Vdf is damaged, or holds something this script does not know: not touching it" }
function SkipCString { $i = [Array]::IndexOf($data, [byte]0, $script:pos); if ($i -lt 0) { Damaged }; $script:pos = $i + 1 }
function SkipMap {
    while ($true) {
        if ($script:pos -ge $data.Length) { Damaged }
        $kind = $data[$script:pos]; $script:pos++
        switch ($kind) {
            8 { return }
            0 { SkipCString; SkipMap }
            1 { SkipCString; SkipCString }
            2 { SkipCString; $script:pos += 4 }
            7 { SkipCString; $script:pos += 8 }
            default { Damaged }
        }
    }
}
for ($i = 0; $i -lt $Header.Length; $i++) { if ($data.Length -le $i -or $data[$i] -ne $Header[$i]) { Damaged } }
$script:pos = $Header.Length
$games = New-Object Collections.ArrayList
while ($script:pos -lt $data.Length -and $data[$script:pos] -eq 0) {
    $script:pos++; SkipCString
    $start = $script:pos; SkipMap
    [void]$games.Add([byte[]]$data[$start..($script:pos - 1)])
}
if ($script:pos -ge $data.Length -or $data[$script:pos] -ne 8) { Damaged }

# Searching a game's bytes as Latin-1 text keeps one character a byte.
$latin1 = [Text.Encoding]::GetEncoding(28591)
function AsText([byte[]]$b) { $latin1.GetString($b) }
function TextOf([byte[]]$b) { $latin1.GetString($b) }
$marks = @(
    (TextOf (StrField "AppName" $AppName)), (TextOf (StrField "appname" $AppName)),
    (TextOf (Concat @((Bytes "$Binary`""), @([byte]0)))), (TextOf (Concat @((Bytes "ConkersBadFurDayRecompiled`""), @([byte]0))))
)
function IsOurs([byte[]]$entry) { $t = AsText $entry; foreach ($m in $marks) { if ($t.Contains($m)) { return $true } }; $false }
function EntryId([byte[]]$entry) {
    $t = AsText $entry
    foreach ($key in "appid", "appID", "AppID") {
        $mark = TextOf (Concat @(@([byte]2), (Bytes $key), @([byte]0)))
        $i = $t.IndexOf($mark, [StringComparison]::Ordinal)
        if ($i -ge 0) { return [BitConverter]::ToUInt32($entry, $i + $mark.Length) }
    }
    $null
}

$others = New-Object Collections.ArrayList
$previous = New-Object Collections.ArrayList
foreach ($entry in $games) { if (IsOurs $entry) { [void]$previous.Add($entry) } else { [void]$others.Add($entry) } }
$oldIds = @($previous | ForEach-Object { EntryId $_ } | Where-Object { $_ -ne $null })

function WriteVdf($list) {
    $parts = New-Object Collections.ArrayList
    [void]$parts.Add($Header)
    $i = 0
    foreach ($entry in $list) { [void]$parts.Add((Concat @(@([byte]0), (Bytes "$i"), @([byte]0), $entry))); $i++ }
    [void]$parts.Add(@([byte]8, [byte]8))
    $bytes = Concat $parts
    New-Item -ItemType Directory -Force -Path $Config | Out-Null
    if (Test-Path $Vdf) { Copy-Item $Vdf "$Vdf.bak" -Force }
    [IO.File]::WriteAllBytes("$Vdf.new", $bytes)
    Move-Item "$Vdf.new" $Vdf -Force
}
function RemoveArt {
    foreach ($id in $oldIds) {
        foreach ($suffix in "p.jpg", ".jpg", "_hero.jpg", "_icon.png", "p.png", ".png", "_hero.png") {
            Remove-Item (Join-Path $Grid "$id$suffix") -ErrorAction SilentlyContinue
        }
    }
}

if ($Remove) {
    if ($previous.Count -eq 0) { Write-Host "$AppName is not in $AccountName's Steam library."; exit 0 }
    Write-Host "Taking $AppName out of $AccountName's Steam library."
    if ($DryRun) { exit 0 }
    WriteVdf $others
    RemoveArt
    Write-Host "Done. Start Steam again."
    exit 0
}

# --- the game and its artwork ----------------------------------------------------------------------

if ($Game) {
    if (-not (Test-Path $Game -PathType Leaf)) { Fail "The game is not at $Game" }
} else {
    # Next to this script (a release), else a checkout's build folders (build\, build\<name>\).
    $builds = @(Get-ChildItem (Join-Path $Here "..\..") -Directory -Filter "build*" -ErrorAction SilentlyContinue)
    $candidates = @((Join-Path $Here $Binary)) + @($builds | ForEach-Object { Join-Path $_.FullName $Binary }) +
        @($builds | ForEach-Object { Get-ChildItem $_.FullName -Directory -ErrorAction SilentlyContinue } | ForEach-Object { Join-Path $_.FullName $Binary })
    foreach ($candidate in $candidates) { if (Test-Path $candidate -PathType Leaf) { $Game = $candidate; break } }
    if (-not $Game) { Fail "The game was not found next to this script: give -Game <ConkersBadFurDayRecompiled.exe>" }
}
$Game = (Resolve-Path $Game).Path
$Art = $null
foreach ($candidate in (Join-Path $Here "steam"), (Join-Path $Here "..\..\assets\steam")) {
    if (Test-Path (Join-Path $candidate "hero.jpg")) { $Art = (Resolve-Path $candidate).Path; break }
}
if (-not $Art) { Fail "The Steam artwork (steam\) was not found next to this script" }

# Steam's id for a non-Steam game: the CRC-32 of its quoted program and its name, top bit set.
function Crc32([byte[]]$bytes) {
    $crc = [UInt32]::MaxValue
    foreach ($b in $bytes) {
        $crc = $crc -bxor $b
        for ($k = 0; $k -lt 8; $k++) {
            if ($crc -band 1) { $crc = ($crc -shr 1) -bxor [UInt32]3988292384 } else { $crc = $crc -shr 1 }
        }
    }
    [UInt32]($crc -bxor [UInt32]::MaxValue)
}
$Exe = "`"$Game`""
$Id = [UInt32]((Crc32 (Bytes "$Exe$AppName")) -bor [UInt32]2147483648)

if ($previous.Count -gt 0) { Write-Host "Updating $AppName in $AccountName's Steam library: $Game" }
else { Write-Host "Adding $AppName to $AccountName's Steam library: $Game" }
if ($DryRun) { Write-Host "Would write $Vdf and the artwork in $Grid (id $Id)."; exit 0 }

$entry = Concat @(
    (IntField "appid" $Id), (StrField "AppName" $AppName), (StrField "Exe" $Exe),
    (StrField "StartDir" "`"$(Split-Path $Game)`""), (StrField "icon" (Join-Path $Grid "$($Id)_icon.png")),
    (StrField "ShortcutPath" ""), (StrField "LaunchOptions" ""), (IntField "IsHidden" 0), (IntField "AllowDesktopConfig" 1),
    (IntField "AllowOverlay" 1), (IntField "OpenVR" 0), (IntField "Devkit" 0), (StrField "DevkitGameID" ""),
    (IntField "DevkitOverrideAppID" 0), (IntField "LastPlayTime" 0), (StrField "FlatpakAppID" ""),
    @([byte]0), (Bytes "tags"), @([byte]0, [byte]8, [byte]8)
)
$list = New-Object Collections.ArrayList
foreach ($e in $others) { [void]$list.Add($e) }
[void]$list.Add($entry)
WriteVdf $list
RemoveArt
New-Item -ItemType Directory -Force -Path $Grid | Out-Null
Copy-Item (Join-Path $Art "capsule_portrait.jpg") (Join-Path $Grid "$($Id)p.jpg") -Force
Copy-Item (Join-Path $Art "capsule_wide.jpg") (Join-Path $Grid "$($Id).jpg") -Force
Copy-Item (Join-Path $Art "hero.jpg") (Join-Path $Grid "$($Id)_hero.jpg") -Force
Copy-Item (Join-Path $Art "icon.png") (Join-Path $Grid "$($Id)_icon.png") -Force
Write-Host "Done. Start Steam: $AppName is in the library with its artwork."
