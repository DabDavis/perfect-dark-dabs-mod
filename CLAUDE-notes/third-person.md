# The third person camera, and everything that measures from the camera

The fork's playable third person is one function, `playerPullBackCamera()` in
`src/game/player.c`, called from the normal tick's camera block. It moves the
copy of the eye that the camera matrix is built from and leaves `bond2`'s own
position and basis vectors alone, so the walk, the aim and the room search all
carry on as if nothing had moved.

What is not obvious from that function is how much of the rest of the game is
written against the camera rather than against the player, and that stock never
had to tell the two apart because the camera *was* the eye.

## The player's shot comes out of the camera

`bgunCalculatePlayerShotSpread()` (bondgun.c) builds the shot as

```c
gunpos2d = {0, 0, 0};                    // the origin, in camera space
cam0f0b4c3c(player->crosspos, gundir2d, 1);   // through the crosshair pixel
```

and the callers turn it into world space with `camGetProjectionMtxF()`, which is
`mtxf0068` - built by `mtx00016b58()` from `cam_pos`, so its translation is
wherever the camera is standing. The shot therefore starts **at the camera** and
points through the pixel the crosshair is drawn on.

That is why the pull-back never needed a reprojection pass, and it is worth
saying plainly because the constants.h comment used to claim the opposite: the
crosshair marks what will be hit from *any* camera offset, sideways included,
because the crosshair and the shot are two readings of the same ray. The only
thing an offset changes about a bullet is where it starts, which is why leaning
the camera past a corner shoots past the corner.

It is also why three things were broken in third person for as long as the mode
has existed, and none of them were the bullet:

| what | why | where |
| --- | --- | --- |
| melee hit nothing, even touching | the reach is measured from the camera | `handInflictMeleeDamage()`, prop.c |
| the laser stream was invisible | its 300 units ended before reaching the player | `shotCalculateHits()`, prop.c |
| rockets and grenades came from behind | they spawn at `hand->muzzlepos` | `bgunCreateFiredProjectile()`, bondgun.c |

The fix is in two halves, both keyed on `playerGetShotOriginPullback()`
(player.c), which is the camera's distance from the eye and **zero for every
camera that is on the eye** - it also returns zero outside `CAMERAMODE_DEFAULT`,
because `thirdpersondist` is only written by the normal tick and a cutscene or
an eyespy entered from third person would otherwise carry the last value it had.

**It is signed, and the sign is not in `thirdpersondist`.** Camera Forward/Back
can bring the camera round in front of the eye, and then every consumer wants
the correction the other way about: the origin slides *back* down the ray, and
the melee reach shortens rather than lengthens. `thirdpersondist` is the length
of an offset that may now point anywhere, so it cannot carry that; the accessor
reads the side off `(eye - campos) . look` instead and negates. Anything new
that takes the pull-back has to add it signed, the way bondgun.c and prop.c do.
`playerGetCameraToEyeOffset()` deliberately does *not* go through the accessor
for its "is there anything to correct" test - a muzzle in front of the player
wants putting back at the hands just as much as one behind them.

1. **The shot's origin slides up its own ray** by that distance, in
   `bgunCalculatePlayerShotSpread()`. The ray does not move, so no bullet
   changes and the crosshair stays honest; the origin lands at the player, which
   is what the melee reach, the stream length and the projectile spawn were all
   asking for.
2. **`hand->muzzlepos` gets the camera-to-eye vector added** at the end of
   `bgun0f0a5550()`, via `playerGetCameraToEyeOffset()`. Every branch there puts
   the muzzle where the *view model's* muzzle is - `posmtx` is `cammtx` through
   the camera matrix, and the two node branches transform by it directly - so in
   third person it lands beside the camera even though the view model itself is
   not drawn. The exact eye rather than the ray, because a muzzle is a point:
   with a sideways offset the beam should leave the gun, not the air beside it.

**Melee does not go through `shotCalculateHits()`.** `handInflictMeleeDamage()`
walks `g_Vars.onscreenprops` itself and gates on `prop->z` (the distance in
front of the camera) and on `func0f0679ac()`, which returns the model's bounding
box in camera space - `min` the far face, `max` the near one, in front being
negative. All three of those are distances from the camera and all three take
the pull-back. Moving the shot's origin does nothing for melee; this is a
separate site.

## The offsets, and what each one is along

| setting | axis | what moves it |
| --- | --- | --- |
| Camera Distance | `-look`, the look vector itself | pitching the view: looking up walks the camera down towards the floor, looking down lifts it |
| Camera Sideways | `look x up`, the right hand | nothing; it is the shoulder the picture is taken over |
| Camera Forward/Back | the facing, flattened level | nothing; it holds its height at every pitch |
| Camera Height | world Y, straight up | nothing; it is not the camera's own up vector |

They add into one `offset` and the trace scales all of it together, so a wall
brings them in as a set and the shoulder is kept.

Forward and back takes its direction out of the **right vector**, not out of the
look vector: `(right.z, -right.x)` is the right hand turned a quarter turn back
onto the facing, and it is exact with the view straight up or straight down,
where flattening the look vector leaves nothing to normalise. Positive is
further back, so it reads the same way round as Camera Distance; negative is
what puts the camera in front of the player, and that is the case the signed
pull-back above exists for.

**Camera Preset is derived, not stored.** `g_ModCamPresets` in optionsmenu.c
names combinations of the four and writes all four on a pick; the row's own
answer comes from comparing the live values against the table, so any slider
move drops it back to Custom without anything having to reset it, and Custom
itself is never applied. Nothing is registered in `pd.ini` for it - the offsets
are already there, and a preset that was also a saved setting would be a second
opinion about the same four numbers. A preset's values have to be reachable by
the sliders, or picking it would strand the row in Custom for good.

**Height has a usable range, and it is shorter than the slider.** Nothing tilts
the view to keep the player in frame - the view direction is the aim, and a
camera that aimed somewhere other than the crosshair would be a different bug
- so a raised camera looks level over the player's head and the player slides
down the screen. The player leaves the bottom of it at about
`atan(camheight / camdist)` past half the vertical FOV, which is around 100 up
at the default 200 back. Further up than that is for a view that is also
pitched down, and the slider goes to 150 because a longer distance earns it.

## Camera Tether

`Mod.ThirdPersonTether` (Off/Loose/Normal/Tight, `g_ModOptions.camtether`,
`MODTETHER_*`) is `playerTetherCamera()`, called from `playerPullBackCamera()`
after the four offsets are summed and before the trace. It keeps the offset's
length and height and replaces its horizontal bearing with a rod that pivots
about the eye: this frame's bearing is where the rod's far end stood last
frame, seen from where the eye is now (`thirdpersontetherpos`, stored
**untraced** - a wall brings the camera in along the rod, not the rod in), then
clamped to the setting's angle either side of the rest bearing and eased back
towards it by the setting's rate (`g_ThirdPersonTethers[]`). The rest bearing
is the rigid offset's own, so a shoulder preset still rests over that shoulder.
`thirdpersontethered` says the stored end is worth reading; it is cleared on
every frame that is not third person, so aiming and the first frame of the
mode both start the rod behind the aim.

**The body has a facing of its own under the tether**, or the right stick turns
it with the camera and the whole thing reads as the rigid camera with a lag -
which is what the first cut was, and what was reported. `playerTetherBody()`
in the body tick keeps `thirdpersonbodytheta` (the radians `chrSetLookAngle()`
takes, world): it turns towards the direction of travel while the left stick
moves the body (the same `look - atan2f(sideways, forwards)` the animation
chooser turns a strafe part way towards, taken the whole way), holds while the
body stands, and faces the camera while either hand's `triggeron` or `firing`
is set and for `TETHER_FIRE_HOLD` ticks after, because the shot is fired from
the camera. The turn rate is Body Turn Speed (`Mod.ThirdPersonTurnSpeed`,
`camturnspeed`, degrees per tick, 30 by default; the first cut's fixed eleven
was reported too slow). The speeds handed to the chooser are re-read relative to the body
so it plays the forward run rather than a strafe, `speedtheta` is zeroed
(the look turning is the camera orbiting, not the body), and the chooser's
own `angleoffset` is applied on top of the body's facing rather than the
look's. The flag `thirdpersonbodyset` is cleared with the tether's, so coming
back from aiming starts the body facing the aim, where first person left it.
Movement needed nothing: the walk is along `vv_theta`, which is now the
camera's yaw, so the left stick is already screen-relative.

Two things that cost a run each:

- **The rod pivots about `bond2.unk10`, not the `campos` handed in.** That copy
  already has the damage shake and the tilt's bob added, and a rod pivoting
  about it reads every shake as the player moving: a simulant's hits sent the
  camera thirty degrees round. The shake still moves the camera, because the
  offset is added to `campos` as before; it just does not turn the rod.
- **The game's `atan2f()` answers in 0 to tau, never negative** (`atan2f.c`,
  `M_TAU - result` for `x < 0`). A rod a hair to the left of rest read as
  nearly a full turn and the clamp landed it on the *far* cap, which showed as
  the camera sitting a few degrees off rest for ever and jumping 45 degrees
  round every few seconds. Wrap to signed first.

The vertical is deliberately not tethered: nothing tilts the view to follow
the body, so a rod free to pivot vertically drops the body out of the frame on
every ledge. And the lag is capped for the same reason the height range is
short - the view looks where the crosshair is, not at the body, and at the
default distance the body leaves a 60 degree field of view about 30 degrees
off the rod.

To test it from gdb, set `g_ModOptions.camtether = 2` alongside `thirdperson`,
turn the player with `vv_theta` and read `thirdpersontetherpos - bond2.unk10`:
with `g_ThirdPersonTethers[2].rate` set to 0 (gdb writes to rodata) the rod
holds exactly the cap's angle on the side it was turned away from, and with the
rate restored it is back at rest before an attach-and-print can catch it. The
attach alone takes over half a second, so "sampled 0.1s after the turn" is not.

## The camera trace

One `cdExamLos08()` from the eye to where the camera wants to be, and the whole
offset is scaled by what fits. Scaling rather than shortening the distance is
what keeps a sideways offset's shoulder while a wall comes up - the camera
slides in along the line it was on instead of swinging back behind the player.

**The trace needs the floor flags.** With `GEOFLAG_WALL | GEOFLAG_BLOCK_SIGHT`
alone, looking straight up puts the offset into the ground behind the player's
heels and the camera goes under the level. `GEOFLAG_FLOOR1 | GEOFLAG_FLOOR2` is
the pair the rest of the game means by a floor and carries ceilings with it -
`cdFindClosestVertical()` tells the two apart by which way they face, not by the
flag - so the same two also stop a low ceiling when looking down.
`GEOFLAG_LIFTFLOOR` goes with them, the way propobj.c asks for them.

**The line is not enough on its own.** It has no width and takes its clearance
along itself, so a wall running beside it - the player walking along one, or
the tether swinging the camera round beside one - never registers and the near
plane sits inside the brickwork, which is the clipping that was reported.
`playerClearCamera()` runs after the line: `cdExamCylMove02()` with Camera
Wall Clearance as the radius, at the camera, and when it is inside a wall
`cdGetEdge()` names the edge, so the camera is pushed out along that edge's
normal (turned to the eye's side) until it is the radius clear - sideways,
off the wall, not back towards the player, or every corridor would drop to
first person. Three passes for corners. Then `cdFindGroundAtCyl()` and
`cdFindCeilingRoomYColourFlagsAtPos()` lift it `CAMERA_VCLEAR` off a floor
and below a ceiling, because a line at a shallow angle to the floor is a
hand's breadth above it after its thirty units. Any push leaves the line the
eye was traced along, so it is traced again and clamped as before, and if a
half-radius `cdTestVolume()` still fails (a corridor narrower than twice the
radius has no clear spot) the camera comes in along the line half a radius at
a time. The rooms for every test come from `func0f065dfc()` from the eye's
rooms to the camera, the way the eyespy finds its own. The distance the ease
and the HUD read is taken from wherever the camera ended up, since it is no
longer on the offset's line.

Coming in is immediate, going back out is eased (`THIRDPERSON_EASE_RATE`). They
are not the same event: a wall arriving is this frame's problem or the camera
draws the inside of it, while a wall leaving is only space becoming free again.
The eye is outside the easing - below `cammindist` there is no view to ease
towards, so that one cuts in both directions.

## Testing it headlessly

The camera can be driven entirely from gdb; no input is needed.

```sh
SDL_VIDEODRIVER=offscreen ./pd.x86_64 --savedir SCRATCH --skip-intro \
    --no-sound --boot-stage 0x32 --mpsims 1 &
gdb -batch -p $(pgrep -x pd.x86_64) \
    -ex 'set g_Vars.players[0]->thirdperson = 1' \
    -ex 'set g_Vars.players[0]->invincible = 1' \
    -ex 'set g_ModOptions.camside = 120' -ex detach
gdb -batch -p $(pgrep -x pd.x86_64) \
    -ex 'print g_Vars.players[0]->thirdpersondist' \
    -ex 'print g_Vars.players[0]->bond2.unk10' \
    -ex 'print g_Vars.players[0]->thirdpersoncampos' \
    -ex 'print g_Vars.players[0]->hands[0].muzzlepos' \
    -ex 'call (void)screenshotRequest()' -ex detach
```

`thirdpersondist` should read `sqrt(camdist^2 + camside^2)` with the view level
and no wall in the way, and `camdist + camfwd` with only the forward offset set;
what says the forward offset is *level* is that `(campos - eye).y` does not
change when it moves, at any pitch. `muzzlepos`
should sit about 45 units from `bond2.unk10` - the same distance it does in
first person - rather than 200 units away next to `thirdpersoncampos`. That one
number is the whole muzzle fix.

Two things that waste a run:

- **A spawn against a wall reads as a broken camera.** `thirdpersondist` comes
  back 0 because the trace clamped below `cammindist`, which is correct
  behaviour. `set g_Vars.players[0]->vv_theta = ... + 180` turns round and the
  distance goes to its full value.
- **A `while` loop in a `gdb -batch` script hangs the game**, and the gdb has to
  be killed before the process will run again. Print slots one at a time.

Melee, projectiles and the beams still want a real match: they need a target
walking into you, which nothing here can arrange.

## Footsteps played in bursts (2026-09-14)

"Footsteps in third person are broken, they sound very rapid." The player's
footsteps are `bmoveTick()`'s: one per 150 units walked, non-positional. In
first person nothing else plays one. In third person the body is animated
with the run cycles `g_FootstepAnims` lists, and `chraTick()` calls
`footstepCheckDefault()` on the player's chr as on any other. Its test is
"the animation frame crossed a footfall since `oldframe`", and `bondhead.c`
overwrites the player chr's `oldframe` every tick with the **head bob
model's** frame, so the test passed about twelve frames running.

Measured on Chicago (`--boot-stage 0x1d --rng-seed 1 --fixed-step`, on the
GPU): a 300-frame walk with `speedforwards` held at `bwalkTick()`'s entry
(scratchpad `footcount.py`, counting `footstepChooseSound()` calls with
`chr->footstep` set, grouped by caller) played 7 player footsteps in first
person, all `bmoveTick`, and 7 plus 68 in third person, the 68 in runs of 12
on consecutive frames. `footstepCheckDefault()` now leaves out
`PROPTYPE_PLAYER` chrs, which is the first person rhythm: third person plays
the same 7 on the same frames.

## Light glares draw over the gun, and over the body in third person (2026-09-09)

A glare (the corona sprite round a light, `bgRenderArtifacts()`) is a
depth-less screen rectangle. Its only occlusion is `artifactTestLos()` ->
`shotTestLos()` (prop.c), which walks the world and the on-screen props and
**leaves the current player's own prop out**. The view model is not a prop at
all. So a light behind the gun drew its glare on top of the gun, and in third
person a light behind Joanna drew on top of her.

Two halves, both port-only. `playerRenderHud()` draws the glares *before*
`bgunRender()`: the gun goes into a freshly cleared depth buffer and is
opaque, so it paints over them, which is the depth test the rectangle cannot
have (Murk's fix, `perfect_dark_netplay/docs/PORT_GLARE_OCCLUSION.md`; his
tree stops there because it has no third person). For our third person the
gun is not drawn, so `shotTestLos()` includes the player's own prop when
`thirdpersondist > 0` and `chrTestHit()` against the body decides it. The
glares are `shotTestLos()`'s only caller, so nothing else changes hands.
Neither half is verified in a picture yet: the headless stages boot into a
cutscene and a glare wants a light behind the gun, so a person confirms it.

## Glare Clipping: halos stop at nearer walls and models (2026-09-14)

The user reported halos "shining through walls, and models", in first person
as well as third. `shotTestLos()` is not wrong in general, but it is not
enough:

- **A halo is a flat rectangle.** Once any sample point of a light passes, the
  whole sprite draws, over whatever stands in front of the rest of it (the
  first F3 shot, Defection's lift shaft: the light just past a wall's edge,
  its halo over the wall).
- **The check misses geometry.** At that Defection spot four lights passed
  4 of 4 sample points while the depth buffer at their pixels read a surface
  about 90 units away and the lights were 800-1000 away. Probably the lift car,
  a prop whose hit boxes can hold the camera; not chased further.
- **The check is a frame or two old.** Artifacts are computed into the write
  list and drawn from the front list, and the draw recomputes the screen
  position. The Chicago F3 shot (a halo in the middle of a wall) was not there
  at all when the exact camera was restored headlessly: light 15 in room 38 is
  seen through the alley gap to the right a frame earlier, so a turn draws it
  over the wall for a frame or two.

**Glare Clipping** (`Mod.GlareClip`, `g_ModOptions.glareclip`, Video page
under Glare Brightness; off by default, on in Dab's Settings, off in Vanilla
and Ghost Trials) depth-tests the halo at draw time, which answers all three:

- `artifactsCalculateGlaresForRoom()` projects each sample point pulled 80% of
  the way from the camera (`GLARE_CLIP_PULL`) through the same `spf8` the
  point itself goes through, into the port-only `artifact.clipz` (normalised
  depth). Pulled, or the wall or ceiling a light is mounted on, receding round
  it, would cut into its own halo; the rooftop's ground lights still lose the
  part of the halo lying across the floor.
- `artifactsRenderGlaresForRoom()` takes the nearest `clipz` of the light's
  sub-artifacts and sends `gDPSetRectDepthEXT(on, z)` (`G_SETRECTDEPTH_EXT`,
  0x48, z as a fraction of 2^30) before the halo rectangles and off after.
- `gfx_draw_rectangle()` then puts the rectangle at that z instead of -1 and
  turns on `G_ZBUFFER` with `Z_CMP`, no `Z_UPD`, `ZMODE_OPA` (`GL_LESS`) for it.
  Glares are drawn before `bgunRender()` clears the depth buffer, so the world
  and props are in it.

Checked on the RX 580 offscreen:

- **The depths are right.** A gdb stop in `gfx_draw_rectangle()` reading the
  depth buffer with `glad_glReadPixels` at each glare's pixel (scratchpad
  `depthprobe2.py`): a light in view sits at the surface's depth, and every
  light the Defection lift-shaft view lost was behind a surface in the buffer.
  An earlier reading that took `glReadPixels`' bottom rows for the top of the
  view looked like a 4x depth mismatch and sent a detour through `mtxF2L()`'s
  stage scale; Defection's scale is 1 and there is no mismatch.
- **Visible glares stay.** Defection's rooftop in third person, eight
  headings: identical glare counts and visible counts off and on, at most 5.5%
  of pixels changed (the halo tails across the floor), no pixel brighter.

Restoring an F3 camera headlessly: under `--spectate` a written `prop->pos`
is walked from the prop's rooms by `func0f065e74()` and lands at the same
wrong spot every run (both F3 cameras here); without `--spectate`, holding
`prop->pos`, `prop->rooms` and the angles every frame puts the eye exactly
where the trace says (scratchpad `glarespot.py`).

## Glares and the sun seen through walls: the GPU reads the depth (2026-09-26)

Four F3s from one tester (Chicago's "puddle reflecting a light that doesn't
exist", Air Base "light showing through wall", Crash Site "sun showing through
walls"), all with Glare Clipping on. Two of the "lights" were the **sun's lens
flare** (`skyRenderFlare()`), the other a light of the yard below Chicago's
street. The halo's depth never mattered: whether a glare or the sun is *seen*
came from `shotTestLos()`, and it was wrong in three ways:

- **It walks the rooms from `cam_room` alone** (`portal00018148()`). At the
  Air Base spot the eye stands in a doorway (prop rooms 108, 107; cam room
  108): the wall in the way is room 107's, the line to the sun crosses no
  portal into 107, so nothing was tested and all eight sun points passed.
- **Collision is not the picture.** Crash Site's hills have none - no room's
  batches hit the line to the sun at any length.
- **Translucent surfaces are skipped** (`g_BgHitXluDisabled`). Chicago's
  street is translucent over its reflection, so the yard's light (room 14,
  300 units under the road) came up through it; Glare Clipping then cut the
  halo along the kerb, which is the "puddle".

The N64 decided all of this from its z-buffer (`zbufSaveArtifactDepths()`,
read back in `schedUpdatePendingArtifacts()`); the port now asks the GPU the
same question:

- `artifactsTestOcclusion()` (from `bgRenderArtifacts()`, after the scene and
  before the gun clears the depth) emits `gDPOcclusionTestEXT()` for every
  artifact **written** this frame: a one-pixel rectangle at the point's own
  depth, depth tested, never written, blended to nothing, inside an
  occlusion query (`G_OCCLUSIONTEST_EXT`, `gfx_occlusion_test()` in
  gfx_pc.cpp). A light's point is tested 30 room units in front of it (at
  least 2% of the way back: `GLARE_TEST_SLACK`), so its own fitting and
  GoldenEye's swinging lamp cages do not hide it; the sun's at
  `SUN_TEST_Z`, just short of the depth buffer's clear value, so anything
  drawn over the sky hides it (no sky writes depth: sky.c, xblasky.c,
  gebeansky.c).
- The query slot is the artifact's place in the three lists
  (`list * MAX_ARTIFACTS + i`). `artifactsResolveOcclusion()` reads a list's
  answers once, when it is the front list two frames later - the delay the
  N64 had - before `skyRenderSuns()` or the glares look at `visiblelos`.
- Backends: GL `GL_SAMPLES_PASSED` (`GL_ANY_SAMPLES_PASSED` on ES), read
  with `GL_QUERY_RESULT`; Vulkan an occlusion query pool, each query reset in
  the frame's upload command buffer (never inside rendering) and begun/ended
  inside the rendering under way (`VKP_BEGIN_QUERY`/`VKP_END_QUERY`), the
  read waiting for exactly the submission it went out in - which
  `vk_begin_recording()` has already waited for two frames on, so nothing
  stalls. Because it is the frame's own depth buffer, resolution, MSAA,
  supersampling/FSR render scale and TAA need nothing of their own.
- `artifactTestLos()` stays as the fallback when a backend cannot
  (`videoHasOcclusionQueries()`).
- A light's point outside its room's portal box on screen
  (`bgGetRoomDrawSlot()`) is dropped before any test: the room is scissored to
  that box, so the point is behind whatever the portal is cut in. That is what
  hides Chicago's yard light: nothing that writes depth is under the street.

Checked on the RX 580, GL and Vulkan, 1080p and 4K, MSAA 8x, SMAA +
Supersampling 2x, SMAA + FSR Performance, TAA, the tester's `[Mod]` settings:
the four spots lose their glare/flare, and a light in plain view (the yard
from above, Chicago) and the sun over Crash Site's ridge still draw - the
latter was *hidden* before (the line test hit something on the way). GE Plus
Caverns' hanging lamps glare as before. Rig: `~/wt/f3glare-run` (`pos.sh`
teleport + hold, `matrix.sh` runs spot x build x renderer x settings and
counts glares seen and sun flares per frame with `prob.py`). Traps: under
`--spectate` every room is on screen over the whole screen (no portal boxes),
so repro without it and hold `vv_manground` where the ground lookup lands on
the wrong floor (Air Base: -491); the tester's `ThirdPersonDistance` in the
ini puts a headless run in third person, so hold `thirdperson = 0`; and the
F3 trace's `[Video]` settings are not in the report.

## The player's own tracers (2026-09-20)

"Bullet tracers only work in first person." The player's tracer is
`hand->beam`, and the one place that draws it is `bgunRender()`, inside the
loop that draws the view model - which `playerRenderHud()` leaves out whole
while `thirdpersondist > 0`. Everybody else's tracers are fireslot beams drawn
in the world pass by `propsRenderBeams()`, which skips the current player
because stock never sees them from outside.

Two halves:

- `propsRenderBeams()` draws the current player's two hand beams when
  `thirdpersondist > 0` - the same test the gun is skipped on, so a beam is
  never drawn twice.
- `beamCreateForHand()` starts the beam at `player->chrmuzzlelastpos[hand]`,
  the gun the **body** is holding, in third person. `hand->muzzlepos` is the
  view model's muzzle carried to the eye (right for a rocket's spawn, above),
  and from a camera straight behind the player a beam from there runs up the
  aim ray behind their own head and is never seen. `playerTickChrBody()`
  already keeps `chrmuzzlelastpos` every frame the body ticks and falls back
  to `muzzlepos` by itself.

Checking it headlessly: a tracer lives three or four frames and a screenshot
requested at `videoEndFrame` is the *next* frame's, so create the beam and
request the picture in the same stop (`beamCreateForHand(0)` from gdb after
writing `hitpos`), pull `beam.dist` back a few hundred, and do it down a long
corridor with Camera Sideways set - with the default camera dead behind the
player the gun, the beam and the crosshair are all behind the body, and a beam
already past the nearest wall is depth-tested away.

## A launcher fired once in third person (2026-09-25)

`hand->rocket` is the rocket in a launcher's tube (`WEAPONFLAG3_HELDROCKET`:
Perfect Dark's rocket launcher and GoldenEye's). Firing turns it into the
flying rocket and sets `firedrocket`; **only bgunRender() let go of it**, after
drawing it in the hand for one last frame. player.c skips bgunRender() in third
person (and under GoldenEye's watch), so there the hand kept the rocket it had
fired: `bgunUpdateRocketLauncher()` makes a new one only when `hand->rocket` is
NULL, so none was made; every tick `bgunUpdateHeldRocket()` posed the one in
flight at the muzzle; and the next shot "fired" it again - freed by then
(`prop` NULL), so nothing flew. Switching to first person for a frame cleared
it, which is why it hid.

`bgunUpdateRocketLauncher()` now lets go of a fired rocket itself when the gun
is not drawn, by player.c's own test (`thirdpersondist > 0 ||
geWatchHidesGun()`). First person still goes through bgunRender() exactly as
before - screenshots of every first-person run (GoldenEye's launcher in both
looks, single, a pair and one in either hand, Perfect Dark's rocket launcher
and the Slayer, which has no held rocket) are pixel-identical to the build
before. In third person, four shots in a row now each fire a rocket, in both
looks, single and akimbo (a pair, the launcher in the left hand or the right),
and switching views while loaded or while the tube is empty keeps the state.

Probe: `~/wt/f3rocket/build/run/probe/multi.py` (`runlane.sh`, `summ.py` prints
per shot which hand fired, whether a hand kept a fired rocket, and how many
rockets are in flight 8 frames later from `g_WeaponSlots`). `VIEWS="fp tp>fp"`
fires the first shot in first person, the second in third and switches to
first while the tube is empty. A pair needs `bgunEquipHands(w, w)`:
equipping the same gun twice does not pair it, and the first equip carries the
right hand's old gun to the left.

