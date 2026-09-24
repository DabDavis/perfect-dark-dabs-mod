# SMAA and FSR 1: the post-process chain

Added 2026-09-24. Video menu, under Anti-aliasing: **SMAA** (`Video.SMAA`),
**Upscaling** (`Video.Upscaling`: Off, FSR Ultra Quality / Quality /
Balanced / Performance = 1/1.3, 1/1.5, 1/1.7, 1/2 of the window) and
**FSR Sharpness** (`Video.FsrSharpness`, RCAS's stops, 0 the sharpest,
default 0.2; the slider runs the other way). All live, all off by default.

## How it runs

- `gfx_start_frame()` sets `gfx_current_dimensions` to the window times
  `gfx_render_scale`; everything the game measures in draw pixels
  (`RATIO_X`, the autoresized framebuffers, `videoGetWidth()`) follows it.
  What must stay in window pixels asks for the window: the mouse
  (`inputMouseGetPosition()`), the Resolution dropdown's match.
- SMAA or a scale makes the game draw into `game_framebuffer`
  (`game_post_processes`), laid out as the window is (invert_y off), and
  `gfx_run()` hands it - resolved first under MSAA - to the backend's
  `post_process()`, which draws into fb 0. A false return falls back to a
  plain scaled blit.
- Passes (gfx_post.h): SMAA edges -> weights -> blend at the drawn size,
  then EASU into a window-sized image and RCAS into the window. One shader
  builder (`gfx_post.cpp`) for both renderers, from the vendored sources in
  `port/fast3d/post/` (MIT; embedded by `tools/postshaders/gen.py` into
  `gfx_post_src.h` - rerun it after replacing a source).
- Vivid Colours still runs after it, on the window.

## Supersampling

Added 2026-09-24 after an RTX 4080 tester asked for 16x MSAA: NVIDIA's Vulkan
lists colour and depth samples only to 8x (gpuinfo report 50636; its 16 is
NoAttachments/Stencil alone), and OpenGL's "16x" there is the driver's own
mix. **Supersampling** (`Video.Supersampling`: Off, 1.5x, 2x) is the same
render scale above 1. `videoSetSupersampling()` and `videoSetUpscaling()`
share `gfx_render_scale`, so choosing one turns the other off; an ini with
both keeps supersampling. The drawn size stops at `GFX_MAX_RENDER_SIDE`
(8192) on the longer side (gfx_pc.cpp). The way down is `GFX_POST_COPY`:
past a 1:1 footprint (from `dFdx`/`dFdy` of vUV) it averages up to 4x4
bilinear taps over the window pixel, which at 2x is an exact 2x2 box. SMAA
still runs at the drawn size, before it. Checked GL and Vulkan on the RX 580
(2x + 8x MSAA + SMAA agree within 9 levels) and live from gdb.

## TAA

Added 2026-09-24. **TAA** is the last entry of the Anti-aliasing dropdown
(`Video.TAA`); it needs a single-sampled frame, so `videoSetTaa()` and
`videoSetMSAA(>1)` turn each other off. SMAA and Supersampling stack on it.

- **Only the world.** lv.c's `lvRenderTaa()` emits `gSPTaaEXT` (gbiex.h,
  0x4a) BEGIN just before `skyRender()` and END just before the gun/HUD
  (`playerRenderHud`, or boltbeams/artifacts outside mode 2), per player.
  Between them `gfx_sp_load_vertex()` adds a Halton(2,3) jitter of up to
  half a pixel in clip space; END flushes and resolves that player's
  viewport. The gun, HUD and glares are never jittered or blended.
- **Camera-only reprojection.** No motion vectors exist (poses are baked in
  view space). BEGIN carries world -> clip: `camGetMtxF006c()` (the rooms'
  perspective x lookat, in draw space) with `globaldrawworldoffset` and
  `scale_bg2gfx` folded in, since the offset moves with the camera's room.
  `gfx_taa_resolve()` (gfx_pc.cpp) builds, in doubles, one 3x4 matrix
  taking (u, v, depth, 1) straight to last frame's (u, v) times w: the
  viewport, the aspect adjustment (`x' = k(x + o w)`), inv(this frame's)
  and last frame's matrix. History per player slot (split screen), valid
  only for the previous `num_dls` at the same size.
- **Shader** (`GFX_POST_TAA`): nearest depth of the 3x3, five-tap
  Catmull-Rom history, variance clip (1.25 sigma) to this frame's 3x3, 10%
  of this frame. Moving chrs/doors rely on the clip alone.
- **Backends** (`taa_resolve`): the game framebuffer's depth is copied out
  first (GL: renderbuffer -> depth texture by `glBlitFramebuffer`; Vulkan:
  `vkCmdCopyImage` of the depth aspect into a sampled depth image with a
  depth-only view - fb depth images gained TRANSFER_SRC), the pass draws
  into history `num_dls & 1` scissored to the rect, and the rect is copied
  back. A false return turns TAA off for the run (no jitter left behind).
- **Trap: the Vulkan recorder's push record was 32 bytes** (`VkpPush`), so
  the 112-byte block reached the GPU as zeros and TAA silently passed the
  frame through. `VKP_PUSH_BIG` carries anything larger; the pipeline
  layout's range is 112.
- **Checked** on the RX 580, both renderers, a seeded match turning the
  camera 0.4 and 2 degrees a frame from gdb (`vv_theta`): the raw
  reprojection (uTaa[4].y = 0, alpha 0) matched the true frame at mean 1.5
  against 7.8 for the unreprojected last frame; GL and Vulkan TAA agree
  within 0.07. Not checked: split screen, a guard walking through view,
  cutscene cuts (the clip should cover a cut).

## Traps met

- **GLSL before 4.20 has no line continuation**, and SMAA.hlsl's banner is
  ASCII art ending lines in `\`: the generator strips trailing backslashes
  (none of the three files continues a macro).
- **SMAA's edge pass discards** where there is no edge, so its target is
  cleared every frame or last frame's edges stay.
- **Vulkan's GLSL cannot pass `sampler2D(tex, smp)` to a function**, and
  SMAA passes textures to functions: `SMAA_CUSTOM_SL` with a texture being
  its bindless slot (an int) and the sampling macros building the combined
  sampler at the read.
- **Nothing is flipped.** Every image is in GL's row order in both
  renderers and SMAA/EASU work in texture space, so the area and search
  textures go up as their headers store them. Checked: SMAA and FSR output
  line up with native, and Vulkan matches GL within 6 levels.
- FSR on GL needs 4.2 (`textureGather` with a component, `packHalf2x16`);
  below that the scale falls back to the bilinear copy. SMAA and the copy
  take whatever GLSL the renderer's own shaders use (1.30 in compat).
- `gfx_copy_framebuffer()` with `use_back` read fb 0's back buffer unless
  MSAA was on; with the game drawing offscreen for any reason it must read
  `game_framebuffer`, which now happens whenever `game_renders_to_framebuffer`.
- The screen shake's offset (vi.c -> `gfx_current_game_window_viewport.y`)
  was only applied when drawing straight to the window or under MSAA; it is
  applied always now, scaled to draw pixels.

## Testing

`SDL_VIDEODRIVER=offscreen` for GL, Xvfb + `--vulkan` for Vulkan, a seeded
`--boot-stage 0x32 --mpsims 1 --rng-seed 7 --fixed-step` match, gdb
breaking `videoEndFrame` at an `lvframenum` and calling `screenshotRequest()`;
settings go in the scratch pd.ini's `[Video]` or live from gdb
(`videoSetUpscaling(4)`). **A menu stops `lvframenum`**, so after opening
one count frames with `ignore $bpnum N` instead. **A shell not in the
`render` group gets llvmpipe for both renderers** (`libEGL warning: failed to
open /dev/dri/renderD128`), which proves correctness but not speed.
