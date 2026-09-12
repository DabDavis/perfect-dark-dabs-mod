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
and the rest are repaints, which is what `riceconvert.py`'s sibling
`xblaconvert.py --only-upscales` exists to separate. The page had a checkbox
for it once and does not since 2026-09-11: the release's art is all of it or
none of it in the game, and a conversion that wrote half of it made the two
disagree. What confirmed the mapping was the
unambiguous cases — the starburst, the tunnel, the flames, the blinds, the
Joanna portrait, the bar chart — all landing on their own texture number, plus
the record count being `NUM_TEXTURES` on the nose.

### The sky's cloud texture is left out (2026-09-10)

One replacement is refused on purpose, in both conversions (`leftOut[]` in
`xblaimport.c`, `LEFT_OUT` in `xblaconvert.py`): **0013**, the sky's cloud
texture, `g_TcSkyWaterConfigs[0]`, which every stage with clouds draws (the
table in `env.c` has every `clouds_type` at 0; 0c90 is never used). The report
was "the sky is messed up for xbla on levels like crash site", and it bisects
to the pack alone - not the meshes, not the level files: with only
`TexturePack=PD XBLA` on, Crash Site's sunset loses its cloud streaks and is a
flat orange gradient.

`skyRender()` draws the clouds with `(SHADE - ENV) * TEXEL0 + ENV`: the texel
lerps between the sky colour and the cloud colour, and only its intensity
counts. The ROM's 0013 is a 64x64 IA8 fractal over the full 0-255 range. The
release's is 64x64 too (8_8_8_8 tiled, three mips, base first - the record's
21844 non-zero bytes are exactly the chain) but never rises above 122, and it
is **a different picture**: correlation with the ROM's is -0.07, and -0.09
against the ROM's blurred, so it is not a softened or resized copy. It is 4J's
noise for 4J's own sky, and through the game's combiner it is half-strength
haze at best. The release's water (0014, RGBA16 32x32) and the unused second
cloud (0c90) are faithful copies and stay in; so do the sun and glare
textures, which draw the same either way.

Because the conversion runs only when the player asks for it, a pack already
on disk keeps the file until something removes it: `xblaImportInit()` looks
in both places a pack can be (`$E` and `$S`, without creating either) and
deletes a left-out texture it finds, and `xblaImportStart()` does the same in
the directory it is about to write, since the conversion passes over a
left-out texture rather than writing something else over it. The Python converter deletes it from `--out`.

Checked frame-exact on the card: `--boot-stage 0x1c --fixed-step --rng-seed
1`, frame 3200 (the player's first view, clouds along the horizon); frame 200
is the crash flythrough. The big untextured wedge in the middle of that view
is the ROM's own geometry and draws the same with every feature off.

### The other skies, and the slots 4J reused (2026-09-10)

Every level with an outdoor view was screenshotted on the card with the
pack on and off five frames apart (`texpackSetLoadEnabled(0)` from gdb at a
`videoEndFrame` stop, same seeded run): Villa, Air Base, Skedar Ruins,
Defection, Chicago, Pelagic II, Air Force One, Attack Ship, Infiltration,
Escape, War, and the fifteen arenas with clouds enabled. No sky differs.
Every stage with clouds is `clouds_type` 0, so 0013 was the whole of it.
The sun, flare and water records (0014-0019, 0c90, 0c92-0c96) are the ROM's
pictures enlarged - 0.92 to 0.99 correlation on the channel the combiners
read. The I8 flares keep their shape in alpha over a near-white colour,
which is what `skyRenderFlare()`'s `(TEXEL0 * ENV)` alpha wants, and
`gfx_replacement_alpha()` leaves a picture that carries alpha alone.

**Record N is not always 4J's version of texture N.** The sweep showed
Villa's cliffs as Area 51's "51" wall with the release rooms and the pack
off, and Crash Site's cliff as a striped panel. A gdb breakpoint on
`texLoadFromGdl()`'s `texturenum = ...` line, printing `w1 & 0xffff` for a
Villa run with the release rooms and one with the ROM's, gives 669 reads and
40 numbers each; the release rooms bind six slots the ROM's never do
(0222, 0224, 0227, 022f, 08a2, 08a3) and drop seven (08f9, 0904, 0905,
0916, 0921, 0924, 092a). In the ROM the six are a police car's light bar and
door star, a "23" sign, and Area 51's "51" wall and door panel; in the
release they are cliff and grass. So with the pack off the release rooms
draw the ROM's signs on cliffs, and with the pack on anything that binds
those slots draws cliffs on its signs. The fix is one table of reused slots
(`port/include/xblaslots.h`): the importer leaves them out of the pack, and
the release branch in `texLoadFromGdl()` binds them through
`xblaStageWriteTexture()` like the records past `NUM_TEXTURES`, so a release
room gets 4J's picture whatever pack is on.

#### The whole list, off the files (2026-09-11)

A run only visits the rooms it walks through, and only of the level it boots.
`tools/texpack/bgtexscan.py` reads the rooms out of the bg file instead -
header, primary, room table, roomgfxdata, every roomblock's display list -
so it covers all 60 files, every room, in a second. It reproduces the Villa
six exactly, which is what says the parse is right, and finds **44**
candidates over the 30 files that have rooms.

A candidate is not a finding. 4J also retextured surfaces with slots the
level had simply not used before, and those come out of the diff looking the
same: 0962 is sand in both copies, 0a4b a blue swirl, 0aa5 blue stripes.
What separates them is the picture, so each candidate was decoded from
Textures.raw and put beside the ROM's dump. Correlation does not decide it -
544 of the 3495 records correlate under 0.2 with the ROM's texture because
4J redrew that much art - but a subject does: a taxi's "FOR HIRE" sign
against a wall of circuit panels is a different picture, a low-res leaf
against a sharp leaf (08ad, the rubber plant) is the same one. Twenty-two of
the 44 changed subject.

**The room diff cannot see a slot only a model binds**, and most of these
are exactly that: no ROM *room* binds 0222 at all. Every reused slot found
belongs to one of five ROM props - `taxicab`, `policecar`, `hovbike`,
`a51interceptor`, `dd_hovercopter`, the Chicago, Defection and Area 51
vehicles - so all 79 slots those five bind were compared picture by picture
too (`bgtexscan.py --models`). Three more came out of it: 0217, 0230, 08ac,
which no room of either copy binds and which therefore only need keeping out
of the pack. Twenty-five slots in all.

Where the release binds them: Villa and Villa (MP) the original six plus
0219, Crash Site 0221/0223/0226, Defection 0216/021b, Ravine 021a, Skedar
(MP) 00a5, Ruins (MP) 00a9, Complex (MP) 089e, Air Base 089f, Grid (MP)
08bb/08bf/08c0, Felicity (MP) 08c1, Attack Ship 08c3. Defection is the level
the Chicago-band slots came *from*, and its release rooms drop 00a5, 00a9
and 00a6-00ad - the other half of the same move.

Deliberately not listed, because the picture says redraw rather than reuse:
007b (Defection's billboard, a printed poster in the ROM and a lit video
panel in the release, and **both** copies' rooms bind it), 0215 (the rope
prop, and both copies' Air Base rooms bind it), 08ad, 08b8 (the
hovercopter's rotor, still a rotor). A false entry costs an upscale and
mis-scales the release room that binds the slot, so the bar is the picture
and not the diff.

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
sorting and screenshots all carry on working.

Four things it costs a wrong turn to work out again.

**The geometry is the model's own coordinates, at the header's scale.** That
scale is 1 for everything unskinned: an Area 51 crate is 100 units across in
the ROM and 100 in the mesh; a lab door is 4000 by 2800 in both. A skinned mesh
is nearly always at a tenth, and drawing one without the scale puts a character
inside its own chest — see "Skinning".
So an **unskinned** mesh wants no transform of its own — it goes under the
node's matrix like the display list it replaces, and its floats quantise back
to the s16 the game's vertices already are with nothing lost, because at scale
1 they were whole units to begin with. The in-game check is
`--xbla-mesh-verbose`, which logs each replaced node's stock box beside its
mesh's; they should sit on top of each other, and when the id mapping was one
out they did not.

**A skinned one does want a transform of its own, and "nothing lost" was wrong
for it** — see "The pose is written in sixteenths" under "Skinning". At scale
10 the mesh states a character to a *tenth* of a unit, and rounding the pose to
whole ones threw that away.

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
them. It said "no door does" here for two days, because every door it was
checked on was closed: the doors that do are trimmed as they open, and the
loader mirrors that now - see "The door trim".

A node's own list is what loads the matrix that says where the node is, out of
segment 3, and this one is drawn in its place — so it loads it too. Without
that a mesh inherits whatever matrix the node before it left behind, which for
a one-part prop is near enough right to look like a scale problem rather than a
missing transform, and for anything else is not.

What works: unskinned props. A crate, a door, a dumpster, a lift door all land
in the right place at the right size, and a model the release replaced only
part of comes out right — a G5 lab door draws its stock frame around the
release's panel, because a node whose id is `0xFFFF` keeps its own geometry.

**What a built mesh holds, and why it is never freed.** A mesh is built once
and kept for the life of the process. That is on purpose: a mesh is the same in
every level that uses it, and the alternative — freeing a display list between
levels — is freeing something the render thread may still be running. What
makes it affordable is the ceiling. There are 595 meshes in the release, and
building every one of them comes to about **69MB** (1.5M vertices and 706K
triangles; the skinning is over half of it, since a skinned mesh keeps a bind
position, two weights and three bone bytes per vertex on top of the vertex
itself). That is against the 250MB package the player already has on disk, and
no session reaches it: a level's own set is a small fraction — 14 meshes in the
G5 Building. `xblaMeshResetModels()` logs `xblamesh: N meshes built, K KB` at
each level load, which is how a session that had drifted upwards would show
itself; `g_XblaMeshBytes` is the same number for gdb.

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
that carry a texture number — so nothing that goes by number can reach them, and
the pack `xblaconvert.py` writes deliberately leaves them out: a `<texnum>.png`
could not name one. What the renderer does have is the address a display list
binds, which is what `texpackLoadReplacement()` is keyed on. This plays the same
trick one registry along. 646 distinct records are used across all 595 meshes,
so what this can ever hold is that many stand-ins and not the 2006 the range
suggests. A pack can still replace one, by record rather than by number - see
"Repainting them" below.

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

**Except the half texel, which the renderer owes the N64 and not this art**
(2026-09-09). Under a linear filter `gfx_pc` adds half a texel to every
triangle's coordinates, because the N64's bilerp puts a texel's centre on the
integer and GL's on the half; the game's own textures, and a pack's images
scaled up from them, are drawn with that offset in mind. That half texel is
*of the tile*, and for a mesh the tile is the 32 texel stand-in, so the
picture landed a sixty-fourth of its width to the right and up of where 4J
drew it - eight pixels of a 512 wide face. The report it came in as was "the
nose texture is off centre of the nose mesh": on the G5 guard the nose, mouth
and goatee all hung to the viewer's right of the face's midline. A texture
`xblaTexLoadReplacement()` supplied is marked `exact_uv` in the cache entry
and the batch's `uv_ofs` leaves the half texel out for it; a point-filtered
draw never had it, so the mark costs nothing there. Not fixable at the vertex:
baking -16 into `s` and `t` would be right under bilerp and wrong by the same
amount under point, and the filter bit belongs to whoever draws the list.

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

The state a material writes is the texture switch, one `gDPLoadTextureBlock`
and a copy of that tile as tile 1. It writes **no combiner and no render
mode** - those are the node's, because they are how a Perfect Dark model is
lit; see "The lighting" below. (Until 2026-09-10 it wrote `G_CC_MODULATERGBA`
and `G_RM_AA_ZB_OPA_SURF` itself, and every mesh drew at full brightness in
every room.) A material whose alpha bit is set is in the alpha span, where the
mode is the caller's - see "The translucent pass" below. The end of the list
puts the texture switch and the colour table back and leaves the rest alone,
since the cutout list drawn straight after has to find the node's state still
there.

#### Repainting them: a pack's own pictures, by record (2026-09-11)

A pack folder called **`xbla`** holds one image per `Textures.raw` record,
named in hex the way `<texnum>.png` is (`1156.png`, and `1156_car.png` too).
That is the only naming there can be: a record is not a texture number, the two
spaces overlap below 3503, and a filename cannot say which it means - so the
folder says it. Anywhere in the pack will do, the scan carries the mark down
into subfolders, and `Mod.LoadTextures` gates it like everything else.

It reaches every material the release draws with, which is the models **and the
rooms** - `xblastage.c` binds a stand-in through the same `xblaTexBind()`, so a
level served from the release's own geometry is repainted by the same folder.
That includes the **reused slots** (`XBLA_REUSED_SLOTS`), whose record number is
*below* NUM_TEXTURES and equal to the texture number it shadows: inside `xbla/`
that name means the release's picture for the slot, and `textures/<texnum>.png`
still means the ROM's texture wherever the ROM's rooms draw it. The two never
meet, which is the whole reason the folder has to say which space a name is in.

**The picture comes back through xblatex, not through the pack hook.**
`xblaTexLoadReplacement()` asks `texpackHaveXblaReplacement(record)` first and
only decodes the release's own art when the answer is no. Two things follow:
the image goes through the same `exact_uv` and stand-in tile the release's art
does, so nothing about the UVs or the half texel changes; and `import_texture()`
needs no second hook, its XBLA branch being the one that answers either way.

**The decode is texpack's queue, kept store and byte budget** - it is the same
kind of picture at the same sizes as a stage texture, and the two compete for
the same memory (`Mod.TexturePackCacheMB`). Records take ids past the glyphs'
(`TEXPACK_XBLA_ID_BASE`) and a keep slot past the texture numbers'
(`TEXPACK_KEPT_SLOTS`), so one queue and one store serve all three. The first
ask returns NULL as it does for any replacement and **the release's own art is
drawn until the image lands**, rather than a frame of white.

**One trap.** `gfx_texture_cache_drop_texnum()` skips an entry that is already
`replaced`, which is what stops two addresses of one texture number taking turns
re-queueing a decode. An XBLA entry is `replaced` the moment it has the
release's art in it - that *is* a replacement - so the drop that follows the
pack's decode would have skipped exactly the entry it had to remove, and the
player's picture would never have appeared until something else evicted it. The
`replaced` test is bypassed when the id names a record; there is one stand-in
address per record, so the ping-pong it guards against cannot happen here.

**Where the pictures come from.** `Mod.DumpTextures` (F7) writes every record
the game draws into an `xbla/` folder under the dump directory, once per record
per run, right way up - the folder a pack reads back, so it is dump, paint,
drop it in. That is also the only way to find out *which* record a jacket or a
wall panel is: stand in front of it and dump. For all of them at once,
`xblaconvert.py --mesh-textures` writes the same folder from the package.

**What a replacement does not change** is the material's classification:
`xblaTexRecordIsSoft()` still reads the release's own picture, on the game
thread as a list is built, and nothing on that thread may touch the pack index.
So a record that draws as a cutout goes on drawing as a cutout however soft the
replacement's alpha is. Painting a glow over a solid is the case that will look
wrong, and the fix if it ever matters is to move the classification to where the
picture arrives rather than to where the list is built.

**Checked** on Chicago under Xvfb, the release's geometry and meshes both on:
the dump of record `1156` (the police car's atlas) put back as the pack's own
image is byte for byte the record (`x360.decode_texture()` against the PNG,
unflipped - which is the flip convention proved in both directions), and every
one of the twelve records that stage draws replaced by a flat magenta turns the
car and the street magenta. `texpack: 12 pictures for the XBLA meshes' own
textures` in the log, and `kept store holds 10 images, answered 10 repeat
requests` at the end, is what "it is being used" looks like. Note that on-screen
pixel diffs of that stage are noisy run to run (about 1200 pixels around the
car's silhouette edges, stock against stock), so a diff is not the test here -
the round trip is.

#### The lighting (2026-09-10)

"The XBLA textures or models are not affected by lights, they stay bright."
They did, and the reason is worth keeping because it is not where anyone
would look: **a Perfect Dark model has no `G_LIGHTING` anywhere in it**. Its
vertex colours are baked, and the room reaches it through the *render state*
that `modelRenderNodeDl()` writes round each of its own lists - by
`modelApplyRenderModeType1..4`, switched on the list's `mcount` (a gun list's
`unk12`). For a chr (`unk30` 7) that is `G_CYC_2CYCLE`, the combiner pair
`G_CC_CUSTOM_17`/`18` - `(TEXEL0 - ENV) * SHADE_ALPHA + ENV`, then times
`SHADE` - with the environment colour a dark tint (`var80062a48`, 64/10/10
by default), under a **`G_RM_FOG_PRIM_A` blend towards the fog colour**, and
the fog colour is the chr's *shade colour*: `propCalculateShadeColour()`
takes the floor's colour times the room's brightness, gives it an alpha that
grows as that gets darker, and `chrRender()` hands it to the renderer as
`fogcolour`. There is no `G_FOG` in the geometry mode, so gfx_pc's fog factor
is the fog colour's alpha, constant - a guard in a dark room is mixed most of
the way to a dark colour. Props are the same blend under `G_CC_TRILERP`
(`unk30` 9), the gun under 4 or 5.

The mesh lists wrote a one-cycle `G_CC_MODULATERGBA` and a plain
`G_RM_AA_ZB_OPA_SURF` over all of that, per material, so nothing the room did
reached them. Now:

- the list writes no combiner and no render mode (`xblaMeshSetMaterial()`),
  and `xblaMeshRenderNode()` writes exactly what the game would have written
  for the node (`xblaMeshApplyNodeMode()`: the same four functions, the same
  switch) before the opaque list, and `modelApplyRenderModeType4(false)` -
  what the game's translucent pass writes - before the alpha span there;
- the cutout and the translucent blend keep the node's **first** cycle
  (`G_RM_FOG_PRIM_A`, or `G_RM_PASS` for mcount 2, or the one-cycle pair for
  mcount 1) and choose only the second (`xblaMeshSetSpanMode()`). A one-cycle
  `TEX_EDGE`/`XLU_SURF` pair written over the two-cycle state moves the
  surface into cycle one and the blend towards the shade colour is gone;
- **tile 1 is declared as a copy of tile 0**, as `texWriteTileLods()` does
  for a one-level texture, because the props' `G_CC_TRILERP` reads `TEXEL1`
  and the lod fraction gfx_pc feeds it (`gfx_lod_fraction()`) runs 0.7 to
  1.0 - TEXEL1 is most of what a prop draws with, and undeclared it is the
  last tile 1 the game loaded;
- the vertex alpha goes in as 255 outside the fading span. The chr combiner
  reads it, and three opaque models carry a few stray zeros that would have
  drawn in the environment tint;
- the fading span is the exception and keeps its own `G_CC_MODULATERGBA`,
  with `G_CC_PASS2` in the second cycle so it is right under either cycle
  type. A beam of light is not lit by the room, and the chr combiner would
  turn its zero alpha into the environment colour, opaque;
- a record that will not bind gets a white 32x32 tile, since the node's
  combiner reads a texel whatever the material says.

**How it was proved**, because a lit room shows almost none of this (G5 solo
at frame 1700, the guard by the car lift: new and old builds differ in 276
pixels). `propCalculateShadeColour()` has two statics, `scol` and `salp`,
that the N64 debugger set and that override every prop's shade colour when
non-zero; the port's `mainOverrideVariable()` is empty but the statics are
addressable from gdb: `set var propCalculateShadeColour::scol = 8` and
`::salp = 255` at frame 1200 (break on `videoEndFrame` when
`g_Vars.lvframenum` reaches it) mixes every model to near-black by frame
1700. Mean brightness of the guard's head: stock 71, the old loader 102, the
new loader 68; the gun 34 / 96 / 39. The crate the gun points at, a prop,
went dark with them.

#### The translucent pass

A model's list node holds *two* display lists and an `mcount` that says how the
pair is used: 1 and 2 draw the first only, 3 draws the second inside the opaque
pass, and **4 draws the second in the translucent pass** - the window in a door,
the glass in a table, the canopy of a hovercar. For a long time a mesh drew in
the opaque pass alone, which left the game drawing its own translucent list over
the top of the release's model. Counted over the release: **54 of the nodes it
replaces are `mcount` 4 with a list to draw there**, and 27 of those have the
same surface in the mesh, marked by the material's alpha bit. Those 27 were
drawing the same glass twice - once as an opaque cutout in the release's mesh
and once as the game's own pane over it.

So a group is now built as **two spans**, split by the alpha bit of the material
each draw names: the solid draws and the alpha ones, each a list of its own
(`groupgfx` and `groupxlu`, -1 where a group has no alpha material - 124 of the
556 meshes have one anywhere). Where each span goes is the node's business
rather than the material's:

  * `mcount` 4 with a translucent list of its own: the alpha span is drawn in
    the translucent pass, blended (`G_RM_AA_ZB_XLU_SURF`), and the game's own
    list is not drawn - the mesh has that surface already;
  * anything else: the alpha span is drawn straight after the solid one in the
    opaque pass as a cutout (`G_RM_AA_ZB_TEX_EDGE`), which is where every alpha
    material was drawn before the split. A grille, a fence, the leaves of a
    plant;
  * a node with a translucent list the mesh has no alpha for - the other 27 -
    returns 0 in that pass and the game draws its own, which is the rule the
    hair follows too: take nothing away that nothing here replaces.

Because the span carries no render mode of its own, the mode is written once by
whoever draws it. It is the plain translucent surface rather than the fog cycle
the game's own lists take, the same trade the solid span already makes.

What says it works: the Pelagic II door's porthole (`Ppelagicdoor`, slot 2284
part 1, 16 triangles) is a flat pale disc before and a pane you can see through
after, at stage 0x21 frame 700; the dataDyne traffic (`Pdd_hovcar`, `hovcop`,
`hovtruck`, 40-odd alpha triangles each) loses the solid band across its
windscreen at stage 0x30 frame 700. The G5 Building's frame 900, which has no
`mcount` 4 mesh in it, is pixel-identical across the change, 1500-frame runs of
0x21, 0x30 and 0x1e exit clean, and two seeded runs still match to the byte.
`--xbla-mesh-verbose` prints `slot N part P draws its alpha span in the
translucent pass` once per mesh, which is how to tell whether a level has one at
all - most do not.

#### The fading span (2026-09-10)

Report: "the translucency is not working correctly for xbla textures, the
first level, the fans have light pouring out but it looks like a sheet". It
was the meshes, not the textures - `XblaMeshes=0` with the pack still on drew
the beams - and the fans are `Pdd_fanroof` (file 146, slot 2186) and
`Pdd_fanwall` (147, 2187) on dataDyne's helipad, behind the player at the
spawn. The game's roof fan is a 9 vertex disc; the release's is that disc, a
housing, and a **column of light a thousand units tall** the game never had.
Its 48 triangles use the same alpha material as the grille (record 4180, a
soft beam whose alpha never reaches 179) and their **vertices carry the fade:
0x7d alpha at the fan, 0 at the top**. The loader threw the vertex alpha away
(`col->a = 0xff`) and, the node being `mcount` 4 with no translucent list of
its own, drew the alpha span as an opaque cutout: a lavender sheet from the
roof to the sky.

So there is a third span. `xblaMeshDrawSpan()` sorts a draw whose material
has the alpha flag *and* any vertex under `XBLAMESH_FADE_ALPHA` (0xf0) into
`groupfade`, and the hook draws that list in the translucent pass under
`G_RM_AA_ZB_XLU_SURF` whatever the node says about itself - blended by texel
times vertex alpha, no depth write. The vertex alpha is now kept for every
span; the solid one ignores it and the cutout one never carries one below
0xf0, so nothing else moves. Counted with `valpha.py` over the release: 26
draws in 18 meshes have any alpha under 255, and the threshold plus the
material flag keep 13 meshes - the two fans, the five hovercars' lights
(`Pdd_hovcab/hovcar/hovcop/hovmoto/hovtruck`, slots 2188-2193), four flat
panes at 127 or 153 and a set of lamps running 0..232 - and leave out a
speaker at 254 on every vertex, a 251 on one vertex of 168, three vertices of
a 794 vertex body, and the stray zeros on three materials with no alpha
channel, which draw as they always did. `--xbla-mesh-verbose` says `slot N
part P draws a fading span in the translucent pass`.

Two things looked like the culprit first and were not, worth not re-deriving:

- the big grey X over the helipad is the game's own searchlight geometry and
  draws the same with no pack at all;
- the police car and taxi (`Ppolicecar` 2290, `Ptaxicab` 2337) have beam
  cones textured with a soft blob (record 4342, DXT1, no alpha, vertex alpha
  255) on a material with no alpha flag, and the game's own node draws under
  `modelApplyRenderModeType4` with `unk30` 9 - `G_RM_FOG_PRIM_A` over
  `G_RM_AA_ZB_OPA_SURF2`, which in this port's `gfx_pc` is **not** an alpha
  blend (`use_alpha` wants cycle 2's blender to be `CLR_MEM` and `1MA`). Those
  cones draw dark over the sky as the game's do and are left alone. The
  material word's top bytes are a number (0, 1, 5, 7, 10..100 in fives, the
  glow at 45/50, car bodies at 5) with flags in bits 24-26; not decoded, and
  not the blend mode.

Test: `--boot-stage 0x30`, wait out the intro (75s), then from gdb `set var
g_Vars.currentplayer->vv_theta = 0` (or 180) and `screenshotRequest()`; the
fans' columns are at the top of the frame at both yaws. The texture-pack
side of the same report is in texture-packs.md, "An opaque picture for a
texture with alpha".

#### The soft picture (2026-09-10)

Report: a screenshot of the comhub (`Pcomhub`, file 406, slot 2163 - the
wall terminal with the two side boxes, on Extraction's red-lit floor) with a
white half-disc across all three of its screens. Reproduces on the GPU with
`Mod.XblaMeshes=1` and goes away with it off. The mesh's first three draws
are 4J's own screen glow: three quads over the game's tvscreen quads, textured
with record 4134 - a pale cyan radial haze whose **alpha never reaches 108**
- on a material with the alpha flag and vertex alpha 255. That is the alpha
span, and with no translucent list on the node it went to the opaque pass as
a cutout (`TEX_EDGE`): a soft picture cut at a threshold is a solid wherever
its alpha clears it and nothing where it does not, so the haze came out as a
white disc clipped by the quad. The game's screen text drew where the disc
did not reach.

The rule, then: a material whose picture has no edge in it cannot be a
cutout. `xblaTexRecordIsSoft()` (xblatex.c) decodes the record once at build
time, counts the texels at or above 0xf0, and calls the record soft when
fewer than one in a hundred are; `xblaMeshDrawSpan()` sorts a soft material
into the **fading span** beside the vertex fades - blended by texel alpha in
the translucent pass, no depth write, its own combiner. The answer is
remembered per record, so it costs one decode per alpha record per run, and
no package or a record that will not decode leaves the material a cutout as
before. The log says `xblatex: record N is WxH with K of T texels opaque -
soft, drawn blended`, and `--xbla-mesh-verbose` marks the material `(fades)`.

Counted over the 59 alpha records the meshes use (`scratchpad/softalpha.py`
in the session: every alpha-flag draw, its vertex alpha range, and the
record's alpha histogram): nine are soft - 4134 (the comhub glow, 2163 and
2251), 3769 and 4201 (flat 104 tinted panes, 2039/2221 and 2198/2205), 4528
(a green glow at 138, 2315/2316), 4587 (116, 2331), 4603/4604 (a yellow
gradient and a red glow, 2334), 4185 (the hovercars' lights, already a
vertex fade) and the fans' 4180/4182. The nearest of the rest is a lamp
atlas (4157, 2174) with 15% of its texels opaque at the centres, then the
furniture glass with an opaque frame in the same picture (3761, 3817, 4177,
20-39% opaque), the hair (4770/4901, 34-38%) and the sunglasses lens (4870,
67%). Those stay cutouts: the hair and the lens must, and the frame glass
either rides the node's translucent list already or draws as it did. One in
a hundred is the line; a fringe on a leaf or a letter never comes near it.

Verified on the GPU, old binary beside new: Extraction (0x22) frame 42 in
front of the comhub, the screens read their text with a faint glow over it;
G5 frame 900 pixel-identical. **The comhub test**: `console.py` in the
session, made from the door test - break at `videoEndFrame` out of the
cutscene, walk `g_Vars.activeprops` for `type == PROPTYPE_OBJ` with
`obj->modelnum == MODEL_COMHUB` (0xb0), teleport 200 units along row 2 of
`obj->realrot` (normalised) with `vv_theta = atan2(nx, -nz)`, screenshot, then
`xblaMeshSetEnabled(0)` and screenshot again. Extraction has one comhub, in
room 62; its intro is over by frame 32. `MODEL_PD_CONSOLE` (0xb2) is the
laptop on a stand and `MODEL_MODEMBOX` (0x17) the small ceiling box - neither
is the wall terminal, which cost two runs.

**The two switches are separate, and both are live.** The menu page
(*Extended Options > Texture & Model Packs > Xbox 360 (XBLA)*) has "Enable
Models/Meshes" and "Enable Textures", and they are separate because either on
its own is worth having - an untextured mesh says whether a shape is right
without an art problem on top of it.

### "Enable Textures" is the whole of the release's art (2026-09-11)

The switch used to mean the meshes' own records alone, and the rest of the
release's textures only reached the game as a converted pack - the player had
to press the button, wait a minute, spend 600MB, and select the pack. It means
both halves now: with it on, a texture that carries a number is served the
release's record for that number straight out of the package
(`xblaTexLoadNumbered()`), which is byte for byte the picture the conversion
would have written to `<texnum>.png`. Record N is texture N, so there is
nothing to match and nothing to decide.

Four things make it the same picture as the pack rather than nearly the same:

- It hangs off the **same branch of `gfx_pc.cpp`** as a pack's image, so it is
  padded to the tile (`gfx_pad_replacement()`) and goes through
  `gfx_replacement_alpha()` exactly as a `<texnum>.png` would. A branch of its
  own beside that one is how the two would drift apart.
- It skips **the same textures the pack skips** - `xblaImportTextureIsLeftOut()`
  is now public and is the one list both read (0013 and the 25 reused slots).
- A pack the player selected **outranks it**, asked as
  `texpackHaveReplacementFor()` rather than by whether the pack returned an
  image: a queued decode also answers NULL, and reading that as "no file" would
  paint the release's art over a pack for the frame or two before its PNG
  lands, every time the texture cache refilled.
- The pack is still what a **model pack's** `n64_xxxx` material asks for first
  (`xblaTexLoadReplacement()`), with this behind it, so a mesh is painted like
  the room around it.
- Record N is texture N **only where texture N is the ROM's**. A mod's texture
  under a stock number is not, and since 2026-09-12 it is served nothing:
  `texpackTextureArt()` says what supplied the texels, and a maps-only mod's
  map (the Stage Loader, GoldenEye X: 2104 textures, all below `NUM_TEXTURES`)
  had every wall repainted with a picture of Perfect Dark's, stretched over
  tiles it was not cut for. Same rule as a mod's level (`romdataFileIsStock()`)
  and a mod's model. See texture-packs.md, "A texture number belongs to
  whatever supplied the texels".

Checked on the card, Chicago (`--boot-stage 0x1d --fixed-step --rng-seed 1
--screenshot-frame 600`): the switch on with no pack is **pixel-identical** to
the converted `PD XBLA` pack selected with the switch off, and both differ from
stock over 29.8% of the frame. With `pdplus` selected (1057 files) and the
switch on, 98.2% of the frame is still pdplus's art and 99.1% of the frame is
pixel-equal to one source or the other - the switch fills the gaps and takes
nothing.

The cost is one LZX chunk per texture per fill of the renderer's cache, which
is what the meshes' records have always cost. The package is opened on the
switch's own setter when there is one to open, so the 250MB unpack of a `.7z`
lands on the player who just asked for it rather than on the render thread at
the first texture of the next room.

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

#### The door trim (2026-09-10)

"The doors on dataDyne Defection are z-fighting when open: they slide into
the wall but show through the wall and fight." True with the meshes on, and
the mechanism is one the game is quiet about. A sliding door with
`DOORFLAG_0004` - nearly every one in dataDyne, the G5 Building's, Chicago's
shutters, the Cetan's, 22 setups in all - does **not** hide inside the wall
as it opens. `door0f08cb20()` copies the door's vertices with everything past
a plane moved on to the plane, and the plane walks across the door with the
opening fraction (`doorGetBbox()`: `xmin + (xmax - xmin) * frac`), so what is
drawn is only the part still in the doorway; a `DOORTYPE_VERTICAL` door is the
same from the top down. The copy is what the node's rwdata points at (a
per-frame `gfxAllocateVertices()` buffer while it moves, `door->unka4` while
it stands), and a mesh drawn in the node's place from its own authored
vertices is the whole door, standing in the wall it was meant to have slid
into. From the corridor it is a door drawn over the wall; from any angle
where the wall's face and the door's nearly coincide it fights.

The loader mirrors the trim rather than reading the door, which a node has no
way to reach: `xblaMeshNodeTrim()` compares the game's copy against the
authored vertices - a raised minimum x is a sliding door trimmed at that x, a
lowered maximum y a vertical one - and the plane is exact, since the game
puts it at a whole unit and the vertices are s16. `xblaMeshTrimCopy()` then
writes the mesh's vertices for the frame into the pose arena with the same
rule applied. Two things it had to get right:

- **the texture has to be carried, not squashed.** The game slides a moved
  vertex's s and t along the edge to its neighbour on the same row, so the
  picture stays put and the door reads as cut. Its models are quads with an
  edge along the slide; the release's are triangles at any angle. The
  equivalent is the texture's gradient along the axis within each triangle's
  own plane, kept per emitted vertex at build time by
  `xblaMeshNoteTriangle()` (the least-squares answer to "which in-plane
  direction is the axis", so a face at right angles to the axis - the door's
  end - gets no gradient and keeps its coordinates, as the game's rule leaves
  those alone). A vertex shared by a batch's triangles keeps the gradient
  from the triangle whose plane holds the axis best. Sixteen bytes a vertex,
  unskinned meshes only - a door is never skinned and a character is most of
  the vertices there are;
- **the detection's seed.** The first version seeded the copy's minimum from
  the *authored* first vertex, so a door whose first vertex sits at the
  authored minimum (the service door, 36 vertices starting at -1250) could
  never report a trim, while the office door (first vertex elsewhere) did.
  Half the doors trimmed and half did not, and it looked like a per-model
  difference for an hour.

Verified on the real GPU with the door test below: the office door (slot
2207) and service door (2213) on Defection, and Chicago's shutter (2150) for
the vertical axis, old binary beside new at the same level frames. Open a
tenth, the mesh stops at the frame with the grain intact; open 0.95, only the
stock-sized sliver at the frame edge is left; the shutter's rolled-up part no
longer hangs over the wall above the doorway. The gun renderer and the screen
prop also swap a node's vertices and neither can read as a trim: the gun puts
the node's own back, the screen copies positions unchanged.

**The door test** (`scratchpad/door.py` in the session, worth rewriting from
this): run the game under `gdb -batch -x door.py` on the real GPU
(`SDL_VIDEODRIVER=offscreen`), break at `videoEndFrame` once
`g_Vars.in_cutscene == 0 && g_Vars.tickmode != 6`, walk `g_Vars.activeprops`
for `type == PROPTYPE_DOOR` with `doorflags & 4`, pick one by model, and put
the player in front of it: `propDeregisterRooms(prop)`, set `prop->pos` and
`bondprevpos` to `door->startpos + 230 * normal`, `rooms[0]` to the door's,
`playerResetBond(&bond2, &pos)`, `vv_theta = atan2(nx, -nz)` in degrees,
`playerSetCamPropertiesWithRoom(&pos, &bond2.unk28, &bond2.unk1c, room)`.
The door's normal is row 2 of `door->base.realrot` **normalised** - the
matrix carries the model's scale, and the raw row put the player 39 units
from the door facing away. `doorsRequestMode(door, DOORMODE_OPENING)` opens
it; `frac` reaches 0.41 twenty frames later and 0.95 at fifty. Defection's
intro is 3840 level frames; Chicago's 2700. Screenshot names are per second
with a `-N` suffix, and **`ls` sorts `-2.png` before `.png`**, so pair old
and new by the log's `screenshot:` order, not by name - the first sheet made
by name paired a closed door with an open one and read as "no difference".

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

#### A mod's model is not the release's model

The release's package is keyed on **the game's own file ids**, and a mod
replaces a file's contents while keeping its id. GoldenEye X's file 447 is a
GoldenEye character; the release's 447 is `CbiotechZ`. Matched anyway, the
node-for-node zip refuses it — the trees disagree — and **`xblaMeshMatchBySize()`
then takes it**, because all that path asks is that the release's copy names one
mesh and numbers its parts with no gaps. It hands the model's biggest list the
whole of somebody else's mesh and leaves every other list drawing its own
geometry through it: on GE-X's file 447, **one 159-vertex list took the mesh and
the other twenty-four kept drawing** — near LOD lists, toggled pieces, and the
head's hair.

That is what "the release's models and the game's are both on the screen at
once, and even the hair floats above the head" is. The hair is one of the
twenty-four, and it is *not* the hair rule failing: that rule is only ever
applied by the zip (`xblaMeshMatchNodes()`), and a model that reaches the
pairing by size has never been near it.

Two things stop it, and both are wanted:

- **A model is only matched when its bytes came out of the ROM** —
  `romdataFileIsStock()`, which is false for a mod's file directory and for a
  loose file beside the game. A mod that leaves a file alone still gets the
  release's mesh for it, which is most of them: 44 of GE-X's models are left
  alone on Runway and the rest of the level is unchanged.
- **The pairing by size has to account for every list it did not take.** Each
  unpaired list must be a far LOD alternative — a distance node whose near
  threshold is not zero, which the game draws *instead of* the near list rather
  than beside it. That is exactly what Joanna's three heads leave behind (the
  combat head's 36-vertex far LOD) and nothing else. Anything else left over is
  drawn *with* what the mesh replaced, so the whole match is refused and the
  model keeps all of its own geometry, which is the right answer for a model
  this cannot read.

The second is the one that matters if a mod ever patches a file in place rather
than replacing it, since `source` would then say ROM.

**The diagnostic that found it, and how to use it.** `--xbla-mesh-verbose` now
answers "are two models on top of each other" directly rather than by eye:
`xblaMeshRenderNode()` counts, per (model, slot) per frame, the nodes that drew
from the mesh against the nodes it let the game draw, and any pair with both
prints one `xblamesh: OVERLAP model ... slot N: A nodes drew the mesh and B drew
the game's own - <reason>` line, deduplicated by slot and reason. Reading it:

- **nothing, on a stock level**, apart from one or two props a level over. Those
  are the deliberate case from "The translucent pass" — an `mcount` 4 node whose
  mesh has no alpha span, where the game draws its own pane and the release has
  nothing to put there.
- **a reason of "the model is not the one the entry was filed under"** would be
  the head graft or a stale address, and has never fired.
- the pairing by size also prints what it refused and why, and
  `xblamesh: model file N is a mod's, not the release's - left alone` says a file
  was never looked at.

Two things about writing this diagnostic that cost a wrong turn each. Counting
every `return 0` in the translucent pass reports **every** replaced node, because
that is where a node with no alpha span goes and the game draws nothing for it
either unless its `mcount` is 4 — the count only means something behind
`xblaMeshNodeDrawsXlu()`. And a node the pairing never registered returns at the
very first line of the draw path, before any of the counters, which is precisely
the leftover case: it had to be found by asking the *match* what it had left
behind rather than by watching the draw.

**The transient this turned up: the pose arena grew one frame late.** It used
to be one block a side, `realloc`ed between frames to whatever the frame before
it asked for — because a `realloc` *inside* a frame moves vertices that commands
already written point at, which is the same mistake as building a display list
around a growing array. So the frame that first wanted more than the last one
drew the tail of its meshes in their **bind pose**, and a head's bind vertices
are in the body's space around y 1400: a head hanging a body's height above the
body, for one frame, every time a character first comes into view. 12 of them
over 3000 frames of an 8-simulant match.

It is **a list of chunks** now. What has been handed out never moves, so a chunk
can be added in the middle of a frame and a frame that wants more gets it there
and then; each side hands its chunks back at the top of its next frame rather
than freeing them, since the 48MB cap is what bounds this. The same seeded match
drops nothing, and what a match holds is small — one 1MB chunk a side for eight
simulants, six for eighty, read out of `frameBytes` in gdb. The per-level
`xblamesh: N meshes built, K KB; pose arena K KB in N chunks` line is where a
session that drifted upwards would show itself, and
`slot N drew its bind pose - the frame arena would not grow` is now only
reachable at the cap or on a failed `malloc`.

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

#### The pose is written in sixteenths

A Perfect Dark vertex holds an s16 and the game's own models are drawn in whole
units, because that is what an N64 model file could say. **A skinned mesh is
not**: the header's scale is a tenth for nearly every one of them, so 4J stated
a character's geometry to a tenth of a unit, and rounding the posed vertices to
whole ones threw nine tenths of it away.

Where that shows is a **head**. The vertices across a nose are three or four
units apart, so half a unit of rounding is a tenth of the spacing, landing a
different way on every vertex; a ridge that should be straight comes out bent,
and the report it arrived as was "the noses are a tad crooked on the head
models". On a wall or a crate the same half unit is a hundredth of the spacing,
which is why nothing else ever looked wrong — and why this survived every check
that had been run on the loader, all of which measure a box or a position and
none of which measure a *shape*.

The fix is the one an s16 vertex allows: write the pose in sixteenths of a unit
and hand the list the bone's matrix with its three rows divided by sixteen, so
the two cancel at the vertex that reaches the screen. It costs one matrix out
of the frame arena per posed mesh per frame.

How far it can be taken is bounded without posing anything, which is what
`xblaMeshPoseFineness()` does: a posed vertex is a blend of one bind position
put through each palette matrix, and a blend of points inside a box carried
through an affine matrix stays inside that box's image — so the eight corners of
the bind box through every palette entry bound every vertex about to be written.
Every mesh in the G5 Building takes the full sixteen; the widest, a body at
`[-336 -1017 -109]..[395 434 431]`, still only reaches 16000 of the 32767.

Three things this has to get right:

- **The bone's own matrix goes back afterwards.** A display list node loads no
  matrix — a chr is drawn under one matrix for the whole model with the pose
  baked into its vertices — so whatever this leaves loaded is what the next node
  inherits, and a node that kept its own geometry (a far LOD, a toggled piece)
  would draw at a sixteenth of its size. Reloading the bone's matrix at the end
  leaves behind exactly what the undivided version left behind.
- **A frame with no room for the matrix writes whole units.** The fineness is
  chosen and the matrix taken before a single vertex is written, so a failed
  allocation goes back to what this did before rather than drawing a mesh
  sixteen times its size.
- **The copied matrix reaches the list as floats, flagged `G_MTX_FLOATS`.**
  `gSPMatrix` in this port reads the N64's s15.16 unless told otherwise —
  `GBI_FLOATS` is never defined and `gfx_sp_matrix` unpacks integer and
  fraction words. A model's own matrices are floats while the list is built
  and the game converts them **in place afterwards** with `mtxF2LBulk()` (the
  end of chrRender's translucent pass, and the same in propobj.c and
  bondgun.c), which is why a pointer into `model->matrices` has always
  worked. The first landing (`23c75707b`) handed the copy over unconverted
  and the renderer read 1.0f, `0x3f800000`, as 16256: every posed chr
  sixteen thousand times too big, reverted the same day. The re-landing
  (`8a21a4557`) ran `mtxF2L()` on the copy, which drew right and **shook**:
  the game's rows carry the model's scale, a tenth, so divided by sixteen
  they are 0.006, and the s16 fraction's 1/65536 is a quarter of a percent
  of that — against vertices written sixteen times larger, up to a tenth of
  a unit on a gun eighteen units from the eye, three or four pixels at
  1080p, landing differently each time a row crossed a step of the fraction.
  "The XBLA char models have the shakes", worst walking slowly in third
  person and on the hands and gun in first (2026-09-10). The fix is the
  port-only flag `G_MTX_FLOATS` (0x80, `include/PR/gbi.h`, outside
  `PLATFORM_N64`) on the `gSPMatrix` parameter byte, which `gfx_sp_matrix`
  answers with a `memcpy` of the sixteen floats; the bone's own matrix put
  back after the lists is one of the model's and is *not* flagged, the game
  converts it. Any matrix this file ever hands a display list that is not
  one of the model's own goes over with the flag and stays float. Measured
  on the dumped matrices of a seeded G5 frame: the fixed-point error was
  0.03 units on the Falcon's vertices and 0.07 on a body's; a float's is a
  thousandth of that.

The check is the posed box in `--xbla-mesh-verbose`, which divides back before
it prints: it reads the same as it did before the change, to the unit.

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

#### What `mtxF2L()` folds in, and the half-scale stages (2026-09-11)

Report: "we are having transparency issues with every xbla texture ... you can
see the ship layers disappearing, and joannas legs are invisible. its when the
camera pans or turns it will repaint them sometimes." Villa, with a trace dump
and screenshots.

It is not the textures and not transparency. **A matrix handed over as floats
skips more than the fixed-point conversion: it skips the scale the conversion
folds in.** `mtxF2L()` multiplies a matrix's first three columns by
`var8005ef10[0]`, which is `65536 * scale_bg2gfx`, and the last by
`var8005ef10[1]`, which is always 65536. `scale_bg2gfx` is the stage table's
own figure (`lv.c` calls `mtx00016748()` with it once a pass, and back to 1 for
the passes drawn in the game's units), and three stages are not 1:

| stage | scale |
| --- | --- |
| Villa (`0x2c`) | 0.5 |
| Crash Site (`0x1c`) | 0.5 |
| Air Base (`0x27`) | 0.5 |

So on those three the game draws the whole world at half its own units and
widens the z range to match (`bg.c`'s `zrange.far / scale_bg2gfx`), while the
divided draw matrix above — the only matrix in the port that goes over as
floats — kept the stage's units. **The picture is right and the depth is
twice everyone else's**, because the scale is about the eye and divides out of
x and y but not out of z: a posed mesh sits in the z buffer at twice its true
distance and loses to anything in front of it. A body's legs lose to the floor
it stands on, a ship's hull loses to the sea behind it and you see the cargo
bay through it, and a guard is cut off at whatever height the ground crosses
him. Nothing is transparent; everything is behind.

`mtxApplyGfxScale()` (beside `mtxF2L()` in `mtx_c.c`, so the two are read
together) folds the same scale into the copy, read where the copy is built
because the scale is live. **Every other stage is 1.0, and the multiply is
exact, so nothing else moves**: the G5 Building at frame 900 and Defection at
frame 700 differ from the build before it only in the on-screen counter box
(rows 62-89), which is the usual seeded-run difference.

Worth not re-deriving, because each of these looked like the answer and was
not: the skeleton pairs exactly (thigh 354.0 against 354.0, shin 366.6 against
366.6, and every posed joint lands on the game's own); all 2593 of the body
mesh's triangles reach `gfx_sp_tri_emit`, none culled, none degenerate, 159
trivially clipped at the bottom of the frame; the render mode, the scissor and
the viewport are the node's own; and the posed box matches the stock model's to
a unit and a half at every corner, feet included. The measurement that ends it
is the **clip coordinates of one vertex**: the same vertex comes out
`w = 213.04` through the divided matrix and `w = 106.50` through the bone's own,
against a true view distance of 213 — the game's own geometry is the half, and
the mesh was the one telling the truth in a world drawn at half scale.

**Test:** `--boot-stage 0x2c --fixed-step --rng-seed 1`, frames 273 (the ship
over the sea, seen from behind) and 876 (Joanna in the hangar). Before, the ship
is its cargo bay and two fins over open water and she has nothing below the
hem; after, the hull is solid and she has legs and shoes. Crash Site frame 900
(she is face down in the snow with her body buried) and Air Base frame 900 (a
guard sunk into the ground, seen through the scanner) are the same fault. It
needs no input at all - the whole report is the opening cutscene, which is
what `tickmode 6` in the trace dump says.

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

The hair rule below does not have to reach these three: none of Joanna's heads
has a `MODELPART_HEAD_HAT` node at all, and the only toggled piece they do have
- the earpiece - is one the release remodelled and gave an id. The combat head
is the one with a third list, a 36-vertex far LOD, and it sorts below the
114-vertex earpiece, so the pairing takes the head and the earpiece and leaves
the LOD its own geometry, which is what it should have.

A head is where the per-group drawing described under "The draw path" earns
itself: the earpiece and the sunglasses are parts of their own under toggles,
and a guard whose glasses the game has turned off now has none, where a list
for the whole mesh gave him a pair whatever the game said. The check that says
so is the same frame with `Mod.XblaMeshes` off — the stock head has no glasses
there either.

**A head's stock hair is drawn over the release's, and the game names the node
that carries it.** A guard in the G5 Building came out with two hairdos: the
release's head, which paints its own short hair, and the N64 hair piece hanging
in the air above it, cut to fit a scalp that is no longer there. It reads as a
slab of hair floating over the head and it is worth knowing how it was pinned
down, because two obvious readings of it are both wrong — it is not the head
drawn low (the posed box is `[-71 -59 -82]..[84 172 122]` against the stock
node's `[-67 7 -79]..[66 171 118]`, the tops agreeing to a unit) and it is not
the mesh's own art (`Mod.XblaMeshTextures=0` keeps it). What it is:
`Cheadwlab*` and its like keep the hair in a **toggled node of its own**,
`[-67 146 -79]..[66 219 112]` on the G5 guard, and the release gives that node
**no mesh id** while giving the head beside it one.

Which node that is does not have to be guessed at from the shape of the tree.
A head model numbers its toggled pieces and the game has names for them —
`MODELPART_HEAD_SUNGLASSES` (0), `MODELPART_HEAD_HAT` (1), `EYESOPEN` (2),
`EYESCLOSED` (3), `HUDPIECE` (4) — and reading the release's parts tables by
name is what settles the rule:

| head part | given a mesh id | left at zero |
| --- | --- | --- |
| HAT (the hair) | 0 | 53 |
| SUNGLASSES | 45 | 6 |
| EYESOPEN / EYESCLOSED | 4 + 4 | 0 |
| HUDPIECE | 3 | 0 |

That asymmetry is the whole argument. A piece 4J remodelled gets a group of its
own — the sunglasses do, in 45 of the 51 heads that have a pair — and **the
hair never does, in any of the 53 heads that have one**, because it is painted
into the head itself. Alex's ponytail and headband are part of his head group
where his sunglasses are a separate 252-triangle group in front of it, which
is what a render of the two groups in different colours shows at a glance.

So `xblaMeshIsHairList()` asks the model for its `MODELPART_HEAD_HAT` and
suppresses the list under that toggle and nothing else. Two things it will not
do, both of which the first version of this did, and both of which took
something off the screen that nothing replaced:

  * **the six heads whose sunglasses the release left at zero** — `Cheadanka`,
    `Cheaddarling`, `Cheaddavec`, `Cheadfem_guard`, `Cheadjon`,
    `Cheadjonathan` — keep the game's own glasses. Their meshes have one group
    and it is a bare face: 1180 triangles on Anka against 1494 for Alex's head
    alone, with no glasses anywhere in the geometry. The stewardess in Air Base
    is spawned `SPAWNFLAG_FORCESUNGLASSES` and wears Anka's head, so this was a
    chr the game had dressed and the mesh had undressed;
  * **a far LOD alternative** keeps its own geometry, hair and all. Of the 132
    ids the release gives a head's nodes, the 124 that sit under a distance
    node are every one of them on an alternative that starts at 0 (the other 8
    are in heads with no LOD pair at all); past 6000 units the game draws its
    own 30-vertex head, and suppressing the hair there just made it bald.

**The rule is a head's alone, and that is decided by the skeleton, not by
the graft** (2026-09-10). `e->suppress` is set while matching and read in
`xblaMeshRenderNode()`, and until this date the draw side also asked that the
node be grafted - "the game grafts nothing else, so a weapon's toggled piece
reaches here with its own model and keeps its geometry". Then the tester
scrolled the Combat Simulator's Character page: "a lot of the xbla head models
have the n64 hair model floating over their head". **The Character page (and
the Ghost Trials pages made from it) zooms on a head by loading the head file
as a model of its own** - `menuRenderModel()`'s `newparams` branch, no body,
no headspot, `curparams` is the head's file id and `model->definition` *is*
the head - so every one of the 53 hat heads came up there with its hair
hanging over the release's, while the same head on a chr in a match was
clean (checked on all 75 Combat Simulator heads under gdb: every hat list
filed `HAIR` under the head's own modeldef, grafted). Now
`xblaMeshIsHairList()` asks `xblaMeshIsHeadModel()` first - a head's skeleton
is the one the game never promotes to a pointer, so `modeldef->skel` is still
the number `SKEL_HEAD`, which is how body.c reads it too - and the draw side
suppresses a `HAIR` entry whether or not the node is grafted. The skeleton
check is what keeps the eleven weapon zeros below safe (part 1 is a toggle in
exactly one other model, `MODELPART_DRCAROLL_0001`); the 179 toggled zeros
this rule does not touch include eleven that have to keep drawing: seven are
`MODELPART_GUN_MUZZLEFLASH1` (the AK47, the MP5K, the Uzi, the Skorpion and the
minigun in both its models) and four more are the minigun's flashes 2 and 3.
4J marked the same part `0xFFFF` on 36 other guns, so its own marking is not
consistent, and a zero on a weapon has to be read as "keep" whatever a head's
means. (The Falcon 2's two toggled zeros, mentioned here before as the muzzle
flash risk, are parts `0x42` and `0x2f`; its flash is one of the 36 marked
`0xFFFF`. The risk was real, on other guns.)

**Robin's hair has no part number** (2026-09-10, the same sweep). With the
53 hat heads clean, one head on the page still had a slab over it:
`CheadrobinZ` (carousel 41). Its parts table names `0x191` and the sunglasses
and not the toggle its hair sits under, so `modelGetPart(MODELPART_HEAD_HAT)`
finds nothing and the hair was left as an ordinary toggled zero - "kept". A
toggle no part names can never be switched by the game (it reaches a toggle
only through `modelGetPart()`), so it is on for ever; for Robin,
`xblaMeshIsHairList()` takes "the toggle no part names" as the hat, keyed on
`FILE_CHEADROBIN` through `xblaMeshFileId`. It is Robin's alone on purpose:
the other two heads with an unnumbered toggle keep theirs - `Cheadfem_guard2`'s
is her sunglasses (96 vertices at eye height, and the mesh has none) and
`Cheadbeau`'s is a 72-vertex piece at the chin. The static check that says so:
load all 76 heads under gdb (`heads.py` in the session: `modeldefLoadToNew()`
each at a level frame, walk the tree, read the hash entry per list) and diff
the report across the change - the one line that moves is Robin's near hair
list, NOENTRY to HAIR, 54 hair lists filed where there were 53.

**How the Character page is driven headlessly**: Xvfb at 1280x720 with the
scratch pd.ini set windowed at that size (`DefaultFullscreen=0`; fullscreen
on Xvfb clips the menu) and `MemorySize=64` (the tester's save says 16, and
the Institute will not load in it). From the title: Return (agent file), Down
x3 Return (Combat Simulator: Solo, Ghost Trials, Randomizer, Combat), Down x3
Return (Advanced Setup), Right (Player Setup), Down Return (Character), Down
(the head row - the preview zooms to the head), then Right per head, `import
-window root` each, crop `240x230+540+265` and `montage` them. `g_Menus[0].menumodel`
is the preview; its `curparams` names the loaded file.

What says the narrowing is right: the same three frames of the G5 Building
(0x1e, seeded, frames 1400/1500/1600, a guard's head filling a quarter of the
screen) are **pixel-identical** between the build that suppressed all three of
a head's toggled zeros and the one that suppresses only the hair — so nothing
that was being drawn correctly changed, and the hair is still going. Each head
model now reports one suppression at load where it reported three. In a
Combat Simulator match the heads are Joanna's, which have no hat node at all:
`xblaMeshMatchBySize()` fires for file 412 on all 21 chrs, pairs 816 vertices
to part 0 and 114 to part 1, and a simulant running past the spectate camera
has her hair and no seams.

The counts a zero comes in:

| where | `0xFFFF` | `0x0000` |
| --- | --- | --- |
| under a toggle | 54, every one a gun's part or a console's screen | 232, of which 166 are a `Chead*` model's and 53 are the hair |
| an LOD alternative | 0 | 163 |
| a plain node | 0 | 1796 |

#### A plain zero is not "keep your own geometry" — it is the mesh's

Read that way, which is what this did until 2026-09-09, **every character in
the game drew the release's body and the N64 body inside it**, and so did the
Carrington Institute's sofa. It is the report that opened the session: "in
game i see both models loaded together, intermingled", walking round the
Institute and in the Combat Simulator, with no mod loaded.

The id is written on **one** node of a model and the mesh named there is the
whole model. `CcarringtonZ` is thirty list nodes; one carries an id, and the
mesh it names is 4792 vertices over a **fifteen-entry palette** — one for each
of the model's sixteen position nodes — spanning y 0..1449, a whole standing
figure. The node it is written on draws 54 stock vertices of that figure. The
other twenty-nine lists, all at zero, draw 2391 vertices: a complete second
character in the same place, posed by the same bones. 127 of the 134 character
models are that shape, and the props that are include `Pci_sofaZ` (the mesh's
box is the whole sofa; the id node is its top and the plain node its base),
`Pdd_hovercopterZ`, `PautosurgeonZ` and `PtesterbotZ` — the Institute, which
is where it was seen.

The 1796 figure above is what made the wrong reading look measured. It is a
count of nodes, and 946 of them are a model's own geometry drawn twice; the G5
lab door's stock frame is a `0xFFFF`, which is a different row of that table
and still keeps what it has.

**The rule now** (`xblaMeshIsCovered()`, filed as
`XBLAMESH_SUPPRESS_COVERED` while matching): a zero on a list of a matched
model draws nothing, unless it is one of the two kinds of list the game draws
*instead of* the one the mesh was named on —

  * **a far LOD alternative**, a distance node whose near threshold is not
    zero. The mesh's own node is under the near alternative of its pair, so
    past that distance nothing of the mesh is drawn at all and the game's
    low-poly copy is the whole model: 864 lists. Suppressed, a guard twenty
    metres away would be nothing at all. (Same test the pairing by size makes
    its leftovers pass.)
  * **a toggled piece**, geometry the game switches: 125 lists — the eleven
    muzzle flashes, the six heads' sunglasses the release left at zero, the
    Nintendo logos. 4J marked a toggled piece they remodelled with an id and
    one they kept with `0xFFFF`, so a toggled zero is one they never looked
    at, and taking it away takes a character's glasses off. The hair is the
    exception and is named from the game's own `MODELPART_HEAD_HAT` before
    this is asked.

946 lists across the release are suppressed by that, 844 of them a character's.

**And the mesh has to be named on a list the game draws beside them**, or
nothing is suppressed at all: `firstslot` is only taken from an id node that
passes the same `xblaMeshIsCovered()` test. One model in 542 needs it —
`CheadgreyZ`, whose only mesh id is on a **toggled** 216-vertex alternative
with the 363-vertex head beside it at zero. Suppress that head and the Grey has
no head whenever the toggle is off.

Two consequences worth knowing. The node table now files every list of a
matched model rather than the one that was named, which is fifteen entries a
character where it was one, so `XBLAMESH_HASHSIZE` is **16384** and the
per-level line reports `N nodes in M of 16384 table slots` beside the meshes
built — a full table stops registering anything at all. And the overlap
diagnostic could never have found this: a node the match never registered
returns at the first line of `xblaMeshRenderNode()`, before any counter, so
`--xbla-mesh-verbose` printed a clean bill through all of it. What found it was
asking the *files* — walk every model that carries an id, count the list nodes
that do not, and compare what they draw against what the mesh spans
(`.xbla-work/mesh/`, `cover.py` and the scripts beside it).

What says the fix draws what it should: the same seeded 20-simulant match on
Temple, before and after, sampled at frame 300 — **198 draws, 14871 triangles,
36706 vertices** becomes **167 draws, 14573 triangles, 34939 vertices**. The
draws that went are the body's fifteen; the triangles barely move because the
mesh was always the bulk of them. The body model reports `14 more lists the
mesh covers, drawing nothing` as it loads.

### The boot logos are a different logo, not a better model

The one place where 4J's mesh is not a higher-poly version of the same object
is the boot sequence. `PnintendologoZ` is the Microsoft Game Studios logo,
`PrarelogoZ` is the flat orange Rare plaque on an orange field that runs to the
edges of the screen, and the `Pnlogo*Z` pieces are the 360's. Matched, they
replace the N64 intro with the Xbox 360's, which is what "the Rare logo,
Microsoft logo and Nintendo 64 logo are all discoloured" is: the Rare screen
turns orange, the Nintendo wordmark turns into Microsoft's, and the N64 logo
loses its four colours.

So `xblaMeshIsBootLogo()` refuses six file ids outright - `FILE_PRARELOGO`,
`FILE_PNINTENDOLOGO`, `FILE_PNLOGO`, `FILE_PNLOGO2`, `FILE_PNLOGO3` and
`FILE_PJPNLOGO` - before the slot is ever read, and the boot sequence keeps the
game's own models with `Mod.XblaMeshes` on.

Reproducing it headlessly: boot with the tester's `pd.ini` and shoot the first
sixteen seconds a frame a second. The sequence is the legal screen, Rare,
Nintendo, the N64 logo, then the Perfect Dark logo, and the whole thing is over
by about fourteen seconds. `Mod.LoadTextures=0` still shows it, which is what
rules the texture packs out - it is the meshes, not the pictures.

## The interface art, and the logo (2026-09-11)

Past the numbered textures and the font atlases, the records hold the art 4J
drew for the console's own frontend. Nothing here stands in for anything the
ROM draws, so it is only used where this port puts it deliberately:

| records | what |
| --- | --- |
| 0dbf-0dd8 | Xbox button, stick and d-pad glyphs, and a few UI marks |
| 0dd9 | the Perfect Dark logo, 420x255 |
| 0dda-0df8 | the achievement icons |
| 0df9-0e1d | 37 country flags |
| 0e1e | an Xbox 360 controller, 256x256 |
| 0e1f-0e51 | the explosion sequence and its smoke puff - see below |
| 0e53-0e9f | 4J's skies: domes, starfields, sunsets, a planet |

**The logo is drawn** (`port/src/xblaui.c`), as the banner of the *Xbox 360
(XBLA)* page, and the row is not there at all without a package. It goes up
through `menuimage.c` - which exists for a picture the menus draw that is not
one of the game's own textures - with one thing added: an image can now name a
loader instead of a PNG in the binary, and this one decodes a record.

**Nothing draws the button glyphs.** The port's menus name a button nowhere -
a bind is a string like `JOY1_A` in a dropdown - and the glyphs are the Xbox's
own controller rather than whatever a player of this port is holding. They are
listed here so that the next person does not have to find them again.

Two things the logo cost, both of which generalise:

- **Row order is a fact about the record, not about the package.** The font
  atlases are stored top row first, the way a screen is drawn and the way
  their `.abc` cells are measured; this record is stored bottom row first, the
  way the game's own textures are. `menuimage.c` hands the renderer a
  top-first picture, so the logo came up upside down in the page that exists
  to show it off. Do not look for a rule covering both - look at the picture.
- **`menuImageDraw()` used to take the caller's alpha alone**, which is right
  for a community pack's cover (opaque) and painted the logo's transparent
  field as a blue slab around it. It multiplies the picture's own alpha in
  now, which changes nothing for an opaque one.

## The explosion (2026-09-11)

**4J did not upscale the ROM's explosion.** Records 001e to 0039 are still the
56x56 puff and the 14x14 colour ramp the N64 draws - their renderer never
asked for them - so "Enable Textures" leaves an explosion exactly as it was.
What their renderer used is a 48 frame animation of its own at **0e1f to
0e4e**: 256x256, its own colour and its own alpha, a fireball that blooms,
rises, throws sparks and fades out (the alpha's peak falls from 246 to 51 over
the run, so the fade is in the art). 0e4f and 0e50 are empty and **0e51 is the
smoke puff** that follows, which the game draws from its own smoke system and
which is left alone.

`port/src/xblaexpl.c` puts those in the ROM's place under
**Mod.XblaExplosions** ("Enable Explosions"), and the whole of it is one
picture: `g_ExplosionTexturePairs[i].texturenum1`, the frame's *shape*, becomes
a stand-in naming one of the release's records.

**The ROM's colour ramp stays on tile 1**, which is the part worth keeping.
`g_TcGdl2` sets up a two-cycle combiner whose colour is `TEXEL0 * TEXEL1 *
shade` (`G_CC_INTERFERENCE` then `G_CC_MODULATEIA2`), and that ramp is a 16x14
picture whose **alpha averages two fifths** (measured over 001f to 0039 with
`--dump-texture`) - it is what makes an explosion a part of the scene rather
than a flash over it. A first version replaced tile 1 with a flat white on the
grounds that the release's frame carries its own colour, and the result was
half again as strong as the game's own explosion. Left alone, the release's
fireball is drawn at the game's size, tint and opacity.

The tiles, the billboard, the sizes, the frames a part ages through, the
bounding-box squeeze and every coordinate are still the game's, which is the
same bargain a texture pack makes and is why this needed no combiner and no
geometry of its own.

- **Frame 0 is left alone: the ROM's is blank** (001e is zero in every alpha -
  the game uses it as a part's first, invisible step), so the fourteen drawn
  frames are what is replaced, frame *i* being `0e1f + round((i-1) * 47/13)`.
  Replacing frame 0 too puts a lit fireball where the game starts a part at
  nothing. The other 34 release frames are never bound; a finer animation
  would mean the game holding more than fifteen frames, which is a different
  change.
- **They go through `xblaTexBindImage()`**, not the numbered path, because they
  are not a version of any texture number - the address of a stand-in tile is
  what names the picture, exactly as a mesh's material does, and that path is
  deliberately not behind Mod.XblaMeshTextures (nor is it given
  `gfx_replacement_alpha()`'s repair, which is right: 4J's alpha is real).
- **Fourteen decodes in total**, not fourteen per explosion: the registry takes
  the picture over and keeps it. The first explosion of a session pays for
  them, on the game thread where the draw is.
- Neither the draw nor a bind ever unpacks a player's archive
  (`xblaImportGetReadyStfsPath()`); the switch's setter does, like the meshes'.

### Judge this frame-exactly or not at all

Two runs of Chicago with an explosion made from gdb a second apart in wall
time are **not comparable**, and reading them as though they were cost an hour:
the stock explosion looked *invisible* beside the release's, and a full-screen
haze appeared that looked like a fault in the swap. Both were the two runs
being at different points in the level - the haze was the scene's own lighting
and the guards walking, and the "invisible" explosion was one drawn while the
camera faced elsewhere. What settles it is the documented recipe (see the
memory note on headless driving): `--fixed-step --rng-seed 1` under
`gdb -batch -x`, a breakpoint on `videoEndFrame` conditioned on
`g_Vars.lvframenum >= N`, the explosion made at that frame, then a top-level
loop of `call (void)screenshotRequest()` and `continue`. Both runs then have
the same camera to a float and the same frame numbers, and the comparison is
a picture of the release's fireball beside the ROM's puff at the same instant.

The diagnostic that answered "is that haze my picture?" in one run: bind a
flat magenta for every frame. The fireball came up magenta and the haze did
not, which ruled the swap out without another theory.

## The font (2026-09-11)

The release set its menus in the same typeface the ROM does - Handel Gothic -
but from the outline at five sizes instead of as 16 texel bitmaps, and the
glyphs are in the package. `port/src/xblafont.c` serves them to the renderer
the way a texture pack's font folder does, behind **Mod.XblaFont** ("Enable
Font" on the *Xbox 360 (XBLA)* page).

Three things had to be found. All three are checkable by looking at the
picture, which is how each was settled.

### The atlases are records 0db7 to 0dbe, one per font file

Seven fonts ship as `DataFiles/*.abc` metrics beside seven atlases, and the
pairing is by the size of the picture - an atlas is exactly as tall as its
file's lowest cell needs:

| `.abc` | line height | glyphs | lowest cell | record | record size |
| --- | --- | --- | --- | --- | --- |
| `Times New Roman_10` | 15 | 15 | 16 | 0db7 | 256x256 |
| `Handel Gothic_12` | 19 | 184 | 40 | 0db8 | 1024x48 |
| `Handel Gothic_14` | 22 | 184 | 69 | 0db9 | 1024x80 |
| `Handel Gothic_22` | 33 | 184 | 136 | 0dba | 1024x150 |
| `Handel Gothic_46` | 69 | 184 | 490 | 0dbb | 1024x512 |
| `Handel Gothic_54` | 81 | 184 | 656 | 0dbc | 1024x670 |
| `DFGHSMaruGothic-W4_18` | 24 | 1053 | 650 | 0dbe | 1024x670 |
| `DFGHSMaruGothic-W4_24` | 33 | 1053 | 1156 | 0dbd | 1024x1170 |

**0dbc and 0dbe are the same size and only the subject separates them**: 54's
cell for 'A' read out of 0dbe is a kanji, and 18's cell read out of 0dbc is a
piece of one. So the table is written down in `xblafont.c` rather than worked
out, and the record's dimensions are checked against it as it is read - a
package that is not the one this was written against says so in the log
instead of drawing a wall texture as text.

The picture is white throughout with the glyph in the alpha, so it is kept as
one byte a texel. Rows are the console's own, which is the order the port
uploads and the order a glyph's own pixel data is in, so nothing is flipped -
the flip in `xblaconvert.py` and in the F7 dump is for the PNG's benefit.

### The `.abc` format, and the two numbers in it that are not the count

```
u32 version (5)
f32 lineheight            the cell's height, and the same for every glyph
...
u16 chars                 at 0x14
...
u16 table[]               at 0x58: a Unicode map from 0x20 up, one based
{ u16 x1, y1, x2, y2; s16 A; u16 B; u16 C; u16 0 }[]
```

- **The count at 0x14 is 30 more than the table is long**, in all eight files
  (8482 against 8452, 65374 against 65344, 88 against 58). So the glyph
  records are placed from the *end* of the file, which they reach exactly, and
  the table is whatever is left in front of them. A record is only accepted
  while it reads as a cell (`B == x2 - x1`, the last field zero), which is
  what stops the walk: a run of table entries cannot satisfy that. Reading the
  count as the table's length puts the glyph array four records late, which
  looks entirely plausible - every cell still lands on a glyph, just not that
  glyph, and '$' comes out of the slot for ' '.
- **`y1` and `y2` are the same for every glyph of a font.** The cell is the
  font's line box, not the ink. `B` is the cell's width and `C` the advance.

For the printable ASCII, `table[c - 0x20] - 1` and `c - 0x20` are the same
record, which is the check that the map is a map: the cell it gives for 'A' in
every Handel Gothic file crops to an 'A'.

### The ink, not the metrics, is what is matched - and it is the *font's* ink

The ROM's cell is the character's ink with the baseline held separately in
`fontchar`; 4J's is a line box. **So the two are fitted on ink**, read off the
character's CI4 data through the font's palette - `var8007fb5c`'s second bank
gives alpha to the body indices (9 and up) and its first bank to the border the
font bakes around it (1 to 7), so one pass over the glyph gives both the body
box and the cell.

Fitting each glyph into its *own* box is what shipped first, and it seats the
text unevenly. **A 16 texel bitmap's box carries the rasteriser's rounding.**
The md font's 'A' has a faint row of spill under its baseline and its 'H' has
none - the cell heights in the font are 11 and 10 - so filling each box in turn
drew the 'A' a whole texel taller than the 'H' beside it. At 1080p a texel of
that font is five pixels, which is exactly the "some letters sit higher" that
brought this up. The ROM's own glyphs hid it: a letter whose ink is 16 texels
of antialiasing does not show a sixteenth of itself.

So since 2026-09-11 the ink is measured **to a fraction of a texel** and the
*font* is placed rather than the glyph:

- `xblaFontSpan()` takes a row's own ink against the ink of the row inside it.
  A row as full as its neighbour is ink to its far side; a tenth of one is a
  tenth of a texel of it. Flat edges - a baseline, a cap line, an x-height -
  come out exact; a taper (the apex of an 'A') reads short, which is why the
  fit is taken from the cloud rather than from any one glyph.
- `xblaFontBuildLine()` fits one `scale`/`offset` per font through the ink
  boxes of all 94 characters at once, in the coordinate a glyph's baseline is
  an offset into. Its inliers are every letter and digit - the two fonts are
  the same typeface, so a cap line is a cap line in both - and its outliers are
  the characters the ROM drew somewhere of its own: **its '_' is an overbar at
  the cap line, its '=' sits up there with it, its ';' has no tail.** The line
  is taken by counting agreement (every pair of anchors a cap-height apart
  proposes one, the most-agreed wins, then it is re-taken as the mean of what
  sits on it) rather than by least squares, which would drag a whole font off
  its row to meet that overbar. 17k candidates against ~186 anchors is under a
  millisecond, once per font.
- A glyph is placed on that line, moved and squeezed by up to a third of a
  texel if a round letter's overshoot would leave the tile, and **shrunk into
  the tile** if it still will not fit - the same factor in both directions,
  placed at the end of the tile nearest the line. That is exactly the
  characters the ROM put elsewhere, plus the lg '7', whose ROM glyph genuinely
  stops a texel above the baseline and has no tile to reach it in.

**The tile's drawn band is rows 1 to height+1, and a glyph off the line is
shrunk, not squashed (2026-09-11).** Two halves of the same report, "the ':'
is cut off a bit":

- The character sits inside a one texel border and `text0f15568c`'s rectangle
  takes `s` and `t` from 32 - a texel in s10.5 - for the character's own width
  and height, so **row 0 is uploaded and never sampled** (`XBLAFONT_TILE_BORDER`).
  Fitting to a band that started at row 0 put 77 placements partly in it, the
  worst losing most of a texel off the top - the lg '$', the brackets, the xs
  ':'. A glyph that overshoots is now squeezed by the overshoot rather than
  shifted into a row that is not drawn.
- The fallback used to fill the ROM's own ink box, which squashes every
  character whose box is a different *shape* from the release's. The colon is
  the case: the ROM's is a pair of dots two thirds the height of the release's,
  drawn between the baseline and the x-height rather than on the baseline, and
  a 35x11 pixel colon stretched into it came out as two flat bars - a colon
  with its ends cut off - beside a period whose dot was square. Shrinking it
  instead (`high / want` in both directions, the ROM's columns still a bound)
  keeps the dots dots. It comes out smaller than the rest of the font, which is
  the ROM's box being the shape it is; the alternatives - squeezing only the
  gap between the dots, or letting the glyph keep its size and clipping - give
  a colon with its dots jammed together or a colon with a dot cut in half.
  Judged by dumping every glyph picture and composing a line of text from them
  at 5-7 px a texel, then confirmed on the file-select screen ("Mission
  Time: 00:47.60") at 1920x1080.

**A row is totalled, a column is taken at its deepest texel (2026-09-11).**
The same ratio read both ways round squeezes the letters whose outermost
column is a short stroke rather than a tall one. The sm `t` is the case that
shows it: its crossbar is one faint row, so the columns it reaches into total
a fraction of the stem's column beside them, the ratio reads them as the
rasteriser's spill, and the box came out **1.6 texels wide against the 2.7 the
ink covers** - the release's `t` was then drawn at half the width of its own
and read as an `l` with a nick in it, which is what "the lower case t looks
funny" was. Down a glyph the total is right (the rows a line is fitted through
are flat edges, and a taper *should* read short so that glyph falls out of the
fit); across it, what is wanted is how far the ink reaches, so `xblaFontRomInk`
and `xblaFontCellInk` hand `xblaFontSpan()` each column's **deepest** texel
instead. Genuine spill is still discounted - a column the rasteriser only
grazed is faint at its deepest texel too - and the vertical fit is untouched,
byte for byte: the log's four `sits on` lines are the same before and after.
It also unsqueezes `W V X A` (about a texel each in sm), the flag of the `1`
and the crossbar of the numeric `7` (2.0 to 3.0 texels). "Battle" on the file
select is the place to look: three identical bars before, `ttl` after.

Measured over A-Z and 0-9, the spread of the drawn cap line and baseline goes
from 1.00/1.00 texel to 0.30/0.17 in md and from 1.00/2.00 to 0.37/0.24 in lg;
what is left is the release font's own overshoot on round letters, which is
what it should be. sm, xs and numeric had uniform boxes and were already flush.

**A glyph may fill its advance, not just the ROM's ink box (2026-09-11).**
"the 1 is thin". Every one of the game's fonts draws the digit one as a bare
stem with no flag - in sm, md and lg it is *the same bitmap as the `I`* - and
lays it out in a two texel advance. Handel Gothic's own `1` carries a flag two
thirds as wide again as its stem (15 pixels against the `I`'s 9 at size 46), so
filling the columns the ROM's ink filled took the stem in with the flag: the sm
stem came out **0.71 texels against the 1.19 the same font's `I` gets**, a `1`
drawn at half the weight of every other stroke on the line. Measured as the
width a glyph is squeezed to over the width its own shape asks for, the `1` is
the outlier of every font - 0.47 sm, 0.53 md, 0.44 lg, 0.57 xs - against a
median near 0.89.

The ROM's ink box is the right bound for a *letter*, whose columns are the same
columns in both fonts and whose space either side is the bearing. It is not a
bound on a *character* the ROM drew as a different shape. What actually bounds
one is the band the game samples: `text0f15568c`'s rectangle runs the
character's own width from one texel in, so ink past it is uploaded and never
drawn and cannot reach the neighbour whatever the kerning does (`bandleft`,
`bandright`, `xblaFontFillAcross()`).

How far a glyph may take that room is **one condensation per font**, the same
idea across as the fitted line is down: `xblaFontBuildCond()` takes the middle
of what the ROM's boxes allow against what the release's glyphs ask for at the
font's scale, over the whole font - 0.891 sm, 0.908 md, 0.931 xs, 0.885 lg,
0.648 numeric, whose digits are all three texels wide. A glyph is drawn no wider
than that and never narrower than its own ROM box, so anything the ROM drew at
the font's condensation does not move and the text keeps its colour; 17 to 40
characters a font widen, most under 15%, and they are the punctuation and the
narrow letters. The middle rather than the mean, for the reason the line is
taken by agreement: the handful of characters the ROM drew narrower than its own
font would otherwise pull the whole font in to meet them. The minimum is
**characters, not anchors** (`XBLAFONT_COND_MIN_CHARS`, 8) - the numeric font
has fourteen in all, and falling through the line's 16 gave it a condensation of
1.000 and a `1` drawn heavier than the digits beside it.

Stem widths in texels, before and after, measured off the pictures the game
serves (`xblaFontLoadGlyph` through gdb, below):

| font | `1` before | `1` now | its `I` |
|---|---|---|---|
| sm | 0.54 | **0.92** | 1.02 |
| md | 0.94 | **1.08** | 1.76 |
| lg | 1.27 | **1.74** | 2.60 |
| xs | 0.32 | 0.32 | 0.32 |
| numeric | 0.40 | **0.46** | - |

**md and xs have no room and that is the ROM's two texels, not a choice**: their
own bar already fills the character's whole advance, so a flagged `1` there is
condensed as far as it will go and stays lighter than the `I` beside it. sm and
lg take their whole band. The alternative - drawing the release's `I` for the
bar the ROM drew, which gives every font a full weight `1` at the cost of the
flag - was written and rendered first and is not what is wanted; it is a
five-line change in `xblaFontSourceIndex()`'s caller if it ever is.

Growing every glyph to its *own* shape instead of the font's condensation was
also tried: it fattens `W Y w`, the quotes and the brackets into their side
bearings until the capitals touch. The fit is what stops that.

The way to see any of this without the menus is to ask the running game for the
picture it hands the renderer, which also proves the switch is on the path:

```
gdb -p $(pgrep -x pd.x86_64) -batch \
    -ex 'set $w = (int*)malloc(8)' \
    -ex 'set $p = (unsigned char *)xblaFontLoadGlyph(0x80000010, $w, $w+1)' \
    -ex 'printf "%d %d\n", *$w, *($w+1)' \
    -ex 'dump binary memory /tmp/sm1.bin $p ($p + *$w * *($w+1) * 4)'
```

The glyph id is `gDPSetFontGlyphEXT`'s - `0x80000000 | outline << 24 | font <<
16 | index`, index 0 being `!` - so `0x80000010` is the sm `1` and `0x80000028`
its `I`. Driving the menus for the same answer is far slower here: under
llvmpipe the file select animates for several seconds and `import -window root`
catches half-drawn frames (F12, `Mod.ScreenshotKey`, reads back the finished
one, and is what the file-select shots in this file are).

**The xs font is written in capitals** - every lowercase character is the same
bitmap as its capital, because a six texel cell has no room for two cases, and
the width the game lays text out to is the capital's. Drawing the release's
real lowercase there put an x-height letter in a capital's cell, so
`xblaFontSourceIndex()` compares the two ROM bitmaps and draws the release's
capital where they are the same texels. "GAME FILES" down the side of the file
select is the place to look.

Everything the game measures is still the ROM's - the width, the baseline, the
kerning table, `textMeasure()` - and the ink stays inside the columns the game
draws of the character, which since the widening above is its advance rather
than the ROM's own ink box, so nothing reflows, nothing can overlap, and a
dialog that fitted before fits now. Reading 4J's metrics instead would mean deciding where
the ROM's baseline sits inside a line box the ROM has no notion of, per font,
and being wrong about it moves a row off its line. The fit never asks: it
measures what both fonts actually drew.

Two details of the sampling:

- **The footprint is averaged, and never narrower than one source texel.**
  The release's ink is about the size of the box it goes into (46 pixel ink
  into a seven texel cell drawn at five pixels a texel), so a point sample
  keeps a stair-stepped edge next to the smoothed CI4 glyph it replaces, and a
  bare area average is just as blocky the other way when the glyph is scaled
  *up* a little - which is most of them, the scale being a whole number of
  texels. One clamp covers both: wider than a texel it is an area average,
  exactly a texel it is the linear blend of the two texels it straddles.
- **Which size goes on which font is a table**, and only decides how much
  detail there is to scale. The menu is 220 units tall whatever the window is,
  so at 1080p a unit is five pixels and the small font's seven unit cell is a
  35 pixel glyph - Handel Gothic 46's ink, near enough. `md` and `lg` take 54,
  `xs` takes 22, and nothing reads 12 or 14: those are hinted bitmaps a few
  pixels tall and no better than the ROM's own.

To check any of this without the game: the ROM's fonts are uncompressed
segments (`fonthandelgothicsm` at 0x7f9d30 in ntsc-final, the rest beside it in
`romdata.c`), 169 kerning words then 94 `{u8 index, s8 baseline, u8 height, u8
width, s32 kerningindex, u32 pixeldata}`, and the pixel data is CI4 at eight
bytes a row. Decoding a glyph and printing it as ASCII is how the spill row
under the 'A' was found, and `tools/texpack/x360.py` reads the atlas beside it.

### The outline pass cannot be left to the shader

`textRender` draws one glyph twice in a two-cycle combiner, tile 0 through the
palette bank that is body plus border and tile 1 through the body's own, and
Clean Text Outlines has `gfx_opengl.cpp` shape a border out of tile 1's alpha
instead of using the filled cell the font bakes (see text-rendering.md). It
measures half a texel as `0.5 / texSize1` - *of what was uploaded* - so
against a picture five times the size of the tile the border would come out a
fifth as wide as it is meant to be. So `xblafont.c` builds the outline itself
and serves it as tile 0's picture - which also turns the shader's own version
off, since that only runs for a tile 0 no pack replaced.

#### The switch picks a band, it does not turn this off (2026-09-12)

It used to: with Clean Text Outlines off nothing was handed over for tile 0 and
the ROM's filled cell was drawn as before, "which is the look that switch
means". It is not. That cell reads as a border only because the ROM's *own*
body fills the rest of it; drawn around the release's glyph - which is a
different shape, on the font's own fitted line - it is a black block with
somebody else's letter punched out of it. The report was **"the black outline
persists"**, and then **"for the default we want the black outline, but it is
too heavy and blocky"**. Blocky is exactly what it was: on the file select
every letter sat in its own dark rectangle, and the HUD's ammo counter was a
green digit in a black box.

So both positions of the switch serve a shaped band and the switch picks
which: the thin `XBLAFONT_OUTLINE_` one when it is on, and the bold
`XBLAFONT_BORDER_` one - opaque for 0.3 of a texel, gone by 0.7 - when it is
off, which is the weight the filled cell stands for at the size the ROM drew
it. 0.5/1.0 was tried first and closes the counters of 'a', 'e' and the
numeric '8' at 640x480; judge a candidate on those three at 640x480 as well as
at 720p, by gathering the band in numpy over the *dumped body picture* rather
than by rebuilding (`xblaFontLoadGlyph` from gdb, below - five candidates in
one pass, no build).

Both bands are built and kept, and the switch chooses between two pictures at
the glyph: rebuilding one would free a picture on the game thread that the
render thread may be copying out. They go through the texture cache under the
glyph's own key, which does not change with the switch, so
`videoSetCleanTextOutlines()` drops the cache when the value changes - without
that the flip does not show until each glyph happens to be evicted.

Band ink outside the body, in pixels of the picture (the ink divided by the
body's perimeter, which is not the measure the 2026-09-12 table above used -
compare the two columns with each other, not with that one):

    font     thin        bold
    sm      1.3-1.6     2.7-3.6
    md      1.1-1.9     2.2-4.2
    xs      1.0-1.5     1.3-3.3
    lg      1.0-1.3     1.4-2.6
    numeric 2.2-2.5     4.8-5.9

The menu row is **Thin Text Outlines** since 2026-09-12. Its pd.ini key is
still `Mod.CleanTextOutlines` and so is everything named after it in the code,
so that a config written by an older build keeps the setting.

#### The shader's arithmetic is not the shader's look (2026-09-12)

Building it *by the shader's arithmetic* - the body's alpha half a texel out in
eight directions, pushed towards opaque by `* 5 / 2`, the diagonals counting
for less - is what this did first, and it came back as **"the black outline
around the XBLA font is too thick"**.

The arithmetic is the same and the picture is not, because the shader reads its
body **bilinearly**. Half a texel out of a bilinear field is nothing like half
a texel of ink: the body's own blur bleeds over the inner half of the band, and
the outer half is the tail of the ramp and fades. What reaches the screen is a
soft edge. The release's glyph is *crisp* - that is the whole point of serving
it - so the same reach, dilated by a plain maximum, is half a texel of solid
black with a hard rim: two pixels of ink around a three pixel stem at 720p, and
the counters of 'e' and 'a' filled in. The `* 5 / 2` made it worse again. It is
there so that one antialiased texel of a 16 texel ROM glyph counts as coverage,
and it has nothing to answer to in a picture whose edges are a pixel wide, but
it grows the *source* shape before the dilation - by more where the edge is
shallow, so the md 'o' came out at 0.83 texels against the 'B' at 0.43.

The band is shaped instead of dilated: the body's alpha gathered over a disc of
`XBLAFONT_OUTLINE_REACH`, each tap weighted by how far out it is - opaque to
`XBLAFONT_OUTLINE_CORE`, falling away to nothing at the reach - which is a
distance falloff, since for a pixel *d* out of the body the tap that wins is
the one at *d*. Both are floored in pixels of the picture and not texels (a
tenth of a texel is half a pixel of the xs font's five, and rounded away there
it left the smallest text with no edge at all), the tap at the centre keeps the
halo under the body's own antialiased edge so no seam opens between the two,
and the cell is still the limit. Measured as the band's ink in picture pixels,
across the five fonts: 2.0-6.0 before, 0.9-1.9 after.

Judge it on the card (`SDL_VIDEODRIVER=offscreen`) and not under Xvfb, and
judge it on the *glyph* first: `xblaFontLoadGlyph()` called from gdb hands back
the picture the font serves, body and outline separately, with no menu driving
at all (the recipe is in the headless-driving note). A screenshot of the file
select cannot be differenced between two builds - the Institute behind it
animates, so nearly every pixel differs and any "count the dark pixels" measure
over a crop is counting the backdrop.

A pack outranks this glyph by glyph, asked as `texpackHaveFontReplacementFor()`
rather than by whether the pack returned an image - a queued decode also
answers NULL, and reading that as "no file" would paint the release's font over
the pack's for a frame or two after every eviction, which is the same trap the
numbered textures had.

### What it is checked on

The file select screen and the Perfect Menu, not the HUD: three fonts at once,
the same every time, and the sideways sibling titles exercise a rotated draw.
`--boot-stage 0x26`, `import -window root` at 14 seconds for "Choose Your
Reality", then Return for the Perfect Menu (see the memory note on headless
driving). Glyphs that are not the release's stay the ROM's: a character with
no body texels (the space), a font this does not cover, and PAL's 41 accented
characters, whose index is past the ASCII the map is keyed on.

## The level files (2026-09-10)

62 `bgdata/bg_*.seg` files differ from the ROM's. 29 are the 512 byte
placeholders of levels that were never built, two are tile files (collision,
a few hundred bytes each, left alone), and **31 are real levels rewritten
with two to four times the triangles** - `bg_arec.seg` goes from 20656 bytes
to 72291, `bg_rit.seg` from 270K to 1.2M. Bevelled panel edges, rounded
pipes, the Crash Site wreck. `port/src/xblastage.c` serves them
(`Mod.XblaStages`, on by default, counted only while `Mod.XblaMeshes` is on;
"Enable Level Geometry" on the Xbox 360 Textures page).

**It is the game's own room format.** The three sections, the primary data
with its room table, `struct roomgfxdata` and the 20 byte roomblocks, 12 byte
`Vtx`, 4 byte colours, the same display list opcodes including the `0xc0`
texture command - all of it. So nothing draws it but the ordinary bg loader.
**Only the rooms are taken from it.** The file slot keeps serving the ROM's
copy of the level, and `bgLoadRoom()` asks `xblaStageRoomSize()` for each
room as it loads it; a room the release has comes out of the release's file
through `xblaStageRoomRead()`, at the room's own address in the release's
room table, and is relocated against that address instead of the ROM's
entry. Everything else the level is built from stays the ROM's, which is
safe because the two copies agree on it: compared byte for byte across the
24 levels the release stores inflated (`~/.cache/claude-xblastage/`, the
release's records dumped through `x360.c` against the ROM's `bg_*.seg`),
section 1 differs only in the room table's offsets (plus the lights pointer
on levels with no lights, `0` in the ROM and the commands pointer in the
release, which `bgReset()` reads the same way), the portals and their
vertices, the commands, the lights and the room positions are identical,
section 2 is identical, and section 3's bounding boxes and light counts are
identical - only its per-room size hints differ.

That is what makes the switch **live inside a level**, as the meshes' is
(the first version of this handed the whole file to the slot, chosen once
per level, because a room table from one copy cannot read rooms out of the
other - and so F6 changed the models but not the rooms). A flip of either
switch goes through `xblaStageSwitched()`, which drops the loaded rooms
(`bgUnloadAllRooms()`, what the game does when short of memory) and the
next frame loads the visible ones again from the copy the switches now
name. The release's file is read on the first room that asks in a level and
kept until `lvReset()`; a level whose file is refused, or a mod's level
(`romdataFileIsStock()`), reads every room from the ROM. `texLoadFromGdl()`
asks `xblaStageIsRelease()`, which is now "the room being converted came
from the release" rather than a per-level fact. The two things the release
does differently and what had to change for each:

- **Section 1 is stored inflated**: the header's primary-stored size equals
  its inflated size, and every room is raw. The game copes on its own
  (`bgInflate()` copies what is not `1173`). Seven of the 31 (`depo`, `cryp`,
  `crad`, `ash`, `mp1`, `mp5`, `mp10`) are stored the ROM's way and are the
  ROM's geometry to the byte; the loader turns those down and remembers it,
  so G5 (`depo`) draws from the ROM. Some of the release's files also store
  section 2 raw, without the `0x8000` bit; the game masks it anyway.
- **Every `gSPVertex` has a zero byte length.** The count is in the `(n-1)<<4`
  byte as always and the game's own code reads only that (`bg.c`, `tex.c`),
  which is why 4J never noticed; `gfx_pc` takes the count from the length and
  would load nothing. 52665 of 55245 loads. `xblaStageFixRoom()` writes
  `12 * n` in. Also `G_COL`'s count is zero, and nothing reads it.
- **Section 3's per-room sizes are mostly zero.** `gfxdatalen` is what
  `bgLoadRoom()` allocates from (x16 + 0x100, x4 on 64-bit), and the release
  has 0 for whole levels; nothing downstream checked the converted room
  against the allocation. `bgLoadRoom()` now floors it at six times the data
  plus 8K for any room, which is under the ROM's own figure for its
  compressed rooms and so changes nothing there. With the rooms served on
  their own this is also what covers the ROM's figure being the ROM room's
  size when the release's, two to four times bigger, is what is loading.
- **Vertex and colour arrays are not padded to 8.** The port's converter
  (`convertRoomGfxData`) rounded the *source* offset up, which reads the
  arrays four bytes late and leaves the header's pointer with nothing to
  relink to - the `[BG] Unable to relink pointer` fatal on Crash Site.
  Only the host side is aligned now. (The ROM's two rooms with an odd
  offset are empty rooms with no vertex pointer at all.)
- **Two stale blocks in `bg_mp11.seg` room 16** still point at the ROM's
  layout of the room (negative offsets in the release's). Both are orphans
  nothing links to, in the ROM's copy too, but the loader relocates every
  block in the table whether or not it is drawn. Emptied on the way in. The
  audit that found them (reachability from the opa/xlu heads, every
  next/child/gdl/vertex/colour pointer checked against the converter's
  rules) found nothing else in any file.
- **Five levels bind textures the ROM does not have**: Crash Site (8
  records), Air Base, Villa, Defection and Air Force One name Textures.raw
  records 3777-4586, the release's own art, 256 to 1024 square, in a texture
  number wider than the twelve bits the game reads (`w1 & 0xfff`; subcmd 1
  keeps its second texture above them, the others leave the bits clear in
  every ROM file). `struct tex` is twelve bits too, so they cannot go through
  the pool. `texLoadFromGdl()` asks `xblaStageIsRelease()` and, for a record
  past `NUM_TEXTURES`, `xblaStageWriteTexture()` writes the meshes' stand-in
  tile (xblatex.h) with the command's wrap modes and a `gSPTexture` scale of
  32/width, 32/height: **4J measured those rooms' s and t in texels of the
  full picture** (a 1024 texture runs to s = 32767, the top of a Vtx), and
  the renderer scales by the gSPTexture before it divides by the tile, so a
  picture width lands on the 32 texel tile the replacement is sampled over.
  Section 2 (the preload list) names none of them, checked.
- **Each leaf block has its own slice of the room's vertex array**, where
  every leaf of one of the ROM's rooms points at the *start* of it. That is
  what `dyntex` - the animated textures, water among them - had been leaning
  on without saying so: a vertex reaches `dyntexAddVertex()` as the offset
  its `gSPVertex` carried, which is measured from the vertex array of the
  block the command sits in, and `bgRenderRoomPass()` ticks the whole room
  off whichever block it draws first (the once-per-frame guard in
  `dyntexTickRoom()` is per room) and hands it *that* block's pointer. One
  base for every block makes those the same number. Chicago's canal is two
  blocks - the water (`0dae`) in one, the ditch walls and the pavement
  (`01c0`, `0174`, `0197`) in the other - so the water's offsets were
  applied to the block that drew first and **the concrete scrolled while
  the water stood still**. Offsets are stored from the *room's* array now
  (`dyntexSetCurrentVtxBase()`, called by `texLoadFromGdl()` with the
  `vtxstart` it is already given) and the room's array is what `bg.c` ticks
  with; on the ROM's rooms the added offset is always zero, so nothing there
  changes. 19 rooms over six levels were affected, all six of Chicago's
  canal rooms among them - `bg_pete` 36-39, 78, 79, `bg_ref` 29, `bg_eld`
  39, `bg_ear` 79, `bg_lee` 103 and nine of Villa's (`bg_pam`); no room of
  any ROM level has a dyntex texture in a block that is not the first, which
  is why the assumption held for twenty-five years. Measured rather than
  eyeballed: force-load the room from gdb (`call bgLoadRoom(79)`), call
  `dyntexTickRoom()` at two `lvframenum`s the way the render pass would, and
  read the `t` of the two blocks' vertices - before the fix Chicago room
  79's concrete went 1536 to 3925 with the water unmoved, after it the water
  goes -1827 to 562 with the concrete unmoved, which is the ROM's own
  numbers for the same room.

Not a defect: Defection's skybox floor in the ROM carries a painted street
strip (texture 133, four vertices) with lamp glows; the release's skybox
has no strip, so the street far below the roof is black in the intro. The
translucent list is otherwise the same 212 vertices.

Verified on the real GPU (offscreen, `--fixed-step --rng-seed 1`): Defection,
Crash Site (the six Xbox-only records bind and draw), Villa, Air Force One,
Air Base and Felicity (the stale-block arena) all boot from the release and
run 1200-2000 frames clean with no `bg:` warnings. Booting `0x33` with
`--mpsims` dies with SIGFPE with the release on or off - pre-existing, not
this. `--xbla-stage-verbose` logs each file taken or turned down, each room
read from the release, each drop of the loaded rooms, and each Xbox-only
record bound with its scale. Headless recipe: the two save dirs
`/home/sdg/pd-testsave-xs1` (XblaMeshes=1, XblaStages=1, TexturePack=PD
XBLA) and `-xs0` (XblaStages=0); the game **writes pd.ini on exit**, so a
setting flipped from gdb is saved and the next run has it - copy the dir.

**The live switch is checked frame-exact, not by eye.** Defection's opening
flythrough moves the camera, so two screenshots seconds apart differ by
70% whatever the rooms are. Two seeded fixed-step runs of `0x30`, one of
them calling `xblaMeshSetEnabled(0)` at a `break xblaMeshTick if
g_Vars.lvframenum >= 450` (the tick is where F6 acts; never call it from a
stop at an arbitrary instruction, it frees the rooms) and `(1)` at 750,
with a gdb Python sum of `g_Rooms[i].gfxdata->numvertices` over the loaded
rooms at `videoEndFrame` on frames 600 and 900: the plain run has 7228
vertices in 5 rooms at both frames, the flipped run 7090 at 600 and 7228 at
900. The city's exterior rooms are nearly the same in both copies, which is
why the frame-600 screenshots differ by 0.13% of pixels (the dropship, a
model); inside a building the difference is plain.

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
`cache/xbla/` (beside the executable, or in the save directory, by
`fsChooseOutputDir()`) and writes `.extracted` beside it when that finished.
It used to come apart in `xbla/.unpacked/`, a dot directory beside the archive;
since 2026-09-11 `xbla/` holds only what the player put there, and a copy left
in the old place is still read (and the log says it can go) but never written.

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
