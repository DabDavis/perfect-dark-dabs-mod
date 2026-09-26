# F3 handoff: Facility door console "z-fighting" (branch fix/f3-facility-console-square)

Worktree /home/sdg/wt/f3faccon, based on dabs-mod 1fc1832d8. Nothing merged, pushed or
deployed. No converter change.

## F3 20260925-225534-959e8efc (Facility, HD, frame 6534): FIXED + VERIFIED

"weird z-fighting glitch happening with door terminal button" - spiral streaks along
the top of the door console's red lamp.

**Cause.** Bean's placeholder spiral (texture `_0x008C4635`; the Community Edition's
grille has the same name) is part of the mesh body: `gebeanBuildRigid()` puts every
triangle in group 0, so handing the screen node back (`xblaMeshNodeIsLiveScreen()`)
never took the spiral away. It lies on GoldenEye's screen quad, crossing it: 0.35
model units in front at the top, 0.29 behind at the bottom. The programme and the
spiral both drew every frame and the depth test picked. It is not frame state (the
previous pass looked for one): head on, all three Facility door consoles show the
spiral over the top half of the lamp on the base build (`faccon-base-*.png`,
`usehead-base-crops.png`); the tester's higher camera sees only a strip. Same thing on
Frigate's bridge consoles 1a/2a/3a/3b (spiral streaks at screen edges, one screen all
spiral), Silo's tuning console, and the other level's door console (stage 0x70).

**Fix** (port/src/gebean.c, `beanFindScreens()` + `beanScreenBacking()`): placeholder
vertices lying on a GoldenEye screen quad (parts 0-3; within 5% of the quad's size of
its plane and 10% of its edges) are laid flat 2% of the screen's size behind the
quad and coloured black. Why not drop it or move it into the screen node's group
(both tried): programmes are often drawn without a depth write (MultiMonitor screens
1-3 when flagged, alpha < 255), and without the placeholder the room drawn afterwards
painted over them - Frigate's screens showed the wall behind. The backing keeps the
depth, always loses to the programme, and is black where a programme is translucent
or absent. Only the placeholder texture is touched: Frigate's radar sweeps
(`_0x07D75EC5`) sit over a bezel that hides GoldenEye's quad and stay as they were.
Log line of each rigid build now has `N screen backing`.

**Verified** (pictures in ~/wt/f3faccon-pics/):
- Facility door consoles head on: base spiral on all three, fix clean (`faccon-cmp.png`).
- Use cycle (propobjInteract, programme 46 -> 47, red to green): base spiral over the
  lamp throughout, fix clean throughout (`usehead-cmp.png`, `fix4use-crops.png` at the
  report camera).
- Frigate 12 monitors (`frig-f4-7/8/9.png`, `frig-fix4-sheet.png`): spirals and streaks
  gone, no holes; unchanged shots 0-4, 6 pixel-identical.
- Silo (`silo-cmp.png`): tuning console spirals gone, other 7 shots identical.
- Stage 0x70 door console (`bunk-cmp.png`): spiral gone.
- Community Edition: placeholder matched (same name), Facility lamp clean (`ce-cmp.png`).
- N64 look: 5 Silo monitor shots pixel-identical base vs fix (the change is only in
  Bean mesh builds).
- Sweep of all monitors on the 20 missions: backing applied to doorconsole (4 verts),
  bridgeconsole1a/2a/3a/3b (4), tuningconsole1 (6); no placeholder vertex anywhere left
  off a screen.

Not reproduced exactly: the tester's own strip at the report camera (our build shows
the spiral there only head on). Same geometry, same mechanism; the fix removes the
crossing rather than depending on the angle.

## Rig (outside the tree)

- run dir `~/wt/f3faccon-run` (copies of f3hdlevels-run's cache/mods/saves), build dir
  `~/wt/f3faccon-build`, binaries `~/wt/f3faccon-rig/pd.base` / `pd.fix4`.
- `look.sh` (STAGE=, BIN=, TAG=, MODELS=hex list, DIST=, N64=1, CE=1, EXTRASED=): one
  shot square on each monitor of a stage, contact sheet `TAG-sheet.png`.
- `use.sh` (CAM=x,y,z,theta,verta; USE=0/1; DAMAGE=; DTS=frame offsets): door console at
  (-3533 -236 1521) used, programme state printed per shot.
- `visitall.sh` + `coplanar3.py`/`infront.py`: build every monitor's Bean mesh per stage
  and list Bean triangles on GoldenEye's screen quads (needs the diagnostic binary
  `pd.diag`, patch `tmp-diag.patch`: TMPV/TMPT/TMPQ logs in gebeanBuildRigid).

## Open

- Silo's console2/console3 placeholders are still dropped by `beanVertexDrops[]`
  (a97daeb6e); the backing would handle them too (and keep their depth), but that table
  is shared with sibling branches, so left alone.
- Frigate's radar screens show Bean's still sweep, not GoldenEye's turning radar
  programme (it is behind Bean's bezel). Unchanged by this branch.
- Several Frigate/Silo programmes draw without a depth write; anything that removes
  Bean geometry behind a screen must keep a backing (see above).
