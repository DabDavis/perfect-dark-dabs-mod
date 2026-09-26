# F3 20260926-004756-627d4444: GE-X tracer/spark colours

Tester dblaney1, stage 0x44, `ModDir=GE-X_6a_01-19-25`: "Fix tracer/spark color
of kf7/klobb/phantom/ar33/golden pp7 when playing goldeneye X mod." The
screenshot shows a KF7 throwing green sparks.

## What GE-X does (evidence: the mod's own code)

The five guns sit at the stock numbers of the Mauler (6 Klobb), Phoenix (7 KF7),
Cyclone (11 Phantom), Callisto (12 AR33) and Reaper (20 Gold PP7). Perfect Dark
picks the wall sparks (`shot_calculate_hits`) and the tracer texture
(`beam_render`) with switches on the weapon number that the compiler made
**jump tables in rodata** - GE-X rewrote both tables and no instruction, so
`modcodediff` never showed them. Read from the patched ROM (stock -> GE-X):

| weapon | sparks            | tracer texture          |
|--------|-------------------|-------------------------|
| 6, 7, 12, 20 | green -> default | Mauler's (4) -> default |
| 11     | electrical -> default | Cyclone's (1) -> default |
| 21, 22 | default/orange -> electrical | default/4 -> Cyclone's (1) |
| 28     | tranquilizer -> default | tranquilizer's (3) -> default |

Plus `beam_create_for_hand`'s two Mauler tests (charge beam) renumbered 6 -> 119
(no weapon), and the Cyclone's half-alpha tracer colour folded out of
`beam_render`. So on the console every one of the five draws the plain
yellow sparks and orange tracer, like the PP7.

## Cause

The port kept deciding all three by the stock numbers (and the importer did not
read jump tables there), so GE-X's Klobb/KF7/AR33/Gold PP7 threw green sparks
with a green tracer (the Klobb's was the Mauler's wide charge beam), and the
Phantom electrical sparks with a faint Cyclone tracer.

## Fix (branch fix/f3-gex-tracer-colours)

- `game/modrules.h/.c`: `g_ModWeaponHitSparks[]`, `g_ModWeaponBeamTexture[]`
  (`MODRULES_STOCKGUNFX` = port's switch), reset per mod; `weapon N { hitsparks
  S }` / `{ beamtexture T }` keys in mod.c.
- `prop.c` (`shotCalculateHits`) and `gunfx.c` (`beamRender`) take the mod's
  value after the stock switch.
- `WEAPONFLAG3_CHARGEBEAM` (`chargebeam`) on the Mauler's definition replaces
  `weaponHost() == WEAPON_MAULER` at both `beamCreateForHand()` sites; a
  `FLAG_SITES` row reads GE-X's 119 as "no weapon".
- Both importers (`tools/importmod`, `port/src/modimport.c`): a `gunfx` region
  read from the two jump tables, plus `weaponflags fainttracer { clear }` when
  the half-alpha constant is gone. `MODIMPORT_VERSION` 33, so GE-X re-imports on
  the next start. IMPORT.txt lines identical between the two.
- Notes: CLAUDE-notes/mods.md "GE-X's guns threw green sparks behind green
  tracers", a line in CLAUDE.md.

## Verification

- Rig `~/wt/f3tracers-rig/` (own mods copy, never the shared build/mods):
  `det.sh before|after GE-X|- slots...` is frame-exact (`--fixed-step
  --rng-seed 1`, weapon handed over and trigger held from gdb at fixed level
  frames, 3 shots per gun, offscreen on the RX 580).
- GE-X, before (1fc1832d8) vs after: `gex_before_after.png` - PP7 control
  pixel-identical; Klobb/KF7/AR33/Gold PP7 green -> yellow sparks and orange
  tracer; Phantom electrical -> yellow sparks. Silenced PP7 (slot 4, GE-X's
  rewrite of the Mauler's definition) checked: no charge beam.
- Stock PD, no mod: Falcon 2, Mauler, Phoenix, Cyclone, Callisto, Reaper,
  FarSight, Tranquilizer - all 24 frames pixel-identical before/after
  (`stock_det.png` shows them still green/electrical/orange).
- GE Plus: no GoldenEye gun hosts any of the switched weapons or the Mauler,
  and the overrides only come from a loaded mod's modconfig.
- Python importer on GE-X 5a: same readings; on a map-only mod: nothing written.
- XBLA spark/explosion timings: not involved (g_SparkTypes is not mirrored).

## Open

- `chargeable` for GE-X: done on fix/gex-pp7sil-charge, below.
- Not merged, pushed or deployed.

# GE-X silenced PP7 and the Mauler's charge (branch fix/gex-pp7sil-charge)

## What GE-X does

`chargeable` stood for two tests of the Mauler's 6. In GE-X's patched code
(`~/wt/f3tracers-work/diff/{stock,mod}.bin`):

- `bgun0f09a6f8` (the shot's sound pitched down by the charge): `li at,6`
  unchanged - its Klobb's number;
- `bgun_tick_inc_attacking_shoot` (the shot spends the charge, `matmot1 = 0`):
  0x7f09b224-0x7f09b23c are seven nops - compare, branch and store all gone,
  so no weapon resets (not an unconditional reset).

Slot 4 (the silenced PP7, GE-X's rewrite of the Mauler's definition) is on
neither list, so it has no charge, as GoldenEye's PP7. The wind-up itself
(`bgunTickMaulerCharge()`) and charged damage are by number in the port
(`weaponHost() == WEAPON_MAULER`, GE-X also left `bgun_tick_gameplay2` on 6),
so slot 4 never actually wound up; the inherited flag cost it a pitch event
(at 1.0) per shot and a charge reset.

## Fix

- `WEAPONFLAG3_CHARGESPENT` (`chargespent`) for the reset site, on the Mauler's
  definition; `chargeable` is now only the pitch site (constants.h, bondgun.c,
  invitems.c, mod.c).
- Both importers: one `FLAG_SITES` row each; GE-X writes
  `weaponflags chargeable { clear 6 }` and `weaponflags chargespent { clear }`.
  `MODIMPORT_VERSION` 34. Python and C write the same two lines; nothing else
  in either importer's output changed.
- Notes: mods.md "The Mauler's charge is two flags", a clause in CLAUDE.md.

## Verification (`~/wt/gexpp7-rig/hold.sh before|after GE-X|- weapon:func...`)

Trigger held (pulsed 4/6) 90 frames per weapon/function, `--fixed-step
--rng-seed 1`, sound on the dummy driver so the pitch site runs; logs
`matmot1`, loaded ammo, and each `audioPostEvent` caller.

- GE-X slot 4: flags2 0xc00004 -> 0xc00000. Primary fires every 6 frames,
  7 -> 0 then reload, `matmot1` 0 throughout, before and after; pitch posts
  from `bgun0f09a6f8` 7 -> 0. Secondary (its melee) unchanged.
- GE-X slots 2, 3, 5, 6: tick-by-tick state lines identical before/after;
  slot 6 (Klobb) gains the pitch post GE-X's code gives it (8, at 1.0).
- Stock PD Mauler: state lines identical before/after (secondary charges to
  3.9, reset on each shot; 102 pitch posts both sides); only flags3 gains the
  new bit.
- GE Plus: no GoldenEye gun hosts the Mauler.

## Open

- Not merged, pushed or deployed.
