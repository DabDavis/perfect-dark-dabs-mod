# The Randomizer

`src/game/modrandom.c`. A mission dealt again from its own pieces: what stands
in every weapon spot, what the crates hold, where the guards are, where the
keys are, where the mission starts, and the objectives themselves.

The file's own header comment covers the design. This note is the things that
cost a detour and that the code does not say.

## Where the roll goes, and why nowhere else

`setupLoadFiles()` pulls the setup file with `fileLoadToNew()` — a **writable
heap copy**, converted to native structs by the port's preprocessor, so the
whole stream can be rewritten in place. `setupCreateProps()` then walks it and
builds the level. The roll goes between the two, inside `setupCreateProps()`
after `setupPreparePads()` (the walk needs pad rooms, which that call fills in
where the file left `-1`) and before the prop walk.

The objectives go in at the **end** of `setupCreateProps()`, not instead of the
stage's own: the walk binds tags, briefing text and the objective commands, and
skipping it to insert ours would lose all of that. So the stage's objectives are
inserted normally and then the list the game checks is replaced.

## Rooms are not what you get back from chrGetPadRoom()

`chrGetPadRoom(chr, id)` is for **AI script parameters**, where an id under
10000 is already a room number and is handed straight back, and 10000+ means
"pad, minus 10000". Calling it with a pad number returns the pad number as a
room, silently, and every reachability answer built on it is nonsense that
still looks plausible — 100 of 107 rooms "reachable" from a room that was
really a pad id. Use `padUnpack(padnum, PADFIELD_ROOM, &pad)`.

The same convention runs the other way in `criteria_roomentered`: the field is
named `pad` and is read by `chrGetPadRoom()`, so an ENTERROOM objective wants a
**room number**, not a pad.

## The intro stream has no length function

The setup stream has `setupGetCmdLength()`. The intro stream (`g_StageSetup.intro`)
has nothing: the lengths live inside the switch statements that read it, in
`playerReset()` (playerreset.c) and `playerStartNewLife()` (player.c). They are
not uniform — 12 bytes for SPAWN, 16 for WEAPON and AMMO, 32 for cmd 3, 40 for
cmd 6, 8 for HILL, OUTFIT, cmd 4 and CREDITOFFSET. Stepping by a flat 12 walks
into the middle of a command and reads a parameter as the next type, which on a
rewriting pass corrupts whatever is there. `modRandomIntroCmdLen()` is the table;
keep it in step with those two functions.

## Rewriting the spawn pad does not move a mission's start

A solo mission's intro lists **one** spawn pad — the several a stage has are
co-operative's, gated behind `param2 != 0` — so "pick a random one of the
stage's spawn pads" picks between one thing and deals the same start on every
seed.

Worse, the pad is where a *new life* starts. A mission that opens with a
cutscene puts the player where its own script says, and rewriting the intro
changes nothing you can see. The start is therefore moved the way a new life
moves: `modRandomTick()` sets `dostartnewlife` on the first frame after the
cutscene hands over, and `playerStartNewLife()` takes the position from
`modRandomTakeSpawn()` — the same override point Mission Respawn uses.

The pads worth starting on are the ones the stage stands a **guard** on: a spot
in a room, on the floor, sized for something man-shaped, and there are dozens
spread over the level. A start is only taken if the portal walk from it reaches
three quarters of what the mission's own start reaches; otherwise it is a lift
interior or a sealed vault.

## A tagged object is not freed when it is picked up

`objTestForPickup()` frees a picked-up object **unless** it carries
`OBJHFLAG_TAGGED`, in which case it goes into the inventory as an
`INVITEMTYPE_PROP` and `invHasProp()` can see it. That is what makes a
COLLECTOBJ objective work, and it is why generated collect objectives may only
name objects that already carry a tag from the setup file. A generated
objective on an untagged gun would be a mission that cannot be finished.

Tags are resolved by the prop walk, which runs *after* the roll, so
`objGetTagNum()` answers -1 for everything at roll time. The roll does that sum
itself: a tag names the object `cmdoffset` commands along from its own command
index (`setupGetObjByCmdIndex()`), which is what `setupCreateProps()` does later.

## A seed belongs to a build

The seed feeds this file's own xorshift, never `rngRandom()`, so a seed and a
stage deal the same mission on any machine. They do **not** deal the same
mission across a change to the generator: adding a retry loop to the weapon roll
moves every later draw. Seeds are shareable between players on the same build,
and not comparable across versions.

## Testing it

`--boot-stage 0x1d` (Chicago) is the test bed: it reaches gameplay headlessly in
about 30 seconds and has weapons, guards and tags. The log line names the whole
roll:

```
randomizer: stage 0x1d seed 12345 - 32 weapons, 32 guards, 0 keys,
spawn pad 184 room 69, 100/107 rooms reachable, 2 objectives
```

**Villa (0x2c), G5 (0x1e) and Air Force One (0x31) never reach gameplay under
`--boot-stage`** — `lvframenum` stays 0 for minutes — and they do that with the
randomizer off too, so a run that shows no objectives on those stages is the
harness, not the roll. Check `lvframenum` before believing anything else.

`pd.log` is opened with `"ab"` and appended to, so a run's line is the *last*
one; delete the file between runs or `head -1` reads the previous run's roll.
The game rewrites `pd.ini` on exit, so a `sed` of `Mod.Randomizer` between two
headless runs can be overwritten by the first run's shutdown — check the file
after setting it.

To prove an objective completes without playing the mission:

```sh
gdb -p PID -batch \
  -ex 'set $tag = ((int*)g_Objectives[0])[5]' \
  -ex 'set $o = (struct defaultobj *)objFindByTagId($tag)' \
  -ex 'call (int)invGiveProp($o->prop)' \
  -ex 'printf "%d\n", objectiveCheck(0)'    # 1 is OBJECTIVE_COMPLETE
```

Word 5 is the first requirement's parameter: the objective struct is four words
and the type word of the requirement is word 4.
