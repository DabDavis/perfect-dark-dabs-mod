# Model packs and the asset dump (2026-09-11)

Files: `port/src/objmesh.c` (the mesh in the middle, OBJ in and out),
`port/src/modelpack.c` (the pack folder, the list, what a pack has for a file
id, and binding a picture of the pack's own), `port/src/assetdump.c` (the
one dump of everything), and the model pack halves of `port/src/xblamesh.c`
and `port/src/xblatex.c`. The player-facing description is in README.md
("Model packs", "Dumping everything", "The XBLA release").

## The directories

```
xbla/                          only what the player put there (the .7z or the package)
cache/xbla/                    the archive unpacked, once - used to be xbla/.unpacked/
texture-dumps/<romid>/         the whole texture table; was texturedump/
texture-dumps/<romid>/xbla/    every Textures.raw record
model-dumps/n64/<name>.obj     every C*, P* and G* file in the ROM
model-dumps/xbla/<name>.obj    every mesh in the package, named for the model that names it
model-packs/<pack>/n64/        replacements for the ROM's models, same names
model-packs/<pack>/xbla/       replacements for the release's meshes, same names
```

All of them are `fsChooseOutputDir()` choices - beside the executable, or in
the save directory when that is not writable. The old `xbla/.unpacked/` and
`texture-packs/.xbla/` are still read and never written.

## One row does all of it

**Dump All Assets To Disk** on the Texture & Model Packs page (the old "Dump
Textures To Disk" checkbox is gone from the menu; F7 still toggles the live
dump of what the game draws, bound as "Dump Drawn Textures") and
`--dump-assets` (also `--dump-textures`, which now means the same) run
`assetdump.c`: a state machine of four passes, stepped from `pdsched.c`'s
frame tick under a 12ms budget so the menu stays alive, or to completion at
startup from `pdmain.c` (after `xblaImportInit()`, since two passes need the
package) followed by `exit(0)`.

The four passes are the whole of each table rather than what was drawn:
`texpackDumpTextureNum()` over `NUM_TEXTURES` (the body of the old
`--dump-textures` loop, split so it can be stepped), `xblaTexDecodeRecord()`
over `xblaTexGetNumRecords()` records written by `texpackWriteXblaRecord()`
(the F7 record writer without its gate), every file id whose name starts
with C, P or G through `assetDumpModel()`, and every package slot through
`xblaMeshSlotToObj()`. The two XBLA passes run only with a package, and
opening it unpacks the archive if that has not happened.

## Dumping a Perfect Dark model without loading it

`assetDumpLoadModel()` loads the file into a buffer of its own with
`fileLoadToAddr()` and promotes the pointers, and stops there: no texture
loading, no `modeldef0f1a7560()`. Three things about that state that the
code does not say:

- **The converter is fatal on a non-model** (`MODEL_CHECK` in
  filemodel.c calls `sysFatalError`), so the file is loaded once with
  `g_LoadType = LOADTYPE_NONE` (raw, no conversion) and its big-endian
  header is checked by `assetDumpLooksLikeModel()` before it is loaded again
  as a model. Gun files (`G*`) are `LOADTYPE_GUN`.
- **The list pointers are not promoted.** `modelPromoteNodeOffsetsToPointers()`
  makes `rodata->dl.vertices` real and sets `colours`/`baseaddr` to the file
  base, but `opagdl`/`xlugdl` stay segment 5 addresses (`0x05xxxxxx|1`) -
  the game's texture rewrite is what replaces them. `assetDumpResolveGdl()`
  turns one into `buffer + offset`; the first run crashed on this.
- **The texture command is still the ROM's compact one** (`G_NOOP` 0xc0,
  texture number in the low twelve bits of w1), which is exactly what makes
  the model worth reading in this state: after the rewrite the number is
  gone. `G_VTX` addresses are segment 4 (the node's own vertex array) for DL
  nodes and segment 5 (the file) for GUNDL nodes, which set no segment 4.
  A texture with `tex->unk0c_03` has its s/t halved at load
  (`texLoadFromGdl()`), so the dump halves them too; UVs are normalised by
  `texpackTexGetPaddedSize()`, the padded row the PNG was written with.

## What names a mesh, and the 39 that nothing names

A mesh slot is named by the model whose nodes carry its id
(`xblaMeshSlotModelFile()`, which walks the release's copy of every C, P and
G file once and files each id it finds against that file). 556 of the 595
meshes are named that way. The other 39 are named for **where they sort**,
`Ghand_a51guardZ+1` being the first mesh after `Ghand_a51guardZ`'s, because:

**The mesh table is in the model names' own order.** Taking each named slot
from 2021 to 2615 in turn gives a sorted list - the props, then the
characters, then the guns, and inside a group the name uppercased with the
ROM's trailing `Z` dropped (so `Pa51wastebinZ` before `Pa51_crate1Z`, `_`
sorting after `Z`; `Pg5_chairZ` before `Pg5_chair2Z`; `CheadjonZ` before
`CheadjonathanZ`). That holds for all 556 with no exception, so 4J built the
table by walking their model list in order.

Which says what the 39 are. They are not leftovers and not a matching
failure: they are meshes for names this ROM does not have - six more heads
between `Cheadelvis_gogsZ` and `Cheadfem_guardZ`, eleven more pairs of hands,
a dozen props - so 4J's build had models the NTSC ROM has not. Fourteen of
them are byte for byte a named mesh's geometry (`Ghand_presidentZ+2` is
`Ghand_trentZ`'s), which is what a second model of the same body looks like.
Nothing in the game can reach any of them, since the only thing that names a
mesh is the id on a model's nodes, and a model pack cannot replace one
either - `xblaMeshBuild()` asks `modelpackFindXbla(fileid)` and their fileid
is 0. They are dumped to be looked at, and each one's OBJ says so.

Two things that are **not** the reason for them, both checked: no model of a
name outside C/P/G names one (walking all 995 model records rather than the
named ones finds nothing more), and only one C/P/G file, `PEXPLOSIONBIT`, has
no copy in the package at all.

## The two conventions an OBJ can be in

An XBLA mesh is **one piece in the model's space** (groups are parts, all
drawn under the first part's matrix, a skinned one posed by its palette). A
Perfect Dark model is **a piece per list node in the node's own space**, each
drawn under its own node's matrix. The dump makes an N64 model viewable by
adding each node's rest offset (`xblaMeshNodeRestOffset()`: the position
nodes above it, summed, the way `modelNodeGetModelRelativePosition()` does
for an instance) and names the groups `node0..` in
`xblaMeshEnumListNodes()`'s order - the game's own depth-first walk, taken
right after promotion in both the dump and the loader so the two agree (the
walk changes later, when `modelCalculateRwDataIndexes()` rewrites the
DISTANCE/TOGGLE children). The loader subtracts the same offsets, giving a
vertex two groups share a copy for each. So a file from `model-dumps/n64/`
belongs in a pack's `n64/` and one from `model-dumps/xbla/` in its `xbla/`;
they are not interchangeable.

## How a pack's file is drawn

An OBJ is read (`objmeshRead()`) and **written back out in 4J's own layout**
(`xblaMeshFromObj()`), then goes through `xblaMeshBuildFile()` like one of
the release's - that was the cheapest way to inherit everything worked out
for those (xbla.md: lighting, the translucent and fading spans, the door
trim, the pose). Two extensions to the builder for it:

- **A material table.** A material word with `XBLAMESH_MAT_TABLE` (bit 30)
  names an entry of the build's `struct xblameshmats` rather than a record:
  a stand-in tile from `xblaTexBindImage()`, which holds a picture of its
  own (a pack's PNG, or one of the ROM's textures decoded through
  `modelpackDecodeN64Texture()` - the texture pack's picture first, by
  `texpackDecodeReplacementNow()`, else the ROM's flipped back to the game's
  row order). Such a tile is not subject to Mod.XblaMeshTextures and cannot
  be replaced by a texture pack (`xblaTexLoadReplacement()` hands the
  picture over before it asks). A material with no picture is a table entry
  with a NULL tile, which draws white times shade.
- **`local` meshes.** A pack's file for an N64 model is registered by
  `xblaMeshRegisterPackModel()` (after the release's matching, and it takes
  the model over from it): every list node is part k of a mesh keyed on the
  *file id*, with `e->pack` set, built by `xblaMeshBuildPack()` into a
  per-file-id table (`packBuilt`) rather than the per-slot `built[]`, and
  drawn with `m->local`: group k under node k's own matrix, `use` ignored,
  and a node the file has no group for (`groupabsent`) keeps its own
  geometry. These need no package and no Mod.XblaMeshes - the render hook's
  gate is now after the lookup. They are **freed at `xblaMeshResetModels()`**
  (level end), unlike the release's meshes: node-local to a model in the pool
  being handed back, and their pictures come through the texture pack.

A pack replacement for one of the release's meshes goes through
`xblaMeshBuild()` as before, with `modelpackFindXbla(slot + 1)` asked first;
the original is parsed too (`xblaMeshFileToObj()`) so a skinned one lends its
palette and scale and **every OBJ vertex takes the bones of the nearest
original vertex** - OBJ carries no skinning, and a character re-exported from
Blender has lost its weights. Brute force, tens of milliseconds a body.

`XBLAMESH_MAXPARTS` went from 16 to 64 for this: a character body is thirty
list nodes. A release mesh with more than 16 groups used to be built as one
group; now it is built as its groups and, not matching the model's parts,
still draws through `allgfx` from part 0 - the same picture.

## Switching packs

A pack change (`modelpackGetGeneration()`) is noticed at the next build: a
mesh from the old pack, or a slot the new pack has a file for, is started
again and **its old lists are leaked, not freed** (`xblaMeshDropStale()`) -
the render thread may be in one. A swap is rare and a mesh is a few hundred
KB. The menu says a pack takes effect at the next level, which is when a
model is registered again; what a level has already built stays.

## Recipes

```sh
# everything, headless, a few minutes
cd build && xvfb-run -a ./pd.x86_64 --dump-assets --savedir /tmp/pdsave --no-sound --log

# make a pack out of the dump and see it drawn
mkdir -p build/model-packs/test/n64 build/model-packs/test/xbla
cp build/model-dumps/n64/Pcrate.obj build/model-dumps/n64/Pcrate.mtl build/model-packs/test/n64/
# pd.ini: Mod.LoadModels=1 and Mod.ModelPack=test under [Mod]
```

`--xbla-mesh-verbose` logs `model file N comes from <path>` and `<what>
built` lines for pack meshes the same as for the release's.
