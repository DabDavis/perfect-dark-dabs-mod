# guns/ - GoldenEye's guns, measured on both sides

| tool | contract |
|---|---|
| `sweep.py --out DIR` | every gun on both sides, the repeat control, `DIR/report.md` + `report.json`; exit 0 agree, 1 mismatches, 2 a control failed (no report) |
| `run.py --out DIR [--guns a,b] [--mission dam]` | runs `gunscen.py` in groups: the oracle through `twin.run_ge` (rendering off), ours in per-group run dirs **with the sound bank loaded** (twin's `--no-sound` leaves GE Plus no GoldenEye sound to start) |
| `gunscen.py` | the gdb scenario, one per side: chrs removed, Bond invincible, at level tick 650 each gun given with a full reserve and asked for; tapped (150-156), held (260-740), watched to 1070. Input is the controller (GoldenEye's pad-script state, our joy sample); events are record-and-continue breakpoints |
| `gundiff.py DIR` | metrics per gun per side and the diff; runs both controls first and refuses to report if either fails |
| `gunlist.py` | the 25 guns: GoldenEye item, name, our weapon number |
| `ge_sfx_names.json` | GoldenEye's SFX_ID names, from the decomp's bondconstants.h, for the report |

Controls, every invocation: one gun (`--repeat-gun`, default pp7) is run a second
time on both sides and must agree with itself within the tolerances; a planted
fault (the oracle's clip one larger) must come out as exactly that mismatch.

Rules the numbers depend on (all in gundiff.py's docstring):

- **The frame rule.** GoldenEye's automatics fire every N *frames*; the oracle
  under lockstep runs ~1 tick a frame, the port models GoldenEye's frame as 2
  ticks (`gegunsRpm()`). Automatic cadence is compared as GoldenEye frames x 2.
  Every other GoldenEye timing adds `g_ClockTimer` and is compared in ticks,
  with the oracle's own frame step added to the tolerance (its frames are
  host-paced, 1-3 ticks).
- **Sounds** are GoldenEye ids on both sides (ours: 1..261 on a converted level
  are GoldenEye's sample of the same id, `geSfxRemap()`; appended ones through
  `g_SfxGeId`). Each sound is classed by who asked for it (logged callers):
  the gun's own, a casing landing, a hit. Both games pick hits and whooshes at
  random within a family, so families are compared, ids shown.
- **Shots** come from the clip where the gun has one (our shoot function runs
  every tick an automatic attacks); throws and launches from their own hooks
  (`generate_player_thrown_*`, `gunSpawnGLGrenade`, `gunFireTankShell` - the
  decomp's name for the rocket; ours `bgunCreateThrownProjectile`,
  `bgunCreateFiredProjectile`).
- `raise_ticks`, not `draw_ticks`, is compared: the lowering before it is the
  previous gun's, and the two sides run their guns in different groups.

Harness traps found building it: the oracle's pad script presses Z after the
level has loaded (frames 1300-1660, to skip the intro), which fires the gun
Bond starts with - the scenario gives that magazine back; `--boot-ge-mission`
does not set `g_FrontInside`, which GE Plus's All Guns list keys off - the
scenario sets it.
