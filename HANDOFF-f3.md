# F3 20260926-052358-cfb616f1: GE MP pause "nearly impossible to see"

Tester (Windows 5ac4c31, 800x600, Vulkan, fovy 90): Combat Simulator on stage
0x11 = Runway of **GoldenEye Arenas** (GE-X installed but dormant). Start in a
remake stage's match is GE Plus's multiplayer overlay (gewatch.c
`watchMpRender()`, GoldenEye's mpmenu.c), not PD's pause dialog.

## Cause
`watchMpRender()` laid its text out with `watchTextFrame()`, the *solo watch's*
frame, which is sized to the watch face at `zoominfovy` (5.9 deg when the arm is
up). In a match no watch comes up, so at fovy 60-90 the 320x240 frame was
~25-45 px tall: the whole overlay was a green smudge above the crosshair (it is
visible, magnified, in the tester's screenshot). Not a mod colour table, XBLA
font (overlay uses GE's Bank Gothic via gexfront.c) or resolution issue.

## Fix (port/src/gewatch.c only)
- `watchMpTextFrame()`: GoldenEye's 320x240 fitted over the player's viewport
  (height fits, x centred, square columns - same idea as gehud.c's frame).
- Rows keep GoldenEye's spacing but the block is centred down the view
  (at y 22 the title sat under PD's own health bar); step shrinks from 16 when
  12 characters would run off the bottom.
- A black 0xa0 panel behind the rows (`COL_MPPANEL`): GE draws its green
  straight onto the view, unreadable on Runway's snow. This is an addition
  over GoldenEye; drop the fill if the user wants GE's bare look.

## Verification (rig ~/wt/f3gexpause-rig, run.sh; pics ~/wt/f3gexpause-pics)
Runway arena is stage 0x52 here (no GE-X in the rig), `--mpsims 3`,
gdb `geWatchPause()` + `'gewatch.c'::g_MpWatch[0].mode`:
- base800 / fix800 (GL), fixvk800 (Vulkan, RX 580), base1080 / fix1080,
  fix12 (11 sims, all 12 rows fit). Before: smudge; after: readable.
- Stock PD MP pause (0x32, `mpPushPauseDialog()`): pdbase vs pdfix pixel
  identical (AE 0).

## Open
- Splitscreen (2-4 humans) not captured; frame follows viGetView*, so each
  view gets its own box.
- Not merged/pushed/deployed.
