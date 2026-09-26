# F3 pass 2026-09-26 (GE sniper rifle in the HD look) - handoff

Branch `fix/f3-ge-sniper-hd` (worktree /home/sdg/wt/f3sniper), based on
dabs-mod 1fc1832d8. Not merged, not pushed, not deployed. No converter change
(GECONVERT_VERSION untouched).

Rig: `~/wt/f3sniper-run` (copy of ~/wt/f3gemission-run's run2.sh as `run.sh
TAG PROBE`, env SAVE/STAGE/TMO/BIN/EXTRA). **`save_hdbase` is NOT the HD look**:
it has `XblaMeshes=0` (N64 look with XblaGoldenEye=1). `save_hd` is the same
with `XblaMeshes=1`, which is the tester's HD look. Dam = 0x15, Surface = 0x69.
Binaries: pd-before (1fc1832d8), pd-after (commit 1), pd-after4 (== HEAD).
Build: ~/wt/f3sniper/build (RelWithDebInfo), logs ~/wt/f3sniper-{cmake,build}.log.

Probes (`~/wt/f3sniper-run/probes/`):
- guards.py: `GUNS=sniper|all|none` swaps every chr-assigned setup weapon at
  `setupPlaceWeapon` (weaponnum + MODEL_REMAKE_FIRST+PROP_CHR*), then under
  `--spectate` puts the camera in front of the N nearest guards (two shots each)
  and `FLOOR=x,y,z` at a floor gun. The player model is shrunk to hide the
  spectator laptop. `GUNS=all` takes one guard per gun.
- fp.py / fpfire.py: give + equip 0x6b, first-person shot (`TP=1` third person
  too, `FIRE=1` holds Z via input.c:1046).
- fpmtx2.py / fpmtx3.py: gun model matrices at `bondgun.c:12369` (modelRender) -
  at videoEndFrame they are already freed garbage.
- list2.py: every weapon prop, holder and model.
Pictures in `~/wt/f3sniper-run/pics/` (sweep_sheet.png, fp_final_sheet.png,
surf_pair.png, fp4_blendcrop.png).

## 1. Guards hold N64 sniper rifles in HD (233626-f24d0b98) - FIXED, 112ac8278

- Cause: the player's held gun is MODEL_GE_FIRST+i (alias of the host pickup,
  gegunstable.h row draws Bean's pickup on it). Guards' guns and floor pickups
  are GoldenEye's own props from the converted setup, `Pgx<PROP_CHR*>Z`
  (Pgx210Z = sniper), and no gebean row named them. Class-wide: every GE gun
  held by a guard or lying on the floor was N64 in the HD look.
- Fix: gebeanPoolRowForFile() maps a remake prop file whose number is a gun's
  PROP_CHR* to that gun's pickup row (the rows were fitted by gunfit2.py on
  exactly these GE models, so the HD gun lands where the N64 one was).
  PROP_CHR* table moved to file scope in geguns.c (gegunsChrProp(), throwing
  knife + mines added; gegunsOwnPropModel() still skips the thrown ones).
- Guns checked held by a guard in HD, before N64 / after HD (sweep_sheet.png,
  Dam): sniper, KF7, PP7, PP7 silenced, DD44, Klobb, ZMG, D5K, AR33, RC-P90,
  shotgun, auto shotgun, Cougar, Golden Gun, Moonraker, grenade launcher,
  rocket launcher, hunting knife, throwing knife, grenade (20 guns; D5K
  silenced and Phantom were not handed to a guard in the sweep, same code
  path). Floor sniper on Dam's tower HD after. Surface snow guard with the
  sniper HD after (surf_pair.png - the tester's picture).
- Mines 199-201 have no Pgx file in this conversion (not written unless used).
- Sizes: held/floor HD guns are the same length as the N64 prop (fit on GE's
  longest axis); the HD sniper is thinner than GE's fat silencer tube, which is
  the release's model.
- The HD rocket launcher on a guard is Bean's RPG-like model (the player's
  third-person one is the same) - release art, not a fit problem.

## 2. First-person sniper too small (234109-d222b51a) - FIXED, 23a639ffc

- The screenshot is first person, HD look. Bean's sniper was placed by its grip
  on the palm of the bullpup host and fitted to the host's length, at the host's
  position (21, -27.2, -31.5) vs GoldenEye's (11, -20.7, -31.5). Measured:
  its eyepiece stood ~11 camera units further from the eye than GE's model in
  the N64 look - a thin rifle in the corner (hdfp_before_fp_0x6b.png).
- Fix: fpGrip mode FP_OWNPLACE (gebean.c) - Bean's gun at 1/4.7 with Bean's
  first bone (SKEL_TOP = GE's root, whose own position is not drawn: the root
  matrix equals the gun matrix) onto the host's root, moved by
  10 x (own - host position) with x and z negated (0.1 scale, model faces away
  from the eye), minus how far the host's idle anim (1036, frame 0, constant
  through idle) holds its body matrix 33 from rest: (8.7, 17.6, 164.0),
  measured with fpmtx3.py. gegunsViewPlacement() (geguns.c) returns the two
  positions. Only the sniper uses it; the mode is generic.
- Verified: HD first person overlaid on the N64 look's screenshot coincides
  (fp4_blendcrop.png, fp_final_sheet.png: N64 | HD before | HD after); firing
  (recoil) fine; HD third person unchanged.
- Open: the 164 is the PD sniper host's idle pose, measured, not read from the
  animation at build time. If another gun gets FP_OWNPLACE, measure its host's
  body offset the same way (fpmtx3.py, K=<body matrix>).

## N64 look

Pixel-identical before/after with the final binary: guard + floor shots on Dam
(n64_before_* vs n64_final_*) and first + third person sniper (n64fp_*).

## Not touched

The sibling fix/f3-tank-aim-fire change (c80118821, second bone-binding pass in
gebeanBuildRigid, guns excluded by weaponnum) is separate: this branch does not
touch gebeanBuildRigid, and the new rows carry the gun's weaponnum so they stay
excluded from that pass.
