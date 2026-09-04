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
`modelmgrInstantiateModel()` now does.

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

ASan only reports if the game's own `SIGSEGV` handler is out of the way: run
with `--no-crash-handler`, or every report is a bare backtrace in a dialog.

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
