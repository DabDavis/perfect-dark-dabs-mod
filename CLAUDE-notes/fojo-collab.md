# The Friends of Joanna collab tree

`../pd-fojo-monorepo-collab/` is Catherine Reprobate's umbrella repo for the
**Friends of Joanna** campaign mod: four squashed subtrees (`pd-fojo/` port fork,
`pd-fojo-n64/` decomp branch with their setups, `pd-fojo-basedir/` mod data,
`docker-caroll/` Python tooling) plus the March 2026 All in One drop as a
reference. First looked at 2026-09-04, at 125 commits (2026-08-04 to 09-03).
The best entry point is `.github/skills/*/SKILL.md` — sixteen docs that
describe every subsystem, and they are more accurate than the code comments.

It forks the same stock port we do (`port` branch here), so the two trees are
directly comparable, and **they cannot be merged**: both rewrite `mod.c`,
`romdata.c` and `fs.c` in opposite directions, and each modconfig parser aborts
the whole file on a block it does not know — our `weapon` blocks kill their
load, their `HeadsAndBodies` blocks kill ours.

## What they built that we have not

- **Many mods active at once.** Up to 64 `--moddir`s, no scanning, no menu.
  A 64×8192 `fileSlots[mod][id]` table; a load searches every mod row in
  *reverse* command-line order, then the ROM. This has the collision our
  [mods.md](mods.md) note avoids on purpose: for a stock filename shipped by
  two mods, the last `--moddir` silently wins.
- **Mods add file ids.** Each mod ships a binary `filetable.dat` (magic `PDFT`,
  v1/v2/v3) built by `pdt build-mod-filetable` from `mod_*_filetable.json`.
  Entries replace a vanilla name or take a fresh id above 2018. The engine
  never reads the JSON.
- **Files pulled from other ROMs.** A v2 manifest names companion ROMs
  (`gex.z64`, jpn-final); the engine mounts them at boot and points the slot at
  the bytes. Only uncompressed source files work — the compressed tail is a
  stub in `romdataFileLoad()`.
- **New heads, bodies, hands from modconfig.** `HeadsAndBodies`, `MpHeads`,
  `MpBodies` blocks realloc `g_HeadsAndBodies` and the MP slot arrays and
  register names, so the MP carousel and CI bios find them; `requiresrom "gex"`
  skips a block when the ROM is absent. This is the one thing we lack that
  their tree actually demonstrates working. Check it against
  [chrs-and-memory.md](chrs-and-memory.md) before copying — that note is about
  why ROM-resident tables do not grow, and theirs grow.
- **Texture ids past the vanilla 3503.** A per-mod texmap allocates ids in
  3600–4095 and `modeldefLoad()` rewrites `G_NOOP` texnums in the model's
  display list. Per-model aliases (`CheadbrosnanZ/055d.bin`) stop two models
  fighting over one source id. Hard ceiling: 495 mod textures across all mods,
  because the field is 12 bits.
- `ext_tex.c`: PNG overrides per mod with ownership stamps — same purpose as
  our `texpack.c`, no shared code. Plus a 3000-line Dear ImGui overlay, a
  per-profile extended INI, and their own N64 setup builds.

## What we have that they do not

One mod from the Load Mods menu, live swap and the segs restart rule;
`importmod` from an xdelta with asset validation; `modcodediff`; the `weapon`,
`weaponfunc`, `tvscreen` blocks; skipping a bad stage block instead of the
whole file; texture packs off the render thread; ghost trials; the recorder;
the updater.

## Rough edges confirmed in their source (not from the docs)

- `modLoadAIO()` never assigns its result: returns failure and logs an error
  on every mod switch. It is not AIO-specific despite the name — it re-applies
  every mod's head/body blocks.
- The `texture` modconfig block parser cannot parse the syntax its own doc
  shows.
- Two romdata loops skip the last mod row (`mod < g_NumModDirs - 1`).
- `force_vanilla` on a stage still loads mod setups: any filename containing
  `setup` is force-allowed.
- `data/mpHeadsAndBodiesTable.dat` is a `merge-heads` artifact; nothing in the
  engine reads it.
- Hardcoded probes for specific mod and file numbers remain in the texture
  load path; `modLoadTextureSurfaceType()` is declared and never defined.

## Decision (2026-09-04): not merged, not ported

Their tree stays its own work with its own goal, and this fork stays on its
path. Nothing from it is on this fork's roadmap. If the head/body or texture
id ideas are ever wanted here, that is a separate piece of work, done from
the shape described below and not by lifting their code.

## How their head/body blocks grow the tables (read 2026-09-04)

`modConfigParseHeadsAndBodies()` in their `port/src/mod.c` copies the stock
`g_HeadsAndBodies` (renamed `g_HeadsAndBodiesOriginal`) to the heap on first
use and reallocs one entry per block; `g_MpHeads` and `g_MpBodies` get the
same treatment, with `bodyslotnum 1` meaning "next free slot". Names go in a
side table so `head_catherine` resolves to an index later. `filenum` is
widened to `u32` and carries the mod number in the top 16 bits, so every
consumer that indexes `g_FileInfo` must mask with `0xffff`. Vanilla
consumers already read through a count variable, so the menus see new
entries with no further change.

What it gets wrong, all of the "fixed width nobody wrote down" shape:

- **The stock table ends in a sentinel** (index 0x97, `filenum` 0) and
  `bodiesReset()` clears cached `modeldef` pointers by walking to it. New
  entries land *after* the sentinel and are never cleared. It only works
  because `modSwitch()` re-parses every mod's blocks on every stage change,
  which overwrites each entry by name with a zeroed copy. Remove that
  re-parse and the second stage draws a head out of a wiped pool.
- **`chrdata.headnum` is `s8`.** Vanilla heads stop at 0x4a so it never
  mattered. A head appended at index 152 stores as -104, and
  `chraction.c` reads `g_HeadsAndBodies[chr->headnum].ismale` through it.
  The head still draws, because the draw path takes the index as an `s32`
  parameter; the voice gender check and the `HEAD_JONATHAN`-style
  comparisons read before the array.
- `MpHeads` / `MpBodies` blocks append unconditionally, so each re-parse
  grows the arrays. Nothing in their shipped mods uses those blocks, so it
  is dormant. `modResetMplayerArrays()`, which would fix both, has no
  callers.
- A saved profile holds `mpheadnum` as a `u8` index into a table whose
  length now depends on which mods are mounted. They clamp the menu preview
  to the vanilla count and leave `mpGetHeadId(u8)` unbounded.

None of this contradicts [chrs-and-memory.md](chrs-and-memory.md): the
table is not ROM-resident, and the MP per-chr head copy and the one-head-
two-bodies rule are untouched. If we do this, do it as a fixed runtime
capacity (`MAX_MODHEADS`, like `MAX_MODSTAGES`), keep the sentinel at the
end, widen `chrdata.headnum` to `s16`, and grep for every `u8`/`s8` that
holds a head or body index before raising anything.
