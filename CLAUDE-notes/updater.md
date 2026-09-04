# The game replaces itself

Check for Updates is `port/src/update.c`. It reads one file — `update.txt` on
the release, written by `dabs-mod.yml` — and downloads one bare executable,
because the releases are a zip and two tarballs, one of them xz with no
decompressor linked in.

**The channel is baked in at build time.** The release job runs
`git checkout -B dabs-mod` for tag builds and branch builds alike, so the
binary has nothing in it that says which it was; CMake takes `UPDATE_CHANNEL`
and puts it in `versioninfo.h`. A tag build follows the stable release, a
branch build follows the rolling `dabs-mod-dev` prerelease, and nothing moves a
player between the two.

**`git rev-parse --short` picks its own length** from how many objects the
repository has, so the manifest and the binary can disagree about the same
commit — a CI runner's fresh clone says seven characters where a working copy
says nine. Compare by the shorter of the two.

**The swap is two renames, never a write.** The file being replaced is the
program doing the replacing, which on Windows cannot be opened for writing at
all. Everything that can fail — the download, the size, the hash — happens to a
file under another name first, and the handover is the last thing `cleanup()`
does, after the window and the audio device are closed. On POSIX `execv` keeps
the pid.

`Mod.UpdateServer` points the whole thing at a server of your own, which is the
only way to exercise it without cutting a release. It is left in `pd.ini` once
set — a test that leaves it there is a game that cannot see real updates.
