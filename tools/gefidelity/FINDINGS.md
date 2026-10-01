# Findings ledger

What the instruments found when they were first run (2026-10-01), with the
cause where it was traced, so a fix session starts from the cause and not from
the symptom. **Fixes were deliberately left for a later session** (the user,
2026-10-01: the tools will expose the same flaws again). Re-run the named
instrument first: if a row has gone quiet, someone fixed it.

A fix session: make the fix, then
`world/gate.py --base ./pd.base --test ./pd.fix --base-tree <tree before> --test-tree <tree after> --out g`
- the row's findings should come out "fixed" and nothing "new" anywhere else;
the convdiff leg shows which converted files moved and the replay leg which
seeded runs changed (add `--neutral` for a change meant to leave gameplay alone).

**Oracle:** *ares* = confirmed on the real cartridge (`--oracle ares`); *port* =
rests on the decomp's native ge007 port, which the user judges incomplete, so
probable until ares agrees; *code* = traced in both games' source. On
2026-10-01 the 20-mission world diff on ares matched the port's placement
findings key for key, so every world-diff row below is *ares*.

| # | finding | instrument that shows it | cause (where traced) | basis |
|---|---|---|---|---|
| 1 | Guards GoldenEye makes at load are missing in ours: Facility 51, Frigate 26, Caverns 10 and 33, Aztec 9 (and the guns/hats they carry); our log says `N refused for something in the way at the pad` on six missions (10 refusals) | `world/sweep.py`: chr.missing, obj.exists, "Our log while loading" | GoldenEye's `expand_09_characters()` (chraction.c:351) places a guard with `getposstan(&pad->pos, pad->stan, 20.0f, ...)`, which finds a clear spot near the pad; ours (`src/game/body.c` ~508) refuses the guard when `cdTestVolume()` at the pad itself collides. A converted guard should take a nearby spot the way GoldenEye does (or `SPAWNFLAG_IGNORECOLLISION` as the crude form) | code + ares |
| 2 | Bond's eye is 7-9 units lower than GoldenEye's on all 20 missions | `world/sweep.py` player.eye; `view/` (most of its top-ranked pairs) | GoldenEye: `eyeheight = headpos.y * player_perspective_height + 7` (bondview2.c:4629, the animated head); ours a fixed `vv_eyeheight` from the body row (player.c:2129); on ares 8.3 low on Dam, 8.8 on Facility at tick 903 with Bond in control | code + ares |
| 3 | ~~The Python converter is no longer the C converter's twin~~ - Python twin retired, user 2026-10-01; the census reads C | `census/census.py` (C, instrumented: census/ctrack) | resolved: geconvert.c is the only converter; tools/geconvert keeps the header generators, fit/ and the modules fit/ and ai/aimap.py import | done |
| 4 | ROM data geconvert.c drops that GoldenEye on the cartridge reads (census/ares read pass, 20 missions, JIT slow path): characters' SHADOW nodes (doshadow reads size/pos/image on 13-24 of the dropped models - GoldenEye's blob shadow); arena COLLECTABLE extrascale/damage/flags/timer and AMMO-crate extrascale/damage/flags/slot models (modelLoad, weaponAssignToHome, chrobjWeaponTick read them in the solo setups; the 13 arena setups are dropped whole); Archives arena DOOR_SCALE 0.75 (proplvreset2 reads it, g_DoorScale is 0.75 on the cartridge); Bunker II arena monitors' OwnerOffset/OwnerPart/ImageNum (setupSingleMonitor); 3 starting-ammo amounts (Archives, Bunker, Bunker II: bondviewLoadSetupIntroSection). Read only on a hit, so unseen in a quiet minute: DLCOLLISION collision vertices (chrCreateBloodStain, chr.c:3151) and GROUP BoundingVolumeRadius (getjointsize). Junk on the cartridge: COLLECTABLE LinkedWeaponType, stan footer, pad-name strings, the fog and level rows the converter skips | `census/census.py` (report.md, read-by-the-cartridge column) | see census/report.md for each field's readers | code |
| 5 | Animated HD pictures with no frame table do not decode: the Complex arena's orange glow quad, the helicopter's rotor-blur disc | `xbla/census.py` | `beanDecodeTexture()` follows a frame-table pointer of 0 and reads the header's `'text'` magic as an offset; the frames are stored back to back in `.gpu`, frame 0 at offset 0 | code |
| 6 | One 4-triangle Dam snow draw (position-only vertex buffer) is never built in HD | `xbla/census.py` | in the report | code |
| 7 | Clip guns refill the clip 16 ticks into a reload; the cartridge 37-43 (most 38), on all 18 clip guns (the native port said ~34). Total reload time agrees but for Phantom/RC-P90 (row 21) | `guns/sweep.py` gun.reload_refill | GoldenEye refills at the start of RELOAD_RAISE (gunfire.c:3743) | ares |
| 8 | Raise after a weapon swap: ours 39-44 ticks, the cartridge 30-33 (KF7, ZMG, D5K, D5K silenced, Phantom, shotgun, Moonraker beyond the tolerance; the rest within it). The native port's ~26 was its own | `guns/sweep.py` gun.raise | not traced | ares |
| 9 | Grenade: press to throw ours 49 ticks, the cartridge 15; throw to bang ours 193, the cartridge 235 (240 minus the time held); throw sound ours 1473 (`pd1473`), the cartridge GRENADE_THROW; a grenade held through the hold cooks off once more on the cartridge (3 throws to our 2) | `guns/sweep.py` gun.release, gun.fuse, gun.sound_*, gun.throws | not traced beyond the fuse rule | ares |
| 10 | Mines: the cartridge places one every ~56 ticks while Z is held (10 in the hold), ours throws one on release (2); no ATTACH_MINE stick sound in ours; a proximity mine beside the thrower goes off 311 ticks after the throw on the cartridge, 614 in ours. ~~prox/remote draw 5 vs 21~~ - the native port's: on the cartridge the raise is within tolerance | `guns/sweep.py` gun.throws, gun.cadence, gun.sound_*, gun.fuse | not traced | ares |
| 11 | Automatic shotgun ejects a casing per shot (and CART_SPENT lands); the cartridge none | `guns/sweep.py` gun.casings, gun.sound_casing | `autoshot_stats` has no cartridge model | code + ares |
| 12 | Cougar and Golden Gun do not dry-click held empty: the cartridge clicks every 30 / 22 ticks (EMPTY_GUN_FIRE x7 / x21 in the hold), ours none | `guns/sweep.py` gun.dry_click, gun.sound_hold | not traced | ares |
| 13 | Grenade launcher / rocket: launch to bang on Dam ours 3 / 12 ticks, the cartridge 26 / 48 (Facility measured on the native port only: 5/3 vs 12/20) | `guns/sweep.py` gun.fuse | spawn point or speed - not traced | ares |
| 14 | Bullet hits on Dam's floor: HIT_BULLET_DIRT in ours, HIT_BULLET_STONE on the cartridge (17 guns); the Moonraker leaves no impact mark (the cartridge one a shot); the hunting knife's slash has no whoosh (the cartridge a KNIFE_THROW1-3 every slash) | `guns/sweep.py` gun.sound_impact, gun.impacts, gun.sound_tap/hold | not traced | ares |
| 15 | Frigate's sea: the real cartridge's water is blue, as ours is - the native port's green was its own fault. Left: the ROM's is a lighter teal with more visible waves than our darker, flatter navy | `ares/padshot.py` at pad 151 | settled on ares; the shade/wave difference not traced | ares |
| 16 | Archives prop 152 (model 18) sits 96 units higher in ours; Frigate armour 131 (model 115) 82 units higher (GoldenEye puts it 70 below its pad, ours 12 above). Both carry object flag 0x1 (the decomp's notes: "embedded crate, chain of boxes") | `world/sweep.py` obj.pos | not traced | ares |
| 17 | Every GoldenEye door has zero health words, every one of ours 1000 (433 doors) | `world/sweep.py` obj.health | whether a converted door can be destroyed where GoldenEye's cannot is not checked | ares |
| 18 | ~300 objects where GoldenEye's prop->pos sits away from its runtime_pos (glass, weapons, some props; ours has one position, at runtime_pos); room membership differs on ~300 objects (mostly one room more or fewer) | `world/sweep.py` obj.refpos, obj.rooms | GoldenEye registers rooms by prop->pos and builds collision from runtime_pos (propobj.c:952, the decomp port's own note); which one it draws from is not settled - a picture on ares (`ares/padshot.py`) decides | ares |
| 19 | Pads moved: Surface pad 288 and Surface 2 pad 279 by 72.6 units, Archives' bound pads by 2-4, Frigate's by ~1; eight pads in a different room | `world/sweep.py` pad.pos, pad.room | not traced | ares |
| 20 | Aztec doors 222-229 turned differently (0.4 off in the matrix) | `world/sweep.py` obj.rot | not traced | ares |
| 21 | Phantom and RC-P90 dry-click every 25 ticks held empty in ours; on the cartridge they do not; and their reload finishes 10 ticks sooner in ours (56 against 66 / 67) | `guns/sweep.py` gun.dry_click, gun.reload_idle | not traced | ares |
| 22 | Smaller gun rows: the sniper rifle clicks once (EMPTY_GUN_FIRE) after the release on the cartridge, not in ours; the automatic shotgun lands 5 pellet impacts a shot in ours, 3.6 on the cartridge; asking for the gun already in hand does nothing on the cartridge (gunRequestHandWeaponChange returns when it is the next weapon) and re-raises it in ours with PICKUP_GUN (the silenced PP7, Bond's starting gun) | `guns/sweep.py` gun.sound_after, gun.impacts, gun.raise | not traced | ares, probable |
| 23 | Frigate: doors 104, 105, 108, 119, 120 and 121 have no portal on the cartridge (portalNumber -1) and portals 57-72 in ours, so a shut one can close a portal GoldenEye never closes (the family of the "shut door onto the void" F3s) | `world/sweep.py` with GF_WIDE=1: door.portal | GoldenEye's setupDoor() (prop.c:941) asks for a door's portal only when it carries PROPFLAG_CULL_BEHIND_DOOR or PROPFLAG_NO_PORTAL_CLOSE; ours finds one for every door | ares |
| 24 | Archives: guard record 307's GoldenEye body 19 becomes our body 153, the other seven body-19 guards our 155 | `world/sweep.py` with GF_WIDE=1: map.guardbody (and map.body) | our port writes its own body rows into the records at the load (gexplus.c gexPlusMissionChr()); why 307 differs not traced | ares |

Behaviour rows (chr positions, ratings, AI lists after five seconds) are in each
sweep's "after the scripts have run" sections and need the AI trace diff to say
which are faults; Cradle's guards' accuracy/speed are assigned by a random pick
in their list (50/60/80), not a conversion fault as far as the world diff can tell.

## CE fixes visible in N64 mode

The regular build applies the Community Edition's data fixes in both looks
(`g_RomPatches` in port/src/geconvert.c, converters 60 and 68; ge-bean.md "The
Community Edition's fixes in the regular build"). N64 mode is judged against the
cartridge as it is, so these show as findings against ares and are left in the
reports. Whether each fix should be limited to XBLA mode is for the fix session
to decide. Keys are from the 20-mission ares world sweep
(`~/wt/gefidelity-run/ares-out/sweep/report.json`).

| finding key(s) | CE inventory item (g_RomPatches) |
|---|---|
| `surface pad.pos 288`, `surface2 pad.pos 279` (72.6 units; part of row 19) | Surface and Surface 2: the railing's path pad moved from z -5001 to -4968 ("stood past the rail") |
| `silo map.model 115` (record 59 becomes our 628, the other model-115s 627) | Silo: armour 59 model 0x73 -> 0x74 ("drawn as the full suit and giving half") |

Visible since the wide dump (`GF_WIDE=1`, ~/wt/gefidelity-run/widen/sweep): Control's
blast door, record 184 (`control door.doorflags 184`, 0x0000 -> 0x0004); Egyptian's
Golden Gun case panes (`egyptian door.doorflags 45-48`, 0x0008 -> 0x000c); Bunker ii's
two stair tiles (`bunker2 tile.link 794:1` and `796:2`, unlinked on the cartridge,
linked to each other in ours). Still invisible to the world dump: Surface's
paired Klobbs flag, an argument of a guard_try_spawning_item in list 0x421
(ai_31), which nothing names at the load and four jump_to_ai_list(self, 0x0421)
reach only on game events - no chr runs it in the first 300 ticks of an
untouched mission; it is a static AI-list comparison's to see. The rest of the
inventory is HD art, Bean's own draws, or the release's engine code, none of
which the N64 look uses.

## Harness facts that stay true

- The GoldenEye side of twin.py runs on ares (`--oracle ares`, the sweep's default;
  87 s for twenty missions against ~7 min on the port). The native port stays for
  its debug info and gdb probes; where it disagreed with ares it was the port
  (Bond's prop left at spawn values, no on-screen flag with rendering off, the sea).
- The gun rows (7-14, 21-22) are measured on the cartridge (`guns/sweep.py`, ares
  the default; 25 guns on Dam with the seeds pinned, both sides' repeat controls
  passing). On the cartridge there is no frame rule: its frames on Dam's quiet
  spot ran 2.0-2.8 ticks, and all eight automatics' cadence agrees with ours in
  its ticks - the native port's "GoldenEye frames x 2" is retired for ares.
  Not counted as findings: explosives show one impact a use in ours and none on
  the cartridge (our explosion makes a wallhit).
- A sweep's report is only as good as its null: if a report says NULL FAILED,
  the tool is wrong, not the game.

## AI trace diff on the cartridge (ai/sweep.py --oracle ares, 2026-10-01)

Each chr's script position sampled every frame on both sides; the first place
they part. Dam, Bunker, Archives and Control agree completely. Run before the
cartridge's seeds were pinned, so `rng` divergences were left aside; re-run on
the pinned oracle before acting on A2/A3.

| # | finding | cause | basis |
|---|---|---|---|
| A1 | The five guards of row 1 never run a script in ours (Facility 51, Frigate 26, Caverns 10 and 33, Aztec 9); Caverns chr 41 the other way round (ours spawns it, the cartridge does not) | row 1's spawn rule; `ai/spawnrefused.py` lists them | ares + code |
| A2 | Background lists part after a condition, ours around tick 4 and the cartridge at 20-40: Runway 4096, Statue 4098, Streets 4097, Depot 4097, Train 4106, Cradle 4096, Egyptian 4103 (at IFBondInRoomWithPad / IFChrDoesNotExist / IFDoorStateEqual) | probably the openings' different length on the cartridge (its still and swirl run before Bond is placed) - unverified, **open** | ares |
| A3 | Chr streams on one side only, not yet checked with spawnrefused.py: Surface 2 chr 7, Streets 39 and 40, Depot chr 2 (cartridge only); Depot's spawn at list 1027 (ours only) | **open** | ares |
| A4 | Movement splits: Statue chrs 6/8/9 at IFImOnPatrolOrStopped (list 1034+56), Jungle chr 1 | likely patrol timing; re-check pinned | ares |

Port-era AI findings the cartridge did not confirm (gone on ares): Facility chr
67's TRYUnknown6e split, Streets 4106's objective bit, Surface 2's
IFBondHasItemEquipped, Cradle's sight splits, the idle-loop RNG splits.

## View diff (view/viewdiff.py, cartridge in ares with pinned randomness; HD against the CE release in Xenia, 2026-10-01)

| # | finding | where | basis |
|---|---|---|---|
| V1 | Our eye 8.0-8.8 units low dominates the top-ranked pairs on every mission (close walls on Train, Depot, Archives, Silo) | row 2's cause | ares |
| V2 | Surface pad 262: the chain-link fence texture has fewer diamonds on the cartridge than ours | not traced | ares, likely real |
| V3 | Surface 2 pad 245: the snow is far whiter in ours | not traced | ares, likely real |
| V4 | Aztec pad 88: "Exhaust bay opening." shows only on the cartridge - script behaviour at that tick, not placement | not traced | ares |
| V5 | HD look, Facility pad 72: ours draws yellow/black hazard stripes where the release (CE) shows bare concrete | not traced | Xenia (CE) |

Not findings: Frigate's sea (the port's fault, row 15); Bunker 99-101, Statue
223-227 and Archives 170-172 end the mission on the cartridge when Bond is put
there (retried without them); Dam pad 329 and Facility pad 6 are Bean-side
faults of the oracle (Bond does not settle / black picture).

## HD draw census (xbla/, three-way: Bean's files, the CE release drawn in Xenia, our CE build, 2026-10-01)

| # | finding | where | basis |
|---|---|---|---|
| H1 | On 12 HD guard bodies the release switches off the body-head section, neck included (175-298 triangles each); ours still builds that neck | xbla/report.md | Xenia (CE) |
| H2 | The release draws its HD cartridge (new/gun/cartridge) on 20 of 23 captures; ours never loads it | xbla/report.md | Xenia (CE) |
| H3 | Animated pictures with no frame table do not decode (Complex textures 20/21/24, the helicopter rotor) - row 5 | `beanDecodeTexture()` | code + Xenia |
| H4 | Muzzle-flash cards, screen panes and the watch arm's casing: the release DRAWS them; ours paints GoldenEye's flash, monitor programmes and watch instead. These were "by design" before the release was asked - now real differences to decide on | xbla/accepted.json notes | Xenia (CE) |
| H5 | One 4-triangle Dam snow draw is never built - row 6 | xbla/report.md | code + Xenia |

Confirmed by design: a body file's own head (the release never draws it
either). Leads, not verdicts: Frigate's guards hold the silenced D5K
(chrmp5ksil) in the release, ours builds the unsilenced one; the release draws
headbrosnan on Aztec and Egyptian (our runs skip the openings where it could
appear). The CE switch changed no finding on either side. Not covered yet:
destroyed/alternate states, pool-only characters, most gadgets.
