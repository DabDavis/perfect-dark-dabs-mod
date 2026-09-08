# The Xbox 360 XBLA release: its containers, its textures, its models

The 2010 XBLA release is the N64 game with its art replaced. Its package is a
useful source of high resolution textures and it is where the "improved models"
people ask about live, but the two are nothing like as similar a job as they
sound: the textures drop straight into the pack loader, and the models are a
renderer feature. The unskinned ones draw with the release's own art
(`Mod.XblaMeshes`, `Mod.XblaMeshTextures`); a gun or a character does not yet —
see "The draw path".

Read this before touching `port/src/x360.c`, `port/src/xblaimport.c`,
`port/src/xblamesh.c`, `port/src/xblatex.c` or `tools/texpack/xblaconvert.py`.
Everything below cost a wrong turn.

## What is in the package

`Perfect Dark XBLA.7z` holds one STFS package — magic `LIVE`, title
`584109C2` — with 64 entries. Two of them are the game:

| entry | what it is |
| --- | --- |
| `DataFiles/Textures.raw` (166 MB) | 5747 texture records, LZX compressed |
| `DataFiles/PackedSegFile` (30 MB) | 1590 of the game's own files, plus 596 extra |

The rest is dashboard furniture, avatar awards, fonts and audio
(`pd_sfx_360.tbl` 17.8 MB, `pd_seq_360.tbl` 5.7 MB, `XMASpeech.dat` 12.8 MB —
4J's own encodings, nothing has been done with them). `584109C20AAAAAAA` is a
second STFS package nested inside the first and holds only Xbox dashboard theme
art; it is not a title update and has nothing in it.

## STFS: the volume descriptor is easy to place one byte out

The descriptor at `0x379` is length, version, **block separation**, then the
file table's block count and block number. Reading it as length, separation,
count, number — one byte early — gives a file table of 257 blocks instead of 1,
and the first 64 entries parse correctly before the rest turns into whatever
happened to follow. That looks like a real 12631-entry table with a corrupt
tail, not like a parse error, and the sizes are plausible enough to extract
against. The tell is the total allocated block count at `0x395`: read right it
is 60836 and multiplies out to the file size, read wrong it is billions.

`x360.h` has the offsets. Data starts at `0xC000`; the level 0 hash table for
each run of `0xAA` blocks sits in the `0x1000` bytes before that run, which is
why the first table is at `0xB000` and why the rounded header size being
`0xB000` is what says a package keeps one hash table set rather than two.

## LZX: the window is 17 bits and a chunk is not a stream

Both payloads are `XMemCompress` LZX, and the chunk framing is *outside* the
bitstream: a chunk led by `0xFF` states its uncompressed and compressed size as
two big-endian 16 bit numbers, and anything else is a bare compressed size with
`0x8000` bytes of output. Summing those against the record's stated size is how
the format was confirmed before a single bit was decoded — all 1461 compressed
entries in `PackedSegFile` and all 5747 in `Textures.raw` add up exactly.

Two things that are not guessable:

- **The window is 17 bits.** That fixes 34 position slots and a 528 symbol main
  tree. Every other size fails outright, so a decode error means the framing
  was read wrong, not that the window wants trying at another size.
- **The window, the repeated offsets and the Huffman trees persist across a
  chunk boundary.** Only the bit buffer restarts. Decoding each chunk from a
  fresh context produces a plausible looking first chunk and rubbish after it,
  which is a much more expensive mistake to spot than a hard failure.

## Textures.raw

```
u32 count
count x 52 bytes   metadata
count x 52 bytes   D3DTexture structs
                   data
```

52 bytes is the Xbox 360 `D3DBaseTexture`: seven dwords (`Common` 3,
`ReferenceCount` 1, `Fence`, `ReadFence`, `Identifier`, `BaseFlush` and
`MipFlush` both `0xFFFF0000`) then the six dword `GPUTEXTURE_FETCH_CONSTANT`.
Recognising the second table for what it is, rather than a second metadata
table, is what gives the format and the tiling; the width and height in the
fetch constant agree with the metadata table's, which is how to check.

The metadata record is `{offset, width, height, srcWidth, srcHeight,
uncompressedSize, compressedSize, ...}`. Offsets chain: each is the previous
one plus its compressed size, and the last ends exactly at the file's end —
but only once the data base is taken as `4 + 2 * count * 52`. Taking it as one
table leaves a shortfall of exactly one table's worth, which is the clue.

Format bits worth writing down:

- `data_format` is the low 6 bits of fetch dword 1: 6 is `8_8_8_8` (BGRA on the
  console), 18/19/20 are DXT1/DXT2-3/DXT4-5. The 8888 ones are tiled and
  8-in-32 byte swapped; the DXT ones are untiled and 8-in-16.
- **Pitch aligns to 32 texels for a linear format and 128 for a block one.**
  `pitch` is dword 0 bits 22-30 counted in 32 texel groups, and a block
  format's rows come out a multiple of 128 texels. 202 of the 3503 records have
  a pitch wider than `roundup32(width)` for exactly this reason, and decoding
  them at the texture's own width shears the picture — a 340x512 DXT1 stored at
  pitch 384 is the worked example.
- Mip levels follow the base level in the same buffer. Nothing needs them; the
  base is the prefix and the rest is left off the end.

### Record index is the texture number, for the first NUM_TEXTURES only

There is no matching step and no checksum to reproduce — the release kept the
game's numbering, so record N is texture N. That is what separates this from an
emulator pack (see `riceconvert.py`, which exists entirely to undo the fact
that an emulator has nothing but a hash to identify a texture by).

The first **3503** records are the replacements, which is `NUM_TEXTURES` for
ntsc-final exactly. The remaining 2244 are the console release's own art — its
dashboard panels, its achievement screens, 1024x150 banners — and have no
texture number to go to. They give themselves away by having `srcWidth`/
`srcHeight` equal to their own dimensions, where a replacement records the N64
size it stands in for.

**Do not read a texture that does not match its stock counterpart as a sign the
index is wrong.** Only about half of them match, and the temptation is to go
hunting for a shift or a drift. There is none: 4J *redrew* a large part of the
environment art rather than enlarging it, so a wall in the pack is often a
different picture from the wall in the ROM. 1470 of the 3503 are true upscales
and the rest are repaints, which is what `--only-upscales` and the page's
"Enlarged Textures Only" exist to separate. What confirmed the mapping was the
unambiguous cases — the starburst, the tunnel, the flames, the blinds, the
Joanna portrait, the bar chart — all landing on their own texture number, plus
the record count being `NUM_TEXTURES` on the nose.

### Row order

The console art is in N64 row order, upside down on screen. A pack file
carrying our own `<texnum>.png` name is expected the right way up and the
loader turns it back over, so the conversion flips — `pngWrite()`'s
`bottomRowFirst`, and `write_image()` in `riceconvert.py` for the same reason
from the other direction. The Joanna portrait at `07f4` is the texture to check
this against; most of the rest are too symmetrical to tell.

## PackedSegFile

`{u32 count, count x 16 bytes, data}`, the record being `{offset,
uncompressedSize, compressedSize, flags}` with a zero compressed size meaning
stored. 1026 of the 2616 slots are unused and hold leftover bytes rather than
zeros, so they need recognising (`usz == 0` and both offsets having an empty
low 24 bits) — the 1590 real ones are perfectly contiguous and end exactly at
the file's end, which is the check.

**Slot *i* is the game's file id *i + 1*.** Confirmed by 906 of the 1590
matching stock's inflated size exactly and by the names lining up. Two things
about how 4J stored them:

- A file that is a plain `1173` blob in the ROM is stored **already inflated**,
  which is why its recorded size equals stock's `1173` header size. Comparing
  the stored bytes against the ROM's *compressed* bytes makes every file look
  different for no reason.
- A file that is a container — a `bgdata/bg_*.seg`, whose 12 byte header is
  three section sizes followed by three `1173` blobs — is kept as it is,
  sections still compressed.

## The models, and why they are not importable as assets

Every `C*`, `G*` and `P*` model file differs from stock, which looks alarming
and is not. The difference is almost always one or two words, and always of
this shape:

```
Pcv_chair4Z  @0007c   0x00180000 -> 0x001808F9    (0x18 = dl node)
GnbomblodZ   @00414   0x00040000 -> 0x000419F5    (0x04 = gundl node)
```

`struct modelnode` is `u16 type` at `0x00` and `rodata` at `0x04`, so bytes
`0x02`-`0x03` are padding. Stock leaves them zero; 4J put a mesh id there.
`filemodel.c` reads `PD_BE16(node->type) & 0xff` and the rodata pointer and
never those two bytes, so importing the XBLA model files into this port is
**harmless and pointless** — identical geometry, ids ignored. A handful carry
real edits as well (LOD distance floats in `Cdark_combatZ`; 81 files changed
size), but that is the exception.

### The id names a slot, and the slot holds the mesh

The id is `(part << 12) | (slot + 1)`, and the slot is a `PackedSegFile` slot in
the **596 extra slots** past the game's file ids — 2012 to 2614, which is why 12
bits are enough. 777 of the 779 ids collected across every changed model file
resolve that way. 542 distinct slots are named.

**The low 12 bits are a file id, not a slot.** Slot *i* is file id *i + 1*
everywhere else in `PackedSegFile` and it is no different here, so the mesh sits
one slot below the number in the node. Reading the number as the slot is a
mistake that hides: every id still lands on a real mesh, and the mesh next door
is nearly always the same kind of thing, so a chair comes back as the chair
beside it and a person as a different person. What settles it is doing it in
bulk. Take every model with one unskinned mesh and compare the box its replaced
nodes occupy against the mesh's: 91 of them match to within 3%, **all 91 at
minus one and not one of them at the id itself**. Rendering says the same — all
eleven `P*chair*Z` models are chairs at minus one, `Cdark_combatZ` is Joanna in
combat gear rather than the evening dress a slot up, and `PnintendologoZ` is the
Microsoft Game Studios logo 4J put in its place.

The **4 high bits are a part index**, not a count of extra files. Take the
nodes of one model file that name the same slot: their high nibbles are a
permutation of 0, 1, ... n-1 in every one of the 545 (model, slot) pairs but
one, and that one is `UsetupdamZ` naming slot 1, which is a setup file rather
than a model and one of the two ids that never resolved. The nibble is less
than the mesh's `matrixCount` for 759 of the 777 ids; the 18 that are not are
all meshes with no matrix palette at all — weapon LODs, doors and windows whose
parts share one unskinned mesh.

`CdrcarrollZ` is the shape of it: 13 nodes, all naming slot 2383, carrying
nibbles 0 to 12, and that mesh is one skinned draw of 15 matrices. So the id is
written into every part of a model rather than into one of them, the mesh is
drawn once, and the nibble is what says which palette entry that part is —
which is the mapping a renderer needs to drive the palette from Perfect Dark's
own skeleton. That last step is a reading that fits rather than something
checked against a running frame.

`0xFFFF` is not an id. It appears 377 times and means the node has no
replacement.

That chain is verified end to end, and it is worth doing again rather than
trusting it: `Pcv_chair4Z` carries `0x0879`, so slot 2168, which exports as a
chair; `Cdark_combatZ` carries `0x0943`, so slot 2370, which exports as Joanna
in combat gear — headless, because Perfect Dark keeps heads in their own model
files and 4J kept that split. `tools/texpack/xblamesh.py --mesh-id` takes the
number as the node carries it and does the subtraction.

### The mesh format

80 MB, **1,518,526 vertices and 706,121 triangles** over the 596 files, 595 of
which parse. It is all decoded now bar two fields, and `tools/texpack/xblamesh.py`
reads it:

```
u32   vertexCount
u32   vertexOffset
u32   indexOffset
u32   drawCount
u32   drawOffset
u32   matrixCount
f32   unknown                            100.0 unskinned, 1000.0 skinned
u32   groupOffset                        == 32 + 48 * matrixCount

f32   matrices[matrixCount][3][4]        row major, translation in column 3
{u32 firstDraw, drawCount, matrixIndex}  groups, up to drawOffset
{u32 firstTri, triCount, material}       draws, drawCount of them
vertex vertices[vertexCount]             stride 36 or 48
u16   indices[]                          to the end of the file
```

A vertex is position (3 floats), **UV (2), normal (3, unit length)**, colour
(one u32). A skinned one adds **two** blend weights, which sum to 1.0 — there
is no third — and a packed `{bone0, bone1, bone2, influenceCount}` byte quad,
whose count runs 1 to 6 against those two weights; see "Skinning, and why it is
not on" below. The two strides are the two forms of the same thing and nothing
else: stride 36 always comes with `matrixCount` 0, stride 48 always with a
palette, 318 files and 277.

The **material** word is a `Textures.raw` record index in bits 0-12, bit 15 set
when that record has an alpha channel, and two bytes above that whose meaning
is not known — bits 16-23 hold a value from 0 to 100 and bits 24-31 a small
ordinal 0-4 that is zero on all but 15 of the 761 distinct materials.

Two things about those texture numbers. They run **3741 to 5746**, so a mesh
never draws with one of the 3503 numbered replacements: it draws with the
console's own art, the records `xblaconvert.py` deliberately leaves out of a
texture pack. And bit 15 is exactly the records whose format carries alpha —
DXT2-3, DXT4-5 or 8888 with something other than 255 in it — against DXT1 for
the rest, which is what says it is a flag and not part of the number. Over the
646 records the meshes actually use, no record is ever named both ways: every
DXT2-3 and DXT4-5 carries the flag, 8888 splits 4 against 12, and two of the
577 DXT1 records carry it as well — DXT1 has a block form of its own that
holds one bit of alpha, so a mesh does ask for `G_RM_AA_ZB_TEX_EDGE` on a
format the paragraph above calls opaque.

**The matrix palette is not a transform to apply to the vertices.** They are in
the mesh's own space already; multiplying them through folds a character in
half. The palette is what a skinned draw needs alongside a pose, and a still
render wants none of it.

#### How this was confirmed

Every claim holds over all 595 files at once, which is what separates it from a
plausible read of one file:

- `vertexOffset == drawOffset + 12 * drawCount` and
  `groupOffset == 32 + 48 * matrixCount`, in all 595.
- The draws' `triCount` sums to **exactly** the file's index count in all 595,
  and their `firstTri` chains from 0 with no gap and no overlap.
- The groups tile the draw list exactly — first group at draw 0, each starting
  where the last ended, the last ending at `drawCount` — in all 595.
- Every group's `matrixIndex` is less than `matrixCount`. One file has
  `0xFFFFFFFF` there and no matrices at all.
- Rendering each draw with its own texture at the decoded UVs gives
  recognisable, correctly placed art: shoes on the feet, hair on the head, a
  screen on the laptop, a keyboard under it. A wrong submesh split, material
  field or UV pair gives a smear instead, which is how the earlier guesses at
  `materialish` gave values like 0, 1, 17 and 35525629.

The blocker that used to be here — "a region of 12 byte entries that is a mix,
some `{firstIndex, indexCount, materialish}` and some plainly float matrices" -
was the 56 byte header being 24 bytes too long. The last six words of it are
the first two table entries, and the matrices are a table of their own that the
header measures. Counting the region from 56 mixed the three tables together,
which is why only 23 of 80 sampled files appeared to chain.

#### What is still open

- The float at `+0x18`. 100.0 on every one of the 318 unskinned meshes, 1000.0
  on 267 of the 277 skinned ones, 200.0 twice and 750.0 once. It does not track
  the mesh's size, so it is not a bounding radius.
- The material's top two bytes, above.
- How the palette is meant to be read. A group is one part of the model and
  names a palette entry — see "Skinning, and why it is not on" below — but what
  an entry's matrix is relative to is not established.

### The draw path

`port/src/xblamesh.c` draws them, behind `Mod.XblaMeshes` in pd.ini. It reads
`PackedSegFile` out of the package, matches a model's nodes against the
release's copy of the same file as the model loads, and builds one Perfect Dark
display list per mesh the first time something asks to draw it. There is no new
renderer code at all: the list is ordinary F3D, so the cull modes, the depth
sorting, Model Smoothing and screenshots all carry on working.

Four things it costs a wrong turn to work out again.

**The geometry is the model's own coordinates, 1:1.** An Area 51 crate is 100
units across in the ROM and 100 in the mesh; a lab door is 4000 by 2800 in both.
So a mesh wants no transform of its own — it goes under the node's matrix like
the display list it replaces — and the floats quantise back to the s16 the
game's vertices already are with nothing lost. The in-game check is
`--xbla-mesh-verbose`, which logs each replaced node's stock box beside its
mesh's; they should sit on top of each other, and when the id mapping was one
out they did not.

**A batch is 25 vertices, and the list must not hold a pointer until the arrays
stop growing.** `gSP1Triangle` multiplies its indices by 10 into a byte, so 25
is the most one `gSPVertex` load can be indexed past — a lie the RSP would
refuse and the renderer does not care about. The vertex and colour arrays grow
as the triangles are walked, so a `G_VTX` written when a batch closed points
into memory `realloc` then moves. That draws as one enormous polygon across the
screen and looks exactly like a scale or a transform problem. The batches are
remembered by index and their addresses filled in at the end.

**A colour table is 64 entries and every vertex has its own colour.** A Perfect
Dark vertex names its colour by a byte offset into a table, so one table holds
64 and the release colours every vertex separately: the table is per batch,
which always fits. The renderer keeps one table pointer with nothing to save or
restore it, so the list puts it back to a table of white at the end — white
being the one wrong answer that cannot darken what draws next.

**Only the first part draws.** One mesh replaces a whole model and every part
of the model carries its id, so drawing at each of them draws the model over
itself once per part, each copy under a different bone. Parts past the first
return without drawing and without falling back to the game's geometry.

One thing that reads as a bug and is not: a mesh can cover much more of the
screen than the geometry it replaced. One node is shared by every instance of
its model, so a mesh drawn twice the size is usually two of them — the G5
Building has two car lift doors in the same view. `--xbla-mesh-verbose` names
the first dozen draws with the model each came from, which is what says which
of the two it is; it also logs each replaced node's authored box against the
box the game is actually drawing from, since the game moves some nodes'
vertices at runtime and a mesh in that node's place would not be moved with
them (no door does, as it turns out).

A node's own list is what loads the matrix that says where the node is, out of
segment 3, and this one is drawn in its place — so it loads it too. Without
that a mesh inherits whatever matrix the node before it left behind, which for
a one-part prop is near enough right to look like a scale problem rather than a
missing transform, and for anything else is not.

What works: unskinned props. A crate, a door, a dumpster, a lift door all land
in the right place at the right size, and a model the release replaced only
part of comes out right — a G5 lab door draws its stock frame around the
release's panel, because a node whose id is `0xFFFF` keeps its own geometry.

#### The textures

`port/src/xblatex.c` draws them with the release's own art, behind
`Mod.XblaMeshTextures`, which is on: an untextured mesh is a flat pale solid
and is not what anyone turns `Mod.XblaMeshes` on to see. Turning it off is how
to tell a shape that is wrong from a texture that is.

A mesh's materials name `Textures.raw` records **3741 to 5746** — past the 3503
that carry a texture number — so nothing that goes by number can reach them and
no texture pack can ship them: `xblaconvert.py` deliberately leaves them out.
What the renderer does have is the address a display list binds, which is what
`texpackLoadReplacement()` is keyed on. This plays the same trick one registry
along. 646 distinct records are used across all 595 meshes, so what this can
ever hold is that many stand-ins and not the 2006 the range suggests.

**A material's texture is a stand-in tile whose address is its name.** The list
binds a 32x32 RGBA16 buffer that means nothing, `xblatex.c` remembers which
record that address stands for, and `import_texture()` swaps the decoded
picture in against it — ahead of the pack lookup, since a stand-in is not a
texture any pack has an opinion about. The stand-in's own texels are never
read; they are white so that a record which fails to decode draws pale rather
than invisible.

**The tile is 32 texels because of what a vertex can hold, not because of the
picture.** A Perfect Dark vertex measures s and t in texels of the tile as 10.5
fixed point and the renderer normalises them by the tile, so a 512x512
replacement on a 32x32 tile is the ordinary texture pack case and nothing has
to know the real size. What the tile size does decide is the range and the
precision of a UV: at 32 texels a UV of one is 1024, so a coordinate runs to 32
before it overflows the s16 and is good to about a thousandth. `vtx->s = u *
XBLATEX_TILE_SCALE` is the whole of the mapping.

**The rows are not flipped.** The console stored its art in the same order the
game's own texture data uses, so what `x360DecodeTexture()` produces is what
the renderer uploads. The `[::-1]` in `xblamesh.py` and the flip in
`xblaconvert.py` are both for the PNG's benefit and have no place here — doing
it "to be consistent with the converter" turns every picture upside down.

**A draw is one material, and the batch has to close at the boundary**, because
a vertex load and the triangles indexing it belong to the state they were
written under. That turned up a latent bug in the batch code: an empty batch
still reserved two commands at its head for the vertex load and the colour
table, and nothing wrote them if it closed without a vertex. With one batch per
list that never happened; with one per material it happens wherever two
materials meet. `xblaMeshCloseBatch()` now takes the reservation back.

The state a material writes is the combiner, the render mode, the texture
switch and one `gDPLoadTextureBlock`: `G_CC_MODULATERGBA`, because the release
colours every vertex and the shade is doing work rather than being a flat
white, and `G_RM_AA_ZB_TEX_EDGE` for a material whose alpha bit is set, since
this only draws in the opaque pass and a cutout there wants the alpha compare
rather than the blender. The end of the list puts all of it back, which the
untextured version did not have to do.

**A decode is one LZX stream and it happens on the render thread**, under the
lock that also covers the registry, because the meshes are built on the game
thread. Nothing is decoded twice unless the renderer's texture cache evicts it,
so the pair of numbers logged at shutdown — `N records bound, M decodes` — is
what says whether a room is paying for its textures once or every frame. Over
2000 frames of Chicago, Extraction and Air Base, M never exceeded N (15, 28 and
12 against 15, 28 and 14), so nothing has needed a cache of decoded pictures
yet.

What confirmed it: the same view of the G5 car lift with `Mod.XblaMeshTextures`
on and off. Off, the lift door behind the guard is a white slab; on, it is the
release's rusted panel with its red and white hazard bar, at the right size and
the right way up. Props stay pixel-identical between two seeded runs of each of
the three levels, which is the check the untextured version passed too.

#### Skinning, and why it is not on

`Mod.XblaMeshPose` poses a skinned mesh from the game's matrices. It is off,
because the release's palette is not Perfect Dark's skeleton in a space this can
use. What the attempt established, which is worth not finding out again:

- **A vertex has two weights, not three.** The two floats after the colour sum
  to 1.0 across every skinned vertex in the release, so the third weight the
  layout looked like it had is not there. The packed byte quad is three bone
  indices and a count; the count runs 1 to 6 while only two weights exist, and
  a rigid vertex repeats its bone in all three bytes — `{33,33,33,1}`.
- **A group is one part of the model.** A model's parts and its mesh's groups
  come in the same order and the same number — `CdrcarrollZ` has thirteen of
  each, a Falcon 2 five — and the group's third word is a palette entry.
- **The part number is not the palette entry, and neither is Perfect Dark's own
  matrix index**, although both look like it. A Falcon 2's nodes load matrices
  33, 36, 38, 40 and 42 against a palette of 43; Dr Carroll's load 0 to 3
  against a palette of exactly 4; and over every model that names a mesh, 242 of
  243 have every node's matrix index inside the palette. That is a coincidence
  of ranges. Posing under that assumption gives transforms whose **rotations
  cancel to an exact identity and whose translations are tens of units out**,
  and the numbers say why: the release's bind translations are zero for most
  entries and a unit or two for the rest, where the game's skeleton has those
  bones tens of units apart. The palette is local to the mesh; the game's
  matrices are a pose in the world. Getting from one to the other needs whatever
  hierarchy the mesh keeps its bones in, and nothing found so far says where
  that is.
- `--xbla-mesh-verbose` prints, per palette entry, the bind translation the file
  holds beside the one that would make that entry come out as the identity.
  Those two columns are what the paragraph above is read off, and they are the
  first thing to look at again.

### The level files were rewritten too

62 `bgdata/bg_*.seg` files changed size, some enormously — `bg_arec.seg` goes
from 20656 bytes to 72291. Those are not covered by anything above and have not
been looked at; whatever 4J did to the level geometry is a separate job from the
models.

## How the two conversions are kept honest

There are two implementations of the same thing on purpose:
`tools/texpack/x360.py` plus `xblaconvert.py` offline, and
`port/src/x360.c` plus `xblaimport.c` in the game. The Python one was written
first and checked against the game's own texture dumps by eye; the C one was
then checked against it — byte-identical CRCs over all 1590 `PackedSegFile`
entries, and identical PNGs over the pack. **Keep them agreeing.** The one
place they nearly diverged is 565 expansion: `r * 255 / 31` (what `dxt.c` has
always done for Glide packs) against `r << 3 | r >> 2`, which differ by a level
at some inputs. Both are defensible; the point is that only one of them is in
both files.

`--xbla-import` runs the in-game conversion headlessly and exits, which is the
only way to exercise it without driving the menus:

```sh
./build/pd.x86_64 --savedir /tmp/pdsave --xbla-import --no-sound --log
```

3503 textures in about 24 seconds. `Mod.XblaPackage` says where the package is;
failing that the game's data directory, the executable's directory and the save
directory are searched, by name and then by looking for STFS magic — a package
is named after a content id hash, so it cannot be found by name alone.
