# Crash reports

What a player's crash looks like from this end, and how it gets here.

Before this, a crash reached us as a screenshot of the dialog: an exception
code and a stack of `[base]+offset` lines. That is enough to name the function
(CLAUDE.md, **Debugging**, has the `addr2line` recipe) and nothing else. The
v3.4.0 report that started this - an access violation in `weatherResetRooms` -
could not be reproduced here on any stage, with or without the mod, under ASan
or under wine, because what was missing was never in the dialog: which mission,
which mod, which switches, and what the log said on the way in.

## The two halves

**Writing** is `crashReportSave()`, called from `sysFatalError()` - so every
fatal error gets one, not only an access violation. It composes and writes;
it does not transmit. The file lands in `$S/crashreports/crash-<stamp>.txt`
and holds:

- the build (`sysGetVersionString()`), the channel, the time, the mod directory
- the dialog's own text: the exception, the stack, and on Windows the `FAULT:`
  line naming the address and whether it was read or written
- the `[Mod]` section of pd.ini, from `configDumpSection()` - the **live**
  values rather than the file's, because a setting changed from the menu since
  the last save is exactly the sort of thing worth knowing
- the last `CRASHREPORT_LOGLINES` (200) lines of the log

**Sending** is `crashReportSend()`, which POSTs to `<Mod.GhostServer>/crash`
through `ghostnetSend()` - the same transport the ghost client and the updater
use, so there is one WinHTTP half and one libcurl half in the port and not
three. A report that is sent is deleted locally; one that is not stays.

## The log is a ring, not the file

`pd.log` **only exists when the game was started with `--log`**, which no
player does. So the report does not read a file: `sysLogPrintf()` copies every
line it formats into a fixed ring in crashreport.c, and the report writes that
out. This is the only copy of those lines an ordinary player has, and it is why
a report from somebody who has never touched the command line still says which
stage was loading.

## Where the player says yes

Two places, and neither one sends anything on its own:

- **The crash dialog** grew a *Send report to Dab* button (`SDL_ShowMessageBox`
  with two buttons in place of `SDL_ShowSimpleMessageBox`). It sends with no
  note, because a message box cannot take text - and the player about to close
  a crashed game is the one who will say yes now and never again. The report is
  on disk **before** the box goes up, so Close loses nothing.
- **Crash Reports**, on the Perfect Menu next to Check for Updates
  (`port/src/crashreportmenu.c`). The row reads *Send Crash Report* when one is
  waiting. This is where a note can be typed, and where a failed send is
  retried. The send is on a worker thread and the page polls it, the way Check
  for Updates and the ghost menus do theirs.

The dialog and the page both say what is in a report before offering to send
it. A player deciding whether to hand over their log should be told that is
what it is.

## Sending from inside a crash

The dialog's button does network I/O in a process that has already crashed -
on Linux, inside a signal handler. That is a real risk and it is taken
knowingly: the file is written first, so the worst case is a second crash with
the report still on disk for the menu to offer next time. Do not add anything
to that path that is not needed to send bytes.

## What is and is not a report

`sysFatalSetupError()` is `sysFatalError()` without the report: the box shows
the message and nothing is written. It is for what the player fixes from the
message itself - no ROM, the wrong ROM, no OpenGL 2.1, no SDL window - and 19
of the first 45 reports were exactly that, each saying nothing more than the
box had. A fault, and any fatal error the player could not have caused, stays
`sysFatalError()`.

A report also carries `[Game]` (Game.MemorySize is there) and the pools' free
space, because four of the first reports crashed after "memory pool is full"
with no way to tell a lowered MemorySize from a level that outgrew 64 MB. The
log ring is only 200 lines, so anything that logs on every call empties it:
`texpack: 0 packs` did, once per dropdown opening, and now logs on a change.

## Reading them

```sh
scp 'sdg@10.8.0.1:~/pdghosts/crashes/*' <scratch>/crashes/
gh release download v3.5.0 -p pd.x86_64-windows.exe -p pd.x86_64-linux -D <scratch>/bins
```

The `version:` line names the commit; `git tag --contains` names the release.
Windows lines are `[base]+offset`, so `0x140000000 + offset` through
`x86_64-w64-mingw32-addr2line -f -C -i -e`; Linux lines are `(+offset)` and go
to plain `addr2line` as they are (the release's Linux binary keeps its DWARF
too). Group by the first frame's offset before reading any one of them - the
first triage (2026-09-14) was two offsets for 14 of 26 real crashes:
`shotCalculateHits` reading `g_Textures` unbounded for the spark colour, and
the spawn chooser dividing by a zero pad count. Read the log ring bottom up for
the stage (`xblastage:`, `setup:`, `run:` lines) and the `[Mod]` block for the
switches every report of a cluster shares.

The `[Mod]` block is the **live** values at the crash, so a report taken the
frame after F6 reads every release switch as off. The log ring's
`xblaswitch: release assets off` line is what says they were on until then.

A player's Randomizer run replays from its `run: begin seed` line, but only
with their map pool: a scratch ini that mounts every mod (`MapMods=*`, as
`build/pd.ini` does) deals a different first stage from the same seed. A seed
above `S32_MAX` does not fit `Mod.RandomizerSeed`, so set it from gdb at
`modrun.c`'s `g_ModRunSeed = ...` line
(`set g_ModOptions.randomseed = (int)SEED`).

## The second pass (2026-09-14, reports after 20260914-023241)

13 reports: three were "Could not open ROM" from v3.5.0, which predates
`sysFatalSetupError()`; the two real v3.5.0 crashes were the first pass's spark
colour and Japanese glyph cache, already fixed in v3.6.0. The two new clusters
both reproduced headlessly on the v3.6.0 commit's own build, and were fixed:

- **F6 off on the Villa** (3 reports, Linux and Windows): "Unknown GBI opcode"
  the frame after `xblaswitch: release assets off`. The switch unloads every
  room to reload it from the other copy, and dyntex adds a room's animated
  vertices once per level, so the release copy's offsets were applied to the
  ROM copy's room: `dyntexUpdateOcean()` wrote wave texture coordinates into
  the upper halves of display list words. `dyntexForgetRooms()`, called by
  `xblaStageSwitched()`. Heap reuse decides whether it shows, so an ASan build
  (whose quarantine hands the reload fresh zeroed memory) runs clean: judge it
  on the normal build. Caught with a `watch -l` on the bad word's upper half
  after the room reloaded (xbla.md, "Segment 5 is the model's").
- **Randomizer hop into MP Complex** (2 v3.6.0 reports plus one dev run):
  a fault in `setupLoadFiles()` walking the paths. An arena's solo setup
  (`UsetuprefZ`) has no AI lists, and the stage list sort read the entry
  after the terminator, which on PC is past the end of the 128-byte file; its
  swaps scrambled the paths. The sort now stops at the terminator
  (randomizer-run.md, "An arena's solo setup has no AI lists").

Left open: a d53c5cd (dev) Linux crash with the chr vertex store full, whose
offsets need a build of that commit to read, and a driver fault inside
`wglChoosePixelFormat` at window creation (7f05950, Windows).

## The third pass (2026-09-15, reports after 20260914-172038)

18 reports, 13 of them v3.6.0 (2595317):

- **Randomizer hop, `setupLoadFiles()` paths** (8, Windows, `+15b697`, read of
  `ffffffffffffffff` right after `run: portal out`): the second pass's AI list
  sort, already fixed by 8c39a8f41 and not yet in a release.
- **F6 off, "Unknown GBI opcode"** (3): two on the Villa are the second pass's
  dyntex fault (c472e7963, unreleased). The third is on Air Force One
  (`bg_rit`), after a string of texture pack toggles; F6 off at frame 1000
  under `f6bt.py` ran clean to 2200 on both v3.6.0 and HEAD, so it is open.
- **GE-X 6a Egyptian, `objFree()` writing `0fff0078`** (3, two players, at
  the stage's end in `objsStop()`): fixed. `objSizeN64()` in
  `port/src/preprocess/filesetup.c` sized a gas bottle by the PC
  `gasbottleobj` (28 words) while the file's is a plain default object (23),
  so every command after one was read five words out. On `UsetupdestZ` the
  command after the one gas bottle decoded as a weapon whose `dualweapon` was
  the misread floor colour, `0x0fff` then `0x0000`: `0x0fff0000`, and
  `0x78` is `dualweapon`'s offset on PC. The converter's own gas bottle case
  had been fixed in a0ab55b37; the size table was not. GE-X's Facility
  (`UsetupearZ`) has 22 gas bottles and was misread the same way; no other
  mod setup has one. Reproduced and verified with `~/pd-crashbins/gexstop.py`
  (`--moddir mods/GE-X_6a_01-19-25 --boot-stage 0x1a`, then
  `mainChangeToStage(0x5a)` at frame 300 and `finish` out of `objsStop`).
  To find a bad command offline, walk the inflated setup (raw deflate after
  the 5 byte `11 73` header, props at header +16) with the N64 sizes, then
  again with the port's.
- **v3.5.0** (4): two are the first pass's spark colour. Open: a NULL text
  in `hudmsgCreateFromArgs()` from `frExecuteHelpScript()`, and
  `bgTestHitOnObj()` reading `0x140003a` from `propFindAimingAt()`. Both were on
  the Carrington Institute after many F6 toggles, from different players.

## The fourth pass (2026-09-22, reports after 20260921-160755)

Three reports, two faults. The dev-channel exe for a commit that is not a
tag is the CI artifact: `gh api repos/{owner}/{repo}/actions/artifacts` and
filter on `workflow_run.head_sha`, then the `/zip` download; they expire,
and 8056aa4 (= v3.8.0) had none left, so that one came from the release.

- **2c412fa, `func0f14a9f8()` reading `0x3bc` under `menuRenderModel()`**
  (2, one player): the Institute's Customize Character head one past the
  head list, read as a Perfect Head the port has no store for - fixed, see
  chrs-and-memory.md "A head past the Combat Simulator's list".
- **v3.8.0 (8056aa4), `gfx_sp_vertex()` reading `0x1738147de00` from
  `gfx_run_dl()`** (1): a Randomizer run's second hop, stage 0x68 = HD
  Caverns (`gebeanstage: cavern at scale 0.50107: 63 of 63 rooms`) on
  the stable release, 11 chrs, F6 not touched. A vertex load from a heap
  address that is not a vertex buffer - a room's list or a mesh's freed
  under it. Open: the seed (2227863130, pool 2, difficulty 2) is above
  S32_MAX and the run needs the player's map pool; not tried on HEAD,
  where the HD stage builder has changed twice since 3.8.0.

## The other end

`tools/pdghostd/pdghostd.py`, `POST /crash`, documented in its README under
**Crash reports**: one file per report under `~/pdghosts/crashes`, no account,
no endpoint that reads one back, twelve an hour from an address, 32 KiB of
report and 200 characters of note, and a cap on the directory. Run
`test_pdghostd.py` before deploying it - the crash cases are in there.
