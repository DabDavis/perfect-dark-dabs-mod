# F3 20260925-224436 - blood decals hanging over a surface edge

Branch `fix/f3-blood-decal-edges` (worktree /home/sdg/wt/f3blood), based on dabs-mod 1fc1832d8.
Not merged, pushed or deployed.

## Status

Done: **Clip Decals at Edges** (Dab's Mod Options > Display, `Mod.DecalEdgeClip`).
Built, verified headless on the RX 580 in GL and Vulkan, committed as 67e20a948
(this file in the commit after it).

The report says "G5 Building 0x1d", but stage 0x1d is Chicago (STAGE_CHICAGO,
constants.h). The tester's camera (-1402.9 57.5 -727.5), rooms 34/37, is on
Chicago's concrete ledge by the canal, and that is where it was reproduced.

## Approach

CPU, once per mark, renderer-agnostic (port/src/wallhitclip.c):

- A wall hit (blood splat, bullet hole, scorch - `wallhitCreateWith20Args()`)
  is a quad laid on the hit triangle's plane, the size of the mark whatever
  is under it. With the setting on, `wallhitsTick()` clips each room mark
  once (at most 8 a tick): the rooms whose bbox the quad touches are walked
  through their vertex batches exactly as `bgTestHitInRoom()` walks them,
  so the ROM's rooms, the XBLA release's (xblastage.c) and GoldenEye's,
  converted or HD (gebeanstage.c), all work the same way.
- A triangle belongs to the mark's surface when it faces within ~37 degrees
  of the quad and every point of it under the quad is within
  `1.5 + 0.03 * size` units of the quad's plane (tessellated floors and
  gentle terrain keep the whole mark; a ledge's face, the floor below, a
  wall at a corner do not).
- The kept area is the union of those triangles clipped to the quad, built
  as disjoint convex pieces (each triangle less every earlier one), so
  overlapping coplanar geometry does not draw a translucent mark twice.
  Worked in the quad's own (a, b) coordinates, so texture coordinates and
  corner colours follow from position, and an expanding splat (drawn scaled
  about its centre by `wallhitsTick()`) is the stored clip cut to the smaller
  square each frame (`wallhitClipSetScale()`).
- A mark the triangles cover whole (most of them) keeps no clip and draws the
  stock quad. A mark whose neighbouring room is unloaded waits (retry every
  30 ticks) instead of being cut where that room's floor continues. Anything
  that cannot be clipped (no triangle found, >256 triangles, >32 pieces)
  draws as stock - a mark never vanishes.
- `wallhitRenderOpaBgHits/XluBgHits()` draw the pieces (per piece one
  gSPVertex + gSPTri4 fans, from gfxAllocateVertices) in place of the quad;
  same colours, same winding.
- Props/doors (`objprop`) are left alone; chr bruises untouched.
- `xblaStageSwitched()` resets every clip (rooms reloaded from the other copy).
- F3 trace gains `wallhits: N in use of M; clip decals at edges 0/1: clipped, whole, not tried`.
- The hit-texture guard d1250ec97 is untouched (no hit-path changes).

Chosen over a stencil/depth renderer technique: no GL/Vulkan state or
pipeline changes, one-off cost, and it follows whichever room geometry is
actually drawn.

## Setting default: OFF

Default off, Vanilla and Ghost Trials off, Dab's Settings on. Reason:
modoptions.c/vanilla-defaults-presets say "fixes on, additions off", but the
visual "fix an N64 quirk" options are all handled as additions: Glare
Clipping (the closest analogue - N64 glare spilling over nearer geometry) is
off by default and on only in Dab's Settings, and the same tester's Glass
See-Through (bdc053f3d) was done the same way. This is the tester calling it
"original N64 behaviour", so it follows that pattern. Flip
`decalclip` in src/game/modoptions.c if the user wants it on for everyone.

## Verification (RX 580, offscreen GL / Xvfb+MESA_VK_WSI_DEBUG=sw Vulkan, --fixed-step --rng-seed 1)

Rig: ~/wt/f3blood-rig (run.sh, runvk.sh, blood.py: teleport with
--spectate, cast a grid of rays with `bgTestHitInRoom()`, make blood with
`wallhitCreateWith20Args()` via gdb, screenshot). Pictures in ~/wt/f3blood-pics.

- Chicago 0x1d, tester's spot, XBLA look: ledge-top blood hung over the
  front edge; clipped at the edge (s01crop.png). N64 look same (cn01crop.png).
  Vulkan same (cv01crop.png).
- G5 Building 0x1e: blood hanging off a pillar's edge into the air cut at the
  edge (g01crop.png); expanding splats (timermax 120) stay clipped through
  the whole spread (ex1strip.png).
- GE Plus Dam 0x15, HD (Bean) rooms: blood over a barrier's top edge cut
  (damccmp.png, 23 of 25 clipped); converted N64-look rooms same (dncmp.png).
  Marks on Dam's flat HD ground: 1 of 15 clipped, no visible change.
- Setting off vs base binary (1fc1832d8), G5 Building with 25 marks:
  pixel-identical except the HUD weapon-name box, whose fade runs on wall
  time (gdb pauses differ); off vs off identical. Chicago differs run to run
  anyway (rain).

## Perf

- Clip compute (temporary timing build, not committed): mean 8.4 us / max
  19.9 us per mark on Chicago (XBLA rooms), mean 20 us / max 35 us on Dam's HD
  rooms; capped at 8 marks a tick.
- Drawing 49 marks (29 clipped) on Chicago: 83 draws either way, +87 tris,
  +91 display-list commands, +7 KB vertex pool.
- Seeded 80-sim match (0x32, 2400 frames): the sims make no wall hits at all,
  so on and off are the same work (58.9/61.2/60.0/60.0 M instr/frame across
  runs is run-to-run noise); the setting costs nothing without marks.

## Open

- Marks on props/doors (tables that are objects, crates) are not clipped.
- A mark next to a room that never loads stays unclipped (stock).
- Only checked visually in the gdb rig, not in a live firefight; the tester
  should confirm on Chicago's ledge.
