# Level Sheen: the K7's sheen on rooms and props (2026-09-14)

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
- **K7 is flat on flat geometry:** a wall has one normal, so one tint. It
  changes with the camera's heading only. Per pixel is what reads as a sheen
  on rooms: a highlight band down a wall, a gradient across a floor.
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
