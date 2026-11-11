# Playing online

Up to four players can play MULTI on different machines, on the same network
or over the internet, with Macs, Windows and Linux PCs in any mix. One player
hosts and the others join. Every machine runs the whole game and only the
controllers' input travels (about 3 KB a second each way), so everyone needs:

* the same build of the game (the lobby says when one differs);
* their own copy of the same ROM, Conker's Bad Fur Day (US);
* no mods on: the lobby's RESTART WITHOUT MODS starts the game again without
  them, for that run only.

**Join only hosts you trust.** When you join, the host sends its whole game to
your machine and yours plays on from it. The game checks what it gets so that
it cannot reach outside the game's own memory, but a host can make anything
happen inside your match.

## The lobby

On the title, go through the **MULTI** door and press Start on any barrel (or
start the game with `--net-lobby`). Controller 1 or the keyboard steers it.
Set your **NAME** first; otherwise you are "Player". Other players see your
pad as its make and model only, never its name.

### Hosting

Press **HOST A GAME**. The game listens on UDP port 37800 and asks your router
to open it (UPnP or NAT-PMP); the lobby says whether that worked. It lists
**join codes** to hand out (**COPY JOIN CODE** copies the first):

* **Internet**: your router's outside address, once the port is open;
* **IPv6**: this machine's IPv6 address, if it has one;
* **This network**: your local address, for players at home or on an overlay
  network.

A code holds your address, port and part of your lobby's key. It is not a
secret, but a guest that joins by code refuses any other machine answering in
your place. When everyone is in, press **START**; you, as player 1, choose the
MULTI mode as on a console, and the others press Start on the join screen.
**REMOVE P2** (and so on) sends a player out of the lobby. Leaving the lobby
or quitting closes the router's port again.

### Joining

Games on your network show up as **JOIN** rows: press one. Otherwise choose
**JOIN** and type the host's join code, address or host name. An address can
have a port after it, for example `203.0.113.7`, `203.0.113.7:37800` or, for
IPv6, `[2001:db8::7]:37800`. A dropped player
gets their place back by joining again from the same machine, and a game that
falls out of step with the host's catches up by itself.

The lobby shows a **KEY CHECK** for the host (the host sees its own). The
first time you join someone by address or from the list, compare it with the
host's over voice or chat. If a host's key changes later, JOIN stops and shows
both; press JOIN again once you have checked the new one on the host's screen.

## In a match

The host's settings that change how the game plays (widescreen, draw distance,
full detail, MP characters, the skips) are used by everyone, and the lobby
shows which differ from yours. A match plays at 30 fps whatever FRAME RATE
says: not every machine keeps 60, and every player waits for the slowest
(SMOOTH MOTION stays each player's own). Your own controls, pad buttons, haptics,
window and volume stay yours. The session plays on a copy of the host's save,
so nobody's own save changes.

Each machine shows only its own player's view over the whole window, with
sounds as that player hears them. MY VIEW in the debug menu's ENHANCEMENTS
page (or `CONKER_MY_VIEW=0`) keeps the shared split screen instead.

The bottom left corner shows your player, the ping and the input delay in
frames. The delay is set from everyone's ping when the session starts. If your
own link to the host gets slower, your own delay goes up to cover it, so that
the others' game does not wait for you, and comes down again when the link
recovers. The game does not stop for a player who is coming back either: they
join in once their machine has caught up.

To leave, press the debug menu's key (Back on a pad, `` ` `` on the keyboard)
and choose **LEAVE THE SESSION**: your own game comes back as it was before
the session.

## When others cannot reach the host

Players on the host's own network need none of this.

1. **Forward the port by hand** if the lobby could not open it: in the
   router's settings (*Port forwarding*, *Virtual server* or *NAT*), forward
   **UDP 37800** to the address the lobby shows under *This network*, and
   give that machine a fixed address in the router's DHCP settings.
2. **Allow the game through the firewall**:
   * macOS: allow incoming connections when asked, or add the game in System
     Settings > Network > Firewall > Options (needed on joining Macs too).
   * Windows: allow the game when Windows Defender Firewall asks.
   * Linux: allow UDP 37800 (`sudo ufw allow 37800/udp`).
3. **Behind two routers** (the lobby says the outside address is private):
   forward the port on both, or put the inner one in bridge mode.
4. **Carrier-grade NAT** (the outside address is `100.64.x` to `100.127.x`):
   there is no port to open. Use IPv6 if both sides have it, or an overlay
   network.

**Overlay networks** such as [Tailscale](https://tailscale.com) and
[ZeroTier](https://www.zerotier.com) put everyone on one private network with
nothing to open on any router: join the same network, host, and have the others
join with the host's overlay address or the *This network* code.

## What JOIN can say

* **"a different version of the game"**, **"different mods"**, **"a different
  ROM"**: see the list at the top.
* **"No answer from ..."**: the code is wrong, the host has not pressed HOST,
  or the host's port is not reachable (see above).
* **"... did not answer the join"** for a game in the list: usually your own
  firewall is dropping the reply.
* **"is in a match that takes no one new"**: ask the host to go back to the
  lobby.
* **"the lobby is full"**: four players is the most.
* **"the machine that answered is not the one the join code names"**: ask the
  host for the code again.

## Command line

* `--net-lobby` opens the lobby at boot.
* `--net-lan-only` keeps the game on the local network for that run: nothing
  from outside it is answered and nothing is opened on the router.
* `--net-host [port]`, `--net-players <n>`, `--net-join <address:port>`,
  `--net-token <hex>` and `--net-host-key <hex>` start a session without the
  lobby (a host started this way prints how to join it).
* `--net-delay <frames>` sets the session's input delay by hand, if
  "WAITING FOR P2" keeps showing.
* `--net-no-mods` loads no mods for that run.

[diagnostics.md](diagnostics.md) lists the switches for testing.
