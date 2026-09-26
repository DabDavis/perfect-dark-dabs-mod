# F3 pass: light glare / sun through walls (branch fix/f3-glare-through-walls)

Tester Parabolee, Windows 718d5dc, 3840x2160, XBLA meshes + stages on, Glare
Clipping on (`Mod.GlareClip=1`), texture pack XBLA Plus HD v0.10. All four
traces say "rooms from release 0": the XBLA release rooms play no part.

One cause behind all four: whether a light's glare or the sun is *seen* was
decided by `shotTestLos()` (collision line tests), not by what is drawn. The
N64 read its z-buffer for this; the port now asks the GPU the same question
with occlusion queries. Details: CLAUDE-notes/third-person.md, "Glares and the
sun seen through walls: the GPU reads the depth".

| Report | Stage | What it was | Status |
|---|---|---|---|
| 20260925-224836-5070ebf7 "puddle reflecting a light" | Chicago 0x1d | light of the yard (room 14) 300 units under the street | fixed |
| 20260925-224933-0278af9d "puddle light showing through walls" | Chicago 0x1d | same light, seen from the yard: that one is real and still draws | fixed (explained) |
| 20260925-235810-dbc6d9de "light showing through wall" | Air Base 0x27 | the **sun's lens flare**, through a wall of room 107 | fixed |
| 20260926-001615-17e65333 "sun showing through walls" | Crash Site 0x1c | the sun's lens flare through a hill | fixed |

## Causes

- **Air Base**: the line test walks rooms from `cam_room` only
  (`portal00018148()`). The eye stands in a doorway (prop rooms 108, 107;
  cam room 108); the wall between it and the sun is room 107's and the line
  crosses no portal into 107, so all eight sun points passed.
- **Crash Site**: the hill has no collision; no room's batches hit the line to
  the sun at any length.
- **Chicago**: glare line tests skip translucent BG (`g_BgHitXluDisabled`), and
  the street is translucent over its reflection, so the yard's light passed
  through the road; Glare Clipping then cut its halo along the kerb (the
  "puddle"). The depth buffer has nothing there either (the street writes no
  depth), so the fix for this one is the portal-box rule below.

## Fix

- `artifactsTestOcclusion()` (bg.c `bgRenderArtifacts()`, after the scene,
  before the gun clears depth) emits `gDPOcclusionTestEXT()` (new
  `G_OCCLUSIONTEST_EXT` 0x4c) for each artifact written this frame: a one-pixel
  rect at the point's depth, depth tested, invisible, inside an occlusion
  query (`gfx_occlusion_test()`, gfx_pc.cpp). Light points are tested 30 room
  units in front of the light (min 2% back: `GLARE_TEST_SLACK`); sun points at
  `SUN_TEST_Z`, just short of the depth clear value.
- `artifactsResolveOcclusion()` reads each list's answers when it becomes the
  front list two frames later (the N64's delay), before `skyRenderSuns()` and
  the glares use `visiblelos`.
- Backends: `occlusion_begin/end/result` in `GfxRenderingAPI`; GL
  `GL_SAMPLES_PASSED`, Vulkan a query pool (reset in the upload command
  buffer, begin/end packets inside rendering, read waits for the query's own
  submission, which is always already done).
- A light point outside its room's portal box on screen is dropped
  (`artifactIsInRoomBox()`): the room is scissored to that box.
- `artifactTestLos()` remains the fallback when queries are unavailable.

Files: src/game/{artifact,sky,bg,player}.c, src/include/{gbiex,types,bss}.h,
src/include/game/artifact.h, port/fast3d/{gfx_pc,gfx_opengl,gfx_vulkan}.cpp,
port/fast3d/{gfx_api,gfx_rendering_api}.h, port/src/video.c,
port/include/video.h, CLAUDE-notes/third-person.md, CLAUDE.md.

## Verification

RX 580, rig `~/wt/f3glare-run` (`matrix.sh`, results in `results.txt`), tester's
`[Mod]`/`[Game]` settings, camera held at each trace's position (no
`--spectate`: it puts every room on screen with no portal boxes).

Each row: glare groups with a point seen / sun flares drawn, frames 330-332.

| Spot | Before (GL and Vulkan, 1080p and 4K) | After (GL and Vulkan; 1080p, 4K, 4K + MSAA 8x, 4K + SMAA + Supersampling 2x (7680x4320), 4K + TAA, 4K + SMAA + FSR Performance) |
|---|---|---|
| r1 Chicago street (report 224836) | yard light room 14 4/4 seen, glare on the road | room 14 light not tested (outside its portal box), nothing drawn |
| r2 Chicago yard (report 224933) | room 14 light 4/4 | room 14 light 4/4 - still glares, it is in plain view |
| r3 Air Base doorway (report 235810) | sun flare every frame | no flare; the room-109 light at the door keeps its 2 points inside its portal box |
| r4 Crash Site (report 001615) | sun flare every frame | no flare |
| r4up Crash Site, 500 up, sun over the ridge | **no** flare (line test wrongly hid it) | flare every frame |

GL and Vulkan agree in every row. Screenshots: `~/wt/f3glare-run/shots_m_*`
(full frames on the card: Vulkan under Xvfb with `MESA_VK_WSI_DEBUG=sw`, GL
offscreen). The last build (rect vertex colours kept across the test) is
pixel-identical to the matrix build at r2 and gives the same counts on six
re-runs.

GE Plus: Caverns (0x66) hanging lamps from below glare the same before and
after (all four lamps 4/4 points every frame); an early build with a 2% pull
let the swinging cages dim them, hence the 30-unit slack.

## Open

- The tester's `[Video]` settings are not in F3 reports, so MSAA/SMAA/etc.
  were covered by the matrix rather than matched.
- Chicago's yard light under the street would also pass a pure depth test (the
  street writes none); only the portal-box rule hides it. A light seen through
  a translucent non-portal surface of its own room is not covered by that.
- Glares are two frames behind the camera for visibility, as on the N64 and as
  before.

---

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

## 5. Runway door to Facility: sky in gaps round it (230105) - REPRODUCED, cause not found

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
