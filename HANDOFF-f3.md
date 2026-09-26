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
- ~~In the N64 look, the third-person held-gun props are still the host alias on PD
  maps.~~ Done below.
- The conversion has no Pgx203Z, so the GE grenade launcher fires PD's grenade round
  (290) on both kinds of stage, as before.

# GoldenEye's own held and floor props on PD maps (caccecbcf)

In the N64 look, a GE gun held in third person on a PD map (by a player, a sim or a guard)
drew its host's pickup (`PchrgeKF7Z` and so on, an alias of the PD gun). So did a GE gun
on the floor. Only a converted level has GoldenEye's PROP_CHR* props.

## Change

- `gegunsOwnPropModel()` asks `modloaderLendRemakeModel(prop)` instead of checking the
  model state's fileid. On a PD stage the prop is registered the first time it is asked
  for. It is read into the stage pool when the first one is made. On a converted level
  nothing changes, because the block has already filled the state.
- New `gegunsFloorModel(weaponnum, fallback)`: setup.c (Combat Sim `MPLOCATION` rows) and
  modrandom.c (random weapons) lay the held prop on the floor. These rows hold
  `MODEL_GE_FIRST + i`, the alias. A gun dropped by a sim, a player or a guard already
  went through `playermgrGetModelOfWeapon()`.
- `currentPlayerDropAllItems()` now checks that a gun is held before it asks for the
  model. Before, every death lent all 21 props.
- 21 props are lent. The throwing knife and the three mines keep the alias, as they do on
  converted levels.

## Verification (rig ~/wt/geguns-pd-run; pd.fix = 5881dee35, pd.new = caccecbcf)

Probe `probes/heldprops.py`, driven by `sweep2.sh`. It hands sim 1 (`g_MpAllChrPtrs[1]`)
each gun through `botinvGiveSingleWeapon` and `botinvSwitchToWeapon`, then logs the model
the sim holds and `playermgrGetModelOfWeapon()`. It drops the gun with `botinvDropOne`
(hooked on `weaponCreateForChr`) and logs every floor weapon at frame 300. Guns
0x5e-0x76 plus PD 0x02/0x0a/0x13/0x24. Runs: Skedar 0x32 and Pipes 0x29 (both
`--mpsims 1 --mp-weapons 49,53,57,61,66,68`), and Dam 0x15 (NOSIM). Logs and montages
are in `evidence/heldprops/`.

- Skedar and Pipes: all 21 guns held and dropped are now `Pgx<prop>Z`, the same model
  numbers Dam gives (703 PP7 ... 723 rocket launcher). Before, each was its
  `Pchrge*Z` alias. The six floor rows are Pgx191/184/194/207/185/186. The throwing
  knife and the mines are unchanged. PD guns are identical between base and fix.
- Dam: models, floor props and pool are byte-identical between base and fix.
- Screenshots (`mont_crop.png`, top = fix, bottom = base): PP7sil is now GoldenEye's
  silenced PP7, not the plain alias. KF7 and RCP90 show GoldenEye's shapes too.
- HD look booted (`XblaMeshes=1`): base, fix and the scratch merge with
  fix/f3-ge-sniper-hd give identical models and pool. `ownInUse` is 0 there, so the alias
  with Bean's row is used, as before.
- HD interplay (scratch merge with fix/f3-ge-sniper-hd, not committed; conflict only in
  geguns.h, where the two declarations sit side by side; geguns.c merged clean and
  `gegunsOwnPropModel()` then lends `gegunsChrProp()`): a lent Pgx prop already in a
  sim's hand when the look is switched to HD (F6) is drawn as the release's gun
  (`mont_tgz.png`: HD silencer and scoped sniper). Without that branch it stays N64. So
  lent props and HD mapping work together once it is merged. Floor props placed in the N64
  look behave the same way.

## Stage pool (mempGetStageFreeTotal)

- The six GE floor rows at frame 300 now leave 11.3 KB more free than before (Skedar
  54303360 vs 54292080; Pipes +11.4 KB). GoldenEye's props are smaller than the host
  pickups they replace.
- Per held prop, about 3-6 KB when it is first made. After the full sweep (all 29 guns
  held and dropped), the fix ends 15.5 KB lower than base (Skedar 54155744 vs 54171280;
  Pipes -15.4 KB). Most of that is the lent rocket (Pgx202Z, from 5881dee35), which only
  now loads for sims because their launcher is GoldenEye's. Nothing is lent or loaded on a
  PD map where no GE gun is in play.

## GE-X only (no ROM), from the code, not run

Without the conversion, `gegunsOwnModel()` is 0, so `ownInUse` is 0 and
`gegunsOwnPropModel()` returns -1 before it lends. `modloaderLendRemakeModel()` would
return -1 anyway, because no `models` block exists. `gegunsFloorModel()` gives back the
row's model. Everything falls back to the alias, as before.

## Open

- A GE grenade on a PD map is now GoldenEye's `Pgx196Z` in the hand and when thrown (the grenade
  is in the held table, as on Dam).
- Props made before an N64/HD switch keep their model until they are remade. This was
  already true on converted levels, and with fix/f3-ge-sniper-hd merged HD is drawn over
  them anyway.
