# The Vulkan renderer

## Digest (moved from CLAUDE.md, 2026-09-30)

The entries CLAUDE.md carried for this note, verbatim. The sections below are
the long form.

- **The Vulkan renderer** — [vulkan.md](CLAUDE-notes/vulkan.md): `Video.Renderer`/`--vulkan`, falls back to OpenGL by itself; every image in GL's row order so nothing flips; bindless textures, a recording worker thread and an on-disk shader cache are why it beats GL; tests run it under `SDL_VIDEODRIVER=offscreen` on the card through a headless surface (Xvfb plus `MESA_VK_WSI_DEBUG=sw` is the fallback); a new `SHADER_OPT_*` goes in both shader generators


`port/fast3d/gfx_vulkan.cpp`, beside `gfx_opengl.cpp` behind the same
`GfxRenderingAPI`. **Video.Renderer** (0 OpenGL, 1 Vulkan; Video menu,
"Renderer", takes effect at the next start), `--vulkan` / `--opengl` override
it. Added 2026-09-23. OpenGL stays the default.

## How it is built

- Nothing links against a Vulkan library: SDL's `vkGetInstanceProcAddr` loads
  every entry point, so a machine with no loader still starts - on OpenGL.
  If anything in init fails (no loader, no device, no surface) `video.c`
  destroys the window, remakes it for GL and runs `gfx_init()` again
  (`gfx_vulkan_failed()`); `Video.Renderer` is left as the player set it.
  `gfx_init()` still asks the failed renderer for its two framebuffers
  before `video.c` looks, so every entry point it calls must return quietly
  with no device: `gfx_vk_create_framebuffer()` did not, and a failed start
  (no window, no loader, no device) was a `PC=(nil)` segfault in
  `vk_get_sampler()` until 2026-09-28.
- Needs Vulkan 1.2 with dynamic rendering (core 1.3 or the KHR extension) and
  descriptor indexing (partially bound, update-after-bind,
  update-unused-while-pending). No render passes, no framebuffer objects.
- Shaders: the GL renderer's GLSL generator, ported to Vulkan GLSL, compiled
  to SPIR-V at run time with **shaderc**, which is linked statically - CMake
  accepts only `libshaderc_combined.a` and the static glslang/SPIRV-Tools
  parts Debian/Ubuntu split out of it, and link-tests them; a shared shaderc
  would be one more DLL every player needs before the game starts at all.
  Without it the build is OpenGL only and says so (the local mingw prefix has
  none; MSYS2 does, and `winpthread` has to go in statically too).
- The generator is duplicated, not shared, with the GL one: the two differ in
  every declaration. A change to one (a new `SHADER_OPT_*`) must go in both.

## The one rule: images are in OpenGL's row order

Every image is laid out the way GL lays out a framebuffer - row 0 is the
bottom. Then viewports, scissors, blit rectangles, `invert_y`, readbacks and
`gl_FragCoord` mean what they mean to the GL renderer and nothing is flipped.
The window is an image of the game's own ("fb 0", two of them: the other is
the **front buffer**, which `gfx_copy_framebuffer(use_back 0)` reads for the
blur and the trail) and it is blitted upside down into the swapchain at
present. Depth: the vertex shader writes `(z + w) / 2`, so the 0..1 buffer
holds what GL's -1..1 maps to, and fog still reads the untouched z/w.
`get_clip_parameters()` therefore says `z_is_from_0_to_1 = false`, like GL.

Framebuffer textures are sampled through a second view with alpha swizzled
to 1, because GL's are RGB8 and the blur draws blend by what they sample.

## Where the speed comes from

Measured on the perf harness's 80-simulant match (seeded, fixed step,
limiters off, 640x480, RX 580; `build/tmp/bench2.py`), game thread bound:

| | round 1 | round 2 (a noisier machine) |
|---|---|---|
| OpenGL | 433 fps | 419 fps |
| Vulkan, recording on the game's thread (`--vk-no-thread`) | 488 fps | 427 fps |
| Vulkan, worker thread (the default) | 494 fps (+14%) | 465 fps (+11%) |

The first working version, with a descriptor set per draw, only matched GL.
Both renderers leave the game's thread idle ~0.3 ms a frame that is not the
GPU (`vulkan:` wait is 0.000) - not yet explained, and not the renderer's.

- **Bindless textures.** Every sampled image and sampler is in one descriptor
  set bound once per command buffer; a draw names its two by index in push
  constants. Per-draw set allocation/write/bind was a third of the backend's
  cost. An image's slot is freed only through the graveyard (after the GPU is
  done with it). GLSL cannot pass a combined `sampler2D(tex, smp)` to a
  function, so the helpers take the `texture2D` and `sampler` apart (`TEX0`).
- **Vertices** go into the frame's ring on a multiple of the batch's stride so
  the chunk stays bound and a draw is `firstVertex`.
- **A worker thread records the command buffer.** The game's thread keeps all
  state (layouts, pipelines, the table) and writes packets (`rcCmd*`,
  `VK_MAIN_CB`); the worker replays them, submits and presents one frame
  behind. Texture uploads still record their own command buffer on the
  game's thread (a different pool from the one the worker holds).
  `--vk-no-thread` replays inline, for comparison.
- **Shader cache**: SPIR-V keyed by a hash of the GLSL
  (`$S/vulkan-shaders.bin`) and the driver's pipeline cache
  (`$S/vulkan-pipelines.bin`). Cold boot to a match: 22 stages compiled in
  146 ms of mid-frame hitches; warm: none.
- `--gfxstats N` adds a `vulkan:` line: GPU wait per frame (0.000 on the
  benchmark - the renderer never blocks), shaders compiled/cached, pipelines.

## Testing it headlessly

- **On the card, no window** (since 2026-09-28), the same way as GL:
  `SDL_VIDEODRIVER=offscreen ./pd.x86_64 --vulkan ...`. SDL 2's offscreen
  driver has no Vulkan at all (no `SDL_WINDOW_VULKAN`, no instance
  extensions, no surface, no loader), so when `SDL_GetCurrentVideoDriver()`
  is `offscreen` (`gfx_sdl_vulkan_headless()`) the window is a plain one, the
  renderer opens `libvulkan.so.1` itself, enables `VK_KHR_surface` +
  `VK_EXT_headless_surface` and makes the surface with
  `vkCreateHeadlessSurfaceEXT`. Everything after is the normal path: the
  swapchain takes the window's size (640x480; a headless surface has no
  currentExtent), presents go nowhere, and `screenshotRequest()` /
  `--screenshot-frame`, traces and F3 read back fb 0 exactly as before. The
  log says `Vulkan: headless surface for SDL's offscreen driver` and lists
  the devices; RADV and llvmpipe both offer the extension and the discrete
  RX 580 wins. Check `Vulkan: using AMD Radeon RX 580` and grep for
  `renderD128: Permission denied` before calling a run "on the card".
  Cost: a 1500-frame seeded match took 13% of one core against 92% for the
  Xvfb recipe below (game + Xvfb), same wall time. No other platform is
  touched: Windows never runs the offscreen driver.
- **Fallback, Xvfb**: RADV cannot present to Xvfb (no DRI3) and the renderer
  then picks llvmpipe; `MESA_VK_WSI_DEBUG=sw` makes RADV present through a
  CPU copy, so the RX 580 renders. Needed only when the test needs a window
  (xdotool input). `--vk-no-present` skips presenting altogether, for
  benchmarks.
- A start that fails is testable headlessly: `VK_ICD_FILENAMES=/nonexistent.json`
  with the offscreen driver fails for want of the headless extension, under
  Xvfb for want of a Vulkan window; both must log `VIDEO: Vulkan could not
  start, using OpenGL` and play on.
- Validation: `apt-get download vulkan-validationlayers`, `dpkg -x` it under
  `build/tmp/vvl`, point the json's `library_path` at the `.so`, then
  `VK_LAYER_PATH=... --debug-vk`; `VK_LAYER_VALIDATE_SYNC=1` adds
  synchronisation validation, which found the one hazard so far (a first-use
  clear followed by a blit into the same layout, with no barrier between).
- Compare against GL frame-exactly (`--rng-seed --fixed-step`, gdb at
  `'video.c'::frames` - `lvframenum` stops while a menu is up - or simply
  `--screenshot-frame N --exit-frame N+10` on both renderers); a clean
  result is ~95% of pixels identical and none more than 8 levels off.
  2026-09-28, offscreen on the card, seed 1: Skedar arena (`0x32 --mpsims 1`,
  frame 500) 98.0% identical, 3 pixels over 8; GE Plus mission `0x66`
  (frame 700) 99.0%, 42 pixels (0.014%) over 8, nearly all in three rows
  of a lit wall panel.
- A benchmark must not use a conditional breakpoint (it stops the game every
  frame): attach once, set `'gfx_sdl2.cpp'::target_fps = 0` and
  `g_TickRateDiv = 0` (the tick loop waits on real time even under
  `--fixed-step`), detach, and run to `--exit-frame`.

## Traps already met

- Two backends each defining `struct ShaderProgram` with `std::map`s of it is
  an ODR violation the linker resolves by picking one instantiation for both:
  everything in `gfx_vulkan.cpp` is in an anonymous namespace.
- A static `std::condition_variable` the worker waits on hangs `exit()` in its
  destructor: the mutex, the condition and the thread are leaked on purpose.
- Nothing may call the driver from an `atexit` handler: SDL may have unloaded
  it. The exit save writes the SPIR-V file only; the pipeline cache is saved
  when nothing new has compiled for 300 frames.
- `/tmp` is a 2.7 GB partition shared with every session: build with
  `TMPDIR=build/tmp`.
