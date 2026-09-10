# The Randomizer

`src/game/modrandom.c`. The mode's *run* - a room at a time across every map,
which is what the Randomizer page's Start Run does - is
[randomizer-run.md](randomizer-run.md); this note is the roll a single mission
is dealt by, which a run's every landing also uses.

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

## A seed keeps its mission across a change to this file

Every decision draws from **its own stream**, seeded from the run (seed, stage,
difficulty) plus a stream id and an index — not from one PRNG walked from the
first decision to the last. With a single stream, adding one draw anywhere moves
every draw after it, and every seed anyone wrote down becomes a different
mission; that is what happened between the first two builds of this file.

With streams, changing the weapon roll moves weapons and nothing else, and a new
kind of thing to randomize takes a new stream id and disturbs nothing. Stream
ids are permanent: renumbering one changes every seed that uses it.

What streams cannot absorb is a change to what a draw *means* — a weapon
dropped from the pool, a different rule for which pads may be a start. Those go
behind `g_ModRandomVersion >= N` with the old behaviour left in place, and
`MODRANDOM_VERSION` (in modrandom.h, so modoptions.c can default to it) goes to
N. A run keeps the version it was dealt by in `Mod.RandomizerVersion`, written
beside the seed and shown in the menu as `12345 v1`; a seed asking for a version
this build does not have is dealt by the newest one it does and says so in the
log.

Seeds from before versioning — the build that first shipped the Randomizer —
are not reproducible and are not claimed to be.

A **kept** seed keeps its version; a seed of 0 (a fresh mission every time) has
nothing to keep and always takes the newest, or a config written by an older
build would pin every future run to that build's generator.

Version 2 widened where a "reach this room" objective may send the player, from
the deepest rooms the walk found to the deep half of them, because Endless Mode
asks for a room over and over and four of them is not enough to ask about. It
shows on a stage whose first objective is a room one — Escape (0x19) is one, and
its objectives fold is `8048d5b7` at v1 and `ff0044df` at v2.

### Checking a change did not move old seeds

The log prints a fold of each part of the roll:

```
randomizer: fold weapons 426344ec guards 40418752 keys 00000000 spawn 000000ae objectives d8a0a69a
```

Record it for a seed before a change and compare after. The part you changed
should move and **nothing else should**. That is a real test: adding one draw
to the weapon roll moves `weapons` from `426344ec` to `a607ddb9` and leaves
guards, keys, spawn and objectives identical. If a second number moves, the
change leaked out of its stream and every seed on this build has quietly become
a different mission.

## Endless Mode

One objective at a time, another dealt the moment it is finished, and the run
ends at the first death — `modRandomTickEndless()`, from `playerTick()`. The
lists and the walk from the level's roll are kept for the whole level (both are
stage-pool allocations that live as long as the level), so dealing another
objective mid-mission is the roll's own work minus the rewriting.

Objectives live one per fixed 64-byte block rather than packed end to end,
because a re-deal rewrites a block in place: packed, an objective's length
would decide where the next one starts and re-dealing would move every
objective after it. `modRandomObjectiveAt()` is the only thing that knows the
layout. Packing them also misaligns every second criteria struct's trailing
pointer, which x86 forgives and the arm64 build does not.

The next objective is dealt in the same tick the last one completes, so
`objectiveIsAllComplete()` is never true for a whole frame — that is what the
stage's own exit trigger asks, and an endless run has no business ending at the
exit.

The score is rooms stood in, counted once each. The death screen is a fade and
then the "Error Saving Game" dialog, so a score shown there is a score nobody
reads: the running total rides along with each new objective's HUD message
instead, and the best is kept in `Mod.EndlessBest` and shown on a label row
under Endless Mode in the menu.

**The Carrington Institute is a level.** It loads a setup file with weapons,
guards and pads, and it is the backdrop the Perfect Menu is drawn over, so the
first version of this rolled it — a random gun in the firing range, the
player's spot behind the menu moved, an objective dealt in a building with no
mission — before the title screen had finished drawing.
`modRandomStageIsMission()` excludes it. Anything else that runs on "a level"
should think about that stage first.

## Testing it

`--boot-stage 0x1d` (Chicago) is the test bed: it reaches gameplay headlessly in
about 30 seconds and has weapons, guards and tags. The log line names the whole
roll:

```
randomizer: stage 0x1d seed 12345 - 32 weapons, 32 guards, 0 keys,
spawn pad 184 room 69, 100/107 rooms reachable, 2 objectives
```

**A stage that seems not to reach gameplay is usually a clogged machine.**
`lvframenum` stuck at 0 for minutes on Villa, G5 and Air Force One looked like
those stages not booting under `--boot-stage`, with the randomizer off as well;
it was two earlier runs still spinning at 100% CPU each because their Xvfb had
been killed out from under them and the game never noticed. Load average was 9.
`ps aux | grep [p]d.x86_64` and `kill -9` the strays, and the same stage loads
in thirty seconds. Check `lvframenum` before believing anything about a run,
and check `uptime` before believing `lvframenum`.

`pd.log` is opened with `"ab"` and appended to, so a run's line is the *last*
one; delete the file between runs or `head -1` reads the previous run's roll.
A headless mission is dealt again with `--random-mission` beside
`--boot-stage`; there is no pd.ini key for it any more (see randomizer-run.md,
"What moved out of Dab's Mod Options"). The game rewrites `pd.ini` on exit, so
a `sed` of a seed between two headless runs can be overwritten by the first
run's shutdown — check the file after setting it.

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
