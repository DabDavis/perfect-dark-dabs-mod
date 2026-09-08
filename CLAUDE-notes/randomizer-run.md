# The Randomizer's run: a room at a time, across every map

`src/game/modrun.c`, and the page it hangs off in `port/src/randommenu.c`. The
mode drops the player into a random room of a random map, deals that room an
objective, and treats **every door out of the room as a portal**: walking
through one ends the level and deals another room somewhere else. It runs until
the player dies and scores objectives finished.

The file's own header comment covers the design. This note is what the code
does not say and what cost a detour.

## A portal is a stage load, and the co-operative campaign is the pattern

One bg file is resident at a time, so two rooms of two maps cannot be joined by
geometry — the same wall modrandom.c hit when it wanted to deal rooms from
different stages. A hop is therefore a whole stage change, and the five calls
that make one are the ones `menuhandlerAcceptMission()` makes and the ones
menutick.c makes when co-operative goes from Deep Sea to the next mission:

```c
g_MissionConfig.stagenum = stagenum;
titleSetNextStage(stagenum);
lvSetDifficulty(...);
titleSetNextMode(TITLEMODE_SKIP);   // or the hop shows the Rare logo
mainChangeToStage(stagenum);
```

**`mainChangeToStage()` takes effect at the end of the frame.** The level being
left keeps ticking for the rest of it, with `g_Vars.stagenum` still its own, so
anything that runs per tick and acts on "the hop's stage" must compare against
`g_Vars.stagenum` and know whether the load it asked for has arrived yet —
`g_ModRunPendingLoad`. Without the flag the tick either acts on the leaving
level (setting `dostartnewlife` on a player about to be thrown away) or, if it
treats a mismatch as "the game left the run", ends the run on the very frame it
asked for the next room.

## A mission's opening cutscene holds the landing off for forty-five seconds

Crash Site's first landing happened at level frame **2678**. The landing is a
new life and `modRunTick()` will not ask for one during a cutscene, and most
solo missions open with one. The fix is the request a button press makes —
`g_CutsceneSkipRequested = true`, plus `g_Vars.autocutgroupskip` when
`g_Vars.autocutplaying` — after the same 30 frames of cutscene the game itself
waits before it will take a press. The same landing then happens at frame 28.

## The kit goes back through the spawn, not from the tick

Everything carries across a portal: health, shield, guns, ammunition. Each part
goes back at the point the game already has for it, and the points are not the
same one:

- **the guns and the ammunition** inside `playerStartNewLife()`, after the
  intro stream has handed out the map's own kit (the two add up, which is why
  a run's inventory grows as it goes) — it cannot be earlier, because that
  call runs `invClear()` on its way past for a solo player;
- **the hands** from `playerSpawnWeapons()`, which is the override Mission
  Respawn uses and the only place a hand may be filled from — a gun in a hand
  is a model to load, and the load is a state machine that runs from the spawn;
- **the health and the shield** at the very end, after `playerSpawn()`, which
  sets the shield back to zero on its way past.

Doing the whole lot from the tick a frame later looked simpler and left the
left hand holding a gun whose model nothing had loaded.

## A hand can be visible with no model behind it

`bgunIsLoaded()` answers for the gun memory as a whole and not for this hand's
model, and the two come apart when a hand is filled at a moment the load
sequence did not expect. A landing is a new life *in the middle of a level*,
and on a mission whose intro arms both hands it left the left hand visible with
a null `leftgunmodeldef`; `bgun0f0a5550()` then dereferenced it while drawing
the HUD — reliably, one landing into a run on Escape (`bondgun.c:8076`, SIGSEGV
with `playerRenderHud` two frames up the stack).

The guard is at the crash site: a hand with no modeldef is not drawn. It is the
shape of bug this fork keeps finding — code unreachable by construction on the
N64, reached the moment something new asks for a new life where stock never
would — and the fix belongs where the assumption is, not in the caller that
happened to break it first.

## The door is "out of the room", not "rooms[0] changed"

A prop straddling a portal lists both rooms, and which of them is `rooms[0]`
changes while the player stands in the doorway. Testing `rooms[0] !=
landingroom` hops the run for leaning through a door. The test is that the
landing room is not anywhere in `prop->rooms[]`.

## A death in a run must not end the stage

`playerTick()`'s death handling calls `mainEndStage()` once the death animation
and the red fade finish, which pushes the endscreen — over the top of the run's
score, naming the map's own objectives. During a run that branch does nothing
and `modRunTick()` ends the run at its own pace: the score for three seconds,
then the Institute, where the mode's page is.

## No objective type fits one room, so the run owns a stage flag

The game has eight objective types and every one of them is a mission's:
collect a tagged object, reach a room, destroy a thing with a tag on it. What
a room can ask for — put down what comes for you, hold the room, pick up the
gun lying there — is none of them. So a run's objective is written as a single
`OBJECTIVETYPE_COMPFLAGS` requirement on **the top bit of `g_StageFlags`**,
which this file owns, clears at every landing and sets itself when its own
condition is met. The objective is then the game's own — `objectiveCheck()`
answers for it and the pause menu draws it — and only the condition is ours.
The bit is the top one because a stage's script sets the low bits and the
highest anything in stock names is `0x00010000`.

Two ways a generated collect objective becomes a free point, and both are
handled: the gun is already in the kit coming through the portal (checked
against the carry snapshot at roll time, since there is no inventory to ask
during `setupCreateProps()`), or the map's own intro hands it out at the
landing (checked after the kit goes back, and the objective becomes a fight
instead).

## The map pool is the game's two lists, never the stage table

The stage table holds everything the build can load, and that includes the
unfinished development maps. `STAGE_TEST_MP16` has no waypoints and no room
with a portal, and the run that landed in it took the game down with SIGFPE
(exit 136) part way through the load — before anything in `modrun.c` could
have refused it. So the pool is built from `g_SoloStages` and `g_MpArenas`,
which are the lists a person can choose from in the menus, and a map in
neither is a map nobody plays.

That is also what makes the widest setting mean what it says: the Stage Loader
registers a mod's maps as arenas, so everything past the stock sixteen in
`g_MpArenas` is exactly "and the maps a mod brought". Two rows in those lists
are not maps: `STAGE_MP_RANDOM` (the arena list's "Random" button, stage
number 1) and the Institute.

## An arena loaded solo starts the player dead

`run: over ... at frame 2` on Temple, on the first hop. An MP arena has no solo
intro, and whatever that leaves the player as, `isdead` is true two frames in.
A run must not end there — an hour of play lost because the next room was
Temple would be the mode's worst moment — and it does not have to: the landing
is a new life, and `playerStartNewLife()` clears `isdead` and hands back full
health. So a death is only the run's death once the run has landed, and every
landing asks for a new life whether or not it found a pad to put it on.

`lvRender()` is what calls `playerStartNewLife()`, later in the same frame the
tick asks for it, and it clears `dostartnewlife` on its way past. That flag
going back down is the only honest signal that the landing happened — the
spawn override answers nothing at all on a hop that found no pad.

## A landing is a waypoint, and its room needs a portal

Waypoints for the same reason modalarm.c uses them: a waypoint is by
construction somewhere man-shaped can stand and walk away from, and a random
point in a random room is inside a wall about as often as not. The room also
has to have at least one portal, or the landing is a sealed box and the run
cannot go on without a death.

## Guards come from the alarm, not from the setup

"Random guards, even Skedar" is `modalarm.c` with its body choice overridden:
during a run `modIsGuardsAlertedOn()` is true whatever the setting says,
`modGetAlertedGuards()` and `modGetGuardSpawnSpeed()` answer with the run's
numbers, and `modAlarmChooseBody()` asks `modRunChooseBody()` — one body per
landing, drawn from a list that includes `BODY_SKEDAR` and `BODY_MINISKEDAR`.
One body per landing rather than per guard keeps the head loads to one, which
is what `modbodies.c` learned the expensive way.

## A body is more than a model, and the alarm only built the model

`BODY_CHICROB` is in the run's list, and a robot is the one body whose chr
carries something the model does not: two `fireslotthing`s in `chr->unk348`,
allocated out of the stage pool. `bodyAllocateChr()` does it for a setup's own
chrs; `modAlarmSpawn()`, which is where every one of a run's guards comes from,
did not. `propsRenderBeams()` reads both beams out of any chr whose race is
`RACE_ROBOT` without asking whether they are there — stock had nowhere for a
robot to come from except a setup, so the question could not arise — and a room
the run dealt robots for went down about five seconds after the landing, in the
render rather than anywhere near the spawn (`propobj.c`, SIGSEGV,
`lvRender` one frame up).

The allocation and the two sizes are now `bodyInitSpecialChr()` in `body.c`,
called by both spawns, and the render skips a robot with no fireslots the way
`robotAttack()` already declined to attack without them. The general rule: a
body chosen out of the whole game reaches spawn paths that were written when
the choice was the stage's, so anything `bodyAllocateChr()` does for a body
number has to happen in `modAlarmSpawn()` too.

**A seed reproduces it exactly.** `Mod.RandomizerSeed` in the savedir's pd.ini,
with `Mod.RunMapPool` and `Mod.RunDifficulty` set to what the crash log printed,
deals the same first stage, the same landing pad and the same body:

```sh
SDL_VIDEODRIVER=offscreen ./pd.x86_64 --moddir mod_allinone --gexmoddir mod_gex \
    --savedir /tmp/pdrun --boot-stage 0x26 --random-run --no-sound \
    --fixed-step --exit-frame 4000
```

The crash handler's backtrace is module offsets, so `addr2line -f -e pd.x86_64
0x...` reads it — but only against the binary that produced it. A rebuild moves
every offset, and the nearest symbol in a binary that is one build out is a
function with nothing to do with the crash.

## What moved out of Dab's Mod Options

The four rows the Randomizer had there - the checkbox, the seed, Endless Mode
and its score - are gone from that page and live on the Randomizer page
instead, the way Ghost Trials took recording out of the settings. The pd.ini
keys all still work (`Mod.Randomizer` is now ini-only, and is what turns the
roll on for missions started any other way), and the page adds
`Mod.RunMapPool`, `Mod.RunDifficulty`, `Mod.RunBestScore` and
`Mod.RunBestRooms`.

Random Mission arms the roll for one mission rather than writing the setting,
which is `modRandomArmMission()` - the same shape as `modGhostArmTrial()`, and
the stock Solo Missions item disarms both on the way past.

## Testing it headlessly

The mode is a main menu door and a headless run cannot press one, so:

```sh
cd build && rm -f pd.log && timeout -k 5 600 xvfb-run -a ./pd.x86_64 \
    --savedir /tmp/pdrun --skip-intro --no-sound --log \
    --random-run --run-autohop 600
```

`--random-run` starts a run from the first level loaded; `--run-autohop N`
takes the portal after N level frames without walking to a door, which is the
half of the mode a person cannot test by playing it once — every map in the
pool loading, the kit surviving the loads, the stage pool coming back. The log
names each step:

```
run: begin seed 2136939108 pool 0 difficulty 0, first stage 0x1c
run: hop 0 stage 0x1c seed ... - land pad 53 room 41, body 131, objective 1 "Hold this room for 29 seconds"
run: landed on stage 0x1c in room 41 at frame 28, health 1.00
run: portal out of room 41 on stage 0x1c at frame 601, 4 guns, health 0.97
```

Narrow the map pool with `Mod.RunMapPool=0` in the savedir's `pd.ini` while
working on the mode itself: the arenas and a mod's maps are loaded solo, which
is a thing they were never asked to do, and a fault there is not a fault in
this file.

**Do not `pkill -f` a pattern that matches your own command line** — the shell
running the test matches it too. `for p in $(pgrep -x pd.x86_64); do kill -9
$p; done`.
