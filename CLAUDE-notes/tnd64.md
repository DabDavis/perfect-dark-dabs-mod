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

## Not done

- Girl Power Mode (the pokes above), and the runtime's side of the new files:
  gexplus.c reading gecast.bin's fourth byte (8 heads), gexfront.c reading
  getimes.bin and taking the mission count from the folder (credits after
  Boat, no advance past The End, 007 after 14), gewatch.c's tint 2 (the
  colours are in critic/RESULT.md section D), the Perfect Menu row.
- The Stage Loader's "NAME (mod)" is cut at 31 characters
  ("Atlantic Hotel (Tomorrow Neve)").
- Nothing checked against the cartridge yet (no ares twin run for TND).

Rigs: `~/wt/tnd-run/rig` (the game, all three sources in added-content/),
`~/wt/tnd-run/rt` (replay test), `~/wt/tnd-run/run.sh` (standalone GE/GF/TND).
