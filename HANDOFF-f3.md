# F3 LOD / torso pass (2026-09-26), branch fix/f3-xbla-lod-torso

Reports (/home/sdg/wt/f3-0926/), both Parabolee (savantique), Extraction 0x22,
Windows ef6bffc, XBLA meshes + texture pack, night vision:
- 20260925-235522-460d61f2 "low polygon models"
- 20260926-000033-e04d2b30 "torso of enemy combatants failing to render at all within a certain range"

## Status
Fixed, verified, committed (f29c2c3e5). Not merged, pushed or deployed.

## Cause (one cause for both)
- `mainInit()` (port/src/pdmain.c) disables the distance checks at boot, so a
  normal session never draws a far LOD. But geintro.c, gewatch.c and
  gegadgets.c call `modelSetDistanceChecksDisabled(true)` around their own
  models and then `(false)`, turning LOD **on** for the rest of the session.
  The tester's previous reports in that session were GE Plus Facility (0x7a).
  From then on the Model LOD option (default on) had something to act on.
- CddshockZ (body 0x5e) has 34 distance nodes, pairs going far at 530, 600
  and 670 units (5300/6000/6700 x scale 0.1). Past 670 the whole N64 far LOD
  draws: "low polygon models" (trace 1: chr 1 at 851).
- Between ~530 and 670 in the XBLA look: the pair carrying the release's mesh
  has gone far, so the mesh is not drawn, while the torso's near list is
  still chosen and, being covered by the mesh (`xblaMeshIsCovered()`), still
  suppressed: no torso (trace 2: chrs 3/4 at 603/638 drew but their mesh was
  "posed for another model"). The N64 look just shows a mixed-LOD body.

## Fix (f29c2c3e5)
- `modelDistanceIsFullDetail()` (src/lib/model.c, always true on the port);
  `modelUpdateDistanceRelations` and the modelasm_c.c DISTANCE case both use
  it, so the GE code's flag flips no longer matter. Pose capture unchanged.
- Model LOD option retired (meaningless now): menu row + handler
  (optionsmenu.c), preset column, `Mod.ModelLod` pd.ini key (main.c; an old
  line is dropped on load), `g_ModOptions.modellod`, `modIsModelLodOn()`,
  README row. CLAUDE-notes/performance.md section rewritten (the 2026-09-09
  "LOD on/off same triangles" finding was the boot-time disable).
- GE files left alone (their calls are now harmless).

## Verification (real GPU, offscreen, --fixed-step --rng-seed 1)
Rig: /home/sdg/wt/f3lod-rig (`views.sh`, `lodview.py`, `montage.py`),
run dir /home/sdg/wt/f3lod-run, pictures /home/sdg/wt/f3lod-pics.
Tester's camera (-1793,-8441,99, theta 3.7, room 49), doors opened, night
vision, guard chr 1 moved (chrMoveToPos) to 450/560/600/640/700/760 units.
`LODON=1` sets `g_ModelDistanceDisabled = 0` = the post-GE-Plus state.
- sweep-xbla.png: baseline fresh session = full mesh at all distances;
  baseline LODON = hollow torso at 560/600, N64 low-poly at 640+ (the
  reports reproduced); fix + LODON = full mesh at all distances.
- sweep-n64.png: baseline LODON = low-poly/mixed; fix + LODON = full.
- Pixel diff, every shot, both looks: fix+LODON == baseline fresh session
  (bbox None outside the fps counter). The first-person gun is unchanged.

## Perf (tools/perf-style seeded 80-sim match 0x32, level frames 300-2400, main thread instructions)
Rig copy: f3lod-rig/perfwin.sh (+perfgdb.py: counter attached at frame 300).
- Fresh session: base 21.199/21.197 vs fix 21.196/21.195 M/frame (N64),
  80.907/80.901 vs 80.901 (XBLA); geometry identical. No cost.
- vs the LOD-on state: N64 18.36 -> 21.20 M (+15%), 4180 -> 5461 tris;
  XBLA 70.94 -> 80.90 M (+14%), 21873 -> 24923 tris.
- XBLA repeats under the machine's load (load avg ~19) diverged (gfx sample
  count 87-148 instead of 40); those runs were discarded.

## Open
- Nothing for this item. Memory note model-lod-two-places.md is out of date
  (says Model LOD on/off changes nothing; the option is gone now).
- The GE calls `modelSetDistanceChecksDisabled(false)` could save/restore
  instead; harmless on the port now, left as is.
