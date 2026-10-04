# Tomorrow Never Dies 64: a second GoldenEye ROM hack beside GE Plus

Tomorrow Never Dies 64 (TND64, 2020-2022) is a GE Editor patch against
GoldenEye 007 (US), converted the way Goldfinger 64 is (read
[goldfinger64.md](goldfinger64.md) first: everything there holds unless this
says otherwise) into `mods/Tomorrow Never Dies 64/`, maps-only, with its own
row in g_Layouts (geconvert.c). Only **Expanded 06-22** converts. It ships as
`tnd64.zip` with nine xdeltas: Expanded 06-22, Expanded's three earlier
releases and the Original V1-V4. `variantPatchesIn()` (gexplusrom.c) applies
each to GoldenEye's ROM and converts the one `layoutOf()` knows; the others
make a ROM no row matches and say nothing (one line if none of a zip's
patches makes a hack). V4 (20 MB, its tables where GoldenEye's are) would
need a row and a name of its own ("... (Original)"): not done.

## How it was measured

`~/wt/tnd-work/measure/{tables,menuraw,levels,chrsguns,critic}/RESULT.md`,
scripts beside them. The method is Goldfinger's: each table's address is the
lui/lo pair at the PC where the US code loads it (`sites.py` pairs every site
in both ROMs), lengths from the code's immediates or the data's own extent,
and the Expansion Pak piece from ares's RDRAM at the Rare logo
(`n64twin --expansion-pak`, `tables/ram_exp.bin`) matched against the ROM
page by page. The critic pass re-measured anything two reports disagreed on.

## The row (geconvert 113-117)

| | US | TND64 Expanded |
|---|---|---|
| ROM | 12 MB | 16 MB, GoldenEye's title and code, its own CRCs |
| images | 8-byte rows, 2698 | 4-byte rows, 23-bit size, 3045 from 0x89e60a |
| props, hats, explosions | | unmoved |
| characters | 0x8003de10, 80 | 0x80702e00, 112 (piece 0x80702000 <- ROM 0xffd000, 0x1c00) |
| animation_data | 0x28e980 | 0x2881b0 (`animdata`) |
| global images (segment 2) | 0x29d160 | 0x296470 (`globalimages`) |
| men a level draws from | 4 | 8 (`maleheads`, bodyChooseHead()'s `andi 7`) |
| missions | 20 | 14 (`nummissions`) |
| watch | green | blue (`watchtint` 2) |

New fields, 0 meaning GoldenEye's: `animdata`, `globalimages`, `maleheads`,
`nummissions`, `aihooks`, `nolaser`. Characters 80-111 (C50Z-C6FZ) are one
placeholder file, a copy of CheaddwayneZ, and convert.

## What it needed beyond Goldfinger

- **Animations.** TND rebuilt animation_data and moved every record but the
  first. An id's record is now read from the data segment's
  animation_table_ptrs1[]/ptrs2[] (0x80029d6c, 0x8002a04c) into the layout's
  segment (`animRecord()`); `g_IntroAnims` names ids. GoldenEye's and
  Goldfinger's tables are geanimtable.h's to the byte.
- **Names in two banks.** Three arena names and most mission names are
  LmiscE's (bank 44), not LtitleE's. `textString()` reads any id by its bank
  through LnameX_lookuptable (0x800484d4, an English and a Japanese file name a
  bank). The folder's names out of another bank are appended to the menu's
  LtitleE as slots past the ROM's (287 on) and missionfolder.bin names those
  slots, so gexfront.c reads them as any other. The arena table ends at its row
  with no text (it ended at a change of bank).
- **Dead missions.** Folder rows for missions 14-19 are still there, names
  blank, setups and briefings gone: skipped, and chapter headings left empty
  dropped. Only the missions' own briefings and banks are copied (and LlenE).
- **Golden Gate Bridge** (level 24, scale 0.1): all four rooms have water and
  hills 100,000 to the sides, single triangles 66,670 long, past the s16 a
  room's vertex is. `roomSplit()` keeps what fits about the room's centre and
  cuts the rest (sides halved to 12,000 at most, texture coordinates and
  colours carried) into 40,000-wide pieces, rooms of their own after the
  level's with no portal and no lights, naming their room in the room table
  row's padding (`struct bgroom.drawnwith`, filebg.c); bgSetRoomOnscreen()
  puts a converted level's pieces on screen with their room. 62 pieces. A cut
  side next to an uncut one can leave a crack, far out in the fog.
- **AI hooks** (`aihooks`). TND's code takes TRYFindCover (0x2b) with a label
  0xfb-0xfe as a memory poke spelt by the PRINT (0xad) after it ("2b fe" then
  "8007A0B0+P0000041C+C00000005"): Girl Power Mode, its clocks, its bonuses.
  Converted as a cover search, a background list (no prop) crashed every
  mission at once in chrGoToCoverProp(). Hook and PRINT are dropped; what they
  poke is not converted.
- **Guns.** Items keep GoldenEye's numbers. The extras go to the H&K P7 (20,
  0x80), Desert Eagle (21, 0x81), Watch Laser (23, 0x82) and Phone Taser (31,
  0x83); the last two are held as no prop, which used to cost them their
  weapon (Hotel's opening gives the taser). The FAMAS is on GlaserZ, so on the
  Moonraker's 0x6e, and TND turned all fourteen laser tests to 0x99
  (`nolaser`): geguns.bin is GGN3 with GEGUNS_NOLASER, and while its set is in
  the Moonraker's number stands on the AR33's host (g_GeWeaponHosts, no
  longer const).
- **menu/getimes.bin** (any variant): solo_target_time_array, "GET1", u16
  missions, three s16 seconds each. Goldfinger's conversion gains it too.

## Traps

- The zip's nine patches all apply to GoldenEye's ROM; only the CRCs tell
  Expanded 06-22 apart (V4 and the earlier Expanded are GOLDENEYE/NGEE too).
- Its code DMAs the folders' backdrop 178 bytes short of its RLE stream; the
  row copies the whole stream.
- `--boot-stage` on a mission stage is the quickest check: its ids depend on
  what is mounted (rig: arenas 0x72-0x7c, missions 0x7d-0x8a, Bridge 0x88),
  read them under gdb from `'modloader.c'::g_ModStageMission`.

## At run time (converter 118)

- **Its row on the Perfect Menu.** The hack rows are two now (mainmenu.c,
  `item->param` the variant's index): "Goldfinger 64" and "Tomorrow Never Dies
  64", each hidden unless its source is in added-content/ (gexplusrom.c's
  `variantReady()`, which keeps them in g_Layouts' order whatever order the
  folder lists them in). Each opens GE Plus's folder in its mode
  (`g_GexPlusVariant`); its music and sounds are its own bank (gemusic.c,
  gesfx.c, by the stage's mod or the folder's), its best times
  `$S/tomorrowneverdies64-times.txt`, no XBLA on its stages
  (`xblaSwitchStageHeld()` answers 1 there; it was already any hack's).
  A fresh save dir gets `Mod.MapMods=GoldenEye Arenas;Goldfinger 64;Tomorrow
  Never Dies 64` from the first conversion (`modMapsEnableByName()`).
- **Fourteen missions and the folder's way on** (gexfront.c). Converter 118
  writes the two code immediates in `menu/getimes.bin`'s spare u16:
  `romlayout.creditsafter` (7F0168BC, `li 0x11` the Cradle; TND 9, the
  Stealth Boat) and `advancebelow` (7F0168D4, `slti 0x12`; TND 13), 0 for
  GoldenEye's, so Goldfinger's file is unchanged. `frontLoadMissionCount()`
  reads the count, the two and the target times with the folder's rows;
  without the file (GoldenEye's own conversion) it is 20, 17, 18 and
  `g_TargetTimes`. So NEXT after The End and after the Boat goes back to the
  grid (a hack's conversion has no Cuba, so no credits to play; the credits'
  mission does not move on either - the cartridge plays the credits and comes
  back to the grid), 007 under Mod.GePlusLockedProgression needs fourteen on
  00 Agent (7F01F4C8), and the statistics page's target time is the hack's
  own. The bonus missions' locks (`frontMissionStatus()`) keep GoldenEye's
  18 and 19: TND left fileIsStageUnlockedAtDifficulty() alone.
  Goldfinger now also gets its own target times (it read GoldenEye's), and
  NEXT after its Cradle-slot mission goes back to the grid where it went on
  to mission 18's briefing (it has no Cuba either).
- **The watch is blue** (gewatch.c, tint 2): each of GoldenEye's seven watch
  colours replaced by TND's (`g_WatchTintBlue`, the table in
  `~/wt/tnd-work/measure/menuraw/REPORT.md`), any other colour and the
  vertex colours with green and blue swapped, the mission status's INCOMPLETE
  a steady 0xffff00b0 and the objectives' a steady 0xbfbf40ff. The HUD's
  ammunition counter follows (`geWatchTint()`). Its text says "Q WATCH v2.02
  BETA" (its own LoptionsE).
- **Eight men a level** (gexplus.c): gecast.bin's fourth byte
  (`g_GeRomMenPerLevel`, 0 for GoldenEye's four, at most the game's eight).
  Hotel draws heads 509-502, eight rows.

Checked (rig `~/wt/tnd-run/rig2`, fresh save dir, the three sources in
added-content/; `boot.sh`/`bootall.sh`): all three convert at the first start
(16 s), MapMods is set, 11 arenas (0x72-0x7c) and 14 missions mount; every
mission (`--boot-ge-mission N --boot-ge-variant "Tomorrow Never Dies 64"`) and
every arena (`--boot-stage`, two sims) runs to frame 1700 with sound (its own
bank, audio RMS 2000-9000), as do GE Plus's Facility and Goldfinger's Junkyard;
pictures in `~/wt/tnd-run/shots/`. Probes (`~/wt/tnd-run/probe2/`): the
Perfect Menu's variants in order, Hotel's inventory (Unarmed, PPK (silenced),
Phone Taser), the watch blue (`watch.py`, 220 frames after
geWatchPause() - a picture sooner shows no watch), the FAMAS fired on Alaska
at frame 1300 (16 rounds in 60 frames, casings, no beam; before frame ~1200
Alaska's opening swirl still hands Bond his PSG-1 back), the folder's
NEXT from each mission in all three modes (`folder.py`).

## After the verifiers (converter 119)

- Volcano's five crates on bound pads 88 under their floor (records 93,
  101-104) stood a storey down: a bound pad whose box is wholly under its
  tile is lifted onto it now, as plain pads were since converter 103. Agrees
  with the cartridge's world dump to 0.001; GE and GF output unchanged.
- A gun's pickup message is the level's LpropobjE string for its item where
  that names the gun ("a H&K P7.", "the Golden Gyrojet."); Goldfinger 64's
  items mostly do not match its strings, and keep the guess from the name.
- A split room's pieces go dark with it when a script disables it.
- Without the GoldenEye ROM, tnd64.zip logs one note, not one a patch.

## Not done

- Girl Power Mode (the pokes above), and its clocks and bonuses.
- The "randomizing guard height and injury sound pitch" ASM mod (changelog
  line 33): on the cartridge about half the chrs, in pairs by chr slot, are
  0.098 where ours are 0.100 (Boat 0.089 vs 0.091) - worlddiff chr.scale on
  Bazaar 6/14, Hotel 18/39, Boat 10/21, Volcano 14/35, the same under every
  GF_SEED. Code touched: makeonebody (7F0233C8) and
  chrlvModelScaleAnimationRelated, not confirmed; the injury pitch half not
  checked.
- Its credits: Cuba is not converted for a hack, so nothing plays after the
  Boat.
- The Stage Loader's "NAME (mod)" is cut at 30 characters
  ("Atlantic Hotel (Tomorrow Neve)", "Complex (Tomorrow Never Dies )"):
  the Combat Simulator's arena row.
- Nothing checked against the cartridge yet (no ares twin run for TND). Hotel,
  Party, Tower and The End start with Bond unarmed (his PPK in the inventory
  on Hotel): not checked against the cartridge.
- ASan (`~/wt/tnd-run/asan.sh`, build `~/wt/tnd-run/build-asan`): all 14
  missions and 11 arenas to frame 1700, nothing new. Seven missions report a
  read past `var800a6470` in bgTestHitOnChr() (bg.c 5103-5111, a chr hit test
  while aiming): Perfect Dark's own bug, kept from the decomp - the bounds
  loop starts at `var800a6470[spdc]` where the vertices are at `spdc * 3`, so
  a 16-vertex load reads 12 bytes past the array. GE Plus's Facility did not
  reach it in 1700 frames; not fixed (a read, and the fix moves hit tests).
- The Windows cross-build builds; not run under wine.

## F3 pass 33 (guns)

- **LAW 80 in the left hand (F3 20261004-000140).** Under Akimbo (the
  port's mixed pairs) the LAW 80 left of a Remington filled the view with the
  inside of its tube. TND's LAW model is built off to the right of its
  origin, and its row (0x00020af1) has neither CAN_DUAL_WIELD nor
  MIRROR_DUAL: the cartridge never holds it in the left hand, and its host
  (Perfect Dark's rocket launcher) has no WEAPONFLAG_DUALFLIP, so drawn
  unmirrored at -posx it stood at the eye. A GoldenEye-row gun with no
  CAN_DUAL_WIELD drawn on its own model now gets DUALFLIP
  (gegunsSetOwnModelInUse(), `flipAdded[]` undoes it for the other look):
  only the LAW changes in TND (its watch laser and taser had DUALFLIP
  already); GoldenEye's own are all CAN_DUAL_WIELD but the thrown ones,
  which are skipped (no model in the hand). GoldenEye's rocket launcher
  (MIRROR_DUAL, CAN_DUAL_WIELD, model centred) stays as it was.
- **Flash on the silenced guns' numbers (F3 20261004-031854).** TND's
  MP5A2 and Goldfinger 64's drum-magazine Thompson stand on the silenced
  D5K (0x65), which gegunsOwnTrigger() gave FUNCFLAG_NOMUZZLEFLASH by number.
  The cartridge has no such rule: gunfire.c lights Switches[1] of whatever
  model is in the hand on every shot (flashvisptr), and in ares the switch
  word (hand->modeldatas[0]) is 1 on every shot of TND's MP5A2 and PPK
  (silenced) - and of GoldenEye's own silenced PP7 and D5K on Dam (US ROM:
  7 of 7, 30 of 30 shots). A hack's set now keeps the flash on both numbers
  (`stats != geStats`); GoldenEye's own still have none (GoldenEye X's
  choice from 31c341608, not the cartridge's - left for a decision).
- **Shot sounds (F3 20261004-030108, the Norinco).** guns/run.py with
  GF_GAME=tnd on Bazaar (both sides; ares-side patch: the watch also reads
  hand->modeldatas, run_ge_group/run_pd_group pass GF_GAME and the variant):
  every gun's shot is the same id as the cartridge's, from TND's own bank
  (109 for the Norinco, re-recorded: 23352 bytes, keyBase 54 detune 50, decay
  0.53 s - no long envelope), as many times a hold. Two differences, neither
  the shot's: the cartridge ran this empty scene at ~1 tick a frame (no frame
  cap in GoldenEye; 2.1 on Dam), so its automatics fired every 3 ticks
  against our fixed GoldenEye frame of 2 ticks (6, gegunsRpm()); and its
  bullets hit Bazaar's walls as HIT_BULLET_SNOW where ours rang STONE.
- **A hack's surfaces (converter, needs a GECONVERT bump).** getexsurface.c
  gave every converted level GoldenEye's own image surfaces
  (geimagesurfaces.h, indexed by GoldenEye's image numbers). A hack's image
  rows carry their own in the top byte (the 4-byte rows' bits 24-31, as
  GoldenEye's first byte): TND changed 15 of the textures its levels use and
  has 345 past GoldenEye's 2698 (55 not default). A variant's conversion now
  writes `menu/gesurfaces.bin` ("GES1", u16 4096, u16 0, a byte per texture
  number as written, after variantTexRemap()), and getexsurface.c takes it
  where it is; GoldenEye's own conversion writes none and is unchanged. On
  Bazaar the guns sweep's impacts now agree with the cartridge (SNOW, RICO)
  for the Norinco, MP5A2 and silenced PPK. Without a bump an existing
  conversion has no file and keeps GoldenEye's table.

Rigs: `~/wt/tnd-run/rig` (the game, all three sources in added-content/),
`~/wt/tnd-run/rt` (replay test), `~/wt/tnd-run/run.sh` (standalone GE/GF/TND).
