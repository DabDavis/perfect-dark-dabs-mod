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

## Where each row stands after the fix session (2026-10-01, merge/fid-1001)

Six branches (fix/fid-conv, -chr, -guns, -look, -hdfix, -hd) were merged and
gated together against `pd.base` (the unmodified build, converter 96) with
the merged toolkit; converter 97, HD cache 13. Gate on the merge
(`~/wt/gefidelity-run/gate/fid-merge`, bases in `gate/fid-merge-base`):
world (20 missions, wide) 923 findings -> 550, placement 378 fixed, 3 new (the
three panes on floor portals, row 18); census 10 dropped + 85 unread fixed,
none new; the other legs' lines are in "Gate on the merge" below. *instrument*
= the tool was wrong, not the game; its fix is named.

| # | status | commit(s) | what is left |
|---|---|---|---|
| 1 | **fixed** | 95c68591b | Not "a clear spot near the pad": GoldenEye's getposstan() never moves a guard - stanTestVolume() at the pad (the tile walk through linked edges within 20, then the edges only of props' collision outlines, in plan) makes it there or not at all. `geStanSpawnLegal()` (gestan.c) is that test on converted missions; pd.log names each refusal's cause. Facility 51, Frigate 26, Caverns 10/33, Aztec 9 made, Caverns 41 refused as on the cartridge. Still refused on both, rightly: Facility 58, Aztec 12/13, Silo 58, Streets 20. The graph's whole-unit rounding is allowed for (0.75) |
| 2 | **fixed** | d7ebb9930 | Converted missions: eye = head y + 7, crouch -100/-60 absolute (bondwalk.c); standing 167.28 on both. player.eye fixed on all 20 |
| 3 | done | - | - |
| 4 | **partly** | 76fac26e1, 24fc3e30c | Multi-ammo crate slots keep their GoldenEye models; the watch magnet's 5 charges are converted (AMMOTYPE_1C) and spent a press, the fist after the last, the count on GoldenEye's HUD. The camera's AMMO_CAMERA x10 is never spent on the cartridge (its AmmoType is AMMO_NONE) - ours already does that. Left, each a converter feature: the 13 arena setups' collectables and ammo crates, Archives arena DOOR_SCALE 0.75 (geSoloDoors() over the arena setup), Bunker II arena monitors' owner/image, characters' SHADOW nodes (GoldenEye's blob shadow); DLCOLLISION / GROUP radius read only on a hit |
| 5 | see H3 | - | - |
| 6 | see H5 | - | - |
| 7 | **fixed** | 7b514bfeb | Refill at RELOAD_RAISE's start, the reload two GoldenEye frames after an empty trigger lets go: refill 37 / ready 60 (cartridge 36-40 / 60-67) |
| 8 | **fixed** | 7b514bfeb | The next gun loaded whole after three frames and raised: raise 31 (cartridge 30-36) |
| 9 | **fixed** | 61c40c190 | Press to throw 15/15, fuse 234/235, throws 3/3, GRENADE_THROW |
| 10 | **mostly fixed** | 61c40c190, 27a90ab1b, e66a97baf, 35fb7d8f4 | Ten placed in the hold every 54 ticks, from GoldenEye's hand position at its speed, ATTACH_MINE, proximity fuse 331 (cartridge 337); mines stack (objEmbed: a mine landing on one is its child, freed unexploded with the chain) and explosions hurt GE thrown weapons only on GoldenEye's quarters inside its box (`explosionGeHurtsWeapon()`). Left: the cartridge's first timed mine sometimes bounces apart without sticking (why not traced; the guns leg allows one `unstuck`) |
| 11 | **fixed** | 7b514bfeb | No casing, no CART_SPENT |
| 12 | **fixed** | 7b514bfeb | GoldenEye's DRY_FIRE rule for every magazine gun: Cougar 28/30, Golden Gun 22/22 |
| 13 | **fixed** (mostly *instrument*) | 62eb45710, 2b769ce16, c0da9f76c, 663b5d76b | The cartridge's shots went out level: its look-ahead wound the poked -30 pitch back to -4 in 46 ticks; held, the flights agree. Then GE rounds and rockets land with DROP_GUN and are hurt by GoldenEye's explosion rule, the rocket gains 1.111 a tick, the launcher's round leaves at 33.3 a tick |
| 14 | **fixed** | 61c40c190; *instrument* 62eb45710, 8166d0655 | Stone/dirt was the pitch (held, the cartridge hits dirt as ours). Moonraker marks + surface sound and the knife's whoosh fixed. GoldenEye's second shot sound is no longer read as an impact |
| 15 | **partly** | 1f608b07a | The pattern: GoldenEye's texSelect() loads image 1509 as CI8 indices with the TLUT on, so the RGBA16 tiles are read by each texel's upper byte - now read the same way, the waves lie the cartridge's way. Left: wave contrast a little lower (ours drawn at 2x, downsampled); the "lighter teal" is not in the pictures (row means within ~5/255) |
| 16 | **fixed** | 97db331bd | GoldenEye stands a new object on the one placed last (searches its prop list from the end), ours searched from the head: `objFindByPosGe()` |
| 17 | not a fault (*instrument*) | bb3c9da20, 5f8b5ead7, be20d4639 | setupDoor() never divides the 16.16 setup word, so a GoldenEye door's float health holds the integer 0x03e80000, which the dump printed as 0.0; doors are mortal in neither game |
| 18 | **mostly fixed** | a91f1c830 | Placement was right: the cartridge draws and collides at runtime_pos (obj.refpos, 301, is not a fault - a TRANSLATE/accepted candidate). Rooms: GoldenEye's own rule ported for converted objects (tile locus round prop->pos + portal expansion over the box widened 30), doors' rooms computed at the conversion; aresge.py reads 8 room bytes as the cartridge does. obj.rooms 282 -> 19. Left: panes standing exactly on a floor portal (Control tinted glass 133/156 and Dam glass 118 now new, Dam 108/117 as before) - the cartridge's box sits 0.003-0.01 off the portal by its own float rounding, a coin toss; Train's six model-119 props and six panes, whose walk the cartridge stops where ours (whole-unit stan points) goes on |
| 19 | **fixed** | c8448e524, 6a9e6c0ca | Pads filed on the tile their setup names; door scale moves door pads along the portal normal as setupDoor() does (DOOR_SCALE written 1.0). Surface 288 / Surface 2 279 were the CE's (now HD only). Left: ten regular pads 1.0 unit up - writePads()'s deliberate lift off a floor level with the pad (an accepted.json candidate) |
| 20 | not a fault (*instrument*) | 5be68f853 | Aztec's chair doors start open; the dump compared our turned matrix with GoldenEye's closed one |
| 21 | **reload fixed**, dry click left | 7b514bfeb | Ready 60 vs 66/67, in tolerance. Left (rcp90 dry_click): the cartridge fires 9 ticks a shot to our 6 (gegunsRpm()'s 30 fps), empties later and clicks once before the release - the cadence, not traced |
| 22 | not faults (*instrument*) | 8166d0655, 62eb45710 | (a) the release-tick click is the hold's on both sides; (b) pellets 5/5 with the pitch held; (c) the cartridge's first gun of a group is already in hand (raise 0) - pp7silenced raise/draw stays listed |
| 23 | **fixed** | c8448e524, a91f1c830 | The converter works out setupDoor()'s own portal (only with CULL_BEHIND_DOOR) and the door's rooms and writes them in the door record |
| 24 | not a fault (*instrument*) | dd402626b | A body worn with the head its record names gets a row of its own carrying that head (geRomBodyRow()); the diff keys such a body as body+head |

### Gate on the merge

Bases swept fresh with the merged toolkit (`pd.base`, own run dir and
conversion; the ai base with `GE_ROM_PATCHES=1`, see README traps):

- **world** (20, wide): placement 378 fixed, 0 better, **3 new** (control
  tintedglass 133 / 156, dam glass 118 - row 18's coin toss), 0 worse; 923 ->
  550 findings. Behaviour 3 fixed, 5 new, 3 worse: Statue chrs 6/7 and their
  bits from the portal walk (A4); Aztec chrs 33/34 further from the
  cartridge's (fid-chr's rows 1/2, in its own gate too); Train player.pos 13.3
  and Frigate/Aztec hidden 0x100 also appear in pd.base's own sweeps
  (run-to-run flips).
- **view** (20, 1815 pairs): 1210 better, 0 new, **1 worse** - Jungle pad 437
  h090 0.08 -> 0.32: a drone gun's tracer in our picture. pd.base repeats 0.08,
  the merge 0.32; the portal walk alone (pd.fid-look) gives 0.23, the other
  branches 0.06-0.08 - which rooms are on screen decides when the autogun
  fires in the frozen tour (fid-look's notes: Jungle 371, Surface 190), not a
  drawing fault.
- **guns** (25 on Dam): 63 -> 4 findings, 59 fixed, 2 better, 0 new, 0 worse.
  Left: pp7silenced raise/draw (instrument), rcp90 dry_click (row 21), timed
  mine explosions (the first mine's stick-or-bounce, row 10).
- **ai** (20): 7 fixed, 1 better, 0 new, 0 worse. Left: Runway bg 4096,
  Statue bg 4098, Statue chrs 7/8 (A4), Surface 2 spawn@1059 (cartridge only,
  in the base too), Egyptian/Jungle timing.
- **census**: 10 census.dropped + 85 unread fixed, 0 new. **convdiff**: 40
  files changed, 13 new (the `_ce` copies, Pgx203Z).
- **replay**: match, solo, gematch, optsolo, randrun, randmission the same -
  Perfect Dark's own game unchanged; gesolo diverges from frame 100 (Dam:
  rooms, eye, truck, guns) and optmatch from frame 400 (GoldenEye's guns rolled
  into that match) - on purpose.

## The rows as found

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

**Settled 2026-10-01 (6a9e6c0ca, 53b462e97, converter 97): the CE's data fixes
are the HD look's only.** The converter writes the cartridge's files and, where
a fix changes one, a `_ce` copy beside it (`ce "file copy ..."` on the
mission/map line); setup.c, tilesreset.c and gestan.c take the copy only while
the release's meshes are on with the CE applied (`geRoomCeData()`, decided at
the stage load). Every key below is gone from the N64 world sweep; the HD look
with the CE shows each fix. The Python twin applies the patches only with
`GE_ROM_PATCHES=1`. What follows is the record as found.

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

After the fix session (2026-10-01):

| # | status | commit(s) | what is left |
|---|---|---|---|
| A1 | **fixed** | 95c68591b | row 1 |
| A2 | *instrument* | 58828402b | The cartridge was sampled once a game frame, so two AI passes between samples hid a Yield and the skipped stop read as the other branch; it is now sampled every video frame (and a residual skipped sample is classed as timing). Left: Runway bg 4096 at tick 439 (Bond's room as GoldenEye's opening ends - ours skips the opening) and Statue bg 4098 at tick 244 (two stops unseen, unexplained) |
| A3 | *instrument* | 759b3fa60 | A chr the cartridge first shows mid-run (already renamed by SetMyChrNum) is named spawn@<list>, as ours is. No stream is one-sided now |
| A4 | open | - | Statue's runners sent to a Bond nobody can see (the harness's) stop at different times; in ordinary play both fight Bond within ~300 ticks of each other. Jungle chr 1: a guard's distance + an rng split, not traced. GoldenEye's portal walk (91460e70e, 2fff513f2) moved Statue chrs 6/7 at tick 300 in the world leg (chr 6 415 -> 102 units, chr 7 482 -> 1217, CHRCFLAG_FORCETOGROUND on 2) - which rooms are on screen decides which chrs tick as on screen; behaviour, same family |

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

After the fix session (2026-10-01):

| # | status | commit(s) | what is left |
|---|---|---|---|
| V1 | **fixed** | d7ebb9930 | row 2: eye dy -8.1..-8.6 -> -0.45..+0.28 on every mission |
| V2 | **fixed** | 91460e70e, 2fff513f2 | Not the texture: room 10's tree billboards cover the fence on the cartridge because its portal walk comes back into the camera's room at depth 2, so the fence's room is blended first. GoldenEye's own walk (`bgTickPortalsWalkGe()`: queue, 9 visits, depth 15, the far-portal rule, the portal table's order) on converted levels; draw lists equal the cartridge's at Surface 262 and Facility 72 |
| V3 | **fixed** | 163df9c91 | Fog: the cartridge's RSP fogs each vertex (with its clipping and guard band) and the RDP carries it across the screen; `G_FOG_VERTEX_EXT` does the same on converted levels in the N64 look (GL and Vulkan). Surface 2 pad 245 h180 0.603 -> 0.166 |
| V4 | not a fault (*instrument*) | - | The tour's AI freeze is not symmetric: on the cartridge Aztec's ai_11 chain (objective 0x40000, text 0x614) runs even with every chr slot's list cleared, ours is stopped. Left as an instrument note |
| V5 | **fixed** | fdba3194f | see the HD section |

Also: the tour runs our side with Head Roll and Always Show Target off and records
headlook (cf4d16635: the head animation's look moved our camera up to 0.45 degree
through a simulation the portal walk had changed); the oracle-repeat null holds the
cartridge's jitter against the one-pad-off median (f3af72744: against the matched
median it failed as soon as ours came close - Bond's eye fixed). Left: Surface pad
190 h090, whose only changed pixels are the gun's idle pose (another channel the
simulation feeds; the scorer would have to mask the gun or the tour pin its
animation).

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

After the fix session (2026-10-01):

| # | status | commit(s) | what is left |
|---|---|---|---|
| H3 / row 5 | **fixed** | 1bf010817, 0ee87a9e2, c440979fc, 70e08a465 | The release's loader dispatches on header +0x1c: 4 = one texture a frame (per-frame tables, each frame at the next 4K), 5 = an array texture (slices back to back from 0); `beanDecodeTextureFrame()` decodes both and the renderer keys its cache on the frame shown (`TextureCacheKey.anim_frame`). Timing and UV motion from the rendergraph's place2d table (+0xdc, 0x64-byte entries): frames and seconds a frame; slides in repeats a second; turns at **half** the table's figure in degrees a second (settled by fid-hd's Xenia captures of the Frigate radar and Dam's water). The Complex beacon blinks, the radars sweep, Dam's reservoir slides. Left: the bump/reflection compositions (Complex water, Dam reservoir, Silo, Control marble) need a material path our one-picture draw does not have - the renderer redesign, not started; df3c951ae draws each such surface's own picture (Complex 23, not its sphere map 22) |
| H5 / row 6 | **fixed** | d556050f8 | Dam stream 0x4dc0 is a stride-12, position-only buffer (the only stride under 16 in any level); built as untextured triangles in its material colour (c12: black, a sheet at the cliff's foot) |
| H1 | **fixed** | 714178be7 | The release turns off a body file's whole head section (0x17 kind 0) on a body that wears a head file; ours now leaves the section out, not only its skin ring. The 23 extras the pool pass still builds (Perfect Dark's Combat Simulator rows) are `hd.accepted.pool` |
| H2 | **fixed** | 655163b89 | Each release casing laid on PD's model of the same casing; a GoldenEye gun's casing loads its alias (`gebeanCasingFile()`), brass sphere-mapped as the release has it. PD guns and the N64 look keep the old casing |
| H4 | **fixed in first person**; third person left | 0d0bf4a1d, 7d39c78db, 6cba264da, 5d2a17703, dccde0d27 | (the user's decision: draw them as the release does; accepted.json no longer excuses them.) Screens: the release's 50 monitor pictures bound at GoldenEye's picture addresses, GoldenEye's programmes run unchanged, the untextured pane drawn as the backing. Watch: the arm's lamp and glass (one coat, blended). Flash: the release's cards (any draw of piece 0, or on the muzzle's bones) in GoldenEye's flash and star lists, blended, clamped, two-sided; where GoldenEye's model has no flash matrix of its own (AR33, RC-P90) on the star. fid-hdfix's turn (half the table's 999: 499.5 degrees a second) runs on the AR33's and RC-P90's cards: one frame a shot, a different roll each shot (checked firing on Dam, merged build; no release capture of those two guns - no mission starts Bond with them in the release). Left (hd.undrawn): third-person flash cards (chrm16, chrfnp90 0x1fc; chrautoshot's four, which the CE moved to the origin) - PD's chrgunfire sprite would need an HD path; the rocket launcher's ring (no flash switch on GoldenEye's model, never captured); console2/console3's pane (no capture shows them) |
| V5 | **fixed** | fdba3194f | The concrete and GoldenEye's stripes lie on the same six triangles; the release draws opaque level geometry in stream order with depth LESS, so the later twin never shows. `dropTwins()` keeps the first opaque twin (22 on Facility, 445 over the levels) |
| HD board | **fixed** | d08b7da49, 89fc4ebe1, 4299cce66, dc28dd303, a63e56f60 | Statue's bulbs (a draw with no UV or picture in its vertex colours; the release's bluish glow along the wire left), Control's stripes (the release's alpha test multiplies the vertex alpha), Archives' papers (of two blended faces the later shows) and bullet holes (lifted off the decal they hit), Aztec's armour closet (room 18's wall, its bigger monitors and the post at pad 139 kept out of the N64 closet, the user's direction to fix what looks wrong in the HD data); Control's elevator door was already fixed |

Gate (HD legs on the merge; the release's side reused, ours against `pd.base`
re-swept with the merged toolkit in run directories of their own): hd-census
(every mission; the base is the morning's census re-analysed with the merged
census.py) 171 -> 83 findings, 111 fixed (84 undrawn, 23 extra, 3 nodecode, 1
unloaded), 0 new, 0 worse, 23 behaviour lines new (row H1's pool builds, now
`hd.accepted.pool`); hd-world Dam 4 fixed (door rooms, row 18), **1 worse**:
player.eye at spawn, 3.6 below the release's before, 4.8 above it now - row
2's eye is the cartridge's (167.28), and the release's own eye sits ~4.8 below
the cartridge's at Dam's spawn; whether the HD look should take the release's
eye is open. hd-view: see "Gate on the merge".

HD cache (`HDCACHE_VERSION`) 13 after the session (fid-hdfix's 12, then fid-hd's level changes).
