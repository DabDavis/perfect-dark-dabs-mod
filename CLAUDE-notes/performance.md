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
- `PD_HOT_O2` (CMake option, off) compiles the hot decomp files at -O2. It
  changed the course of the seeded match after four seconds (float
  reordering, or the uninitialised locals the -Og comment warns about), so it
  is not measurable by the trace test and stays off.

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

The visual check is a third-person screenshot of the player:
`set g_Vars.players[0]->thirdperson = 1` and `->invincible = 1` over gdb,
then `screenshotRequest()`; the `gfx: tris ... of which smoothed N` stats
line (`--gfxstats 60`) says whether it engaged at all.
