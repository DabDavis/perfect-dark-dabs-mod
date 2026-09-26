# F3 pass 2026-09-26 (GE mission logic) - handoff

Branch `fix/f3-ge-mission-logic` (worktree /home/sdg/wt/f3gemission), based on
dabs-mod 5ac4c3176. Not merged, not pushed. **Converter now 73** (was 71; 72 and
73 are this branch's, one real change each).

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

## 3. Silo - IN PROGRESS / not fixed

- 233940 Ourumov dies instead of fleeing: his converted list 0x414 is faithful
  (armour 30 via aiAddHealth, max 20 -> 50 to kill; flees on list 0x415 when
  target < 500 units (0052) or health < 20 (0081, correct direction); 0x415
  sets CHRCFLAG_INVINCIBLE, runs to pad 0xc7, then aiRemoveChr). probes/ouru.py
  showed the proximity flee working. So he died by taking >= 50 damage before the
  health check fired: next step is per-hit damage of GE guns on chrs in PD
  (chrDamage: head x4 * headshotdamagescale, torso x2) against GoldenEye's
  chrlvDamage / gun damage - likely PD multipliers applied to GE guns.
- 234037 Bond does not stow weapon in outro: not started.

## 4. Cradle Trevelyan up the ladder (001445) - NOT STARTED

## 5. Security cameras

- Rotate / face backwards (231132 Bunker, 233118 Surface): FIXED, 6d4f81b9a
  (converter 73). CCTV tail (look pad, yleft, yright, ymaxspeed, maxdist) was
  never converted - all nought, cameras still and aimed at pad 0. Verified by
  numbers on Bunker (probes/cctv.py: 4 cameras, pads 88/95/80/63, sweep limits
  +40/-20, 145/0, 180/0, +-30 deg, the near one sweeping). No screenshot, Surface
  not checked in game.
- One shot in the lens (231244): NOT STARTED (GE object damage / lens hit part).

## Before merging

Converter 72 and 73 touch every mission's setup: run the 20-mission sweep
(build/gexrom/runall.sh or hdsweep/ab.sh) after a forced reconvert, and the C vs
Python parity check (should stay the known 210 diffs).
