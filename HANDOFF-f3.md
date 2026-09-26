# F3 handoffs, pass of 2026-09-26 (combined on merge/f3-0926)

One section per merged branch, newest merge on top; the 1fc1832d8 handoff
(fix/f3-hd-level-render + fix/f3-dam-guard-aim2) is kept whole at the bottom.

Index:
- fix/f3-runway-door-gaps: F3 handoff: Runway door gaps (branch fix/f3-runway-door-gaps)
- fix/f3-dam-zoom-fog: F3 20260925-235312 (Dam, HD, sniper zoom) - branch fix/f3-dam-zoom-fog
- fix/f3-dam-gate-void: F3 20260925-234249 Dam opening gate is a blue void (HD) - branch fix/f3-dam-gate-void
- fix/f3-dam-white-decals: F3 Dam white markings + white pyramid (2026-09-26), branch fix/f3-dam-white-decals
- fix/f3-facility-console-square: F3 handoff: Facility door console "z-fighting" (branch fix/f3-facility-console-square)
- fix/f3-dam-monitor-button: F3 Dam modem monitor + gate switch (2026-09-26), branch fix/f3-dam-monitor-button
- fix/f3-hd-doak-head: F3 HD character heads (2026-09-26), branch fix/f3-hd-doak-head
- fix/ge-guns-on-pd-maps: GoldenEye guns on Perfect Dark maps (2026-09-26), branch fix/ge-guns-on-pd-maps
- fix/f3-ge-sniper-hd: F3 pass 2026-09-26 (GE sniper rifle in the HD look) - handoff
- feat/ge-hd-gas-tank: HD prop textures (F3 pass 2026-09-26) - fix/f3-hd-prop-textures
- fix/f3-runway-emplacement-hd: HD autogun models (2026-09-26), branch fix/f3-runway-emplacement-hd
- fix/f3-tank-aim-fire: F3 GE Plus tank pass (2026-09-26), branch fix/f3-tank-aim-fire
- fix/f3-ge-mines: F3 GoldenEye mines pass (2026-09-26), branch fix/f3-ge-mines
- fix/f3-ge-mission-logic: F3 pass 2026-09-26 (GE mission logic) - handoff
- 1fc1832d8 (fix/f3-hd-level-render, fix/f3-dam-guard-aim2): see the bottom

---

<!-- section: fix/f3-runway-door-gaps -->
# F3 handoff: Runway door gaps (branch fix/f3-runway-door-gaps)

Report 20260925-230105-3075525b (tester stage 0x75 = our 0x5e, Runway, HD look,
frame 393): "door leading to facility ... renders a skybox behind it and there are
gaps". Commit 8fd302785 (port/src/gebeanstage.c only). Not merged, pushed or deployed.

## Cause

- Not the door model. The HD mesh for modelnum 0x29b is Pgx155Z <- Bean
  `new/prop/gasplantsw2do1`, fitted onto the N64 model exactly: HD extents
  x -349.9..350.1, y -787.2..788.0, z -43.7..43.8 against the N64 bbox node
  +-350, -787..788, +-44 (temporary print in gebeanBuildRigid()). GoldenEye's own
  Runway setup (007 decomp `assets/obseg/setup/UsetuprunZ.c`, doors 121/122 on pads
  6/7) has this same double door, model 155 = gas_plant_sw2_do1; in the N64 look the
  two plain grey leaves just read as one door.
- It is Bean's level. The two leaves fill x -3734.1..-3533.5, y -17..195, z
  9461.2..9473.3 (pad box). Bean's doorway (wall triangles 608-613 of the level) is
  x -3734.9..-3531.6, y -16.5..199.5, in a zero-thickness wall at z 9458.8: 4.5 too
  tall, 2 too wide on one side, and 2.4 in front of the doors' face with nothing
  between. Nothing is behind the doors (GoldenEye's Runway has no room there), so
  the sky showed through the strip at the top, the side, and - by parallax through
  the 2.4 - a line along the far side and the foot.
- Survey over all 20 missions (doors from g_Vars.props, Bean triangles dumped):
  most doors have a gap of 0.5-2 units, a handful 4-10.

## Fix (closeDoorGaps() in build(), after clampCutouts())

For every door prop, its pad box (padUnpack: normal/up/look + bbox, the box setup
scales the door to):
1. Snap: a Bean vertex within the door's thickness + 10 of its faces and up to
   min(12, 6% of that side) past a side or the top is pulled onto the edge; its UV
   moves with it (triUvShift) so the picture stays put. Not along the foot (floors),
   not on a side a line through the door's thickness 0.5 past the edge finds covered
   (doorSideOpen - Bunker's bevelled frames), not on the side a sliding door slides
   into (DOORFLAG_0080 + unk98 - Bunker's vertical doors rise into a slot above;
   pulling that down changed the header). Vertices inside another leaf's box stay.
2. Reveals: each wall edge on a side/top in front of the door's face, and the
   floor edge in front of the foot, gets a 2-triangle strip back to the door's
   middle plane (not its face: ending on the face left a dotted line of sky where
   the reveal met the door's edge), clipped to each leaf (a double door's top edge
   spans both), in the source triangle's picture smeared; skipped where the level
   already has a triangle (stripCovered, e.g. Bean's own jamb or the floor running
   under the door).
Per-door triangle lists (doorNearTris) keep it cheap: the mesh phase is within load
noise on Frigate (180k triangles, 29 doors). Log line per level:
`gebeanstage: N vertices pulled onto the edges of D doors, R reveals filled`
(Runway 72 / 6 / 8).

## Verification (pictures in /home/sdg/wt/f3rwdoor-pics/)

- Report camera (-3237.4 142 9140.1, theta 53.7, verta -0.3, room 1), HD:
  report-final.png = base | fix | fix with the leaves moved away | N64. No sky
  round the doors; blue pixels in the door area outside the keypad 1 (base 1765).
- N64 look at the report camera: pixel-identical to base (rep-n64-base-0.png vs
  rep-n64-final-0.png).
- Door animated open and shut from inside room 4 (doorsRequestMode 1 then 2):
  anim-final-sheet.png (base left, fix right) - swings as before, no sky when shut.
- HD A/B sweep, all 20 missions, frame 400 (rig ab.sh, ab-base vs ab-final):
  16 identical; Dam 0x15, Runway 0x5e, Bunker 2 0x64 and Frigate 0x6c differ by
  sub-pixel texture/edge shifts near doors (ab12-*.png, abfinal-0x64.png).
- Close-ups both sides of Runway pads 185 (roller door) and 186, Facility pads
  388/404/406 (cu-*.png; the second view of each has no door drawn because the rig
  leaves the player's room stale there - not a game fault, checked from the right
  room in back185.png); Bunker pad 121 with and without doors (bk121-sheet.png,
  from before the sliding-side rule; Bunker 0x6f is now pixel-identical at spawn).

## Open / not done

- Gaps past the 6%/12-unit reach are left: Bunker 2 (0x64) pads 221/222 (6-16),
  Archives (0x65) pad 510 right side (~10), Frigate (0x6c) six doors' tops (~8,
  the probe finds those tops covered - likely a frame in depth), Facility
  pads 432/442/443 (16 on every side - Bean frames). None reported; look before
  widening the reach.
- The report's own right-edge line at extreme angles is gone; very grazing views
  of other doors may still show a hairline where a Bean wall is thicker than the
  slab (10 units past the door's faces).
- Unrelated items of the 0926 pass stay as sections 3, 4, 6-9 above.

## Rig (outside the tree)

- run dir `/home/sdg/wt/f3rwdoor-run` (copy of f3hdlevels-run), rig
  `/home/sdg/wt/f3rwdoor-rig`: `views.sh` (as f3hdlevels'), `ab.sh`,
  `pd.base` (1fc1832d8) and `pd.final` (8fd302785).
- `survey.sh BIN stages...` (+ `doorsurvey.py`): writes door poses
  (`survey*/doors_S.jsonl`) and needs a binary with the GEBEAN_DUMP hunk for the
  triangles (`survey/` = base dumps, `survey_fix5/` = an earlier fix build);
  `gaps.py doors tris` = per closed door, how far past each edge the level starts;
  `perdoor.py S` / `moved.py S x y z` = what moved near a door between two dumps.
- `doorviews.py S pad...` = camera both sides of a door; `closeups.sh S pad...`;
  `hidedoors.py` (gdb `source` it to lift every door 5000 up for a shot).

---

<!-- section: fix/f3-dam-zoom-fog -->
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

---

<!-- section: fix/f3-dam-gate-void -->
# F3 20260925-234249 Dam opening gate is a blue void (HD) - branch fix/f3-dam-gate-void

Status: FIXED + VERIFIED, fix commit c55258431 (src/game/env.c). Not merged, pushed or deployed.

## Cause

The gate (door prop, modelnum 690, at 16792 13313 25803, room 135) was never flagged on
screen in HD (prop flags 0x4, N64 look 0xc6). In func0f08e8ac() every step passed but
envIsPosInDrawDistance(), the HD look's draw-distance test for objects: it took the prop's
depth as the dot product with `cam_look`. `cam_look` is a unit vector in play but the
look-at offset in a cutscene (length ~2010 in Dam's opening at frame 927, 190-450 in other
openings' swirls), so the gate 2000 units away measured ~4 million deep, past the release's
fog end (80000), and was culled. The blue is the sky behind where it stands.

## Fix

envIsPosInDrawDistance() divides the depth by the look's length (a zero-length look draws).
Nothing else calls it; the N64 look never reaches it (gebeanStageFog() is 0 there).

## Verification (rig ~/wt/f3damgate-rig, pics ~/wt/f3damgate-pics)

- Dam opening, frames 875 + 925 (`FRAMES=875,925 rungdb.sh gateshot.py`): gate drawn,
  prop flags 0xc6 as in the N64 look (gate-cmp0.png, gate-cmp1.png: before / after /
  N64). N64 look before vs after: pixel-identical.
- Gameplay frame 400 (`ab.sh`), HD look, Facility 0x63, Bunker 0x6f, Dam 0x15,
  Runway 0x5e, Archives 0x65: pixel-identical (play's look is a unit).
- Opening swirls (frames 400/460/520): Facility and Bunker identical; Runway changed for
  the better - its shutter door was also culled in HD (the outside showed through the
  doorway at frame 520) and now draws closed, as in the N64 look (swirl-0x5e-cmp.png,
  swirl-0x5e-n64.png).

## Open

- Any other port code reading `cam_look` as a unit during cutscenes: roomsheen.c's shift
  (look along/across) and trace.c print it raw; gebeanstage.c normalises its own copy.
  Not investigated further (no report).
- The other items from the f3hdlevels pass (Facility console, glass, Runway door gaps,
  Dam markings/sniper fog/lamp cone) remain as in that branch's HANDOFF.

---

<!-- section: fix/f3-dam-white-decals -->
# F3 Dam white markings + white pyramid (2026-09-26), branch fix/f3-dam-white-decals

Reports (/home/sdg/wt/f3-0926/), Dam 0x15, HD look:
- 20260925-234751-eaa59e4e: painted ground markings by the truck park drawn as
  opaque pure-white patches.
- 20260925-235806-a256acd5: "weird pyramid object" in the server room - the
  release's lamp light cone drawn near solid white, with a solid white skirt.

Earlier analysis: /home/sdg/wt/f3hdlevels/HANDOFF-f3.md sections 7 and 9.

## Cause

Both are Bean room triangles whose look comes from their vertex alpha: the
markings (tex 61, a white DXT5 dash picture) carry 0x80 (rooms 121/122) or
0x88 tan (room 111); the cone (tex 59, 32x32 white, alpha 251 -> 6) carries
0x82. Three things lost it:

1. bg.c's fog swap (g_GfxGroup01/05; also the no-transparency swap 06/07)
   rewrites writeLeaf()'s picture combiner (G_CC_TRILERP, G_CC_MODULATEIA2 =
   fc26a004 1f1093ff) to (G_CC_TRILERP, G_CC_CUSTOM_06 = fc26a004 1f1493ff):
   alpha = texel x ENV alpha instead of texel x SHADE alpha, because on the
   N64 the RSP writes fog into shade alpha. The PC renderer keeps fog apart
   (gfx_pc.cpp keeps vcn->a under G_FOG), so the vertex alpha was just dropped.
   Traced with a temporary gfx_sp_tri1 hook (DBGTRI): cycle type was 2-cycle
   throughout (the "one cycle" guess was wrong), vertex colours arrived intact
   (ffffff80 / ffffff82), combiner cycle-2 alpha read ENV. Only fog levels
   were hit: Silo and Depot (no fog) already drew the same kind of data faded.
2. The cone's picture has >1% texels at >= 0xf0, so it is not "soft": it sat
   in the opaque leaf as a cut-out, and the renderer's texture edge sets alpha
   to 1 above 0.19 - solid white wherever texel x 0x82 cleared the threshold.
3. The cone's v runs 0 (lamp) .. 1.32 (floor), wrapped: its foot sampled the
   lamp end again (alpha ~250) - the white skirt round the base.

## Fix (commit on this branch)

- gebeanstage.c gebeanStageFogRoom(..., fog): after bg.c's swap, puts the
  picture combiner back (1f1493ff -> 1f1093ff) in served HD rooms; the fog
  render-mode part only when fog is on. bg.c calls it from both swap branches
  (fog, and no-transparency with fog = false). N64 look untouched (HD rooms
  only, gated as before on built/roomData/xblaStageIsRelease()).
- triFades(): a triangle of a picture with alpha whose vertex alpha is under
  0xf0 (xblamesh.c's XBLAMESH_FADE_ALPHA rule for meshes) goes to the
  translucent leaf, not the cut-out path.
- clampCutouts(): a picture used by such faded triangles is clamped in t when
  its v starts at a repeat, runs past it by less than half, and its first and
  last rows' mean alpha differ by >= 0x80 (a fade that does not tile). New
  xblaTexImageEdgeAlpha() in xblatex.c. On all 20 missions this hits only Dam
  tex 59 (logged). A looser first version (any fade texture, range < 2)
  clamped Silo's tex 42/43/45 and erased Silo's catwalk rails - do not loosen.

## Verification

- Report cameras, GL and Vulkan (RX 580, Xvfb + MESA_VK_WSI_DEBUG=sw):
  markings now white/tan at about half opacity; cone a faint glow fading to the
  floor, no skirt. GL vs Vulkan: 11 and 5 pixels > 24 apart.
  Pictures: /home/sdg/wt/f3damwhite-pics/report-cams-gl.png,
  report-cams-vk.png (left base, right fix).
- 20-mission A/B at frame 400, both looks (rig ab2.sh): N64 look 20/20
  identical. HD: 17 identical, Dam (a shoreline grass fade), 0x64 and 0x6d
  (sub-threshold) differ.
- Every affected triangle was logged per level (temporary FADETRI log) and
  cameras aimed at the biggest groups (fvcmp3-<stage>.png). What changes, all
  vertex-alpha Bean triangles, all in fog levels:
  Dam terrain blend strips (tex 71, soft, alpha 0..ff) now fade instead of a
  hard edge; a grating's black overlay gone; markings half white.
  Jungle light shafts (tex 51) fade at their edges. Archives window shadows on
  the floor (tex 41) soft instead of solid black bars. Surface fence (tex 43)
  fades where its vertices say. Aztec floor markings and BAY-4 lettering, and
  Control's floor diamonds/stripes, at their 0x80 (half). Silo/Depot signs at
  0xb3/0x93 look the same before and after (no fog there: they already drew
  faded, so the fog levels now match them).

## Open

- Pictures without an alpha channel that carry vertex alpha (Dam tex 16/47,
  0/ff) still draw opaque, as before - untouched on purpose.
- The room palette (buildPalette(), 64 entries) can still merge an alpha level
  into a neighbour in a very colourful room; not seen here.
- Whether the release really shows Aztec's BAY-4 / Control's floor paint at
  half is inferred from the data (and Silo/Depot already drew so); a Xenia
  draw log would settle it.

## Rig (outside the tree)

- run dirs /home/sdg/wt/f3damwhite-run (A/B) and -run2 (views, logs); rig
  /home/sdg/wt/f3damwhite-rig: views.sh (RUN=, ARGS=--vulkan, VDRV=x11, per-view
  7th field = ground), ab2.sh (both looks, LOOKS=), mkviews.py (cameras from a
  FADETRI log), texprobe.py (gdb: per-texture DBGTRI over frames; needs a
  binary with the hook - pd.dbgtoggle). Binaries: pd.base0 (1fc1832d8), pd.fix3
  (this fix). Pictures: /home/sdg/wt/f3damwhite-pics.

---

<!-- section: fix/f3-facility-console-square -->
# F3 handoff: Facility door console "z-fighting" (branch fix/f3-facility-console-square)

Worktree /home/sdg/wt/f3faccon, based on dabs-mod 1fc1832d8. Nothing merged, pushed or
deployed. No converter change.

## F3 20260925-225534-959e8efc (Facility, HD, frame 6534): FIXED + VERIFIED

"weird z-fighting glitch happening with door terminal button" - spiral streaks along
the top of the door console's red lamp.

**Cause.** Bean's placeholder spiral (texture `_0x008C4635`; the Community Edition's
grille has the same name) is part of the mesh body: `gebeanBuildRigid()` puts every
triangle in group 0, so handing the screen node back (`xblaMeshNodeIsLiveScreen()`)
never took the spiral away. It lies on GoldenEye's screen quad, crossing it: 0.35
model units in front at the top, 0.29 behind at the bottom. The programme and the
spiral both drew every frame and the depth test picked. It is not frame state (the
previous pass looked for one): head on, all three Facility door consoles show the
spiral over the top half of the lamp on the base build (`faccon-base-*.png`,
`usehead-base-crops.png`); the tester's higher camera sees only a strip. Same thing on
Frigate's bridge consoles 1a/2a/3a/3b (spiral streaks at screen edges, one screen all
spiral), Silo's tuning console, and the other level's door console (stage 0x70).

**Fix** (port/src/gebean.c, `beanFindScreens()` + `beanScreenBacking()`): placeholder
vertices lying on a GoldenEye screen quad (parts 0-3; within 5% of the quad's size of
its plane and 10% of its edges) are laid flat 2% of the screen's size behind the
quad and coloured black. Why not drop it or move it into the screen node's group
(both tried): programmes are often drawn without a depth write (MultiMonitor screens
1-3 when flagged, alpha < 255), and without the placeholder the room drawn afterwards
painted over them - Frigate's screens showed the wall behind. The backing keeps the
depth, always loses to the programme, and is black where a programme is translucent
or absent. Only the placeholder texture is touched: Frigate's radar sweeps
(`_0x07D75EC5`) sit over a bezel that hides GoldenEye's quad and stay as they were.
Log line of each rigid build now has `N screen backing`.

**Verified** (pictures in ~/wt/f3faccon-pics/):
- Facility door consoles head on: base spiral on all three, fix clean (`faccon-cmp.png`).
- Use cycle (propobjInteract, programme 46 -> 47, red to green): base spiral over the
  lamp throughout, fix clean throughout (`usehead-cmp.png`, `fix4use-crops.png` at the
  report camera).
- Frigate 12 monitors (`frig-f4-7/8/9.png`, `frig-fix4-sheet.png`): spirals and streaks
  gone, no holes; unchanged shots 0-4, 6 pixel-identical.
- Silo (`silo-cmp.png`): tuning console spirals gone, other 7 shots identical.
- Stage 0x70 door console (`bunk-cmp.png`): spiral gone.
- Community Edition: placeholder matched (same name), Facility lamp clean (`ce-cmp.png`).
- N64 look: 5 Silo monitor shots pixel-identical base vs fix (the change is only in
  Bean mesh builds).
- Sweep of all monitors on the 20 missions: backing applied to doorconsole (4 verts),
  bridgeconsole1a/2a/3a/3b (4), tuningconsole1 (6); no placeholder vertex anywhere left
  off a screen.

Not reproduced exactly: the tester's own strip at the report camera (our build shows
the spiral there only head on). Same geometry, same mechanism; the fix removes the
crossing rather than depending on the angle.

## Rig (outside the tree)

- run dir `~/wt/f3faccon-run` (copies of f3hdlevels-run's cache/mods/saves), build dir
  `~/wt/f3faccon-build`, binaries `~/wt/f3faccon-rig/pd.base` / `pd.fix4`.
- `look.sh` (STAGE=, BIN=, TAG=, MODELS=hex list, DIST=, N64=1, CE=1, EXTRASED=): one
  shot square on each monitor of a stage, contact sheet `TAG-sheet.png`.
- `use.sh` (CAM=x,y,z,theta,verta; USE=0/1; DAMAGE=; DTS=frame offsets): door console at
  (-3533 -236 1521) used, programme state printed per shot.
- `visitall.sh` + `coplanar3.py`/`infront.py`: build every monitor's Bean mesh per stage
  and list Bean triangles on GoldenEye's screen quads (needs the diagnostic binary
  `pd.diag`, patch `tmp-diag.patch`: TMPV/TMPT/TMPQ logs in gebeanBuildRigid).

## Open

- Silo's console2/console3 placeholders are still dropped by `beanVertexDrops[]`
  (a97daeb6e); the backing would handle them too (and keep their depth), but that table
  is shared with sibling branches, so left alone.
- Frigate's radar screens show Bean's still sweep, not GoldenEye's turning radar
  programme (it is behind Bean's bezel). Unchanged by this branch.
- Several Frigate/Silo programmes draw without a depth write; anything that removes
  Bean geometry behind a screen must keep a backing (see above).

---

<!-- section: fix/f3-dam-monitor-button -->
# F3 Dam modem monitor + gate switch (2026-09-26), branch fix/f3-dam-monitor-button

Reports (trace + screenshot in /home/sdg/wt/f3-0926/), both HD look, Dam 0x15,
tester build ef6bffc (x86_64-windows, Community Edition on):
- 20260925-235034-ae5a6c08: "this monitor ... the one where you throw the modem bug
  onto is the wrong model. it should be the monitor, not the gate switch"
- 20260925-234625-7f17e175: "this button normally glows green when pressed to open
  the gate for the truck ... pushing it doesn't change its color"

## Cause (one fault, both reports) - FIXED + VERIFIED

The models are right. The monitor at (11228 13277 10335) is `Pgx335Z` =
GoldenEye `PROP_MODEMBOX` (setup index 290, SingleMonitor, tag 5, programme 5
"green text up"). The switch at (14075 13256 17402) is `Pgx336Z` = `PROP_DOORPANEL`
(MultiMonitor, tags 15-18, two per gate). Dam's ai_26/ai_27 set its screens 0/1 from
gate 8's/9's door state with `tv_change_screen_bank` (our 0x00da): closed 49/48
(red), closing 46/48, opening 48/47, open 48/50 (green). Both mapped by name in
geproptable.h to Bean's `prop/modembox` and `prop/doorpanel`.

Bean's two meshes are the same frame, and each has its screen as a separate quad on
one flat grey texel (all UVs equal): modembox draw 1, `.gpu` offset 1264, vertices
0-3; doorpanel draw 2, offset 1160, vertices 0-7 (the two lamps). The quad lies in
the screen's plane in front of GoldenEye's own screen quad. c5ed72971 already hands
the screen node (part 0-3) back to the game so tvscreenRender()'s programme draws
- it did, underneath Bean's grey card. So in HD the modem screen showed no green
text and looked exactly like the gate switch, and the switch's lamps never showed
the AI's red/green. The N64 look was always right; the AI and the gate were fine.

Fix, commit on this branch: `port/src/gebean.c` `beanVertexDrops[]` gains the two
quads (same mechanism as console2/console3). Notes: CLAUDE-notes/ge-bean.md,
paragraph after "The door consoles' lamp". Offline check
(`.xbla-work/ge-bean/bean2obj.py` Model): of every GoldenEye monitor prop with a
Bean mesh (tv1, console1-3, consolesev2b, doorconsole, modembox, doorpanel) only
these two have a flat-texel screen card. No converter change, no version bump.

## Verification (pictures in /home/sdg/wt/f3dammon-pics/)

- Look check: HD runs use `data/save_hd` (XblaMeshes=1); their logs build
  `gebean: Pgx335Z <- new/prop/modembox` (52 verts/31 tris before, 48/29 after) and
  `Pgx336Z <- new/prop/doorpanel` (52/32 before, 44/28 after). `save_hdbase`
  (copied from f3gemission-run) has XblaMeshes=0 = HD levels, N64 props; used only
  for the N64-look shots.
- Before, HD: `hd-before-modem.png`, `hd-before-panel.png` - grey cards, as reported.
- Before, N64: `n64-before-modem.png` (green text), `n64-before-panel.png` (red lamp).
- After, HD: `hd-after-modem.png` green text; `hd-after-press-sheet.png`: modem,
  switch before the press (red, gate 8 closing), then `propobjInteract()` on tag 15:
  gate opens (door 8 mode 1 -> idle frac 0.95), lamp goes dark -> green brightening
  -> green solid. `n64-after-press-sheet.png`: the same sequence in the N64 look,
  identical colours.
- Modem objective, HD with the fix (`hd-after-modem-objective-sheet.png`,
  modemprobe.py USE=6): thrown at tag 5 -> "Covert modem installed.", objective 2
  complete, using tag 6 starts the countdown.

## Rig (outside the tree)

- build `/home/sdg/wt/f3dammon-build` (RelWithDebInfo), run dir
  `/home/sdg/wt/f3dammon-run` (own ROM copy, Bean symlink, CE zip, own converted
  mods; `pd.before` = 1fc1832d8, `pd.fix` = the fix). Saves `data/save_hd`
  (XblaMeshes=1, CE on) and `data/save_hdbase` (N64 look).
- `views.sh TAG SAVEBASE` with `VIEWS="x y z theta verta"` lines; `runpy.sh TAG
  SAVEBASE probe.py`; `button.py` (both report views, press the nearest switch,
  shots at +20/+60/+150/+400 with door and screen state printed);
  `dumpmodel.py` (gdb `dumpobj TAG`: parts and node tree with live screen lists);
  `modemprobe.py` (copy of build/gexrom's).

## Open

Nothing for these two reports.

---

<!-- section: fix/f3-hd-doak-head -->
# F3 HD character heads (2026-09-26), branch fix/f3-hd-doak-head

Reports (tester savantique, 718d5dc, HD look, GE Plus solo missions), in /home/sdg/wt/f3-0926/:
- 20260925-225738-588b23cc, tester stage 0x7a = our Facility 0x63: "dr doak using wrong face texture"
- 20260925-230158-9586b9ba, tester stage 0x75 = our Runway 0x5e: "geometry bug: head and body are disjointed"

## 1. Dr. Doak's face (225738) - FIXED + VERIFIED

Doak = chr 79, body Cgx035Z (char/techman), head Cgx051Z = GoldenEye's CheaddaveZ
(HEAD_Male_Dave_Dr_Doak). The release's new/head/headdave is a different man (clean-shaven,
glasses, one 256x256 face picture); GoldenEye's Doak (and Bean's own original/head/headdave)
has dark curly hair, moustache, goatee. Not a wrong-texture pick: the file has no other face
and no release file has Doak's. Fix: Cgx051Z left out of gebeanchrtable.h ("the release's is
another face, left in the N64 look"), so Doak wears GoldenEye's own converted head on the HD
coat in the HD look (N64 neck stub under it, as with GoldenEye X heads). Generator:
/home/sdg/perfect-dark/.xbla-work/ge-bean/gen_chrtable.py gained NOT_THEIRS = {'headdave'}
(that file is outside git; the header in the tree is its output, byte-for-byte).
Also affects any random guard/scientist rolled the Dave head (GoldenEye's random pool has it).
Not changed: PD Combat Simulator pool rows "Scientist" (POOLBODY CgeTechmanZ, head/headdave)
and POOLHEAD "CgeheadDaveZ" in gebean.c still show the release's face under the name "Dave".

Verified: Facility, a scientist given the Dave head at model build (Doak himself spawns late,
Secret Agent+): pics /home/sdg/wt/f3doak-run/doakb_cmp.png (rows: before HD, after HD, N64 look).

Open for the user: if a low-poly N64 head on an HD body is not wanted for Doak, the alternative
is picking a release head that resembles him (none is close: headlee has curly hair and a full
beard) - a creative call, not made here.

## 2. Head and body disjointed (230158) - FIXED + VERIFIED

Dead Runway guard, body Cgx037Z (greatguard2), head Cgx059Z (headsteveh), head thrown back:
the ground showed through the throat in a row of teeth. Cause (gebean.c, gebeanBuild(), head
files `head/*`): (a) the head was rigid on the neck, but a release head file's neck reaches into
the collar weighted to the back (lowest ring ~3/4 back, as Bean skins it), so the ring swung out
of the collar; (b) only neck-dominant triangles were kept, dropping the neck's lowest band and
leaving a zig-zag edge (also visible standing: dark notches at nape/jaw from the side/back).
Fix: `neckback` heads (HD head files on the conversion's rows only - not originals, not the
Brosnan heads cut from their own Bond bodies, not pool/GE-X rows) keep the band and carry two
palette entries: 0 = neck, 1 = the back's share. xblamesh.c xblaMeshNeckBack() resolves entry 1
on the body the head is grafted to (joint above matrix 0, bound by the neck's rest offset from
the body's nodes); drawn alone it follows entry 0. Same in xblaMeshHitTest().

Verified (pics in /home/sdg/wt/f3doak-run/): dead guard, tester-like pose (rw_final.png: before,
after, N64 look), Facility guard standing 4 sides (fg_cmp.png, before|after pairs), Dam
greatguard2 guard 3 sides (dam_cmp.png, top before, bottom after). Ourumov (Silo) and Jungle
Cgx011Z: identical except the guards behind them; Xenia-row 0 px; N64 look 0 px on the Facility
guard. Not visually checked: female heads (mandy/marion/sally/vivien; same code path, female
Bean skeleton), Bond (unchanged by construction: fromchar heads keep the rigid path).

## Commits (on fix/f3-hd-doak-head, based on 1fc1832d8; not merged/pushed/deployed)
- Doak table: port/src/gebeanchrtable.h
- Neck: port/src/gebean.c, port/include/gebean.h, port/src/xblamesh.c (head build path +
  pose/hit test only; separable from fix/f3-tank-aim-fire and fix/f3-ge-sniper-hd)
- Notes: CLAUDE-notes/ge-bean.md "Two HD heads: Dr. Doak's face, and a neck that left the collar"

## Rig
/home/sdg/wt/f3doak-run (own mods/ copy, ROM + Bean symlinked), build dir /home/sdg/wt/f3doak-build.
`run.sh TAG probes/look.py` with env: STAGE, HD (1/0 = XblaMeshes), BIN, CHR or HEAD=<file>,
AT, DIST, HOFF, SIDES=deg,..; KILL=1 (chrDamageByImpact, DEAD frames later, KANGLE, HITPART),
UP/HDIST aim at the headspot (read inside xblaMeshPose) or HEADPOS=x,y,z; SWAPHEAD=<file>
SWAPBODY=<bodynum> grafts a head at the first body0f02ce8c for that body (not with DIFF).
Runway chr 23 killed at AT=259 lands in the tester's pose. Binaries: pd-base (1fc1832d8),
pd-new (fix).

---

## Update 2026-09-26: Doak's head decision reversed by the user

The user chose the release's own HD head for Cgx051Z (headdave) over GoldenEye's
low-poly head, although the release's face is not Doak's. 332cf6283 is reverted
(cb052e42c); the generator's NOT_THEIRS is now empty (outside git,
.xbla-work/ge-bean/gen_chrtable.py). The neck fix (ad93e5904) stays.

---

<!-- section: fix/ge-guns-on-pd-maps -->
# GoldenEye guns on Perfect Dark maps (2026-09-26), branch fix/ge-guns-on-pd-maps

Two reports that /home/sdg/wt/verify6/RESULT.md confirmed still open on 1fc1832d8:
- 20260919-174510 (Combat Simulator Pipes 0x29, N64 look): the GE rocket launcher held
  and fired Perfect Dark's rocket (model 287), and nothing showed at the tube's mouth.
- 20260924-035424: "in perfect dark mode, almost all goldeneye weapons use the wrong
  sound effects when firing and reloading".

Both have one cause. GoldenEye's rocket (Pgx202Z, MODEL_REMAKE_FIRST + 202) and its
sfx bank (menu/sfxctl + sfxtbl) come from the conversion, and only a converted level
used them. On a PD stage, `modloaderApplyStageModels()` leaves the remake's model
states empty, and `geSfxStage()` is false. So the gun kept its host's rocket, its
host's shot, and PD's reload and empty-click sounds.

## Fix

- `modloaderLendRemakeModel(slot)` (modloader.c): on a stage that is not one of the
  remake's own, this fills one remake model state from the `models` block for the rest
  of the stage. It only registers the file. The model loads the first time something
  is made of it. `gegunsOwnRocketModel()` and `gegunsChrProjectileModel()` (geguns.c)
  ask through it. A converted level behaves as before, because the fileid is already
  set there or the lend is refused.
- gesfx.c now has three functions for a GE gun on any stage:
  - `geSfxGuns()`: whether the bank is available. It loads the bank on the first call.
  - `geSfxGunShot(id)`: on a converted level, the id itself (the player remaps it, as
    before). Elsewhere, `sfxPropNum(id)`, which is GoldenEye's sample under GoldenEye's
    audio config, so a sim's shot keeps GoldenEye's falloff.
  - `geSfxGunSound(weapon, num)`: for a GE weapon on a PD stage, it resolves the russ
    mapping and applies the same rule as `geSfxRemap()` (now shared as
    `sfxRemappable()`). A config is kept with an appended russ row, cached per row.
- Call sites:
  - game_0b0fd0.c: the shoot sound and the fire-slot duration (SoundTriggerRate).
  - bondgun.c: GUNCMD_PLAYSOUND, SFX_RELOAD_DEFAULT, and the default SFX_FIREEMPTY
    click. Each only changes a GE weapon on a PD stage.
- With only GE-X installed (no conversion), `sfxLoad()` is false, so everything falls
  back as it did before. This was not run; it follows from the code.

## Memory

- Stage pool: the lent rocket costs about 4.4 KB. On Skedar, total stage free at the
  same point was 53675200 on base and 53670704 on the fix. It is only spent once the
  launcher is loaded.
- Heap, not the stage pool: the GE bank (about 23 KB ctl + 797 KB tbl) loads the first
  time a GE gun asks for a sound and is kept. Nothing is loaded on a PD map where no GE
  gun is fired.

## Verification

Rig: `/home/sdg/wt/geguns-pd-run` (a copy of verify6's). `pd.base` is 1fc1832d8 and
`pd.fix` is this branch. Sound is on (`SOUNDFLAG=`), and the probe is
`probes/gunsnd.py`. It tags each sound by the function that called it and resolves the
number to what it plays: geN is GoldenEye SFX_ID N, pd0x.. is PD's. Compare with
`summ.py`. Evidence is in `/home/sdg/wt/geguns-pd-run/evidence/`.

- Rocket, Pipes (`--mpsims 1` is needed: in solo, Pipes has no spawn pads and the player
  falls, which is why verify6's recipe without it hangs on the pak dialog) and Skedar:
  GE launcher 287 -> 714, held and fired (the `weaponCreateProjectileFromWeaponNum`
  model). Its warhead now shows at the mouth (`rocket_pipes_fix_loaded.png`). The PD
  launcher (0x18) still gives 287.
- Sounds (`compare.txt`): on Skedar, all 19 GE guns 0x5e-0x70 now have the same shot,
  fire-slot rate, reload (ge50) and empty click (ge89) as on Dam. Before, they had the
  host's (pd0x5ed and so on, reload pd0x32, click pd0x59). Four PD guns on Skedar
  (0x02, 0x0a, 0x13, 0x24) are identical between base and fix. Dam fix and Dam base are
  identical for all 23 guns, and so is the stage pool.

## Open

- GE sounds on PD maps play at full volume. On converted levels the player scales
  remapped sounds to GESFX_VOLUME, which gives 3/4 against GoldenEye's music. Tune this
  if a tester finds them loud or quiet next to PD's guns.
- Equip (draw) sounds are still the host's switch on PD maps, and so are the Moonraker's
  ricochet pair. Surface, casing and ricochet sounds stay PD's on PD maps on purpose.
- ~~In the N64 look, the third-person held-gun props are still the host alias on PD
  maps.~~ Done below.
- The conversion has no Pgx203Z, so the GE grenade launcher fires PD's grenade round
  (290) on both kinds of stage, as before.

# GoldenEye's own held and floor props on PD maps (caccecbcf)

In the N64 look, a GE gun held in third person on a PD map (by a player, a sim or a guard)
drew its host's pickup (`PchrgeKF7Z` and so on, an alias of the PD gun). So did a GE gun
on the floor. Only a converted level has GoldenEye's PROP_CHR* props.

## Change

- `gegunsOwnPropModel()` asks `modloaderLendRemakeModel(prop)` instead of checking the
  model state's fileid. On a PD stage the prop is registered the first time it is asked
  for. It is read into the stage pool when the first one is made. On a converted level
  nothing changes, because the block has already filled the state.
- New `gegunsFloorModel(weaponnum, fallback)`: setup.c (Combat Sim `MPLOCATION` rows) and
  modrandom.c (random weapons) lay the held prop on the floor. These rows hold
  `MODEL_GE_FIRST + i`, the alias. A gun dropped by a sim, a player or a guard already
  went through `playermgrGetModelOfWeapon()`.
- `currentPlayerDropAllItems()` now checks that a gun is held before it asks for the
  model. Before, every death lent all 21 props.
- 21 props are lent. The throwing knife and the three mines keep the alias, as they do on
  converted levels.

## Verification (rig ~/wt/geguns-pd-run; pd.fix = 5881dee35, pd.new = caccecbcf)

Probe `probes/heldprops.py`, driven by `sweep2.sh`. It hands sim 1 (`g_MpAllChrPtrs[1]`)
each gun through `botinvGiveSingleWeapon` and `botinvSwitchToWeapon`, then logs the model
the sim holds and `playermgrGetModelOfWeapon()`. It drops the gun with `botinvDropOne`
(hooked on `weaponCreateForChr`) and logs every floor weapon at frame 300. Guns
0x5e-0x76 plus PD 0x02/0x0a/0x13/0x24. Runs: Skedar 0x32 and Pipes 0x29 (both
`--mpsims 1 --mp-weapons 49,53,57,61,66,68`), and Dam 0x15 (NOSIM). Logs and montages
are in `evidence/heldprops/`.

- Skedar and Pipes: all 21 guns held and dropped are now `Pgx<prop>Z`, the same model
  numbers Dam gives (703 PP7 ... 723 rocket launcher). Before, each was its
  `Pchrge*Z` alias. The six floor rows are Pgx191/184/194/207/185/186. The throwing
  knife and the mines are unchanged. PD guns are identical between base and fix.
- Dam: models, floor props and pool are byte-identical between base and fix.
- Screenshots (`mont_crop.png`, top = fix, bottom = base): PP7sil is now GoldenEye's
  silenced PP7, not the plain alias. KF7 and RCP90 show GoldenEye's shapes too.
- HD look booted (`XblaMeshes=1`): base, fix and the scratch merge with
  fix/f3-ge-sniper-hd give identical models and pool. `ownInUse` is 0 there, so the alias
  with Bean's row is used, as before.
- HD interplay (scratch merge with fix/f3-ge-sniper-hd, not committed; conflict only in
  geguns.h, where the two declarations sit side by side; geguns.c merged clean and
  `gegunsOwnPropModel()` then lends `gegunsChrProp()`): a lent Pgx prop already in a
  sim's hand when the look is switched to HD (F6) is drawn as the release's gun
  (`mont_tgz.png`: HD silencer and scoped sniper). Without that branch it stays N64. So
  lent props and HD mapping work together once it is merged. Floor props placed in the N64
  look behave the same way.

## Stage pool (mempGetStageFreeTotal)

- The six GE floor rows at frame 300 now leave 11.3 KB more free than before (Skedar
  54303360 vs 54292080; Pipes +11.4 KB). GoldenEye's props are smaller than the host
  pickups they replace.
- Per held prop, about 3-6 KB when it is first made. After the full sweep (all 29 guns
  held and dropped), the fix ends 15.5 KB lower than base (Skedar 54155744 vs 54171280;
  Pipes -15.4 KB). Most of that is the lent rocket (Pgx202Z, from 5881dee35), which only
  now loads for sims because their launcher is GoldenEye's. Nothing is lent or loaded on a
  PD map where no GE gun is in play.

## GE-X only (no ROM), from the code, not run

Without the conversion, `gegunsOwnModel()` is 0, so `ownInUse` is 0 and
`gegunsOwnPropModel()` returns -1 before it lends. `modloaderLendRemakeModel()` would
return -1 anyway, because no `models` block exists. `gegunsFloorModel()` gives back the
row's model. Everything falls back to the alias, as before.

## Open

- A GE grenade on a PD map is now GoldenEye's `Pgx196Z` in the hand and when thrown (the grenade
  is in the held table, as on Dam).
- Props made before an N64/HD switch keep their model until they are remade. This was
  already true on converted levels, and with fix/f3-ge-sniper-hd merged HD is drawn over
  them anyway.

---

<!-- section: fix/f3-ge-sniper-hd -->
# F3 pass 2026-09-26 (GE sniper rifle in the HD look) - handoff

Branch `fix/f3-ge-sniper-hd` (worktree /home/sdg/wt/f3sniper), based on
dabs-mod 1fc1832d8. Not merged, not pushed, not deployed. No converter change
(GECONVERT_VERSION untouched).

Rig: `~/wt/f3sniper-run` (copy of ~/wt/f3gemission-run's run2.sh as `run.sh
TAG PROBE`, env SAVE/STAGE/TMO/BIN/EXTRA). **`save_hdbase` is NOT the HD look**:
it has `XblaMeshes=0` (N64 look with XblaGoldenEye=1). `save_hd` is the same
with `XblaMeshes=1`, which is the tester's HD look. Dam = 0x15, Surface = 0x69.
Binaries: pd-before (1fc1832d8), pd-after (commit 1), pd-after4 (== HEAD).
Build: ~/wt/f3sniper/build (RelWithDebInfo), logs ~/wt/f3sniper-{cmake,build}.log.

Probes (`~/wt/f3sniper-run/probes/`):
- guards.py: `GUNS=sniper|all|none` swaps every chr-assigned setup weapon at
  `setupPlaceWeapon` (weaponnum + MODEL_REMAKE_FIRST+PROP_CHR*), then under
  `--spectate` puts the camera in front of the N nearest guards (two shots each)
  and `FLOOR=x,y,z` at a floor gun. The player model is shrunk to hide the
  spectator laptop. `GUNS=all` takes one guard per gun.
- fp.py / fpfire.py: give + equip 0x6b, first-person shot (`TP=1` third person
  too, `FIRE=1` holds Z via input.c:1046).
- fpmtx2.py / fpmtx3.py: gun model matrices at `bondgun.c:12369` (modelRender) -
  at videoEndFrame they are already freed garbage.
- list2.py: every weapon prop, holder and model.
Pictures in `~/wt/f3sniper-run/pics/` (sweep_sheet.png, fp_final_sheet.png,
surf_pair.png, fp4_blendcrop.png).

## 1. Guards hold N64 sniper rifles in HD (233626-f24d0b98) - FIXED, 112ac8278

- Cause: the player's held gun is MODEL_GE_FIRST+i (alias of the host pickup,
  gegunstable.h row draws Bean's pickup on it). Guards' guns and floor pickups
  are GoldenEye's own props from the converted setup, `Pgx<PROP_CHR*>Z`
  (Pgx210Z = sniper), and no gebean row named them. Class-wide: every GE gun
  held by a guard or lying on the floor was N64 in the HD look.
- Fix: gebeanPoolRowForFile() maps a remake prop file whose number is a gun's
  PROP_CHR* to that gun's pickup row (the rows were fitted by gunfit2.py on
  exactly these GE models, so the HD gun lands where the N64 one was).
  PROP_CHR* table moved to file scope in geguns.c (gegunsChrProp(), throwing
  knife + mines added; gegunsOwnPropModel() still skips the thrown ones).
- Guns checked held by a guard in HD, before N64 / after HD (sweep_sheet.png,
  Dam): sniper, KF7, PP7, PP7 silenced, DD44, Klobb, ZMG, D5K, AR33, RC-P90,
  shotgun, auto shotgun, Cougar, Golden Gun, Moonraker, grenade launcher,
  rocket launcher, hunting knife, throwing knife, grenade (20 guns; D5K
  silenced and Phantom were not handed to a guard in the sweep, same code
  path). Floor sniper on Dam's tower HD after. Surface snow guard with the
  sniper HD after (surf_pair.png - the tester's picture).
- Mines 199-201 have no Pgx file in this conversion (not written unless used).
- Sizes: held/floor HD guns are the same length as the N64 prop (fit on GE's
  longest axis); the HD sniper is thinner than GE's fat silencer tube, which is
  the release's model.
- The HD rocket launcher on a guard is Bean's RPG-like model (the player's
  third-person one is the same) - release art, not a fit problem.

## 2. First-person sniper too small (234109-d222b51a) - FIXED, 23a639ffc

- The screenshot is first person, HD look. Bean's sniper was placed by its grip
  on the palm of the bullpup host and fitted to the host's length, at the host's
  position (21, -27.2, -31.5) vs GoldenEye's (11, -20.7, -31.5). Measured:
  its eyepiece stood ~11 camera units further from the eye than GE's model in
  the N64 look - a thin rifle in the corner (hdfp_before_fp_0x6b.png).
- Fix: fpGrip mode FP_OWNPLACE (gebean.c) - Bean's gun at 1/4.7 with Bean's
  first bone (SKEL_TOP = GE's root, whose own position is not drawn: the root
  matrix equals the gun matrix) onto the host's root, moved by
  10 x (own - host position) with x and z negated (0.1 scale, model faces away
  from the eye), minus how far the host's idle anim (1036, frame 0, constant
  through idle) holds its body matrix 33 from rest: (8.7, 17.6, 164.0),
  measured with fpmtx3.py. gegunsViewPlacement() (geguns.c) returns the two
  positions. Only the sniper uses it; the mode is generic.
- Verified: HD first person overlaid on the N64 look's screenshot coincides
  (fp4_blendcrop.png, fp_final_sheet.png: N64 | HD before | HD after); firing
  (recoil) fine; HD third person unchanged.
- Open: the 164 is the PD sniper host's idle pose, measured, not read from the
  animation at build time. If another gun gets FP_OWNPLACE, measure its host's
  body offset the same way (fpmtx3.py, K=<body matrix>).

## N64 look

Pixel-identical before/after with the final binary: guard + floor shots on Dam
(n64_before_* vs n64_final_*) and first + third person sniper (n64fp_*).

## Not touched

The sibling fix/f3-tank-aim-fire change (c80118821, second bone-binding pass in
gebeanBuildRigid, guns excluded by weaponnum) is separate: this branch does not
touch gebeanBuildRigid, and the new rows carry the gun's weaponnum so they stay
excluded from that pass.

---

<!-- section: feat/ge-hd-gas-tank -->
# HD prop textures (F3 pass 2026-09-26) - fix/f3-hd-prop-textures

Three tester reports, HD look (Bean meshes on, `xbla: meshes 1`), GE Plus missions.
There are three separate causes, one per report. Each fix covers a class of props.

| Report | Prop | Cause | Fix |
|---|---|---|---|
| 20260925-233253 Surface (0x69) "mini-car missing texture" | Pgx301Z <- `new/prop/carbmw` | (b) HD mesh misread. The car's stride-20 vertices are position + normal + **16:16 UV**. The reader took the last 4 bytes as a colour, so the car came out purple/blue and sampled texel (0,0). Its windows are stride 16 (position + normal), which the reader refused, so they were dropped. | `beanShaderUv20()`: the file's own Xenos vertex-fetch instructions decide UV or colour. Stride 16 is now read. |
| 20260925-233617 Silo (tester 0x82 = mission Silo, 0x6b here) "missing reflective texture on satellite" | Pgx069Z <- `new/prop/sat1reflect` | (b) sphere map. The panels are a UV-less draw with a material of their own (44x44 blue sphere). The release's shader generates the UV from the normal (GoldenEye's texgen). We read UV 0,0, which is the black corner. | `beanDrawIsSphereMapped()` + `beanSphereFrame()/beanSphereUv()` in the rigid build bake a sphere lookup from the normal (mean-normal view, slightly from above, surface spread ±0.2). |
| 20260926-000440 Facility (0x63) "computer uses n64 texture" | Pgx035Z console_sev2d | (a) no HD mapping. `geproptable.h` (propfit.json, 2026-09-17) was fitted only on the arenas' props. Mission props never got rows. | propfit run for every Pgx model the 20 missions load without HD. 37 rows added (`geproptable.h` 143 -> 180). |

Commit files: `port/src/gebean.c`, `port/src/geproptable.h` (generated). No converter change, no GECONVERT bump.
The shared functions touched are small: `beanReadVb`, `beanVertex`, `beanWalkStream` (`ownmat`) and `beanLoad` (one line). `beanShaderUvScale` now calls the new `beanBuffersEnd()` helper for its buffer-end loop.

## Class coverage
- **stride-20 UV** (per shaders, all Bean files): new/prop carbmw, carescort, carzil, landmine. `new/background/complex` fetches both a UV and a colour, so it keeps the colour reading as before. Every other file is unchanged: 4J's colours carry non-0xff alphas on whole buffers (0x00/0x7f/0x80/0xb3/0xfc), so an alpha-byte heuristic would be wrong.
- **sphere-mapped rigid draws** (UV-less + own material): sat1reflect panels, carbmw windows, chrkeyyale, chrgoldeneyekey, glassware2 panes, sevdoorwind, sevdoorwood, cargolf/carweird windscreens, rarewarelogo.
  - Skinned col28 draws (tank, helicopter, tiger, cctv, cartridges) are not touched. Their shaders show stride 28 has **no UV at all**, so how the tank etc. get their UVs is a separate open question (maybe a second vertex stream).
  - UV-less draws that inherit a material across a shader change (0x02) are plain colour in the release. They still sample their inherited picture's first texel, which was left alone.
- **props without rows**: the before sweep (`~/wt/f3hdprops-rig/sweep.sh`, 20 missions to frame 400) found 75 Pgx models loaded without HD. Now covered: 004 005 018 020 024 035 036 046 086 096 105 106 107 112 113 115 116 122 126 131 133 134 160 202 234 243 244 248 269-271 274 275 294 296 310 312. After the fix, these are still N64 in HD:
  - no Bean model: 023 041 320-324.
  - fit 0 / below 0.6: 038 desk1, 118 glassware1, 198 briefcase 0.47, 273 plastique, 282 helicopter 0.40.
  - 077/078 TV screens: the Bean file has no stream the fitter reads, and they are monitor programmes anyway.
  - 183 doorstatgate: the fitter finds no GE points. Open.
  - 184-211 chr guns: done on fix/f3-ge-sniper-hd (112ac8278), not here.
  - **Deliberately excluded** (EXCLUDE in `.xbla-work/ge-arena/gen_proptable.py`):
    - autoguns 098 roofgun, 299 groundgun, 292 gunrunway1. The rigid build puts most of the gun on the base/first matrix, so turret yaw/pitch would not carry the HD gun.
    - 117 gastank. The material blends 3 pictures (spot map, landscape reflection, pale base), and the largest-picture rule draws GoldenEye's black bottling-room tanks white.
    - Each needs its own build work, or a call from the user.

## Verification (RX 580 offscreen, `~/wt/f3hdprops-rig`, save_hd has XblaMeshes=1)
- `~/wt/f3hdprops-pics/reports_before_after_n64.jpg`: rows car / satellite / console. Columns HD before, HD after, N64 look.
- New rows: `boxes_compare.jpg` (cardbox3), `barrels_compare.jpg` (gasbarrel), `lockers_compare.jpg` (locker3), `lab_compare.jpg`, and `misc_compare.jpg` (sevdish moves on its 2 bones; the runway/roof guns shown were before the exclusion).
- `tanks_compare.jpg` shows why the gas tank is excluded.
- The 20-mission sweep with the final binary is in `sweep_final/`. No crash, and no new WARNING lines against `sweep_before/`.
- Rig: `cam.sh TAG BIN STAGE cx cy cz lx ly lz`, `shot3.sh NAME STAGE c.. t..` (HD before/after/N64), `objs.sh STAGE "modelnums"` (modelnum = 0x200 + GE prop number).

## Offline tools
- `.xbla-work/ge-arena/propfit.json` now has the new fits. The old file is kept as `propfit.json.before-f3hdprops`.
- `gen_proptable.py` gained EXCLUDE and reproduces the committed header byte for byte.
- Fetch scan research: the scratch `vf.py` pattern is w0&31==0, bit19, w2&0x7fffffff == offset<<8|stride in dwords, fmt=(w1>>16)&63 (6 = 8:8:8:8, 25 = 16:16).

## Open
- The sphere lookup is baked, so it does not move with the view. A live version would need a per-material texgen pass for TABLE materials in xblamesh.c (the logo path, `m->logocol`, is the model to follow).
- Gas tank look and autogun parts, as above.

# Facility's gas tank in the HD look (2026-09-26) - feat/ge-hd-gas-tank

User's call: build the tank's three-texture HD material (prop 117 was left on
GoldenEye's model by fix/f3-hd-prop-textures, whose biggest-picture rule drew
the bottling room's tanks white). Built on 71ec65134 (fast-forward).

## The release's material (read from the file, not seen in the release)
- `new/prop/gastank`, first shader alternative: material 0x2d binds three
  textures, slot = sampler: t0 `_0x0E1C2BF5` 256x256 spot map (grey, black disc
  in the middle), t1 `_0x0C2BCE25` 256x128 landscape (valley under a pale
  sky), t2 `_0x05D30D85` 256x512 picture (pale grey, pool of light at the top -
  GoldenEye's IMAGE_700/716 redrawn). Constants: c12 (c_constant0) = 0.5624
  grey, c13 = 1; c14 comes from a 0x08 record (Maya's "eccentricity").
- Pixel shader, disassembled with Xenia's `xenia-gpu-shader-compiler`
  (built 2026-09-26 in `~/perfect-dark/xenia-canary/build`; tool
  `.xbla-work/ge-bean/psdis.py FILE 0xRECORD`): the microcode is the **pool**
  asset's .gpu (one block a shader in 0x102a1100-header order, 32-byte aligned,
  64 bytes of literals first); vertex shaders are asset 1's .gpu past the
  buffers. Stream record 0x02's word + 0x28 is the pixel shader's header.
  - spot and landscape both fetched at (nx/2+1/2, 1/2-ny/2) of the
    normalised view-space normal; the picture at the UV.
  - `R = c12 * spot * land`; `lit = pic * vcol * (c_ambient + sat(n.L) * c_light0colour)`;
    `out = lit * (1 - R) + R + spec`, then c7 global colour and fog.
- With the release's constants (ambient 1, light 0 on every draw - see
  ge-bean-shader-uv-scale memory) and Bean's white tank vertex colours
  (0xf5-0xff), the release itself would draw the tank **pale grey**: it is the
  picture, plus a rim reflection. The black comes only from GoldenEye's own
  vertex colours (its baked light, 0x00 down most of the body, 0xff along one
  top edge). Not checked in Xenia: the bottling room is too far from any
  scripted spot of the rig.

## What the port draws now (gebean.c + xblamesh.c)
- `beanTexSphereMap()`: the five maps the release looks up by the normal (the
  shared spot + landscape, the ICBM's own spot, the plane's copies) are never a
  material's picture while it has another (`beanMaterialTexture()`).
- `beanWalkStream()` marks the gas tank's shape (spot slot 0, landscape slot 1,
  picture after, c12/c13 only): `d->reflamount` = c12 (143), `reflspot/reflenv`.
- `gebeanBuildRigid()` for such a draw:
  - `beanStockShade()`: the vertex colour times GoldenEye's shade under it -
    closest point of the nearest same-facing stock triangle of the converted
    model's lists (G_COL/G_VTX/G_TRI walk, `beanStockTriangles()`),
    barycentric. 472 of 568 vertices (the plinth keeps its own).
  - `beanReflectPicture()`: spot x landscape as one 256x256 sphere cell,
    handed on in `gebeanmats.env/envamount/envkey` (gebean.h).
- xblamesh.c: `xblameshmats.env*`; a TABLE material with a map gets
  `envindex/envamount`; `xblaMeshBuildEnvironment()` takes the map as the atlas
  cell (`m->envown`), no sheen copies, no dimming (added, as the screen is for a
  dark base). `xblaMeshEnvironmentVertices()` puts the lookup in s/t from the
  view-space normal through `root` (the release's lookup; the per-pixel
  reflected ray of G_ENVMAP_EXT lit the whole near side), drawn without
  G_ENVMAP_EXT; not cached per frame (split screen). `xblaMeshEnvironmentLight()`
  now lets an own map through mode 9 (every object) when the object has no
  fade/tint.
- **Destroyed GoldenEye props** (all Bean props, not only the tank): under
  objRender()'s destroyed mode (unk30 9, env alpha 100+50/level) the Bean mesh
  vanished whole - shot tanks left only smoke. Now drawn with the env alpha off
  round its own draw and its colours at a quarter (`XBLAMESH_SCORCH`), no
  reflection: the tank stays, scorched, squashed and sunk with the object, as
  the N64 look's does. objDeform's per-vertex jitter (+-10 units) is still not
  mirrored on Bean meshes (`use` is NULL for them; pre-existing).
- geproptable.h: only the Pgx117Z row added. The generator
  (`.xbla-work/ge-arena/gen_proptable.py`, EXCLUDE now empty) also emits the
  three autogun rows, which belong to fix/f3-runway-emplacement-hd /
  fix/f3-tank-aim-fire; their merge brings the table to the generator's output.

## Other props of the same family (release shaders all read)
- Same maps, other shapes: CCTV (Pgx024Z) and ICBM nose/ICBM (092/093) put the
  picture first and tint the reflection with it; desk lamp (040) and oil drum
  (062) have the tank's slots but constants that lerp the reflection to black;
  destroyed Seawolf (103) and the plane (291) differ again. None gets a
  reflection pass (one pass cannot multiply by the picture).
- Picture choice changed by the sphere-map rule: desk lamp (was the spot map
  on a 32x32 picture - drew silver, now its dark head: `lamp_compare.jpg`),
  ICBM's third material and the nose's second (were the spot map, now the
  85x324 CCCP decal; derived from the data, not framed in game). Everything
  else in the survey picks as before.

## Verification (RX 580; rig `~/wt/f3gastank-rig`, pictures `~/wt/f3gastank-pics`)
- `tanks_compare.jpg`: HD before (GoldenEye model) / HD biggest-picture trial
  (white) / HD now / N64 look, from the bottling room aisle.
- `tanks_gl_vk.jpg`: GL vs Vulkan, 98.2% identical, 31 px over 8 levels.
- `tank_shot_compare.jpg`: tank 53 before / 3 hits / exploding / 3 s later;
  rows GL, Vulkan, N64 look (`tankshot.sh TAG BIN [SAVE]`, VIEW env; objDamage
  from gdb, after build/minethrow/tank1.py).
- 20-mission HD sweep to frame 396 (`sweep2.sh`), pd.before (71ec65134 build)
  vs this: all 20 frames pixel-identical, no crash, no new WARNING/ERROR.
- Final binary = pd.after4 plus comment edits (tank frame 100% identical).

## Open
- The release's own look (pale grey) is not what this draws; GoldenEye's shade
  is a deliberate fix (bean-release-incomplete memory). If the user wants the
  release as it is, drop the `beanStockShade()` call.
- objDeform's vertex jitter on Bean props; the CCTV/ICBM/plane reflections.

## Update: the tank is gray (user, same day)
The user: GoldenEye's gas tanks are gray - the black is only GoldenEye's baked
vertex shading. `beanStockShade()` and its stock-triangle walk
(`beanStockTriangles/Walk/Tri`, `struct beanstock`) are removed; the HD tank
is the release's own picture (pale grey, pool of light at the top) times
Bean's white vertex colours, plus the spot x landscape rim reflection.
Everything else above stays (reflection pass, scorched destroyed props, the
sphere-map picture rule). So the "Open" item about the release's look is
settled, and the bullet on `beanStockShade()` above is history.
- Pictures regenerated with the gray build (`pd.gray` in the rig):
  `tanks_compare.jpg` (HD before / white trial / HD now / N64),
  `tanks_gl_vk.jpg` (GL vs Vulkan 97.9% identical, 31 px over 8 levels),
  `tank_shot_compare.jpg` (GL, Vulkan, N64: before / hit / exploding /
  after; GL vs Vulkan 95-96% identical, under 50 px over 8 levels each).
- The 20-mission sweep was not rerun: the removed code ran only for draws with
  a reflection amount (the tank), and no sweep frame shows the tank.
- The reflection is added, not screened; on the gray picture the difference
  (picture x R) brightens the rim slightly over the release's. A rig trap
  fixed: two screenshots in one second are named `X-2.png` and `X.png`, and
  `-2` sorts first, so tankshot.sh now collects by time (`ls -tr`).

---

<!-- section: fix/f3-runway-emplacement-hd -->
# HD autogun models (2026-09-26), branch fix/f3-runway-emplacement-hd

Follow-up from the tank pass (fix/f3-tank-aim-fire, its "Open": Pgx292Z has no
HD model row). Nothing merged, pushed or deployed.

**Merge after fix/f3-tank-aim-fire.** The rows need c80118821 ("HD tank's
barrel turns with its turret", the second binding pass for props) to pose the
guns; without it they draw wrong (see below). Not duplicated here.

## The item

Runway's three heavy gun emplacements (00 Agent "destroy heavy gun
emplacements"; GoldenEye autoguns, model 292 `gun_runway1`) drew GoldenEye's
N64 model in the HD look although Bean has `new/prop/gunrunway1`.

- Cause: GoldenEye has three autogun models - 98 `roofgun` (ceiling drone
  gun), 292 `gun_runway1`, 299 `groundgun` (free-standing drone gun) - and
  none had a row in port/src/geproptable.h. They were never fitted when the
  table was made (2a094ac15); the prop texture pass (fix/f3-hd-prop-textures,
  71ec65134) fitted them and excluded them on purpose, because the rigid build
  put their moving parts on the wrong matrices - true without c80118821.
- Fix: the three rows, exactly as `.xbla-work/ge-arena/gen_proptable.py`
  writes them from propfit.json (fits: score 1.00, axes unchanged, scale 2.0).
  The generator stays the source of truth: its EXCLUDE list is now `{117}`
  (the gas tank only), with a docstring note on the c80118821 dependency
  (the generator lives outside git; backup of the old one in the session
  scratchpad `emplace/gen_proptable.before.py`). Regenerating on top of
  71ec65134's 180-row table gives 183 rows whose three new lines are
  byte-identical to this branch's. If the table merge conflicts, regenerate.
- How the bones land with c80118821 (in-game dump, `bones.py` / `bonecount.py`):
  - gunrunway1: rigid on matrix 1 (the turret, part 1 - the whole N64 body is
    under it too); 271 vertices on bone 1 (body), 76 on bone 3 (barrel) ->
    part 3, 0.5 units from the part's place; bones 2/4 (pitch pivot) carry no
    vertices. The barrel pitches with part 2's matrix and spins with part 3's.
  - groundgun: matrix 0 (the tripod); bones 1-6 -> parts 1, 2, 3, 5 (yaw,
    pitch, both barrels); 122 of 206 vertices on moving parts.
  - roofgun: matrix 0 (the ceiling mount); 95 of 110 vertices on 5 bones.
  - Without c80118821 (this branch alone, checked): gunrunway1 rides the yaw
    with no pitch; groundgun draws static, its gun parts on the tripod.

## Other autoguns covered

Every GoldenEye autogun uses one of the three models (listing of every
OBJTYPE_AUTOGUN on the 8 stages that load them, `ag.py`):

| model | stages (our id, GE key) |
|---|---|
| 292 gun_runway1 | 0x5e Runway (3) |
| 299 groundgun | 0x62 Jungle (7), 0x67 Egyptian (4) |
| 98 roofgun | 0x64 Bunker 2 (3), 0x66 Caverns (2), 0x67 Egyptian (3), 0x68 Cradle (2), 0x6d Depot (1), 0x6e Control (2), 0x70 Aztec (6) |

`dest_gun` (101, Frigate) already had a row and is one static part.

## Verification

Rig outside the tree: run dir `~/wt/f3emplace-run` (`run.sh TAG [probe]`,
env SAVE/STAGE/VIEWS/AT/PIN/KILL/BIN; `save_base` N64 look, `save_hdmesh`
HD look with XblaMeshes=1), probes `~/wt/f3emplace-rig`: `ag.py` (lists
autoguns; views as `x,y,z,theta,verta` or `gunN,dist,angle,dy`; PIN
`prop,yrot,xrot;...` per shot pins a gun's angles; KILL destroys autoguns and
prints objectives), `bones.py`, `bonecount.py`, `sheet.py`. Test binary = this
branch + cherry-picked c80118821 and f5bb7a5ef (autogunGeEye), not committed.

- Runway HD, at the tank pass's views: the HD gun idle, turning on Bond and
  firing (muzzle flash from its gunfire node), same yaw/pitch state as the
  tank binary without the rows.
- Pinned angles (yaw 1.4/2.1 x pitch +-0.35) N64 vs HD: barrel and plate pose
  alike in every picture. Same for groundgun (Jungle, prop 415, four poses)
  and roofgun (Egyptian, prop 11, four poses).
- 00 Agent (`-hard2`), HD and N64: objDamage on the three guns completes
  "destroy heavy gun emplacements" (objective 1); the gun is gone, the recess
  behind it shows, in both looks.
- N64 look: pinned Runway pictures pixel-identical with and without the rows.
- Not done: the release in Xenia (the Bean build is not set up to be driven to
  Runway headlessly); the fit against Bean's own N64-look copy scores 100%.

## Open

- In HD the Runway gun's armour plate stands proud of the bunker face rather
  than inside its recess (the HD level's recess is visible once the gun is
  destroyed). Placement is Bean's own relative to the model (100% fit); not
  compared with the release.
- Autoguns tick from level start in the HD look (every room drawn), so their
  yaw at a given frame differs between looks; a teleported Bond just outside a
  gun's 70-degree wake cone is not seen (Jungle prop 415). Same with and
  without these rows; GoldenEye behaviour not checked.

---

<!-- section: fix/f3-tank-aim-fire -->
# F3 GE Plus tank pass (2026-09-26), branch fix/f3-tank-aim-fire

Reports: /home/sdg/wt/f3-0926/ (tester savantique, Windows 718d5dc, GE Plus
Runway; tester stage 0x75 = our 0x5e). All three fixed; nothing merged,
pushed or deployed.

Rig (outside the tree): run dir `/home/sdg/wt/f3tank2-run` (`run.sh TAG
[probe.py]`, env SAVE / STAGE / TMO / BIN / EXTRA; saves `data/save_base` =
N64 look, `data/save_hdmesh` = HD look with XBLA meshes on), probes in
`/home/sdg/wt/f3tank2-rig`:
- `tankaim.py` - climbs into Runway's tank and turns the view 0/+-40/+-90
  against the hull, a shot each (FIRE=n fires too)
- `tankfire.py` - fires shells (GAP ticks between tries, N, NOGATE=1 skips the
  fire-rate gate, TURN/VERTA aim, SHOTS=1 pictures) and logs every
  explosionCreate with its distance from the tank
- `bones.py` - dumps new/prop/tank's Bean bones against the N64 tank's parts
- `agview.py` (VIEWS='x,y,z,theta,verta,room;...', GUNYAW/GUNPITCH pin gun 46),
  `agfire.py` (VIEW=..., logs autogun state and every beam), `autoguns.py`
- GoldenEye oracle: `gerunwaygun.py` in the session scratchpad, run on
  10.8.0.3 from ~/claude-007/007 as in ge-plus-dam-oracle (dam.padscript,
  level swapped to LEVELID_RUNWAY). Runway offset ours = GE + (-11259, 441, 14441).

## 20260925-230256 "tank cannon direction is inconsistent with aiming position"

- Status: fixed.
- Cause: HD look only. gebeanBuildRigid() binds a Bean bone to a part of the
  N64 model only when the part hangs straight off the first list's matrix, and
  compares the bone with the node's *own* offset. The tank's barrel (part 3)
  hangs off the turret (part 1), so Bean's barrel bone (158 vertices) stayed
  on the hull's matrix: the HD turret turned with the view, the barrel kept
  pointing along the hull. The shell always flew at the crosshair; N64 look
  was right.
- Fix: gebean.c, a second pass for props: an unbound bone within 100 units of
  a nested position node's place in the model (its offset plus its parent
  parts') goes on that node's matrix. Parts under a switch/LOD/reorder/headspot
  are refused (their matrices are only built while chosen). Guns excluded.
- Verification: tankaim.py HD before/after (barrel now follows at 0/+-40/+-90
  and pitches); N64 look unchanged; A/B sweep of all 20 GE Plus stages in the
  HD look (see commit message for the result).

## 20260925-230611 fire rate / shells exploding in earlier blasts

- Status: fixed.
- Cause 1: TANK_SHELL_GAP was 60 ticks. GoldenEye's tank_stats: magazine 1,
  RecoilSpeed 0x780078FF, so the hand's RECOIL1 state holds 120 ticks
  (gunfire.c), then the empty-magazine reload swaps for 17 (the hand is hidden,
  so no lower/raise): 137 ticks, ~2.3 s.
- Cause 2: the shell is Perfect Dark's rocket, and objDamage() sets off a
  rocket that takes damage; an explosion damages what is in it every frame
  (sustained damage), so each new shell went off in the previous blast, ~230
  units nearer each time. GoldenEye damages what is in a blast only every
  quarter of its life (explosion.c unk3CA).
- Fix: getank.c, TANK_SHELL_GAP = TICKS(120 + 17); each shell gets
  OBJFLAG2_IMMUNETOEXPLOSIONS (still set off by gunfire and by what it hits).
- Verification: tankfire.py on the old binary, 60-tick gap: blasts at 4464,
  4238, 3996, ... 1718 (the walk back). New binary: every blast at 4464, even
  with the gate bypassed at a 30-tick gap; the fastest trigger mashing fires at
  210, 347, 484 (137 apart).

## 20260925-230637 "nonexistent turret"

- Status: fixed (as a GoldenEye mismatch; see Open).
- What it is: the crosshair is on one of Runway's three heavy gun
  emplacements (GoldenEye autoguns, model 292, bound pads 3-5; 00 Agent's
  "destroy the heavy gun emplacements"). They are in GoldenEye; the oracle
  shows the first one in its bunker recess, turned on Bond and firing.
- Cause: GoldenEye places an object with flags2 & 1 (every Runway autogun, and
  no other GoldenEye autogun) with prop->pos on the pad and only the model
  (runtime_pos) out at the pad's box; its sight line and a muzzle-less shot
  start at prop->pos. Perfect Dark has one position, the model's (identical to
  GoldenEye's runtime_pos, checked), which is inside the recess: every sight
  line hit the bunker, the gun never woke, and it sat with its armour plate
  out and barrels in the wall - a turret that is not there.
- Fix: propobj.c autogunGeEye(): on a remake stage, such an autogun sees (the
  cdTestLos05 in autogunTick) and falls back to shooting from its pad. N64
  build untouched.
- Verification: agfire.py at the report's spot: before, lastseebond60 -1 and
  no beam in 180 frames; after, the gun turns to yaw 1.79 (GoldenEye's
  1.79-2.03 at the same spot) and fires from its muzzle, pictures matching the
  oracle's in both looks.

## Open

- "nonexistent turret" read as "a turret that does nothing/shows nothing";
  if the tester meant GoldenEye has no turret there, it does (oracle frames).
- Guns 2 and 3 (on the cliffs, pads 4/5) now wake the same way; not looked at
  from close range.
- Pgx292Z has no HD model row (new/prop/gunrunway1 exists in Bean); the N64
  model is drawn in the HD look.
- The shell still spawns ahead of the hull's middle, not at GoldenEye's muzzle
  node; it leaves the barrel's end closely enough at every turret angle tried.

---

<!-- section: fix/f3-ge-mines -->
# F3 GoldenEye mines pass (2026-09-26), branch fix/f3-ge-mines

Report: /home/sdg/wt/f3-0926/20260925-230009-c0cd3262 (savantique, Windows, 718d5dc):
"odd bug that treats held mines as though they're being wielded akimbo. ability to
see function change (detonate) doesn't yet exist." Facility mission (tester 0x7a = our
0x63), bottling room, HD look, remote mine in the hand. User's call: mines work like
GoldenEye.

## What GoldenEye does (decomp /home/sdg/perfect-dark/007, oracle on 10.8.0.3)

- Held: one hand, nothing drawn (WEAPONSTATBITFLAG_HIDE_FIRST_PERSON_HAND), one ammo
  icon bottom right. Oracle: ~/dam-oracle/getrigger.py on 10.8.0.3, frames in
  /home/sdg/wt/f3mines-pics/oracle/ (sheet.png: remote mine held, then the detonator).
- Detonating has two ways, no "function change":
  - A+B together with ITEM_REMOTEMINE in the hand (bondview2.c:5045 and 5310,
    moveData.detonating -> trigger_remote_mine_detonation(), propobj.c:11517, plays
    WATCH_DETONATE_MINE_SFX 243). Perfect Dark kept this whole (bondmove.c, both
    control styles; PC: use + reload/weapon-back/radial).
  - ITEM_TRIGGER, "Detonator": a separate item given with the remote mines
    (propobj.c:10163 add_ammo_to_inventory, :10549 propPickupByPlayer), in the weapon
    cycle right after the remote mine (bondinvCycleForward takes items < ITEM_BOMBCASE),
    auto-selected when the last remote mine is thrown and the trigger released
    (gun.c:1116 autoadvance_on_deplete_all_ammo), drawn as Bond's two hands at the
    watch (gun.c:918, gunfire.c:474/526), and its trigger detonates
    (chrprop.c:1454 chraiCheckUseHeldItem).
- Fuse: thrown mines count THROWN_ITEM_TIMER_SOLO 300 / _MULTI 180 sixtieths (gun.c:2036,
  NTSC): the timed mine goes off, the proximity mine arms (then 250 units from the
  player or a guard, propobj.c:3517, 11628), the remote mine arms (a detonation sets
  it off at once anyway).
- No mine or the grenade is ever a pair (no CAN_DUAL_WIELD).
- Auto-advance after the last timed/prox mine goes to the next weapon with HAS_AMMO
  (the mines themselves lack it) - PD's bgun0f0a1a10() is the same test.

## Cause

GoldenEye's remote mine is a copy of Perfect Dark's (host WEAPON_REMOTEMINE), which holds
the detonator in the LEFT hand (WEAPONFLAG2_DETONATORHAND -> gunctrl.dualwielding):
the left hand was in use with the mine's number, so gehud.c drew the mine icon in both
bottom corners (the "akimbo"), and the detonate was PD's second function (B+Z), which
GoldenEye's HUD never names and which is saved per host (a player who left PD's remote
mine on "Detonate" could not throw GoldenEye's).

## Fix (commit on fix/f3-ge-mines)

- port/src/geguns.c gegunsOwnThrown() (at init and after a GE-X borrow): grenade + 3 mines
  keep WEAPONFLAG_THROWABLE (65607dda1 had cleared it with the hunting knife's), lose
  WEAPONFLAG_DUALWIELD and WEAPONFLAG2_DETONATORHAND; the remote mine loses its second
  function. New WEAPON_GE_DETONATOR = 0x7f (the LAST s8 weapon number), host Data
  Uplink, one function = a copy of PD's remote mine detonate (HANDATTACKTYPE_DETONATE),
  no ammo, undroppable. gegunsNeverPairs(), gegunsThrownFuse60().
- src/game/inv.c invGiveSingleWeapon(): giving WEAPON_GE_REMOTEMINE gives the detonator.
- src/game/bondgun.c bgunAutoSwitchWeapon(): out of GE remote mines -> the detonator;
  out of timed/prox -> next in cycle with ammo, else back (GE's autoadvance).
  bgunCreateThrownProjectile2(): GE mines' fuse 300/180.
- src/game/propobj.c playerActivateRemoteMineDetonator(): GoldenEye's watch beep (243)
  on a converted level.
- src/game/modoptions.c modCanAkimbo(): false for gegunsNeverPairs() weapons (since
  097d9a2c5 the detonator only - see "Akimbo pairs" below).
- port/src/gegadgets.c: the detonator is a gadget row (item 30, "Detonator"); draws
  GoldenEye's own model when the conversion has Igx030Z, else nothing (never the Data
  Uplink). src/include/constants.h: INV_CYCLEABLE includes the detonator.

## Verified (rig /home/sdg/wt/f3mines-run, probe probes/mines.py via go.sh; before-binary rig /home/sdg/wt/f3mines-before)

Facility mission 0x63, tester's spot (3034,-373,-3455 room 66):
- Before: right 0x76 + left 0x76 in use, dualwielding 1, two icons (reproduces the F3).
  After: left hand not in use, dual 0, one icon, fn1 none. N64 and HD look (save_hd:
  XblaGoldenEye=1 + XblaMeshes=1; HD draws the release's mine in the right hand as before).
- Throw 5 remote mines -> the 5th throw auto-selects 0x7f; its trigger detonates all.
- A+B (injected into osContGetReadData) with the remote mine in hand detonates.
- Timed mine explodes ~312 frames after the throw (300 fuse + throw); prox arms at 300
  and goes off when a moving guard is 120 away. Thrown mines rest on the floor (y -370
  over -372): d2cebc2e5's NaN-muzzle fix holds.
- After the last timed mine: back to the PP7 (GoldenEye's HAS_AMMO skip of the mines).
- Akimbo on: modCanAkimbo / weaponHasFlag(DUALWIELD) 0 for 0x73-0x76, 0x7f.
- PD's own remote mine (0x22) unchanged (detonator hand, B+Z); a stock PD stage (0x09)
  runs 600 frames. gegunsDump diff: only the intended flag/function changes + 0x7f.
Pictures: /home/sdg/wt/f3mines-pics/final/ (before_*, f_* N64, h_* HD, *_sheet.png).

## Open

- **Converter (not mine to change): the detonator has no model.** Converting item 30
  (GtriggerZ) fails the whole conversion with "GtriggerZ: node type 0xf" (tried in a
  scratch build with geconvert.c's `item > 29` made `item > 30`, not committed). Once
  geconvert.c reads node 0xf and writes files/Igx030Z, gegadgets.c draws it; its
  placement row in g_Hands ({4,-12,-30}, width 20) is a guess to check then. Until
  then the detonator shows an empty hand and no ammo icon.
- ~~Pair rule on fix/f3-ge-mission-logic~~: superseded, see "Akimbo pairs" below.
- HD look draws the release's mine model in the hand (all three), GoldenEye draws
  nothing; left as it was (user's call).
- Mines thrown into a burning tank area go off early - also on the old binary.
- Simulants never use the detonator (stock bots never detonate remote mines either).

## Akimbo pairs the grenade and mines (user, 2026-09-26) - 097d9a2c5

The user's house rule: with Akimbo on, a second of any weapon held makes a pair,
GoldenEye's grenade and mines included; only the detonator stays single.
gegunsNeverPairs() is now `weaponnum == WEAPON_GE_DETONATOR`; the grenade/mine
definitions still have no WEAPONFLAG_DUALWIELD (Akimbo off: never a pair, as GoldenEye).
Consequence: with Akimbo on, weaponHasFlag(DUALWIELD) and modCanAkimbo() now say yes for
0x73-0x76 (Start Armed with a mine would hand two). The pickup rule itself is on
fix/f3-ge-mission-logic (inv.c invAkimboPairsPickup(), HANDOFF section 7 there), tested
in the scratch merge scratch/pair-mines (~/wt/pairmines, not for merging).
Throwing a pair (Facility, this rig, probes/mines.py ops pair:W / qty:W + a throw log):
PD's dual throw alternates hands, one ammo per throw; the remote-mine pair's last throw
goes to the (single) detonator, which sets them all off; timed pair 300-frame fuse;
grenade pair alternates and explodes. No crash or stuck state.
Merge note: both branches add HANDOFF-f3.md (add/add conflict) - keep both texts.

## Detonator model (converter 77 on fix/f3-ge-mission-logic) - 29fb8c20e

Converter 77 (1e37e3ff2, fix/f3-ge-mission-logic, which owns the converter) leaves
GoldenEye's interlink node 0x0f out and writes Igx030Z. The draw is here, because
WEAPON_GE_DETONATOR and its g_Hands row live on this branch; the two branches touch
disjoint files (except HANDOFF: keep both texts). gegadgets.c's new `press` field sits
after `def` so it does not conflict with mission-logic's removal of `lastweapon`.
- Pose: a g_Hands row with width 0 = GoldenEye's own pose, not measured and fitted: model
  at its own size under the host root's rotation (sway) times gunfire.c's ITEM_TRIGGER
  turn (D_80035C70), root at trigger_stats PosX/Y/Z (-2, -21.5, -19) plus the host's
  motion. Matches the oracle frame (trigger_060) to a few pixels at 4:3.
- Press: switch 6 turns about the interlink axis (20.21, 32.67, -18.41), -5 degrees at
  rest, 0 while the trigger is held (GE's field_A84 rates). Cuffs 29-34: one worn, chosen
  as the watch arm's (geWatchCuff(), new in gewatch.c).
- Crash found on the way (fixed, src/lib/modelasm_c.c): with no animation,
  modelasm00018680() read a position node's flags from sp00[part], cleared only for
  i < nummatrices; GE's parts run past that (pressing hand part 4 of 4 matrices), so stack
  garbage sent it to anim->animscale with anim NULL (SIGSEGV, depended on the build's stack).
  Now 0 in the port. May be what gave the older "grenade/mines garbage matrices" note.
- Verified (scratch/pair-mines = both heads merged, fa42d3b2e; ~/wt/f3mines-run,
  go43.sh = 1024x768, probes/mines.py + new op hold:N): Facility 0x63, 5 remote mines
  thrown -> 6th trigger is the detonator -> all 5 explode; the detonator drawn N64 and HD
  look, rest vs press differ in the right hand only; no SIGSEGV. Pictures
  ~/wt/f3mines-pics/detonator/ (oracle_n64_hd.png, press_cmp.png).
- 20-mission 1800-frame sweep of that merge after a forced reconvert to 77: all clean.

---

<!-- section: fix/f3-ge-mission-logic -->
# F3 pass 2026-09-26 (GE mission logic) - handoff

Branch `fix/f3-ge-mission-logic` (worktree /home/sdg/wt/f3gemission), based on
dabs-mod 5ac4c3176. Not merged, not pushed. **Converter now 78** (see 8, 9; was 76 here) (was 71; 72 to 76
are this branch's, one real change each). All 20 missions boot and run
1800 frames clean at converter 76 (~/wt/f3gemission-run/sweep1800.sh, sweep.out).

Rig: `~/wt/f3gemission-run` (copy of ~/wt/f3cradle-run; `run.sh TAG PROBE` with
STAGE/TMO/EXTRA env, `run2.sh` takes SAVE=save_hdbase for the HD look,
added-content/goldeneye -> Bean). Stage ids in this rig: Bunker 1 = 0x6f,
Facility 0x63, Silo 0x6b, Cradle 0x68, Surface 0x69 (the tester's differ).
Probes in `~/wt/f3gemission-run/probes/`: keyrun.py (gadgetprobe2.py + HUD
trace; gadgetprobe2 adds `hold:N` = trigger held N ticks), face.py (teleport in
front of a chr, watch act/alert), ouru.py (Ourumov), dumplist.py (LISTS=0x414
dumps a converted AI list with command lengths), cctv.py, idle.py (CHRS= act/anim/pos every STEP; TELE= puts Bond by the first), place.py (Bond at PX/PY/PZ/ROOM/THETA, logs CHRS + shots), patcount.py (chr actions at AT).

## 1. Bunker GoldenEye key (231502, 231553) - FIXED + verified

- 57998ca25 (converter 72): converted weapon records left `dualweaponnum` 0
  (wrote 0xff over 0x5d/0x5e instead of 0x61), so every converted floor pickup
  was taken for half a pair: non-dual guns "not given" -> only the ammo message
  ("Picked up an ."), dual-wieldable ones given as a pair with WEAPON_NONE.
  Game-wide, all missions.
- 57998ca25: key analyser now works on the **trigger** (GE gunTickHandState:
  TRIGGER_PRESS -> USE_ITEM -> analyzeGEKey), not on equip; empties left hand.
- 26e458cd7: objective B is COPY_ITEM + DEPOSIT_OBJECT(tag 4). GE throws the
  picked-up prop itself; PD's throw made a new prop and the picked-up one stayed
  a child of the player (invHasProp true forever). Tag now moves to the thrown
  prop, carried prop freed.
- Verified (probes/keyrun.py, Bunker 0x6f): "Picked up a GoldenEye Key.",
  analyser trigger -> key in hand, throw -> "Objective 1: Completed"; picking
  the key up again -> Incomplete (GE semantics).
- Open: in the probe a *second* throw after re-picking the key did not fire
  (hand already showed 0x79; likely the re-pickup does not re-equip/load it).
  Minor; check with a real trigger press.
- Player-facing: after analysing, the key is in hand and must be THROWN (fire)
  to "leave original" - same as GoldenEye.

## 2. Facility guards standing at spawn (225349, 225423) - FIXED, converter 76

The user asked for GoldenEye's idle. **GoldenEye's idle is the AI list's, not the
chr tick's**: an ACT_STAND guard only loops the stance (chraction.c's
chrlvIdleAnimationRelated(): ANIM_idle 0-120 at 0.25, ours already the same,
row 1 = GE idle); the fidgets (yawn, swat flies, scratch leg/butt, adjust
crotch, sneeze) are global list 3, GAILIST_PLAY_IDLE_ANIMATION (chraidata.c's
m_IdleAnimations), CALLed only by GAILIST_STANDARD_GUARD (0x802) - about a 2/256
chance a tick while stopped and not already animating. GAILIST_SIMPLE_GUARD
(0x807) is "no clones, no animations" by design. Measured (probes/idle.py):
Facility's 0x802 guards (chrs 0, 3, 12, 13, 16) already play them (ACT_ANIM,
GE anims 1350-1354) - nothing to do there.

**The real bug**: the two guards were never meant to stand. Facility's setup
gives chr 39 list 0x406 = `guard_start_patrol(5)` then 0x807, and chr 44 (under
the stairs) 0x401 = patrol 0; twelve Facility guards are on 0x401-0x407.
GoldenEye's StartPatrol converted to PD's **aiSetPath (0x21) only**, which just
stores the path; PD's aiStartPatrol (0x22) never followed, so **no GE Plus guard
ever patrolled** (decomp setups: ~76 StartPatrols - Facility 7, Caverns 7, Dam
7, Silo 15, Surface/Surface 2 16 each, Bunker, Runway, Aztec...).
- converter 76 (geconvert.c writeSoloAilist(), gesolo.py GE_STARTPATROL_OP):
  StartPatrol -> `0021 <path> 0022`. C and Python setups identical (ark, dam).
- chraction.c chrStartPatrol(), remake stages only (geRoomActive()): GoldenEye's
  set_actor_on_path() first step - a path pad in the guard's room within 100
  units, else step 0 (not PD's nearest / resume step). PD missions unchanged.
Verified: Facility 0x63 chrs 39/40/44/45 all ACT_PATROL from frame 1 and walk
their paths (39 out through the double doors, 44 across the stairs room, 40
upstairs), N64 and HD (save with XblaMeshes=1 - save_hdbase has it 0, so
earlier "HD" runs in this rig were N64 levels); face.py: chr 44 surprised ->
gopos -> attack, chr 39 (HD) attacks. PD stage 0x30: chr actions/positions at
frame 600 identical old vs new binary. 20-mission sweep 1800 frames clean at 76.
Open: not compared against the native GE oracle (decomp is unambiguous); PD's
line-of-sight/gopos lead-in to the first step kept (GE walks it as a patrol).

## 3. Silo

- 233940 Ourumov dies instead of fleeing: FIXED + verified, fba0c1166 (3b).
  His converted list 0x414 is faithful (armour 30 via aiAddHealth, max 20 -> 50
  to kill; flees on 0x415 when target < 500 units or health < 20; 0x415 sets
  CHRCFLAG_INVINCIBLE, runs to pad 0xc7, then aiRemoveChr). NB the GE decomp's
  setup macros print their words byte-swapped: guard_flags_set_on(0x10000000)
  is flag 0x10 (INVINCIBLE), armour 0x2c01 is 300 (30.0), list 0x1504 is 0x415.

## 3b. Scripted chrs die to one headshot (233940 Silo, 005817 + 010126 Cradle) - FIXED

Root cause: Perfect Dark's player headshot is x4 **and then x25**
(`headshotdamagescale = g_ModPlayerHeadshotScale`, default 25) in solo, co-op
and counter-op; GoldenEye's chrlvDamage is x4 only. A PP7 headshot on Agent
(tx 2) did 1 x 2 x 4 x 25 = 200: Ourumov -30 -> 170 and Trevelyan -2 -> 198,
dead in one shot, before their lists (0x414 health check; the Cradle's 0x411
"damage off" that sets INVINCIBLE) got a tick. The tester traces match that
exactly: dead chr 0 with INVINCIBLE set (0x00200b1c / 0x00280a1c) - the list
set it *after* the death. Trevelyan is not invincible while running on list
0x411 until a wound is seen (flags 0x00080a0c), so any headshot then killed him.

Fix fba0c1166 (chraction.c chrDamage()): on a converted level
(`geRoomActive()`) headshotdamagescale stays 1 - GoldenEye's own x4. Stock PD
stages keep x25.

Verified with real PP7 bullets, probes/headshot.py (teleport in front of CHR,
aim at bbox top - HOFF each frame, trigger pulses, chrDamage logged with
hitpart; WAITLIST waits for a list, DIFF sets g_Difficulty):
- Cradle 0x68 chr 0, WAITLIST=0x413 HOFF=24 DIST=200: before, hitpart 8 ->
  damage 198, ACT_DIE (hsb_trev_24.log). After, damage 6, INVINCIBLE set by his
  list, he runs on to his next stand spot, which clears it (hsa_trev.log,
  shots_hsa_trev2: spark on his head, then running out of the door).
- Silo 0x6b chr 0, AT=300 DIST=550 HOFF=13: before, damage 170, dies
  (hsb_ou_13.log). After, two headshots -30 -> -22 -> -14, he then switches
  to 0x415 and flees to pad 0xc7 and is removed (hsa_ou.log, shots_hsa_ou).
- Normal guards, one headshot on Agent: Silo chr 41 (no armour) 8 >= 4, dies;
  Cradle chr 1 (armour 2) 6 >= 4, dies.
- Behaviour change to know about: on Secret Agent / 00 Agent (tx 1) a headshot
  is 4, so a guard GoldenEye gave armour (Cradle's guards: 2) takes a second
  hit, as in GoldenEye (hsa_sa_c1.log); unarmoured guards still die at once
  (hsa_sa_g41.log). If the user wants PD's one-shot heads for plain guards,
  the scale would have to be kept for chrs whose lists never touch armour or
  invincibility - not GoldenEye's rule.

## 3c. Silo outro

- 234037 Bond does not stow weapon in outro: FIXED + verified, eba3af6e4.
  GE BondHideWeapons -> aiChrDrawWeaponInCutscene(bond, WEAPON_NONE); PD's
  switch only completes when the gun ticks, which it does not under the outro
  camera. On remake stages the hands are emptied (inuse false) and the body's
  held guns deleted at once. Shot: Bond in the lift, arms crossed, no gun
  (shots_siloend5).

## 4. Cradle ladder at the end (001445 Trevelyan up it, 005903 Bond stuck) - FIXED

No converter change (still 74). Cradle's shaft: one ladder (upright tiles 625/626,
special 3, **room 7**) from the platform floor (y 1162, room 7) to the deck round
the hatch (2609, room 8; the hatch is x -1618..-1448, z -1093..-927, walled
except the ladder's 81-wide link). GE route: TH (0x410) sprints to pad 0x96 on
the deck, 0x415 walks to pad 0x74 at the foot (waypoint link 0x71 -> 0x74 *is*
the ladder: GE's guard just drops down it, ours falls the same way - fine), 0x417
runs 0x74 -> 0x77 -> 0x76 on the floor, 0x418 fights until he is in pad 0x94's
room or dying.

Three causes, all runtime:
- **Trevelyan up the ladder**: Perfect Dark's guards take hold of any ladder
  within 2.5 radii while GOPOS/PATROL and climb it whichever way they go. On
  0x417 he ran past the ladder's foot (dodging Bond, who stood at it as in the
  tester's shot) and went back up to the deck. Fix (chr.c chrGeTakesLadder()):
  on remake stages a *newly found* ladder is taken only if the chr's current
  waypoint is > 150 over its manground (pads are ~90 over their floor); one
  taken is kept. GE guards have no ladders - they follow tile links.
- **Bond could not climb down**: the ladder is room 7's, the deck room 8's, and
  the player's ladder tests only ask the rooms he is in, so from the deck it was
  never found; walking on, he fell into the hatch and hung in its far lip
  (vv_ground 2609 over his feet, can't rise, can't move - gestan skips the lip's
  walls from the floor tile under him). Even with the room right, the drop is
  seen only once his circle is clear of the edge (> 30 out) and the take-from-top
  test reaches 33 - a 3-unit window. Fix (bondwalk.c): ladder tests ask
  near rooms too (bwalkCdRooms() -> geroom.c geRoomAddNear(), rooms whose box
  meets the body's); at the drop a ladder is looked for 2 radii out and 60
  down (collision.c cdFindLadderDist(), also returns the plane distance and
  the head's height), the player is put back to radius + 1.5 from it and let
  down to 2 under its head, where the hold is kept.
- Found on the way: Dam's three short ladders (rooms 72/75/78) have their head
  at the foot of a 34-high ramp off the deck, so walking off the deck was a
  fall with the old binary too (pd-before.x86_64, dl75b.log) - the 2026-09-20
  "five down" test must have started on the ramp. Now taken (dl75/dl72/dl71).
- **Bond stuck at the stair head (the 005903 position, -2006 2922 -1391)**: the
  stair down from the landing (room 3) to the hatch deck is room 6's; the portal
  walk left the player in room 3 while on the stair's top tread, so room 6's side
  wall (z -1411) was never asked, he slid to 13 from it, and the moment room 6
  was his he was inside its radius for good. Fix: the player's move/vertical
  collision tests (bwalkCalculateNewPosition's cdExamCylMove02,
  bwalkTryMoveUpwards, bwalkCanMoveUpwards) ask bwalkCdRooms() as well; the
  player's own room list is untouched (AI "Bond in room with pad" reads it).

Verified (rig ~/wt/f3gemission-run, Cradle 0x68):
- probes/trevend.py (TPAD=106 puts Trevelyan near the end, LIST=0x410, player
  at the tester's spot PXZ=-1516,-1019 looking up): before (te3.log) 0x417 at
  f740-910 climbs from 1184 to 2161 up the ladder; after (te4.log) he drops down
  the shaft, runs 116 119 118 on the floor, 0x418 attacks. KILLAT=1000 (te6-te8):
  dies, GE's ending camera + exit state 1 from ~f1450, still cycling at f2000;
  the level's own end was not seen (te7 timed out, te8's gdb hung after f2000 -
  the previous pass saw the ending finish by itself ~700 frames in).
- 20-mission sweep (sweep.sh) with the final binary: identical to before.
- probes/ladwalk.py (player at X/Z/Y/ROOM, PHASES of from:to:stick:theta[:side]):
  lw1 before: walked across the hatch, stuck at z -923.7 in the lip; after:
  takes the ladder at z -1061.5, down to the floor (f655), and lw3 climbs back up
  onto the deck (f1420). Backwards (lwb), slow 0.3 stick (lwc), diagonal (lwd)
  all take hold. sw2 (strafe into the stair's side wall): before stuck at
  -2010.6 -1398.3; after slides at z -1381 (a radius) and down the stair.
  Dam (STAGE=0x15): dl75 down the room 75 ladder and back up onto the deck,
  dl72 and dl71 (face ladder) down.
- Open: sliding sideways off the ladder mid-way still lets go (stock PD
  ladders allow strafing; GE would hold Bond to the ladder tile) - he falls to
  the floor. Guards' own wall tests still ask only their rooms (same class of
  bug as the stair head, not seen for guards yet).

## 5. Security cameras

- Rotate / face backwards (231132 Bunker, 233118 Surface): FIXED, 6d4f81b9a
  (converter 73). CCTV tail (look pad, yleft, yright, ymaxspeed, maxdist) was
  never converted - all nought, cameras still and aimed at pad 0. Verified by
  numbers on Bunker (probes/cctv.py: 4 cameras, pads 88/95/80/63, sweep limits
  +40/-20, 145/0, 180/0, +-30 deg, the near one sweeping). No screenshot, Surface
  not checked in game.
- One shot in the lens (231244): FIXED + verified, c813882a5 (converter 74).
  Converted CCTV models were SKEL_BASIC; PD's x100 lens rule (GE's own) only
  applies to g_SkelCctv. Skeleton carried across. probes/cctv2.py (AIMY=16/28):
  casing hit 500 of 1000 (two to kill, as GE), lens hit -> cctvHandleLensShot,
  200 damage, destroyed at once.
- Surface camera now faces out from the cabin wall (shots_surfcam).

## Side effect to know about (converter 72)

Before 72 a converted floor gun's dualweaponnum was 0, so picking up a
dual-wieldable one gave an odd pair (gun + nothing) and could put it in the
left hand. Now such a pickup is a single gun, as a guard's dropped gun always
was. The user asked for GoldenEye's own pair rule: see section 6 (GoldenEye
pairs only what its setup pairs).

## 6. GoldenEye's pairs of guns (user: yes) - DONE, converter 75

**GoldenEye has no "second of a gun makes a pair" rule** (the section above
assumed it did). Its pickup (bondinv.c's bondinvAddWeaponByProp(), propobj.c's
propPickupByPlayer()) pairs only guns its **setup** pairs:
- two collectables joined by a PROPDEF_LINK (0x0e) record (prop.c's setup ->
  propweaponSetDual()): the first picked up is a single gun, the second the pair;
- a guard holding a gun in each hand (PROPFLAG_IS_DOUBLE, propobj.c's
  chr-attach -> propweaponSetDual()): his two dropped guns are such a pair.
A second of a gun that is no one's pair gives only its ammunition ("Picked up
some ammo."), or is left lying if the ammo is full. CAN_DUAL_WIELD is never
read by the pickup (only by the all-guns cheat). Perfect Dark's solo pickup is
the same code (inv.c's invGiveWeaponsByProp(); its "second makes a pair" branch
is multiplayer only), and IS_DOUBLE is PD's OBJFLAG_WEAPON_CANMIXDUAL (flags
are copied whole), so guards' pairs already worked. What was missing:
- ed432ee88 (**converter 75**): the link record was converted as a one-word
  nothing; now OBJTYPE_LINKGUNS (s16 offsets; gesolo.py too). Two in the ROM:
  Caverns' AR33s (inside two boxes) and Bunker 2's silenced PP7s.
- ed432ee88: the pair's second gun is worded as GoldenEye does, "Picked up a
  ZMG (9mm).", not PD's "Double ZMG (9mm)." - converted missions only
  (currentPlayerQueuePickupWeaponHudmsg(), !normmplayerisrunning).

ROM census (tools/geconvert, every Usetup*Z): guard pairs Bunker 1 Klobb x1,
Archives DD44 x2 + Klobb x3, Train ZMG x2, Frigate Phantom/D5K (four guns), Bunker 2 Klobb
x3, Aztec AR33, Egyptian ZMG x14, Jungle Xenia's RC-P90 + grenade launcher (a
mixed pair), Caverns ZMG x2, Surface 2 Klobb; links as above. **No grenade or
mine is ever paired**, and nothing in the rule reads weaponHasFlag(DUALWIELD),
so Akimbo cannot make one; fix/f3-ge-mines needs nothing from this.

Verified (probes/pair.py: duals/kill/list/pickw/pickc/free/msg/print; ALL=1
walks contained props):
- Caverns 0x66: linked AR33s -> single, then pair, "Picked up an AR33 Assault
  Rifle." both times; guard 19's ZMGs -> pair, "Picked up a ZMG (9mm)." twice;
  two single guards' ZMGs (Bond starts with one) -> "Picked up some ammo.", no pair.
- Dam 0x15: two guards' KF7s -> "Picked up a KF7 Soviet." then "some ammo".
- Jungle 0x62: Xenia's RC-P90 + GL -> mixed pair (right RC-P90, left GL).
- Bunker 2 0x64: the two PP7 (silenced) on the floor are linked.
- Defection 0x30 (PD): the same message call still says "Double Falcon 2."
- Scratch merge with fix/f3-ge-mines (worktree /home/sdg/wt/pairmines, branch
  scratch/pair-mines, not for merging): Bunker 1 guard 12's Klobbs pair;
  Statue Park grenades three times -> grenade then ammo, never a pair; the
  grenade and three mines have no DUALWIELD, PP7/Klobb/KF7/AR33 do.
- 20-mission sweep clean at converter 75 (sweep.out).
Open: GoldenEye does not draw the second gun on pickup; PD puts it in the left
hand when the right already holds that gun (kept). Not checked in a real
playthrough with the fire button (probe pickups).

## 7. Akimbo house rule: a second of any weapon makes a pair (user decided) - DONE

User, 2026-09-26: with **Akimbo on** (Akimbo = Everyone or Players and Sims,
modIsAkimboForPlayers()), picking up a second of any weapon already held makes a
pair, on PD missions and GE Plus missions alike, **GoldenEye's grenade and mines
included**; the GE Detonator (WEAPON_GE_DETONATOR, fix/f3-ge-mines) stays single.
With Akimbo off, section 6 stands (GoldenEye's setup pairs only) and PD solo is stock.
(Section 6's "Akimbo cannot make one" is superseded.)

- src/game/inv.c invAkimboPairsPickup(w): solo, Akimbo for players, modCanAkimbo(w),
  not gegadgetsIsGadget(w) (detonator, key, camera, modem, plastique, tank shells: items),
  holds a single and no pair. invGiveWeaponsByProp() asks it *before* the single is given
  and gives the pair *after* the link/guard-pair bookkeeping (so a linked gun's partner
  never keeps a pointer to the freed prop).
- src/game/propobj.c propPickupByPlayer(): a full gun's second is no longer left lying
  when it would make the pair.
- fix/f3-ge-mines 097d9a2c5: gegunsNeverPairs() is now the detonator alone (modCanAkimbo
  honours it); grenade/mine definitions keep no WEAPONFLAG_DUALWIELD, so Akimbo off never
  pairs them.

**PD before** (verified with the converter-75 binary, Akimbo on, Defection 0x30): a second
grenade / CMP150 / Dragon picked up solo gave ammo only (the pickup's "second makes a pair"
branch is multiplayer only; Akimbo only paired Start Armed's gun and the switch's carried
pair). **After**: pair. Akimbo off: unchanged. Multiplayer/Combat Sim: unchanged (its branch
already paired any weapon under Akimbo from a different pad).

Verified (rig ~/wt/f3gemission-run, pd-am.x86_64 = scratch/pair-mines merge of both branches,
probes/pair.py + new ops spawn:W (weaponCreateForPlayerDrop, the MP drop), skipcut, bt;
am.sh <tag> <stage> <DO> runs Akimbo off (save_base) and on (save_akon, Akimbo=2)):
- Dam 0x15: two guards' KF7s -> off: single + ammo; on: pair. Remote/timed/prox mines and
  grenades, two each -> off: singles; on: pairs. Detonator: modCanAkimbo 0,
  invAkimboPairsPickup 0, never a pair. (spawn:0x7f crashes in projectileTick - no model,
  playermgrGetModelOfWeapon -1 - a probe artefact: the detonator is undroppable.)
- Caverns 0x66, Akimbo off: linked AR33s single then pair; guard 19's ZMGs pair. Akimbo
  on: AR33s the same (Bond starts with a ZMG pair under Akimbo).
- Defection 0x30: on -> Falcon/grenade/remote mine/full-ammo CMP150 pairs; off -> stock.
- Throwing a pair (~/wt/f3mines-run, probes/mines.py + pair/qty ops and a throw log, Facility
  0x63): PD's dual throw alternates hands (right, left, right...), one ammo per throw.
  Remote-mine pair: 4 throws, then both hands go to the detonator (single), which sets
  them all off. Timed pair: alternating throws, 300-frame fuse, the second chains off the
  first. Grenade pair: alternating, each explodes; out of grenades the hands stay on the
  empty grenade as with a single one. No crash or stuck state.
- 20-mission sweep, Akimbo off and on (sweep.am.off / sweep.am.on): all 20 clean, no fatal/crash.

## Before merging

Converter 72 and 73 touch every mission's setup: run the 20-mission sweep
(build/gexrom/runall.sh or hdsweep/ab.sh) after a forced reconvert, and the C vs
Python parity check (should stay the known 210 diffs).

## 8. GoldenEye's detonator model: node type 0xf (converter 77) - DONE

GoldenEye's model node type 0x0f is MODELNODE_OPCODE_INTERLINK
(bondtypes.h ModelRoData_InterlinkageRecord: pos, pos2, scale). It draws nothing; GE
reads it only as a prop's depth sort (objecthandler.c) and, in the detonator, as switch
28 = the hinge axis switch 6 (the right hand) turns about to press the watch
(gunfire.c:829). Perfect Dark has no such node (filemodel.c maps 0x0f to -1), so
modelWalk() leaves it out like the 0x0d shadow; the prev-pointer check now accepts a
next that is a dropped 0x0d/0x0f. The draw side keeps the axis (fix/f3-ge-mines,
gegadgets.c).
- Scan of every GE prop, character and hand item in the ROM: only GtriggerZ (item 30)
  and GwatchlaserZ (item 23, the same file) have a 0x0f, each one childless leaf at the
  end of its chain. Nothing was silently skipped: an unknown node fails the whole
  conversion, and item 23 was (and stays) excluded as no gun of the port's.
- The item loop takes item 30 (`item > 30 && !soloGadgetItem`): files/Igx030Z.
- Python twin (gechr.py, gemodelconv.py) drops 0x0f too; it converts no hand items.
- Verified: standalone C 76 vs 77 output differs only by the new Igx030Z; Python 76 vs 77
  identical; C vs Python = the known 210 diffs + Igx030Z (C only). Draw/probe/sweep: see
  fix/f3-ge-mines HANDOFF "Detonator model". 20-mission 1800-frame sweep after a forced
  reconvert to 77 (~/wt/f3gemission-run/sweep77.sh, sweep.out.pair77): all clean.

## 9. Facility: conveyor escape + gas cloud (F3 20260925-235430, user: like GoldenEye) - DONE, converter 78

Tester (HD look, standing at the belt): "conveyer belt isn't accessible as an escape
route, cannisters that have been destroyed do not cloud the area with a visible poison mist."

**Conveyor (2f1c8a013, no converter change).** The ending is setup ai_47: Bond in the
room of pad 0x135 (309, room 70, the conveyor tunnel) or 0x87. GoldenEye has no
moving belt; its belt tiles (-266, rim -253) are joined to the bottling-room floor (-372)
by upright tiles, i.e. *links*, and bondviewTryMoveToStan() walks Bond into them and
lifts him (only refusal: stanTestLocusEdgeAboveY(), an edge > eye + 175). The tunnel
tiles are special 1 (force crouch). Ours: the conversion's climb wall on that link
(climb 119 > WALL_CLIMB 60) plus the upright tile's own geometry (flags 0x1b) stopped
the player 3.8 short of the rim - PD steps up nothing over manground + 30.
- gestan.c geStanClimbFloor(): highest floor of the tiles flood-linked to the one
  underfoot within the radius that the target circle touches, only for a move *into*
  it (nearer by >= half the move; brushing along the belt's side lifts nothing).
- bondwalk.c bwalk0f0c63bc(), remake stages only, not on a ladder/falling/in the tank:
  floor > manground + 30 and <= eye + 175 and bwalkTryMoveUpwards() clear -> manground,
  ground and sumground set to it (a snap; GoldenEye eases, PD's box cannot).
- Verified (probes/realwalk.py, convwalk.py; OBST=1 prints the blocking geo): head-on
  and diagonal walks lift at the rim, walk the belt, crouch in the tunnel, room 70 ->
  aiEndLevel (objectives incomplete: mission failed screen); with ALLDONE=1 (objective
  check forced true) Bond's own outro plays on the belt, then aiEndLevel - N64 and HD
  (save_hdmesh). Walk parallel along the belt side: ground stays -372.
- **Behaviour change to know about:** this is GoldenEye's rule everywhere on converted
  levels - any linked ledge/sill/deck up to eye + 175 is climbable by walking into it,
  including Dam's tower decks 317 over the treads (the climb walls stay for everyone
  else). Not walked on Dam in this rig (realwalk on 0x15 did not move at all, before
  or after - the mission start; use build/gexrom's stair scripts).

**Gas (df8980eae, converter 78).** GoldenEye: a GASBOTTLE reaching destroyed level 1
calls init_trigger_toxic_gas_effect(); handle_gas_damage() fades the fog with
fogSwitchToSolosky2(timer / 3600) toward g_EnvironmentAltp (the fog row after the
level's: Facility id+100 = far 5000 -> 1000, colour 0x102010 -> 0x408040), coughs from
600 ticks, 0.125 damage every 225 ticks from 1800, Bond only (guards untouched). PD
kept all of it (gasReleaseFromPos/gasTick), but a mod stage's transition was fog -> same
fog, so nothing was visible (the cough and damage did happen).
- Converters write the level's +100 row as the mission's ` altfog "..."` (C
  romFogAltRow(), Python gerom fog_alt_rows(); Train, Facility, Aztec, Egypt; maps
  block unchanged). C vs Python lines identical for ark and cryp.
- modloader.c parses `altfog` (missions block), modloaderGetStageFogAlt(); env.c uses
  it as g_EnvTransitionTo; envGetTransition() exposes the fraction; gebeanstage.c's
  HD fog shrinks its end by the same far ratio and lerps to the alt colour.
- Verified (probes/gasprobe.py: objDamage on the 10 tanks, logs timer/frac/sky/health;
  TIMER= jumps the clock): frac 0 -> 1 over 3600 ticks, sky 102010 -> 408040, health
  1.0 -> 0.84 by 4000 frames (first hit at ~1860), N64 and HD screenshots: the room past
  ~1000 is solid green, near walls tinted (HD strongly).
- Forced reconvert to 78 + 20-mission 1800-frame sweep: all clean (sweep.out).
- Open: Egypt's gas_leak_and_fade_fog (0xfb, fog only) is still dropped by the
  converter (geaitable.py None) - its alt row now converts, so mapping it to a port
  command that calls gasReleaseFromPos() with no damage would finish it. PD's
  "visual only" check is on STAGE_MP_G5BUILDING, not a converted Egypt. Not compared
  with the native GE oracle (decomp is unambiguous for both).

---

<!-- section: 1fc1832d8 (fix/f3-hd-level-render, fix/f3-dam-guard-aim2) -->
# F3 HD level rendering pass (2026-09-26), branch fix/f3-hd-level-render

Reports: /home/sdg/wt/f3-0926/. Tester stage ids differ from ours: tester 0x86 = our
0x6f (Bunker, GE `sev`), 0x7a = 0x63 (Facility), 0x82 = 0x6b (Silo), 0x75 = 0x5e
(Runway), 0x15 = 0x15 (Dam). All are GE Plus solo missions, HD look.

Rig (all outside the tree):
- run dir `/home/sdg/wt/f3hdlevels-run` (own copies of cache + mods; saves
  `data/save_sw_hd`, `data/save_sw_n64`; the ROM is a symlink)
- `/home/sdg/wt/f3hdlevels-rig/views.sh STAGE TAG` with `VIEWS='x,y,z,theta,verta,room;...'`,
  `GROUND=` (manground; set it or the player lands on the wrong storey),
  `CE=1`, `N64=1`, `PRECMD="gdb cmd;;gdb cmd"` (before the shot), `SHOTS2="cmd;;cmd"`
  (one extra shot per command, each after its command). Pictures go to
  `/home/sdg/wt/f3hdlevels-pics/`.
- `ab.sh BIN TAG stages...` (HD look, frame 400) + `abdiff.py` (ab-base vs ab-fix1).
- `ray.py` / `near.py`: ray-cast / list Bean level triangles from a dump. The dump
  needs a temporary hunk in build() (not committed): write `c.num` then every
  `struct stri` (80 bytes) to `$GEBEAN_DUMP` just before the `lists[r] = ...` loop.
  Dumps: `tris_0x6f.bin` (Bunker), `tris_0x15.bin` (Dam). `pd.dump` = binary with it.
- `gate.py`, `gate2.py`, `gate3.py`: Dam opening (no --skip-mission-intro); the gate
  shot is frame ~925 with --fixed-step --rng-seed 1.
- `lamp.py`: sets Facility's door console (singlemonitor at -3533 -236 1521) to
  GoldenEye programme 46.

## 1. Bunker z-fighting / "inconsistent wall" (231104, 231159, 231244) - FIXED + VERIFIED

Commit f99bb4e79. Cause: markDecals() picked the smaller of an overlapping coplanar
pair as the decal. Bunker's hammer-and-sickle plaques (tex 31) and vent grilles
overlap a wall panel (tex 4) and hang past it; the panel's half-quad was smaller, so
it became the decal, its other half did not (fought the plaque), and the part of the
panel off the plaque, drawn in the no-depth-write decal mode, was painted over by
the back-facing rock of room 14 drawn later (the brown triangles). Fix: a triangle
lying wholly on other pictures (middle, pulled-in corners and edge middles) is the
decal of a pair where the other is not; solid decals also write depth (Z_UPD).
Verified at all three report cameras (pics bunker-cmp-0..2.png: plaques + vent grille
whole, holes gone). HD A/B sweep of all 20 missions (frame 400): 18 identical, Train
0x60 (a dashed seam line on a wall gone) and Archives 0x65 (seam at an emblem's edge)
changed, both for the better (abdiff-0x60.png, abdiff-0x65.png). N64 look not swept:
gebeanstage.c serves rooms only in the HD look.

## 2. Silo monitors z-fighting, spiral screens (233641) - ALREADY FIXED (a97daeb6e)

Tester's 718d5dc predates a97daeb6e (console2/console3 placeholder spiral quads
dropped). At the report camera on 5ac4c3176+ the four screens show programmes, no
spiral, three frames identical (silo-base-*.png).

## 3. Facility door console red square (225534) - NOT REPRODUCED, open

The streaks at the top of the red lamp are Bean's placeholder spiral (doorconsole
draw 10, vb 0x300, tex 5, 2 triangles at x 78..91, y 158..170) peeking above the
programme's quad. At the report camera, frame ~400 and with the console forced to
programme 46 (lamp.py) the spiral never shows (fac-lamp-crops.png): the node is
handed back (xblaMeshNodeIsLiveScreen()). Not found what frame state lets both draw
on the tester's frame 6534 (door opened/used by then). Next: drive the console's
door (AI sets 46/47 on activation) and watch xblaMeshNodeIsLiveScreen() for the
console; or drop doorconsole's spiral quad the way a97daeb6e drops console2/3's
(beanVertexDrops in gebean.c, vertices of vb 0x300) - but only if the hole stays
covered when no programme list is written.

## 4. Facility windows solid blue at distance (225628) - NEEDS USER DECISION

GoldenEye's own tinted-glass fade (glassCalculateOpacity(), the same function in the
007 decomp): past opadist a pane is drawn opaque in its tint. The Glass See-Through
slider (bdc053f3d, 5ac4c3176; default off, Dab's Settings 50) is newer than the
tester's 718d5dc. With 50 the far pane is lighter (fac-glass-cmp.png) but still reads
as blue glass. Question for the user: should GE Plus HD windows stay clear at any
distance by default (as the release may), or is the slider enough?

## 5. Runway door to Facility: sky in gaps round it (230105) - FIXED on fix/f3-runway-door-gaps (see the section of that name below)

At the report camera on current code the double door (modelnum 0x29b, two props at
(-3684 89 9467) and (-3583 89 9467)) shows sky along its top and right edges
(runway-door-0.png): nothing is behind it and the HD door mesh does not fill the
frame. HD only: in the N64 look the same camera shows one plain door filling the
doorway with no sky (runway-door-n64-0.png). Next: compare the Bean door mesh's bbox with the N64 model's (setup scales doors to
their pad bbox; if the HD mesh's extents differ from the N64 model's, scale the HD
mesh to the N64 bbox).

## 6. Dam opening gate is a blue void (234249) - REPRODUCED, cause narrowed

gate-cmp.png (HD), gate-n64.png (N64 shows a rusty gate at the tunnel end). The gate
is door prop modelnum 690 at (16792 13313 25803), room 135 - the cutscene camera's
own room. Room 135 is onscreen in HD (bgRoomIsOnscreen 1, g_BgPortalSeen 1), but the
door's prop flags are 0x4 in HD against 0xc6 in the N64 look: the prop is never
flagged on screen in HD. Drawing the 39 hidden kept rooms changes nothing (not
gebeanStageRoomHidden). Next: find why the door's onscreen test fails in HD
(objTick/prop visibility for doors - bbox from the HD mesh? the door's room list?);
gate2.py/gate3.py are the probes (frame 927).

## 7. Dam white patches on the ground by the truck park (234751) - CAUSE FOUND, not fixed

They are the release's own painted markings: 18 triangles of tex 61
(_0x030D1495, DXT5, white RGB, alpha = a dashed curved line), vertex colour
0x88e6ca89 (tan, alpha 0x88), rooms 111/121/122, all marked decal. The release would
draw them tan at about half opacity; we draw them opaque pure white, so the vertex
colour and alpha are lost on this path (alpha decal leaf: XLU decal mode, combiner
fc26a004 1f1093ff). bg.c's lighting (dlights.c) keeps the palette's alpha, and the
palette is ruled out (see 9). Next: as 9.

## 8. Dam sniper zoom: flat blue polygons over the mountains (235312) - REPRODUCED, cause narrowed

Reproduced with zoominfovy/old/new = 7 at the report camera (dam-sniper-cmp.png,
dam-sniper4-0.png). Not the backdrop (numBackdrop = 0 changes nothing). The island hut
turns into a pale ghost while zoomed: the fog grows with the zoom (about 1/0.1156),
so far rooms go solid fog colour (light blue) and some go dark blue. Suspect the
fog-line depth (env.c gebeanStageFogLine(bgGetScaleBg2Gfx(), ...) and the renderer's
G_FOGLINE_LINEAR_EXT depth) picks up the projection's fov scale. Next: print fm/fo
and the renderer's fog depth at fovy 60 and 7 for one vertex.

## 9. Dam white see-through pyramid in the server room (235806) - CAUSE FAMILY FOUND, not fixed

Reproduced (dam-fix2b-0.png). Same family as 7: 8 triangles of tex 59
(_0x0EDC9FE5, 32x32 DXT5, a white-to-grey gradient with alpha 250 -> 6), vertex
colour 0x82ffffff, room 109 - the release's lamp light cone, meant to be a faint
glow; we draw it near solid white. Report camera (10673.9 12736.7 10409.2) theta
236.3 verta -10.7, room 108; GROUND=12578 (without it the player lands a storey up).

Ruled out for 7 and 9: the room palette (buildPalette()/paletteIndex() merging the
0x82/0x88 colours into opaque ones). Seeding a palette entry per alpha level and
weighting alpha x64 in paletteIndex() changed neither picture (tried, reverted).
The leaf combiner (fc26a004 1f1093ff) takes SHADE colour and alpha only in its
second cycle - so a one-cycle draw of these (or a render mode swap that leaves the
list in one cycle) would give exactly texel white at texel alpha. Next: log the
cycle type and render mode in force when tex 59's triangles are drawn (gfx_pc
trace on room 109), and whether nofog/xlu paths put them in one cycle.


# F3 handoff: GE Plus guard aim + Facility railing (branch fix/f3-dam-guard-aim2)

Worktree /home/sdg/wt/f3gunaim, based on dabs-mod 5ac4c3176. Nothing merged, pushed or deployed.
No converter change (GECONVERT_VERSION untouched).

## 1. Dam guards never hit (F3 20260925-235655-caf13a35): FIXED + VERIFIED

Commit 6b93d5e5f (notes be2cb7145).

**Cause.** The conversion passes GoldenEye's attack bitfield through for
TRYFireOrAimAtTarget / Kneel / Update and TRYFacingTarget (AI 0x15-0x18).
GoldenEye's TARGET_BOND is 0x0001, which is Perfect Dark's ATTACKFLAG_AIMATBOND,
and nothing in PD reads that flag. PD uses ATTACKFLAG_AIMATTARGET (0x0200) for
"shoot at the target". With 0x0001:
- chrTickShoot() only calls chrCalculateHit() for AIMATTARGET, so every bullet
  of a guard's own attack went down the "shooting at something else" path. **No
  guard of a converted mission could ever hit the player**, at any difficulty
  and in either look.
- chrCalculateAimEnd() aimed from the guard's root at the player's prop position,
  which is his eye. That is the "shooting above Bond": mean shoulder pitch was
  0.233, and 0.117 after the fix.

The fourth-pass aim work (732114521, 6d1b84b29, 30c17afc2) measured with attacks
forced from gdb using 0x0200, so it never saw this.

**Fix.** aiGeAttackFlags() in src/game/chraicommands.c turns 0x0001 into 0x0200
for those four commands on a remake stage (modloaderStageIsRemake()). The
converter could translate the bit instead, but do one or the other, not both.

**Measured**: guard left to his own AI (aimprobe KEEPAI=1), player invincible and
healed each tick. The numbers are hits a minute, with chrCalculateHit() calls in
brackets.

| where | before | after |
|---|---|---|
| Dam, report's spot (guard 115 away), HD, Agent, 1800 fr | 0 (0) | 34 (103) |
| Dam, report's spot, HD, 00 Agent | 0 (0) | 50 (103) |
| Dam, report's spot, N64 look, 00 Agent | 0 (0) | 50 (103) |
| Facility (0x63 here), guard 250 away, HD, Agent, 1200 fr | 0 (0) | 12 (27) |
| Facility, 00 Agent | 0 (0) | 18 (26) |

- Defection (PD): the probe's guard never attacked in either binary, so there
  is no rate to compare. The change is gated on a remake stage and on
  GoldenEye's bit, and PD's own lists use 0x0200.
- The oracle's forced-attack figures at 300 units are 22 (Agent) and 29-31 (00).
  Natural-AI numbers are lower because the list paces attacks.

**Open.** The natural-AI hit rate has not been compared against GoldenEye's own
natural AI. gehitrate.py on the oracle forces attacks too; next step is a
KEEPAI-style run there. The fourth pass's "Dam kneel hit-rate 6 (barrier
LOS?)" was measured with forced attacks and should be re-measured with
KEEPAI=1.

## 2. Facility "can't shoot through railing" (F3 20260925-225349-5d307248): FIXED, background verified

Commit 9acab5c29.

**Cause.** In the HD look the room is the release's: gebeanstage.c serves all
rooms, and room 8 has 39 batches in HD against 30 in the N64 look. Bean draws the
railings as cut-outs in the opaque leaf. bgTestHitInRoom() tests opaque batches,
so the shot stopped on the rail 100 units out, at (-4443 12 1341). The N64 look
at the same spot already hits the floor past the guard, at (-3921 -319 1233).
GoldenEye tests the primary list only.

**Fix.** bgTriPassesShots() (bg.c) and gebeanStageTilePassesShots()
(gebeanstage.c): on a remake stage, a triangle whose picture is a served room's
cut-out or translucent one lets the shot through. It matches by the G_SETTIMG
tile pointer, because the texture number a served room gives is garbage (see the
note).

**Verified.** The HD shot now lands at (-3924 -319 1235), the same point as the
N64 look's. A gdb-called shotCreate() never registers a chr hit in either look,
so the kill itself is unverified; a tester should confirm in game.

**Side effect to watch.** In HD, bullets now also pass every cut-out: fences,
grates and leaves (for example Jungle foliage). That matches GoldenEye's
primary-list-only rule as far as those cut-outs are its secondary-list pictures.
This touches gebeanstage.c, which the HD-levels agent (~/wt/f3hdlevels) also
edits. The addition is small (one function and a stub), but check the merge.

## Rig

- Build: `cmake -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo . && make -C build -j6`
- Run dir: build/run. data/ and added-content/ are copied from
  ~/wt/f3guardaim/build/ge; mods/GoldenEye Arenas is reconverted to 71. save_hd
  is the HD look (XblaMeshes=1) and save_n64 the N64 look.
- Binaries in build/run: pd-before.x86_64 (5ac4c3176), pd-after.x86_64 (+aim fix),
  pd-rail.x86_64 (+railing fix).
- tools/guardaim/aimprobe.py adds ROOMS, GX/GZ(+PIN) and KEEPAI. Scripts
  build/run/batch*.sh; logs build/run/m_*.log.
- tools/guardaim/shotprobe.py is the railing probe. Logs: build/run/rail_*.log.
- Report spots:
  - Dam: X=8018.7 Y=12732.4 Z=9169.7 ROOMS=103,104 TH=40.1, guard GX=7940
    GZ=9254. Without ROOMS the player lands on the road 540 above. In HD,
    chrMoveToPos() puts the guard 60 units nearer (spawn adjust), hence GX/GZ.
  - Facility (0x63 in this install, 0x7a in the tester's): X=-4544.9 Y=80
    Z=1361.7 ROOMS=8 TH=258.4 VA=-32 CHR=44.
