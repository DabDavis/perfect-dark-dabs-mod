# view/ - the view diff

`viewdiff.py` takes the same pictures in GoldenEye (the decomp's native port on
the oracle host) and in our N64 look, scores each pair, and writes an HTML
contact sheet sorted worst first. It finds what a tester would see first:
missing or moved geometry, wrong props, wrong sky, fog, lighting or texture
colour.

| tool | contract |
|---|---|
| `viewdiff.py run --mission M [--oracle ares\|port]` | one mission: tour both sides, score, `OUT/M/index.html` + `scores.json` + `pairs/` |
| `viewdiff.py sweep [--missions a,b]` | every mission (the oracle <= 4 at once, ours 1), plus `OUT/index.html` |
| `viewdiff.py score DIR...` | (re)score runs already on disk; exit 2 if any null fails |
| `--bin ./pd.fix`, `--reuse-oracle BASE` | our binary (relative to the run root); GoldenEye's pictures taken from a previous run instead of shot again |
| `gatesweep.py --bin B --out DIR [--oracle ares\|xenia\|port] [--reuse-oracle BASE]` | the fidelity gate's `view` / `hd-view` legs: `DIR/report.json`, one finding a pair (`view.pair`, key `pad:heading`, mag the score; unranked pairs as `view.unranked`); exit 2 when a mission's null failed, a mission did not run, or a second sweep of the first mission on the same oracle pictures moved a score by more than 0.05 |
| `tour.py` | the scenario both sides run (through `twin.py`); writes `manifest.json` + one shot per pad and heading |
| `ares_side.py`, `ares_view_syms.json` | the tour's GoldenEye side on the cartridge (twin.py `--oracle ares`, the default): every step in memory, no function calls; the extra ROM addresses (`nm build/u/ge007.u.elf`) |
| `xenia_side.py` | the tour's GoldenEye side on the XBLA release ("Bean") in Xenia (`--oracle xenia`): through `common/xeniage.py` and `xenia/run_scenario.py`, the rig's lock held (one Xenia, one virtual pad on this machine); Bond on the pad's tile at the height `stanGetPositionYValue` gives with GoldenEye's `levelinfotable` scale, Bean's eye height measured at spawn |
| `fovfit.py OUT/<mission>` | the release's field of view against ours, measured from matched pairs (the zoom that maps our picture onto Bean's; its null: ours against itself fits 1, against itself zoomed 1.10 fits 1.10); feed `bean_fovy` back as `--bean-fovy` |
| `texsample_ares.py` | where the cartridge samples an image: finds it in RDRAM by its first bytes (`GF_TEXHEX`, as our renderer loaded it), pictures it as is, under a ramp along its rows and one along its columns (a surface's brightness is then its texel row and column), or filled with one byte (`GF_FILL=ff`: the shade alone); the ramps I4 only, the fill any bytes with `GF_FILLLEN` (a CI image's palette: `GF_TEXHEX` its first bytes, `GF_FILL=ff`, `GF_FILLLEN=2 x entries`) |
| `lightpoke_ares.py` | at the tour's camera on the cartridge: GoldenEye's GlobalLight found in RDRAM by its bytes and rewritten, one picture per setting (`GF_LIGHTS='orig;px:0,200,127,0,0;...'`, ambient, light, direction), optionally over a palette filled white (`GF_TEXHEX`, `GF_FILLLEN`); a lit surface's shade under a light along each axis tells which space the RSP reads the light in. Two copies of the bytes on Goldfinger 64 - the rooms read the second (`GF_LIGHTONLY=1`, 0x80044840). The first picture after `orig` can be a frame stale: put a throwaway setting first |
| `ramfind_ares.py`, `ramdump_ares.py` | at the tour's camera on the cartridge: count byte patterns in RDRAM (`GF_PATS`), or dump a range (`GF_FROM`..`GF_TO`) to `ram.bin` |
| `--oracle-only`, then `--reuse-oracle OUT` on the same OUT | the oracle's pictures now, ours later (one game of ours at a time on this box) |
| `VIEW_SET='var=value;...'` (environment of `viewdiff.py`) | gdb assignments in our game after its boot: an A/B of a setting a run's pd.ini cannot reach (twin.py runs on a fresh save dir) |

**--oracle xenia is the HD look against the release, both with the Community
Edition** (user 2026-10-01: "use Community edition, since that is what we use
for ge plus"): Bean's CE build (`.xbla-work/ge-bean/BeanCE/defaultCE.xex`,
unless the rig's own default already is a CE build) and ours with
`Mod.GeXblaCommunityEdition=1`, the updater in `added-content/`, the HD keys on
(`view-hd-rundir-{0,1}`). Never retail against CE. Out:
`~/wt/gefidelity-run/view-xenia`.

**The oracle is the cartridge in ares by default** (user 2026-10-01; out
`~/wt/gefidelity-run/view-ares`); `--oracle port` is the decomp's native port
under gdb (out `view-out`). The port's green Frigate sea was the port's: on
the cartridge the sea is blue like ours. On ares GoldenEye runs first and our
window and field of view are set from the viewport and fovy it reports
(`g_ViBackData`: 320x220 at y=10, fovy 60 on every mission measured), and our
tour starts on the cartridge's first-shot tick with the cartridge's own pad
list.

Ours runs from `~/wt/gefidelity-run/view-rundir-{0,1}` (binary hard-linked, so
pd.log and `screenshots/` are private to the run; data/, added-content/, mods/
are symlinks to the run root).

## How a pair is made the same picture

- **Same place.** Pad N and heading H mean the same in both games. GoldenEye's
  Bond goes onto the pad's own floor tile (`pad->stan`) the way its spawn
  does (`change_player_pos_to_target`; on ares the same writes made by hand,
  the tile's height computed as `stanGetPositionYValue` does - it agrees with
  the port's call to the hundredth on Frigate's decks); ours onto `cdFindGroundAtCyl` under
  the pad; each game then puts its own eye over its floor. Look Ahead is off
  and unlatched on both (`automovecentre*`, `docentreupdown`). Every pair's
  cameras are recorded and checked after the level's offset: more than 4 units
  apart across the floor or 1 degree apart in where they look is a *camera
  mismatch* and is not scored. A difference in eye height alone is scored and
  shown (`eye -9.9`): ours stands 8-11 units under GoldenEye on Dam; that is
  the world diff's finding, reported here per pair, not compensated.
- **Same time, same world.** Every chr's AI list is taken away on the level's
  first frame on both sides (`VIEW_FREEZE=1`), and shot n is taken at level
  tick 400 + 40n on both: guards stand where the setup put them and the trucks
  their scripts drive stay parked. A guard or vehicle somewhere else is then
  the conversion's, not two runs of a script drifting apart.
- **Same frame.** GoldenEye draws a level into a 320x220 viewport at y=10 of
  its 320x240 framebuffer (`viSetFovY(60)`, aspect = viewport w/h); ours runs
  at 640x440 (a pd.ini in the run's own save directory), 60 degrees vertical.
  The viewport is cut out of GoldenEye's picture and both are compared at
  320x220. Checked by overlaying a pair: the static geometry and the gun line
  up to the pixel.

## The score

0 is the same picture: the mean of three terms on GoldenEye's 320x220, taken
at a scale where the two renderers' texture filtering and dithering are gone
(gaussian blur 2) and a missing wall is not:

- **struct**: 1 - SSIM of the blurred greys at half resolution;
- **edge**: 1 - the correlation of blurred edge maps (geometry, moved props);
- **colour** x 8: the mean difference of 20x20 block colours, 0..1 (sky, fog,
  lighting, a wrong texture).

Tuned on Dam: matched pairs median 0.19-0.21, one pad off 0.60. The difference
map shows coarse SSIM loss and block colour difference in red over
GoldenEye's grey, only past the level the renderers alone reach (0.3).

## The null (runs on every invocation; the ranking is refused when it fails)

- every GoldenEye picture scored against itself must give 0;
- **alignment**: each matched pair against the same GoldenEye picture joined
  to our picture of the **next pad** (the next heading when the run has one
  pad): matched median under 0.85 x the one-pad-off median, and matched better
  on at least 75% of pairs;
- **sensitivity**: a flat 64x64 block planted in our picture (black over a
  light region, white over a dark one) must raise the score of 90% of pairs by
  0.05. A mid-grey block was invisible on Facility's walls and failed this,
  which is what it is for.

- **oracle twice** (ares): the cartridge's tour is run a second time
  (`ge_repeat/`); its median difference from the first (the jitter: the tick a
  picture lands on varies by 1-3, and the gun sways) must be under 0.25 x the
  one-pad-off median - the cartridge repeats itself far more closely than a
  wrong picture scores - and under 0.06 outright (pinned runs measure
  0.01-0.04; an unpinned oracle is what this is here to catch). A pair whose own picture moves more than 0.15 is not
  ranked. `aresge.boot()` pins GoldenEye's RNG seeds and random-head rotation,
  so even pictures with guards in them repeat. `--no-oracle-repeat` skips it.
  Xenia runs in real time and does not repeat; it has no such leg. Until
  2026-10-01 the jitter was held against the *matched* median, which failed
  the null as soon as ours came within 4x of the cartridge's own noise (Bond's
  eye fixed: Bunker, Silo, Cradle, Runway) - it punished getting closer. How
  near the matched pairs are to the jitter is reported (`oracle_repeat.vs_matched`);
  near 1, the order of the best pairs is the cartridge's own noise.

## Pairs that are not ranked (shown, with the reason)

- cameras more than 4 units apart across the floor or 1 degree apart in where
  they look;
- eyes more than 40 units apart in height: the two stand on different floors;
- a chr within 40 units of the camera on either side: guards are frozen on
  their setup pads, a waypoint pad can be one, and the camera is inside him
  (GoldenEye's eye sees his hat brim, ours, 8 units lower, his jacket).

## Traps met building it

- **Our look is theta and pitch alone.** Ours runs with Head Roll off: the head
  animation's look tilts the view by tenths of a degree in a phase the whole
  simulation feeds, and a change that only moved which rooms are on screen (the
  portal walk) moved Streets 89's camera 0.45 degree and its score 0.05 with
  nothing drawn differently. Always Show Target is off too: on GE Plus it keeps
  GoldenEye's sight up with the gun lowered, which the cartridge shows only while
  aiming. A base swept before this (base1001) is not comparable: sweep it again.
- **Our side takes each picture at the oracle's own tick** (`VIEW_TICKS`, from
  the oracle's manifest), not at `T0 + n * DT`: the release in Xenia runs in real
  time and fell to twice the schedule (Dam's last pictures at tick 10987 against
  our 5844), by when the truck its freeze did not park was somewhere else.
- **Bond in the release (Xenia).** `xeniage.place()` writes GoldenEye's own
  fields, which Bean keeps, but Bean also keeps x and z of its own at
  player + 0x4ec/0x4f4, 0x530/0x538 and ten times them at 0x4e0/0x4e8, and slides
  Bond from them; `xenia_side.py` writes those too (the README's +0x150 is not
  one - it moves to unrelated numbers with Bond). Out of some places Bean still
  will not let him be put elsewhere (Dam's pad 329, where he never settles and
  sinks; every warp from there to 323 was refused, from the spawn or 317 it
  lands): the camera check leaves such pairs unranked, and Bean draws the rooms
  of the wrong place there (a blue void under Dam's cliffs) - an oracle fault.
  Bean's freeze clears every chr's AI list but did not park Dam's truck.
- **Bean's field of view** (`fovfit.py`, both nulls passing): our picture maps
  onto Bean's at a zoom of exactly 1.00 across on Dam (118 pairs) and Facility
  (79); up and down 0.97 and 0.92 with a spread of 0.22-0.26 - the rig's 695
  rows hold the release's 720 (0.965) and our eye is 8 units low, which moves
  near things up and down. So the release's view is ours at FovY 60 in 16:9,
  and both are compared at 320x180 from their whole frames.

- **Pads that end the mission.** On the cartridge, putting Bond down on
  Bunker 1's waypoint pads 101, 100 and 99 ends the mission within about 40
  ticks (the screen goes black, `lvlStageLoad` zeroes the level clock) with
  every chr frozen, background ones included - an exit the level checks
  outside the chrs' scripts. The tour raises `LevelEnded` when the level clock
  goes backwards, and `viewdiff.py` runs the oracle again without that pad (up
  to six times: they come in runs of neighbouring waypoints round an exit -
  Statue 227/226/225, Archives 172/171/170, Surface 2 289/288, Bunker 2 51);
  the page lists the pads left out. Our side follows the
  oracle's pad list.
- The background chrs that run a level's own script have no prop; the freeze
  clears every chr slot with an AI list, prop or not.

- The cartridge runs 2-3 ticks a video frame where the port under lockstep
  runs one: the port's minimum hold of 8 x 3 frames overran a 40-tick slot and
  the cartridge fell 500 ticks behind ours by the sixth pad. On ares the
  minimum is 3 x 3 frames.
- `n64twin` is rebuilt by other agents: a run that starts while it is being
  relinked gets "Permission denied"; run again.

- `stanFindFloorTileBelowY` is not "the highest tile below a point": on
  Frigate's pad 133 it gave a deck 440 units under the pad's own tile. Use
  `pad->stan`. Writing x and z alone (gdbge.place) also leaves Bond's height
  where it was; on a level of decks he stayed inside the one below.
- Some waypoint pads float hundreds of units over their floor in both games
  (Frigate's 151: 833); GoldenEye puts Bond on the tile at once, our walk was
  still falling when the picture was taken.
- GoldenEye's Look Ahead latches and pulls the pitch toward the slope: 2-6
  degrees off the -5 written, on Frigate.
- The framebuffer on display can be one the parallel rasteriser is still
  filling; with the rasteriser off between pictures its unfilled part is black.
  The tour breaks at `f3dDumpFrameToDir` (called once a task's bands are
  harvested) and writes `g_colorImage`.
- GoldenEye keeps stale chr slots (chrnum 1, a dangling prop) after its real
  chrs; reading their positions faults.
- Look vectors saved to 2 decimals are worth a degree; they are saved to 4.

- GoldenEye's software rasteriser costs 200-300 ms a frame under gdb, against
  10-60 ms without: a 140-shot tour took 30 minutes with it always on. The
  oracle now boots with it off (twin.py's PORT_RENDER_FROM) and `tour.py`
  writes `portRenderEnabled::from` (os_vi.c) to turn it on for the three
  frames before each picture and off again while Bond is held - 3-5 minutes a
  mission. Only the RDP's drawing is skipped; the game's frame runs either way.
- Under load PORT_LOCKSTEP sometimes "invents a retrace"; the run stops being
  reproducible from there. The tour syncs on the level clock, not on frames,
  so it does not care.

- The port names screenshots by the second and logs them in pd.log; a
  run directory shared with another run loses both. A killed process loses
  pd.log's buffer: `tour.py` calls `fflush(0)` before `finish()`.
- `solo.padscript` presses fire four times; three shots are fired in the level
  before the tour starts and their holes are in the pictures. The tour relies
  on twin.py's default, `common/solo-quiet.padscript` (one press).
- GoldenEye reaches first person (`g_CameraMode` 4) about frame 1370 on Dam
  after the one press; a mission whose opening ignores it is put there with
  `g_IntroSwirl = 0; bondviewSetCameraMode(CAMERAMODE_SWIRL)`.
- GoldenEye's chr slots hold ten extra entries numbered 1 with a prop on the
  first frame of Dam (46 slots, 36 real chrs); freezing them is harmless.
