# F3 20260926-052358-cfb616f1: GE MP pause "nearly impossible to see"

Tester (Windows 5ac4c31, 800x600, Vulkan, fovy 90): Combat Simulator on stage
0x11 = Runway of **GoldenEye Arenas** (GE-X installed but dormant). Start in any
match on a remake stage went to GE Plus's multiplayer overlay (gewatch.c
`watchMpRender()`, GoldenEye's mpmenu.c), not PD's pause dialog.

## Cause
1. Routing: `geWatchPause()` took every match on a remake stage
   (`g_Watch.loaded` = `modloaderStageIsRemake()`), Combat Simulator included.
2. Size: `watchMpRender()` laid its text out with `watchTextFrame()`, the solo
   watch's frame, sized to the watch face at `zoominfovy`. With no watch up, at
   fovy 60-90 GoldenEye's 320x240 became ~25-45 px: a green smudge above the
   crosshair (visible, magnified, in the tester's screenshot).
Not a mod colour table, XBLA font (overlay uses GE's Bank Gothic) or resolution.

## Fix (port/src/gewatch.c only) - user's decisions via coordinator
- `geWatchPause()`: a match's overlay only when `gexFrontIsInside()` (a match
  GE Plus's folder started); otherwise returns 0 and bondmove.c pushes PD's
  `mpPushPauseDialog()`. Combat Sim on GE Arenas maps and GE-X maps (never
  remake stages, so never had the watch) get PD's pause. Solo path untouched.
- GE Plus overlay: `watchMpTextFrame()` fits GoldenEye's 320x240 over the
  player's viewport; rows keep GoldenEye's spacing, block centred down the view
  (clear of PD's health bar), step shrinks from 16 if 12 players would overflow.
  **No background panel** (user: GoldenEye's bare look).

## Verification (rig ~/wt/f3gexpause-rig run.sh + pause.py; pics ~/wt/f3gexpause-pics)
Runway arena is stage 0x52 in the rig (no GE-X there); pd.base = 1fc1832d8.
- base800/base1080: the smudge reproduces.
- cs_start / cs_start1080: Combat Sim on the GE arena, Start logic as bondmove
  (geWatchPause() returns 0 -> mpPushPauseDialog()) -> PD's Player Ranking dialog.
- geplus800 / geplus1080: `'gexfront.c'::g_FrontInside = 1` (as a folder-started
  match) -> bare GoldenEye overlay, readable size, scores + pause pages.
- fix12 (earlier build, with panel): 12 rows fit - row logic unchanged since.
- Stock PD MP pause (0x32): pdbase vs pdfix2 pixel identical (AE 0).
- GE Plus solo watch (mission 0x5e, frame 400): solo_base vs solo_fix2 AE 0.

## Open
- A real folder-started GE Plus match not driven end to end (flag set by gdb).
- Splitscreen not captured; frame follows viGetView* per player.
- Not merged/pushed/deployed.
