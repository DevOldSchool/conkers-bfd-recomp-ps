# CI plan

The constraint that shapes everything: the game cannot be built without the ROM.
`tools/regenerate.sh` extracts code and data from it through the decomp's RZIP
tool and recompiles that into `build/us/RecompiledFuncs`, and nothing in
`src/` links without those files. A public workflow that anyone's pull request
can run must never see the ROM or anything made from it, so the pipelines split
in two: checks that need no ROM, run on everything, and ROM builds, run only on
trusted triggers.

## 1. Checks without the ROM (every push and pull request, forks included)

Cheap, run on `ubuntu-latest`. Built: `.github/workflows/checks.yml`. Each
check is a script that runs the same way locally.

* **Privacy and ROM guard** (`tools/check_tree.py`). Fails on a tracked
  file that is a ROM (by extension, or by the N64 header in any byte order),
  over 2 MB, or holds a home path (`/Users/<name>`, `/home/<name>`,
  `C:\Users\<name>`) or an email address other than no-reply ones; and on the
  same in the messages of the pushed or pull-request commits. The optional
  `PRIVATE_PATTERNS` secret (one regular expression a line) adds names that
  must not appear; it is not given to forks' pull requests, so those run
  without it. The `.gitignore` already blocks the common cases; this catches
  `git add -f` and paths pasted into docs and comments.
* **Patches apply** (`tools/apply_patches.sh`, which the README uses too).
  Applies each submodule's patches in order to a scratch index first, so a
  submodule bump that breaks a patch, or a patch missing from the list, fails.
  Safe to run again on a patched checkout.
* **N64Recomp builds.** Configures and builds the patched `lib/N64Recomp`
  (`N64Recomp` and `RSPRecomp` only).
* **Lint.** `ruff` over `tools/`, `shellcheck --severity=warning` over
  `tools/*.sh`.
* **Docs links** (`tools/check_links.py`). Relative links in `README.md`
  and `docs/`: the file exists and a `#heading` is one of its headings.

## 2. ROM builds (push to `main`, tags, manual dispatch; never pull requests)

Each job checks out this repository and the public decomp, puts the ROM at
`../conkers-bfd-decomp/roms/baserom.us.z64`, checks its SHA-1, runs
`tools/regenerate.sh` and builds.

* **Where the ROM comes from.** `baserom.us.z64` from the private
  [conkers-bfd-assets](https://github.com/DevOldSchool/conkers-bfd-assets)
  repository, checked out with a read-only deploy key (or a fine-grained token
  limited to that repository) kept as a secret here. GitHub never passes
  secrets to workflows from forks, so a fork's pull request cannot read it. Do
  not use `pull_request_target` for these jobs, and never upload the ROM or
  `build/us` as an artifact.
* **Build matrix.** macOS arm64 (RT64, Metal), Windows x64 (RT64, D3D12 /
  Vulkan) and Linux x64 (RT64, Vulkan), RelWithDebInfo. Only macOS is tested
  today, so this is where the README's Windows and Linux claims get checked.
  Cache the regenerated `build/us` keyed on the ROM hash, `config/`, the
  decomp commit and the N64Recomp commit; it is the slowest step and changes
  rarely. The cache stays in this repository's private cache storage, never an
  uploaded artifact.
* **Boot smoke test.** The null build (no window, sound or input) on Linux,
  `CONKER_SKIP_INTRO=1 CONKER_AUDIO=0` with scripted input to start a new game,
  run for a fixed number of frames, fail on a crash or on `main thread stuck`.
  Upload the log only.
* **Save check (nightly).** `tools/save_check.py` over a small corpus of
  debug reports kept with the ROM, reporting new classes against the last run.
  Reports hold game state, not ROM bytes, but keep them private with the ROM
  anyway since they come from players.

Releases are out of scope for now: no packaging and no published binaries.

## 3. Housekeeping

* **Dependabot** for `github-actions` only. The submodules are patched, so
  they are bumped by hand.
* **Upstream watch (weekly).** Try the patches on the latest upstream of
  rt64, N64Recomp and N64ModernRuntime; open an issue when one stops applying.
* **Bug-report triage.** Label a `bug` issue `needs-report` when it has no
  zip attached and no "Where in the game" answer.

## Before going public (once, not a pipeline)

* Scan the whole history (222 commits), not only the tree, for personal
  information and secrets: `gitleaks detect` plus a grep of
  `git log -p --all` for home paths and email addresses. Rewriting history is
  only easy before the repository is public and forked.
* Check that no ROM-derived file was ever committed:
  `git log --all --diff-filter=A --name-only` against the ignore rules, and
  the largest blobs in the history.
* `config/us.syms.toml` is written by `tools/gen_syms.py` from the ROM: check
  that it holds only names, addresses and sizes, no ROM bytes.

## Order

1. Privacy and ROM guard, patches apply, lint (before the repository is public).
2. ROM build matrix with the boot smoke test.
3. Nightly save check, upstream watch, triage.
