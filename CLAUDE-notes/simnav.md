# The simulants' navmesh and Mod.SimBrain

## Digest

- **The navmesh** — the section of that name: Recast/Detour v1.6.0 built per
  stage from the collision tiles (`port/src/simnavstage.c`, `simnav.cpp`),
  sized to a simulant's real radius, step, wall and duck heights; cached in
  `cache/navmesh/pd/`; off-mesh links for ladders, lifts, drops and jumps
  (`simnavlinks.cpp`); `Mod.SimNavDebug` / `--simnav-debug` draws it, the
  links and paths; `--simnav-build-all` builds every arena. GE Plus stages
  have no mesh.
- **Mod.SimBrain: stock or modern** — the section of that name: stock (the
  default) is bit-identical to a build without any of this; modern keeps
  stock's decisions and walks the simulant along a Detour path
  (`port/src/simbrain.c`, `simnavagents.cpp`), handing stock's movement only
  an aim, a speed and wait/duck/jump; latched at match start; falls back to
  stock's waypoints whenever the mesh cannot help.
- **Traps met** — the section of that name: stock's navigation must not look
  for walls on a mesh path; a lift's door will not open on a chr standing in
  it; drops down a lift shaft; an off-screen simulant moves four frames at a
  time; corners that flip between two floors; `circling` in `summary.py`
  means nothing for modern; the trace's `b` is the bot config, the probe's
  is `g_MpBotChrPtrs`.

## The navmesh (M1-M2, 2026-09-30)

PLANS/AI-REWORK.md (outside the tree) has the plan and each milestone's
numbers; the commits ae6f200ff, 0f191c80d and 708e46063 have the detail. In
short: every GEOFLAG_FLOOR1/2 tile is walkable, every GEOFLAG_WALL tile is
solid, a GEOFLAG_DIE floor is solid ground nobody may stand on; radius 20,
height 185 (90 kept on AIBOTDUCK/AIBOTCROUCH floors as areas DUCK/CROUCH),
climb 60, walls under 20 walked over. Props (doors, crates, lift cars, glass)
are not in it. The cache file's name carries a hash of the input and every
parameter; bump `SIMNAV_EXTRACT_VERSION` when the extraction or the links
change (it is 4 since M3 took drops down lift shafts out).

Links: a ladder (GEOFLAG_LADDER faces) joins foot and head both ways; a lift
joins each side of its shaft at one stop to each at the next; drops and jumps
are found by walking a simulated chr off every open mesh edge with the real
gravity and jump impulse. Jump links carry the lowest Jump Height setting
that makes them (`SIMNAV_FLAG_JUMP(n)`).

## Mod.SimBrain: stock or modern (M3, 2026-09-30)

`Mod.SimBrain = stock | modern` in pd.ini, the "Simulant AI" row on Dab's
Mod's Player page (Stock / Modern Movement), or `--simbrain modern` on the
command line (it overrides pd.ini and is not saved). Default stock, and it is
in no Settings Preset (so Enhancements Off does not touch it). It is read in
`simnavStageStart()` from `lvReset()`: a change during a match takes effect
at the next one, so switching mid-match is safe - nothing changes under a
running match. Only a Combat Simulator match (`g_Vars.normmplayerisrunning`)
with a mesh goes modern; co-op/counter-op buddies and GE Plus stay stock.

**Stock is bit-identical.** Every hook is behind `g_SimBrainModern`, which
only `simbrainStageStart()` sets; with stock no mesh is built (unless the
debug view is on, which never moved anyone). Check each change with
`tools/ci/replaytest.sh compare <main build> pd.x86_64` (both cases must say
`same`), and the modern side with `EXTRA="--simbrain modern"
tools/ci/replaytest.sh self pd.x86_64` (modern uses no random number at all,
so a seeded match replays).

**What modern does** (`port/src/simbrain.c` has the long form):

- bot.c still decides where to go and calls `chrGoToRoomPos()`. That is now
  a wrapper: `simbrainHoldsGoTo()` (a simulant crossing a link keeps
  crossing, and takes up the new goal after), stock's routing unchanged
  (`chrGoToRoomPosStock()`, so stock can take over at any moment), then
  `simbrainAfterGoTo()`, which asks the mesh for a path to the same end. A
  goal that moved under 150 (a chase re-plans every second) keeps its
  corridor (`moveTargetPosition`); otherwise the request is queued and at
  most 4 run a frame, oldest first. Where stock finds no route at all (the
  same nearest waypoint at both ends) and the mesh has a whole path, the
  simulant sets out anyway (`chrGoPosStartBare()`), where stock stood.
- Each `chrTickGoPos()` of a simulant goes to `simbrainTickGoPos()` first:
  the corridor (`dtPathCorridor`) is moved to the chr's feet, the corners
  ahead found every 4 ticks, and a `struct simcontrol` filled - the aim
  point, a speed share, wait, duck/crouch (from DUCK/CROUCH polygons ahead),
  jump. The aim goes to stock's `chrNavTickMain()`, which turns the simulant
  to it, opens doors and walks round props and chrs; the speed caps
  `aibot->speedmultforwards`; wait sets GOPOSFLAG_WAITING. The last stretch
  (`SIMBRAIN_TICK_FINAL`) is stock's own: straight to `endpos`, arriving by
  `chrGoPosIsArrivingAtPos()`, with `waypoints[curindex] == NULL` so
  `botCalculateMaxSpeed()`, `botCheckFetch()` and the item pick-up behave
  as on stock's last leg. While the first path is queued, stock's follower
  has the tick.
- Links: a drop is walked off; a jump is jumped with `botTryJump()` 30 short
  of the edge (waiting there if the jump's cooldown is not up); a ladder is
  walked into and the chr code climbs it; going down, the ladder's head is
  walked off - the mesh's drop link from the head is cheaper than the ladder
  link (area cost 2), so down-ladders are crossed as drops, which is the
  same move. A lift: wait a step back from the shaft, call it
  (`chrOpenDoor()` toward the car every half second), board when the car is
  at this floor and its door half open, ride with GOPOSFLAG_WAITING until
  the car is within 30 of the far end's floor, step off.
- Jump links are in a query only when `modCanChrJump()` and the Jump Height
  setting allow them, and only for a simulant at least 95% as fast as the
  MeatSim speed they were found at (the speed slider goes down to 10%).
- Area costs per unit of distance: duck 2, crouch 3, water 1.5, ladder 2,
  lift 10 (a lift with doors takes ten seconds and more), jump 1.5.
- Local avoidance: DetourCrowd's pieces, not `dtCrowd` itself (which moves
  its agents and crosses links on a timer): `dtObstacleAvoidanceQuery`
  against the six nearest chrs within 300 (players too) and
  `dtLocalBoundary` walls, only when there is a neighbour. The new heading
  bends the aim; its length caps the speed (never under 25%, so a doorway
  crowd keeps moving).
- Fast simulants: an off-screen simulant is ticked every fourth frame with
  four frames of movement (at 500% speed about 150 units a step). Within a
  stride (4.8 ticks of its run speed) of a corner the aim slides along the
  next leg and the speed drops in proportion; on a link it slows over the
  last stride to the far end.
- When it goes wrong: off the corridor (60 across, 200 up or down) asks for a
  new path; so does no headway (60 from where it was 2.5 s ago), which also
  blames the way from the polygon it is on to the next - that pair costs
  3000 more in every later query, and at a second complaint 1e6, as good as
  closed (`simnavAgentBlockAhead()`, needs `DT_VIRTUAL_QUERYFILTER` for the
  whole library). A link that fails twice is taken out of the mesh for the
  match (`setPolyFlags(ref, 0)`), and a lift link to a side of the shaft
  that has no door is taken out as soon as a path shows it. A goal with no
  floor under it (a pad on a crate, a run-away point 10000 off the map), no
  way there, a path that keeps getting lost (over 4 new paths in 2 s) or
  three stalls running hand the simulant to stock's follower for 4 s
  (`simbrainToStock()`); a goal the mesh failed is not asked again for 5 s.
  Stuck over an edge, it gets the same second of free stepping stock's watch
  gives (`aibot->navedgefree60`).
- State is in `g_SimBrain.sims[]` by bot config index; nothing is added to
  chrdata or aibot. Memory is Recast's own malloc, freed at `lvStop()`.

**Measuring it.** `tools/simstall/probe.sh`/`eval.sh` take `EXTRA` (and
`BIN`), so the 48-match eval runs both brains from one binary.
`PD_SIMBRAIN_TRACE=1` prints each simulant's state every half second (`SB`
lines: state, aim, corner, link, lift phase) and every hand-back, blame and
link taken out, plus link counts every 30 s; without it the log gets one line
of counts at the end of a match.

**Where it stands** (2026-09-30, feat/simnav-m3, not merged). The
simstall eval, stock and modern from one binary, 3-minute seeded matches
with the player invincible at the spawn, stalled samples (a simulant in
ACT_GOPOS within 40 of where it was 3 s before; waiting for a lift counts):

| | stock | modern |
|---|---|---|
| G5, Complex, Pipes, Skedar x 3 seeds, NormalSims | 198 | 179 |
| the same, 500% DarkSims | 788 | 326 |
| Warehouse, Area 52, Grid x 2 seeds, NormalSims | 820 | 283 |
| falls, NormalSims / DarkSims (all into Pipes' pit) | 3 / 5 | 1 / 12 |

Not the ~0 the plan hoped for. What is left is mostly single simulants
wedged in the level where stock wedges too (one on Complex's stair at
(-1060, 490, -1827), manground 250 over its ground, for 42 of the 179;
Warehouse's upper floor), lift waits, and the player standing in a
corridor. The fast simulants fall into Pipes' pit more than stock's (12
against 5); nothing traced ties it to a link, and they are also nearer the
player and fighting more. Watching (spectated, `PD_SIMBRAIN_TRACE`, 12 sims,
two minutes each): ladders are climbed on Pipes (15-21 a match), Warehouse
(14-16) and Area 52 (6-8), and come down by the head's drop link; Pipes'
lift is ridden both ways (5 down, 1-3 up) and Grid's up (4); drops 17-62 a
match; jumps with Jump Height 2: Ruins 9 (4 failed), Villa 2. Cost on the
80-sim seeded match: the simulants' ticks 2.16 M instructions a frame stock,
2.06 M modern, the navmesh code 0.58 M of it (half path queries).

## Traps met (M3)

- **Stock's navigation looks for walls on the way to its aim.**
  `chrNavCanSeeNextPos()` casts lines 19 either side of the chr; the mesh
  keeps a walker's centre 20 from a wall only to within its simplification
  error, so a path along a wall (Pipes' lower tunnel) read as blocked and the
  simulant wandered off obstacle-avoiding. While it walks to a mesh aim,
  `g_NavMeshAim` drops CDTYPE_BG from the sight and obstacle tests - until
  it has been blocked for 15 ticks (Skedar's leaning pillars), when stock's
  way round walls is wanted again.
- **Aim level across a link.** An aim down a drop or up a ladder is a line
  through the floor; across a link the aim is at the chr's own height.
- **A lift's door will not open on a chr standing in it.** Grid's did not
  for ten seconds; the wait point is 70 back from the link's start.
- **Drops down a lift shaft.** The car is not in the mesh, so its shaft is a
  hole and the drop finder linked every floor over it to the bottom; a
  simulant took one with the car there. `throughLiftShaft()` drops any drop
  or jump passing within 120 of a stop.
- **Lift sides behind walls.** The lift linker takes any floor within 300 of
  a stop as a side; with doors, only a side within 180 of a door is one.
- **Up a ladder, the foot first.** Walking at the head from wherever the
  link was taken caught the ladder by its side, where the climb meets the
  floor above rather than the hole. The link's two ends often share x and
  z, so once on the ladder the aim is the chr's own spot; a chr climbs by
  walking any way at all while `onladder`, which it still does, but one on
  Warehouse's ladder at (225, 729) hung at 106 until the link timed out and
  was taken out. Open.
- **Corners that flip.** Where the mesh merged a wall-flagged riser into the
  floor above (G5 at (-570, -60)), the corner alternated between the two
  floors' polygons each look, each "nearer" the last, and a
  nearer-the-corner watch never fired. Headway is measured as distance moved.
- **An off-screen fast simulant overshoots.** See the stride above; before
  it, 500% DarkSims ran back and forth past a zigzag of corners 20 apart
  (Complex) for the rest of a match.
- **Tick sampling.** Off-screen simulants are ticked every fourth frame, so
  a trace at `lvframe60 % 30 == 0` never sees them; trace once per half
  second per simulant instead.
- **The probes' indices.** `tools/simstall`'s `b` is the index into
  `g_MpBotChrPtrs`; the trace's is the bot config index. Match by position.
  `summary.py`'s `circling` reads stock's `waypoints[curindex]`, which a
  modern simulant does not advance: it means nothing for modern.
- **A relative `--savedir`** is taken from `data/`, not the working
  directory, and `data/` here is a link to the main checkout's.
