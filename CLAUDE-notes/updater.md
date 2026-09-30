# The game replaces itself

## Digest (moved from CLAUDE.md, 2026-09-30)

The entries CLAUDE.md carried for this note, verbatim. The sections below are
the long form.

- **Check for Updates** — [updater.md](CLAUDE-notes/updater.md): `update.txt`, the baked-in channel, the two-rename swap


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

## The update notice (2026-09-30)

`updatemenu.c`, bottom half. At startup `updatenoticeStart()` asks this build's
**own** channel once, on the updater's worker (`updateCheckInBackground()`: a
failure goes back to idle without a word, and `updateShutdown()` detaches a
startup check still in flight instead of waiting on it - WinHTTP only looks at
the cancel flag once connected). A newer build puts "Update Available" over the
Perfect Menu: Update Now (opens Check for Updates and asks again), Later, Skip
This Version (`Mod.UpdateNoticeSkipDev` / `SkipStable`, per channel), Don't Show
Update Notices (`Mod.UpdateNotice`, also a checkbox on the page; on by default -
the owner's call, it is about the mod, not the game).

- Owner's rule: the notice only ever announces the player's own channel. The
  other channel is the switch row on the page, never the notice.
- The answer is cached in pd.ini (`Mod.UpdateLastCheck/Build/Version/Commit`)
  for 6 hours, keyed by `<hash>-<channel>`, so a switch or an update asks afresh.
- It opens from the Perfect Menu's tick when that menu is on top and the
  controls have been left alone for 45/60 s - after the patch notes popup, never
  in a mission, never under GE Plus's folder.
- Off on a fresh install's first start and under automation (`--boot-stage`,
  `--fixed-step`, `--exit-frame`, `SDL_VIDEODRIVER=offscreen`,
  `--no-update-notice`, `PD_NO_UPDATE_NOTICE=1`). Testing:
  `PD_UPDATE_NOTICE_FAKE=<version>` pretends the check found that version (no
  network, no cache); `PD_UPDATE_NOTICE_FORCE=1` lifts the automation guard for
  a real check against `Mod.UpdateServer`.

## Switching channels

A check the player makes also fetches the other channel's `update.txt` and
`patchnotes.txt` (`updateFetchOther()`); the page shows both channels' latest
("patch N" = the newest entry in that release's notes, which is how a dev build
and a stable tag are put in order - both number from the same file) and a
"Switch to Stable/Dev" row. Its confirm dialog explains the channels, says
DOWNGRADE when the other build's patch number is lower, then
`updateBackupSaves()` copies pd.ini, the eeprom, mpsetups and the other top-level
.ini/.bin saves (not vulkan-*.bin) into `backups/<date>-<time>/` with a FROM.txt,
and `updateSwitchChannel()` downloads through the same size/hash/rename path as
an update, except the replaced build is kept as `<exe>.prev` (nothing removes
it). No pd.ini key remembers the channel: the downloaded build has its own
channel baked in, so it follows that channel from then on.

With `Mod.UpdateServer` set, the other channel is `<server>/<channel>/` (own
channel stays at the root), so one local server can play both.

Going backwards is safe for the caches: every versioned cache compares its stamp
for equality (GE conversion `CONVERT.txt`, the HD level cache header, the mod
importer's IMPORT.txt, saved option blobs), so an older build rebuilds rather
than misreads. An older build drops pd.ini keys it does not know - that is what
the backup is for. A build from before this feature (stable v3.10.0 and older)
has no switch row, so a player who switches to such a stable cannot switch back
in game until the next stable: the `.prev` file is the way back (rename it over
the exe), and the backup folder restores the settings.

`UPDATE_SWITCH_FIRST_NOTES` (updatemenu.c, 20) is the first patch notes entry
whose builds have the switch row; the confirm dialog warns that a target older
than that has no switch back. **Set it to the entry this feature ships in** if
the merge numbers it otherwise.
