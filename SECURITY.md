# Security

## Reporting a problem

Report a security problem privately, not in a public issue: on this repository's **Security** tab,
choose **Report a vulnerability**
([direct link](https://github.com/DevOldSchool/conkers-bfd-recomp/security/advisories/new)). Say
what an attacker can do, on which build and platform, and how to make it happen. You will get an
answer there, and the fix is published with a new release before the details are.

## What counts

Fixes go into the latest release; older builds are not patched.

Most worth reporting is anything that lets **another machine** do harm on yours:

* **Online play**: a host, a player or anyone else on the network making the game reach outside its
  own memory (run code, read or write files), crash on purpose, or give away more than the session
  needs, through packets, join codes, the snapshot of the host's game a joining machine loads, or
  port mapping on the router. Inside a match a host can make anything happen, by design; that is
  why [docs/online-play.md](docs/online-play.md) says to join only hosts you trust.
* **Files the game reads that people pass around**: debug reports, saves, texture packs.
* **Mods**: a mod is code, and installing one means trusting whoever made it
  ([docs/modding.md](docs/modding.md)). A mod that reaches outside what the mod runtime gives it
  (other files, memory outside the game's) is worth reporting; a mod that does harm through what it
  is allowed to do is not a vulnerability in the game.

Crashes and bugs with no way for someone else to cause them belong in the issues.
