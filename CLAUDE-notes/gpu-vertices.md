# Vertex work on the GPU (Video.GpuVertices, G_MESH_EXT)

## Digest

- **Meshes posed and transformed on the GPU** — "How a mesh is drawn": the
  XBLA release's and GoldenEye XBLA's meshes (`xblamesh.c`) are bracketed by
  `gSPMeshEXT()`; the renderer keeps a copy of each mesh on the GPU, reads each
  run of its lists once and keeps the run's triangles there too, and draws a
  run as one indexed draw whose vertex shader poses (palette), transforms,
  lights, texgens and fogs as `gfx_sp_load_vertex()` / `gfx_light_vertex()` /
  `gfx_emit_vertex()` would have. `Video.GpuVertices` (Video page, "GPU Vertex
  Shading", default on, live), `--cpu-vertices` turns it off for a run.
- **What it bought** — "Measured": the 80-simulant XBLA-look match went from
  210M to 11M game-thread instructions a frame (cycles 164M to 8.7M) on GL and
  206M to 10.5M on Vulkan; GE Plus Dam in HD 82M to 62M (its HD rooms are not
  meshes and still go through the CPU).
- **What still goes through the CPU and why** — the section of that name:
  rooms (N64, the release's, HD), the N64 look's models, rectangles, the sky,
  per-vertex-fog clipping, sealed seams, a door's trimmed copy, a crumpled
  prop, the title's logo passes.
- **Checking a change to it** — the section of that name: `--cpu-vertices` on
  the same binary is the reference; a clean result differs only at silhouette
  pixels; the replay test must stay identical; `--savedir` must be absolute.
- **Traps met** — the section of that name: Vulkan's winding is reckoned with
  the rows the other way up; a mesh's colour index is its vertex index and the
  renderer checks it; the validation layer crashes inside itself with the
  recording thread on (not ours, see there).

## How a mesh is drawn

**Game side (`port/src/xblamesh.c`).** `xblaMeshRenderNode()` used to pose a
skinned mesh into a frame copy of its vertices (`xblaMeshPose()`, up to three
bone transforms a vertex), bind the copy to segment 4 and call the mesh's
lists. With `gfxMeshGpuAvailable()` it builds only the palette instead
(`xblaMeshGpuPalette()`: the same matrices, from `xblaMeshPoseMatrices()`,
which `xblaMeshPose()` now calls too) and draws the bind pose under the bone's
matrix as floats (`mtxApplyGfxScale()`, no s15.16 rounding). The node's draws
are bracketed by `gSPMeshEXT(draw)` ... `gSPMeshEXT(NULL)`; `draw` names the
mesh's `struct gfxmesh` (kept in `xblameshbuilt.gpu`) and the frame's palette.
The reflection and Level Metal passes draw from `m->envst`, a copy of the
vertices made once with each reflecting vertex's atlas cell in s/t
(`xblaMeshEnvStatic()`), under a second `gfxmesh` (`envgpu`); their colours
carry only the amount in alpha (`xblaMeshEnvironmentGpu()`) and a skinned
mesh's normal is posed on the GPU from `gfxmesh.normals`. Still posed on the
CPU: a destroyed GoldenEye prop (crumpling goes in with the pose), the
title's logo passes, a GoldenEye prop's own map (`envown`). Rigid meshes take
the path whenever segment 4 names their own vertices (not a trimmed door or a
crumpled copy). Hit tests pose their own copy, so gameplay never depended on
the render pose: the replay test is identical.

**Renderer (`port/fast3d/gfx_pc.cpp`, "G_MESH_EXT").** Under a mesh draw a
`G_VTX` from the mesh's own vertices does no CPU work: the slots remember the
vertices' indices and triangles over them are gathered as indices. The first
time a run of geometry (`G_COL`/`G_VTX`/`G_TRI*` between two other commands)
is met it is read whole (`gfx_mesh_read_run()`) and its triangles kept on the
GPU with the mesh (`mesh_add_indices`); afterwards the interpreter skips the
run and issues one draw (`gfx_mesh_kept_run()`). A run is only kept when every
load names the mesh through one segment with `dest` 0 and the colour layout
below; the segments are checked again on every use. The draw's parameters
(`GFX_MESH_PARAMS`, laid out in `gfx_rendering_api.h`) are everything the CPU
would have read from the RSP and RDP state for every vertex of the run - a
list holds no state change inside a run, so load-time and draw-time state are
the same. Culling moves to the GPU (`gfx_mesh_cull()`), the far-plane
trivial reject to a clip distance (a corner past the far plane is -1, one
before it +1e6, so only a triangle with all three past it is lost). A run the
GPU cannot do is loaded on the CPU, posed there from the bind pose by the same
palette (`gfx_mesh_load_one()`), and a backend that refuses a draw gets the
same treatment (`gfx_mesh_draw_cpu()`).

**The vertex shader** is written once (`gfx_mesh_vs_main()`, in gfx_pc.cpp)
and each backend puts its own declarations in front. A new `SHADER_OPT_*` that
adds a vertex output has to go in it as well as in both backends' own
generators.

**GL (`gfx_opengl.cpp`)**: a mesh is a VBO and a VAO; the parameters and the
palette are uniform arrays (`uP[45]`, `uPal[192]`, within GL 3.0's 1024
components); colours and non-kept indices stream through per-frame buffers.
A mesh draw leaves its program, VAO, culling and `GL_CLIP_DISTANCE0` bound,
and `gl_mesh_leave()` restores the CPU path's state at every entry point that
draws. Needs desktop GL 3.0+; ES and GL 2.1 draw on the CPU.

**Vulkan (`gfx_vulkan.cpp`)**: a mesh is a vertex buffer and a kept index
buffer; set 1 (`vk_mesh_set_layout`, two dynamic storage buffers) reads the
parameters and palette out of the frame slot's mesh stream, which also holds
the colours and non-kept indices; mesh pipelines bake the cull mode in. Needs
`shaderClipDistance`. Two new packets, `VKP_BIND_MESH` and
`VKP_DRAW_INDEXED`.

## What still goes through the CPU and why

- **Rooms** - the N64 rooms, the release's (`xblastage.c`) and the HD levels'
  (`gebeanstage.c`). Their colours are a per-room palette (a vertex's colour
  byte indexes the G_COL table), not one per vertex, and dyntex rewrites some
  of their vertices in place. On GE Plus HD they are now most of the
  remaining frame (`gfx_run_dl` ~45% of Dam's game thread).
- **The N64 look's models**, rectangles (text, HUD), the sky
  (`G_NO_CLIPPING_EXT`), per-vertex fog's RSP clipping (`G_FOG_VERTEX_EXT`
  triangles in a run are fogged per vertex by the shader without the
  clipping, which differs only for triangles crossing the eye plane or the
  guard band), sealed seams (`G_SEAL_SEAMS_EXT`), a face's own texgen
  (`G_TEXGEN_FACE_EXT`, unused).

## Measured

`--rng-seed 12345 --fixed-step`, game thread only (`perf stat -t`),
instructions and cycles per level frame, RX 580, 2026-10-02.

| scene | before | after |
|---|---|---|
| 80-sim match 0x32, XBLA look, GL | 210.0M instr, 163.9M cyc | 11.0M instr, 8.7M cyc |
| same, Vulkan | 206.5M, 158.1M | 10.5M, 7.6M |
| GE Plus Dam (`--boot-ge-mission 0`), HD look, GL | 82.5M, 43.1M | 61.8M, 31.8M |
| solo 0x34, XBLA look, GL | 5.4M, 3.1M | 2.4M, 1.8M |

The steps on the match (GL): posing on the GPU 210M -> 53M; the reflections
and Level Metal on the GPU 53M -> 29M; kept runs 29M -> 11M. Before, the frame
was 55% `xblaMeshPose()` and 17% the interpreter; the match now runs at the
fixed-step cap. Draw calls fell from ~620 to ~414 a frame (a run is one draw).

## Checking a change to it

- The reference is the same binary with `--cpu-vertices`, frame-exact
  (`--rng-seed --fixed-step`, gdb `screenshotRequest()` at a level frame). A
  clean result differs only along silhouettes and in ±1 noise on linearly
  filtered surfaces: on the 80-sim match at frames 600/1500, 781 and 2947 of
  921600 pixels differ, a handful by more than 96. The CPU path itself is
  byte-identical to the build before the change.
- `tools/ci/replaytest.sh compare` against the build before: all eight cases
  identical (render-only change). A state hash of the XBLA-look match too.
- The XBLA look in a headless run: a save dir whose pd.ini has `[Mod]`
  `XblaMeshes=1` `XblaMeshPose=1` `XblaStages=1` `XblaMeshTextures=1`, and an
  **absolute** `--savedir` - a relative one is resolved against the base and
  mod directories (`fsFullPath()`), not the working directory, unless it
  starts with `./`, and the meshes silently stay off.
  `--gfxstats N` prints `gpu meshes: D draws, T tris (R refused to the cpu)`.

## Traps met

- **Vulkan's winding.** Images are in GL's row order and the viewport maps NDC
  the same way, but Vulkan's facing formula has the opposite sign, so GL's
  anticlockwise is Vulkan's clockwise: mesh pipelines use
  `VK_FRONT_FACE_CLOCKWISE` (`vk_create_pipeline()`).
- **A mesh's colour is the entry of its own vertex index.** The renderer
  feeds a mesh draw one colour per vertex from whatever array segment 5 names,
  which is only right because `xblaMeshWriteBatches()` gives each batch a
  `G_COL` at its first vertex and each vertex `colour = slot * 4`. Both are
  checked at the ends of every load; a list that breaks it draws on the CPU.
- **Kept runs are keyed by list address** for the mesh's lifetime: the lists
  are only rewritten at build (`xblaMeshBuildEnvironment()`'s no-ops). Every
  path that frees or rebuilds a mesh calls `gfxMeshForget()`; the renderer
  also notices a mesh rebuilt in place (its arrays' pointers) and lets copies
  idle for 1800 frames go.
- **The Vulkan validation layer crashes inside itself** (`unordered_map::at`
  or a segfault in the layer, at the first indexed draws) when the recording
  worker thread is on, on the 80-sim match - and the build from before this
  change does exactly the same (2026-10-02, `pd-base`), so it is the layer and
  the threading, not the meshes. With `--vk-no-thread` the same run is clean
  with the meshes on - no VUID, no sync hazard - and so is a small match
  threaded. Validate with `--vk-no-thread`.
