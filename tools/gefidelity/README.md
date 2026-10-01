# tools/gefidelity: GE Plus against GoldenEye itself

The instruments that judge the GoldenEye conversion against the originals:
the **N64 ROM**, run in ares on the oracle host (`--oracle ares`), with the
GoldenEye decomp's native port (`sdg@10.8.0.3:~/claude-007/007`, never
modified) kept for its debug info and gdb probes - it is incomplete, and where
the two disagree the cartridge is right - and the **XBLA release** ("Bean"). They exist so a conversion fault is found by a sweep over
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
| `common/aresge.py` | **the real cartridge**: the same API over `n64twin`, a scriptable headless ares (`ares/twin.cpp`, built on the oracle host beside `n64oracle`; line protocol in `ares/README.md`). Addresses from the decomp's N64 ELF, offsets from the -m32 port's DWARF (`common/gen_ares_layout.py` -> `ares_layout.json`). `--oracle ares` on `twin.py`, the default for `world/sweep.py` |
| `ares/padshot.py` | pictures from the real cartridge at a pad and heading - what settles a picture the native port disagrees on |

A scenario is a gdb Python file that imports `gdbge` or `gdbpd` by
`GF_SIDE` and calls the shared API (`world/dump.py` is ten lines). Our side
runs from a **run directory** (`--rundir`, default `~/wt/gefidelity-run`)
holding the binary, `data/`, `added-content/` with the GoldenEye ROM, and
`mods/` - never a player's install; the binary converts the ROM there on first
start (six seconds).

## Instruments

| tool | contract |
|---|---|
| `world/dump.py` + `world/worlddiff.py` | every setup record, object placement/rotation/scale/rooms/health/door state, chr position/rooms/health/AI list/head/body/weapons, every pad and Bond's spawn, both sides, diffed after measuring the level offset from the pads. Translations the conversion makes on purpose are in `TRANSLATE`, each with its reason; anything else is a finding. Null: GoldenEye's dump moved by an offset gives nothing, five planted faults give exactly five. **With `GF_WIDE=1`** (`--env GF_WIDE=1`; ares and ours, `common/wide_ares.py`, `wide_pd.py`, `ares_layout_wide.json` from `gen_ares_layout_wide.py`) the dump also carries what each record decides beyond its placement - every door's flags, type, key flags, auto-close, fractions, speeds, sound, glass distances, portal and sibling; pickups' weapons and pairs; crates' contents; keys; monitors' owners and pictures; autoguns' target and limits; tinted glass; armour; every guard record (pad, body, head, AI list, presets, hearing, vision, spawn and clone flags) and its grenade odds; every object's flag words bit by bit with both games' names; the chrs' held weapons and their flag words; and GoldenEye's whole tile graph (room, special, points, links as tile indices) - compared by `world/widediff.py`, whose `TRANSLATE` gives the converter line behind each rule; the null plants a door flag, a tile link, a guard field and a flag bit as well. On 20 missions: 18 wide findings, eight of them the CE's fixes |
| `world/sweep.py` | the world diff over all twenty missions (`--diff` for each difficulty); `report.md` + `report.json`; a mission whose dump failed is listed as not compared |
| `world/gate.py` + `world/compare.py` | **the fidelity gate**, in legs - `world`, `view`, `guns` and `ai` (N64 look against ares), `hd-census`, `hd-world` and `hd-view` (HD look against the release in Xenia), `census` (the converter's ROM reads) and `convdiff` (which converted files changed - it informs, never fails), both judged on source trees with `--base-tree`/`--test-tree`, and `replay` (`tools/ci/replaytest.sh compare`: which seeded runs diverge and where - it informs, and with `--neutral`, for a change meant to leave gameplay alone, a divergence fails the gate); a leg whose tool is missing is skipped and says so; the oracle's side is swept once and reused. It sweeps a base and a test binary (or reuses a base sweep) and lists placement findings fixed, new, better and worse by mission; exit 1 when a placement finding appears or grows, behaviour findings listed but never failing it. It answers "did this change move GE Plus towards the originals or away", and through its replay leg "did it change gameplay at all". Null: a report against itself is unchanged, and one removed plus one planted finding come out as exactly that |
| `world/inspect.py` | prints chosen setup records, their props, pads and any expressions in full on both sides, each as its own game's type - where a world-diff finding is looked at |
| `world/accepted.json` | findings looked at and accepted, each with the reason; counted, not listed |

| `census/census.py` | builds `port/src/geconvert.c` (the only converter; the Python twin was retired 2026-10-01) with every load instrumented (`census/ctrack/`: GCC kernel-address callbacks, geconvert.c untouched, output byte for byte the plain build's) and lists, per record kind and GoldenEye field (named from the port's DWARF), the bytes that are **non-zero and never read** - data the conversion drops - with a **read by the cartridge** column from `census/ares/run.py` (the read watch in n64twin over twenty missions). `census/gatesweep.py --tree SRC --out DIR` is the gate's `census` leg. Null: Dam's pad positions hidden from a second read map must all come back dropped; the cartridge column's: every Dam pad's plink read, its pad-name strings never. See `census/README.md` |
| `convdiff/` | **the conversion diff**: `convert.sh --tree SRC --out DIR` converts the ROM with that tree's `geconvert.c` built alone (~10 s); `diff.py A B` lists the files a change altered with the first differing offset (inflated where compressed, bg `.seg` part by part). The gate's `convdiff` leg: it informs, never passes or fails. Null: a one-byte flip in a raw and a compressed copy must both be seen |

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

## Which original judges which look

- **N64 look:** the cartridge in ares, as it is. No Community Edition allowance:
  where the regular build carries a rebuilt CE fix, that is a finding against
  the cartridge (FINDINGS.md lists them), never an accepted difference.
- **HD (XBLA) look:** the release with the Community Edition applied, in Xenia,
  against ours with `Mod.GeXblaCommunityEdition=1` and
  `added-content/CommunityEditionUpdaterV6.zip` - GE Plus's XBLA mode runs the
  CE, so a retail-against-CE comparison judges the wrong thing.

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
- **No controller may reach our own headless runs.** The Xenia rig's virtual
  Xbox 360 pad (045e:028e) is visible to every SDL program on the machine: it
  leaked into a gun run (43,366 frames). `twin.run_pd`, `guns/run.py` and
  `tools/ci/replaytest.sh` set `SDL_GAMECONTROLLER_IGNORE_DEVICES` (DualSense and
  the virtual pad), `..._EXCEPT=0x0000/0x0000` and `SDL_JOYSTICK_HIDAPI=0`; a new
  runner must too.
- **One Xenia rig on the box at a time, and only through `xenia/rig.sh`.** The
  virtual Xbox pad is a `/dev/uinput` device, machine-wide, and Xenia with
  `--hid=sdl` takes input from every virtual pad - so a second rig's pad drives
  the first rig's Xenia even on another display (two agents' Dam captures
  collided this way). `rig.sh start` holds an exclusive lock on
  `~/wt/gefidelity-run/xenia.lock` until `rig.sh stop`; a `pgrep xenia_canary`
  check is a race, not a lock.
- **`n64twin` has one owner at a time** (`ares/OWNER`): only the owner edits
  `ares/twin.cpp` (the tree's copy is the source of truth) and builds, and only
  with `ares/build.sh` (a host build lock, installed atomically as
  `build/n64twin.installed`). `aresge.py` runs a private snapshot of the
  installed binary, refusing anything that is not a whole ELF, and takes one of
  `GF_ARES_SLOTS` (5) host slots per run. Each of these was a collision once:
  eight missions died with "Permission denied" mid-link, six with "Exec format
  error" on a 0-byte copy, and four agents ran nine `n64twin` on eight cores.
- **The cartridge is only repeatable with its randomness pinned.** Two runs of
  one binary differed in every random head, sleep timer and patrol: GoldenEye's
  two generators (`g_randomSeed`, `g_chrObjRandomSeed`) are seeded from the
  boot's timing, and the random-head rotation (`current_random_male_head`,
  `_female_head`) starts somewhere new each boot. `aresge.boot()` pins all four
  at `proplvreset2` (`GF_SEED`, default 1; `off` to leave them); since then the
  same binary gives byte-identical dumps at ticks 1 and 300, so a pinned run is
  as repeatable as ours with `--rng-seed`.
- **`ai/aimap.py` still maps AI offsets through the retired Python converter**
  (`tools/geconvert/gesolo.py`'s `convert_ailist`, kept only for it and for
  `fit/`). A change to AI conversion in `geconvert.c` that is not mirrored there
  makes the AI trace diff misplace offsets; the lasting fix is to map by walking
  each game's converted list with its own command lengths.
- **Each source tree has its own toolkit copy on the oracle host**
  (`~/gefidelity/trees/<tree>-<hash>`, `twin.GE_TOOLS`; `GF_GE_TOOLS` overrides). It was
  one shared `~/gefidelity` until 2026-10-01, when three worktrees' syncs (`rsync
  --delete`) reverted each other's oracle code and scenarios mid-run for an hour.
- **Sweeps from before the evening of 2026-10-01 are not comparable with
  today's toolkit**; re-sweep the base with the test's toolkit. That day
  `aresge.py` began reading 8 room bytes (PropRecord.rooms is u8[4], but
  `chrpropUpdateRoomList()` writes up to seven and the terminator on into
  unk30 and the cartridge reads them all), and a door's health as the 16.16
  setup word `setupDoor()` never divides (the float printed 0.0 for 1000); the
  guns' scenarios hold the pitch with the cartridge's look-ahead cleared (it
  wound a poked -30 back to -4 in 46 ticks: every earlier gun sweep fired
  GoldenEye's guns nearly level); the AI sampler reads the cartridge every
  video frame (once a game frame let two AI passes hide a Yield) and names a
  chr first seen mid-run spawn@<list>; the view tour runs ours with Head Roll
  and Always Show Target off, and its oracle-repeat null holds the cartridge's
  jitter against the one-pad-off median.
- **A converter bump needs a run directory per side.** `gate.py` sweeps base
  and test under one `GF_RUNDIR` (the view leg: `GF_RUNDIR_ROOT`), whose
  `mods/GoldenEye Arenas` the first binary to start converts. Sweep the base by
  hand with each leg's own tool (`world/sweep.py`, `view/gatesweep.py`,
  `guns/gatesweep.py`, `ai/gatesweep.py` - the arguments gate.py's `LEGS`
  gives them) in a run directory of its own, then `gate.py --base-report DIR`
  with the test's. Boot each run directory once alone before a sweep: a `-j`
  sweep's first starts race the conversion. `--base-report` looks for
  `DIR/<leg>/report.json` (base1001 keeps some legs in `<leg>/base`, which it
  does not find). The census and convdiff legs build each tree's converter
  themselves; `tools/ci/replaytest.sh` stages two converters on its own.
- **The world leg's behaviour keys flip between runs of one binary** (Train's
  player.pos 13.3, Frigate/Aztec hidden 0x100 on one chr, Statue chr 7's
  rooms came and went across pd.base's own sweeps): before blaming a change for
  a "new" behaviour line, sweep the mission again and sweep the base on it.
- **The Python twin applies the CE's patches only with `GE_ROM_PATCHES=1`**
  (converter 97 made them the HD look's); `ai/aimap.py`'s null failed on Surface
  while it still applied them. The other way round too: the ai leg's sweep of a
  binary from before converter 97 (whose N64 conversion carries the patches)
  needs `GE_ROM_PATCHES=1`, or aimap's null fails on Surface's list 1057 (0x421,
  the paired Klobbs) and the sweep stops after its traces.
- Kill only PIDs you started; `pkill -f` over ssh kills your own session.
