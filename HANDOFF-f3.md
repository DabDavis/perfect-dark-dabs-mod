# F3 handoff: GE Plus guard aim + Facility railing (branch fix/f3-dam-guard-aim2)

Worktree /home/sdg/wt/f3gunaim, based on dabs-mod 5ac4c3176. Nothing merged, pushed or deployed.
No converter change (GECONVERT_VERSION untouched).

## 1. Dam guards never hit (F3 20260925-235655-caf13a35): FIXED + VERIFIED

Commit 6b93d5e5f (notes be2cb7145).

**Cause.** The conversion passes GoldenEye's attack bitfield through for
TRYFireOrAimAtTarget / Kneel / Update and TRYFacingTarget (AI 0x15-0x18).
GoldenEye's TARGET_BOND is 0x0001, which is Perfect Dark's ATTACKFLAG_AIMATBOND,
and nothing in PD reads that flag. PD uses ATTACKFLAG_AIMATTARGET (0x0200) for
"shoot at the target". With 0x0001:
- chrTickShoot() only calls chrCalculateHit() for AIMATTARGET, so every bullet
  of a guard's own attack went down the "shooting at something else" path. **No
  guard of a converted mission could ever hit the player**, at any difficulty
  and in either look.
- chrCalculateAimEnd() aimed from the guard's root at the player's prop position,
  which is his eye. That is the "shooting above Bond": mean shoulder pitch was
  0.233, and 0.117 after the fix.

The fourth-pass aim work (732114521, 6d1b84b29, 30c17afc2) measured with attacks
forced from gdb using 0x0200, so it never saw this.

**Fix.** aiGeAttackFlags() in src/game/chraicommands.c turns 0x0001 into 0x0200
for those four commands on a remake stage (modloaderStageIsRemake()). The
converter could translate the bit instead, but do one or the other, not both.

**Measured**: guard left to his own AI (aimprobe KEEPAI=1), player invincible and
healed each tick. The numbers are hits a minute, with chrCalculateHit() calls in
brackets.

| where | before | after |
|---|---|---|
| Dam, report's spot (guard 115 away), HD, Agent, 1800 fr | 0 (0) | 34 (103) |
| Dam, report's spot, HD, 00 Agent | 0 (0) | 50 (103) |
| Dam, report's spot, N64 look, 00 Agent | 0 (0) | 50 (103) |
| Facility (0x63 here), guard 250 away, HD, Agent, 1200 fr | 0 (0) | 12 (27) |
| Facility, 00 Agent | 0 (0) | 18 (26) |

- Defection (PD): the probe's guard never attacked in either binary, so there
  is no rate to compare. The change is gated on a remake stage and on
  GoldenEye's bit, and PD's own lists use 0x0200.
- The oracle's forced-attack figures at 300 units are 22 (Agent) and 29-31 (00).
  Natural-AI numbers are lower because the list paces attacks.

**Open.** The natural-AI hit rate has not been compared against GoldenEye's own
natural AI. gehitrate.py on the oracle forces attacks too; next step is a
KEEPAI-style run there. The fourth pass's "Dam kneel hit-rate 6 (barrier
LOS?)" was measured with forced attacks and should be re-measured with
KEEPAI=1.

## 2. Facility "can't shoot through railing" (F3 20260925-225349-5d307248): FIXED, background verified

Commit 9acab5c29.

**Cause.** In the HD look the room is the release's: gebeanstage.c serves all
rooms, and room 8 has 39 batches in HD against 30 in the N64 look. Bean draws the
railings as cut-outs in the opaque leaf. bgTestHitInRoom() tests opaque batches,
so the shot stopped on the rail 100 units out, at (-4443 12 1341). The N64 look
at the same spot already hits the floor past the guard, at (-3921 -319 1233).
GoldenEye tests the primary list only.

**Fix.** bgTriPassesShots() (bg.c) and gebeanStageTilePassesShots()
(gebeanstage.c): on a remake stage, a triangle whose picture is a served room's
cut-out or translucent one lets the shot through. It matches by the G_SETTIMG
tile pointer, because the texture number a served room gives is garbage (see the
note).

**Verified.** The HD shot now lands at (-3924 -319 1235), the same point as the
N64 look's. A gdb-called shotCreate() never registers a chr hit in either look,
so the kill itself is unverified; a tester should confirm in game.

**Side effect to watch.** In HD, bullets now also pass every cut-out: fences,
grates and leaves (for example Jungle foliage). That matches GoldenEye's
primary-list-only rule as far as those cut-outs are its secondary-list pictures.
This touches gebeanstage.c, which the HD-levels agent (~/wt/f3hdlevels) also
edits. The addition is small (one function and a stub), but check the merge.

## Rig

- Build: `cmake -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo . && make -C build -j6`
- Run dir: build/run. data/ and added-content/ are copied from
  ~/wt/f3guardaim/build/ge; mods/GoldenEye Arenas is reconverted to 71. save_hd
  is the HD look (XblaMeshes=1) and save_n64 the N64 look.
- Binaries in build/run: pd-before.x86_64 (5ac4c3176), pd-after.x86_64 (+aim fix),
  pd-rail.x86_64 (+railing fix).
- tools/guardaim/aimprobe.py adds ROOMS, GX/GZ(+PIN) and KEEPAI. Scripts
  build/run/batch*.sh; logs build/run/m_*.log.
- tools/guardaim/shotprobe.py is the railing probe. Logs: build/run/rail_*.log.
- Report spots:
  - Dam: X=8018.7 Y=12732.4 Z=9169.7 ROOMS=103,104 TH=40.1, guard GX=7940
    GZ=9254. Without ROOMS the player lands on the road 540 above. In HD,
    chrMoveToPos() puts the guard 60 units nearer (spawn adjust), hence GX/GZ.
  - Facility (0x63 in this install, 0x7a in the tester's): X=-4544.9 Y=80
    Z=1361.7 ROOMS=8 TH=258.4 VA=-32 CHR=44.
