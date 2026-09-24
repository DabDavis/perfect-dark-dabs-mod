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
