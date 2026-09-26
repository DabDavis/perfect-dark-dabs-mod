# HD autogun models (2026-09-26), branch fix/f3-runway-emplacement-hd

Follow-up from the tank pass (fix/f3-tank-aim-fire, its "Open": Pgx292Z has no
HD model row). Nothing merged, pushed or deployed.

**Merge after fix/f3-tank-aim-fire.** The rows need c80118821 ("HD tank's
barrel turns with its turret", the second binding pass for props) to pose the
guns; without it they draw wrong (see below). Not duplicated here.

## The item

Runway's three heavy gun emplacements (00 Agent "destroy heavy gun
emplacements"; GoldenEye autoguns, model 292 `gun_runway1`) drew GoldenEye's
N64 model in the HD look although Bean has `new/prop/gunrunway1`.

- Cause: GoldenEye has three autogun models - 98 `roofgun` (ceiling drone
  gun), 292 `gun_runway1`, 299 `groundgun` (free-standing drone gun) - and
  none had a row in port/src/geproptable.h. They were never fitted when the
  table was made (2a094ac15); the prop texture pass (fix/f3-hd-prop-textures,
  71ec65134) fitted them and excluded them on purpose, because the rigid build
  put their moving parts on the wrong matrices - true without c80118821.
- Fix: the three rows, exactly as `.xbla-work/ge-arena/gen_proptable.py`
  writes them from propfit.json (fits: score 1.00, axes unchanged, scale 2.0).
  The generator stays the source of truth: its EXCLUDE list is now `{117}`
  (the gas tank only), with a docstring note on the c80118821 dependency
  (the generator lives outside git; backup of the old one in the session
  scratchpad `emplace/gen_proptable.before.py`). Regenerating on top of
  71ec65134's 180-row table gives 183 rows whose three new lines are
  byte-identical to this branch's. If the table merge conflicts, regenerate.
- How the bones land with c80118821 (in-game dump, `bones.py` / `bonecount.py`):
  - gunrunway1: rigid on matrix 1 (the turret, part 1 - the whole N64 body is
    under it too); 271 vertices on bone 1 (body), 76 on bone 3 (barrel) ->
    part 3, 0.5 units from the part's place; bones 2/4 (pitch pivot) carry no
    vertices. The barrel pitches with part 2's matrix and spins with part 3's.
  - groundgun: matrix 0 (the tripod); bones 1-6 -> parts 1, 2, 3, 5 (yaw,
    pitch, both barrels); 122 of 206 vertices on moving parts.
  - roofgun: matrix 0 (the ceiling mount); 95 of 110 vertices on 5 bones.
  - Without c80118821 (this branch alone, checked): gunrunway1 rides the yaw
    with no pitch; groundgun draws static, its gun parts on the tripod.

## Other autoguns covered

Every GoldenEye autogun uses one of the three models (listing of every
OBJTYPE_AUTOGUN on the 8 stages that load them, `ag.py`):

| model | stages (our id, GE key) |
|---|---|
| 292 gun_runway1 | 0x5e Runway (3) |
| 299 groundgun | 0x62 Jungle (7), 0x67 Egyptian (4) |
| 98 roofgun | 0x64 Bunker 2 (3), 0x66 Caverns (2), 0x67 Egyptian (3), 0x68 Cradle (2), 0x6d Depot (1), 0x6e Control (2), 0x70 Aztec (6) |

`dest_gun` (101, Frigate) already had a row and is one static part.

## Verification

Rig outside the tree: run dir `~/wt/f3emplace-run` (`run.sh TAG [probe]`,
env SAVE/STAGE/VIEWS/AT/PIN/KILL/BIN; `save_base` N64 look, `save_hdmesh`
HD look with XblaMeshes=1), probes `~/wt/f3emplace-rig`: `ag.py` (lists
autoguns; views as `x,y,z,theta,verta` or `gunN,dist,angle,dy`; PIN
`prop,yrot,xrot;...` per shot pins a gun's angles; KILL destroys autoguns and
prints objectives), `bones.py`, `bonecount.py`, `sheet.py`. Test binary = this
branch + cherry-picked c80118821 and f5bb7a5ef (autogunGeEye), not committed.

- Runway HD, at the tank pass's views: the HD gun idle, turning on Bond and
  firing (muzzle flash from its gunfire node), same yaw/pitch state as the
  tank binary without the rows.
- Pinned angles (yaw 1.4/2.1 x pitch +-0.35) N64 vs HD: barrel and plate pose
  alike in every picture. Same for groundgun (Jungle, prop 415, four poses)
  and roofgun (Egyptian, prop 11, four poses).
- 00 Agent (`-hard2`), HD and N64: objDamage on the three guns completes
  "destroy heavy gun emplacements" (objective 1); the gun is gone, the recess
  behind it shows, in both looks.
- N64 look: pinned Runway pictures pixel-identical with and without the rows.
- Not done: the release in Xenia (the Bean build is not set up to be driven to
  Runway headlessly); the fit against Bean's own N64-look copy scores 100%.

## Open

- In HD the Runway gun's armour plate stands proud of the bunker face rather
  than inside its recess (the HD level's recess is visible once the gun is
  destroyed). Placement is Bean's own relative to the model (100% fit); not
  compared with the release.
- Autoguns tick from level start in the HD look (every room drawn), so their
  yaw at a given frame differs between looks; a teleported Bond just outside a
  gun's 70-degree wake cone is not seen (Jungle prop 415). Same with and
  without these rows; GoldenEye behaviour not checked.
