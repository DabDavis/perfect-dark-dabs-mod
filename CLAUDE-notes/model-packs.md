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
  own (a pack's PNG), or from `xblaTexBindTexture()`, which holds one of the
  ROM's textures *and the number it is*. A tile of the first kind cannot be
  repainted by a texture pack; one of the second is, at every fill of the
  renderer's cache: `xblaTexLoadReplacement()` asks
  `texpackDecodeReplacementNow()` for the number before it hands the kept
  picture over, so what is bound at build time is deliberately the ROM's own
  picture and never the pack's. Neither kind is subject to
  Mod.XblaMeshTextures, which is about the release's art. A material with no
  picture is a table entry with a NULL tile, which draws white times shade.

  **Which materials are numbers at all** is `objClassifyMaterial()`, and it is
  the other half of this: the dump writes both a name (`n64_050a`) and a
  `map_Kd` pointing out of the pack at `../../texture-dumps/...`, and taking
  the `map_Kd` literally - which it did until 2026-09-11 - made *every* dumped
  material a picture of its own, bound once and never asked about again. A
  `map_Kd` that leaves the model's own folder is a reference to something the
  game has, and the name is what the material draws with; one that stays
  inside the folder is the author's own picture and still wins.
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

## Switching packs, live

Everything about a pack is decided at the draw and not at the model load, so
choosing a pack, switching packs off and changing the preference are all on
screen on the next frame.

**Both halves of a node are filed as the model loads.** A
`struct xblameshentry` carries the matcher's side (`slot`, `part`, `use`,
`suppress`, `matched`) and the pack's (`fileid`, `packpart`, `packuse`) at
once, written by two passes of the same load through `xblaMeshEntryFor()` -
which starts an entry from nothing when it finds it belongs to some other
model, and leaves the other half alone when it does not. `xblaMeshUseFor()`
takes a `pack` flag as part of its key, or a model whose file id happens to
equal its mesh slot would find the other one's parts.

The pack's side is filed for **every** model that loads, not only the ones a
pack can replace, because the load is the only moment a model's tree may be
walked: a modeldef is freed and reused inside a stage, so a register of loaded
models to go back over when the pack changes is the crash xbla.md describes.
It is gated on `modelpackHavePacks()` - anybody with nothing in `model-packs/`
files nothing and pays nothing, and the draw path's `g_XblaMeshNumNodes` test
costs what it always did. With a pack installed, Chicago goes from 111 filed
nodes to 209, of 16384.

`xblaMeshRenderNode()` then asks, per node per frame: the pack's file
(`modelpackFindN64()`, a table lookup) unless `Mod.ModelPackPrefer` is
`MODELPACK_PREFER_XBLA` and the model has a mesh. **The model's, not the
node's** - `packhasmesh` is filed on every one of a model's entries from
whether the matcher, which has just run, found any of them a mesh. Keyed on
the node instead, the preference left a matched model's *unmatched* lists (a
far LOD alternative) still drawing the pack's geometry inside the release's
mesh, which is neither of the two things being chosen between.

The mesh side's suppressions - the hair, the covered lists - belong to the
mesh and are skipped whenever the pack's file is what draws.

What a *change* costs: a mesh from the old pack, or a slot the new pack has a
file for, is started again at its next build (`modelpackGetGeneration()`,
`xblaMeshDropStale()`) and **its old lists are leaked, not freed** - the
render thread may be in one. A swap is rare and a mesh is a few hundred KB. A
pack mesh that would *not* build is dropped rather than leaked, so the next
pack's file for the same model is not refused for the last one's sake.

`modelpackReload()` - the texture packs' reload key (F9) and the menu's Reload
Pack row, which now do both - takes the folder list and the pack's files again
and bumps the generation, which is how an OBJ edited in a modeller is looked
at without leaving the level.

The one thing that is still not live is a pack folder appearing in
`model-packs/` *while a level runs* on a machine that had none at all: nothing
was filed as those models loaded. The next level has it.

## Recipes

```sh
# everything, headless, a few minutes
cd build && xvfb-run -a ./pd.x86_64 --dump-assets --savedir /tmp/pdsave --no-sound --log

# make a pack out of the dump and see it drawn
mkdir -p build/model-packs/test/n64 build/model-packs/test/xbla
cp build/model-dumps/n64/Pcrate.obj build/model-dumps/n64/Pcrate.mtl build/model-packs/test/n64/
# pd.ini: Mod.LoadModels=1 and Mod.ModelPack=test under [Mod]

# something always on screen to look at: the player's gun, in a Combat Sim
# match, with Mod.StartArmed=1 in pd.ini
cp build/model-dumps/n64/Gfalcon2Z.obj build/model-dumps/n64/Gfalcon2Z.mtl build/model-packs/test/n64/
cd build && xvfb-run -a ./pd.x86_64 --savedir /tmp/pdsave --skip-intro --no-sound \
    --boot-stage 0x1f --mpsims 1 --fixed-step --rng-seed 1 --screenshot-frame 400 --exit-frame 420 --log
```

The live half is driven against a running game, and every one of these shows
up in the log (a rebuilt pack mesh says `comes from`):

```sh
gdb -p $(pgrep -x pd.x86_64) -batch \
    -ex 'call (void)modelpackSetLoadEnabled(1)'   # packs on, mid level
    -ex 'call (void)modelpackSetPrefer(1)'        # the XBLA mesh instead
    -ex 'call (void)modelpackSetSelectedPack(0)'  # another pack
    -ex 'call (void)modelpackReload()'            # re-read an edited OBJ
    -ex 'call (void)texpackSetLoadEnabled(0)'     # repaint it, with no rebuild
    -ex 'call (void)screenshotRequest()'
```

That a texture pack repaints a pack's mesh is tested with a pack of solid
magenta PNGs named for the numbers in the model's own `.mtl`: the magenta
lands on the model, and the frame diff against the same seeded frame with
`Mod.LoadTextures=0` is the gun and nothing else.

`--xbla-mesh-verbose` logs `model file N comes from <path>` and `<what>
built` lines for pack meshes the same as for the release's.
