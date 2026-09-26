# F3 GE Plus tank pass (2026-09-26), branch fix/f3-tank-aim-fire

Reports: /home/sdg/wt/f3-0926/ (tester savantique, Windows 718d5dc, GE Plus
Runway; tester stage 0x75 = our 0x5e). All three fixed; nothing merged,
pushed or deployed.

Rig (outside the tree): run dir `/home/sdg/wt/f3tank2-run` (`run.sh TAG
[probe.py]`, env SAVE / STAGE / TMO / BIN / EXTRA; saves `data/save_base` =
N64 look, `data/save_hdmesh` = HD look with XBLA meshes on), probes in
`/home/sdg/wt/f3tank2-rig`:
- `tankaim.py` - climbs into Runway's tank and turns the view 0/+-40/+-90
  against the hull, a shot each (FIRE=n fires too)
- `tankfire.py` - fires shells (GAP ticks between tries, N, NOGATE=1 skips the
  fire-rate gate, TURN/VERTA aim, SHOTS=1 pictures) and logs every
  explosionCreate with its distance from the tank
- `bones.py` - dumps new/prop/tank's Bean bones against the N64 tank's parts
- `agview.py` (VIEWS='x,y,z,theta,verta,room;...', GUNYAW/GUNPITCH pin gun 46),
  `agfire.py` (VIEW=..., logs autogun state and every beam), `autoguns.py`
- GoldenEye oracle: `gerunwaygun.py` in the session scratchpad, run on
  10.8.0.3 from ~/claude-007/007 as in ge-plus-dam-oracle (dam.padscript,
  level swapped to LEVELID_RUNWAY). Runway offset ours = GE + (-11259, 441, 14441).

## 20260925-230256 "tank cannon direction is inconsistent with aiming position"

- Status: fixed.
- Cause: HD look only. gebeanBuildRigid() binds a Bean bone to a part of the
  N64 model only when the part hangs straight off the first list's matrix, and
  compares the bone with the node's *own* offset. The tank's barrel (part 3)
  hangs off the turret (part 1), so Bean's barrel bone (158 vertices) stayed
  on the hull's matrix: the HD turret turned with the view, the barrel kept
  pointing along the hull. The shell always flew at the crosshair; N64 look
  was right.
- Fix: gebean.c, a second pass for props: an unbound bone within 100 units of
  a nested position node's place in the model (its offset plus its parent
  parts') goes on that node's matrix. Parts under a switch/LOD/reorder/headspot
  are refused (their matrices are only built while chosen). Guns excluded.
- Verification: tankaim.py HD before/after (barrel now follows at 0/+-40/+-90
  and pitches); N64 look unchanged; A/B sweep of all 20 GE Plus stages in the
  HD look (see commit message for the result).

## 20260925-230611 fire rate / shells exploding in earlier blasts

- Status: fixed.
- Cause 1: TANK_SHELL_GAP was 60 ticks. GoldenEye's tank_stats: magazine 1,
  RecoilSpeed 0x780078FF, so the hand's RECOIL1 state holds 120 ticks
  (gunfire.c), then the empty-magazine reload swaps for 17 (the hand is hidden,
  so no lower/raise): 137 ticks, ~2.3 s.
- Cause 2: the shell is Perfect Dark's rocket, and objDamage() sets off a
  rocket that takes damage; an explosion damages what is in it every frame
  (sustained damage), so each new shell went off in the previous blast, ~230
  units nearer each time. GoldenEye damages what is in a blast only every
  quarter of its life (explosion.c unk3CA).
- Fix: getank.c, TANK_SHELL_GAP = TICKS(120 + 17); each shell gets
  OBJFLAG2_IMMUNETOEXPLOSIONS (still set off by gunfire and by what it hits).
- Verification: tankfire.py on the old binary, 60-tick gap: blasts at 4464,
  4238, 3996, ... 1718 (the walk back). New binary: every blast at 4464, even
  with the gate bypassed at a 30-tick gap; the fastest trigger mashing fires at
  210, 347, 484 (137 apart).

## 20260925-230637 "nonexistent turret"

- Status: fixed (as a GoldenEye mismatch; see Open).
- What it is: the crosshair is on one of Runway's three heavy gun
  emplacements (GoldenEye autoguns, model 292, bound pads 3-5; 00 Agent's
  "destroy the heavy gun emplacements"). They are in GoldenEye; the oracle
  shows the first one in its bunker recess, turned on Bond and firing.
- Cause: GoldenEye places an object with flags2 & 1 (every Runway autogun, and
  no other GoldenEye autogun) with prop->pos on the pad and only the model
  (runtime_pos) out at the pad's box; its sight line and a muzzle-less shot
  start at prop->pos. Perfect Dark has one position, the model's (identical to
  GoldenEye's runtime_pos, checked), which is inside the recess: every sight
  line hit the bunker, the gun never woke, and it sat with its armour plate
  out and barrels in the wall - a turret that is not there.
- Fix: propobj.c autogunGeEye(): on a remake stage, such an autogun sees (the
  cdTestLos05 in autogunTick) and falls back to shooting from its pad. N64
  build untouched.
- Verification: agfire.py at the report's spot: before, lastseebond60 -1 and
  no beam in 180 frames; after, the gun turns to yaw 1.79 (GoldenEye's
  1.79-2.03 at the same spot) and fires from its muzzle, pictures matching the
  oracle's in both looks.

## Open

- "nonexistent turret" read as "a turret that does nothing/shows nothing";
  if the tester meant GoldenEye has no turret there, it does (oracle frames).
- Guns 2 and 3 (on the cliffs, pads 4/5) now wake the same way; not looked at
  from close range.
- Pgx292Z has no HD model row (new/prop/gunrunway1 exists in Bean); the N64
  model is drawn in the HD look.
- The shell still spawns ahead of the hull's middle, not at GoldenEye's muzzle
  node; it leaves the barrel's end closely enough at every turret angle tried.
