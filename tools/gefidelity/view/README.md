# view/ - the view diff

`viewdiff.py` takes the same pictures in GoldenEye (the decomp's native port on
the oracle host) and in our N64 look, scores each pair, and writes an HTML
contact sheet sorted worst first. It finds what a tester would see first:
missing or moved geometry, wrong props, wrong sky, fog, lighting or texture
colour.

| tool | contract |
|---|---|
| `viewdiff.py run --mission M` | one mission: tour both sides, score, `OUT/M/index.html` + `scores.json` + `pairs/` |
| `viewdiff.py sweep [--missions a,b]` | every mission (oracle runs <= 4 at once, ours <= 2), plus `OUT/index.html` |
| `viewdiff.py score DIR...` | (re)score runs already on disk; exit 2 if any null fails |
| `tour.py` | the gdb scenario both sides run (through `twin.py`); writes `manifest.json` + one shot per pad and heading |

OUT defaults to `~/wt/gefidelity-run/view-out`. Ours runs from
`~/wt/gefidelity-run/view-rundir-{0,1}` (binary hard-linked, so pd.log and
`screenshots/` are private to the run; data/, added-content/, mods/ are
symlinks to the run root).

## How a pair is made the same picture

- **Same place.** Pad N and heading H mean the same in both games. GoldenEye's
  Bond goes onto the pad's own floor tile (`pad->stan`) the way its spawn
  does (`change_player_pos_to_target`); ours onto `cdFindGroundAtCyl` under
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

## Pairs that are not ranked (shown, with the reason)

- cameras more than 4 units apart across the floor or 1 degree apart in where
  they look;
- eyes more than 40 units apart in height: the two stand on different floors;
- a chr within 40 units of the camera on either side: guards are frozen on
  their setup pads, a waypoint pad can be one, and the camera is inside him
  (GoldenEye's eye sees his hat brim, ours, 8 units lower, his jacket).

## Traps met building it

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
