# Goldfinger 64: a GoldenEye ROM hack converted beside GE Plus

Goldfinger 64 (2017, made with SubDrag's GoldenEye Setup Editor) is a patch
against GoldenEye 007 (US). The game converts it the way it converts GoldenEye
for GE Plus, into a folder of its own, `mods/Goldfinger 64/`, and lists its
arenas through the Stage Loader and under its own row on the Perfect Menu. It is
never part of GE Plus, which is GoldenEye's ROM alone.

## Where it comes from

`added-content/` is where it goes, like the GoldenEye ROM and both XBLA
releases: the zip it is passed around as (`goldfinger64.zip`, holding
`Goldfinger64_1_0.xdelta`), the bare patch, or a Goldfinger ROM patched
already. `gexPlusRomConvertVariants()` (gexplusrom.c) runs after GE Plus's
conversion at startup:

- a patch is applied to the GoldenEye 007 (US) ROM in `.z64` order (the
  player's may be `.n64`: `geconvertIsGoldenEyeUs()` swaps it in place first;
  applied to the swapped bytes the xdelta's window checksum fails);
- a zip is looked into for a patch (`archiveFindEntry()` wants the path
  expanded: `$E/...` finds nothing) and unpacked once into `cache/romhacks/`;
- what comes out is converted when `geconvertVariantName()` knows it.

Done once per source: `CONVERT.txt` has the converter's line and `Converted
from <path>`, the path made canonical (`realpath`), since `$E/added-content`
and `./added-content` are the same folder spelt two ways. Two sources of one
hack (its zip and the patch out of it) used to convert over each other on every
start, each finding the folder stamped by the other; a hack is converted once a
start, by whichever source is seen first.

A drop in `mods/` is a Perfect Dark mod to the Mod list, which imported the
patch, found it applies to no Perfect Dark ROM and left an IMPORT.txt saying so.
Now a new drop there - a patch nothing imported yet, an archive nothing
unpacked yet - is tried on GoldenEye's ROM first (`variantAdoptFromMods()`,
before the Mod list is read) and moved to `added-content/` when it makes a
hack, so it converts and registers at the same start. The Mod list's import
asks too (`gexPlusRomPatchIsHack()` in `modListPrepareDir()`), for an archive
unpacked by an older build: it moves the archive, clears what the import and
the unpack wrote, and the hack converts at the next start.

The XBLA importer took `goldfinger64.zip` for the Perfect Dark XBLA release
where the release was not there (and would before it, named with a capital,
where it was): an archive holding a ROM patch is skipped (xblaimport.c).

## The converter: a layout per ROM

GE Editor patches GoldenEye's code in place. What stays where it was: the data
segment (ROM 0x21990, 0x80020d90), the file table (727 rows, the same names -
characters renamed, `CusgruntZ`), the fog, level info and special portal tables,
`multi_stage_setups`, `setup_text_pointers`. What moved is found in
`g_Layouts` (geconvert.c), each table at the address the hack's code loads it
from (the lui/addiu pair at the US code's site for it):

| | US | Goldfinger 64 |
|---|---|---|
| ROM | 12 MB, NGEE | 24 MB, NGFE, needs the Expansion Pak |
| props | 0x8003a228, 340 | 0x8070b400, 416 |
| characters | 0x8003de10, 80 | 0x80700fc0, 126 |
| images | 0x80049300, 8-byte rows, 24-bit size, 2698 from ROM 0x8f7df0 | 4-byte rows, 23-bit size, 3915 from ROM 0xeae714 |

Goldfinger's boot takes 1 MB off `osMemSize` (0x80000318 reads 0x700000) and
copies three pieces of the ROM's tail into it. Its loader was not found; the
pieces are ares's RAM at the title matched against the ROM page by page:
ROM 0x17fd000 -> 0x80700000 (characters), 0x17f8800 -> 0x80703000,
0x17f0000 -> 0x80708000 (props). `romOpen()` grows the data segment over them,
so a pointer into them reads as `ptr - DATA_VRAM` as any other does. The pages
of them that do not match the ROM at the title are model headers the game has
written pointers into since.

ares runs Goldfinger only with the Expansion Pak: `n64twin --expansion-pak`
(tools/gefidelity/ares/twin.cpp; GoldenEye's runs stay 4 MB).

A new hack is a new row: the code sites give its addresses
(`tools/gefidelity` has none for it yet - the sites were paired by a script
in the session, lui/lo at the same PCs in both ROMs), ares its pieces.

## Converting a variant

- Arenas only, for now: the levels are `multi_stage_setups`' rows, read as the
  game reads them (`variantLevels()`): name from LtitleE, bg and tiles files,
  scale and fog visibility from the level info row, setup `Ump_` + the level's
  `setup_text_pointers` name. Goldfinger put its arenas on mission levels
  (Offices is Dam's level 33, China is Bunker 1's with tiles of its own,
  `Tbg_wax`, Tunnels is Surface 2's slot with `bg_lue`), so nothing of
  `g_Levels` is assumed.
- A variant level's key is its bg and level id (`dish38`). No GoldenEye key is
  one, so nothing keyed on GoldenEye's levels reaches it. What is keyed on
  GoldenEye's *file names* - which the hack reuses - is switched off by
  `g_Layout->variant` instead: the later cartridges' setups and portals
  (`revisionSetup()`, `revisionPortals()`) and the Community Edition's fixes
  (`romPatched()`). The special portals are read from the ROM by level id and
  are the hack's own.
- No missions block (it is one list for the whole game, GE Plus's), no folder
  screens, intro, characters, monitors or watch. Their absence logs three
  warnings at a hack's stage (LoptionsE, mission animations, chr animations)
  and the game falls back.
- Textures: the hack's images run past GoldenEye's last, over the numbers
  `g_TexRemap` moves GoldenEye's to, so a variant moves its own at first use
  past its last image (Goldfinger: 31 of the 181 free). `geconvertTexRemap()`,
  which the runtime uses, is always the US table; no image the runtime fetches
  by number is a reserved one.
- `NUM_REMAKE_MODELS` is 1024 (was 340): a stage fills the slots from its own
  mod, GoldenEye's and a hack's side by side.

## At run time

- `modloaderStageIsRemake()` is true on a hack's stage (it has a models block),
  so it gets GoldenEye's rooms, lights and props. `modloaderStageIsGexPlus()`
  is the GoldenEye conversion's alone and is what GE Plus's lists use;
  `modloaderStageInGexPlusList()` is the list of the mode chosen, GoldenEye's or
  the hack's (`g_GexPlusVariant`, set by the Perfect Menu's "Goldfinger 64").
- The model lend (`modloaderLendRemakeModel()`) takes GoldenEye's own only.
- **No XBLA on a hack's stage** (user, 2026-10-02: incomplete textures).
  `xblaSwitchStageHeld()` makes every part's getter answer 0 there; pd.ini and
  the parts' own switches are untouched, F6 says it is off. Each part's file
  read its own `optEnabled` directly in places - all of them go through the
  getter now. And the release's GoldenEye models are matched by file name in
  both looks (`gebeanFindRow()`, and `gebeanPoolRowForFile()`'s remake-file
  branch, which is where Goldfinger's `Pgx030Z` became GoldenEye's console):
  `gebeanFileIsRomHack()` keeps them off a hack's files.

## Open

- The hack's missions (20, its own text, its own AI), its music and sound
  banks (the runtime takes the first remake mod's: GoldenEye's tunes play), the
  folder screens.
- Judged against ares: not yet. The arenas render and play; nothing has been
  compared with the cartridge.
