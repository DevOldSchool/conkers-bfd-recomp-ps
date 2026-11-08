# Licenses for the PS5 title's notices

`tools/ps5/package_notices.py` puts these into the title's `licenses/THIRD-PARTY-NOTICES.txt`. They
are kept here because the build does not keep the sources they come from (Mesa's is removed once RADV
is built; PacBrew ships its packages without them, and Monocypher's is in the build volume only).

| File | From |
| --- | --- |
| `mesa-license.rst`, `mesa-MIT`, `mesa-Apache-2.0`, `mesa-BSL-1.0` | [PS5_Mesa](https://github.com/mihawk-99/PS5_Mesa) at the revision `tools/ps5/build.sh` pins: `docs/license.rst` and `licenses/` |
| `libpng-LICENSE` | libpng 1.6.43, PacBrew's version |
| `bzip2-LICENSE` | bzip2 1.0.8 |
| `zlib-LICENSE` | zlib 1.3.2, PacBrew's version |
| `monocypher-LICENCE.md` | Monocypher 4.0.2, the version `CMakeLists.txt` pins |

When a pinned version changes, the matching file is taken again from the new one. The authors' email
addresses at the end of the bzip2 and zlib texts are left out (`tools/check_tree.py` allows none in
the repository); their names and copyright lines are kept.
