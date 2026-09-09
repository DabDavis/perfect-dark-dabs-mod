# The Xbox 360 XBLA release: its containers, its textures, its models

The 2010 XBLA release is the N64 game with its art replaced. Its package is a
useful source of high resolution textures and it is where the "improved models"
people ask about live, but the two are nothing like as similar a job as they
sound: the textures drop straight into the pack loader, and the models are a
renderer feature. They draw with the release's own art and, when they are
skinned, in the pose the game has put their model in (`Mod.XblaMeshes`,
`Mod.XblaMeshTextures`, `Mod.XblaMeshPose`) — see "The draw path" and
"Skinning".

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
f32   scale x 100                        100.0 unskinned, 1000.0 skinned
u32   groupOffset                        == 32 + 48 * matrixCount

f32   matrices[matrixCount][3][4]        row major, translation in column 3
{u32 firstDraw, drawCount, matrixIndex}  groups, up to drawOffset
{u32 firstTri, triCount, material}       draws, drawCount of them
vertex vertices[vertexCount]             stride 36 or 48
u16   indices[]                          to the end of the file
```

A vertex is position (3 floats), **UV (2), normal (3, unit length)**, colour
(one u32). A skinned one adds **two** blend weights and a packed
`{bone0, bone1, bone2, count}` byte quad. **The third weight is the one that is
left**: the two stored ones sum to 1.0 on 93% of the release's skinned vertices
and to as little as 0.5 on the rest, so `1 - w0 - w1` belongs to `bone2` and
dropping it pulls those vertices towards the origin. **The fourth byte is not a
count of influences.** It runs 1 to 6 against three bones, it is the same value
for every vertex of a draw, and its 2s carry three real influences as often as
its 3s do — 4.4% of them repeat bone0 in bone1, against 95% that repeat bone1
in bone2, which is how a vertex with fewer than three bones is written. All
three always apply and the repeats collapse themselves; see "Skinning" below.

The two strides are the two forms of the same thing and nothing else: stride 36
always comes with `matrixCount` 0, stride 48 always with a palette, 318 files
and 277.

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
half, because what the palette holds is each bone's *inverse* bind. It is what
a skinned draw needs alongside a pose, and a still render wants none of it.
"Skinning" below is how it is read.

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

- The material's top two bytes, above.

The float at `+0x18` and the palette are answered under "Skinning" below, and
the heads under "The heads".

### The draw path

`port/src/xblamesh.c` draws them, behind `Mod.XblaMeshes` in pd.ini. It reads
`PackedSegFile` out of the package, matches a model's nodes against the
release's copy of the same file as the model loads, and builds one Perfect Dark
display list per mesh the first time something asks to draw it. There is no new
renderer code at all: the list is ordinary F3D, so the cull modes, the depth
sorting, Model Smoothing and screenshots all carry on working.

Four things it costs a wrong turn to work out again.

**The geometry is the model's own coordinates, at the header's scale.** That
scale is 1 for everything unskinned: an Area 51 crate is 100 units across in
the ROM and 100 in the mesh; a lab door is 4000 by 2800 in both. A skinned mesh
is nearly always at a tenth, and drawing one without the scale puts a character
inside its own chest — see "Skinning".
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

**A part draws its own group, and only its own.** One mesh replaces a whole
model and every part of the model carries its id, so a list for the whole mesh
drawn at each of them draws the model over itself once per part — which is
what turned a room into overlapping sheets, and why this used to draw at part 0
and nothing at the rest. The list is per group instead: a group is one part, in
the same order and the same number, and it holds in all **542** (model, mesh)
pairs the release has, with the parts numbered 0..n-1 and no gaps. So the node
carrying part p draws group p.

That is what lets a piece the game has hidden stay hidden — a head's earpiece
or its sunglasses are a part of their own under a toggle — and it is also what
draws a part whose *first* part is hidden: the golden Magnum's hand was missing
until this, because the hands model's part 0 is not always the one the game
walks. A model whose parts do not line up with the groups, which is nothing in
the release but could be a mod's, falls back to the old rule through a list
that calls every group in turn.

Every group draws under the **first part's matrix**, whichever part is drawing.
A group is not in its own part's space: a door's window pane is where the door
has it, all five multi-part meshes in the release that are not skinned load one
matrix for every part anyway, and a posed mesh comes out in the first part's
space by construction. And since a part now draws on its own, the posed copy is
remembered for the frame and the model it was made for, so Dr Carroll poses
once a frame rather than thirteen times.

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

**A texture pack goes on underneath both, and never has to come off.** The two
cannot collide by construction: a mesh's materials name records 3741 to 5746,
past the 3503 that carry a texture number, and what its list binds is a
stand-in that no pack can name either. `import_texture()` tries the stand-in
registry first and falls straight through to the pack when the address is not
one of ours, so a level draws its own art from the pack and its meshes from the
release in the same frame. The check is the G5 Building with the *PD XBLA* pack
selected: meshes off, the lift door is the stock door wearing the pack's
picture of it; meshes on, it is the release's panelled door - and the hazard
bar on the frame beside it is the pack's either way.

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

**The rows are not flipped, and `v` is.** These are one decision and getting
half of it right is worse than getting neither, so they are written down
together.

The console stored its art in the same order the game's own texture data uses,
so what `x360DecodeTexture()` produces is what the renderer uploads. The
`[::-1]` in `xblamesh.py` and the flip in `xblaconvert.py` are both for the
PNG's benefit and have no place here — doing it "to be consistent with the
converter" turns every picture upside down. That much a texture pack proves:
the converter writes a flipped PNG and the pack loader flips it back, so what a
pack uploads *is* the decode order, and a pack's levels are right.

The other half is that **a mesh's `v` is Direct3D's and counts from the top of
the picture as it was drawn**, which is the end the game's `t` counts *away*
from. So `xblaMeshAddVertex()` writes `1 - v` — the coordinate turns over, not
the picture. This was wrong for a while and is worth knowing how it looked,
because it never once looked like a flip: a body's texture is an atlas of
pieces, and mirroring the sheet moves each piece to some *other* piece rather
than upside down, so what shows up is art that is merely wrong. The CI's lab
tech wore her white sleeves across her chest and her waistband round her hips;
a G5 guard lost his belt and gained a patch of skin over his shirt; the
dumpster's vent panel sat near its rim. The tell, if it comes back: dump the
mesh and its textures with `xblamesh.py --slot N --textures`, which writes the
atlas the right way up, and see whether the piece under a given part of the
model is the piece mirrored about the sheet's middle.

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

**The two switches are separate, and both are live.** The menu page
(*Extended Options > Texture Packs > Xbox 360 (XBLA)*) has "Enable
Models/Meshes" and "Enable Textures", and they are separate because either on
its own is worth having - an untextured mesh says whether a shape is right
without an art problem on top of it.

They are live for different reasons, and the textures' one is the reusable
idea. `Mod.XblaMeshTextures` used to be read in `xblaMeshSetMaterial()`, at the point
a display list is *built*, so changing it did nothing to a mesh already built.
It is read here now instead, in `xblaTexHaveTextures()`, at the point a picture
is handed to the renderer: a material always binds its stand-in, and the flag
only decides whether a picture arrives in its place. **The stand-in's own texels
are white**, so with it off the material draws white times shade, which is the
same flat solid a list built with no texture at all would give - the toggle
costs a `videoResetTextureCache()` and nothing else. That is the trick worth
keeping: a switch read where the data is handed over is live, one read where a
list is built is not.

`Mod.XblaMeshes` is live too, and getting there took one wrong turn worth
writing down. A model is matched against the release's copy as it *loads*
(`xblaMeshRegisterModel()`), so a level loaded with the switch off used to have
nothing to draw when it went on.

**Do not try to fix that by keeping a list of loaded models to go back over.**
It looks like the obvious answer - note the `modeldef` and the file id as each
model loads, and walk the list when the switch is flipped - and it crashes,
because **a modeldef can be freed inside a stage and its memory handed out
again**. `lvReset()` is not the only thing that ends a model's life. The
worked example is file 1369 in the G5 Building: noted with a good `rootnode`,
and by the time the switch was flipped a minute later its `rootnode` read
`0xbe0003ffe0`, which `xblaMeshMatchNodes()` walks straight into. Nothing
cheap distinguishes that from a live model, since the stage pool stays mapped
and reading it gives garbage rather than a fault.

What works is doing the matching up front for every model, whether or not the
switch is on, and leaving the switch to the draw path alone. It is affordable:
the G5 Building loads 55 models that carry a mesh, matching one is a slot read
and a tree walk, and four loads of the level with this always on (1.75-2.10s)
sat inside the spread of four with no package present at all (1.84-1.98s).

The one thing that is *not* affordable is unpacking a 250MB archive for
somebody who only ever wanted the texture pack, so a model load asks for the
package through `xblaImportGetReadyStfsPath()`, which hands back a package
already on disk and never extracts one. A player whose copy is still inside its
`.7z` therefore matches nothing until the first time they switch the meshes on;
`xblaMeshSetEnabled()` calls `xblaMeshOpen(1)`, which is where the archive
comes apart, and the next stage load is like everyone else's. That is checked:
with only a `.7z` in `xbla/` and the switch off, no level load unpacks
anything; switching it on unpacks once and Chicago then builds 18 meshes.

Two things fall out of matching always, and both of them wanted a second pass.

**A "not ready" answer is remembered, a failure is not.** `xblaMeshOpen()`
cannot set `opened` to -1 when the package was simply not on disk yet, or the
later call with `mayUnpack` could never succeed - so it does not, and the
question is asked again by every model that loads. Left there that costs real
work, because the empty answer is not cheap to reach: `xblaEnsureUnpackedLocked()`
takes the mutex, reads the first four bytes of the player's archive to see
whether it is a package after all, stats the `.extracted` marker and scans the
two legacy directories. Straced over one load of the G5 Building with only a
`.7z` in `xbla/`, that was **57 opens of the 233MB archive and 110 probes of the
old texture-packs directory**, every one of them re-deciding what the first one
decided. So `xblaimport` keeps a `notReady` flag beside `unpackFailed`: set when
a lookup that was not allowed to unpack comes up empty, read before the mutex,
ignored by `xblaImportGetStfsPath()`, and cleared by `xblaImportRedetect()`. The
same run is 3 opens and 2 probes, which is the detection pass and the first
lookup. The flag cannot strand the switch, because the only other thing that
changes the answer is an unpack, and that fills `unpackedPath`, which is found
first - checked in gdb by letting a level load set the flag and then calling
`xblaMeshSetEnabled(1)`, which unpacks and opens 2616 slots as it did before.

The one thing that is left over is the level the switch was flipped in: its
models were loaded before there was a package to match them against, so
turning the meshes on there changes nothing that is drawn, and the level after
it is like everybody else's. That is the only case on the page where a
checkbox does nothing visible, so it says so - `xblaMeshModelsAreLate()` is
true when *this* call to `xblaMeshSetEnabled()` is what opened the package and
`STAGE_IS_LEVEL(mainGetStageNum())`, and `xblaMeshResetModels()` clears it at
`lvReset()`, which runs before a stage's models load. So it is set for exactly
as long as it is true, which is what the note that used to live under the
checkbox could not manage - that one guessed at whether the switch had applied
and was wrong half the time. The row is *hidden* rather than blanked
(`MENUOP_CHECKHIDDEN`), so a player who has ever had a package on disk does not
carry an empty line for a sentence they will never read: the page is
byte-for-byte its old self for everyone but the one player it is addressed to.

**A model forgets its own address as it loads, and `lvReset()` is the
backstop.** `xblaMeshResetModels()` drops the node registry and the palette uses
at `lvReset()`, beside the texture ids that go there for the same reason: every
address in them belonged to the pool that has just been rebuilt. That is right
but it is not sufficient, and it must not be what the registry's correctness
rests on, **because a modeldef is freed and reused inside a stage as well** -
the same fact that makes a list of loaded models unwalkable. The hole: the draw
path tells a live entry from a dead one by the definition the model is drawn
from, and that test cannot tell the two apart when the new modeldef has landed
on the old one's address, which is exactly what happens when a file is loaded
back into the slot it was freed from. `xblaMeshForgetModel()` was being called
after the release's copy of the file had been read, so a model the release has
*no* copy of - most of them - returned before it and inherited the dead
entries, mesh and all. It is called at the top of the match now, before
anything can return, so the table only ever holds entries put there by a model
that is still in the memory they name. It is guarded on `g_XblaMeshNumNodes` so
that a machine with no package does not walk a 4096-entry table per model load,
and it changes nothing when there is one: the same 56 models match and the same
14 meshes build in the G5 Building either way.

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

#### Four faults that were in here, and what says they are gone

Each of these was found by running the thing rather than by reading it, and
none of them announces itself.

**`Mod.XblaMeshes=1` in pd.ini with the release still inside its `.7z` drew
nothing, for ever.** A model load asks for the package with `mayUnpack` clear
so that somebody who only wanted the texture pack is never charged 250MB — and
with the switch already on out of the config file, nothing else ever asks: the
menu's `xblaMeshSetEnabled()` is what unpacks, and it is never called, because
the checkbox is already ticked. So the package was never opened, no model was
ever matched, and the page looked right. The match now asks with
`xblaMeshOpen(optEnabled)`: off it is still the speculative pass that keeps the
switch live, on it is somebody who has asked, and the first model load of the
first level pays the three seconds. Checked by emptying `xbla/.unpacked` and
booting the G5 Building with the flag set — `xbla: unpacking ... this happens
once` followed by the same 14 meshes, in one run, and frame 900 identical to
the run with the package already unpacked.

**The node registry filled up with the dead.** A dropped entry has to keep its
node as a tombstone, since open addressing cannot leave a hole in a probe
chain, but `xblaMeshSlotFor()` only ever took an *empty* slot - so every model
freed and loaded again inside a stage (each weapon the player switches to, each
body and head a simulant spawns with) cost a slot that never came back. Full,
the table stops registering anything and every lookup in the draw path walks
all 4096 entries first. The probe now hands back the first tombstone it passes,
and `g_XblaMeshNumSlots` beside `g_XblaMeshNumNodes` is what says how much of
the table is gone: 183 and 183 after 9577 frames of an 80-simulant endless
match, where the count of nodes ever registered is what the old code would
have held.

**The pose arena stopped growing when a frame wanted more than it can hold.**
`frameWanted > cap && frameWanted <= 48MB` left the arena at whatever size it
already was, so one crowded frame past the cap dropped every pose it could not
fit rather than the tail of them - a room of characters in their bind pose,
which is a heap of limbs. It grows to the cap now. (What a crowded match
actually wants: 8MB, in the same 80-simulant match.)

**Three bounds were a word short**, all of them reads of a node the file
itself pointed at: a distance node's rodata (the target is at `+8`, so the
bytes reach `+12`), the next link after a step up to a parent (the parent's
offset came out of the file and had not been looked at), and the draw table's
extent, which was named as `drawoffset + 12 * numdraws == vertexoffset` and can
wrap round to the right answer. They matter because **a slot that a mesh id
names is not always a mesh**: `UsetupdamZ` names slot 1, and every one of these
reads happens before `xblaMeshReadHeader()` has had the chance to say so.

#### Skinning

`Mod.XblaMeshPose` poses a skinned mesh from the game's own matrices, and it is
**on**: without it a whole body draws in its bind pose under one bone's matrix,
which is a heap of limbs rather than a person. Turning it off is how a shape
that is wrong is told apart from a pose that is.

It comes to three facts, and each of them was got at in bulk rather than by
eye — the same way the mesh id was.

**Palette entry i is matrix i of the model.** Not the part number, and not
anything the group table has to be read for. The check: take every model that
names a mesh, walk its joints (the `POSITION` and `CHRINFO` nodes, whose rodata
holds a translation from the parent joint and the matrix index that joint
drives), and compare the offset the model file states against the offset the
palette implies for the same two entries, turned into the parent's frame. Under
this mapping they agree; under the palette read one, two or three entries along
they do not. The identity wins for 166 of the 170 models with a palette, and
the four it does not win are files with one usable joint between them.

**The palette holds the inverse bind, and its last column is a translation.**
Entry 0 of the evening dress mesh translates by -146 in y where the mesh stands
from 0 to 149, and the head is at +146: a point at the head lands on the origin
of the bone, which is what an inverse bind is for. Two mistakes are available
here and both were made. Inverting it again — reading it as the bind — puts
every vertex through its bone twice and folds a character in half. Reading the
last column as the bone's *position* instead of as the translation, and
rotating and negating it to make a translation, moves every bone that has a
rotation somewhere else entirely and every bone without one to twice its own
height away; that one survives a glance, because the bones with identity
rotations are the spine and the character stands up straight.

**The float at `+0x18` is a scale, times 100.** 100.0 means the mesh is in the
model file's own coordinates and 1000.0 means it is at a tenth of them. The
same joint comparison is what says so: of the 100 models with eight or more
joints to compare, 96 come out at `unknown / 100` exactly. The four that do not
are 4J's remodelled Bonds — Connery, Dalton, Moore and the DJ — which are a
uniform 10% larger than the skeleton the game poses them with, and which draw
10% short at the joints for it. A skinned mesh drawn without the scale is a
tenth of its size, which is why one used to sit inside its own chest.

So the transform for palette entry i is: out of the bind (the palette entry, in
the game's units), into the game's matrix for bone i, and back out of the matrix
the list is drawn under — the first part's, which keeps the result inside the
s16 a Perfect Dark vertex holds. The posed copy of the vertices goes in a frame
arena of its own, doubled, because a character is thousands of vertices and the
game's vtx pool is sized for what an N64 drew.

What it costs: 2000 frames of the G5 Building took 24s with the meshes off, 30s
with them on and 34s with them posed as well, so the pose itself is about a
seventh of what the meshes cost there. 1500 frame runs of the G5 Building,
Chicago, Air Base and Skedar exit clean with nothing failing to build, and two
seeded runs of the G5 Building are pixel-identical at the same level frame —
the same check the textures passed.

**That check only means anything on a frame with no chr in it.** `--rng-seed
--fixed-step --screenshot-frame` reproduces frame 900 of the G5 Building to
the byte across runs and across every build in this file's history, and does
not reproduce frame 1700, where a guard is walking past: three runs of the
same binary give three different pictures. That is not the meshes — the build
before any of this does the same — but it is worth knowing before a chr frame
is used to tell two builds apart.

`--xbla-mesh-verbose` prints, per palette entry, the distance from entry 0 in
the mesh beside the same distance in the game's matrices, and their ratio. If
the two rigs are the same rig every ratio is 1 — that is what confirmed this at
runtime, on the evening dress, where all fifteen came out at 1.000 give or take
the pose's own rotations. A single ratio on its own is a bone the release moved;
all of them off by one constant is the scale being read wrong.

A **first person** view is the release's hands and gun meshes drawn at their
own bones, which works; the Bonds' 10% is the kind of thing to look for there.
Heads are their own job — below.

#### The heads

A character's head is a model file of its own (`Chead*Z` — not `H*`, which is
what a search for them comes up empty on), and 72 of the 76 carry mesh ids like
everything else. What kept them off the screen was not the data: it was that a
head is **grafted into the body's tree**. `modelApplyHeadRelations()` makes the
head modeldef's root a child of the body's `HEADSPOT` node and gives the head's
top level nodes that node as their parent, so a head's display list arrives at
`modelRenderNodeDl()` with the **body's** model and the **head's** modeldef —
and the check that an entry's modeldef is the one being drawn, which is there
because a freed model's address can come back as something else's node, threw
every one of them away. `xblaMeshNodeIsGrafted()` is what lets them through: it
walks the node's parents to the model's own root and wants a `HEADSPOT` on the
way. That is as strict as the check it replaces — a head is the only thing the
game ever grafts, every `->parent` the renderer writes being a headspot's — and
it re-tests the node's address on the way past.

Everything else falls out of the body being the model:

- **A head mesh's vertices are in the body's space, not the head's.** Joanna's
  head mesh stands at y 1402 to 1680 where her body mesh is 1489 tall, and the
  stock head's own vertices sit around the origin. The pose puts it right: its
  posed box comes out at `[-87 -40 -103]..[87 217 123]` against the stock
  head node's `[-87 -26 -86]..[81 214 112]`.
- **Palette entry i is matrix i of the body**, the same rule as everywhere
  else. Entry 0 is the head bone and entry 1 the spine — a head is weighted to
  both, which is what lets its neck bend — and entry 2 is an identity nothing
  is weighted to. All 125 head nodes that carry an id load matrix 0 in their
  own display list, which is the body's head bone and where the game draws the
  stock head too.
- **A head model file has one matrix of its own.** Grafted, that does not
  matter: the matrices come from the body. Drawn on its own it would leave
  entry 1 unposed, so an entry the model has no matrix for follows entry 0
  rather than staying at its bind — otherwise the neck trails back to where the
  body would have been.

**Joanna's three heads are matched by size.** `Cheaddark_combatZ`,
`Cheaddark_frockZ` and `CheaddarkaquaZ` are the only three models in the
release that are not ours with two bytes changed: 4J moved the toggled piece —
the earpiece on the right of her head — in front of the head itself, and added
a node to the aqua one, so the node-for-node zip refuses them. They are also
the model a player looks at most, so `xblaMeshMatchBySize()` pairs them by
vertex count instead: our biggest list is the head and takes part 0, the next
is the earpiece and takes part 1. That works because the mesh's own groups come
the same way round — her head group is 2341 vertices against the earpiece's
672. It runs only after the zip has failed, and only for a model whose release
copy names one mesh and numbers its parts 0..n-1 with no gaps.

A head is where the per-group drawing described under "The draw path" earns
itself: the earpiece and the sunglasses are parts of their own under toggles,
and a guard whose glasses the game has turned off now has none, where a list
for the whole mesh gave him a pair whatever the game said. The check that says
so is the same frame with `Mod.XblaMeshes` off — the stock head has no glasses
there either.

**A head's stock hair is drawn over the release's, and the id says which one
to keep.** A guard in the G5 Building came out with two hairdos: the release's
head, which paints its own short hair, and the N64 hair piece hanging in the
air above it, cut to fit a scalp that is no longer there. It reads as a slab of
hair floating over the head and it is worth knowing how it was pinned down,
because two obvious readings of it are both wrong — it is not the head drawn
low (the posed box is `[-71 -59 -82]..[84 172 122]` against the stock node's
`[-67 7 -79]..[66 171 118]`, the tops agreeing to a unit) and it is not the
mesh's own art (`Mod.XblaMeshTextures=0` keeps it). What it is: `Cheadwlab*`
and its like keep the hair in a **toggled node of its own**, `[-67 146 -79]..[66
219 112]` on the G5 guard, and the release gives that node **no mesh id** while
giving the head and the sunglasses beside it one each.

So a zero is not always "no replacement" the way `0xFFFF` is, and the
difference is exactly where a node sits:

| where | `0xFFFF` | `0x0000` |
| --- | --- | --- |
| under a toggle | 54, every one a gun's part or a console's screen | 232, of which 166 are a `Chead*` model's |
| an LOD alternative | 0 | 163 |
| a plain node | 0 | 1796 |

4J had a marker for a toggled piece it meant to keep and used it on the things
that have to keep toggling — a magazine, a laser sight, a screen. The zeros
under a toggle are the hair. Everything else that is zero is a node its
exporter never reached, and those still keep their own geometry: the 1796 plain
ones are why a G5 lab door draws its stock frame around the release's panel,
and the 163 LOD ones are what a chr twenty metres away is drawn from.

**The rule is a head's alone, and the draw path is what confines it there**
(`e->suppress`, set while matching and read in `xblaMeshRenderNode()`). The
other 66 toggled zeros are mostly a weapon's — the Falcon 2 has two beside the
one it marks `0xFFFF` and the four its mesh replaces — and there the same
reading would cost a muzzle flash to save nothing that was drawn twice. A
grafted node is what says a head is a head at draw time, which is a fact the
game supplies rather than a guess: nothing else is ever grafted.

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

3503 textures in about 24 seconds, and about four more if the archive still has
to come apart.

## Where the player puts it: the `xbla/` folder

`xbla/` is to this what `mods/` is to a mod — one folder, drop the file in,
nothing to configure. `xblaImportInit()` creates it at startup (beside the
executable where that is writable and in the save directory where it is not,
`fsChooseOutputDir()`) and logs either what it found or where to put one, so a
player who has never had a package still has somewhere obvious to put it.

Three placements all work, and each was checked: `Perfect Dark XBLA.7z` itself,
the STFS package unpacked out of it, and a folder holding either — the scan
goes two deep, because the release's archive wraps its package in a `Perfect
Dark/` folder and a player who unpacked it by hand has that folder. A package
is looked for before an archive across the whole folder, not per entry, so
somebody with both does not pay for an extraction they have already done.
`Mod.XblaPackage` still names a file somewhere else, and the older search — the
game's data directory, the executable's directory and the save directory, by
name and then by STFS magic — is still there behind `xbla/`, so an install that
was working before this keeps working.

**A package is found by its magic, not its name**, `LIVE`/`CON `/`PIRS`: it is
named after a content id hash. An archive is found by its extension, since it
is not necessarily named anything in particular either.

### The archive is unpacked once, by whoever needs it first

Nothing reads a `.7z` a file at a time — it is one LZMA stream, so one file out
of it costs the whole archive — so `xblaImportGetStfsPath()` extracts it into
`xbla/.unpacked/` and writes `.extracted` beside it when that finished. Both
names start with a dot, so `fsScanDir()` skips them and the unpacked copy is
never mistaken for the player's own file.

It is worth knowing how cheap that is, because it is what makes doing it on
demand reasonable rather than a background job with a progress bar: the release's
233MB archive is already-compressed data and barely compresses again, so it
unpacks in **about three seconds**, once, for 250MB of disk. The texture
conversion used to do its own extraction into `texture-packs/.xbla`; both it and
the mesh loader now go through the one function and whichever gets there first
pays, under a mutex, since the mesh loader runs on the game thread as a level
loads and the conversion on its worker. That old directory is still read and
never written, so an install that already has the 250MB there is not made to
unpack it again.

`xblaImportIsAvailable()` is the question to ask when all you want to know is
whether there is a package — it never unpacks. `xblaImportGetStfsPath()` blocks
for the unpack and belongs only on a path that is about to read the package.
