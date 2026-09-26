# F3 20260925-235312 (Dam, HD, sniper zoom) - branch fix/f3-dam-zoom-fog

Report: /home/sdg/wt/f3-0926/20260925-235312-7d6c5894.{txt,png}. Zoomed to fov 7 at the
secret island, flat light-blue and dark-blue triangles covered the mountains and the island
hut turned into a pale ghost. Continues section 8 of /home/sdg/wt/f3hdlevels/HANDOFF-f3.md.

## Cause

Two things go wrong together.

1. Perfect Dark shrinks the world while you zoom. `playerUpdateZoom()` (player.c) lowers
   `scale_bg2gfx` below fov 15 (0.3 at fov 7, 0.1 at fov 2), and `mtxF2L()` scales every
   matrix by that amount (`var8005ef10`). An HD level draws every room, and the rooms more
   than 32767 units from the camera use float matrices (room.c `roomTouchMtx()`,
   `G_MTX_FLOATS`). Those float matrices skipped the scale, so when zoomed the far rooms sat
   about 3x deeper than everything else (at fov 7, 1/0.3; the trace's lodscalez 0.1156 is
   unrelated). The release's linear fog (`envStartFog()`, `gebeanStageFogLine(bgGetScaleBg2Gfx())`)
   therefore covered them completely: they came out as solid fog colour, or dark blue where
   the depth order broke. The fog line itself was correct. Probe output: fm = 0.00318/scale
   at every zoom, and the renderer's w is plain eye depth (P[2][3] = -1).
2. The zoom scaling is Perfect Dark's behaviour, not GoldenEye's. PD's fog and far plane are
   set in gfx units, so shrinking the world pushes them out in world units, and the sniper
   sees about 3x further. GoldenEye sets its level scale once, at load (007 decomp bg.c
   `sub_GAME_7F0B4810()`, called only from the stage setup). Its zoom
   (bondview2.c `bondviewUpdateWatchZoomIn()`) only calls `viSetFovY()`, and its z/w fog
   does not depend on fov. So in GoldenEye, zooming never brings anything out of the fog.
   Before this fix, the GE Plus N64 look *did* show the island through the fog at fov 7,
   while at fov 15 it was fully fogged (pics n64-cmp.png).

## Fix (one commit)

- src/game/room.c `roomTouchMtx()`: a room's float matrix now gets `mtxApplyGfxScale()`, the
  scale `mtxF2L()` would have applied (the same helper xblamesh.c uses for its float matrices).
- src/game/player.c `playerUpdateZoom()`: on a level converted from GoldenEye
  (`geRoomActive()`), the zoom leaves the world scale at 1. Fog and far plane now stay put in
  world units at any zoom, in both the HD and the N64 look, as in GoldenEye.
  PD's own stages and GE-X are unchanged. The GE Plus watch zoom (gewatch.c) has its own
  tween and never touched the scale. Cutscene fov goes through `viSetFovY()` only.

## Verification (rig /home/sdg/wt/f3zoomfog-rig, pics /home/sdg/wt/f3zoomfog-pics)

- `zoom7.sh TAG BIN` (ZOOM=, N64=1, VK=1 with SDLVD=x11 MESA_VK_WSI_DEBUG=sw under xvfb-run):
  report camera (-4475.3 13272 4835.6, theta 176.8, room 54, GROUND=13112). Shot 0 is at
  fov 60, shot 1 at ZOOM. `fogprobe.py` holds gdb helpers (`zoom(z)`, `info()`); probe.patch
  is the temporary printf probe that produced the fm/scale/P numbers (not committed).
- HD GL: before, triangles over the mountains (probe2-1.png); after, the island hut and the
  mountains are fogged as at fov 15 (fix2-1.png, fix1-cmp.png). Room fix alone: fix1-1.png.
- HD Vulkan (RX 580): same before/after (vk-cmp.png, top two).
- N64 look fov 7 (GL and Vulkan): the island stays in the fog, as at fov 15 (fix2-cmp.png,
  vk-cmp.png bottom). Before the fix it showed through (n64-cmp.png).
- Fov 60 shots are pixel-identical before and after: HD GL, HD Vulkan, N64.
- A/B un-zoomed HD, frame 400, Dam/Facility/Runway/Bunker/Silo/Train/Archives/Streets
  (0x15 0x63 0x5e 0x6f 0x6b 0x60 0x65 0x61): all 8 identical (ab.sh + abdiff.py).

## Open

- Faint lighter triangles in Dam's far mountainside are there at fov 15 and 60 too, both
  before and after this fix (z15-cmp.png). They look like a fog or alpha mismatch between the
  backdrop and the rooms in front of it. Not a zoom effect; not looked into.
- Behaviour change for the user to confirm: in GE Plus (N64 look and HD) the sniper no longer
  sees past the fog. This matches GoldenEye. To revert it, drop the `geRoomActive()` block in
  `playerUpdateZoom()`; the room.c fix is enough on its own for the HD picture.
- c55258431 (envIsPosInDrawDistance look length, fix/f3-dam-gate-void) is not on this
  branch. No other fov or cam_look assumption was found in the HD fog path:
  gebeanStageTickCamera() normalises, prop and chr fog use world-unit prop->z, and the
  backdrop is fogged from the unscaled world matrix.
