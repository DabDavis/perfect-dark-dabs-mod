# Findings ledger

What the instruments found when they were first run (2026-10-01), with the
cause where it was traced, so a fix session starts from the cause and not from
the symptom. **Fixes were deliberately left for a later session** (the user,
2026-10-01: the tools will expose the same flaws again). Re-run the named
instrument first: if a row has gone quiet, someone fixed it.

**Oracle:** *ares* = confirmed on the real cartridge (`--oracle ares`); *port* =
rests on the decomp's native ge007 port, which the user judges incomplete, so
probable until ares agrees; *code* = traced in both games' source. On
2026-10-01 the 20-mission world diff on ares matched the port's placement
findings key for key, so every world-diff row below is *ares*.

| # | finding | instrument that shows it | cause (where traced) | basis |
|---|---|---|---|---|
| 1 | Guards GoldenEye makes at load are missing in ours: Facility 51, Frigate 26, Caverns 10 and 33, Aztec 9 (and the guns/hats they carry); our log says `N refused for something in the way at the pad` on six missions (10 refusals) | `world/sweep.py`: chr.missing, obj.exists, "Our log while loading" | GoldenEye's `expand_09_characters()` (chraction.c:351) places a guard with `getposstan(&pad->pos, pad->stan, 20.0f, ...)`, which finds a clear spot near the pad; ours (`src/game/body.c` ~508) refuses the guard when `cdTestVolume()` at the pad itself collides. A converted guard should take a nearby spot the way GoldenEye does (or `SPAWNFLAG_IGNORECOLLISION` as the crude form) | code + ares |
| 2 | Bond's eye is 7-9 units lower than GoldenEye's on all 20 missions | `world/sweep.py` player.eye; `view/` (most of its top-ranked pairs) | GoldenEye: `eyeheight = headpos.y * player_perspective_height + 7` (bondview2.c:4629, the animated head); ours a fixed `vv_eyeheight` from the body row (player.c:2129); on ares 8.3 low on Dam, 8.8 on Facility at tick 903 with Bond in control | code + ares |
| 3 | The Python converter is no longer the C converter's twin: 55 shared files differ, 195 written only by C | `parity/parity.sh` | the Python twin lags geconvert.c (guard grenade probability since converter 93, windowed doors' glass distances, Igx guns, menu banks...). Decide: bring it back to parity, or retire it and move the census onto the C converter | code |
| 4 | ROM data both converters drop: character DLCOLLISION vertices (GoldenEye paints blood stains on them, chr.c:3151), SHADOW nodes on 42 characters, Archives arena DOOR_SCALE 0.75 (prop.c:1611), Bunker II arena monitors' mount (OwnerOffset/OwnerPart/ImageNum - the solo path handles it, geconvert.c:5477), arena weapon pickups' extrascale, group BoundingVolumeRadius | `census/census.py` (report.md) | see census/report.md for each field's GoldenEye use | code |
| 5 | Animated HD pictures with no frame table do not decode: the Complex arena's orange glow quad, the helicopter's rotor-blur disc | `xbla/census.py` | `beanDecodeTexture()` follows a frame-table pointer of 0 and reads the header's `'text'` magic as an offset; the frames are stored back to back in `.gpu`, frame 0 at offset 0 | code |
| 6 | One 4-triangle Dam snow draw (position-only vertex buffer) is never built in HD | `xbla/census.py` | in the report | code |
| 7 | Clip guns refill at 16 ticks into a reload, GoldenEye at ~34 (total reload time matches) | `guns/sweep.py` | GoldenEye refills at the start of RELOAD_RAISE (gunfire.c:3743) | port |
| 8 | Raise after a weapon swap ~40 ticks, GoldenEye ~26 | `guns/sweep.py` | not traced | port |
| 9 | Grenade: press to throw 49 ticks (GoldenEye 14); throw to bang 193 (GoldenEye 235 = 240 minus held); throw sound is our 1473, GoldenEye's GRENADE_THROW | `guns/sweep.py` | not traced beyond the fuse rule | port |
| 10 | Mines: GoldenEye places one after another while Z is held, ours throws one on release; no ATTACH_MINE stick sound; prox/remote draw time 5 vs 21; a prox mine beside the thrower blows at 302 ticks in GoldenEye only | `guns/sweep.py` | not traced | port |
| 11 | Automatic shotgun ejects a casing per shot; GoldenEye none | `guns/sweep.py` | `autoshot_stats` has no cartridge model | code + port |
| 12 | Cougar and Golden Gun do not dry-click when held empty | `guns/sweep.py` | not traced | port |
| 13 | Grenade launcher / rocket rounds land far sooner (Dam 3/12 vs 25/49 ticks, Facility 5/3 vs 12/20) | `guns/sweep.py` | spawn point or speed - not traced | port, probable |
| 14 | Bullet hits on Dam's floor sound HIT_BULLET_DIRT, GoldenEye HIT_BULLET_STONE; Moonraker leaves no impact mark; the hunting knife's slash has no whoosh | `guns/sweep.py` | not traced | port, probable |
| 15 | Frigate's sea: the real cartridge's water is blue, as ours is - the native port's green was its own fault. Left: the ROM's is a lighter teal with more visible waves than our darker, flatter navy | `ares/padshot.py` at pad 151 | settled on ares; the shade/wave difference not traced | ares |
| 16 | Archives prop 152 (model 18) sits 96 units higher in ours; Frigate armour 131 (model 115) 82 units higher (GoldenEye puts it 70 below its pad, ours 12 above). Both carry object flag 0x1 (the decomp's notes: "embedded crate, chain of boxes") | `world/sweep.py` obj.pos | not traced | ares |
| 17 | Every GoldenEye door has zero health words, every one of ours 1000 (433 doors) | `world/sweep.py` obj.health | whether a converted door can be destroyed where GoldenEye's cannot is not checked | ares |
| 18 | ~300 objects where GoldenEye's prop->pos sits away from its runtime_pos (glass, weapons, some props; ours has one position, at runtime_pos); room membership differs on ~300 objects (mostly one room more or fewer) | `world/sweep.py` obj.refpos, obj.rooms | GoldenEye registers rooms by prop->pos and builds collision from runtime_pos (propobj.c:952, the decomp port's own note); which one it draws from is not settled - a picture on ares (`ares/padshot.py`) decides | ares |
| 19 | Pads moved: Surface pad 288 and Surface 2 pad 279 by 72.6 units, Archives' bound pads by 2-4, Frigate's by ~1; eight pads in a different room | `world/sweep.py` pad.pos, pad.room | not traced | ares |
| 20 | Aztec doors 222-229 turned differently (0.4 off in the matrix) | `world/sweep.py` obj.rot | not traced | ares |

Behaviour rows (chr positions, ratings, AI lists after five seconds) are in each
sweep's "after the scripts have run" sections and need the AI trace diff to say
which are faults; Cradle's guards' accuracy/speed are assigned by a random pick
in their list (50/60/80), not a conversion fault as far as the world diff can tell.

## Harness facts that stay true

- The GoldenEye side of twin.py runs on ares (`--oracle ares`, the sweep's default;
  87 s for twenty missions against ~7 min on the port). The native port stays for
  its debug info and gdb probes; where it disagreed with ares it was the port
  (Bond's prop left at spawn values, no on-screen flag with rendering off, the sea).
- The gun rows (7-14) were measured on the port; guns/ drives gdb breakpoints and
  has not been moved to ares yet - re-measure there before fixing.
- A sweep's report is only as good as its null: if a report says NULL FAILED,
  the tool is wrong, not the game.
