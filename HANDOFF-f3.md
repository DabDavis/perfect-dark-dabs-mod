# F3: Chicago robot's shots freeze in the air (2026-09-26), branch fix/f3-g5-beams-freeze

Reports (in /home/sdg/wt/f3-0926/): 20260925-224209-e9d359f1 ("shots from the g5 robot
freeze in the air and stay there") and 20260925-224248-8a9f8748 ("they only go away the
next time it shoots"). Parabolee, Windows 718d5dc, stage 0x1d (Chicago - the "G5 robot"
is Chicago's patrol robot, chr body 0x76 BODY_CHICROB), XBLA look, 3840x2160,
`TickRateDivisor=0` (uncapped). In the first trace the robot (chr 31, body 118) is on
screen in `act 14` (ACT_PATROL) with the beams still drawn.

## Status: FIXED + VERIFIED (not merged, pushed or deployed)

Fix commit f7e3da464, `src/game/chraction.c` only.

## Cause

A robot's two beams (`chr->unk348[i]->beam`) are ticked in `func0f041a74()`, and stock does
that only inside the `ACT_ROBOTATTACK` branch. `propsRenderBeams()` draws them whenever
`age >= 0`, whatever the action. `chrTickRobotAttack()` sets `finished` once both guns have
swung back to within 0.03 rad of centre, the AI then replaces the action, and any shot
still in flight stops advancing but is still drawn. It stays until `robotAttack()` resets
`age = -1` at the next attack, which matches the second note exactly.

The frame rate decides whether it shows. A beam's flight is in 60ths (`dist += speed *
lvupdate60f`, speed a quarter of the distance, and it starts 10-40% behind the muzzle). The
gun swing eases a fixed 15% per tick. At 30 fps a shot lands in a tick or two, and below
30 fps `beamTick()`'s "lagging" branch moves it 2-2.5x a tick, so on N64 it practically never
showed. At 60 fps and above a shot needs 4 or more ticks. Chrs' other beams (`g_Fireslots`,
`chrTickBeams()` every frame), autoguns and the chopper are ticked unconditionally, so only
the robot has this problem. It is not specific to the XBLA look: the N64 look does the same.

## Fix

The port ticks the robot's beams at the end of `func0f041a74()` for any chr with both
`unk348` fireslots, whatever its action. The in-branch tick is compiled only for
`PLATFORM_N64`, so a beam is still ticked once per full tick and the attack itself
is unchanged. `unk348` is zeroed in `chrInit()` for every other chr.

## Verification (headless, RX 580 offscreen, gdb)

Rig outside the tree: `~/wt/f3g5beams-rig/` (`run.sh BIN TAG`, `probe2.py`; env `LOOK=xbla|n64`,
`PD_STEP240`, `BACK`, `SHOTS`, `NATTACK`, `AFTER`), run dir `~/wt/f3g5beams-run/`, pictures
`~/wt/f3g5beams-pics/`. Chicago `--boot-stage 0x1d --fixed-step --rng-seed 1 --spectate`.
At frame 120 the intro is skipped (`g_CutsceneSkipRequested` + `autocutgroupskip`). Then
each attack runs as follows:

1. The camera goes 1000-1500 units in front of the robot, in first person.
2. `call robotAttack(robot)` starts the attack (the same call the AI makes at chraction.c:8231).
3. The probe waits for the action to change.
4. It reads both beams' age/dist/maxdist then, and again one second later.

A frame-rate knob was needed for the high-fps cases. `PD_STEP240=N` forces N 240ths per
fixed-step frame. It was a temporary uncommitted hunk in timing.c, built into
`pd.base240`/`pd.fix240` only. Real-clock uncapped runs were no use on this loaded box
(about 23 fps).

Bursts that left beams frozen one second after the attack ended:

| frames per 60th | stock (base) | fix |
|---|---|---|
| 30 fps (step 8) | 0 of 10 | 0 of 10 (numbers identical to base) |
| 60 fps (step 4) | 2 of 10 | 0 of 10 |
| 120 fps (step 2) | 5 of 10 | 0 of 10 |
| 240 fps (step 1) | 4 of 6 / 3 of 4 | 0 of 10 / 0 of 4 |

Pictures: `xbla_montage.png` and `n64_montage.png` in `~/wt/f3g5beams-pics/`, 240 fps,
top row base, bottom row fix. Columns 3-4 are one burst: at its end, then a second later.
Base keeps the two blue beams hanging off the robot in both looks. Fix shows them in
flight and then gone. The final binary (knob removed) ran 5 attacks at 60 fps with none
stuck.

## Open

- The robot's attack is per tick, not per 60th: it fires every other tick and eases its
  guns 15% a tick. With `TickRateDivisor=0` its burst plays in a quarter of the
  real time at 240 fps (same shot count). That is a stock frame-rate dependence of
  uncapped mode and was left alone. The beams now finish at the right speed at any rate.
