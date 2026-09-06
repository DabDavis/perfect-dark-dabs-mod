# Performance: measuring a crowded match, and what was found

A Combat Simulator match with 80 simulants and 80 alerted guards is the
fork's heaviest ordinary load. This is how it was measured on 2026-09-06 and
what the frame was spending its time on; read it before touching the renderer
or the AI for speed, because the naive measurement lied twice.

## Measuring

`tools/perf/perfrun.sh BIN LABEL [SECONDS] [STAGE] [SIMS]` boots that match on
the real GPU with no window (`SDL_VIDEODRIVER=offscreen`, see recording.md),
and reports CPU per rendered frame for every thread, `perf stat` instruction
counts for the main thread and Mesa's `gl0` thread, and the geometry the
frame carried. `tools/perf/perfprof.sh BIN LABEL` samples the main thread
with `perf record` for 20s and prints the flat and inclusive profiles. Both
expect `$PDPERF_DIR` (default `/tmp/pdperf`) to hold `pdsave/` seeded as in
headless-game-driving (eeprom.bin, mpsetups.bin, a pd.ini with the settings
under test: `[Mod]` `ModelSmoothing=3`, `GuardsAlerted=1`, `AlertedGuards=80`,
`GuardSpawnSpeed=50`, `StartArmedFor=1`, and `[Video]` `VSync=0`
`FramerateLimit=240` `DefaultFullscreen=0` `DefaultWidth=1920`
`DefaultHeight=1080`). The game rewrites pd.ini on exit and writes `[Mod]`
twice; a key appended at the end of the file lands in the last section and
is ignored, so edit the key in place.

**Two flags make a run repeatable.** `--rng-seed N` seeds the RNG and
`--fixed-step` makes every frame exactly one tick whatever the clock says
(stock derives the tick count from wall time, so two runs of the same seed
diverge on the first hitch). With both, two binaries play the same match tick
for tick and their per-frame geometry lines match exactly, which is how a
change is shown to be behaviour-preserving: `diff` the `gfx: N draws` lines of
the two logs. That is also why the per-frame numbers between binaries are
comparable at all. A fixed-step run is not capped at 60: it renders as fast
as it can (about 120 on this box) and the sim runs that much faster.

**Compare instructions per frame, not milliseconds.** The governor is
schedutil; a 40s run's ms/frame varied 30% between identical binaries until
it was pinned to `performance` (and put back after). Instructions per frame
were stable to 1%.

**Scene variance dwarfs most changes.** Without the seed, the same 80-sim
match measured 3.6ms and 14.9ms a frame on consecutive runs, because the
fight wandered into view or not. Never judge a change from one unseeded run.

**A match ends.** The first profile sampled the "Save Player" dialog at 60fps
and 250 triangles. `--endless` keeps it going; `--spectate` keeps the human
player alive.

**gdb's `size()` on a container in this binary returns garbage** (it printed
0 for a map holding 30,328 entries). Read `_M_h._M_element_count` instead.

## Where the frame went (main thread, 160 chrs, Heavy models)

- Display list interpreter (`gfx_run_dl`, `gfx_sp_tri_emit`,
  `gfx_sp_load_vertex`): about half. Every vertex is transformed and every
  triangle written to the vertex buffer in software.
- Bot and chr ticks: about 40%. Collision (`cdExam*`, `cdCollectGeo*`),
  navigation, AI scripts, model animation (`modelUpdateChrInfo`).
- `botChooseGeneralTarget` alone was 7.4%: a selection sort of all chrs by
  distance, per bot, per tick - chrs squared times chrs. Now an insertion
  sort seeded from last tick's order, same result, under 1%.
- Mesa's `gl0` and `gdrv0` threads: about 2ms a frame on their own cores,
  from ~500 draw calls a frame; `tex 2000 (92 distinct)` in the stats means
  the same textures rebound over and over (gfx_pc.cpp explains). Not
  addressed; sorting the opaque pass by texture is the next big one.

## What changed, and by how much (same seeded match)

| | instructions/frame, main thread |
| - | - |
| before, Increase Poly Models off | 24.8M |
| after, off | 19.7M |
| after, Heavy, whole level on every triangle | 146M (23ms) |
| after, Heavy, per-edge pixel level | 25.4M (5.3ms) |

- `port/fast3d/*.cpp` at `-O2 -finline-functions` (CMakeLists.txt; the
  decomp stays at -Og): -25% on the main thread by itself.
- The combiner inputs of `gfx_sp_tri_emit` are resolved once when the
  primitive/environment colour or combiner changes, not switched per vertex.
- A triangle's facing is tested once in `gfx_sp_tri1`, not again per patch.
- `PD_HOT_O2` (CMake option, on since 2026-09-06) compiles the hot decomp
  files at -O2: 25.4M to 24.6M instructions a frame, about 3%. It first
  changed the course of the seeded match after four seconds; the section
  below is what that was.

## The interpreter's vertex load and emit paths (2026-09-06, later)

With the decomp at -O2 the renderer was 60% of the main thread, half of
that in `gfx_sp_load_vertex` (transform, clip flags) and `gfx_sp_tri_emit`
(writing the vertex buffer). What changed, same seeded match, main thread:

| | instructions/frame | cycles/frame |
| - | - | - |
| before | 24.6M | 16.1M |
| transform and cull test as four-lane vectors, colour bytes through a table, write pointer | 23.0M | 15.7M |
| plus the vertex layout as a per-batch template | 22.8M | 15.6M |
| plus `gfx_sp_load_vertex` inlined into the vertex loop | 22.5M | 15.5M |

- The four dot products of the transform are one expression over GCC's
  `vector_size(16)` type (`v4f` in gfx_pc.cpp), which is SSE on x86 and
  NEON on ARM with no intrinsics. Each lane adds in the scalar order, so the
  positions are bit-identical; the same for the cull test's four divisions.
- `c / 255.0f` on a colour byte is a 256-entry table filled with that
  division once, so the shader gets the same floats without the divide.
- The per-vertex layout (position, texcoords, clamps, fog, grayscale, the
  combiner inputs) was rebuilt with a switch per input per vertex. It is now
  a template per batch: the constant floats copied whole (32 floats,
  straight-line stores; buf_vbo has room for a full-size vertex at every
  position), then the few slots that come from the vertex. Rebuilt when the
  batch state or the resolved inputs change, or the fog/grayscale colour.
- What is left in the load is the aspect-ratio divide, the clip-flag
  branches and the stores; in the emit, the UV multiply-adds and the slot
  writes. Neither is worth more without changing the floats that reach the
  GPU (the divide could be a multiply, but not on the same bits).
- `gfx_sp_tri_smooth_level` took three `sqrtf` and three `ceilf` per model
  triangle (the game's own `ceilf`, a call) to pick the patch level. It now
  compares the squared edge length with the thresholds and does the six
  screen divisions as two vector ones: 22.5M to 22.3M instructions. An edge
  within a rounding of a threshold can take the other level; the match
  replay and the frame-2400 capture showed none.

**The check for a renderer change is a pixel diff, not the replay alone.**
The replay proves the game took the same course and the counters match;
it says nothing about the floats in the vertex buffer. A frame-exact
screenshot from each binary settles that:

```sh
gdb -batch -ex "break videoEndFrame if g_Vars.lvframenum == 2400" -ex run \
    -ex "call (void)screenshotRequest()" -ex delete \
    -ex "break videoEndFrame if g_Vars.lvframenum == 2410" -ex continue -ex kill \
    --args ./pd.x86_64 <the perfrun.sh arguments>
```

The PNG lands in `screenshots/` next to the *binary* (fsChooseOutputDir's
first root is `$E`), not the save dir. Two captures differ only inside the
frame-rate counter's box at the top left, which is also the ±1 draw in the
stats; `PIL.ImageChops.difference(...).getbbox()` shows where.

## Why the decomp at -O2 played a different game, and how it was bisected

Two causes, neither of them float reordering (GCC does not reorder without
fast-math, and this target has no FMA to contract). Both are fixed.

**The game defines `sinf` and `cosf`** (modelasm_c.c, the N64 table code) and
at -O2 GCC fuses a `sinf(x)` and `cosf(x)` of the same `x` into one
`sincosf(x)` call, which resolved to glibc's and differs in the last bits.
`nm -u` of the -Og and -O2 objects showed the new `sincosf` reference in
exactly the files that diverged (bot.c, chraction.c, model.c, chr.c). The fix
is a `sincosf` in modelasm_c.c built on the game's `sinf`. Any libm name the
game defines itself (also `atan2f`, `acosf`, `asinf`, `floorf`, `ceilf`) is
exposed to this kind of substitution; diffing the objects' undefined symbols
is the first check when a file diverges at a new optimisation level.

**`chrTickGoPos` unpacked `pad2` without `PADFIELD_FLAGS` and then tested
`pad2.flags`**, reading whatever the stack held. That garbage depends on the
frame layout of every function that ran before it in the tick, so *any*
change to *any* caller - a tail call instead of a call in a five-line
function - changed a guard's route. It only showed on a solo mission (Attack
Ship, 40 s in, with the mod's 80 alerted guards); the match never took that
branch. Valgrind on the solo run found it in one line
(`Conditional jump ... chraction.c:13775`, origin a stack allocation in
`chrTickGoPos`). `cdBlockExcludesBlockLaterally` had an uninitialised `sum2`
of the same shape, fixed at the same time although the replay never showed it.

**With both fixed, -Og and -O2 replay the match (17,000 frames) and the solo
mission (14,000 frames) identically**, and the whole decomp at -O2
(`PD_DECOMP_O2`, off) measured the same 24.6M instructions a frame as the
twelve hot files, so it stays off. -O3 on the hot files was 24.3M.

The method, for the next time a build diverges:

1. Per-file: compile each suspect file at the new flags into a scratch
   directory (take the command from `build/compile_commands.json`), copy the
   object over the one in `build/CMakeFiles/pd.dir/`, and link by running
   `build/CMakeFiles/pd.dir/link.txt` directly. `make` does not relink after
   an object is swapped by hand; the first hour of the bisection compared one
   binary with itself.
2. Run each binary on the seeded fixed-step match with `--gfxstats 1` and
   diff the vertex counts of the `gfx: N draws` lines. Draw counts carry a
   ±1 HUD element that differs between identical runs; vertex counts do not.
   Ten runs in parallel under `SDL_VIDEODRIVER=offscreen` are fine; each is
   a 22 MB binary, so mind `/tmp`.
3. Within a file: insert `#pragma GCC optimize ("O2")` before a range of
   function signatures and `#pragma GCC optimize ("Og")` after it, compile,
   link, run. The pragma keeps the command line's `-fno-strict-aliasing`
   (checked). Halve the range each round; expect several disjoint regions to
   diverge at the same frame when the cause is a garbage read downstream,
   because every caller's frame layout feeds it.
4. When single functions whose -O2 code is semantically identical (a tail
   call, a smaller frame) still diverge, stop bisecting and run valgrind on
   the diverging stage: `valgrind --track-origins=yes` takes about 13
   minutes to reach frame 5,000 of a solo mission here, and names the line.
   Filter out `texShrink*`/`gfx_texscale` noise (texture decompression reads
   past its input; harmless, not addressed).

A solo mission (`--boot-stage 0x34`, no `--mpsims`) is a second replay
worth running for anything that touches chr code; the match alone missed
the `pad2.flags` read.

## Increase Poly Models never ran on a character until 2026-09-06

The smoothing gate required a *lit* vertex (`G_LIGHTING` on and a normal in
the colour table). The game lights nothing on the RSP but the title logo and
glass shards: characters, weapons and props carry colours, not normals, so
the counters showed zero triangles smoothed in any match or cutscene, and the
tester reported square shoulders. The mesh pass already computes the surface
normals from the geometry, so the gate now only needs those; an unlit patch
blends the corners' colours. Once it ran, Heavy on every triangle was a slide
show (above), hence the per-edge pixel level, and the seams that appeared
between bent patches and flat neighbours - joint triangles the renderer
cannot bend, creases, open edges at the hands - are sealed by the mesh pass
marking those edges straight and both sides drawing them as lines.

**The surface is Modified Butterfly subdivision, not PN patches, since
2026-09-06.** A PN patch bends each triangle on its own and only meets its
neighbour along the edge, which read as quilting on a back. The mesh pass
(`meshSubdivide` in modelsmooth.c) now cuts the node's mesh along its
straight edges, subdivides twice with Butterfly (interpolating, so the
corners stay put and joints still seal), and hands the renderer the 15 grid
points over each triangle; the renderer draws those at two or four pieces an
edge (three rounds up) and falls back to the PN patch only when a loaded
corner does not match the precomputed one (a vertex copy the game rewrote:
`gfx: ... patches: N subdivided, M bent alone` in the stats). The pass is
load-time only: 279 ms for the 107 models and 30,779 triangles an 80-simulant
match loads, 2.6 ms a model, measured with `g_ModelSmoothUs`. That is why
there is no on-disk cache of it: a cache would save a quarter of a second a
stage and nothing per frame, and the per-frame cost is transforming the
points, which precomputation cannot remove. Model Depth (a displacement from
texture brightness) was prototyped the same day and dropped: with no lights
to shade it, it only moves silhouettes, invisibly at safe strengths and as
noise and cracks at visible ones.

The visual check is a third-person screenshot of the player:
`set g_Vars.players[0]->thirdperson = 1` and `->invincible = 1` over gdb,
then `screenshotRequest()`; the `gfx: tris ... of which smoothed N` stats
line (`--gfxstats 60`) says whether it engaged at all.
