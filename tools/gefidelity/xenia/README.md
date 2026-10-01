# xenia/ - GoldenEye XBLA ("Bean") as an oracle, on Xenia

Bean in the self-built xenia-canary (`/home/sdg/perfect-dark/xenia-canary`,
binary `build/bin/Linux/Release/xenia_canary`) on a private Xvfb, driven by a
virtual Xbox 360 pad and read/written through the **twin channel**: a small
hook at each frame swap that answers line commands on two FIFOs. The HD look is
judged against it the way the N64 look is judged against ares.

**One rig on this machine at a time, under a lock.** `rig.sh start` takes an
exclusive `flock` on `~/wt/gefidelity-run/xenia.lock` (who holds it:
`xenia.lock.who` beside it) and holds it for the rig's whole life; a second
start waits (up to `GF_XENIA_LOCK_WAIT` s, default 3600) and says who has it.
`rig.sh stop` kills Xenia, the pad and Xvfb, then releases the lock. Not just
one Xenia: the virtual pad is a `/dev/uinput` device, global to the machine,
and Xenia's `--hid=sdl` reads every pad there is - two rigs (even with one
Xenia) put each other's presses into the wrong game. So **pad.py and Xenia are
only ever started by `rig.sh start`**; anything else that runs Xenia wraps
itself in `flock -x ~/wt/gefidelity-run/xenia.lock`. (2026-10-01: a capture rig
and this one overlapped on Dam before the lock; the dumps from that window were
redone.)

| file | contract |
|---|---|
| `rig.sh start/stop/shot PATH/pad CMD` | Xvfb on **:118** (`GF_XENIA_DISPLAY`), `pad.py` on `$ST/pad.fifo`, Xenia with `XE_TWIN_DIR=$ST`; state, logs and FIFOs in `$ST` = `~/wt/gefidelity-run/xenia-out/rig` (`GF_XENIA_STATE`). `stop` kill -9s only the PIDs it started (Xenia ignores SIGTERM). `shot` is the game area, 1280x695 under Xenia's 25 px menu bar |
| `pad.py` | the virtual pad (copy of `.xbla-work/gunturn/pad.py`): `press A`, `stick LY -32000 0.12`, ... Bean's menus take the left stick in 0.12 s pulses; the d-pad is ignored |
| `xenia-twin.patch` | the hook, against xenia-canary d9bf601e1 **on top of** `tools/xblaintro/xenia-drawlog.patch` (both live uncommitted in the xenia-canary tree; the binary has both) |

## The twin channel (XE_TWIN_DIR with FIFOs `cmd` and `ans`)

One line in, one line out, served at each swap on the GPU thread:

    frame               ok N            swaps since boot
    peek ADDR LEN       ok HEX          guest virtual memory, big-endian as the guest sees it
    poke ADDR HEX       ok
    find ADDR LEN HEX   ok A1 A2 ...    up to 16 matches over readable pages
    dump ADDR LEN PATH  ok              raw bytes to a file (unreadable pages as zeros)
    pause               at N            the GPU thread waits at this swap (at once if already waiting)
    frames N            at M            resume, wait again N swaps later
    run                 ok              free running (the start state)

An unmapped page answers `err unmapped` instead of faulting. While paused
nothing new is presented, so `rig.sh shot` photographs that swap.

## Which build: the Community Edition by default

GE Plus's XBLA look uses the GoldenEye XBLA Community Edition, so the oracle
runs the **CE-patched release** unless told otherwise: `GF_BEAN=ce` (default)
runs `.xbla-work/ge-bean/BeanCE/defaultCE.xex` (the release with
CommunityEditionUpdaterV6 applied - xex.diff through hpatchz, filesCE, musCE.xwb;
made once from `.xbla-work/ce/`, see CLAUDE-notes/ge-bean.md, "The Community
Edition's fixes in the regular build"; read-only here), `GF_BEAN=retail` the
release as it shipped (`.xbla-work/ge-bean/Bean/default.xex`). The guest
addresses below hold for both (checked on Dam 2026-10-01: same frame counter,
players[0], g_CurrentSetup, propDefs, pads); CE's Bond spawns on the N64's
exact spot, retail's 0.1 unit off. **Our side in the HD look runs with
`Mod.GeXblaCommunityEdition=1` and `added-content/CommunityEditionUpdaterV6.zip`**
(`xenia-out/hd-run`'s `hd-pd.ini` and added-content), as a GE Plus player has it.

## Reaching a solo mission (this profile has all 20 open)

`xeniage.boot()`: each folder page opens its crosshair at its own spot (title
(0, 0), main folder (176, 131), mission grid (73, 62), difficulty page (106,
276)), so every press is repeated until the next page's spot is there - a
press during the gun barrel or a page turn is lost. **The mission and the
difficulty are not chosen with the stick**: the crosshair glides (a 0.12 s
pulse moves it about half a 70-unit cell), so pulses landed between cells
(Surface came out Facility, Streets Frigate). The crosshair is written at
`0x8272B37C` (x, y as f32 - the recomp's GE_MENU_XY) to the cell's centre,
mission N at (73 + 70 (N mod 5), 62 + 70 (N div 5)), difficulty d at (106,
200 + 30 d), and the folder's own selection at `0x82F60AF4` must read N (or d)
before A is pressed. After the load, pad 0 of the level that came up is checked
against GoldenEye's for that mission (`PAD0`), so a wrong level fails loudly.
Checked on the CE build: Surface, Streets, Statue, Bunker 2, Egyptian at 00 Agent.

**Use a state directory of your own** (`GF_XENIA_STATE`) for anything run by
hand: two users of one state directory drive each other's game (the second's
`rig.sh start` is refused, but its pad and channel reach the first's rig).
`run_scenario.py` gives each run `OUT/bean/rig`.

## The twin API on Bean (common/xeniage.py) and the tools

| file | contract |
|---|---|
| `../common/xeniage.py` | gdbge.py's API on Bean: `boot()` walks the folder screens (tick 1 = the level's first frame, caught by the setup's pads pointer; the opening is dismissed at tick 190 as the oracle's quiet pad script does; Look Ahead off), `until_tick()`/`frames()` leave the game waiting at a swap, `pads()`, `props()` (Bean's own record sizes learned per type, each checked five records ahead), `chrs()`, `player()`, `world()`; `place()`/`hold()`/`hold_pad(n, theta, verta)` put Bond on a pad's own floor tile with a heading and pitch; `shot()` photographs the waiting frame |
| `run_scenario.py SCRIPT --mission M --out OUT` | a world/ scenario (dump.py) against Bean, xeniage standing in for gdbge; starts and stops the rig; OUT/bean |
| `beandiff.py BEAN N64 [--ours PD]` | Bean aligned to the N64 oracle's world record by record (type, model, pad): Bean's own changes as `bean.*`; with `--ours`, ours against Bean in world/worlddiff.py's kinds keyed by the N64 record index. Null: Bean against itself clean, a planted drop and move found exactly |
| `hdsweep.py --out DIR [--bin] [--missions] [--reuse-oracle DIR] [--null-repeat]` | the HD world diff over the missions in world/sweep.py's finding format (`report.json`; `report.md` lists ours-against-Bean and Bean's own changes apart). Ours runs in its own run directory under `--rundir` (default `xenia-out/hd-run`, the XBLA agent's pd.ini as `hd-pd.ini`). `--reuse-oracle` takes Bean's dumps from an earlier DIR so a gate's two sweeps run Xenia once; `--null-repeat` dumps ours twice on the first mission and requires the same placement findings. ~75 s a mission for Bean, ~10 s for ours |

| `pair.py --mission M --pad N --theta T [--verta V] --out DIR` | one matched picture pair, Bean (CE) against our HD look with the CE on, Bond on the pad's own tile; scored like the view diff at 320x180, with Bean's own two shots of the spot as the null (must score under 0.05) and a poke read back. Dam pad 0 heading 90, 2026-10-01: pair 0.18, null 0.013 - the same view; ours lights the tunnel mouth brighter |
| `pair_scene.py` | the pair's scenario, one file for both sides |

**`place()`/`hold()` set x and z, the tiles, heading and pitch - not the height.**
Bean re-grounds Bond only when he walks, so a pad on another floor keeps the
eye at the old height; for the view diff's tour use `view/xenia_side.py`'s
`stand()`, which takes the floor from the tile. On Dam pad 0 (same floor as the
spawn) the two eyes agree to 0.4 units.

**Pictures** (for the view diff): Xenia presents Bean at 1280x720 in a
1280x720 Xvfb screen; the game area is **1280x695 at y=25** (Xenia's ImGui
menu bar covers the top 25 px), which `rig.sh shot` crops. Take the shot while
the game waits at a swap (after `hold()`/`frames()`), or the frame may be mid
update. Bean's in-level field of view is not located yet (the N64's is 60
degrees vertical at 4:3). Measured by `pair.py`: ours at 1280x720 with FovY 60
(vertical) lines up with Bean's frame wall for wall, so Bean's in-level
vertical field of view is 60 degrees at 16:9 too.

## Guest addresses (GoldenEye_Nov2007_Release; from GoldenEye-XBLA-Recomp's ge_hooks.cpp, checked here)

- `0x8308851C` frame counter (60 a second)
- `0x82F1FA98` players[0] pointer (Bond). Bean's player struct is NOT the N64
  layout: vv_theta at +0x254, vv_verta at +0x264; +0x150 is NOT Bond's position (the view agent found it) - Bean keeps Bond's x/z at +0x4e0/+0x4e8 (x10), +0x4ec/+0x4f4 and +0x530/+0x538, and view/xenia_side.py writes all three pairs to place him
  (N64: vv_theta at 0x148). Found by matching the N64 oracle's numbers on Dam -
  Bean keeps GoldenEye's world coordinates exactly.
- `0x82F303A0` GoldenEye's `g_CurrentSetup`: the N64's ten pointers in the
  N64's order (waypoints, groups, intro, propDefs, paths, AI lists, pads,
  bound pads, pad names, bound pad names). Found from Dam's pad 0 position.
- Setup records keep the N64 header and ObjectRecord layout (model +4, pad +6,
  flags +8, prop +0x10, model pointer +0x14, mtx +0x18, runtime_pos +0x58,
  health words +0x70/+0x74, door maxFrac 132 / slide 168 / openPosition 180)
  but **4J changed some sizes**: weapon +1 word, door -1, vehicle +15 (more
  may turn up on other missions; `xeniage.walk()` learns them).
- Bean's `PropRecord` has an extra word: object pointer +0x08, pos +0x0C,
  tile +0x18, parent +0x20, prev/next +0x28/+0x2C, rooms +0x30 (bytes, 0xFF ends).
- `ChrRecord` is the N64 layout as far as read (chrnum 0, body 15, chrflags
  20, prop 24, model 28, visionrange 208, hearingscale 236, damage 252,
  maxdamage 256, ailist 260, aioffset 264, weapons_held 352); head numbers are
  Bean's own. Model scale is at model +20 as on the N64.
- players[0]: prop at +0x1AC, vv_theta +0x254, vv_verta +0x264, GoldenEye's
  collision434 at **+0x5B0** (tile +0, collision position +4, ground +0x1C,
  radius +0x28, eye +0x2C, portal tile +0x50 - the N64 layout inside it).
- A StandTile's room is the **low byte of its first word** (byte 3 on the
  console); the port's -m32 DWARF shows it first, which is the little-endian view.
- Settings at `*0x83088228 + 0x298`: 0x80 Look Ahead, 0x10 auto aim.
- AI list ids: the setup's table (slot 5), else GoldenEye's global table in
  the xex (the pointer is followed by its id).

## Changes to xenia-canary

`xenia-twin.patch` (this directory) on top of `tools/xblaintro/xenia-drawlog.patch`:
the hook in `src/xenia/gpu/pm4_command_processor_implement.h` (called at
PM4_XE_SWAP beside the draw log's swap) and three POSIX includes at the top of
`command_processor.cc` and `vulkan/vulkan_command_processor.cc` (the hook header
is read inside their namespaces). Off unless `XE_TWIN_DIR` is set. Rebuild:
`VULKAN_SDK=~/vulkan-sdk/1.4.357.1/x86_64 cmake --build build --config Release
--target xenia-app -j2` - under the lock (`flock -x ~/wt/gefidelity-run/xenia.lock`),
since the binary is shared.
