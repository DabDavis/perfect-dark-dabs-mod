# Weapon behaviour belongs on the weapon

## Digest (moved from CLAUDE.md, 2026-09-30)

The entries CLAUDE.md carried for this note, verbatim. The sections below are
the long form.

- **The CamSpy launched into a wall** — a launch tests only the player's line of sight to the spot, so a wall beside the line put the CamSpy's body (radius 26) into it and every move after collided: it could not fly on or change height (pre-existing, every CamSpy mission). `eyespyFitLaunchPos()` finds the nearest spot the body fits in. See "The CamSpy's launch spot".
- **Weapon numbers, `flags2`, converting a `weaponnum` comparison** — [weapons.md](CLAUDE-notes/weapons.md): the four checks, and what is deliberately not converted; a launcher branch keyed on the number must test the function's type, or a mod's table crashes it; and **weapons past the stock table** (2026-09-15, GoldenEye's guns 0x5e-0x76): every switch or `==` on a weapon number asks `weaponHost()`, range tests were decided one by one because some guard arrays indexed by the real number, and the check that stock is unchanged is a seeded match compared pixel for pixel


The game decides a lot by comparing the weapon number - `if (weaponnum ==
WEAPON_SHOTGUN)` - which is a question a mod cannot answer, because a mod that
brings its own guns numbers them its own way. GE-X patched 28 functions in
`bondgun.c` and 31 regions in `propobj.c` for exactly that reason, nearly all of
them number swaps; `tools/modcodediff` says which functions a given mod cares
about.

So the behaviour moves onto the weapon:

- `struct weapon.flags2` - a second flags word, the first having all 32 bits
  spoken for; and `.flags3`, since the second filled up too (2026-09-07). 35
  behaviours so far, read with `weaponHasFlag2()` / `weaponHasFlag3()`; the
  name table in mod.c says which word each is in.
- `struct weapon.pickupsound` and `.unequippedreloadindex` - where the answer is
  a value rather than a yes.
- `struct weaponfunc.flags` - for what belongs to one *function* of a weapon
  rather than the weapon. The Dragon is a rifle until you throw it down and then
  it is a mine. Read with `weaponfuncHasFlag()`, or `gsetHasFunctionFlags()`
  where a `gset` is to hand.

A modconfig `weapon`, `weaponfunc` or `tvscreen` block sets any of them:

```
weapon 15 { unequippedreload 1 unequippedreloadindex 1 pumpaction 1 }
weaponfunc 21 1 { proximitymine 1 }
tvscreen 5 { sameas 3 }
```

## Converting another one

`bondgun.c` still has around 60 of these comparisons and `propobj.c` around 63
(2026-09-07; the `case WEAPON_X:` labels are another 170 and are dispatch, below).
Three checks before the edit, one after. Each of them has already caught a
silent bug.

1. **Is the weapon definition shared?** Three are: `invitem_keycard` by eight
   numbers, `invitem_hammer` by four, `invitem_rocket` by the rocket and the
   Skedar rocket. A field on the definition cannot tell those apart, and the
   rocket pair genuinely want different answers - opposite ones, in
   `objTestForPickup`. Those tests stay keyed on the number, with a comment.

2. **Is the *function* definition shared?** Ten are, and this bites harder.
   Flagging the five functions that leave a proxy also flagged the timed mine,
   which shares its threat detector with the proximity mine. "Is a proximity
   mine" ended up a weapon flag for the mine and a function flag for the three
   that only become one on their second function.

3. **Does the flag's set differ from the list *inside its enclosing
   condition*?** A flag can be exactly right in isolation and still change
   behaviour. Substituting `FUNCFLAG_PROXIMITYMINE` for the Dragon clause in
   `objDamage` would have armed the N-bomb, which carries that function flag but
   is not on the explodes-when-shot list. `FUNCFLAG_WALLHUGGER` would have set
   the Devastator hugging walls, because the wall hugger function is the
   launcher's own and already carries `FUNCFLAG_STICKTOWALL`.

4. **Afterwards, dump the sets and compare them against the lists you replaced**,
   resolved through the `g_Weapons[]` designators so a shared definition shows up
   as all of its numbers. For a function flag, enumerate all 188
   weapon-and-function pairs - a shared function is invisible from the weapon
   side. For a mapping, read the old chain back out of `git show HEAD:` and
   compare entry by entry.

   This is not optional. Appending a second `flags2` initialiser to a weapon that
   already had one is not a duplicate, it is the next field: that put
   `WEAPONFLAG2_LANDSONHIT` into `unequippedreloadindex` and gave the remote mine
   a reload index of 32, and it built cleanly.

`g_Weapons[]` is written with designated initialisers - `[WEAPON_SHOTGUN] =
&invitem_shotgun` - so a behaviour can no longer land on the wrong gun by
miscounting, with a `_Static_assert` tying its length to the enum. It was a
positional list until the first of these conversions.

## What is deliberately not converted

**Dispatch is not behaviour.** `objLand` picking `boltLand` or `knifeLand` by
weapon number, and the `case WEAPON_X:` labels in `bondgun.c`, are jump tables.
A function pointer on the weapon would do it and would be a different kind of
change - moving code identity into data rather than parameters.

**A weapon-and-function test keeps its function half literal.** `weaponTick`'s
grenade fuse is `FUSETIMER && gunfunc == FUNC_PRIMARY` (2026-09-07): the
number was the mod's to renumber, `FUNC_PRIMARY` is not, so the flag replaces
only the number and the function test stays beside it. The same shape did the
timed mine (`TIMEDFUSE`), the remote mine (`REMOTEDETONATED`), the grenade's
held time coming off its fuse and its secondary's bounce (`PINBALL`). What stays
by number there: the grenade round's and the rockets' ticks (projectile numbers
no mod renumbers), the N-bomb's storm (GE-X leaves 31 alone everywhere), and
the Devastator's wall hugger. Where a mod renumbers *both* halves - GE-X's
Moonraker streams from its primary, so the laser's `29 && 1` became `22 && 0`
at three sites - the test is a function flag, `FUNCFLAG_LASERSTREAM`, read as
(weapon, function) pairs by a `FUNCFLAG_SITES` row and written as a
`weaponfuncflags` block (mods.md, "The laser's stream is a function flag").

**Some are one weapon with one quirk** whose intent is not visible from the
surrounding window - the remote mine's left-hand rule before it was understood
as the detonator hand. Naming those from a guess is worse than leaving the
comparison in place. The combat knife's sites in the hand state machine were
this until read together (2026-09-07): four places that are one reload quirk,
`WEAPONFLAG3_KNIFERELOAD`.

**A list that is the function types can be the function types.** The bots'
throwable list was exactly the weapons with a throw function, and
`botactIsWeaponThrowable()` asks the table now - with the mines' rule kept
(a weapon thrown by its primary is throwable whichever function is asked
about), because the first draft lost it and the dumped set said so.

## A number does not say what the function is

`chrTickShoot()` decided "this is a launcher" from the weapon number and cast
`functions[weaponfunc]` to `weaponfunc_shootprojectile`. With stock's table that
is true by construction; with a mod's it is not: GE-X puts its timed mine in the
Crossbow's slot (0x1b), a throw function 0x24 bytes long, and the cast read
`projectilemodelnum` out of whatever followed it. Guards Alerted! with Random
weapons rolled that slot on GE-X's Runway, and the guard's first shot crashed in
`setupLoadModeldef()` (the tester's backtrace) or, when the garbage happened to
be a mapped address, built a projectile with no bbox and crashed in
`projectileTick()` a tick later - one bug, two faces (2026-09-06).

The rule: a branch keyed on the number tests the function's type before it
casts (`chrGetProjectileFunc()`; `bgunCreateFiredProjectile()` already did),
`weaponCreateProjectileFromGset()` refuses a model number outside
`g_ModelStates`, `bgunCreateThrownProjectile2()` refuses a function that is not
a throw, and the guard gun list goes through `modAlarmCanChrFire()` - the
primary function must shoot - because the list is stock numbers and the AI has
no attack for anything else.

The Reaper's mechanics are `WEAPONFLAG2_MINIGUN` (2026-09-06): the barrel
spin (`bgunUpdateReaper()`), the trigger-held spin-up in the melee state and
the switch into it, the shot every third burst tick, the three muzzles and two
eject parts, the aim jitter, the smoke, the grinder's boost scale and the
simulant spin-up. GE-X's Gold PP7 sits in the Reaper's number (0x14) and spun
in the hand with green smoke; its Reaper is at 0x23, at the stock Reaper's
address, so the import's by-address inheritance gives it the flag. The switch
into the secondary now also asks that a secondary exists - a mod's minigun has
no grinder. Still keyed on the number, as jump tables: the equip sound in
`bgunTickIncChangeGun()`, the spark colour in `propFindAimingAt()`, the beam
list in `bgunCreateFx()` and the chr weapon lists.

**The hit sounds are three flags, read out of the mod's code by running it**
(2026-09-07). `bgunPlayPropHitSound()` and `bgunPlayBgHitSound()` picked a
sound by number: the knife and the bolt ring (`WEAPONFLAG2_BLADEHIT`), the
laser crackles (`WEAPONFLAG2_LASERHIT`), and unarmed or the secondary function
of five pistols lands a blow (`WEAPONFLAG2_BLUNTMELEE`, with the function in
use being melee - `bgunIsBluntMelee()`; the type alone would have taken the
Reaper's grinder and the Tranquilizer's injection, check 3). GE-X renumbered
the first two (knife 26 -> 2, laser 29 -> 22, its Moonraker at the Dragon's
address, which inheritance by address could never have flagged) and rewrote
the pistol list to {3,4,5,17,19,20}, every one a pistol whose secondary is a
melee function. Following constants through a rewritten list is not a
reading, so the importer runs the function for every weapon number, on a chr
with either function and on an object, with the sound tables it copies to its
stack seeded with markers (`HITSOUND_SEEDS`), and reads which sound it stored
- the chr branch's in one stack local, the object branch's in another. The
result is `weaponflags FLAG { clear N... }` lines for the whole table, a
block that exists for this: a flag *list* rather than a weapon's flags, and
`clear` because a slot that sits at a stock definition's address inherits its
flags and the mod's code may not agree. GE-X's chr branch gives the blade to
nobody (its knife thumps on a chr and rings on a wall); one flag cannot say
that, so the flag follows the object branch and the report says so. Slots
41-43 share slot 3's definition and get its flag with it.

**A flag's list can be read from the mod's code** where the game tests one
number (2026-09-07): `FLAG_SITES` in both importers names a flag's sites as
(function, stock constant), the compare chain at each is read
(mods.md, "One weapon that became two"), and `weaponflags FLAG { clear ...
}` is written when every site agrees. Pump action, the small pistols'
casing (`WEAPONFLAG2_PISTOLCASING`, the four in `casingCreateForHand()`)
the magnums' no-eject (`NOCARTEJECT`, two sites), the sticks-where-it-lands
list (`STICKSTOWALL`), the thrown blade's four embed sites in
`projectileTick()` (`BLADEHIT`), the shotgun's distance falloff and the
FarSight's shield-piercing in `chrDamage()` (`SHOTGUNDAMAGE`,
`PIERCESSHIELD`), and the laser's beam in `beamRender()` and `beamCreate()`
(`LASERBEAM`, `CROSSBEAM`, `LASERFLIGHT`; the Cyclone's `FAINTTRACER`), the
shotgun's pellets in `handTickAttack()` and the simulants' laser clip and
RC-P120 cloak in `botTickUnpaused()` (`PELLETS`, `BOTLIMITLESS`,
`WEAPONFLAG3_CLOAKAMMO`), and the shot's no-bullet-hole, through-walls and
no-sparks tests in `shotCalculateHits()` and `objHit()` (`NOWALLHIT`,
`WEAPONFLAG3_XRAYSHOT`, `WEAPONFLAG3_NOSPARKS`) are read that way;
the chargeable flag's two sites disagree in GE-X and it is reported
instead. Adding a converted site to that table is how a mod's list reaches
the flag. The Reaper's two casing tests in `casingCreateForHand()` are
`MINIGUN` and are not in the table: GE-X zeroed them, as it did most of
its Reaper sites, and the flag stays on the definition on purpose.

**The thrown weapons' kinds and the gun models' updates are flags3**
(2026-09-07, mods.md "The number sites: weapon_tick ..."): eighteen of them,
each a `FLAG_SITES` row, every one of GE-X's renumberings in `weapon_tick`,
`bgun0f0a5550`, the two thrown-projectile functions, `obj_damage` and
`bot_is_obj_collectable` read. Two more shapes a row can name besides a chain:
`'imm'`, one immediate at an address (a constant hoisted into a register for
the whole function, or loaded in the delay slot of the branch that skips its
body - `weapon_tick` does both), and `'range'`, an `slti` pair (the Falcon 2
laser sight's `2 <= w < 5`). And `FLAG_BYNUMBER` takes out of a chain what is
not the flag's: a shared definition the port keeps testing by number (the
rocket in `obj_damage`'s list, the Skedar rocket in the bots'), or another
flag's test sharing the body (the Reaper's, `MINIGUN`, before the shotgun's in
`bgun0f0a4e44`). The per-weapon model updates (`SHOTGUNMODEL`, `SNIPERSCOPE`,
`LOADSLIDE`, `REVOLVER`, `HELDROCKET`) are the `MINIGUN` precedent: code
identity as a flag, because the alternative leaves GE-X's pistol at 19
animating as a shotgun. `HELDROCKET` tests the function's type before its
cast, like `chrGetProjectileFunc()`.

Reproduce a guard fight headlessly: copy the tester's `pd.ini` (Guards
Alerted!, Random, Akimbo) into the scratch savedir, boot Runway under gdb with a
Python breakpoint on `weaponCreateProjectileFromGset` that prints the calling
`chrTickShoot()`'s `gset` and the chr's `weapons_held`, and fire now and then
with a real mouse press; the guards arrive and shoot within a minute.

## How a mission arms the player, and why Start Armed has to go through the script

Three things put a gun in the player's hands at a mission start, in this order,
and only the last one sticks:

1. `playerReset()` reads the intro's `INTROCMD_WEAPON` commands into the
   inventory and `g_DefaultWeapons`, then calls `player0f0b9a20()`, which
   equips them. This runs **before** `playerSpawn()`.
2. `playerSpawn()`. In a match the `else` branch under `mplayerisrunning`
   gives Start Armed's gun; a mission never reached that branch at all, which is
   why Start Armed and Akimbo did nothing in solo until 2026-09-06.
   `playerSpawnWeapons()` is now called from both.
3. The stage script's `chr_draw_weapon` (`aiChrDrawWeapon()`, command 00ec),
   a few frames in, during the intro cutscene. The Villa has **no** default
   weapon (`g_DefaultWeapons[HAND_RIGHT]` is `WEAPON_UNARMED`) and draws the
   sniper rifle this way at frame 5; the command equips the right hand and
   empties the left, over whatever step 2 did.

So the spawn's choice lives in `player->spawnweaponnums[]` and
`aiChrDrawWeapon()` applies it instead of the script's gun when the spawn
changed the hands, or doubles the script's gun under Akimbo. The left hand is
equipped first: `bgunEquipWeapon2(HAND_LEFT, x)` records `leftwant`, and the
right hand's switch pairs against it; the other order carries the *previous*
right-hand gun into the left, which is the mixed-pair pickup behaviour.

Nothing re-equips at the cutscene's end. Trace it with gdb breakpoints on
`bgunEquipWeapon2` rather than reading for it - the frame-5 draw was invisible
from the code.

`mpGetSpawnWeapon()` now answers outside a match too: Random rolls every gun
(the unlock test is the Combat Simulator's), First Weapon is -1 there.

## Mission Respawn rides the co-operative restart

A solo death is `playerDieByShooter()`, the death animation, a fade to black,
then `mainEndStage()` from `playerTick()` (the "Handle mission exit on death"
block). `modrespawn.c` sets `dostartnewlife` there instead, and `lvRender()`
calls `playerStartNewLife()` for it - that call is not multiplayer-gated. The
solo death body lives in gunmem; nothing special takes it down, the ordinary
per-tick `playerRemoveChrBody()` does once `isdead` is false, and bgun takes
its memory back. What had to change in `playerStartNewLife()`: the position
(`posdie`, not the spawn pad), the inventory (the co-operative branch keeps
it), and the hands (`modRespawnGetWeapon()`, not another Start Armed roll).

Test it with `gdb -p PID -ex 'call (void)playerDie(1)'`; a death to respawn is
about five seconds. A HUD message of the default type lasts 80 ticks, so a
screenshot two seconds later misses it; `hudmsgCreateWithDuration()` is the
longer one. Reading `g_HudMessages[0].state` from an attached gdb showed 0
while the message was on screen - trust the screenshot, not that field.

## Weapons past the stock table (2026-09-15)

GoldenEye's guns are weapon numbers 0x5e-0x76 (`WEAPON_GE_FIRST`,
`NUM_WEAPONS` 0x77; ge-bean.md has the rest). Each stands on a Perfect Dark
host - its definition is built from GoldenEye's own row, drawn on the host's
model and run by the host's engine (ge-bean.md, "A GoldenEye gun's definition
is built, not copied from its host") - and **every test of a weapon by number
asks `weaponHost()`** (data.h,
static inline, identity for a stock number): a copy fires, sounds, animates and
aims as its host. What keeps the real number is what is the weapon's own - its
`g_Weapons` definition, name, model state, inventory item and pickup.

- **Switches and `==`/`!=` against a `WEAPON_` constant** were wrapped by a
  script (318 sites, everything in src/game but training*.c and invitems.c):
  a host is always a gun, so an item or placeholder test is unchanged by it.
  A new test of that kind wraps its number too. The script is
  `tools/weaponhost_codemod.py [--apply] FILES`: a switch whose own case labels
  name WEAPON_ constants, and either side of ==/!= opposite one, skipping
  NONE/UNARMED/MPLOCATION and comments, strings and `#` lines. Run without
  `--apply` it lists what it would wrap, which is 0 on a converted tree - a
  quick way to find a new test someone added by number.
- **Range tests were not scripted**: several guard an array indexed by the real
  number, and wrapping the test would let a copy overflow the array. Decided
  one by one: `gunfuncs` bits (the secondary-function choice) index by the
  host (`VALIDWEAPON()`, `FUNCISSEC()`, bondgun.c's three, the active menu's);
  "is a gun" (`<= WEAPON_PSYCHOSISGUN`, `<= WEAPON_RCP45` drop-on-death, pickup
  sound, text-override pickup, cycle back) asks the host; bounds that meant
  "a real weapon" (`> WEAPON_SUICIDEPILL` in setup.c, bondgun.c's equip and
  definition, botact.c, the deployable and drop-all loops) are `NUM_WEAPONS`;
  the All Guns cheat (`inv.c`, `<= WEAPON_PSYCHOSISGUN`) leaves the copies out;
  port/src/mod.c and moddata.c keep `WEAPON_SUICIDEPILL` (a mod edits stock).
- **Tables**: `g_AibotWeaponPreferences[]` is read at `weaponHost(n)`;
  `INV_CYCLEABLE()` lets next/previous weapon stop at a copy; the weapon wheel
  (`activemenu.c`) lists them; `playermgrGetModelOfWeapon()` gives each its own
  model state; `weaponsfound` (48 bits) and the firing range never see them.
- **Still keyed on the number with no answer for a copy**: anything that
  indexes by weapon number and was not listed above. `gunctrl`'s three
  weapon numbers are `s16` (they were `s8`, which capped the numbers below
  0x80 until the ROM hacks' pistols went there); a `gset` and a weapon prop
  keep a `u8`, so 0xff is the last number there can be.

Checked by a seeded fixed-step Combat Simulator match on the build before and
after: pixel-identical frames and identical simulant weapons and positions.


## GoldenEye's cheat guns (2026-10-10)

The owner's "as a toggle in weapon sets" (F3 pass 35, item 9): GoldenEye's
**Silver PP7, Gold PP7 and taser**, which its cartridge hands out only by
cheat (the Gold and Silver PP7 cheats, All Guns) and never in multiplayer, as
GoldenEye weapons of the port's.

- **Numbers** `WEAPON_GE_SILVERPP7` 0x84, `WEAPON_GE_GOLDPP7` 0x85,
  `WEAPON_GE_TASER` 0x86, after the ROM hacks' four pistols (0x80-0x83), so
  nothing before them moves; `NUM_GE_CHEATGUNS`. `GE_GUN_INDEX()`'s second
  range (past the gadgets) covers them with the pistols, and so does
  `INV_CYCLEABLE()`. Host: the PP9i, as the pistols'.
- **Definitions** built from GoldenEye's own rows like every other gun's
  (`tools/geguns/gen_gunstats.py` -> `gegunstats.h`: `silverwppk`, `goldwppk`,
  `taser`): the Silver PP7 a PP7 that does twice the damage and goes through 10
  (the PP7's penetration is 1), the Gold PP7 one that does 100 (a kill a hit),
  the taser 1 with no magazine and no AmmoType. Sounds 107, 107 and 100 (its
  rows' Sound); names "Silver PP7", "Gold PP7", "Taser"; held, the PP7s are
  GoldenEye's PP7 prop (`PROP_CHRWPPK`, which `getPropForHeldItem()` gives
  both) and the taser nothing (none there); on the floor the PP7s lie as that
  prop and the taser as its host's pickup.
- **The taser** fires as GoldenEye's does: no ammunition spent and none needed
  (`WEAPONFLAG3_FREESHOTS` - Perfect Dark's hand spent a round out of its empty
  clip, so `shotstotake` went to 0 and it fired nothing), no flash, lowered
  first and fired when lowered (`gegunsTriggerDelay60()` 16, gun.c's
  `taserFireKeyFrames`), held lowered with no second shot while the trigger
  is held (`gegunsOnePerPress()`, `bgun0f09aba4()`), and raised when it is let
  go (`taserRaiseKeyframes`, `gegunsOwnTaserTick()`, the knife's sampler).
- **Models**: GoldenEye's own first person models in either look, `Igx020Z`,
  `Igx021Z` and `Igx031Z` (geconvert.c no longer skips items 20, 21 and 31 of
  GoldenEye's own; a hack's loop is the other branch), always out of
  GoldenEye's conversion - on a hack's stage too, whose items 20, 21 and 31 are
  its own guns (`gegunsFindConverted()`) - and set wherever gebean.c sets the
  others' (`gegunsExtraModelsRefresh()`). Bean has `gun/silverPPK`,
  `gun/goldppk` and `gun/taser`; not wired (gebean.c's rows stop at the 25),
  so the HD look holds the N64 model, as it does the hacks' pistols.
  **Needs a converter bump to reach an install** (none made here: gfmission
  owns 125); until then they are drawn on the host, a PP9i. The same converter
  change gives items 20 and 21 their own weapons in GoldenEye's missions
  (`g_GeItemWeapon`: the PP7 before), where the only use is the ending
  cinema's check of what Bond holds (`if_chr_weapon_equipped(CHR_P1P2, ...)`:
  Archives, Facility, Caverns, Depot, Frigate, Jungle and Silo's, twelve
  setup files with the Japanese and revision-fix copies): a Silver or Gold PP7 is kept
  for the cinema, as the cartridge does, where the PP7 was tested twice.
- **Rows**: `MPWEAPON_GE_SILVERPP7` 0x4e-0x50, after the pistols' 0x4a-0x4d
  (`NUM_MPWEAPONS` 0x51; 7 bits in a setup file). Hidden (`MPFEATURE_NEVER`)
  unless **Mod.GePlusCheatGuns** is on, GoldenEye's guns are listed at all
  (the PP7's row shown) and the menus are not a hack's (`gegunsMenuSet()`):
  `gegunsCheatRowsRefresh()`, from `gegunsExtraRowsRefresh()` and
  `gebeanGunsRefresh()`. Every roll skips a hidden row and counts only the
  shown ones (Start Armed, Guards Alerted), the Randomizer rolls only the rows
  before the pistols (`MODRANDOM_MPWEAPONS`), so off, no seed changes.
- **Sets**: three of the port's after GoldenEye's fourteen in its group only,
  while the toggle is on (`g_GeCheatSets`, `geSetsGroupCount()`): "Silver
  PP7s" (Pistols with the Silver PP7 at the top), "Gold PP7" (Golden Gun with
  the Gold PP7 in its place) and "Tasers" (one gun all through, as Slappers
  Only and Throwing Knives are), eight slots taken to six as GoldenEye's are.
  The block changes length in place (`geSetsCheatGunsChanged()`), and so does
  it switching between GoldenEye's mode (17) and a hack's (14):
  `geSetsNumMoved()` moves Random Five, Random and Custom with the list's end
  (`WEAPONSET_RANDOMFIVE` is `g_MpNumWeaponSets`) and takes a set past the new
  end to the block's first that hands anything out. Off, a slot holding one of
  them is emptied.
- **The toggle**, off by default: "GoldenEye Cheat Guns" on the Weapons page
  (shown where the rows could be, `gegunsCheatGunsOffered()`), "GoldenEye:
  Cheat Guns" on Dab's Mod's Mission page beside Include Perfect Dark Guns,
  for GoldenEye's own folder, whose weapon set is a row, and on a lobby room's
  Room Rules. `gexPlusSetCheatGuns()` puts everything in step each call.
- **Online** the host's: a SYNC key (netrules.c), applied before the host's
  slots and set number (`netRulesApplyCheatGuns()`), since a set's number
  counts by a list the three lengthen, and the client's own again before its
  own setup comes back. No change of shape: keys are sent by name.
- **All Guns** on GoldenEye's own levels lists them where the cartridge's
  `equipallguns` does (after the Golden Gun; the taser after the detonator);
  a hack's level leaves them out (`invGeAllGunsHas()`), whose items 20, 21
  and 31 are guns of its own. The watch laser stays out (no weapon of its
  own).

Probes (`~/wt/f3-1010a-geplay-run/probes/`): `cheatguns.py` (each held, fired,
both looks), `damage.py`/`taser.py` (a sim's damage a shot: Silver 2x the
PP7's, Gold 100, taser once a press), `toggle.py` (rows, sets, set numbers,
GoldenEye's mode to Goldfinger 64's and back), `allguns.py`/`allguns-gf.py`
(the list on Dam and on a Goldfinger 64 mission), `menu.py` (the page).


## A ROM hack's guns in Perfect Dark's Combat Simulator (2026-10-10)

The owner said yes to Goldfinger 64's and TND64's guns in the Combat
Simulator on any map (F3 20261009-173850, pdplay's analysis). A hack's guns
are GoldenEye's gun numbers with the hack's versions put in (`gegunsStageSet()`
swaps `g_GeWeaponDefs` for the hack's `menu/geguns.bin` set), and until now
only on the hack's own stages. **Design: a match plays one game's guns** -
GoldenEye's, or one hack's - chosen on the Weapons page. Distinct weapon
numbers for every hack gun were the other way: ~60 weapons and rows, past the
7 bits a setup file keeps a row in, each needing a host, a model state and the
tables GoldenEye's 25 have; and mixing families in one match was never asked
for. The chosen set's family deciding was the third: no explicit choice, and
Custom would carry it invisibly.

- **The choice**: `Mod.CsHackGuns`, the hack's conversion tag ("gf64",
  "tnd64"; "" GoldenEye's own, the default): **"ROM Hack Guns"** on the
  Weapons page, Off or each hack converted and mounted here
  (`gexPlusCsHackNumOptions()`, by its `menu/geguns.bin`). Hidden in
  GoldenEye's own Combat Simulator (its mode decides there), under a mod with
  its own weapon list, and with no hack converted.
- **Sets and menus**: the block after Perfect Dark's sets holds the hack's
  fourteen, tagged [GF] or [TND], where GoldenEye's were
  (`gexPlusWeaponSetsAppend()`; the chosen set keeps its index, Random Five,
  Random and Custom their meaning: `geSetsNumMoved()`); the menus name the
  hack's guns and show its pistols' rows (`gegunsMenuSet()` falls back to
  the choice between matches); GoldenEye's cheat guns' toggle and rows go
  (they are GoldenEye's own).
- **A match** (`g_Vars.normmplayerisrunning`, not GoldenEye's mode): the
  hack's set on any stage but a ROM hack's own, which keeps its own as it
  always has (TND64 chosen on Goldfinger's Junkyard plays Goldfinger's);
  `g_GunSetLent` says the set is in off the hack's stages. GoldenEye's
  arenas take the choice too. Solo missions and co-op never do.
- **Looks**: the hack's own first-person models in either look
  (`gebeanGunsAreN64()` is true while a hack's set is in: the release has none
  of its guns). Its props, held and lying, are its own: nearly all of a
  hack's gun props differ from GoldenEye's under the same number (GF64's
  AK47 on 184 is not the KF7), so each gun's own model state
  (`MODEL_GE_FIRST`) takes the hack's file for its prop
  (`gegunsHackPropsRefresh()`, `modloaderRemakeModelFileOf()`), which works on
  GoldenEye's arenas too, whose remake slots are the arena's. A thrown or
  fired one: the per-weapon state where the gun is held as what it throws
  (the grenade, the mines), else lent from the hack on a stage of Perfect
  Dark's (`modloaderLendRemakeModelFrom()`); on GoldenEye's arenas
  GoldenEye's own then (Goldfinger's thrown Oddjob's Hat is GoldenEye's
  knife there). Neither hack has a rocket of its own (GoldenEye's, as their
  cartridges).
- **Online**, the host's: `Mod.CsHackGuns` is a SYNC key, applied before the
  host's slots and set number (`netRulesApplyCsHackGuns()`) and the guest's
  own again before its own setup comes back. At STAGE_LOAD a guest with the
  hack installed but left out of Mod.MapMods mounts it, and one without it
  fetches it from the host as a map's conversion is
  (`netContentCsHackGunsFollow()`, reading the host's value out of the RULES
  before `netRulesApply()` writes them; the STAGE_LOAD kept meanwhile), which
  the host serves as a folder its session needs (`netContentSessionNeeds()`).
  Keys go by name, so RULES' shape is unchanged: no protocol bump. Gates:
  `netcontenttest.sh` gfcs and gfcsfetch (Goldfinger 64 fetched: 3491
  files, 19.9 MB in 3.9 s on loopback).

Probes (`~/wt/f3-1010a-geplay-run/probes/`): `hackmenu.py` (the choice, the
block, the rows, both hacks), `hackmatch.py` (Complex: names, models, props,
pickups, a sim), `hackcheck.py` (Temple, the HD look, a hack's own map with
the other chosen), `hackthrow.py` (a grenade and a rocket).

## The CamSpy's launch spot (2026-10-10)

Reported as "the CamSpy is at the wrong height and the objective can't be
done; up/down does nothing", every CamSpy mission. It was not the height code:
`eyespyTryLaunch()` puts the CamSpy 100 units in front of the player and asks
only `cdExamLos08()` (a ray with no width) whether the player sees the spot.
Launched along a wall, the wall is within the CamSpy's body (`cdTestVolume()`,
radius 26, the same body `eyespyTryMoveUpwards()` and the horizontal moves
test) at every point of the line, so every move it tries collides, including a
move of zero: forward moved it 0 units, 242 climbs in a row were refused, and
the height sat at its minimum. The obstacle pointer is NULL - it is the level.

Reproduce it without input: at G5 Building's start (`--boot-stage 0x1e
--fixed-step`, frame 300) set `vv_theta` and call `eyespyTryLaunch()` from gdb,
then `eyespyTryMoveUpwards(0)`: 1 is free, 0 wedged. 15 and 75 degrees wedged
on e19341b35. A held stick is `break joyGetStickY` with `return (s8)80` in its
commands (offscreen, no window needed). Run a sweep of angles in fresh
processes, or reset the CamSpy's chr between launches: one launch's leftovers
change the next one's line of sight.

`eyespyFitLaunchPos()` (PC only): when the body does not fit at the launch
spot, it tries back towards the player 10 units at a time and up to 30 to
either side, each spot tested as the move test does (`chr0f021fa8()` first, so
the rooms the body reaches into count - leaving it out passed a spot the move
test then refused) and in the player's sight. A launch with room, and one the
player cannot see ("Not enough room to launch"), are exactly as before. On the
G5 case the CamSpy lands 20 units aside and two seconds of stick up take it
from 30 to its ceiling of 160.

