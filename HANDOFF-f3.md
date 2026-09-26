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
