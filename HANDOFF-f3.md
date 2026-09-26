# F3 handoffs, pass of 2026-09-26 (combined on merge/f3-0926)

One section per merged branch, newest merge on top; the 1fc1832d8 handoff
(fix/f3-hd-level-render + fix/f3-dam-guard-aim2) is kept whole at the bottom.

Index:
- fix/f3-ge-mines: F3 GoldenEye mines pass (2026-09-26), branch fix/f3-ge-mines
- fix/f3-ge-mission-logic: F3 pass 2026-09-26 (GE mission logic) - handoff
- 1fc1832d8 (fix/f3-hd-level-render, fix/f3-dam-guard-aim2): see the bottom

---

<!-- section: fix/f3-ge-mines -->
# F3 GoldenEye mines pass (2026-09-26), branch fix/f3-ge-mines

Report: /home/sdg/wt/f3-0926/20260925-230009-c0cd3262 (savantique, Windows, 718d5dc):
"odd bug that treats held mines as though they're being wielded akimbo. ability to
see function change (detonate) doesn't yet exist." Facility mission (tester 0x7a = our
0x63), bottling room, HD look, remote mine in the hand. User's call: mines work like
GoldenEye.

## What GoldenEye does (decomp /home/sdg/perfect-dark/007, oracle on 10.8.0.3)

- Held: one hand, nothing drawn (WEAPONSTATBITFLAG_HIDE_FIRST_PERSON_HAND), one ammo
  icon bottom right. Oracle: ~/dam-oracle/getrigger.py on 10.8.0.3, frames in
  /home/sdg/wt/f3mines-pics/oracle/ (sheet.png: remote mine held, then the detonator).
- Detonating has two ways, no "function change":
  - A+B together with ITEM_REMOTEMINE in the hand (bondview2.c:5045 and 5310,
    moveData.detonating -> trigger_remote_mine_detonation(), propobj.c:11517, plays
    WATCH_DETONATE_MINE_SFX 243). Perfect Dark kept this whole (bondmove.c, both
    control styles; PC: use + reload/weapon-back/radial).
  - ITEM_TRIGGER, "Detonator": a separate item given with the remote mines
    (propobj.c:10163 add_ammo_to_inventory, :10549 propPickupByPlayer), in the weapon
    cycle right after the remote mine (bondinvCycleForward takes items < ITEM_BOMBCASE),
    auto-selected when the last remote mine is thrown and the trigger released
    (gun.c:1116 autoadvance_on_deplete_all_ammo), drawn as Bond's two hands at the
    watch (gun.c:918, gunfire.c:474/526), and its trigger detonates
    (chrprop.c:1454 chraiCheckUseHeldItem).
- Fuse: thrown mines count THROWN_ITEM_TIMER_SOLO 300 / _MULTI 180 sixtieths (gun.c:2036,
  NTSC): the timed mine goes off, the proximity mine arms (then 250 units from the
  player or a guard, propobj.c:3517, 11628), the remote mine arms (a detonation sets
  it off at once anyway).
- No mine or the grenade is ever a pair (no CAN_DUAL_WIELD).
- Auto-advance after the last timed/prox mine goes to the next weapon with HAS_AMMO
  (the mines themselves lack it) - PD's bgun0f0a1a10() is the same test.

## Cause

GoldenEye's remote mine is a copy of Perfect Dark's (host WEAPON_REMOTEMINE), which holds
the detonator in the LEFT hand (WEAPONFLAG2_DETONATORHAND -> gunctrl.dualwielding):
the left hand was in use with the mine's number, so gehud.c drew the mine icon in both
bottom corners (the "akimbo"), and the detonate was PD's second function (B+Z), which
GoldenEye's HUD never names and which is saved per host (a player who left PD's remote
mine on "Detonate" could not throw GoldenEye's).

## Fix (commit on fix/f3-ge-mines)

- port/src/geguns.c gegunsOwnThrown() (at init and after a GE-X borrow): grenade + 3 mines
  keep WEAPONFLAG_THROWABLE (65607dda1 had cleared it with the hunting knife's), lose
  WEAPONFLAG_DUALWIELD and WEAPONFLAG2_DETONATORHAND; the remote mine loses its second
  function. New WEAPON_GE_DETONATOR = 0x7f (the LAST s8 weapon number), host Data
  Uplink, one function = a copy of PD's remote mine detonate (HANDATTACKTYPE_DETONATE),
  no ammo, undroppable. gegunsNeverPairs(), gegunsThrownFuse60().
- src/game/inv.c invGiveSingleWeapon(): giving WEAPON_GE_REMOTEMINE gives the detonator.
- src/game/bondgun.c bgunAutoSwitchWeapon(): out of GE remote mines -> the detonator;
  out of timed/prox -> next in cycle with ammo, else back (GE's autoadvance).
  bgunCreateThrownProjectile2(): GE mines' fuse 300/180.
- src/game/propobj.c playerActivateRemoteMineDetonator(): GoldenEye's watch beep (243)
  on a converted level.
- src/game/modoptions.c modCanAkimbo(): false for gegunsNeverPairs() weapons (since
  097d9a2c5 the detonator only - see "Akimbo pairs" below).
- port/src/gegadgets.c: the detonator is a gadget row (item 30, "Detonator"); draws
  GoldenEye's own model when the conversion has Igx030Z, else nothing (never the Data
  Uplink). src/include/constants.h: INV_CYCLEABLE includes the detonator.

## Verified (rig /home/sdg/wt/f3mines-run, probe probes/mines.py via go.sh; before-binary rig /home/sdg/wt/f3mines-before)

Facility mission 0x63, tester's spot (3034,-373,-3455 room 66):
- Before: right 0x76 + left 0x76 in use, dualwielding 1, two icons (reproduces the F3).
  After: left hand not in use, dual 0, one icon, fn1 none. N64 and HD look (save_hd:
  XblaGoldenEye=1 + XblaMeshes=1; HD draws the release's mine in the right hand as before).
- Throw 5 remote mines -> the 5th throw auto-selects 0x7f; its trigger detonates all.
- A+B (injected into osContGetReadData) with the remote mine in hand detonates.
- Timed mine explodes ~312 frames after the throw (300 fuse + throw); prox arms at 300
  and goes off when a moving guard is 120 away. Thrown mines rest on the floor (y -370
  over -372): d2cebc2e5's NaN-muzzle fix holds.
- After the last timed mine: back to the PP7 (GoldenEye's HAS_AMMO skip of the mines).
- Akimbo on: modCanAkimbo / weaponHasFlag(DUALWIELD) 0 for 0x73-0x76, 0x7f.
- PD's own remote mine (0x22) unchanged (detonator hand, B+Z); a stock PD stage (0x09)
  runs 600 frames. gegunsDump diff: only the intended flag/function changes + 0x7f.
Pictures: /home/sdg/wt/f3mines-pics/final/ (before_*, f_* N64, h_* HD, *_sheet.png).

## Open

- **Converter (not mine to change): the detonator has no model.** Converting item 30
  (GtriggerZ) fails the whole conversion with "GtriggerZ: node type 0xf" (tried in a
  scratch build with geconvert.c's `item > 29` made `item > 30`, not committed). Once
  geconvert.c reads node 0xf and writes files/Igx030Z, gegadgets.c draws it; its
  placement row in g_Hands ({4,-12,-30}, width 20) is a guess to check then. Until
  then the detonator shows an empty hand and no ammo icon.
- ~~Pair rule on fix/f3-ge-mission-logic~~: superseded, see "Akimbo pairs" below.
- HD look draws the release's mine model in the hand (all three), GoldenEye draws
  nothing; left as it was (user's call).
- Mines thrown into a burning tank area go off early - also on the old binary.
- Simulants never use the detonator (stock bots never detonate remote mines either).

## Akimbo pairs the grenade and mines (user, 2026-09-26) - 097d9a2c5

The user's house rule: with Akimbo on, a second of any weapon held makes a pair,
GoldenEye's grenade and mines included; only the detonator stays single.
gegunsNeverPairs() is now `weaponnum == WEAPON_GE_DETONATOR`; the grenade/mine
definitions still have no WEAPONFLAG_DUALWIELD (Akimbo off: never a pair, as GoldenEye).
Consequence: with Akimbo on, weaponHasFlag(DUALWIELD) and modCanAkimbo() now say yes for
0x73-0x76 (Start Armed with a mine would hand two). The pickup rule itself is on
fix/f3-ge-mission-logic (inv.c invAkimboPairsPickup(), HANDOFF section 7 there), tested
in the scratch merge scratch/pair-mines (~/wt/pairmines, not for merging).
Throwing a pair (Facility, this rig, probes/mines.py ops pair:W / qty:W + a throw log):
PD's dual throw alternates hands, one ammo per throw; the remote-mine pair's last throw
goes to the (single) detonator, which sets them all off; timed pair 300-frame fuse;
grenade pair alternates and explodes. No crash or stuck state.
Merge note: both branches add HANDOFF-f3.md (add/add conflict) - keep both texts.

## Detonator model (converter 77 on fix/f3-ge-mission-logic) - 29fb8c20e

Converter 77 (1e37e3ff2, fix/f3-ge-mission-logic, which owns the converter) leaves
GoldenEye's interlink node 0x0f out and writes Igx030Z. The draw is here, because
WEAPON_GE_DETONATOR and its g_Hands row live on this branch; the two branches touch
disjoint files (except HANDOFF: keep both texts). gegadgets.c's new `press` field sits
after `def` so it does not conflict with mission-logic's removal of `lastweapon`.
- Pose: a g_Hands row with width 0 = GoldenEye's own pose, not measured and fitted: model
  at its own size under the host root's rotation (sway) times gunfire.c's ITEM_TRIGGER
  turn (D_80035C70), root at trigger_stats PosX/Y/Z (-2, -21.5, -19) plus the host's
  motion. Matches the oracle frame (trigger_060) to a few pixels at 4:3.
- Press: switch 6 turns about the interlink axis (20.21, 32.67, -18.41), -5 degrees at
  rest, 0 while the trigger is held (GE's field_A84 rates). Cuffs 29-34: one worn, chosen
  as the watch arm's (geWatchCuff(), new in gewatch.c).
- Crash found on the way (fixed, src/lib/modelasm_c.c): with no animation,
  modelasm00018680() read a position node's flags from sp00[part], cleared only for
  i < nummatrices; GE's parts run past that (pressing hand part 4 of 4 matrices), so stack
  garbage sent it to anim->animscale with anim NULL (SIGSEGV, depended on the build's stack).
  Now 0 in the port. May be what gave the older "grenade/mines garbage matrices" note.
- Verified (scratch/pair-mines = both heads merged, fa42d3b2e; ~/wt/f3mines-run,
  go43.sh = 1024x768, probes/mines.py + new op hold:N): Facility 0x63, 5 remote mines
  thrown -> 6th trigger is the detonator -> all 5 explode; the detonator drawn N64 and HD
  look, rest vs press differ in the right hand only; no SIGSEGV. Pictures
  ~/wt/f3mines-pics/detonator/ (oracle_n64_hd.png, press_cmp.png).
- 20-mission 1800-frame sweep of that merge after a forced reconvert to 77: all clean.

---

<!-- section: fix/f3-ge-mission-logic -->
# F3 pass 2026-09-26 (GE mission logic) - handoff

Branch `fix/f3-ge-mission-logic` (worktree /home/sdg/wt/f3gemission), based on
dabs-mod 5ac4c3176. Not merged, not pushed. **Converter now 78** (see 8, 9; was 76 here) (was 71; 72 to 76
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

## 9. Facility: conveyor escape + gas cloud (F3 20260925-235430, user: like GoldenEye) - DONE, converter 78

Tester (HD look, standing at the belt): "conveyer belt isn't accessible as an escape
route, cannisters that have been destroyed do not cloud the area with a visible poison mist."

**Conveyor (2f1c8a013, no converter change).** The ending is setup ai_47: Bond in the
room of pad 0x135 (309, room 70, the conveyor tunnel) or 0x87. GoldenEye has no
moving belt; its belt tiles (-266, rim -253) are joined to the bottling-room floor (-372)
by upright tiles, i.e. *links*, and bondviewTryMoveToStan() walks Bond into them and
lifts him (only refusal: stanTestLocusEdgeAboveY(), an edge > eye + 175). The tunnel
tiles are special 1 (force crouch). Ours: the conversion's climb wall on that link
(climb 119 > WALL_CLIMB 60) plus the upright tile's own geometry (flags 0x1b) stopped
the player 3.8 short of the rim - PD steps up nothing over manground + 30.
- gestan.c geStanClimbFloor(): highest floor of the tiles flood-linked to the one
  underfoot within the radius that the target circle touches, only for a move *into*
  it (nearer by >= half the move; brushing along the belt's side lifts nothing).
- bondwalk.c bwalk0f0c63bc(), remake stages only, not on a ladder/falling/in the tank:
  floor > manground + 30 and <= eye + 175 and bwalkTryMoveUpwards() clear -> manground,
  ground and sumground set to it (a snap; GoldenEye eases, PD's box cannot).
- Verified (probes/realwalk.py, convwalk.py; OBST=1 prints the blocking geo): head-on
  and diagonal walks lift at the rim, walk the belt, crouch in the tunnel, room 70 ->
  aiEndLevel (objectives incomplete: mission failed screen); with ALLDONE=1 (objective
  check forced true) Bond's own outro plays on the belt, then aiEndLevel - N64 and HD
  (save_hdmesh). Walk parallel along the belt side: ground stays -372.
- **Behaviour change to know about:** this is GoldenEye's rule everywhere on converted
  levels - any linked ledge/sill/deck up to eye + 175 is climbable by walking into it,
  including Dam's tower decks 317 over the treads (the climb walls stay for everyone
  else). Not walked on Dam in this rig (realwalk on 0x15 did not move at all, before
  or after - the mission start; use build/gexrom's stair scripts).

**Gas (df8980eae, converter 78).** GoldenEye: a GASBOTTLE reaching destroyed level 1
calls init_trigger_toxic_gas_effect(); handle_gas_damage() fades the fog with
fogSwitchToSolosky2(timer / 3600) toward g_EnvironmentAltp (the fog row after the
level's: Facility id+100 = far 5000 -> 1000, colour 0x102010 -> 0x408040), coughs from
600 ticks, 0.125 damage every 225 ticks from 1800, Bond only (guards untouched). PD
kept all of it (gasReleaseFromPos/gasTick), but a mod stage's transition was fog -> same
fog, so nothing was visible (the cough and damage did happen).
- Converters write the level's +100 row as the mission's ` altfog "..."` (C
  romFogAltRow(), Python gerom fog_alt_rows(); Train, Facility, Aztec, Egypt; maps
  block unchanged). C vs Python lines identical for ark and cryp.
- modloader.c parses `altfog` (missions block), modloaderGetStageFogAlt(); env.c uses
  it as g_EnvTransitionTo; envGetTransition() exposes the fraction; gebeanstage.c's
  HD fog shrinks its end by the same far ratio and lerps to the alt colour.
- Verified (probes/gasprobe.py: objDamage on the 10 tanks, logs timer/frac/sky/health;
  TIMER= jumps the clock): frac 0 -> 1 over 3600 ticks, sky 102010 -> 408040, health
  1.0 -> 0.84 by 4000 frames (first hit at ~1860), N64 and HD screenshots: the room past
  ~1000 is solid green, near walls tinted (HD strongly).
- Forced reconvert to 78 + 20-mission 1800-frame sweep: all clean (sweep.out).
- Open: Egypt's gas_leak_and_fade_fog (0xfb, fog only) is still dropped by the
  converter (geaitable.py None) - its alt row now converts, so mapping it to a port
  command that calls gasReleaseFromPos() with no damage would finish it. PD's
  "visual only" check is on STAGE_MP_G5BUILDING, not a converted Egypt. Not compared
  with the native GE oracle (decomp is unambiguous for both).

---

<!-- section: 1fc1832d8 (fix/f3-hd-level-render, fix/f3-dam-guard-aim2) -->
# F3 HD level rendering pass (2026-09-26), branch fix/f3-hd-level-render

Reports: /home/sdg/wt/f3-0926/. Tester stage ids differ from ours: tester 0x86 = our
0x6f (Bunker, GE `sev`), 0x7a = 0x63 (Facility), 0x82 = 0x6b (Silo), 0x75 = 0x5e
(Runway), 0x15 = 0x15 (Dam). All are GE Plus solo missions, HD look.

Rig (all outside the tree):
- run dir `/home/sdg/wt/f3hdlevels-run` (own copies of cache + mods; saves
  `data/save_sw_hd`, `data/save_sw_n64`; the ROM is a symlink)
- `/home/sdg/wt/f3hdlevels-rig/views.sh STAGE TAG` with `VIEWS='x,y,z,theta,verta,room;...'`,
  `GROUND=` (manground; set it or the player lands on the wrong storey),
  `CE=1`, `N64=1`, `PRECMD="gdb cmd;;gdb cmd"` (before the shot), `SHOTS2="cmd;;cmd"`
  (one extra shot per command, each after its command). Pictures go to
  `/home/sdg/wt/f3hdlevels-pics/`.
- `ab.sh BIN TAG stages...` (HD look, frame 400) + `abdiff.py` (ab-base vs ab-fix1).
- `ray.py` / `near.py`: ray-cast / list Bean level triangles from a dump. The dump
  needs a temporary hunk in build() (not committed): write `c.num` then every
  `struct stri` (80 bytes) to `$GEBEAN_DUMP` just before the `lists[r] = ...` loop.
  Dumps: `tris_0x6f.bin` (Bunker), `tris_0x15.bin` (Dam). `pd.dump` = binary with it.
- `gate.py`, `gate2.py`, `gate3.py`: Dam opening (no --skip-mission-intro); the gate
  shot is frame ~925 with --fixed-step --rng-seed 1.
- `lamp.py`: sets Facility's door console (singlemonitor at -3533 -236 1521) to
  GoldenEye programme 46.

## 1. Bunker z-fighting / "inconsistent wall" (231104, 231159, 231244) - FIXED + VERIFIED

Commit f99bb4e79. Cause: markDecals() picked the smaller of an overlapping coplanar
pair as the decal. Bunker's hammer-and-sickle plaques (tex 31) and vent grilles
overlap a wall panel (tex 4) and hang past it; the panel's half-quad was smaller, so
it became the decal, its other half did not (fought the plaque), and the part of the
panel off the plaque, drawn in the no-depth-write decal mode, was painted over by
the back-facing rock of room 14 drawn later (the brown triangles). Fix: a triangle
lying wholly on other pictures (middle, pulled-in corners and edge middles) is the
decal of a pair where the other is not; solid decals also write depth (Z_UPD).
Verified at all three report cameras (pics bunker-cmp-0..2.png: plaques + vent grille
whole, holes gone). HD A/B sweep of all 20 missions (frame 400): 18 identical, Train
0x60 (a dashed seam line on a wall gone) and Archives 0x65 (seam at an emblem's edge)
changed, both for the better (abdiff-0x60.png, abdiff-0x65.png). N64 look not swept:
gebeanstage.c serves rooms only in the HD look.

## 2. Silo monitors z-fighting, spiral screens (233641) - ALREADY FIXED (a97daeb6e)

Tester's 718d5dc predates a97daeb6e (console2/console3 placeholder spiral quads
dropped). At the report camera on 5ac4c3176+ the four screens show programmes, no
spiral, three frames identical (silo-base-*.png).

## 3. Facility door console red square (225534) - NOT REPRODUCED, open

The streaks at the top of the red lamp are Bean's placeholder spiral (doorconsole
draw 10, vb 0x300, tex 5, 2 triangles at x 78..91, y 158..170) peeking above the
programme's quad. At the report camera, frame ~400 and with the console forced to
programme 46 (lamp.py) the spiral never shows (fac-lamp-crops.png): the node is
handed back (xblaMeshNodeIsLiveScreen()). Not found what frame state lets both draw
on the tester's frame 6534 (door opened/used by then). Next: drive the console's
door (AI sets 46/47 on activation) and watch xblaMeshNodeIsLiveScreen() for the
console; or drop doorconsole's spiral quad the way a97daeb6e drops console2/3's
(beanVertexDrops in gebean.c, vertices of vb 0x300) - but only if the hole stays
covered when no programme list is written.

## 4. Facility windows solid blue at distance (225628) - NEEDS USER DECISION

GoldenEye's own tinted-glass fade (glassCalculateOpacity(), the same function in the
007 decomp): past opadist a pane is drawn opaque in its tint. The Glass See-Through
slider (bdc053f3d, 5ac4c3176; default off, Dab's Settings 50) is newer than the
tester's 718d5dc. With 50 the far pane is lighter (fac-glass-cmp.png) but still reads
as blue glass. Question for the user: should GE Plus HD windows stay clear at any
distance by default (as the release may), or is the slider enough?

## 5. Runway door to Facility: sky in gaps round it (230105) - FIXED on fix/f3-runway-door-gaps (see the section of that name below)

At the report camera on current code the double door (modelnum 0x29b, two props at
(-3684 89 9467) and (-3583 89 9467)) shows sky along its top and right edges
(runway-door-0.png): nothing is behind it and the HD door mesh does not fill the
frame. HD only: in the N64 look the same camera shows one plain door filling the
doorway with no sky (runway-door-n64-0.png). Next: compare the Bean door mesh's bbox with the N64 model's (setup scales doors to
their pad bbox; if the HD mesh's extents differ from the N64 model's, scale the HD
mesh to the N64 bbox).

## 6. Dam opening gate is a blue void (234249) - REPRODUCED, cause narrowed

gate-cmp.png (HD), gate-n64.png (N64 shows a rusty gate at the tunnel end). The gate
is door prop modelnum 690 at (16792 13313 25803), room 135 - the cutscene camera's
own room. Room 135 is onscreen in HD (bgRoomIsOnscreen 1, g_BgPortalSeen 1), but the
door's prop flags are 0x4 in HD against 0xc6 in the N64 look: the prop is never
flagged on screen in HD. Drawing the 39 hidden kept rooms changes nothing (not
gebeanStageRoomHidden). Next: find why the door's onscreen test fails in HD
(objTick/prop visibility for doors - bbox from the HD mesh? the door's room list?);
gate2.py/gate3.py are the probes (frame 927).

## 7. Dam white patches on the ground by the truck park (234751) - CAUSE FOUND, not fixed

They are the release's own painted markings: 18 triangles of tex 61
(_0x030D1495, DXT5, white RGB, alpha = a dashed curved line), vertex colour
0x88e6ca89 (tan, alpha 0x88), rooms 111/121/122, all marked decal. The release would
draw them tan at about half opacity; we draw them opaque pure white, so the vertex
colour and alpha are lost on this path (alpha decal leaf: XLU decal mode, combiner
fc26a004 1f1093ff). bg.c's lighting (dlights.c) keeps the palette's alpha, and the
palette is ruled out (see 9). Next: as 9.

## 8. Dam sniper zoom: flat blue polygons over the mountains (235312) - REPRODUCED, cause narrowed

Reproduced with zoominfovy/old/new = 7 at the report camera (dam-sniper-cmp.png,
dam-sniper4-0.png). Not the backdrop (numBackdrop = 0 changes nothing). The island hut
turns into a pale ghost while zoomed: the fog grows with the zoom (about 1/0.1156),
so far rooms go solid fog colour (light blue) and some go dark blue. Suspect the
fog-line depth (env.c gebeanStageFogLine(bgGetScaleBg2Gfx(), ...) and the renderer's
G_FOGLINE_LINEAR_EXT depth) picks up the projection's fov scale. Next: print fm/fo
and the renderer's fog depth at fovy 60 and 7 for one vertex.

## 9. Dam white see-through pyramid in the server room (235806) - CAUSE FAMILY FOUND, not fixed

Reproduced (dam-fix2b-0.png). Same family as 7: 8 triangles of tex 59
(_0x0EDC9FE5, 32x32 DXT5, a white-to-grey gradient with alpha 250 -> 6), vertex
colour 0x82ffffff, room 109 - the release's lamp light cone, meant to be a faint
glow; we draw it near solid white. Report camera (10673.9 12736.7 10409.2) theta
236.3 verta -10.7, room 108; GROUND=12578 (without it the player lands a storey up).

Ruled out for 7 and 9: the room palette (buildPalette()/paletteIndex() merging the
0x82/0x88 colours into opaque ones). Seeding a palette entry per alpha level and
weighting alpha x64 in paletteIndex() changed neither picture (tried, reverted).
The leaf combiner (fc26a004 1f1093ff) takes SHADE colour and alpha only in its
second cycle - so a one-cycle draw of these (or a render mode swap that leaves the
list in one cycle) would give exactly texel white at texel alpha. Next: log the
cycle type and render mode in force when tex 59's triangles are drawn (gfx_pc
trace on room 109), and whether nofog/xlu paths put them in one cycle.


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
