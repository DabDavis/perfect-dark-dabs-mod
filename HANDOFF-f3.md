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

- `chargeable` (WEAPONFLAG2_CHARGEABLE) is still "left as the port has it" for
  GE-X (its two sites disagree), so GE-X's silenced PP7 inherits the Mauler's
  chargeable flag by address. Not part of this report; worth a look.
- Not merged, pushed or deployed.
