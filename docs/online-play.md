# Playing online

Work in progress: online play is being built (docs/online-multiplayer-plan.md) and has only been
tried between Macs on one home network so far.

**Internet play is off for now: LAN only.** Until the online code's security is trusted, the game hosts and joins only
on the local network: loopback, private addresses (`10.x`, `172.16.x` to `172.31.x`, `192.168.x`),
link-local addresses and IPv6 unique-local (`fc00::/7`) and link-local ones. Every other sender's
packets are dropped unanswered, JOIN refuses an address or code outside the LAN, nothing is opened on
the router, and the lobby shows no Internet or IPv6 code. Overlay networks such as Tailscale use
carrier-grade NAT addresses (`100.64.x` to `100.127.x`), which count as the internet here: playing
over one needs the switch too.

To play over the internet anyway, start the game on **every** machine with
`--net-unsafe-internet`. There is no menu toggle. The lobby then says **"UNSAFE: internet play on"**
and the log says so at startup. Play only with people you know: a stranger who reaches your port can
still cause trouble that the reviews have not ruled out. The rest of this page describes internet
play as it works with the switch on.

**Test builds start with internet play on.** A build configured with
`-DCONKER_NET_INTERNET_DEFAULT=ON` (off in the source; the packaged tester builds have it on for
now) behaves as if `--net-unsafe-internet` were given: the router mapping (UPnP/NAT-PMP), the
Internet join code and joins from outside the LAN all work. The lobby still shows the red UNSAFE
line. `--net-lan-only` turns it off for that run. A session the lobby starts keeps the lobby's choice
either way.

## Security

**Join only hosts you trust.** The host's game is the match: when you join a session under way, or
come back to one, the host sends its whole game to your machine, and your game then plays on from
that. The game checks what it gets so that it cannot reach outside the game's own memory, but a host
that wants to can make anything happen inside your match. The lobby says so under JOIN.

Everything the lobby and the session send is encrypted and signed (X25519 and XChaCha20-Poly1305,
from Monocypher), except the LAN list's two small packets, which carry the host's name, build, game
port and player count. A machine that only sees the traffic, on public Wi-Fi or a shared network,
learns no player's secret or session token from it, and cannot take a player's seat, send a player
out or pose as a player. Each machine makes a host key the first time it hosts and keeps it
(`conker-recomp-data/net/host_key`: keep it private, whoever has it can pose as your lobby), and each
guest makes a new key for every join.

Three things stop another machine answering in the host's place:

* A **join code** holds 40 bits of the host's key: a guest that joins by code refuses another key
  ("the machine that answered is not the one the join code names").
* The **KEY CHECK**: the host's lobby shows its own (`7QX4-M2KD`), and a guest's lobby shows the
  key check of the host it reached, under the players. They should be the same: compare them over
  voice or chat, above all the first time you join someone by address or from the LAN list.
* A guest remembers every host's key by address (`conker-recomp-data/net/known_hosts`). If a host's
  key has changed since you last joined it, JOIN stops and says so, with both key checks; press JOIN
  again to take the new key once the host's screen shows it. A new key is normal after the host
  reinstalls or deletes its `host_key`, or when another machine has that address now.

The session the lobby starts takes only the lobby's host key.

Online, the camera does not pull in for scenery close in front of it as it does offline: the game
read that from the graphics card's depth, which differs between machines and put them out of sync.

MULTI can be played by up to four players on different machines. One player hosts and the others
join. Every machine runs the whole game and only the controllers' input travels, so everyone needs:

* the same build of the game (the lobby says when one differs);
* their own copy of the same ROM, Conker's Bad Fur Day (US);
* the same mods on, or none.

Macs, Linux and Windows PCs play together: the game gives the same results on all of them, Apple
silicon and Intel or AMD alike.

The host's settings (widescreen, draw distance, full detail, 60 fps and the like) are used by
everyone for the session, since they change how the game plays; the lobby shows them and which differ
from yours. Each player keeps their own controls (CONSISTENT CONTROLS, the camera swap, INVERT AIM), MY VIEW,
SHARP TEXT, PAD BUTTONS, HAPTICS and ADAPTIVE TRIGGERS, and of course their own window, volume and
key mapping: every machine applies each player's controls to that player alike, and the rest only
change your own screen and pad. WIDESCREEN, WIDESCREEN CULLING, DRAW DISTANCE, FULL DETAIL, 60 FPS,
MP CHARACTERS, the cutscene and intro skips and SURROUND DEFAULT stay the host's: each changes what
the game itself works out, so every machine must have the same. The session plays on a copy of the host's save, so nobody's own save
changes.

Nothing starts again. When the host presses START, every game goes on from the host's: the host
plays on where it is, and each guest's game becomes a copy of the host's within a second or two
(its player can move a few seconds after that). When you leave the session, your own game comes
back exactly as it was when the session started, with your own save and settings.

In a match each machine shows only its own player's view over the whole window, with that player's
HUD, as a match against bots with one pad looks, and plays the sounds as that player hears them:
sounds near the others are quiet or silent unless you are near them too, and the other players' own
sounds (footsteps, shots, voices), which the split screen plays to everyone at full volume, come
from where their characters are. The pause menu and the screens between matches show the split
screen as before. This is your own choice, not the host's: `CONKER_MY_VIEW=0`, or MY VIEW on the
debug menu's ENHANCEMENTS page, keeps the shared split screen and sounds. Each machine draws its
player's view a second time each frame, after the split screen every machine draws, so it costs some
speed.

[Mods](modding.md) change how the game plays, so a mod on one machine and not the others would put
them out of sync: everyone needs the same mods on (the same versions), or none. The lobby says
"different mods" otherwise; switch them on the debug menu's MODS page, then start the game again.

### Leaving a session

Press the debug menu's key (Back on a pad, `` ` `` on the keyboard) during a session: a panel
shows the session and **LEAVE THE SESSION** (or **BACK TO THE GAME**). Leaving brings your own
game back as it was before the session. When the host leaves, or a player cannot carry on (dropped
too often, or the host's game never reached them), their game shows why for a few seconds and then
goes back to their own game the same way. Quitting the game works too.

A session started from the command line (`--net-host`, `--net-join`) starts the game into it and has
no game of yours to go back to: quit to leave it.

## The lobby

On the title, go through the **MULTI** door to the barrels and, on any of them, press Start (Options
on a PlayStation pad, Menu on an Xbox pad, Enter on the keyboard), as the line under the barrel says,
or start the game with `--net-lobby`. The barrel turns its face up and the lobby is on it, in the
game's own lettering, as the MULTI modes are. Controller 1 steers it (the d-pad or stick moves, A
presses, B goes back), and so does the keyboard (the arrows, Enter, Esc). **CLOSE** turns the barrel
back.

Set your **NAME** first: it is what the others see. Type it on the keyboard (Cmd+V or Ctrl+V pastes),
or pick the letters from the keyboard on the barrel with the pad: A adds one, Z (R2 or RT) deletes one,
Start is done and B goes back without keeping what was typed. If you leave it, you are "Player"; the game never
takes a name from the computer.

Each player's pad shows as its make and model only ("DualSense", "Xbox", "Switch Pro", "Controller",
or "Keyboard"). Your own pad's full name shows on your screen alone: macOS names a Bluetooth pad
after its owner, so the name never goes over the network. Logs and debug reports also give the model
only.

### Hosting

Press **HOST A GAME**. The game listens on UDP port 37800 and, with `--net-unsafe-internet`, asks
your router to open that port (UPnP or NAT-PMP). Without the switch it says "Nothing is opened on the
router: internet play is off". With it, the lobby then says one of:

* **"UDP port 37800 is open on the router"**: players on other networks can join with your
  Internet join code.
* **"Could not open UDP port 37800 on the router"**: players on your own network can still join;
  for anyone else, see [When the port cannot be opened](#when-the-port-cannot-be-opened).
* **"...outside address ... is a private one"** or **"...shares between customers (carrier-grade
  NAT)"**: the router did open the port but sits behind another router or your provider's own NAT,
  so the port opening does not reach the internet. See below.

The lobby lists **join codes** to hand out (**COPY JOIN CODE** puts the first on the clipboard):

* **Internet**: your router's outside address, once the port is open.
* **IPv6**: this machine's own IPv6 address, if it has one. IPv6 needs no port forwarding, but many
  routers' firewalls refuse incoming IPv6 traffic unless it is allowed.
* **This network**: your address on the local network, for players at home (or on an overlay
  network, below).

A join code is your address and port and part of this lobby's key, written short (24 characters
with the dashes); it is not a secret. Whoever joins learns your address, as you learn theirs, and a
guest that joins by code refuses a machine posing as yours (see *Security* above). Your key stays the
same from one HOST to the next, so your codes do too while your address stays the same. When everyone is in, press
**START**: the session starts in a moment, without starting any game again, and you, as player 1,
choose MULTI and the game as on a console. The others press Start on the join screen.

Leaving the lobby or quitting the game closes the router's port again. A port left open by a
crash closes itself within ten minutes (the game asks the router for a ten-minute lease and renews
it while it runs; a router that only takes permanent mappings is not given one).

Anyone with your address or code can join your lobby. **REMOVE P2** (and so on) sends them out of
the lobby, and their machine cannot join it again until you host anew. Only the players in the lobby
when you press START can join the session it starts.

### Joining

Games on your network show up by themselves as **JOIN** rows with the host's name: press one.
Otherwise choose the **JOIN** row and type the host's join code, or its address (`203.0.113.7`, `203.0.113.7:37800`,
`[2001:db8::7]:37800`, or a host name), then Enter (or DONE). In the host's lobby, wait for the host to press **START**.

A game shown as **PLAYING** has already started. A match started from a lobby takes only that
lobby's players, and one started by hand with `--net-host` only guests with its token (below), so
only a host started with `--net-open` takes new players this way. Joining it starts your game straight into the
match (from a copy of the host's game, so it takes a few seconds, not a replay of everything so
far). A player who dropped out gets their port back and plays on at once, from the same machine
(it remembers a secret for that host; another machine cannot take a dropped player's port for the
first minute).

The others never wait long for a player. Quitting tells the host, which goes on without you at
once; a player whose game or connection stops is left behind after under a second. Everyone else
sees "P2 LEFT" (or "P2 CATCHING UP") and plays on, the missing player's character standing still. If
your connection only paused, or your game fell out of step with the host's, it notices, connects
again by itself and plays on from a copy of the host's game within a few seconds. A new player takes a free port and comes in at the next join screen: a match under
way cannot take on a new character, so until it ends you watch. When no port is free, a new player
can take the port of one who has been gone for more than a minute.

What JOIN can say:

* **"a different version of the game"**: everyone needs the same build.
* **"different mods"**: everyone needs the same mods on, or none.
* **"a different ROM"**: everyone needs Conker's Bad Fur Day (US).
* **"No answer from ..."**: the address or code is wrong, the host has not pressed HOST, or the
  host's port is not open to you (see below). It comes after 4 seconds. A host still in a match
  started without internet play on also stays silent to anyone outside its LAN: ask them to go back
  to the lobby.
* **"... is in a match that takes no one new"**: the host is still playing a match, which lets in
  only the players of its own lobby (they come back by themselves). Ask the host to go back to the
  lobby, or wait.
* **"... is on this network (it is in GAMES ON THIS NETWORK) but did not answer the join"**: the address is
  right, as the host's game answers the LAN list, but its game port's answer did not reach you.
  Most often your own firewall is dropping it: see *Allow it through the firewall* below. Otherwise
  the host's game may have been busy or stuck for a moment: try again, or press JOIN in the list.
* **"the lobby is full"** or **"the session is full"**: four players is the most. "...'s place is
  kept for them" means a dropped player's machine can still come back for it: try again in a minute.
* **"the host removed this machine from the lobby"**: the host pressed REMOVE.
* **"the code is not a host's address"**: the code or address is a broadcast, multicast or empty
  one, which the game does not send to.
* **"the machine that answered is not the one the join code names"**: something other than the
  host's game answered at that address, with another key: another machine posing as the host, or a
  code from before the host's key changed. Ask the host for the code again.
* **"The host at ... has another key than when you last joined it"**: see *Security*. Compare the key
  check the message gives with the host's screen, and press JOIN again if they match.
* **"the code names this machine itself"**: a join code is for another machine. Type `127.0.0.1`
  instead to join a game on this one.
* **"internet play is off"**: the address or code is outside the LAN. Start the game with
  `--net-unsafe-internet`, on every machine, to play over the internet.
* **"the match is full, or takes only the players of its lobby"**: a match started from a lobby
  takes only that lobby's players, plus dropped players coming back on their own machines.

## When the port cannot be opened

The host needs UDP port 37800 reachable from the other players' networks. Players on the same
network as the host need nothing of this.

1. **Forward the port by hand.** In your router's settings (usually a web page at its address,
   such as `192.168.1.1` or `10.0.0.1`; look for *Port forwarding*, *Virtual server* or *NAT*),
   forward **UDP** port **37800** to this machine's local address, the one the lobby shows next
   to *This network*. Give this machine a fixed (reserved) local address in the router's DHCP
   settings, or the forward will point elsewhere after a restart.
2. **Allow it through the firewall.**
   * macOS: the first time you host, macOS may ask whether to accept incoming connections; allow
     it. If you said no, open System Settings, Network, Firewall, Options, and allow the game.
     This is needed on the joining machines too when their firewall is on, and with *stealth mode*
     it may not ask at all: add the game there by hand. A build you compiled yourself is not
     signed, so macOS knows it by its file: after every rebuild remove the old entry and add the
     game again. Packaged releases are signed and keep their entry.
   * Linux: allow UDP 37800 (for example `sudo ufw allow 37800/udp`).
   * Windows: allow the game when Windows Defender Firewall asks (private networks are enough on a LAN).
3. **Behind two routers** ("double NAT", the lobby says the router's outside address is private):
   forward the port on both, or put the inner one in bridge mode.
4. **Carrier-grade NAT** (the outside address starts with `100.64` to `100.127`, or your provider
   says so): there is no IPv4 port to open. Use IPv6 if both sides have it, or an overlay network.

### Overlay networks (no router settings at all)

[Tailscale](https://tailscale.com) and [ZeroTier](https://www.zerotier.com) put the players' machines
on one private network over the internet, with nothing to open on any router. Everyone joins the
same overlay network, the host presses HOST, and the others join with the host's overlay address
(Tailscale's start with `100.`), or the join code the lobby shows for it under *This network*. The
LAN list does not reach across an overlay network: use the code or address. Overlay addresses are
carrier-grade NAT ones, so this needs `--net-unsafe-internet` on every machine for now.

## Command line

The lobby starts the session with these options, which also work on their own:

* `--net-host [port]` hosts on that UDP port (37800 by default); `--net-players <n>` waits for n
  players in all (2 by default); `--net-map` also opens the port on the router.
* `--net-join <address:port>` joins one (`--net-want-port <n>` asks for port n, 2 to 4).
* `--net-token <hex>`: the session token. A host lets in only guests that bring it. The lobby's
  START hands one to every machine, in `CONKER_NET_TOKEN` rather than on the command line, where
  other users of the machine could read it. A host started by hand makes its own and prints how to
  join it: `guests join with --net-join <this machine's address>:37800 --net-token <hex>
  --net-host-key <hex>`, and its key check. The token is the session's own; the key the machine's.
* `--net-host-key <hex>`: the host's key, as the host prints it: a guest given it refuses a machine
  that answers with another one (see *Security*). Without it, a guest takes the key the host offers,
  unless it has joined that address before with another key, which it refuses with both key checks.
* `--net-open`: a host without a token, which lets anyone with the same build join (the test
  scripts use it). Anyone who reaches the port can then take a seat and gets a copy of the host's save.
* `--net-delay <frames>` sets the input delay by hand. Given to a lobby, it is passed on to the
  match the lobby starts. Without it, a lobby's match takes the delay from the pings the host heard
  during the lobby: the slowest player's 90th-percentile ping, plus 45 ms for frame timing and
  jitter, in 33 ms frames (a 150 ms ping gives 6 frames). A host started directly with `--net-host`
  measures the round trip once, at the start. If "WAITING FOR P2" keeps showing, the delay is too
  short for that player's link: a larger `--net-delay` trades a little input lag for no stops.
* `--net-lobby` opens the lobby at boot instead.
* `--net-unsafe-internet` allows play outside the LAN (see the top of this page). Without it,
  everything above is LAN only. The lobby passes it on to the session it starts.

docs/diagnostics.md lists the switches for testing.
