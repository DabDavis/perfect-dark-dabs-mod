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
frame. Next: same shot in the N64 look (N64=1) to see whether GoldenEye's door fills
it; compare the Bean door mesh's bbox with the N64 model's (setup scales doors to
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
fc26a004 1f1093ff; check what bg.c's room lighting does to the palette's alpha and
whether the shade reaches the colour). Next: dump the room's G_COL palette at draw
time and the combiner actually used for tex 61.

## 8. Dam sniper zoom: flat blue polygons over the mountains (235312) - REPRODUCED, cause narrowed

Reproduced with zoominfovy/old/new = 7 at the report camera (dam-sniper-cmp.png,
dam-sniper4-0.png). Not the backdrop (numBackdrop = 0 changes nothing). The island hut
turns into a pale ghost while zoomed: the fog grows with the zoom (about 1/0.1156),
so far rooms go solid fog colour (light blue) and some go dark blue. Suspect the
fog-line depth (env.c gebeanStageFogLine(bgGetScaleBg2Gfx(), ...) and the renderer's
G_FOGLINE_LINEAR_EXT depth) picks up the projection's fov scale. Next: print fm/fo
and the renderer's fog depth at fovy 60 and 7 for one vertex.

## 9. Dam white see-through pyramid in the server room (235806) - NOT STARTED

Likely the same family as 7 (a release mesh with vertex alpha drawn solid white:
a light cone). Report camera (10673.9 12736.7 10409.2) theta 236.3 verta -10.7, rooms
108/107/109; use GROUND=12578 (without it the player lands a storey up).
