# Weapon behaviour belongs on the weapon

The game decides a lot by comparing the weapon number - `if (weaponnum ==
WEAPON_SHOTGUN)` - which is a question a mod cannot answer, because a mod that
brings its own guns numbers them its own way. GE-X patched 28 functions in
`bondgun.c` and 31 regions in `propobj.c` for exactly that reason, nearly all of
them number swaps; `tools/modcodediff` says which functions a given mod cares
about.

So the behaviour moves onto the weapon:

- `struct weapon.flags2` - a second flags word, the first having all 32 bits
  spoken for. 19 behaviours so far, read with `weaponHasFlag2()`.
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
