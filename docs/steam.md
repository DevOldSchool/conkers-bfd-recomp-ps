# Playing through Steam

The game can sit in your Steam library as a non-Steam game, with its own artwork, which suits the
Steam Deck and Big Picture. It is not sold on Steam: this only adds a shortcut to your own library.

## With the script

Quit Steam first (it rewrites its list of non-Steam games as it quits, so a change made while it
runs is lost), then run the script that comes with the game:

* macOS: `Add to Steam.command` beside the app. The first time, macOS blocks it as it does the app:
  open it once, then choose Open Anyway in System Settings > Privacy & Security.
* Windows: `Add to Steam.bat`.
* Linux and the Steam Deck (in Desktop Mode): `./add-to-steam.sh` in the game's folder.

The next time Steam starts, the game is in the library with its grid tile, banner and icon. Run the
script again after moving the game; `--remove` (`-Remove` on Windows) takes it out, and `--dry-run`
(`-DryRun`) says what it would do without changing anything. With more than one Steam account on the
computer it uses the last one signed in; `--user <name>` (`-User`) picks another.

The script changes only its own entry in Steam's list of non-Steam games
(`userdata/<account>/config/shortcuts.vdf`), keeps a copy of the list as it was beside it
(`shortcuts.vdf.bak`), and puts the artwork in `userdata/<account>/config/grid`. From a checkout the
scripts are in `tools/steam` and the artwork in `assets/steam`.

## By hand

Steam's own **Games > Add a Non-Steam Game to My Library** works too: choose the game's app or
executable. The library entry then has no artwork unless you add it yourself (the pictures are in
the game's `steam` folder, or `Contents/Resources/steam` inside the macOS app).
