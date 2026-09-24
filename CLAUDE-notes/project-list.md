# Projects we can do

Work the user has judged worth doing but not started. Each entry says what
is already known, so picking one up does not start by measuring again.

## Rendering the scene more than once a frame (real reflections)

Added 2026-09-14. Today's reflections are all environment maps: K7 Sheen
(the stock K7's texgen spans off each vertex's normal, now with the eye's
ray and a walking shift) and the release's cube maps baked to sphere-map
atlases (per vertex, or per pixel through `G_ENVMAP_EXT`). None of them shows
the level. Planar reflections and per-room probes would, and both need the
scene drawn more than once a frame. The user's rule: **make the extra passes
cheap first, or it is not worth doing.**

### What a pass costs today

Profiled with `tools/perf/perfprof.sh` on the seeded 80-simulant match at
stage 0x32, game thread, 20 s:

| | share of the thread |
| - | - |
| `gfx_run`: the list interpreter, every vertex transformed and every triangle written on the CPU | 59% |
| building the lists: `bgRender` 1.7%, `propsRender` 1.6% | ~3% |
| `propsTickPlayer`, the chr and object ticks, which run inside `lvRender`'s player loop | 29% |

A second full view is about +60% of the thread (roughly 36M to 58M
instructions a frame) and twice the draw calls on Mesa's threads, nearly all
of it interpretation. A mirror sees fewer rooms, so likely less, but that is
not measured. Six probe faces a frame would be about +350%.

### The order to do it in

1. **Measure how `gfx_run` splits into rooms, chrs and props, and the HUD.**
   Not done yet. It decides whether step 2 or step 3 comes first: a crowded
   match is probably mostly chrs, a solo level mostly rooms.
2. **Keep room geometry on the GPU.** Upload a room's vertices once and
   transform them in the vertex shader, so an extra pass over rooms is a few
   draw calls. This pays on the one pass too. It moves the transform to GPU
   floats, which ends the byte-exact screenshot check in performance.md for
   rooms; that check needs a tolerance instead.
3. **Draw posed models again under one more matrix.** Poses are built in the
   tick and already carry the main camera's view (`chrTick` from
   `camGetWorldToScreenMtxf()`, chr.c:2801-2828; objects at propobj.c:11121),
   so another view is P * (V_other * V_cam^-1), which is cheap on the GPU once
   step 2's path exists.

### What a render-only pass has to deal with

Split-screen (`lv.c:1223`) proves the list, matrix pool, depth and portal
code take several views a frame, but it is not a render-only pass: every
view runs the tick too, and player 0 is treated specially (`bg.c:2236`,
`prop.c:2069`).

- `PROPFLAG_ONTHISSCREENTHISTICK` (set chr.c:2759) is read by the AI
  (chraction.c:6687, 13695; chraicommands.c:1490; objectives.c:506), and
  `ROOMFLAG_ONSCREEN` decides which props tick fully (prop.c:2135) and which
  rooms load (bg.c:6171). A pass that reruns `bgTickPortals` must save and
  restore `g_Rooms[].flags` and `g_BgDrawSlots` (bg.c:6093-6122).
- Chrs off the main camera are never posed (`needsupdate`, chr.c:2572-2713),
  and a mirror mostly shows what is behind the player. Posing them without
  setting gameplay flags costs tick time.
- The sun flare timers advance in render (`skyRenderSuns` adds
  `lvupdate240`, sky.c:2741), and so do the drug blur countdown and a few
  player actions in `lvRender`'s loop body (lv.c:1257, 1350-1480, 1678).
- Already fine: pools are sized per player and 8x on PC (gfxmemory.c:98-166),
  the draw-slot cache is keyed on `g_BgFrameCount`, dyntex and Level Sheen
  cope with two draws a frame, glare artifacts clear per player, textures
  load with the room.

### The renderer

One `gfx_run` a frame (video.c:137), so every pass goes in the one list.
Offscreen targets exist (`gfx_create_framebuffer`, gfx_pc.cpp:4293;
`G_SETFB_EXT`, `G_SETTIMG_FB_EXT`, `G_COPYFB_EXT`; the menu blur uses them)
and `G_INVERT_CULLING_EXT` is there for a mirrored view. Missing: colour
targets are `GL_RGB8` only, depth is a renderbuffer that cannot be sampled,
and there are no cube textures or mipmaps.

### What it would give

- **Planar reflection** on a chosen floor or mirror: a mirrored camera, a
  clip plane, inverted culling, drawn to a target and sampled back. Medium
  on top of the render-only pass.
- **Per-room probes**: the real level captured into a cube map per room,
  once at load or a face a frame, sampled per pixel. Needs cube textures in
  the renderer. Nothing moving shows in them.
- K7 Sheen stays either way; these would be styles beside it.

## A real main light on the HD GoldenEye models (noted 2026-09-24, not pressing)

Bean's pixel shaders carry a lighting model the release never turns on:
`saturate(dot(n, light0dir)) * light0colour + ambient`, times vertex colour
and texel, and on the gun/wheel shader a Blinn highlight (power 44.6, 0.35).
The release sets ambient 1 and light colour 0 on every draw, so its HD look
is texel times baked vertex colour (ge-bean.md, "The release's lighting: the
shaders can light, the game never turns it on"). The user judged a real
light a possible future endeavour, not a fix. What is known for it:
- the formula and constants above, and that the normals already reach the
  renderer for the reflections (xbla.md, "Per pixel": the vertex colour
  carries the normal under `G_ENVMAP_EXT`);
- no light values exist to copy - they would be chosen, per level (a sun
  for the outdoor levels), and so it is a new look behind a setting that
  defaults off;
- the port's HD meshes also take Perfect Dark's per-prop room shade, which
  the release does not; matching the release exactly would mean dropping it.
