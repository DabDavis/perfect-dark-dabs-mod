# chrs, bodies, heads and memory pools

## A chr's prop and model are read before its tick, not during

`chrTick()` (`src/game/chr.c`) reads `prop` and `model` once, then calls into the
action handler and keeps using those locals afterwards — `chrRemove(prop, true)` at
the end is on the prop it read at the top. So anything that moves a body between chrs
has to happen outside the prop tick. `modBodiesTick()` runs from `chraTickBg()`, which
is the first thing `propsTick()` does and the one point in the frame where no prop is
part way through its own tick; the dead simulant is left waiting a tick rather than
respawned from inside `chrTickDead()`.

## Memory: what an MP chr costs, and which pool it comes from

`body0f02ce8c()` (`src/game/body.c`) takes a different path in multiplayer: with no
`headmodeldef` passed in it calls `modeldefLoadToNew()` **every time**, a fresh ~50KB
copy of the head from `MEMPOOL_STAGE` that is never freed, because each simulant's
head is offset to fit its own body. Stock pays that once per bot at match start.
Anything that creates chr models *during* a match must pass the modeldefs in, or it
empties the stage pool in about a minute.

`mempAlloc()` fills the onboard bank first and falls through to the expansion bank,
which is a hardcoded 8MB (`MEMP_EXPANSION_POOL_SIZE`) no matter what `Game.MemorySize`
says — that setting grows the onboard side only. So `mempGetStageFree()`, which
answers for one bank, is not "how much room is left"; `mempGetStageFreeTotal()` is.
A pool-full warning naming one pool is the onboard bank spilling over, not the end.

`mempAlloc()` returns NULL when both are out, and most callers do not check.
`modelmgrInstantiateModel()` now does. It logs one `memp: pool N is out of memory`
error the first time that happens for a pool other than 7 and 8 (those two are
asked and refused at every boot, and would spend the line), so a crash report's
log names the cause.

**A pd.ini keeps the MemorySize it was written with, and 16 was the default** here
before v1.0 and still is upstream. At 16 the onboard stage pool is 75 KB free on
a GE-X level with the XBLA switches on and 700 bytes on Chicago with PD Plus HD,
against 49 MB at 64 on the same stages; the eight v3.5.0 crash reports with
pool-full warnings were that (their request sizes - 563264, 307200, 76800 - are
the ones a 16 MB run prints). `main()` now raises anything from 5 to 63 to 64
straight after `configInit()`, before the heap is allocated; 4 is left alone,
because that is the N64's 4MB mode (`g_Is4Mb` is set from the heap size).
Measure a pool by printing `mempGetPoolFree(4,0)` and `(4,1)` from a gdb
breakpoint on `lvTick` at a given `g_Vars.lvframenum`, not by counting warnings.

## One head modeldef cannot sit on two bodies

`modelAttachHead()` re-parents the head's root nodes to the headspot of
whichever body attached it last, and `modelGetNodeRwData()` walks *up* from a
head node to find a headspot and take its `rwdatas`. So a head modeldef shared
between two *different* body definitions resolves correctly for one of them and
reads a meaningless index into the other's rwdata — which showed as a write to
`0x5008b7d`, the same address every run, because what was being read as a
pointer was model data out of the ROM.

Stock never hits it: solo shares one modeldef per head number but only ever
pairs a head with the body it was built for, and multiplayer loads a fresh copy
per chr *and* calls `bodyCalculateHeadOffset()` for the body it is going on.
Ghost Trials puts any head on any body in solo, so a trial takes the
multiplayer path — in **both** places that decision is made: `body0f02ce8c()`
for everyone, and the player's own copy in `player.c`, which is what runs the
moment third person is switched on. Fixing the first alone moved the crash and
did not remove it.

**Stock solo does it too, and it crashed a player in Area 51 (2026-09-12).**
`bodyChooseHead()` deals every male body a head off one rotating pool
(`g_ActiveMaleHeads`), so a mission with more guards than heads puts one head
on several body definitions as a matter of course. The renderer papers over it
per draw — `MODELNODETYPE_HEADSPOT` in `modelasm_c.c` re-parents the head's
roots to the headspot of the model it is *drawing* — which means the tree is
left pointing at whoever drew last, and `chrTick()` then resolves a head node
through that body's headspot instead of its own. It reads this model's rwdata
at a foreign index, takes what is there as the head's rwdata base, and writes
the hair toggle through it (`MODELPART_HEAD_HAT` is the hair). Two crash
reports from v3.2.2 and v3.3.3, one dialog each, both `0xc0000005` on the same
`mov %ebx,(%rax)` — `hatrwdata->toggle.visible = hatvisible` in `chr.c`.

Since 2026-09-12 the base does not come from the walk. `struct model` carries
`headspotnode`, the model's own headspot, found by `modelInit()` as it walks
the definition; `modelGetNodeRwData()` and `modelasmGetNodeRwData()` substitute
it for whatever headspot the walk arrives at. Only the base was ever
body-specific — a head's own node indexes start at 0 and `modelAttachHead()`
recomputes them identically on every attach — so nothing else had to move.

The audit is three gdb calls per chr and is how a stage is checked: for every
chr with a head, compare `modelGetNodeRwData(model, hatnode)` against
`headspot.rwdatas + hat's rwdataindex` read out of the chr's own headspot. On
v3.3.3: Infiltration (0x2f) had one guard of 26 resolving to an **unmapped**
address, Rescue (0x35) twelve of 42 landing in another chr's rwdata block, and
Escape (0x19) none. With the fix all three are zero, and a seeded fixed-step
frame is pixel-identical either way.

ASan only reports if the game's own `SIGSEGV` handler is out of the way: run
with `--no-crash-handler`, or every report is a bare backtrace in a dialog.

**A headless chr walked into another chr's head (2026-09-14).** The headspot
case links `node->child` to the model's own head, but did nothing when the
model had none - and the child is on the body's *modeldef*, which every chr
wearing the body shares. `body0f02ce8c()` sends a chr headless when a mod's head
number names a body model (Mario Characters' heads 64 and 65 are `CmariodaisyZ`
and `Cpelagicfem1Z`; body 75 is `CboshiplayerZ`, which has a headspot), so once
a headed chr on body 75 had been walked, the headless one went on into its head
and resolved each node through its own empty headspot: `rwdatas` NULL, the
lookup `NULL + index`, and `modelRender()` read a toggle's `visible` at address
0 - a v3.5.0 crash report. Every walker re-links the child before descending,
through `modelApplyHeadRelations()` or one of three inline copies (two render
walks in model.c, the matrix walk in modelasm_c.c), and all four now clear it
for a model with no head. The matrix walk mattered as much as the render: it
indexed the other head's matrices into a model that never allocated them.
Checked from gdb on the Mario mod: build a headed and a headless body-75 model,
re-apply the headed one, then the headless one, and count head nodes still
reachable (7 before, 0 after).

## ROM-resident structures — never grow these

`preprocessMpConfigs()` (`port/src/preprocess/misc.c`) casts raw ROM bytes to
`struct mpconfig` and strides by `sizeof`. `struct mpconfig` and `struct mpstrings`
are therefore layout-locked; `_Static_assert`s in `mplayer.c` enforce it.

That is why `MAX_BOTS_CONFIG` (8, ROM-resident) is separate from `MAX_BOTS` (80,
runtime). Anything reading `config->simulants[]` or `strings.aibotnames[]` must
bound by `MAX_BOTS_CONFIG`.

`mpsetup.chrslots` is a `u16` inside that struct, so simulant participation lives in
a separate runtime array (`g_MpSimSlots`), mirrored back into `chrslots` for the low
8 slots. It was a `u64` bitmask until 80 exceeded 64 — prefer per-slot flags over
bitmasks here.

## The third person body needs an anim slot, and the alarm can take them all

`g_MaxAnims = numchrs + 20` (`modelmgrAllocateSlots()`), and
`modelmgrInstantiateModel(def, withanim=true)` returns NULL when no anim slot
is free. `body0f02ce8c()` passes that NULL up and `playerTickChrBody()` handed
it straight to `chr0f020b14()`, which crashed in `modelSetAnim70()` - seen on a
Villa run with Guards Alerted at 80 guards in third person, 125 chrs alive of
128 slots. Stock never built a solo body outside gunmem, so it never needed a
slot; the port's third person builds one out of the heap.

Now: `setupLoadFiles()` reserves `PLAYERCOUNT()` chrs outside a match for the
players' bodies (both counts, they must agree), counts two weapons per alerted
guard under Akimbo and two waves of drops, and `playerTickChrBody()` treats a
NULL body as "no body this tick" with a warning, the way the gunmem branch
already did. Reproduce the failure path with `gdb -p` and `set g_MaxAnims = 1`
before pressing V; restore it and the body builds next tick.

## Kept bodies still disappeared: the chr vertex store's reaper (2026-09-09)

A tester with the body pool on still saw bodies vanish. The pool's own tick
never retired them; two stock reapers did, and neither knew about the pool.

`vtxstoreAllocate()` (`src/game/vtxstore.c`) hands out the writable copies of
a chr's vertices and colours - every bullet hit on a chr in view copies that
part's colours in for the blood (`chr0f0260c4()` from propobj.c), an
explosion copies vertices (`chrDisfigure()`), and the copy lives as long as
the model does. The store is sized for the N64 (`g_VtxstoreTypes`: 120
blocks of chr vertices or colours in a mission, 80 in a match, cut from a
300KB mema heap). When a request does not fit, the function marks **every
off-screen corpse but six, plus half of those six**, `fadewheninvis`, and
`chrTickDead()` deletes each one two seconds after it leaves the screen. A
kept body is a corpse to that loop. So past the eightieth bloodied body the
whole pool went the moment the player looked away, which is exactly how the
report read. `chrSpawnAtCoord()` has a smaller reaper of the same shape
(fewer than four chr slots free - it fades a dead chr for the slot).

Both skip `modBodyIsKept()` now, and `vtxstoreReset()` grows the two chr
types with the cap (two vertex blocks and three colour blocks a body, 200
and 300 entries) while `pdmain.c` grows the mema heap by the same 500
entries a body, 12 bytes each - 768KB at 128 bodies, 3MB at 500, from the
stage pool. When the store is full anyway the hit simply goes without its
blood: `vtxstoreAllocate()` returning NULL is handled at every caller. The
reaper logs `vtxstore: out of type N ...` when it fires and `chrTickDead()`
warns `bodies: kept body N removed off-screen` if a kept body ever reaches
the deletion, so a recurrence names itself. Measured on a seeded 32-simulant
match (`--rng-seed 12345 --fixed-step --mpsims 32 --spectate --endless`,
gdb counting `keptbody60 >= 0` chrs). A spectator match never reaches the
allocator at all - the blood copy wants the hit chr *on screen*, and the
camera sees no fights - so the reaper was invoked by hand: `set
g_VtxstoreTypes[2].val2 = 0` then `call (void*)vtxstoreAllocate(100, 2, 0,
0)` at 60s. Before the fix it marked 82 of 95 kept bodies and 12 seconds
later 33 were left; after it, 0 of 106 marked and 128 kept 12 seconds
later, the log carrying one `vtxstore: out of type 2` line. Attach with
`pgrep -x`, not `-f`: `-f` answers with the `timeout` wrapper's pid and gdb
then says `No symbol "g_NumChrSlots" in current context`.

## Heads fitted to bodies by measurement (2026-09-17)

`port/src/headfit.c`, called from `bodyCalculateHeadOffset()`. The user: "a lot
of heads are too tall for some bodies and they float", "if we can cause the
heads to auto fit to any body that would be helpful, n64 also"; chose **only a
head on a body it was not made for** and **height only**.

- **The ROM's fit is a type table.** A head's vertices move up or down by an
  amount looked up from the head's and the body's `type`, and a pair of one type
  moves by nothing. Nothing is measured, so GoldenEye X's, the pool's or a mod's
  rows float or sink by whatever their type byte says.
- **The release's meshes never took even that.** They are posed from the
  model's matrices and their own vertices, not the shifted N64 ones. Now every
  head copy's offset goes into a small registry (`headfitNoteApplied()`, keyed
  by modeldef and checked against its root node, since a freed modeldef's
  address comes back as another file), and `xblaMeshPose()` lifts a grafted
  head's palette along each matrix's own up by it.
- **What is measured.** The head's neck base: the 2nd percentile of its
  vertices' y, in its own space (a head has one). The body's neck top: the
  highest vertex of the lists under the headspot's joint, above the headspot.
  **A list's vertices are in the space of the matrix loaded when they were
  (G_MTX), not the node's**: a body's neck list holds its lower ring in the
  back's space, and read in the node's space the neck top came out 400 units
  up. `gebeanListVertexMatrices()` gives each vertex's matrix; its joint's rest
  is its position node's offset summed up the tree.
- **The rule.** Over 115 GoldenEye X-borrowed bodies with their own heads, the
  head's base sits 40-77 units under the body's neck top (Elvis, Mr Blonde and a
  hooded body are the outliers). So a head goes where the body's own head's base
  is (`g_MpBodies[].headnum`; its own head is then exactly 0), and a body naming
  none takes its neck top less 55. Clamped to 200.
- **Left alone**: the body's own head, the ROM's stock pairs of one type, and
  the release's pool (`gebeanIsPoolRow()`: its meshes stand on a host's models,
  whose geometry says nothing about the mesh).
- **A head nothing has loaded** is measured from a scratch copy: the file
  inflated into a buffer (`g_LoadType = LOADTYPE_MODEL` first, or the PC
  widening is skipped and the promote crashes; size as fileLoadToNew allows it,
  +0x8000) and only its pointers promoted - a real load binds textures and
  registers with the XBLA matcher, which a freed buffer must never be.
  `g_FileInfo[]` is put back. Cached per file, cleared at `lvReset()`.
- **HD necks under a foreign head.** A GoldenEye X body whose own head file
  carries its neck (`GEBEAN_BODY_WITH_HEAD`) blanks its neck lists; under any
  other head the collar stood open. `gebeanmats.neckblank` marks those groups,
  and they give way to the model's own N64 neck when the grafted head was fitted
  (`headfitWasMeasured()`). A pale ring can still show where the HD collar and
  the N64 stub differ (Jaws with Joanna's head).
- Survey and checks: `headfitSurvey()` from gdb at `lvReset` logs every body's
  neck and own head's base. Harness `build/gexcmp/heads/face2.sh` takes
  `body:head` pairs; the camera finds the chr by body number (simulant order
  is not config order), and `PITCH` holds the head up through the shoulder
  aims. Checked: eight mixed pairs in both looks against the previous binary,
  own pairs unchanged (stock pairs pixel-identical), stock heads on stock
  bodies with no GoldenEye X.
