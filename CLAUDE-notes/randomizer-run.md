# The Randomizer's run: a room at a time, across every map

`src/game/modrun.c`, and the page it hangs off in `port/src/randommenu.c`. The
mode drops the player into a random room of a random map, deals that room an
objective, and treats **every door out of the room as a portal**: walking
through one ends the level and deals another room somewhere else. The doors are
shut until the room's objective is done. It runs until the player dies and
scores objectives finished.

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

## The room is sealed until its objective is done

Every door being a portal made leaving free, and free leaving is a mode with
nothing in it: walk in, walk out, and the score is doors walked through. So
while the objective stands the doorway is a **wall from the inside** -
`modRunSealMove()`, called by `bwalkCalculateNewPosition()` before it asks the
collision system anything - and the door is a door again the moment the
objective is met.

**A refused move has to leave a collision behind.** Everything that runs after
one reads what it left: `bwalkCalculateNewPositionWithPush()` asks
`cdGetObstacleProp()`, and the slide asks `cdGetEdge()`. Return
`CDRESULT_COLLISION` without writing either and both read whatever the *last
real* collision left there - a door prop that is nowhere near the player, and
with `DOORFLAG_DAMAGEONCONTACT` a wall that is not there hurting them. So the
barrier writes its own: `cdSetObstacleVtxProp(vtx1, vtx2, NULL)`, which sets
the edge and clears the prop in the same call. The edge is the **portal's own
plane** - the portal being left, found by `modRunZoneExitPortal()`; its normal
turned a quarter turn in XZ, laid through the point the move was going to - so
the player slides along the doorway instead of stopping dead in front of it.

**The barrier's test is the portal's test.** "Not in `prop->rooms[]` any more",
the same one the hop is taken on, so the two agree by construction: no move is
refused that would also have hopped, and nothing hops that did not pass the
barrier. Leaning through a doorway - a position listing both rooms - is neither,
which is what lets the player see what is on the other side.

**What is sealed is a room wider than the landing room.** `modRunBuildZone()`:
the landing room and every room a *portal* of it opens onto, one door deep.
A single room is sometimes a stairwell or a corridor two strides across and a
fight held in one of those is fought against the walls, which is what "sometimes
it's real tight" was. Two doors deep is most of a small map, and the helping of
level either side of a hop is what pays for the load.

Because the barrier and the door are one test, the zone is **also what a hop is
measured from**: the portal out is the first door leaving those rooms, not the
first door out of the one landed in. Extending only the seal would seal a
neighbour room the player hops out of the instant they enter it.

Three things about the zone are not obvious:

- **It is built at the landing, not at the roll**, because the landing is the
  only point that knows where the player actually stands: `g_ModRunLandRoom` is
  taken from `player->prop->rooms[0]`, and a stage with no waypoint in a room
  with a door starts them its own way. Not for want of a bg — this note used to
  say `g_Rooms`, `g_RoomPortals` and `g_BgPortals` still belonged to the level
  being torn down at roll time, and that is **wrong**: `lvReset()` calls
  `bgReset()` and `bgBuildTables()` for the new stage at line 352 and does not
  read the setup file until line 400, so every one of them is this stage's
  before the roll runs. The roll's own `modRandomBuildPortalKeys()` walks them,
  and the landing check below asks the collision system from the same place.
- **The zone has to have a door out of it**, or a run ends without a death:
  the seal opens when the objective is done and there is nothing left to hop
  through. On a map small enough that the landing room's neighbours are the
  whole of it, the zone falls back to the landing room alone
  (`modRunZoneExitPortal(NULL) < 0`). An arena of three rooms all touching is a
  real shape, not a hypothetical one.
- **Room 0 is not in it.** A landing already rejects it; a neighbour that is
  room 0 is left outside the seal, where it counts as a way out the same as any
  other room the run did not land in.

The collect objective still names a gun in the **landing room**, not one
anywhere in the zone: it is dealt at the roll, from the seed, and widening it
would move what every existing seed deals.

The zone is logged at every landing, and the seal names it once:

```
run: sealing 3 room(s) on stage 0x33 - 16 15 17
run: sealed in room 38 (+4 touching) on stage 0x2a at frame 58 - "Hold this room for 36 seconds"
```

**Only a move that *starts* in the room is refused.** A barrier that tested the
destination alone would freeze a player who is already outside, and things that
are not the walk put them there: a lift, a blast, a fall. Outside a sealed room
the tick does not hop either - the objective is still that room's - it says
"return to the room" and the seal re-arms when they do.

**A hoverbike is a second way out.** `bbikeCalculateNewPosition()` is the walk
written again for the bike and moves the player through its own collision
tests; the same call sits in it.

**A sealed room has to be finishable, or the run cannot go on.** The three
objectives are not equally certain: the gun a collect objective names can be
destroyed where it lies, and guards have to reach the room to be killed in it.
So a room that has stood sealed for `MODRUN_STUCK_SECS` (150) is dealt the one
thing that always finishes - a clock - from its own stream id, since a re-deal
must not move what the seed deals anybody else.

`--run-autohop N` hops through the seal. It has to: it is what walks a chain of
maps in a minute, and it is not the player.

**The switch is `Mod.RunSealRooms`**, on by default, and "Seal Rooms Until
Done" on the Randomizer Options page. `modRunIsSealed()` reads it on every ask
rather than at the landing, so a run already under way answers to it and
nothing about a room is dealt differently for it. Off is the mode as it first
shipped - the objective an offer and the next door always open - which is a
different game rather than a broken one, and is the whole reason the switch
exists.

### Testing it

The mode is a menu door and a sealed room is a thing you walk into, so both
halves are driven from gdb against a landed run. The predicate first:

```sh
gdb -p PID -batch -ex 'thread 1' \
  -ex "set \$pl = g_Vars.currentplayer" \
  -ex "set \$land = 'modrun.c'::g_ModRunLandRoom" \
  -ex "set \$to = (short *)malloc(16)" \
  -ex "set \$to[0] = <a room the landing room has a portal to>" -ex "set \$to[1] = -1" \
  -ex 'printf "%d\n", (int)modRunSealMove($pl->prop->rooms, &$pl->prop->pos, $to, &$pl->prop->pos)'
```

1 leaving, 0 staying, 0 for a move that starts outside, and 0 for all of them
with `g_ModRunObjective.done` set. A room the landing room has a portal to is
now **in** the zone and answers 0: the `sealing N room(s)` log line names the
rooms, and a room two doors out is the one to point the test at.

The wall itself wants a walk, and a walk can be driven: step the player toward
a portal's centre with repeated `bwalkCalculateNewPositionWithPush()` calls
(`bgGetPortalVtxInfo()` gives the vertices) and watch `prop->rooms`. **Run the
open pass first and from the same starting position**, or the test proves
nothing: a synthetic walk gets stuck on doorframes and closed doors often
enough that "did not leave the room" on its own is not evidence. On Escape
(0x19), room 165, all four portals leave the room with the objective done and
none of them leave it sealed; on Chicago's room 86 neither pass ever got out,
which is the shape of an inconclusive run rather than a passing one.

The log names what the seal did:

```
run: sealed in room 38 (+4 touching) on stage 0x2a at frame 58 - "Hold this room for 36 seconds"
run: room 38 on stage 0x2a stood sealed for 150 seconds; dealing a clock - "Hold this room for 20 seconds"
```

The stuck clock is reachable without waiting two and a half minutes: set
`g_ModRunObjDealt` back by 100000 and it deals on the next tick.

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

## A mod map's stage entry had no setup of its own, and that froze the game

A Stage Loader map is registered by cloning the Skedar arena's stage table row
and pointing it at the mod's files - `modloaderAddStage()`. It pointed
`mpsetupfileid` at the mod's setup and left `setupfileid` as **Skedar's**,
which is invisible in the Combat Simulator, where `setupLoadFiles()` reads the
mp one. A run's landing is a solo load and reads the other.

So the stage came up as Skedar's props and Skedar's intro over the mod's bg,
tiles and pads. Skedar's intro starts the player on its own spawn pad, pad 99,
and the mod map's pads file had 98 pads. Nothing bounds a pad number:
`padUnpack()` indexes `g_PadOffsets` with it, reads a u16 from past the end of
the table as an offset, and unpacks whatever is at that offset as a pad. The
room is a signed ten bit field, so out of arbitrary bits it came back as -256.

`cdCollectGeoForCyl()` then asked `if (roomnum < g_TileNumRooms)` - the only
bound it has, and it has no other end - indexed `g_TileRooms[-256]`, and handed
`cdCollectGeoForCylFromList()` a start and an end pointer into memory that is
not a geo list. **That walk steps over each geo by the length its own type
gives it, and a type it does not know has no length**: the pointer stopped
moving and the game sat in that loop at 100% of a core, on the loading screen,
until it was killed. Not a crash and not a slow load - a freeze with a
backtrace that is the same every time you look:

```
cdCollectGeoForCylFromList (... roomnum=-256) at collision.c:1201
cdCollectGeoForCyl, cdFindGroundInfoAtCyl, cdFindGroundAtCyl
chrAdjustPosForSpawn, playerChooseSpawnLocation, playerReset, lvReset
```

Four things were wrong and all four are fixed, because any one of them alone
leaves the next one waiting for the next mod:

- the stage entry now takes the mod's setup for `setupfileid` too, so the map
  is loaded with its own props, its own intro and pads that match;
- `padUnpack()` and the pad writers answer a pad the file does not have with a
  pad in room -1 at the origin, and log it once per pads file (`coverUnpack()`
  already guarded a cover number this way);
- every `roomnum < g_TileNumRooms` in collision.c is `roomnum >= 0 &&` as
  well, in all seven places;
- every geo walk breaks out on a type it does not know, in all nine.

A mod map's setup is its **arena** setup, and loading one solo is not free:
its weapon props include the MP location markers, weapon numbers 240-255 that
`setupPlaceWeapon()` translates into real weapons only while a match is
running. Outside one they fell through to `g_Weapons[244]`, which is past the
end of a 94 entry table - `setupPlaceWeapon()` now drops a weapon number this
game has no weapon for instead. There is no `mpGetMpWeaponByLocation()` to ask
outside a match, and a marker is a slot for the match to fill rather than a
gun.

Two gdb calls on a running game are the whole test:

```sh
gdb -p PID -batch \
  -ex 'set $b = (struct pad *)malloc(256)' \
  -ex 'call (void)padUnpack(99999, 0xffffffff, $b)' \
  -ex 'p $b->room' \
  -ex 'set $r = (short *)malloc(16)' -ex 'set $r[0] = -256' -ex 'set $r[1] = -1' \
  -ex 'p (float)cdFindGroundAtCyl(&$b->pos, 30, $r, 0, 0)'
```

Room -1 and a ground of -4294967296, both answered at once. Before the fix the
second call is the freeze, in the process you attached to.

## A landing is a waypoint, and its room needs a portal

Waypoints for the same reason modalarm.c uses them: a waypoint is by
construction somewhere man-shaped can stand and walk away from, and a random
point in a random room is inside a wall about as often as not. The room also
has to have at least one portal, or the landing is a sealed box and the run
cannot go on without a death.

## A landing has to have a floor under it

A waypoint is somewhere a chr can stand and walk away from, and that is not the
same as somewhere a player can be put down. Between 2% and 8% of a stage's
waypoints have no bg floor beneath them at all — 31 of one stage's 371 — and a
landing on one does not fail: `playerStartNewLife()`'s ground search answers
`-4294967296`, and the player starts four billion units under the level. That
is "sometimes it spawns you out of bounds and you die".

`modRunChooseLanding()` asks `modRandomPadCanSpawn()` before the draw, so a bad
pad is never in the pool rather than being drawn and worked around; the rule is
behind `MODRANDOM_VERSION >= 3` because it changes which pad a seed deals. The
full question — no floor, a `GEOFLAG_DIE` tile, a floor too far below, no room
to stand — is randomizer.md, "A pad is not automatically somewhere a player can
stand". `modRunTakeSpawn()` then hands back the **floor's** y rather than the
pad's, which every version gets: it does not change which pad the seed dealt.

The log names what a stage lost to it:

```
run: stage 0x44 - 31 of 371 waypoints in a room with a door are not standable
run: landed on stage 0x1e in room 80 at frame 33, health 1.00, standing at 3088,409,-9
```

## A sealed room the guards cannot walk into

The other half of what the mode was reported for: "doors can lock you in a room
and enemies cannot reach you". The seal is a wall the player cannot walk out
of, and `modAlarmSpawnOne()` puts its guards at a waypoint between eight and
forty-five metres from the nearest player and lets them walk in — which is
right for a mission and wrong for a sealed room. Eight metres is often the
whole zone, so every waypoint inside it is refused as **too near** and every
guard starts outside; it then has to get in through whatever the map put
between the two, and on a room behind a locked door or at the end of a lift
there is nothing it can use. "Eliminate 5 hostiles" in an empty room, for as
long as the player is willing to stand there.

Three things now stop that, and all three are needed:

- **The guards are dealt into the zone.** While a room is sealed
  (`modRunGuardsWantZone()`), the alarm takes its waypoint from the zone's own
  rooms, no nearer than `MODRUN_GUARDNEAR` (400) and with no maximum, and falls
  back to its own rule the moment the zone has nowhere to put one. A spawn in
  view is still refused — that is `chrAdjustPosForSpawn()`'s own test and the
  reason a guard never pops in front of the player.
- **The zone's waypoints are found by walking, not by sampling.**
  `MODALARM_TRIES` is twelve random draws, and five rooms of a level's hundred
  will not come up in twelve draws out of three hundred waypoints.
  `modAlarmFindZoneWaypoint()` walks the list from a random start instead.
- **A starved kill objective is re-dealt in forty seconds, not a hundred and
  fifty.** `MODRUN_STARVE_SECS`: if nothing hostile has been inside the zone at
  all for that long, nothing is coming, and the room is dealt the clock the
  stuck rule would have dealt eventually. A kill counts as well as a body
  standing there, or a player quick enough to drop each guard in the doorway
  would starve their own objective. Same stream as the stuck clock, so a
  re-deal never moves what the seed deals anybody else. The full
  `MODRUN_STUCK_SECS` stays as the backstop for the other two objective kinds.

### Testing it

The zone spawn is named in the alarm's own trace, so `--chr-trace` on a landed
run answers it directly - the room has to be one the `sealing N room(s)` line
lists:

```
run: sealing 3 room(s) on stage 0x1c - 39 37 40
alarm: guard body 110 head 67 weapon 11 left -1 at pad 357 in room 40 (sealed zone), 2997cm from player 0, ...
```

Spread over several pads, not all on one. The first version of this took the
first qualifying waypoint from a random start, and a zone with one qualifying
waypoint then filed all twelve guards out of the same corner;
`modAlarmFindZoneWaypoint()` walks the whole list and takes one of the
qualifying waypoints at random instead.

The starve clock cannot be waited for while guards are arriving - they feed it
every tick - so it is driven: empty the zone out from under it and put its
clock in the past.

```sh
gdb -p PID -batch -ex 'thread 1' \
  -ex "set variable 'modrun.c'::g_ModRunNumZone = 1" \
  -ex "set variable 'modrun.c'::g_ModRunZone[0] = 0" \
  -ex "set variable 'modrun.c'::g_ModRunObjFed = 'modrun.c'::g_ModRunObjFed - 100000"
```

Room 0 is the zone nothing can be standing in, which is the point of using it.
The next tick says so:

```
run: nothing has reached room 39 on stage 0x1c for 40 seconds; dealing a clock - "Hold this room for 40 seconds"
```

## The way out has to be openable

A door with key flags on it is not a doorway when the seal lifts: it wants a
card the roll had no reason to leave in this room, and a zone whose every exit
wants one is a room the player is shut into for good with nothing left to do in
it. `modRunOpenExits()` takes the keys off the doors standing in a portal that
leaves the zone, at the moment the objective completes — a locked door deeper
in the map is the map's own business and is left alone.

The setup stream's `doorobj` **is** the live door: `setupCreateProps()` fills
in its `portalnum` and its `prop` in place, which is why the stream can be
walked for doors at any point in the level. `portalnum` is only meaningful when
`OBJFLAG_DOOR_HASPORTAL` is set.

## Guards come from the alarm, not from the setup

"Random guards, even Skedar" is `modalarm.c` with its body choice overridden:
during a run `modIsGuardsAlertedOn()` is true whatever the setting says,
`modGetAlertedGuards()` and `modGetGuardSpawnSpeed()` answer with the run's
numbers, and `modAlarmChooseBody()` asks `modRunChooseBody()` — one body per
landing, drawn from a list that includes `BODY_SKEDAR` and `BODY_MINISKEDAR`.
One body per landing rather than per guard keeps the head loads to one, which
is what `modbodies.c` learned the expensive way.

## A room won is a room to rest in

The mode used to end a room the moment its objective landed and nothing else:
the doors opened, the score went up, and the guards kept coming through the
door the player was walking towards. A run is **one long life** across every
map it deals, and the only thing that ever gave health back was whatever the
map's own intro happened to hand out at the landing - so a room survived on a
tenth of a bar was a run over in the next one however well it was played, and
there was no moment in a run at which the player was not being shot at.

So finishing a room now finishes the fight in it, all in `modRunTickObjective()`
where the score goes up:

- **full health and shield.** `bondhealth` to 1 and `playerSetShieldFrac(1)`,
  with `oldhealth`/`apparenthealth` and their armour pair zeroed so the HUD
  sweeps back up rather than cutting to full - the same thing
  `modRunRestoreHealth()` does when it hands the carried kit back. The carry
  snapshot is taken at the portal, so the next room is landed in on full as
  well.
- **the hostiles in the zone die.** `modRunSweepZone(true)`, which is the walk
  `modRunEnemyInZone()` was, now shared: the same filter (not the player's
  team, not a non-combatant, not already dying) over the same sealed rooms.
  The zone rather than the map, for the reason the seal is the zone - the
  rooms the player was shut into are the fight, and a guard three rooms away
  the run never showed them is the map's own business.
- **and no more arrive.** `modRunGetGuardCount()` answers 0 once
  `g_ModRunObjective.done` is set. Nothing else has to be told: `modAlarmTick()`
  stops at `alive >= maxalive`, and nothing is not under zero. `modRunRoll()`
  clears `done` for the next room, so the guards come back with it.

The pause is the player's to end - they leave when they walk out - which is
the point. It is also why the guards are *killed* rather than taken away: a
room whose enemies blink out reads as the mode breaking, and the corpses are
the evidence of what the room was.

**`chrDamage()` will not take a NULL vector or a NULL gset.** It reads
`vector->x` and `gset->weaponnum` on the way to its alive branch and only
tests `vector` for NULL a few hundred lines further down, in a part a kill
never reaches. Both no-shooter damage sites the game already has - the poison
tick in `chr.c` and the AI list's damage command - pass a zeroed `struct
coord` and `{WEAPON_COMBATKNIFE, 0, 0, FUNC_POISON}`, so this passes the same.
FUNC_POISON is not the knife's own special case either; that one wants
FUNC_PRIMARY and a shot in the back. Passing NULL for both is a SIGSEGV in
`chrDamage()` one frame after the objective completes, which is how this was
found.

The amount is 10000 and any number past the chr's `maxdamage` would do -
`chrDamage()` clamps it. No attacker prop, so the kills are nobody's: the
objective is already done and the tick has stopped counting, but a kill
objective that scored these would be scoring the reward for finishing itself.

### Half as many guards

`MODRUN_GUARDS` and `MODRUN_SPEED` are 6 and 6, halved from 12 and 12, which
was too much of a room to fight rather than a room to be in. **Both halves
matter.** The cap alone would not have done it: with twelve still arriving
every ten seconds against a cap of six, every kill is answered inside a second
and the room feels exactly as it did however few are standing at any moment.
`alarm: N guards up` in a `--chr-trace` log is the number to read it off.

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
instead, the way Ghost Trials took recording out of the settings. The seed,
version and Endless keys still work, and the page adds `Mod.RunMapPool`,
`Mod.RunDifficulty`, `Mod.RunSealRooms`, `Mod.RunBestScore` and
`Mod.RunBestRooms`.

`Mod.Randomizer` itself is **gone** (2026-09-10). It was kept as an ini-only
switch when its checkbox left, and it outlived the checkbox in every pd.ini
written while the box was ticked: every Solo Mission came up dealt again, with
Endless on a single "reach this room" objective in place of the stage's own,
and nothing left in the menu to switch it off. A stale line is ignored on load
and dropped on the next save. Nothing in pd.ini turns the roll on now; the two
doors on the page do, and `--random-mission` does for a headless run.

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
