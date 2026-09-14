# Level Sheen: the K7's sheen on rooms and props (2026-09-14)

**Removed the same day.** The user tried it, found it "too shimmery, not
realistic", said the premise (a sheen on every surface) was wrong, and asked
for it to be removed. Gone: `Mod.LevelSheen`, `Mod.LevelSheenStyle`, both menu
rows, the Settings Preset column, the room and prop-part copies and their
call sites in `bg.c`, `model.c`, `xblamesh.c`, `propobj.c`, `lv.c` and
`struct room`. What stays in `port/src/roomsheen.c`:

- `roomSheenTexgenShift()`, the movement scroll the XBLA meshes' K7 Sheen uses
  (the user: "the weapon sheen is a keep k7 sheen way").
- Level Reflections (`Mod.LevelReflectFollow`, below), which moves the
  reflective surfaces the levels mark themselves.
- In the renderer, `SHADER_OPT_FOG_FADE`, which still fades `G_ADDITIVE_EXT`
  under fog. `G_MULADD_EXT` (the `GL_DST_COLOR, GL_ONE` blend and the fourth
  `set_use_alpha` argument) was removed at the user's request, also the same
  day; bit 0x1000 of the extra geometry mode is free again.

After the removal, a Defection drive with Level Reflections on and off still
differs only on the walkway's metal trim and the FPS counter. The rest of
this note is the record of what was built and measured before it went.

The stock K7 Avenger's sheen, which the user preferred to the release's cube
maps on the XBLA meshes (xbla.md, "The N64 sheen"), drawn on the level itself:
every solid room surface and every prop standing in the level. It works on
the ROM's rooms and models and on the release's alike.

It is behind `Mod.LevelSheen` (0 off, 1 Subtle, 2 Normal, 3 Strong; "Level
Sheen" on the Xbox 360 (XBLA) page, off by default) and `Mod.LevelSheenStyle`
("Level Sheen Style", K7 or Per Pixel, hidden while the sheen is off). Both
are live. K7 is the style a new player starts in, at the user's pick on
2026-09-14. The Settings Preset covers the strength only: Dab's Settings
turns it on at Strong, and Vanilla and Ghost Trials keep it off. Code:
`port/src/roomsheen.c`, `port/include/roomsheen.h`.

## Why it is built from triangles

The release carries no shine for levels. Its level files are the N64 room
format; bits 16-31 of all 20,136 room texture words are zero except for 4J's
own bookkeeping (xbla.md, and memory: top-task-xbla-lighting). A Xenia draw
log of Defection does show the release putting its cube-map shader on a few
level-space draws (about 2.6k indices a frame on about six textures), so 4J's
code picks some surfaces. The user chose to pick our own instead, and then
said almost every surface could benefit, so it is all of them.

Neither rooms nor the ROM's models have normals: they carry baked vertex
colours. So each room's copy, and each prop model part's, is made from its
triangles:

- **Rooms:** the opaque layer's lists after `texLoadFromGdl()` and the fog
  replacements. Decals, cutouts (`CVG_X_ALPHA`) and translucent render modes
  inside the lists are skipped.
- **Model parts:** the part's opaque list. A model's lists set no render mode
  of their own (model.c writes them), so a part is judged by the renderdata
  instead.
- **Normals:** per corner, the area-weighted normals of the faces meeting at
  that position within about 37 degrees of its own face. Boxes keep their
  edges and curves stay smooth.
- **The copy:** triangle soup, three vertices a triangle and four a batch.
  Each batch is its own `G_COL` window, which keeps colour and `G_TRI4`
  indices in range. The normals go in the colours' RGB and the amount in
  their alpha. The vertices are named through segment 4.

## How it is drawn

- **Rooms:** `roomSheenRender()` right after `bgRenderRoomOpaque()` in the main
  room loop of `bg.c`. That call site leaves out the special sky rooms, which
  go through `bgRenderRoomOpaque()` from elsewhere.
- **Stock model parts:** `modelRenderNodeDl()` in `model.c`, after the opaque
  list and its mcount-3 translucent list.
- **XBLA mesh parts:** the end of `xblaMeshRenderNode()`'s opaque section,
  after the cutout span and the reflection pass. Drawing any earlier leaks the
  pass's combiner into the cutout span, whose list writes none of its own.
- **Pass state:** 2-cycle with `G_RM_AA_ZB_XLU_INTER`, so the depth compare
  lands on the surface just drawn, and 0x3eb through `xblaMeshSheenTile()`
  (shared with the meshes' sheen, and repainted by a texture pack).
- **K7 style:** `G_LIGHTING | G_TEXTURE_GEN` at `G_TEXTURE 0x0800`, lit by
  `lightsSetForRoom()`.
- **Per Pixel style:** `G_ENVMAP_EXT`, with the tile as a one-cell atlas
  (s = half a tile, t = a whole one).

### K7 follows movement, not only turning (G_TEXGEN_EYE_EXT, 2026-09-14)

The N64 texgen reads only the normal against the LookAt, so it cannot see
where the eye stands: turning moved the sheen, walking did nothing. Every K7
sheen pass sets the port-only `G_TEXGEN_EYE_EXT` beside `G_TEXTURE_GEN`: the
rooms and props here, and the XBLA meshes' K7 Sheen in `xblamesh.c`. Stock
texgen (the N64 guns) is untouched. The flag does two things in
`gfx_sp_load_vertex()`:

- **Eye ray:** `gfx_texgen_eye_normal()` hands the texgen the half-way vector
  between the straight-on ray and the reflection of the ray the vertex is
  seen along. It is worked in modelview space and brought back to model
  space. In the middle of the screen it is the old normal exactly; elsewhere
  it turns as the vertex crosses the view, so a wall is no longer one tint.
- **Shift:** a gun held in front of the eye walks with it, so the eye ray
  cannot move its sheen. `roomSheenTexgenShift()` sends
  `G_SETTEXGENSHIFT_EXT` (opcode 0x47, s and t in 1/16384ths of a span) from a
  per-player accumulator. Movement across the view scrolls s, and movement
  along it (minus rise) scrolls t, at one span per 400 units
  (`ROOMSHEEN_SHIFT_PERIOD`). It is a delta, never the absolute position, or
  turning far from the origin would spin it. A move over 200 units in one
  frame (a teleport, a respawn, a cutscene cut) scrolls nothing.

Checked on Chicago under `--spectate` at Strong, K7. The intro was skipped
at 600, and the camera walked 142 units in ten frames with the heading held
(scratchpad `move.py`). The sheen's gain changed on 66% of the lit pixels
whose geometry stayed put (mean change 0.10), and no pixel was darker than
with the sheen off. The XBLA mesh path shares the flag and the helper but
was not captured with a gun in hand.

### The levels mark their own reflective surfaces (Level Reflections, 2026-09-14)

The user tried the all-surface sheen and found it "too shimmery, not
realistic", and pointed at what the stock game already shines: Defection's
metal and its windows. `bgRenderRoomOpaque()` sets a room's lights and LookAt
before its lists for this reason. A room's own lists turn on
`G_LIGHTING | G_TEXTURE_GEN` over those triangles, the vertex colours are
normals there, and the bound texture is a round environment map (0042 is a
blue sphere, 006d a grey one). Counted statically from the ROM (scratchpad
`texgenscan.py`, which pulls `bg_*.seg` with `tools/extract`'s class and walks
rooms as `bgtexscan.py` does):

| bg file | texgen tris | opaque / translucent (approx.) | textures |
|---|---|---|---|
| `bg_ame` (Defection) | 1537 of 19780, 102 rooms | 1002 / 535 | 0042, 006d, 0043, 0059 |
| `bg_sho` (Skedar Ruins) | 432 | 0 / 432 | 0296, 02dd |
| `bg_dish` (CI, Defense, Duel) | 376 | 20 / 356 | 0042, 027e |
| `bg_rit` (Air Force One) | 321 | 0 / 321 | 029d, 0228 |
| `bg_ear` (Investigation) | 314 | 262 / 52 | 0043, 0059 |
| `bg_pete` (Chicago) | 242 | 0 / 242 | 0189, 01c9 |
| `bg_azt`, `bg_dam`, `bg_depo`, `bg_oat`, `bg_eld`, `bg_lee` | 231 down to 4 | mostly translucent | |

`Mod.LevelReflectFollow` ("Level Reflections" on the XBLA page, Original /
Follow Movement, off by default, not in the preset table) wraps both room
passes in `roomSheenStockBegin()`/`End()`, which set `G_TEXGEN_EYE_EXT`. The
flag only acts where a list has `G_TEXTURE_GEN` on, so nothing else moves.
The texgen shift is written as zero first. A room stands still in the world,
so the eye ray alone moves its reflection as the player walks; the scroll is
for a gun that walks with the eye, and a leftover gun shift would slide every
window.

Checked on Defection (`--boot-stage 0x30`, spectate, seed 1, fixed step, two
shots 120 units apart with the heading held, Level Sheen off): Follow
Movement against Original differs on 2.0% and 1.4% of pixels, every one of
them on the walkway's metal trim, and the mean brightness is identical. The
all-surface copy (Mod.LevelSheen) is untouched and still separate.

Traps met doing it:

- **`--boot-stage` takes the stage id.** Defection is 0x30; 0x1c is its row
  in `g_Stages`, and booting 0x1c drew a canyon with no texgen surface, where
  the setting changed only the FPS counter.
- **Never `mv screenshots/pd-*.png` out of `build/screenshots`.** The folder
  holds hundreds of older shots. Take a stamp file before the run and move
  only `find -newer` the stamp.

### Walking turns the levels' reflections (G_TEXGEN_TURN_EXT, 2026-09-14)

The user, with the XBLA switch on, found the levels' metal "not acting the
same as the camera view, but snaps to a frame". Two things were wrong with
the check above:

- **It ran with the release off.** Its save had no `XblaMeshes` line, and the
  meshes (so the release's rooms) default off. The release's rooms keep the
  ROM's texgen marks exactly: a static scan of all 31 rewritten `bg_*.seg`
  counts the same texgen triangles on the same textures (Defection 1537 in
  both; only Aztec drops, 231 to 37, and Air Force One renumbers 029d to
  01e9), and the release's 0042/006d are still sphere maps. So the flag
  reaches the release's rooms too, and nothing about XBLA needed its own code.
- **The eye ray alone is too faint to see.** A seeded 24-frame walk (4 units a
  frame, heading held) of Follow against Original on the release's Defection
  changed the flag's contribution by about 6 levels a frame on 1.2% of the
  pixels, and the contact sheets were indistinguishable. A reflection of an
  environment at infinity in a flat surface *is* pinned to the screen while
  strafing, so the eye ray was physically right and still looked like the
  N64's stuck lookup.

The fix makes walking do what turning does. `roomSheenStockBegin()` sends the
movement accumulator (the same one the guns scroll by) as a fraction of a
turn, and `G_TEXGEN_TURN_EXT` (0x1000, free since `G_MULADD_EXT` went) makes
`gfx_sp_load_vertex()` yaw the LookAt about its own y by the across-the-view
distance and then pitch it about the turned x by the along-the-view distance,
half a turn per 400 units, before the texgen dots. A turn stays on the round
map and wraps with no seam; adding a scroll as the gun does would run off the
picture into its black corners, since 0042 does not tile as 0x3eb does. The
accumulator now wraps at 800 units and the guns read it at two spans a turn,
which is the old 400-unit span to the value. Same walk after: 10-13 levels a
frame, steady, no spike, and the walkway's bars visibly change down the walk.

The "snaps" were not reproduced as such; the user's pd.ini on the test box
had Level Reflections at Original, which is the pinned look.

### Props carry texgen of their own (2026-09-14)

The user then found Defection's windows and the lift's metal still pinned.
Those are **props**, not room geometry: the glass panes (`OBJTYPE_GLASS` and
`OBJTYPE_TINTEDGLASS`, model 0x3f) and the lift (`OBJTYPE_LIFT`, model 0x37)
draw through `objRender()`, never through the room passes. A static scan of
every `P*Z` model file (scratchpad `modeltexgen.py`: inflate, then walk the
words for `0xb7` carrying `G_TEXTURE_GEN`) finds about 90 prop models that
turn texgen on in their own lists: `Pdd_liftrZ`, `Pdd_liftdoorZ`,
`Pdd_windowZ`, `Pdd_window_foyerZ`, `PwindowZ`, `Pci_liftdoorZ`, the
hovercars, the logos and more. None of Defection's lift and window models
has a release mesh, so they stay stock with XBLA on.

`objRender()` now wraps `objRenderProp()` in `roomSheenStockBegin()`/`End()`
(children included, both passes). Chrs, held guns and the first-person gun do
not go through it and are untouched. An XBLA mesh drawn inside the wrap draws
its K7 sheen as before: that pass clears `G_TEXGEN_TURN_EXT` so its scroll is
not read as a turn, and `roomSheenStockResume()` writes the turn and both
flags back after it for the prop's remaining stock spans.

Checked on the release's Defection (XBLA meshes, stages, reflections at K7;
`--boot-stage 0x30 --spectate --rng-seed 1 --fixed-step`, on the GPU; the
scratchpad's `propwalk.py` walks `g_Vars.activeprops`, parks the eye 350 units
in front of the first lift and pane of each glass type, and shoots before and
after a 120-unit strafe). Follow against Original differs on 13-20% of pixels,
every one of them on the lift's cage and rail and on the panes. Under
Original a pane shows the same part of the map before and after the walk;
under Follow it turns to another part.

**A broken window's shards** are drawn by `shardsRenderGlass()` in
`shards.c`, which turns texgen on for them itself (the wood pass does not), so
that pass is wrapped too: Begin after its set (not under X-ray, which draws
them untextured), End after its clear. Checked with `shardwalk.py`, which
parks the eye 250 units from a tinted pane, calls `glassDestroy()` on it from
gdb and shoots three frames later and again after a 120-unit strafe: Follow
against Original now differs on the flying shards as well as on the
remaining panes. Seed and fixed step put the shards in the same place in
both runs.

### The blend has to multiply, not add (G_MULADD_EXT)

The first version added the sheen as the meshes do (`G_ADDITIVE_EXT`). On
Defection's night streets at Normal that took the mean brightness from 18 to
32 (K7) and 52 (per pixel) and turned the dark walls flat grey. 0x3eb is
mostly navy, and adding it lifts every dark texel equally.

`G_MULADD_EXT` (gbiex.h, 0x1000) makes the renderer blend
`glBlendFunc(GL_DST_COLOR, GL_ONE)`: dst + src x dst. What was drawn gains in
proportion to its own brightness, so dark stays dark and lit windows catch the
streak. The blend reads no alpha, so the amount is folded into the colour
(the combiner's second cycle is COMBINED x SHADE_ALPHA). It is threaded
through `set_use_alpha()` in the rendering API as a fourth flag.

### Fog (SHADER_OPT_FOG_FADE)

The room lists draw under a fog blend. Mixing an additive or multiplied pass
towards the fog colour, as the shader does for everything else, would add the
fog colour on top. `SHADER_OPT_FOG_FADE` (gfx_cc.h, bit 15) is set for any
draw that uses fog with `G_ADDITIVE_EXT` or `G_MULADD_EXT`, and the fragment
shader then scales the colour by `1 - fogFactor` instead. The room pass uses
`G_RM_FOG_SHADE_A` in its first cycle when `g_FogEnabled`. The prop pass
does not, since props draw with fog stopped.

## Props: which, and the cache

- **Which models:** a part takes the sheen only while `objRenderProp()` has
  named its prop (`roomSheenSetProp()` around its `modelRender()`) and:
  - the prop is an object, door or pickup with no parent;
  - the pass is opaque, with the z buffer on;
  - `unk30` is 9 and the envcolour's low byte is 0. A low byte set means
    glass or a fade, which `modelApplyRenderModeType3` draws as `TEX_EDGE`
    cutouts that a sheen cannot follow.

  Characters, held guns (drawn through the chr's renderdata), the
  first-person gun and menu models are never drawn with a prop named.
  `struct model`'s chr/obj union cannot be trusted for this: `modelInit()`
  sets neither.
- **The cache:** a part's copy is kept in an 8192-slot table keyed by (node,
  list). The key is checked against the list's first command so that a
  modeldef freed and reused inside a stage cannot hand back a copy of
  something else. `lvReset()` clears the table beside
  `xblaMeshResetModels()`.
- **Positions are read again at every draw** from the part's vertex array
  (`srcidx`). Doors trim their own vertices as they open, and an XBLA mesh's
  pose is a new array every frame.
- **Two props of one model in one frame** each need their own copy of the
  vertices (`gfxAllocate()`). The renderer runs the frame's lists after they
  are all built, so a single shared buffer would put every chair at the
  position of the last one drawn. The shared copy serves the first draw of a
  frame (`lvframenum` and the base it was filled from).

## Measured

- **Look at Normal, Defection level frame 600** (seeded, fixed step):
  - K7: mean 18.1 to 19.1, 13% of pixels changed.
  - Per pixel: mean 18.1 to 21.0, 44% of pixels changed.
  - Essentially no pixel darker (the few lightly darker ones are within
    noise).
- **Look at Strong** (screenshots in the session scratchpad):
  - Carrington Institute frame 290: mean 83 to 88 (K7), 83 to 92 (per pixel).
  - Chicago frame 900: mean 40 to 42.5 (K7), 40 to 43.7 (per pixel).
- **K7 was flat on flat geometry until 2026-09-14:** a wall has one normal,
  so one tint, and it changed with the camera's heading only. The user saw it
  move on the right stick and never the left. Fixed as below ("K7 follows
  movement"). Per pixel still reads as the smoother sheen on rooms.
- **Cost, rooms only** (80-simulant seeded match, 1200 frames, main thread):
  off 36.7M, K7 Normal 37.2M, per pixel Normal 38.0M instructions a frame; +6
  draws a frame.
- **Cost with props** (2026-09-14, idle machine, load 0.1; same match, two
  runs of each, interleaved off/K7/per pixel):
  - Off: 36.33M and 36.36M instructions a frame, 129 draws.
  - K7 Normal: 38.69M and 38.15M, about +2.1M (+6%), 140 draws.
  - Per pixel Normal: 37.55M and 38.22M, about +1.5M (+4%), 140 draws.
  - Run to run, K7 and per pixel vary by about 0.6M, off by 0.03M.
  - Against rooms only (+0.5M K7, +1.3M per pixel), props are most of K7's
    cost: K7 lights and texgens every prop vertex on the CPU, while per pixel
    does its work on the GPU, which this count does not see.
  - All six matches reached frame 1200 with no fatal errors.
  - An earlier run, taken while builds and screenshot jobs were running, read
    off 40.9M, higher than on. Load changes what the main thread does, so
    measure only on an idle machine.
- **The prop pass on screen** (2026-09-14, Strong, the frames of the
  rooms-only shots above): props change 0.4% to 7.8% of the pixels over the
  rooms-only picture (Carrington 150 and 290, Chicago 600 and 900), with no
  pixel darker than the sheen off. The police car on Chicago's street at
  frame 900 keeps its texture and takes a soft streak in either style.
  Carrington's 290 means are now 88.5 (K7) and 93.1 (per pixel); Chicago's
  900 are 43.0 and 45.2.

## Traps met on the way

- **One Per Pixel run drew every prop as stripes, and it has not come back.**
  - *What:* Chicago (`--boot-stage 0x1d`, seed 1, fixed step, Strong, Per
    Pixel), frames 600 and 900 of the same run. The car, the shutter and the
    flying cars were bands of blue, cyan and green with no red: white
    (163,162,164) came out (0,243,221). 26% of frame 900 was darker than the
    sheen off, which `dst + src x dst` cannot do. The sheen tile's colours
    wrapped over each prop's own UVs, so it looks like the props sampling the
    wrong texture, not a bad blend.
  - *Tally:* 1 of 9 Per Pixel runs of those frames. The other 8 were clean
    and pixel-identical to each other, 5 of them with no gdb at all
    (`--screenshot-frame 900 --exit-frame 910`). No K7 run broke.
  - *Ruled out:*
    - The batch state: gdb at gfx_pc.cpp:2244 showed `muladd=1`, alpha on,
      no modulate, for every sheen batch.
    - A state leak into the next part: both styles' batch sequences are
      identical after each sheen batch.
    - The vertex layout: the emit plan and the shader put the envmap floats
      in the same place.
  - *Suspect:* timing. A breakpoint inside `roomSheenEmit()` that changed
    nothing made it vanish. The texture pack's decodes land off the thread
    and `gfx_texpack_poll()` drops cache entries when they do. The cache also
    leaves `rendering_state.textures` pointing at an entry it evicts.
  - *Next time:* the broken run's log was overwritten by the re-run. Keep
    each run's log under its own name, and pass `--gfxstats 60` so evictions
    are counted.

- **The Carrington Institute stops at level frame 302 under
  `--boot-stage 0x26`.** A dialog opens and waits for input: `lvupdate240` is
  0 and `g_MenuData.count` is 1, while the main loop keeps presenting. HEAD
  does the same. gdb waiting for a later `lvframenum` waits for ever, so take
  shots before 300.
- **The `pd-testsave*` saves carried `MemorySize=16`** (the default is 64).
  That floods "memory pool is full" warnings, which are only the onboard bank
  spilling into the 8 MB expansion bank. It had nothing to do with the stall.
  All 40 were set to 64 on 2026-09-14.
- **gdb `-batch` keeps executing `-ex echo` commands after the inferior is
  killed**, so the last "REACHED n" printed is not the last frame reached.
  Read the markers before "Program terminated".
- **A backgrounded build run from the wrong directory prints "not a
  directory"**, which a `grep "Built target"` filter hides. Check the
  binary's mtime.
