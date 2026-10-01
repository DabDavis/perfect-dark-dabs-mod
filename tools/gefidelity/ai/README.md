# ai/ - the AI trace diff

Does a converted mission's script run the way GoldenEye's does? Each side runs
the mission for its first 90 s (5400 ticks) with Bond invincible and unseen at
his spawn; every AI command each chr runs is traced on both; the traces are
put in one numbering through the converter's own offset map, and each chr's
stream is compared run by run.

| tool | contract |
|---|---|
| `aimap.py` | GoldenEye (list, offset) <-> our (list, offset), from `gesolo.convert_ailist()` itself, recompiled with one hook line. **Null:** the instrumented walk must rebuild every list of the converted setup the game loads (`mods/GoldenEye Arenas/files/Usetupgs<key>Z`) byte for byte, IfBondY's offset value aside, or it refuses |
| `trace.py` | the gdb scenario for `twin.py`: GoldenEye's own `PORT_AI_TRACE` printf plus a `GFAI` marker from a breakpoint at `ai()`'s first instruction (tick, chr, list id, offset, return list); ours `--ai-trace`. Plays our mission's opening (twin.py passes `--skip-mission-intro`; GoldenEye's lists wait on the still and the swirl) unless `GF_AI_INTRO=0` |
| `aidiff.py` | one mission: first divergence per stream, classed `logic` / `world` / `timing` / `rng` / `window-end` by the command the two last agreed on; later deterministic splits after resyncing; commands one side ran and the other never did. **Nulls, every run:** each GoldenEye command is the opcode its bytecode holds where the list tracking says it is; each of ours is the converted command at that offset; each side aligns with itself; a command planted mid-stream in a copy is reported at exactly that step. Exit 2 when any fails. `--null-twice` also traces ours a second time and requires the two to be identical |
| `sweep.py` | all twenty missions: GoldenEye traces `-j 3` at once on the oracle host, ours two at once, then the compare; one `report.md` with every deterministic split first. `--skip-run` re-compares |
| `resume.py` | the cartridge mode (default, `--oracle ares`): ares hooks function entries only, so trace.py samples every chr's `(ailist, aioffset)` per frame (`g_ChrSlots`, `g_ActiveChrs`, vehicle records); the chain of Yields each list reached is compared with ours' run starts, and the conditional that went the other way is recovered from GoldenEye's bytecode control flow. **Nulls:** every sample sits just after a Yield (or at a list's start) in a list the map knows; self-alignment; a planted step found |
| `gatesweep.py` | the gate leg (`world/gate.py`'s `ai`): `--bin --out [--oracle] [--missions] [--reuse-oracle BASE]` -> `report.json` of `ai.diverge` / `ai.timing` findings (key `chr12:list0x40c`, mag = ticks left after the split). **Null:** ours traced twice on the first mission gives the same `ai.diverge` keys |
| `spawnrefused.py` | which setup chrs our `bodyAllocateChr()` refuses for "something in the way at the pad" (GoldenEye spawns at a clear spot within 20 units, `getposstan()`); the cause behind a `missing` chr stream |

```sh
tools/gefidelity/ai/aidiff.py --mission dam --run          # ~5-10 min, both sides
tools/gefidelity/ai/aidiff.py --mission dam                # re-compare the traces in the out dir
tools/gefidelity/ai/sweep.py                               # everything (~90 min); ~/wt/gefidelity-run/ai-out/report.md
tools/gefidelity/ai/aimap.py $(seq 0 19)                   # the map's null alone, every mission
```

The report also gives each mission's **AI cadence**: how often a chr's list
runs, median ticks between runs, both sides. GoldenEye ticks every chr every
frame; ours, in solo play, splits off-screen props over seven round-robin prop
states (`g_Vars.numpropstates = 7`, `src/game/varsreset.c`) and ticks each every
seventh frame with the time banked, so an off-screen guard's list runs at
7/14/21 ticks where GoldenEye's runs at 1 or 15-19. Measured 1.24x on Dam and
Facility.

Reading a finding: `chr12 step 2 ... after IFRandomGreaterThan@2051+33
GoldenEye ran PlayAnimation@2051+36, ours Label@2051+47` means the twelfth
chr's list 2051 (ours; GoldenEye's global 3) took the other branch at GoldenEye
offset 33. Commands are named by GoldenEye's names at GoldenEye's offsets in
both columns; `(ours: ...)` gives the converted commands' own names.

Traps met building it:
- **The two sides run a chr's list at different rates** (the cadence above),
  so a run repeated unchanged is folded into one step, and a loop exit on one
  side against "still looping" on the other is judged against the loop body,
  not the next step; a side that only goes on round the same cycle after the
  other's trace ended is `window-end`.
- **Our trace prints command 0 as `000000`** (`%#06x` drops the 0x for zero),
  so a pattern wanting `0x` loses every GotoNext.
- **GoldenEye runs every background list as chr 254**; ours numbers them from
  4000. Both are keyed by their first list; chrs made at runtime (5000 and up,
  GoldenEye's opening Bond is 5036) the same way.
- **A chr is followed, not its number.** `SetMyChrNum` renames a background
  or spawned chr mid-run (ours prints the new number from that command on),
  and GoldenEye reuses a chr slot for the next spawned guard: GoldenEye's
  streams are keyed by record address plus occupant, ours by chr number with
  the renames aliased.
- **The harness's own opening.** Our cinema hands control back with
  `g_Vars.bondvisible = true`; a write watchpoint holds it at 0 when Bond is
  meant unseen, or every sight check splits. GoldenEye's pad script dismisses
  its still at frame 1300 (about tick 190); ours dismisses its still at that
  tick too (`GF_AI_STILL_TICK`), or lists that test what Bond holds split on
  the openings' lengths. Ours plays the US cartridge's setups
  (`Mod.GePlusRevisionFixes` 0; `GF_AI_REVISION=1` for the later ones).
- **The map follows the converter the game runs.** geconvert.c renumbers a
  duplicate background list (Surface's second 4106 is 4107); the Python twin
  drops it - `aimap.renumber_bg_duplicates()` and the two-way null.
- **Two of our runs in one directory write one pd.log**: each mission gets
  `ai-rundir-<key>` with a hard link to the binary.
- Ours is deterministic (`--fixed-step --rng-seed 1`): `--null-twice` requires
  two traces of ours to be identical in every class (Dam: identical).
- **The oracle runs at its own pace** - about 17 ticks a second under gdb,
  slower when the host is shared; `PORT_TIME_SCALE` does not help (lockstep and
  real-time timers pace it). A 3600-tick trace is ~5 minutes alone, ~15 with
  three at once. The per-`ai()` breakpoint is not the cost.
- gdbpd.boot() deletes every breakpoint on its way into the level, so a probe
  that must catch setup (spawnrefused.py) or an AI pass before the first frame
  (trace.py) boots itself.
