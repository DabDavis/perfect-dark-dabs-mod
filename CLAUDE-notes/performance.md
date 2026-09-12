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
under test: `[Mod]` `GuardsAlerted=1`, `AlertedGuards=80`,
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

## Where the frame went (main thread, 160 chrs)

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
| before | 24.8M |
| after | 19.7M |

- `port/fast3d/*.cpp` at `-O2 -finline-functions` (CMakeLists.txt; the
  decomp stays at -Og): -25% on the main thread by itself.
- The combiner inputs of `gfx_sp_tri_emit` are resolved once when the
  primitive/environment colour or combiner changes, not switched per vertex.
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

## Run to a level frame, not for a wall-clock span

`--exit-frame N` quits when `lvframenum` reaches N (lv.c), and `tools/perf/perfframes.sh` counts a
whole run with `perf stat`, so two binaries are compared over the very same
frames however fast each one renders them. `perfrun.sh`'s window of seconds
answers a different question and gives each binary a different span of
match. Attach the counter to the game's own thread and after the driver's
threads exist: `perf stat -t $pid --no-inherit` started four seconds in.
Counting the process counts Mesa's eight `llvmpipe` threads too, which on
this box is 25 times the number you want and looks plausible until you
compare it with the same binary's `perfrun.sh` figure.

## Model LOD was decided in two places, and does nothing in a match

`modelUpdateDistanceRelations` (model.c) is not the only place a distance
node is decided: `modelasm_c.c` has a second copy of the same rule for
`MODELNODETYPE_DISTANCE`, and it did not consult the option at all. Most
models come through that one, so **turning Model LOD off in the menu did
nothing to them**. Both paths now ask `modIsModelLodOn()`.

None of which changes a frame. In the seeded match, and in Crash Site, Air
Base and Villa, **Model LOD on and off render the same triangle and vertex
counts to the digit**, before the change and after it, with the setting on
or off. 177 distance nodes in the match are past their threshold, so the
flags do flip; the geometry either side of them is evidently the same. The
change is worth keeping because the option now means what it says in both
code paths, but do not expect frames from it, and do not repeat the estimate
that the near meshes of a distant crowd are costing anything.

**The offscreen driver renders 640x480 whatever pd.ini says.** `videoGetHeight()`
returns 480 in a `SDL_VIDEODRIVER=offscreen` run with `DefaultHeight=1080`
in the ini, so a headless measurement is a 480-line measurement of whatever
the desktop it stands in for would draw. A screenshot taken through gdb
comes out at the ini's size.

## The posed skin's third bone, and the batch cap that does not duplicate

A skinned XBLA mesh stores three bone influences per vertex, and
`xblaMeshPose()` applied all three, clamping each palette index as it went,
because the file's fourth byte is not a count of them (it runs 1 to 6
against three bones and says the same thing for every vertex of a draw).
Most vertices are not three: a vertex with fewer repeats a bone in the
bytes it does not need, and a third weight written as the remainder of the
other two is often zero outright. Folded down once at build time
(`xblaMeshCompactSkin()`, which clamps the index, adds a repeat's weights
together, drops the zero terms and counts what is left into that fourth
byte), **44% of the release's skinned vertices come down to one bone and
34% to two** - 41% fewer matrix transforms per posed frame.

Measured on the 80-sim match at stage 0x32 over 2400 fixed-step frames,
three runs each: **83.3 → 72.8 M instructions/frame on the game's thread,
-12.6%**, spread under 0.2% between runs. Same geometry to the digit (257
draws, 46253 tris, 134427 verts, 15523 clipped, 123 culled) and the
screenshots are byte-identical at a fixed level frame on five replays -
Skedar at 8 and 32 sims, MP Villa and Warehouse at 24, and the Villa solo
mission.

Only weights that are *exactly* zero are dropped. The remainder lands a
hair off zero rather than on it about 15% of the time, and dropping those
as well takes another tenth of the transforms; it is not worth it, because
a term that small still moves the rounded vertex by one step of the write
wherever it falls either side of a half, and that costs the byte-exact
comparison above. Merging a repeat is safe on the same measure: the two
weights are added before the transform instead of after it, which is the
same sum in a different order, against a write that rounds to a sixteenth
of a unit.

**Do not repeat the estimate that the 25-vertex batch cap duplicates a
character's vertices about 2.3x.** It does not. `XBLAMESH_BATCH` is 25 and
a vertex is only shared within the batch being built, so a vertex on a seam
between batches is emitted twice - but the release's meshes are ordered so
that this almost never happens: measured across the three skinned meshes of
an MP Villa match, **11210 emitted vertices against 11114 distinct file
vertices, 1.01x**, each mesh exactly 1.01x. Posing the unique set and
scattering was written and works (byte-exact), and was thrown away: it
saves 0.9% of the transforms and costs 35% more skinning memory, because
the index back from each emitted vertex is four bytes against the 28 the
sharing saves. The 2.3x figure came from reading "CcarringtonZ is 4792
vertices" (the file count of the one *named* node's mesh) against "a single
character here is eleven thousand vertices" (a whole character, every node)
as if they were the same mesh counted two ways.

## The XBLA texture decode on the render path is a hitch, not a cost

Measured 2026-09-12 on the seeded 80-sim match at stage 0x32 over 2400
fixed-step frames, XBLA meshes/stages/textures on. **45 decodes in the whole
run** - 0.019 a frame - and the renderer's texture cache sat at **76 of 1024
entries with zero evictions**. The cliff described above this section could
not be provoked: `--gfxtexcache 64` still gave zero evictions, and a 500-body
run (`Bodies=500 BodiesDrawn=500`, 6000 frames) reached 94 of 1024. So there
is no per-frame decode cost to remove, and **the claim that a body-heavy room
puts this cache under pressure does not hold** for the XBLA path.

What a decode costs, timed around its three stages: **read 1.9 ms, LZX 189 ms,
untile 56.7 ms across all 45** - the STFS seek is 0.8% of it and LZX is 76%,
so there is nothing to win by touching the file handling. Median decode 4 ms,
worst **31.8 ms** for a 1024x1024 record, of which 25 ms is the inflate.

The distribution is what matters: **34 of the 45 land in the first second**
(level load), and **10 land mid-play**, spread over 30 s, 62.5 ms in total,
one of them the 30 ms one. That is a two-frame stall on the render thread the
first time a new character or effect is seen - a hitch worth roughly 0.15% of
wall time, not a throughput item.

**A kept store for these is a bad trade.** 43 distinct records decode in a
match and only two of them decode twice, so keeping every decoded record
costs **34 MB to save 4.6 ms a match**. If the stall is ever worth fixing,
the way is to prefetch on bind through the texpack worker (the mesh builder
knows every record it binds) with the current synchronous decode left as the
fallback, so the worst case stays what it is today and no model ever draws as
its white stand-in. Note that the worker cannot simply take xblatex.c's lock
to do it: the mesh builder on the game thread wants that lock for every
material, so a 30 ms inflate under it just moves the stall to the other
thread. It needs a second STFS stream handle.

## A model's release slot was inflated once per chr that shared the model

Every model load is matched against the release's copy of the same file
(`xblaMeshMatchModel`), and the match reads nothing out of that copy but a
node type and an id per node. It inflated the whole file to get them, and it
did so once per *load*, not once per file: eighty simulants sharing one body
made the game load file 412 and read slot 411 **eighty-one times in a row**.

Measured on the seeded 80-sim match at stage 0x32 over 2400 frames: 98 slot
reads through the matcher, 35.4 ms in total, of which **file 412 is 81 reads
and 30.7 ms, all inside a 46 ms window at the level load**. Mid-play the
whole match spends 3.0 ms in here across 9 reads, worst 0.87 ms - so this is
a level-load cost and nothing else, and the earlier guess that models reload
"all through a stage on weapon switches and spawns" is not what the reads
look like.

The repeats come in a run, so **one** kept slot collapses them: 80 of the 98
reads become a memcpy, 30 of the 35 ms go, and a second slot asked for in
between costs only the re-read it always did. Counted with the memo in:
80 hits, 32 inflates. No cache, no budget, no eviction rule - the repeats are
consecutive and a cache of one is the whole of the win. Frame-exact against
HEAD on four seeded replays.
