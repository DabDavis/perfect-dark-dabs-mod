# GoldenEye guns on Perfect Dark maps (2026-09-26), branch fix/ge-guns-on-pd-maps

Two reports that /home/sdg/wt/verify6/RESULT.md confirmed still open on 1fc1832d8:
- 20260919-174510 (Combat Simulator Pipes 0x29, N64 look): the GE rocket launcher held
  and fired Perfect Dark's rocket (model 287), and nothing showed at the tube's mouth.
- 20260924-035424: "in perfect dark mode, almost all goldeneye weapons use the wrong
  sound effects when firing and reloading".

Both have one cause. GoldenEye's rocket (Pgx202Z, MODEL_REMAKE_FIRST + 202) and its
sfx bank (menu/sfxctl + sfxtbl) come from the conversion, and only a converted level
used them. On a PD stage, `modloaderApplyStageModels()` leaves the remake's model
states empty, and `geSfxStage()` is false. So the gun kept its host's rocket, its
host's shot, and PD's reload and empty-click sounds.

## Fix

- `modloaderLendRemakeModel(slot)` (modloader.c): on a stage that is not one of the
  remake's own, this fills one remake model state from the `models` block for the rest
  of the stage. It only registers the file. The model loads the first time something
  is made of it. `gegunsOwnRocketModel()` and `gegunsChrProjectileModel()` (geguns.c)
  ask through it. A converted level behaves as before, because the fileid is already
  set there or the lend is refused.
- gesfx.c now has three functions for a GE gun on any stage:
  - `geSfxGuns()`: whether the bank is available. It loads the bank on the first call.
  - `geSfxGunShot(id)`: on a converted level, the id itself (the player remaps it, as
    before). Elsewhere, `sfxPropNum(id)`, which is GoldenEye's sample under GoldenEye's
    audio config, so a sim's shot keeps GoldenEye's falloff.
  - `geSfxGunSound(weapon, num)`: for a GE weapon on a PD stage, it resolves the russ
    mapping and applies the same rule as `geSfxRemap()` (now shared as
    `sfxRemappable()`). A config is kept with an appended russ row, cached per row.
- Call sites:
  - game_0b0fd0.c: the shoot sound and the fire-slot duration (SoundTriggerRate).
  - bondgun.c: GUNCMD_PLAYSOUND, SFX_RELOAD_DEFAULT, and the default SFX_FIREEMPTY
    click. Each only changes a GE weapon on a PD stage.
- With only GE-X installed (no conversion), `sfxLoad()` is false, so everything falls
  back as it did before. This was not run; it follows from the code.

## Memory

- Stage pool: the lent rocket costs about 4.4 KB. On Skedar, total stage free at the
  same point was 53675200 on base and 53670704 on the fix. It is only spent once the
  launcher is loaded.
- Heap, not the stage pool: the GE bank (about 23 KB ctl + 797 KB tbl) loads the first
  time a GE gun asks for a sound and is kept. Nothing is loaded on a PD map where no GE
  gun is fired.

## Verification

Rig: `/home/sdg/wt/geguns-pd-run` (a copy of verify6's). `pd.base` is 1fc1832d8 and
`pd.fix` is this branch. Sound is on (`SOUNDFLAG=`), and the probe is
`probes/gunsnd.py`. It tags each sound by the function that called it and resolves the
number to what it plays: geN is GoldenEye SFX_ID N, pd0x.. is PD's. Compare with
`summ.py`. Evidence is in `/home/sdg/wt/geguns-pd-run/evidence/`.

- Rocket, Pipes (`--mpsims 1` is needed: in solo, Pipes has no spawn pads and the player
  falls, which is why verify6's recipe without it hangs on the pak dialog) and Skedar:
  GE launcher 287 -> 714, held and fired (the `weaponCreateProjectileFromWeaponNum`
  model). Its warhead now shows at the mouth (`rocket_pipes_fix_loaded.png`). The PD
  launcher (0x18) still gives 287.
- Sounds (`compare.txt`): on Skedar, all 19 GE guns 0x5e-0x70 now have the same shot,
  fire-slot rate, reload (ge50) and empty click (ge89) as on Dam. Before, they had the
  host's (pd0x5ed and so on, reload pd0x32, click pd0x59). Four PD guns on Skedar
  (0x02, 0x0a, 0x13, 0x24) are identical between base and fix. Dam fix and Dam base are
  identical for all 23 guns, and so is the stage pool.

## Open

- GE sounds on PD maps play at full volume. On converted levels the player scales
  remapped sounds to GESFX_VOLUME, which gives 3/4 against GoldenEye's music. Tune this
  if a tester finds them loud or quiet next to PD's guns.
- Equip (draw) sounds are still the host's switch on PD maps, and so are the Moonraker's
  ricochet pair. Surface, casing and ricochet sounds stay PD's on PD maps on purpose.
- In the N64 look, the third-person held-gun props (`gegunsOwnPropModel`) are still the
  host alias on PD maps. The same lend could serve them (21 props).
- The conversion has no Pgx203Z, so the GE grenade launcher fires PD's grenade round
  (290) on both kinds of stage, as before.
