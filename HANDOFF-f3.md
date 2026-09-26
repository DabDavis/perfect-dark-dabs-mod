# F3 handoff: GE Plus guard aim + Facility railing (branch fix/f3-dam-guard-aim2)

Worktree /home/sdg/wt/f3gunaim, based on dabs-mod 5ac4c3176. Nothing merged/pushed/deployed.

## 1. Dam guards never hit (F3 20260925-235655-caf13a35) - FIXED + VERIFIED

**Cause.** The conversion passes GoldenEye's attack bitfield through for
TRYFireOrAimAtTarget / Kneel / Update and TRYFacingTarget (AI 0x15-0x18).
GoldenEye's TARGET_BOND is 0x0001 = Perfect Dark's ATTACKFLAG_AIMATBOND, which
nothing in PD reads; PD says it with ATTACKFLAG_AIMATTARGET (0x0200). With
0x0001:
- chrTickShoot() only calls chrCalculateHit() when `attackflags &
  ATTACKFLAG_AIMATTARGET`, so every bullet of a guard's own attack went down the
  "shooting at something else" path: **no guard of a converted mission could
  ever hit the player** (any difficulty, any look).
- chrCalculateAimEnd() aimed from the guard's root at the player's prop pos (the
  eye) instead of PD's body-height rule: the "shooting above Bond".

The fourth-pass aim work (732114521, 6d1b84b29, 30c17afc2) measured with
attacks forced from gdb with flags 0x200, so it never saw this.

**Fix.** 6b93d5e5f: `aiGeAttackFlags()` in src/game/chraicommands.c turns 0x0001
into 0x0200 for those four commands on a remake stage
(modloaderStageIsRemake()). No converter change needed (the converter could
instead translate the bit; that is the other agent's file).

**Measured** (see "Measurements" below; probe = guard's own AI, KEEPAI=1).

## 2. Facility "can't shoot through railing" (F3 20260925-225349-5d307248) - see status below

bgTestHitInRoom() already skips XLU batches on remake stages (efda98e59, in the
tester's 718d5dc). Probe written: build/run/shotprobe.py (teleports the player
to the report's spot, calls shotCreate() and prints what bgTestHitInRoom /
objTestHit / chrTestHit return).

## Rig

- build: `cmake -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo . && make -C build -j6`
- run dir: build/run (data/, added-content/ copied from ~/wt/f3guardaim/build/ge,
  mods/GoldenEye Arenas reconverted to 71; save_hd = HD look, save_n64 = N64 look)
- build/run/aimprobe.py: the fourth pass's probe plus ROOMS (player rooms),
  GX/GZ (+PIN) to put the guard at an exact spot, KEEPAI=1 (guard keeps his own
  AI list and attacks by himself - the only way to see this bug)
- build/run/batch.sh: before/after table; pd-before.x86_64 / pd-after.x86_64
- Tester's Dam spot: X=8018.7 Y=12732.4 Z=9169.7 ROOMS=103,104 TH=40.1, guard
  GX=7940 GZ=9254 (Dam is two storeys there - without ROOMS the player lands on
  the road above). In HD, chrMoveToPos() puts the guard 60 units nearer than in
  the N64 look at that spot (spawn adjust), hence GX/GZ.
