# GoldenEye HD fit tables

These scripts fit the GoldenEye XBLA release ("Project Bean") onto GoldenEye's
own geometry and write the tables the port uses to draw the HD look over
GE Plus. Moved here from `.xbla-work/ge-bean/` and `.xbla-work/ge-arena/`
(step 1 of `PLANS/GE-CONVERT-DECOUPLE.md`) so the headers can be regenerated
and checked from the repo.

Only scripts and numbers are kept here: fit results, scales, offsets, and
name mappings. Nothing from the release or the ROM is copied. The scripts read
those from the player's own copies, found through `paths.py`.

## Where the inputs are (`paths.py`)

| Variable | What | Default |
|---|---|---|
| `GEFIT_BEAN` | the unpacked GoldenEye XBLA release, the directory holding `files/new`, `files/original` | `<repo>/../.xbla-work/ge-bean/Bean` |
| `GEFIT_GE_DECOMP` | the GoldenEye decomp tree (`assets/obseg/...`, `src/aicommands2.h`) | `~/claude-007/007` |
| `GEFIT_GEX_MOD` | GoldenEye X 6a as the importer unpacked it (`files/`, `segs/data`) | `<repo>/build/mods/GE-X_6a_01-19-25` |
| `GEFIT_GEX_FILES` | GE-X's model/bg files (compressed or inflated) | `$GEFIT_GEX_MOD/files` |
| `GEFIT_GEX_TEXDUMP` | a `--dump-textures` run with GE-X mounted | `<repo>/build/texture-dumps/ntsc-final` |
| `GEFIT_ORIGTEX`, `GEFIT_SCRATCH` | texcompare.py only: cafftool's decode of the release's original/ characters, and where the contact sheet goes | `<repo>/build/origtex`, `<repo>/build` |
| `GE_ROM` | the GoldenEye ROM (US), read by `tools/geconvert/gefiles.py` | `gefiles.ROM` (the decomp's `baserom.u.z64`) |

`paths.py` also puts this directory and `tools/geconvert` on `sys.path`
(`gefiles`, `gemodelconv`, `gerom`), and `tools/texpack` for `cafftool.py`
(`x360`).

Dependencies: Python 3, numpy, scipy (`cKDTree`, used by the fit scripts),
Pillow (texcompare.py, and bean2obj.py/cafftool.py when they write pictures).
The `gen_*` table generators need only numpy.

## Order

```
ROM / release / decomp  ->  fit scripts  ->  *.json (committed)  ->  gen_*.py  ->  port/src/*.h
```

The JSON files are committed, so regenerating a header needs only its
generator and whatever it reads directly (see below). Re-run a fit script only
to change a fit, then its generator.

Generators write the real header by default. `-o PATH` writes elsewhere, and
`-o -` writes to stdout:

```
python3 tools/geconvert/fit/gen_stagetable.py -o /tmp/gebeanstagetable.h
diff /tmp/gebeanstagetable.h port/src/gebeanstagetable.h
```

## The headers

| Header | Generator | Reads | Needs |
|---|---|---|---|
| `port/src/gebeanstagetable.h` | `gen_stagetable.py` | `beanscales.json`, `arenas.json` | nothing else (`gefiles.LEVELS` from the repo) |
| `port/src/geproptable.h` | `gen_proptable.py` | `propfit.json` | the ROM (model group origins, prop names) |
| `port/src/gegunstable.h` | `gen_guntable.py` | `gunfit.json` | nothing else |
| `port/src/gebeanchrtable.h` | `gen_chrtable.py` | none | the ROM (character list), the release (`files/new/char`, `head` listings) |
| `port/src/gebeantable.h` | `gen_beantable.py` | `gemap.json`, `gexassign.json`, `texcompare.json` | GE-X 6a's model files, the release (`files/new/head` models); imports `batch_gex.py`, `bean2obj.py`, `pdmodel.py` |
| `port/include/geaitable.h`, `tools/geconvert/geaitable.py` | `genaitable.py` (historical) | none | the decomp's `src/aicommands2.h`, the repo's `src/game/chrai.c`, GE-X 6a (`geaicmds.py`, `pdaicmds.py`, `gexdata.py`, `gexprops.py`, `n64objsizes.txt`) |

Every header above except `geaitable.h` regenerates byte-identical to the
committed one (checked 2026-09-30). The hand edits made to two of them after
generation are now generator logic: `gunfit2.py` no longer turns the throwing
knife a half turn (f15515659, see `HALF_TURN`; `gunfit.json` refitted), and
`gen_chrtable.py` maps `suit_lf_hand` to the release's `suitlfhand` as a
whole model (f405e7c87, `RENAMED` and `KIND`). `geaitable.h` and
`tools/geconvert/geaitable.py` are kept by hand, well ahead of `genaitable.py`,
which requires `-o OUTDIR` and never writes over the repo's copies.

Hand-written rows in `port/src/gebean.c` (the `Igx001Z` fist, the detonator,
pool rows) are not generated. `fistfit.py` prints the fist's fit. The row is
the fit offset minus GfistZ's root position.

## The fit scripts

| Script | Writes | Reads |
|---|---|---|
| `beancover.py BEAN:KEY ...` | `beanscales.json` (per GoldenEye level key: Bean background, Bean->GE scale, share of GE vertices the HD mesh covers) | the ROM's level geometry, the release's `background/<name>` (both looks) |
| `geconvert.py` with `GE_ARENAS_JSON=arenas.json` (in `tools/geconvert`) | `arenas.json` (per level: level scale, offset the conversion takes off; the `bg` paths in it are the scratch files it was made from and are not read) | the ROM (retired 2026-10-01: geconvert.c is the only converter and `geconvert.py` is gone; `arenas.json` stays as checked in - its scale and offset follow from the ROM alone. `gefiles`, `gerom`, `gemodelconv`, `texremap`, `gesolo` and `geobjects` stay in `tools/geconvert` because these tools and `tools/gefidelity/ai/aimap.py` import them.) |
| `propfit.py [ID ...]` | `propfit.json` (merges the IDs given, all props when none) | the ROM's prop models, the release's `original/prop` |
| `gunfit2.py [--report]` | `gunfit.json` (refits the keys already in it) | the decomp's `assets/obseg/prop/<name>/Model.c`, the release's `new/prop` |
| `fistfit.py` | prints the fist fit | the ROM's GfistZ, the release's `gun/fist` |
| `stagefit.py`, `stagefit_all.py` | `stagefit_all.json` (every Bean level against every GE-X bg) | GE-X 6a bg files, the release's backgrounds |
| `stagefit_refine.py` | `stagetable.json` (refined scales) | GE-X 6a bg files, the release's backgrounds |
| `gemap.py` | `gemap.json` (GoldenEye character -> GE-X files by shared vertex arrays) | the decomp's `assets/obseg/chr`, GE-X 6a model files |
| `texcompare.py` | `texcompare.json` | `gexassign.json`, GE-X texture dump, decoded release textures |
| `batch_gex.py [PACK_N64_DIR]` | the old offline OBJ pack (`build/model-packs/bean/n64`) and `batch_report.txt` | as `gen_beantable.py`, plus `bean2pack.py` |

Helpers: `paths.py`, `cafftool.py` (Rare CAFF reader), `bean2obj.py` (Bean
model reader), `bean2pack.py` (the offline skinning that `gebean.c` now does
at run time), `pdmodel.py` (PD model file walk), `pdbg.py` (PD bg rooms),
`geaicmds.py`, `pdaicmds.py`, `gexdata.py`, `gexprops.py`.

## Known state

- **The Cradle ending's helicopter (282) has a row despite its 0.40 score**
  (`SCORE_EXEMPT` in `gen_proptable.py`, 2026-10-03): every one of Bean's
  vertices lands on GoldenEye's, and the score is low only because
  GoldenEye's body list also holds the pilot, Natalya (1003 of its 1624
  vertices). `gebeanBuildRigid()` keeps those triangles in GoldenEye's look
  inside the HD aircraft and turns the release's rotors about their own hubs;
  see `CLAUDE-notes/ge-bean.md`, "The Cradle helicopter".

- **`propfit.json` is a full run of today's `propfit.py`** (2026-09-30,
  feat/ge-hd-propfit-full): 313 props, 282 table rows. Against the partial
  runs it replaced:
  - 103 props added, 97 of them scoring 0.6 or more. Of those 97, 21 are
    GoldenEye's guns' own props (`GUN_PROPS` in `gen_proptable.py`: 184-187,
    189-196, 199-201, 204-206, 208-210) and are left out: each already has a
    row in `gegunstable.h`, and `gebeanPoolRowForFile()` would find a prop row
    first - the Golden Gun came out silver and half as big again on Egyptian's
    table. The intro's GoldenEye logo (277) is left out too: the release's prop
    took the place of GoldenEye's gold under the Level Metal pass
    (`introLogoMetal()`) with a flat orange ramp.
  - That leaves 75 new rows, and **only one of them is ever drawn today**:
    the thrown covert modem (245, `chrbug`), checked on Surface. The other 74
    (0, 6-9, 37, 39, 47, 52, 54, 56-61, 64, 71, 74, 80, 81, 84, 87, 102, 121,
    123-125, 127-130, 132, 135, 137, 142, 145-148, 151, 153, 154, 156, 157,
    163, 226, 235, 237, 238, 241, 242, 246, 249, 252, 253, 256, 262, 276,
    281, 285, 286, 289, 290, 302, 305, 308, 309, 318, 319, 333, 338, 339) are
    props no setup places, so the conversion writes no `Pgx%03dZ` for them
    (`geconvert.c`'s `allmodels`) and their rows never match. They have not
    been seen drawn: look at any of them before the conversion starts
    writing it.
  - 15 rows refitted, all scoring higher: 12, 13, 14, 16, 17, 83, 92, 94, 108,
    141, 159, 167, 181, 291, 296. The changes are under a unit of the model's
    own space (83's numbers did not change at all); in the HD look each draws
    within a pixel of the old fit, doors opened and shut.
  - The other 195 rows reproduce exactly. Running `propfit.py` with no
    arguments rewrites all of them.
  - The check: `build/propfit-check/` in the worktree the branch was made in
    (`survey.py` lists every object of the twenty missions with its file and
    row; `capture.py` stands in front of chosen props and shoots them in both
    looks; `sheet.py` makes new table | old table | N64 look contact sheets).
- **`gexassign.json` has no generator.** The script that wrote it (each GE-X
  file's best GoldenEye model by vertex overlap) was not in `.xbla-work`.
- **`gunfit.json` is fitted on the decomp**
  (`assets/obseg/prop/<name>/Model.c`), not the ROM. It reproduces exactly.
- **Inputs still derived from GoldenEye X.** GE Plus is built from the ROM
  alone (`PLANS/GE-CONVERT-DECOUPLE.md`); these are not:
  - `gebeantable.h` (`gen_beantable.py`, for GE-X's own character files) and
    its `gemap.json`, `gexassign.json`, `texcompare.json`
  - `stagetable.json` and `stagefit_all.json` (`stagefit_all.py`,
    `stagefit_refine.py`), the GE-X level pairing. No generator reads them.
  - `genaitable.py`, which uses GE-X's converted ai lists as an oracle
  The live stage, prop, gun and character tables (`gebeanstagetable.h`,
  `geproptable.h`, `gegunstable.h`, `gebeanchrtable.h`) take no GE-X input.

## Left out

- Everything from the release, the ROM, Xenia or captures: `Bean/`,
  `BeanCE/`, `xenia/`, `drawlog/`, `folderfit/`, `n64` dumps, `.seg`/`.bin`.
- Backups: `gen_stagetable.py.before-statue`,
  `beanscales.json.before-statue`, `propfit.json.before-*`, plus
  `__pycache__` and `batch_report.txt` (output).
- `gen_gunstats.py`: superseded by `tools/geguns/gen_gunstats.py`, which
  writes `gegunstats.h`/`gegunsfist.h`.
- Exploratory scripts that no table depends on: `beanmatsurvey.py`,
  `beanwinding.py`, `fphand.py` (prints hand boxes) and its
  `fpfit_probe.json`, `gebg.py`, `menudraws.py`, `psdis.py` (Xenia shader
  disassembly), `render_obj.py` (Blender), `uvdensity.py`, `uvprobe.py`,
  `xenosvs.py`. From `ge-arena`: `gemodels.py`, `gemodels.json` and
  `gemodels_extra.json` (GE-X model numbering, retired), `gexobjects.py`,
  `learn.py`, `padcmp.py`, `padfit.py`, `pdmodeldump.py`, `propmatch.py`.
