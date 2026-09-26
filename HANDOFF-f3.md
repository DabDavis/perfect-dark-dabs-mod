# F3 pass 2026-09-26 (GE mission logic) - handoff

Branch `fix/f3-ge-mission-logic` (worktree /home/sdg/wt/f3gemission), based on
dabs-mod 5ac4c3176. Not merged, not pushed. **Converter now 74** (was 71; 72, 73
and 74 are this branch's, one real change each). All 20 missions boot and run
600 frames clean at converter 73 (~/wt/f3gemission-run/sweep.sh, sweep.out).

Rig: `~/wt/f3gemission-run` (copy of ~/wt/f3cradle-run; `run.sh TAG PROBE` with
STAGE/TMO/EXTRA env, `run2.sh` takes SAVE=save_hdbase for the HD look,
added-content/goldeneye -> Bean). Stage ids in this rig: Bunker 1 = 0x6f,
Facility 0x63, Silo 0x6b, Cradle 0x68, Surface 0x69 (the tester's differ).
Probes in `~/wt/f3gemission-run/probes/`: keyrun.py (gadgetprobe2.py + HUD
trace; gadgetprobe2 adds `hold:N` = trigger held N ticks), face.py (teleport in
front of a chr, watch act/alert), ouru.py (Ourumov), dumplist.py (LISTS=0x414
dumps a converted AI list with command lengths), cctv.py.

## 1. Bunker GoldenEye key (231502, 231553) - FIXED + verified

- 57998ca25 (converter 72): converted weapon records left `dualweaponnum` 0
  (wrote 0xff over 0x5d/0x5e instead of 0x61), so every converted floor pickup
  was taken for half a pair: non-dual guns "not given" -> only the ammo message
  ("Picked up an ."), dual-wieldable ones given as a pair with WEAPON_NONE.
  Game-wide, all missions.
- 57998ca25: key analyser now works on the **trigger** (GE gunTickHandState:
  TRIGGER_PRESS -> USE_ITEM -> analyzeGEKey), not on equip; empties left hand.
- 26e458cd7: objective B is COPY_ITEM + DEPOSIT_OBJECT(tag 4). GE throws the
  picked-up prop itself; PD's throw made a new prop and the picked-up one stayed
  a child of the player (invHasProp true forever). Tag now moves to the thrown
  prop, carried prop freed.
- Verified (probes/keyrun.py, Bunker 0x6f): "Picked up a GoldenEye Key.",
  analyser trigger -> key in hand, throw -> "Objective 1: Completed"; picking
  the key up again -> Incomplete (GE semantics).
- Open: in the probe a *second* throw after re-picking the key did not fire
  (hand already showed 0x79; likely the re-pickup does not re-equip/load it).
  Minor; check with a real trigger press.
- Player-facing: after analysing, the key is in hand and must be THROWN (fire)
  to "leave original" - same as GoldenEye.

## 2. Facility HD guards standing at spawn (225349, 225423) - NOT A BUG as measured

Guards in the shots (chr 39 in the toilet stall, the one under the stairs) run
GoldenEye's global SIMPLE_GUARD list (0x807): stand still, no idle animations,
until they see/hear Bond or see a shot/death. probes/face.py puts Bond in front
of chr 39: surprised (act 18) then attacks (act 8) in both N64 and HD looks.
Both tester shots have Bond behind the guard with a silenced PP7. Needs the
tester/user to say what they expected (idle animations? reaction to noise?).

## 3. Silo

- 233940 Ourumov dies instead of fleeing: his converted list 0x414 is faithful
  (armour 30 via aiAddHealth, max 20 -> 50 to kill; flees on list 0x415 when
  target < 500 units (0052) or health < 20 (0081, correct direction); 0x415
  sets CHRCFLAG_INVINCIBLE, runs to pad 0xc7, then aiRemoveChr). probes/ouru.py
  showed the proximity flee working. So he died by taking >= 50 damage before the
  health check fired: next step is per-hit damage of GE guns on chrs in PD
  (chrDamage: head x4 * headshotdamagescale, torso x2) against GoldenEye's
  chrlvDamage / gun damage - likely PD multipliers applied to GE guns.
  Also measured (probes/ouru.py DIST=900 DMG=4 HIT=15): hits spaced 20 frames
  apart -> he switches to 0x415 and flees the moment damage passes 0. GE and
  PD use the same x4 head / x2 chest multipliers; KF7 GE damage 1.0. So the
  tester's death needs ~50 damage between two polls of his list (a burst of
  headshots, or something that multiplies damage). NOT FIXED; next: shoot him
  with real bullets (gadgetprobe2 hold:N) and log chrDamage calls.
- 234037 Bond does not stow weapon in outro: FIXED + verified, eba3af6e4.
  GE BondHideWeapons -> aiChrDrawWeaponInCutscene(bond, WEAPON_NONE); PD's
  switch only completes when the gun ticks, which it does not under the outro
  camera. On remake stages the hands are emptied (inuse false) and the body's
  held guns deleted at once. Shot: Bond in the lift, arms crossed, no gun
  (shots_siloend5).

## 4. Cradle Trevelyan up the ladder (001445) - NOT STARTED

Trace: stage 0x68 f8022, chr 0 act 15 (GOPOS) at y 2711, chrs 1,2,3,5 GOPOS too.
Screenshot looks up a ladder shaft with Trevelyan at the top. Compare his
final-run pads with GoldenEye's (the previous Cradle pass: branch
fix/f3-cradle-trevelyan, rig ~/wt/f3cradle-run, probes trev*.py).

## 5. Security cameras

- Rotate / face backwards (231132 Bunker, 233118 Surface): FIXED, 6d4f81b9a
  (converter 73). CCTV tail (look pad, yleft, yright, ymaxspeed, maxdist) was
  never converted - all nought, cameras still and aimed at pad 0. Verified by
  numbers on Bunker (probes/cctv.py: 4 cameras, pads 88/95/80/63, sweep limits
  +40/-20, 145/0, 180/0, +-30 deg, the near one sweeping). No screenshot, Surface
  not checked in game.
- One shot in the lens (231244): FIXED + verified, c813882a5 (converter 74).
  Converted CCTV models were SKEL_BASIC; PD's x100 lens rule (GE's own) only
  applies to g_SkelCctv. Skeleton carried across. probes/cctv2.py (AIMY=16/28):
  casing hit 500 of 1000 (two to kill, as GE), lens hit -> cctvHandleLensShot,
  200 damage, destroyed at once.
- Surface camera now faces out from the cabin wall (shots_surfcam).

## Side effect to know about (converter 72)

Before 72 a converted floor gun's dualweaponnum was 0, so picking up a
dual-wieldable one gave an odd pair (gun + nothing) and could put it in the
left hand. Now such a pickup is a single gun, as a guard's dropped gun always
was. Whether GoldenEye's own "second of a gun makes a pair" rule should be added
for GE Plus is a user decision (tried and reverted, not committed).

## Before merging

Converter 72 and 73 touch every mission's setup: run the 20-mission sweep
(build/gexrom/runall.sh or hdsweep/ab.sh) after a forced reconvert, and the C vs
Python parity check (should stay the known 210 diffs).
