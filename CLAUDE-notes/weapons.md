# Weapon behaviour belongs on the weapon

The game decides a lot by comparing the weapon number - `if (weaponnum ==
WEAPON_SHOTGUN)` - which is a question a mod cannot answer, because a mod that
brings its own guns numbers them its own way. GE-X patched 28 functions in
`bondgun.c` and 31 regions in `propobj.c` for exactly that reason, nearly all of
them number swaps; `tools/modcodediff` says which functions a given mod cares
about.

So the behaviour moves onto the weapon:

- `struct weapon.flags2` - a second flags word, the first having all 32 bits
  spoken for. 20 behaviours so far, read with `weaponHasFlag2()`.
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

`bondgun.c` still has around 100 of these comparisons and `propobj.c` around 74.
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

**Some tests belong to the shot, not the gun.** `weaponTick`'s grenade timers and
the Devastator's wall hugger read weapon and function together with live timer
state; a flag on either one does not hold them.

**Some are one weapon with one quirk** whose intent is not visible from the
surrounding window - the combat knife's two sites in the hand state machine, the
remote mine's left-hand rule before it was understood as the detonator hand.
Naming those from a guess is worse than leaving the comparison in place.

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
