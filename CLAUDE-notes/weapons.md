# Weapon behaviour belongs on the weapon

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
