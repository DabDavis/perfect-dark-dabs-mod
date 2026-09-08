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
