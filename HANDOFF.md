# Patch notes on Check for Updates, crediting testers (feat/update-patch-notes)

Worktree /home/sdg/wt/patchnotes, based on dabs-mod 1fc1832d8. Not merged, pushed,
tagged or deployed.

## What a player sees

- **Check for Updates** has two new rows under Re-Download:
  - *What's in the Update (N)* when a check found a newer build whose notes have
    entries this build lacks (N = number of lines), else *What's New in This Build*.
    Either opens a scrollable page of the notes.
  - *Show What's New After Updating* (checkbox, `Mod.PatchNotesPopup`, default on).
- **Once after an update**, the first time the Perfect Menu is on top, a *What's New*
  page opens over it with every entry newer than the last one this install saw.
  Back or confirm closes it; up/down (stick, d-pad, arrows, mouse wheel) scroll.

## Files

- `patchnotes.txt` (repo root) - the notes. Its header says how to add an entry.
- `port/src/patchnotes.c`, `port/include/patchnotes.h` - parse, format, the page
  (`g_PatchNotesMenuDialog`), the popup, the two pd.ini keys.
- `port/include/patchnotesdata.h.in` - CMake writes `build/port/include/patchnotesdata.h`:
  `PATCHNOTES_NUMBER` (newest entry) and the whole file as bytes. Only patchnotes.c
  includes it, so a new entry rebuilds one file (versioninfo.h would rebuild all).
- `CMakeLists.txt` - reads the number and bytes after versioninfo.h; patchnotes.txt is
  a `CMAKE_CONFIGURE_DEPENDS`, so editing it re-runs configure on the next build (no
  manual `cmake -B build .` for this value, unlike the commit hash).
- `port/src/update.c` / `update.h` - after a successful manifest fetch the check
  fetches `patchnotes.txt` from the same release URL (same default budget, 512 KB cap)
  before publishing its result. Any failure is a log line (`update: no patch notes
  (...)`) and no notes, never an error on the page. `updateCopyNotes()`,
  `updateGetNotesGeneration()`.
- `port/src/updatemenu.c` - the two rows.
- `src/game/menuitem.c`, `src/include/constants.h` - `DESCRIPTION_PATCHNOTES` (0x0d)
  scrollable text source; its length is re-measured every tick (the notes can arrive
  while the page is open).
- `src/game/mainmenu.c` - `menudialogMainMenu` MENUOP_TICK (Perfect Menu on top) calls
  `patchnotesMainMenuTick()`.
- `port/src/main.c` - `patchnotesInit()` straight after `configInit()`.
- `.github/workflows/dabs-mod.yml` - "Lay out the bare builds..." copies
  `patchnotes.txt` into `ci-release/`, so both release uploads carry it next to
  update.txt.

## File format

```
# comment lines and anything above the first "notes" line are ignored
notes 2 2026-09-27
Chicago: the patrol robot's shots no longer freeze in mid-air (thanks Parabolee)
Another fix, one line each (thanks savantique)

notes 1 2026-09-26
...
```

Header = exactly `notes <number> [<date>]`. Every non-blank, non-# line until the next
header is one fix. The game: turns non-ASCII into `?` (one per character), `|` into
`/` (it starts a heading in the scrollable), drops any line containing an IPv4-looking
dotted quad, cuts the page at ~6000 characters on a whole line with a pointer to the
release page's patchnotes.txt.

## How to add an entry at merge time (coordinator)

1. In `patchnotes.txt`, above the newest entry: `notes <highest+1> <YYYY-MM-DD>`.
2. One line per fix: place, then what is different, in player words; end with
   `(thanks Name)` using the report's `name:` field as spelled (or the name typed into
   the note, as LINKmendez did). No name, no credit. Never an IP, report stamp or hash.
3. Blank line after the entry. Commit it with (or right after) the merge; the next build
   re-configures by itself. A build made from that commit carries the new number and
   text; CI uploads the file with the release.
4. One number per pushed batch. Do not add lines to an entry that has been pushed
   already - builds that have it consider it seen - start a new number instead.
5. Today's unmerged fixes (headshots, G5 beams, Dam gate, etc.) are NOT in entry 1;
   add them as entry 2 when they merge.

## Rules chosen (and why)

- **Popup default ON**, not added to the Settings Preset table. vanilla-defaults
  (new additions default off) is about how the game plays and looks; this changes
  neither, shows once per update, never to a new player, and the user asked for it on.
  Vanilla/Dab's/Ghost Trials presets leave it alone.
- **Last seen** = `Mod.PatchNotesSeen` in pd.ini (per install / savedir), written with
  `configSave()` the moment the popup opens (so a crash does not show it twice).
- **First ever run** (no pd.ini at start): number stored silently, nothing shown.
  **pd.ini exists but no key** = updating from a build before patch notes: shown all
  entries (the whole of entry 1 for today's testers - the point of the feature).
- **Popup off**: the number follows the build silently, so turning it on later does not
  dump a backlog.
- **Downgrade**: the number never goes down; nothing shows.
- The popup uses the build's own embedded copy (works offline). The update page uses the
  fetched copy for what an update brings, and the embedded copy for "this build"
  (by definition what the running build contains; identical offline and online).
- Popup gate: the Perfect Menu's own tick, only while it is the top dialog and fully
  open, only on the CI stage, not while GE Plus's folder is up. So never on the title,
  intro, boot notices (drawn on the window before any menu), mid-mission, or over a
  dialog already open. Once per process.
- Separate page rather than a scrollable on the update page itself: a focused
  scrollable eats up/down, and the update page must stay short at 4:3.

## Seeded entry 1

From the 3rd-7th F3 passes, every fix checked to be in 1fc1832d8's first-parent merges
(fix/f3-goldengun..fix/f3-dam-guard-aim2). Credits by the report's name field
(Odeyseis wrote ODEYSEIS in the 4th/5th pass and Odeyseis in the 7th; the latter is
used). The crash 20260925-211932 has no name field; it came from the same address as
savantique's named reports that night, so it is credited to savantique. Myles'
glasses report (name field empty) and the user's own reports are left out.
The 3rd/4th-pass fixes (dblaney1's GE-X maps; Odeyseis' guard aim, stairs, scientists,
console light; Parabolee's Investigation screens) were included as well as the 5th-7th
because they are shipped and credited - drop them if entry 1 should be 5th-7th only.

## Verification

Linux build `~/wt/patchnotes-build` (RelWithDebInfo, -j4). Rig: `~/wt/patchnotes-rig`
(`run.sh` = offscreen on the RX 580 + gdb `menu.py`, `--boot-stage 0x26`, pushes the
Perfect Menu; `xvfb.sh` = short Xvfb run with real key presses), pictures in
`~/wt/patchnotes-pics`, fake releases in `~/wt/patchnotes-rig/release/{found,current,nonotes,big}`
served by `python3 -m http.server 8471` with `Mod.UpdateServer=http://127.0.0.1:8471/<dir>`.

- Popup, pd.ini present without the key (upgrade from a pre-notes build): What's New
  opens over the Perfect Menu with all 20 lines (popup1-0.png, 4:3; wide169-0.png,
  1280x720); `menuPopDialog` returns to the Perfect Menu; pd.ini then holds
  `PatchNotesSeen=1` (written at once).
- Second boot, same pd.ini: nothing (popup1 KEEP run).
- `PatchNotesSeen=0` + `PatchNotesPopup=0`: nothing, and Seen moves to 1.
- No pd.ini (fresh install): nothing.
- Real keys (Xvfb, xvfb-*.png): title Return -> popup; held Down scrolls to the end;
  Return closes it to the Perfect Menu; Check for Updates -> Check Now -> "What's in
  the Update (32)"; Return opens the page and it stays open; held Down scrolls; Return
  closes back to the page; the checkbox toggles and pd.ini gets `PatchNotesPopup=0`.
- Update found + notes (found-*.png): row "What's in the Update (32)", page shows
  Patch 3 + 2 only; the line with 10.8.0.1 in it dropped; UTF-8 e-acute -> `?`, `|` ->
  `/`; scrolled to maxscrolloffset (1025) the last line is Tester30.
- Over 6000 characters (big-0.png): cut on a whole line, with "(Older notes are in
  patchnotes.txt on the release page.)" as the last line, reachable by scrolling.
- Same commit on the server (current-*.png): "This is the latest dev build.", row
  "What's New in This Build", page = this build's entry 1.
- Notes file missing on the server (nonotes-*.png): check still "dev-abcdef1 is out",
  Download row as before, row "What's New in This Build", log
  `update: no patch notes (the release has none)`.
- Windows: `~/wt/patchnotes-win` configured with cmake/mingw-w64.cmake (WinHTTP on,
  "Patch notes: newest entry 1"); the six changed C files compile clean with mingw.
  Not linked and not run under wine (a full Windows build is ~20 min at -j4).

## Open

- CI: the release job's `cp patchnotes.txt ci-release/` is untested until the next
  push runs it (not run here, as instructed). Until a release carries the file, the
  update page simply shows this build's notes.
- While Check Now is busy its row and Re-Download are disabled, and the menu's
  disabled-row skip moves the cursor down onto "What's in the Update" (it used to land
  on Back). Harmless, pre-existing behaviour; noted in case it surprises anyone.
- The popup only fires from the Perfect Menu in the Institute. A player who boots
  straight into GE Plus's folder sees it the first time they reach the Perfect Menu.
- /tmp on this box is full (2.6 GB of other sessions' files under /tmp/claude-1000);
  a Bash call's output was lost to ENOSPC once. Not touched.
