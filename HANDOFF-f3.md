# F3 pass 2026-09-26 (GE mission logic) - handoff

Branch `fix/f3-ge-mission-logic` (worktree /home/sdg/wt/f3gemission), based on
dabs-mod 5ac4c3176. Not merged, not pushed. **Converter now 76** (was 71; 72 to 76
are this branch's, one real change each). All 20 missions boot and run
1800 frames clean at converter 76 (~/wt/f3gemission-run/sweep1800.sh, sweep.out).

Rig: `~/wt/f3gemission-run` (copy of ~/wt/f3cradle-run; `run.sh TAG PROBE` with
STAGE/TMO/EXTRA env, `run2.sh` takes SAVE=save_hdbase for the HD look,
added-content/goldeneye -> Bean). Stage ids in this rig: Bunker 1 = 0x6f,
Facility 0x63, Silo 0x6b, Cradle 0x68, Surface 0x69 (the tester's differ).
Probes in `~/wt/f3gemission-run/probes/`: keyrun.py (gadgetprobe2.py + HUD
trace; gadgetprobe2 adds `hold:N` = trigger held N ticks), face.py (teleport in
front of a chr, watch act/alert), ouru.py (Ourumov), dumplist.py (LISTS=0x414
dumps a converted AI list with command lengths), cctv.py, idle.py (CHRS= act/anim/pos every STEP; TELE= puts Bond by the first), place.py (Bond at PX/PY/PZ/ROOM/THETA, logs CHRS + shots), patcount.py (chr actions at AT).

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

## 2. Facility guards standing at spawn (225349, 225423) - FIXED, converter 76

The user asked for GoldenEye's idle. **GoldenEye's idle is the AI list's, not the
chr tick's**: an ACT_STAND guard only loops the stance (chraction.c's
chrlvIdleAnimationRelated(): ANIM_idle 0-120 at 0.25, ours already the same,
row 1 = GE idle); the fidgets (yawn, swat flies, scratch leg/butt, adjust
crotch, sneeze) are global list 3, GAILIST_PLAY_IDLE_ANIMATION (chraidata.c's
m_IdleAnimations), CALLed only by GAILIST_STANDARD_GUARD (0x802) - about a 2/256
chance a tick while stopped and not already animating. GAILIST_SIMPLE_GUARD
(0x807) is "no clones, no animations" by design. Measured (probes/idle.py):
Facility's 0x802 guards (chrs 0, 3, 12, 13, 16) already play them (ACT_ANIM,
GE anims 1350-1354) - nothing to do there.

**The real bug**: the two guards were never meant to stand. Facility's setup
gives chr 39 list 0x406 = `guard_start_patrol(5)` then 0x807, and chr 44 (under
the stairs) 0x401 = patrol 0; twelve Facility guards are on 0x401-0x407.
GoldenEye's StartPatrol converted to PD's **aiSetPath (0x21) only**, which just
stores the path; PD's aiStartPatrol (0x22) never followed, so **no GE Plus guard
ever patrolled** (decomp setups: ~76 StartPatrols - Facility 7, Caverns 7, Dam
7, Silo 15, Surface/Surface 2 16 each, Bunker, Runway, Aztec...).
- converter 76 (geconvert.c writeSoloAilist(), gesolo.py GE_STARTPATROL_OP):
  StartPatrol -> `0021 <path> 0022`. C and Python setups identical (ark, dam).
- chraction.c chrStartPatrol(), remake stages only (geRoomActive()): GoldenEye's
  set_actor_on_path() first step - a path pad in the guard's room within 100
  units, else step 0 (not PD's nearest / resume step). PD missions unchanged.
Verified: Facility 0x63 chrs 39/40/44/45 all ACT_PATROL from frame 1 and walk
their paths (39 out through the double doors, 44 across the stairs room, 40
upstairs), N64 and HD (save with XblaMeshes=1 - save_hdbase has it 0, so
earlier "HD" runs in this rig were N64 levels); face.py: chr 44 surprised ->
gopos -> attack, chr 39 (HD) attacks. PD stage 0x30: chr actions/positions at
frame 600 identical old vs new binary. 20-mission sweep 1800 frames clean at 76.
Open: not compared against the native GE oracle (decomp is unambiguous); PD's
line-of-sight/gopos lead-in to the first step kept (GE walks it as a patrol).

## 3. Silo

- 233940 Ourumov dies instead of fleeing: FIXED + verified, fba0c1166 (3b).
  His converted list 0x414 is faithful (armour 30 via aiAddHealth, max 20 -> 50
  to kill; flees on 0x415 when target < 500 units or health < 20; 0x415 sets
  CHRCFLAG_INVINCIBLE, runs to pad 0xc7, then aiRemoveChr). NB the GE decomp's
  setup macros print their words byte-swapped: guard_flags_set_on(0x10000000)
  is flag 0x10 (INVINCIBLE), armour 0x2c01 is 300 (30.0), list 0x1504 is 0x415.

## 3b. Scripted chrs die to one headshot (233940 Silo, 005817 + 010126 Cradle) - FIXED

Root cause: Perfect Dark's player headshot is x4 **and then x25**
(`headshotdamagescale = g_ModPlayerHeadshotScale`, default 25) in solo, co-op
and counter-op; GoldenEye's chrlvDamage is x4 only. A PP7 headshot on Agent
(tx 2) did 1 x 2 x 4 x 25 = 200: Ourumov -30 -> 170 and Trevelyan -2 -> 198,
dead in one shot, before their lists (0x414 health check; the Cradle's 0x411
"damage off" that sets INVINCIBLE) got a tick. The tester traces match that
exactly: dead chr 0 with INVINCIBLE set (0x00200b1c / 0x00280a1c) - the list
set it *after* the death. Trevelyan is not invincible while running on list
0x411 until a wound is seen (flags 0x00080a0c), so any headshot then killed him.

Fix fba0c1166 (chraction.c chrDamage()): on a converted level
(`geRoomActive()`) headshotdamagescale stays 1 - GoldenEye's own x4. Stock PD
stages keep x25.

Verified with real PP7 bullets, probes/headshot.py (teleport in front of CHR,
aim at bbox top - HOFF each frame, trigger pulses, chrDamage logged with
hitpart; WAITLIST waits for a list, DIFF sets g_Difficulty):
- Cradle 0x68 chr 0, WAITLIST=0x413 HOFF=24 DIST=200: before, hitpart 8 ->
  damage 198, ACT_DIE (hsb_trev_24.log). After, damage 6, INVINCIBLE set by his
  list, he runs on to his next stand spot, which clears it (hsa_trev.log,
  shots_hsa_trev2: spark on his head, then running out of the door).
- Silo 0x6b chr 0, AT=300 DIST=550 HOFF=13: before, damage 170, dies
  (hsb_ou_13.log). After, two headshots -30 -> -22 -> -14, he then switches
  to 0x415 and flees to pad 0xc7 and is removed (hsa_ou.log, shots_hsa_ou).
- Normal guards, one headshot on Agent: Silo chr 41 (no armour) 8 >= 4, dies;
  Cradle chr 1 (armour 2) 6 >= 4, dies.
- Behaviour change to know about: on Secret Agent / 00 Agent (tx 1) a headshot
  is 4, so a guard GoldenEye gave armour (Cradle's guards: 2) takes a second
  hit, as in GoldenEye (hsa_sa_c1.log); unarmoured guards still die at once
  (hsa_sa_g41.log). If the user wants PD's one-shot heads for plain guards,
  the scale would have to be kept for chrs whose lists never touch armour or
  invincibility - not GoldenEye's rule.

## 3c. Silo outro

- 234037 Bond does not stow weapon in outro: FIXED + verified, eba3af6e4.
  GE BondHideWeapons -> aiChrDrawWeaponInCutscene(bond, WEAPON_NONE); PD's
  switch only completes when the gun ticks, which it does not under the outro
  camera. On remake stages the hands are emptied (inuse false) and the body's
  held guns deleted at once. Shot: Bond in the lift, arms crossed, no gun
  (shots_siloend5).

## 4. Cradle ladder at the end (001445 Trevelyan up it, 005903 Bond stuck) - FIXED

No converter change (still 74). Cradle's shaft: one ladder (upright tiles 625/626,
special 3, **room 7**) from the platform floor (y 1162, room 7) to the deck round
the hatch (2609, room 8; the hatch is x -1618..-1448, z -1093..-927, walled
except the ladder's 81-wide link). GE route: TH (0x410) sprints to pad 0x96 on
the deck, 0x415 walks to pad 0x74 at the foot (waypoint link 0x71 -> 0x74 *is*
the ladder: GE's guard just drops down it, ours falls the same way - fine), 0x417
runs 0x74 -> 0x77 -> 0x76 on the floor, 0x418 fights until he is in pad 0x94's
room or dying.

Three causes, all runtime:
- **Trevelyan up the ladder**: Perfect Dark's guards take hold of any ladder
  within 2.5 radii while GOPOS/PATROL and climb it whichever way they go. On
  0x417 he ran past the ladder's foot (dodging Bond, who stood at it as in the
  tester's shot) and went back up to the deck. Fix (chr.c chrGeTakesLadder()):
  on remake stages a *newly found* ladder is taken only if the chr's current
  waypoint is > 150 over its manground (pads are ~90 over their floor); one
  taken is kept. GE guards have no ladders - they follow tile links.
- **Bond could not climb down**: the ladder is room 7's, the deck room 8's, and
  the player's ladder tests only ask the rooms he is in, so from the deck it was
  never found; walking on, he fell into the hatch and hung in its far lip
  (vv_ground 2609 over his feet, can't rise, can't move - gestan skips the lip's
  walls from the floor tile under him). Even with the room right, the drop is
  seen only once his circle is clear of the edge (> 30 out) and the take-from-top
  test reaches 33 - a 3-unit window. Fix (bondwalk.c): ladder tests ask
  near rooms too (bwalkCdRooms() -> geroom.c geRoomAddNear(), rooms whose box
  meets the body's); at the drop a ladder is looked for 2 radii out and 60
  down (collision.c cdFindLadderDist(), also returns the plane distance and
  the head's height), the player is put back to radius + 1.5 from it and let
  down to 2 under its head, where the hold is kept.
- Found on the way: Dam's three short ladders (rooms 72/75/78) have their head
  at the foot of a 34-high ramp off the deck, so walking off the deck was a
  fall with the old binary too (pd-before.x86_64, dl75b.log) - the 2026-09-20
  "five down" test must have started on the ramp. Now taken (dl75/dl72/dl71).
- **Bond stuck at the stair head (the 005903 position, -2006 2922 -1391)**: the
  stair down from the landing (room 3) to the hatch deck is room 6's; the portal
  walk left the player in room 3 while on the stair's top tread, so room 6's side
  wall (z -1411) was never asked, he slid to 13 from it, and the moment room 6
  was his he was inside its radius for good. Fix: the player's move/vertical
  collision tests (bwalkCalculateNewPosition's cdExamCylMove02,
  bwalkTryMoveUpwards, bwalkCanMoveUpwards) ask bwalkCdRooms() as well; the
  player's own room list is untouched (AI "Bond in room with pad" reads it).

Verified (rig ~/wt/f3gemission-run, Cradle 0x68):
- probes/trevend.py (TPAD=106 puts Trevelyan near the end, LIST=0x410, player
  at the tester's spot PXZ=-1516,-1019 looking up): before (te3.log) 0x417 at
  f740-910 climbs from 1184 to 2161 up the ladder; after (te4.log) he drops down
  the shaft, runs 116 119 118 on the floor, 0x418 attacks. KILLAT=1000 (te6-te8):
  dies, GE's ending camera + exit state 1 from ~f1450, still cycling at f2000;
  the level's own end was not seen (te7 timed out, te8's gdb hung after f2000 -
  the previous pass saw the ending finish by itself ~700 frames in).
- 20-mission sweep (sweep.sh) with the final binary: identical to before.
- probes/ladwalk.py (player at X/Z/Y/ROOM, PHASES of from:to:stick:theta[:side]):
  lw1 before: walked across the hatch, stuck at z -923.7 in the lip; after:
  takes the ladder at z -1061.5, down to the floor (f655), and lw3 climbs back up
  onto the deck (f1420). Backwards (lwb), slow 0.3 stick (lwc), diagonal (lwd)
  all take hold. sw2 (strafe into the stair's side wall): before stuck at
  -2010.6 -1398.3; after slides at z -1381 (a radius) and down the stair.
  Dam (STAGE=0x15): dl75 down the room 75 ladder and back up onto the deck,
  dl72 and dl71 (face ladder) down.
- Open: sliding sideways off the ladder mid-way still lets go (stock PD
  ladders allow strafing; GE would hold Bond to the ladder tile) - he falls to
  the floor. Guards' own wall tests still ask only their rooms (same class of
  bug as the stair head, not seen for guards yet).

## 5. Security cameras

- Rotate / face backwards (231132 Bunker, 233118 Surface): FIXED, 6d4f81b9a
  (converter 73). CCTV tail (look pad, yleft, yright, ymaxspeed, maxdist) was
  never converted - all nought, cameras still and aimed at pad 0. Verified by
  numbers on Bunker (probes/cctv.py: 4 cameras, pads 88/95/80/63, sweep limits
  +40/-20, 145/0, 180/0, +-30 deg, the near one sweeping). No screenshot, Surface
  not checked in game.
- One shot in the lens (231244): FIXED + verified, c813882a5 (converter 74).
  Converted CCTV models were SKEL_BASIC; PD's x100 lens rule (GE's own) only
  applies to g_SkelCctv. Skeleton carried across. probes/cctv2.py (AIMY=16/28):
  casing hit 500 of 1000 (two to kill, as GE), lens hit -> cctvHandleLensShot,
  200 damage, destroyed at once.
- Surface camera now faces out from the cabin wall (shots_surfcam).

## Side effect to know about (converter 72)

Before 72 a converted floor gun's dualweaponnum was 0, so picking up a
dual-wieldable one gave an odd pair (gun + nothing) and could put it in the
left hand. Now such a pickup is a single gun, as a guard's dropped gun always
was. The user asked for GoldenEye's own pair rule: see section 6 (GoldenEye
pairs only what its setup pairs).

## 6. GoldenEye's pairs of guns (user: yes) - DONE, converter 75

**GoldenEye has no "second of a gun makes a pair" rule** (the section above
assumed it did). Its pickup (bondinv.c's bondinvAddWeaponByProp(), propobj.c's
propPickupByPlayer()) pairs only guns its **setup** pairs:
- two collectables joined by a PROPDEF_LINK (0x0e) record (prop.c's setup ->
  propweaponSetDual()): the first picked up is a single gun, the second the pair;
- a guard holding a gun in each hand (PROPFLAG_IS_DOUBLE, propobj.c's
  chr-attach -> propweaponSetDual()): his two dropped guns are such a pair.
A second of a gun that is no one's pair gives only its ammunition ("Picked up
some ammo."), or is left lying if the ammo is full. CAN_DUAL_WIELD is never
read by the pickup (only by the all-guns cheat). Perfect Dark's solo pickup is
the same code (inv.c's invGiveWeaponsByProp(); its "second makes a pair" branch
is multiplayer only), and IS_DOUBLE is PD's OBJFLAG_WEAPON_CANMIXDUAL (flags
are copied whole), so guards' pairs already worked. What was missing:
- ed432ee88 (**converter 75**): the link record was converted as a one-word
  nothing; now OBJTYPE_LINKGUNS (s16 offsets; gesolo.py too). Two in the ROM:
  Caverns' AR33s (inside two boxes) and Bunker 2's silenced PP7s.
- ed432ee88: the pair's second gun is worded as GoldenEye does, "Picked up a
  ZMG (9mm).", not PD's "Double ZMG (9mm)." - converted missions only
  (currentPlayerQueuePickupWeaponHudmsg(), !normmplayerisrunning).

ROM census (tools/geconvert, every Usetup*Z): guard pairs Bunker 1 Klobb x1,
Archives DD44 x2 + Klobb x3, Train ZMG x2, Frigate Phantom/D5K (four guns), Bunker 2 Klobb
x3, Aztec AR33, Egyptian ZMG x14, Jungle Xenia's RC-P90 + grenade launcher (a
mixed pair), Caverns ZMG x2, Surface 2 Klobb; links as above. **No grenade or
mine is ever paired**, and nothing in the rule reads weaponHasFlag(DUALWIELD),
so Akimbo cannot make one; fix/f3-ge-mines needs nothing from this.

Verified (probes/pair.py: duals/kill/list/pickw/pickc/free/msg/print; ALL=1
walks contained props):
- Caverns 0x66: linked AR33s -> single, then pair, "Picked up an AR33 Assault
  Rifle." both times; guard 19's ZMGs -> pair, "Picked up a ZMG (9mm)." twice;
  two single guards' ZMGs (Bond starts with one) -> "Picked up some ammo.", no pair.
- Dam 0x15: two guards' KF7s -> "Picked up a KF7 Soviet." then "some ammo".
- Jungle 0x62: Xenia's RC-P90 + GL -> mixed pair (right RC-P90, left GL).
- Bunker 2 0x64: the two PP7 (silenced) on the floor are linked.
- Defection 0x30 (PD): the same message call still says "Double Falcon 2."
- Scratch merge with fix/f3-ge-mines (worktree /home/sdg/wt/pairmines, branch
  scratch/pair-mines, not for merging): Bunker 1 guard 12's Klobbs pair;
  Statue Park grenades three times -> grenade then ammo, never a pair; the
  grenade and three mines have no DUALWIELD, PP7/Klobb/KF7/AR33 do.
- 20-mission sweep clean at converter 75 (sweep.out).
Open: GoldenEye does not draw the second gun on pickup; PD puts it in the left
hand when the right already holds that gun (kept). Not checked in a real
playthrough with the fire button (probe pickups).

## 7. Akimbo house rule: a second of any weapon makes a pair (user decided) - DONE

User, 2026-09-26: with **Akimbo on** (Akimbo = Everyone or Players and Sims,
modIsAkimboForPlayers()), picking up a second of any weapon already held makes a
pair, on PD missions and GE Plus missions alike, **GoldenEye's grenade and mines
included**; the GE Detonator (WEAPON_GE_DETONATOR, fix/f3-ge-mines) stays single.
With Akimbo off, section 6 stands (GoldenEye's setup pairs only) and PD solo is stock.
(Section 6's "Akimbo cannot make one" is superseded.)

- src/game/inv.c invAkimboPairsPickup(w): solo, Akimbo for players, modCanAkimbo(w),
  not gegadgetsIsGadget(w) (detonator, key, camera, modem, plastique, tank shells: items),
  holds a single and no pair. invGiveWeaponsByProp() asks it *before* the single is given
  and gives the pair *after* the link/guard-pair bookkeeping (so a linked gun's partner
  never keeps a pointer to the freed prop).
- src/game/propobj.c propPickupByPlayer(): a full gun's second is no longer left lying
  when it would make the pair.
- fix/f3-ge-mines 097d9a2c5: gegunsNeverPairs() is now the detonator alone (modCanAkimbo
  honours it); grenade/mine definitions keep no WEAPONFLAG_DUALWIELD, so Akimbo off never
  pairs them.

**PD before** (verified with the converter-75 binary, Akimbo on, Defection 0x30): a second
grenade / CMP150 / Dragon picked up solo gave ammo only (the pickup's "second makes a pair"
branch is multiplayer only; Akimbo only paired Start Armed's gun and the switch's carried
pair). **After**: pair. Akimbo off: unchanged. Multiplayer/Combat Sim: unchanged (its branch
already paired any weapon under Akimbo from a different pad).

Verified (rig ~/wt/f3gemission-run, pd-am.x86_64 = scratch/pair-mines merge of both branches,
probes/pair.py + new ops spawn:W (weaponCreateForPlayerDrop, the MP drop), skipcut, bt;
am.sh <tag> <stage> <DO> runs Akimbo off (save_base) and on (save_akon, Akimbo=2)):
- Dam 0x15: two guards' KF7s -> off: single + ammo; on: pair. Remote/timed/prox mines and
  grenades, two each -> off: singles; on: pairs. Detonator: modCanAkimbo 0,
  invAkimboPairsPickup 0, never a pair. (spawn:0x7f crashes in projectileTick - no model,
  playermgrGetModelOfWeapon -1 - a probe artefact: the detonator is undroppable.)
- Caverns 0x66, Akimbo off: linked AR33s single then pair; guard 19's ZMGs pair. Akimbo
  on: AR33s the same (Bond starts with a ZMG pair under Akimbo).
- Defection 0x30: on -> Falcon/grenade/remote mine/full-ammo CMP150 pairs; off -> stock.
- Throwing a pair (~/wt/f3mines-run, probes/mines.py + pair/qty ops and a throw log, Facility
  0x63): PD's dual throw alternates hands (right, left, right...), one ammo per throw.
  Remote-mine pair: 4 throws, then both hands go to the detonator (single), which sets
  them all off. Timed pair: alternating throws, 300-frame fuse, the second chains off the
  first. Grenade pair: alternating, each explodes; out of grenades the hands stay on the
  empty grenade as with a single one. No crash or stuck state.
- 20-mission sweep, Akimbo off and on (sweep.am.off / sweep.am.on): all 20 clean, no fatal/crash.

## Before merging

Converter 72 and 73 touch every mission's setup: run the 20-mission sweep
(build/gexrom/runall.sh or hdsweep/ab.sh) after a forced reconvert, and the C vs
Python parity check (should stay the known 210 diffs).

## 8. GoldenEye's detonator model: node type 0xf (converter 77) - DONE

GoldenEye's model node type 0x0f is MODELNODE_OPCODE_INTERLINK
(bondtypes.h ModelRoData_InterlinkageRecord: pos, pos2, scale). It draws nothing; GE
reads it only as a prop's depth sort (objecthandler.c) and, in the detonator, as switch
28 = the hinge axis switch 6 (the right hand) turns about to press the watch
(gunfire.c:829). Perfect Dark has no such node (filemodel.c maps 0x0f to -1), so
modelWalk() leaves it out like the 0x0d shadow; the prev-pointer check now accepts a
next that is a dropped 0x0d/0x0f. The draw side keeps the axis (fix/f3-ge-mines,
gegadgets.c).
- Scan of every GE prop, character and hand item in the ROM: only GtriggerZ (item 30)
  and GwatchlaserZ (item 23, the same file) have a 0x0f, each one childless leaf at the
  end of its chain. Nothing was silently skipped: an unknown node fails the whole
  conversion, and item 23 was (and stays) excluded as no gun of the port's.
- The item loop takes item 30 (`item > 30 && !soloGadgetItem`): files/Igx030Z.
- Python twin (gechr.py, gemodelconv.py) drops 0x0f too; it converts no hand items.
- Verified: standalone C 76 vs 77 output differs only by the new Igx030Z; Python 76 vs 77
  identical; C vs Python = the known 210 diffs + Igx030Z (C only). Draw/probe/sweep: see
  fix/f3-ge-mines HANDOFF "Detonator model". 20-mission 1800-frame sweep after a forced
  reconvert to 77 (~/wt/f3gemission-run/sweep77.sh, sweep.out.pair77): all clean.
