# tools/gefidelity: GE Plus against GoldenEye itself

The instruments that judge the GoldenEye conversion against the originals:
the **N64 ROM**, through the GoldenEye decomp's native port on the oracle
host (`sdg@10.8.0.3:~/claude-007/007`, never modified), and the **XBLA
release** ("Bean"). They exist so a conversion fault is found by a sweep over
twenty missions instead of by a tester, and so an F3 investigation starts from
"run the twin driver" instead of two new one-off gdb scripts (there were ~400
of those in `build/gexrom/` and `~/dam-oracle/` when this was written).

**The rule, borrowed from the decomp port's `port/tools`: an instrument ships
with its null.** Every tool here runs a control on the same invocation as the
measurement - a clean copy must come out clean and a planted fault must be
found - and refuses to report when the control fails. A check that silently
stops checking looks exactly like a check that passes.

## The twin driver

| file | contract |
|---|---|
| `twin.py` | runs one gdb scenario on the oracle (over ssh), on ours (locally, from a run directory) or on both at once, same mission and difficulty; results in `OUT/ge`, `OUT/pd` |
| `common/gdbge.py` | the oracle side: `boot(levelid, difficulty)`, `tick()`/`until_tick()` on the level's 60ths, `hold()` Bond at a position and heading, `shot()`, `pads()`/`props()`/`chrs()`/`player()`/`world()` |
| `common/gdbpd.py` | ours, the same API name for name |
| `common/levels.py` | the twenty missions: GoldenEye's number (= our `--boot-ge-mission`), key, title, `LEVELID_*` |
| `common/solo.padscript` | the oracle's front-end walk into solo Dam (the level is swapped at `bossSetLoadedStage`) |

A scenario is a gdb Python file that imports `gdbge` or `gdbpd` by
`GF_SIDE` and calls the shared API (`world/dump.py` is ten lines). Our side
runs from a **run directory** (`--rundir`, default `~/wt/gefidelity-run`)
holding the binary, `data/`, `added-content/` with the GoldenEye ROM, and
`mods/` - never a player's install; the binary converts the ROM there on first
start (six seconds).

## Instruments

| tool | contract |
|---|---|
| `world/dump.py` + `world/worlddiff.py` | every setup record, object placement/rotation/scale/rooms/health/door state, chr position/rooms/health/AI list/head/body/weapons, every pad and Bond's spawn, both sides, diffed after measuring the level offset from the pads. Translations the conversion makes on purpose are in `TRANSLATE`, each with its reason; anything else is a finding. Null: GoldenEye's dump moved by an offset gives nothing, five planted faults give exactly five |
| `world/sweep.py` | the world diff over all twenty missions (`--diff` for each difficulty); `report.md` + `report.json`; a mission whose dump failed is listed as not compared |
| `world/inspect.py` | prints chosen setup records, their props, pads and any expressions in full on both sides, each as its own game's type - where a world-diff finding is looked at |
| `world/accepted.json` | findings looked at and accepted, each with the reason; counted, not listed |

| `census/census.py` | runs the Python converter with every ROM byte it reads tracked and lists, per record kind and GoldenEye field (named from the port's DWARF), the bytes that are **non-zero and never read** - data the conversion drops. Null: Dam's pad positions hidden from a second read map must all come back dropped. See `census/README.md` |
| `parity/parity.sh` | the converter twin gate: `tools/geconvert/geconvert.py` against `port/src/geconvert.c` (booted, or `--standalone`), every output file compared; exit 0 only when identical. Null: a flipped byte in a raw and a compressed copy must both be caught. The census instruments the Python twin, so its rows hold for the game only while this says IDENTICAL |

| `guns/sweep.py` | GoldenEye's 25 guns on both sides through each game's real controller path: clip and reserve, cadence (GoldenEye frames x2), reload and raise timing, every sound (mapped to GoldenEye's ids, random families compared as families), casings, impacts, dry clicks, dual rule, thrown fuse and flight. Null: one gun run twice must agree, a planted clip fault must come out as exactly one mismatch. See `guns/README.md` |
| `xbla/census.py` | the HD (Bean) draw census: what each release model file draws, read independently of the game (`beanref.py`), against what our builds walked and built under the env-gated hook (`census-hook.patch`, branch `feat/gefidelity-xbla`) - undrawn pieces, pictures that do not decode, files never loaded. Null: a struck draw and a planted skip must both come back. See `xbla/README.md` |

| `view/viewdiff.py` | matched-camera pictures, GoldenEye's against ours, on waypoint pads at four headings: both at GoldenEye's 320x220 viewport and 60 degree vertical field, Bond on the pad's own floor tile, guards' AI removed on the first frame, every shot at the same level tick; scored (structure, edges, colour) into `index.html` worst first; pairs whose cameras disagree are shown but not ranked. Null: each picture against itself scores 0, every matched pair must beat the same picture joined one pad off, and a planted block must raise 90% of scores. See `view/README.md` |

(Further instruments - the AI trace diff - are listed here as they land.)

## Running

```sh
cd tools/gefidelity
./twin.py both world/dump.py --mission dam --out ~/wt/gefidelity-run/out/dam
world/worlddiff.py ~/wt/gefidelity-run/out/dam
world/sweep.py --out ~/wt/gefidelity-run/out/sweep            # all twenty, ~6 min
./twin.py both world/inspect.py --mission dam --out /tmp/x --env GF_RECORDS=292,307
```

## Traps (each cost a run)

- The oracle enters on **00 Agent**; `boot()` writes `g_SelectedDifficulty`
  at `proplvreset2` (the setup load), because the front end writes it after
  `bossSetLoadedStage`. Ours sets `g_Difficulty` at `lvReset`.
- Without `PORT_LOCKSTEP=1 PORT_VI_LOCKSTEP=1` every gdb stop stretches the
  oracle's frame; `twin.py` sets both. `PORT_RENDER_FROM=999999` (rendering
  off) unless `--render`.
- Our setup list ends with type **0x34**; GoldenEye's with 0x30. Every other
  type number is the same and the records are one for one.
- GoldenEye's bound pads are 10000+n; the conversion appends them after the
  pads (`367 + n` on Dam).
- GoldenEye's decomp names an object's two health words the other way round
  from ours (`damage - maxdamage` there is `maxdamage - damage` here).
- An object's placement in GoldenEye is `runtime_pos`; for glass it moves
  `prop->pos` off it afterwards.
- GoldenEye's global AI lists (ids under 0x400) are ours 0x800 + n.
- `hidden`/`chrflags` bits mean different things in the two games
  (`CHRHIDDEN_BACKGROUND_AI` 0x200 is our `CHRHFLAG_CONSIDERPROXIES`); the
  diff prints both names, never trust a raw bit.
- The oracle's random heads are not seeded alike from run to run; a head
  mapping that is one-to-many is a random pool, not a fault, unless the pools differ.
- A free chr slot can hold a stale prop pointer; a live chr's prop points back at it.
- The oracle's pad script: `solo-quiet.padscript` (the default) presses Z once
  to dismiss the opening still; `solo.padscript` went on pressing Z inside the
  level and Bond fired his PP7 twice, which alerts guards (`--env
  GF_PADSCRIPT=solo.padscript` to have it back).
- `--boot-ge-mission` leaves `g_FrontInside` clear, which a mission started from
  GE Plus's folder screens has set; `gdbpd.boot()` sets it.
- Our port writes `pd.log` and `screenshots/` beside its executable, so every
  run gets its own run directory with the binary hard-linked into it.
- Kill only PIDs you started; `pkill -f` over ssh kills your own session.
