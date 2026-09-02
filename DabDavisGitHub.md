# GitHub: branches, commits, releases, and the updater

Companion to `CLAUDE.md`. That file is about the game; this one is about how the
work leaves this machine and reaches a player. Read it before pushing anything,
and before cutting a release.

## Where this lives

```
origin    git@github.com:DabDavis/perfect-dark-dabs-mod.git   (public)
upstream  https://github.com/perfect-dark-pc-port/perfect_dark.git
```

`gh` is authenticated as **DabDavis** over ssh, so `git push` and `gh release`
both work without a prompt.

**`dabs-mod` is the repository's default branch and the repository is public.**
A push is immediately what a visitor sees, and — if it is a push to `dabs-mod`
rather than a topic branch — it also rebuilds the dev release that every dev-channel
player's Check for Updates is pointed at. There is no staging step between a push
and a player, so confirm before pushing rather than after.

| branch | what it is |
| --- | --- |
| `dabs-mod` | the mod. Default branch, and the only one CI publishes from. |
| `port` | stock upstream, plus the CI disable below. `git checkout port` returns to stock at any time. Merge upstream into here first, then into `dabs-mod`. |
| `wip/modstages` | incomplete runtime stage registration. Not merged, not built. |

## Committing

Commits here are written to be read later. The subject is one imperative line
with no full stop, naming what changed rather than the files it touched — *Record
on the GPU, and stop taking the frame with it*, not *update record.c*. Under it
goes prose, wrapped at about 76 columns: what was wrong, what it does now, and
why it was done that way rather than the other way. Measurements and the failure
that prompted the change belong here, because this is the only place anyone will
find them a year on.

Claude's commits end with both trailers, the session link being how a commit is
traced back to the conversation that produced it:

```
Co-Authored-By: Claude <model> <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_...
```

Build both targets before committing anything under `port/`. The Windows
cross-build is the only thing that exercises the `popen` half of the recorder and
the WinHTTP half of the network code, and neither is reachable from a Linux build:

```sh
cmake --build build -j8        # Linux
cmake --build build-win -j8    # mingw, see the memory note for the prefix
```

## Pushing

```sh
git push origin dabs-mod
```

Fast-forward only. Nothing here rewrites published history — `dabs-mod` is what
CI builds from and what the dev release tracks, so a force push would strand
every player whose game has already seen a build that no longer exists.

The one exception is the `dabs-mod-dev` tag, which the release job itself moves
with `git tag --force` on every build. That is deliberate and is CI's to do, not
yours.

## Releases: stable and dev

Both come out of `.github/workflows/dabs-mod.yml`, from the same four builds —
`x86_64-windows`, `x86_64-linux`, `x86_64-osx`, `arm64-osx`. What differs is the
trigger and where the result is published.

**Dev is a branch push.** Every push to `dabs-mod` builds, force-moves the
`dabs-mod-dev` tag to that commit, and re-uploads the assets to a prerelease of
the same name. It is one rolling release that always describes the tip of the
branch, so it is a different program every time and has no version number of its
own — it is named `dev-<shortsha>` after the commit it was built from.

**Stable is a `v*` tag.** Tagging cuts a release named after the tag, created
once and thereafter only ever added to. It is not a prerelease, so GitHub makes
it the repository's *Latest*, which is where a visitor who is not looking for a
dev build lands.

```sh
git tag v2.0.4 && git push origin v2.0.4    # cuts the stable release
```

There is nothing else to do — no release notes to write by hand, no assets to
upload. Existing tags: `v2.0.3`, `v2.0.2`, `v2.0.1`, `v2.0.0`.

Each release carries four archives — a zip for Windows, a `.tar.gz` for Linux and
a `.tar.xz` for each macOS build — *and* the four bare executables beside them,
because the updater downloads one file and puts it where the running program is.
Teaching it to unpack a zip and two kinds of tarball, one of them xz with no
decompressor linked in, is the thing the bare builds exist to avoid. The bare
ones are named after the platform string the binary carries in `VERSION_TARGET`,
since `pd.x86_64` for Linux and `pd.x86_64` for macOS would otherwise be one
asset name for two different programs.

Alongside them is `update.txt`, the manifest, written by the release job:

```
version v2.0.4                  (or dev-28387cf for a dev build)
commit  28387cf
build   x86_64-windows pd.x86_64-windows.exe <sha256> <bytes>
build   x86_64-linux   pd.x86_64-linux       <sha256> <bytes>
build   x86_64-osx     pd.x86_64-osx         <sha256> <bytes>
build   arm64-osx      pd.arm64-osx          <sha256> <bytes>
```

## The updater, from the release side

`port/src/update.c`. `CLAUDE.md` covers how the game replaces itself; what
matters here is that **the channel is baked in at build time and the two channels
never mix**.

CMake takes `UPDATE_CHANNEL` (`dev` or `stable`, defaulting to `dev`) into
`VERSION_CHANNEL`, and the game reads one of two URLs accordingly:

| channel | where it looks |
| --- | --- |
| `stable` | `.../releases/latest/download/` — GitHub's own redirect to the newest non-prerelease, so which release that is stays the server's decision |
| `dev` | `.../releases/download/dabs-mod-dev/` — named directly, because the rolling tag is one GitHub would never call "latest" |

The release job sets it from the trigger: a tag build is `stable`, everything
else is `dev`. This is why the binary cannot work it out for itself — the job
runs `git checkout -B dabs-mod` for tag and branch builds alike, so nothing
inside the checkout says which it was.

A stable player is offered stable releases and a dev player dev builds, and
nothing moves anyone between the two. Cutting a release therefore updates only
half the players; the other half were already updated by the push that preceded
it.

**Testing it without cutting a release.** `Mod.UpdateServer` in `pd.ini` points
the whole mechanism at a server of your own, and is the only way to exercise the
download and swap for real. It stays in `pd.ini` once set — a test that leaves it
there is a game that can no longer see real updates.

## Two things that will trip you up

**`git rev-parse --short` picks its own length.** It grows with the number of
objects in the repository, so a CI runner's fresh clone says seven characters
where this working copy says nine — the manifest and the binary can name the same
commit and disagree about how to spell it. Compare by the shorter of the two.

**Upstream's CI is disabled on `port` only.** `c-cpp.yml` builds seven targets
and uploads to a `ci-dev-build` release that exists in upstream's repository and
not in this one, so it would spend an hour to fail at the last step. It could not
be disabled through the API — GitHub registers a workflow when it first runs, and
disabling one that never has returns 404 — so the triggers were emptied instead,
on `port`, where upstream's future edits to the jobs below still merge cleanly.

That commit has **not** been merged into `dabs-mod`, which still carries
upstream's original triggers naming `port`. Nothing fires them from a `dabs-mod`
push and a `port` push uses `port`'s own disabled copy, so it is inert — but it
is a real difference between the branches, and worth knowing before concluding
that a workflow file has been tampered with.
