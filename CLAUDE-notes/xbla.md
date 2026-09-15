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
  base is the prefix and the rest is left off the end — **except when the whole
  chain shares one tile**, which is the next section and is where the base is
  not the prefix.

### The packed mip tail: a small picture is not at the tile's origin (2026-09-12)

A tiled surface is stored in 32x32 element tiles, and a picture with a side of
16 or less leaves half of one free. The console fills that half with the
picture's own mip chain rather than starting a new tile for it — the *packed
mip tail* — and level 0 is **not** at the tile's origin when it is in there. It
sits 16 elements in: **down** the tile when the picture is wider than it is
tall, **across** it otherwise, with each smaller level halving the offset in
front of it (16, then 8, then 4). A picture with both sides past 16 has a tile
to itself and starts where it always did.

Read from the origin regardless — which is what `x360DecodeTexture()` did until
2026-09-12 — such a record comes back as its own mip levels stacked in front of
it with the picture itself missing: two or three faint fragments where the
subject should be. **400 of the package's records are that shape**, 188 of them
numbered textures.

The rule is `min(log2ceil(w), log2ceil(h)) <= 4`, and it is on the *texel*
dimensions while the offset is in elements — blocks for a block format. In this
package every one of the 400 is `8_8_8_8`, so the block half of it is the
standard rule written down rather than something measured here.

This is what emptied the explosion (below): the game's colour ramp is 14x14, so
every one of its fifteen records was being served the tail instead of the
picture, and the ramp is a *multiplier* — `TEXEL0 * TEXEL1 * shade` — so a near
empty one takes the whole explosion off the screen, the ROM's puff as much as
the release's fireball. With **Enable Textures** on there was no explosion in
the game at all between 2026-09-11, when that switch became the whole of the
release's art, and this.

**How it was pinned down**, since none of it is inferable from the record:

- The switch, not the feature. Turning off each of the five in turn from gdb at
  a frame-exact explosion (`xblaTexSetEnabled(0)`, `xblaStageSetEnabled(0)`,
  ...) put the fireball back for exactly one of them, and it was not the
  explosion's own.
- The tile, not the switch. `leftOut[]` in xblaimport.c is `static const` and
  gdb writes it happily: poking the fifteen *ramp* numbers into it drew the
  explosion, poking the fifteen *shape* numbers into it did not.
- The offset, not the content. Decoding the whole 32x32 tile instead of the
  14x14 window shows the picture at x=16 and a clean half-size copy of it at
  x=8 — and the copy is a 2x2 box filter of it to an RMS of 0.4, which is a mip
  level and not a coincidence. Every shape in the package agrees: the records
  wider than tall have theirs down the tile at y=16 instead.
- Against the ROM. The port's own decode of texture 0x29 went from RMS 132 to
  RMS 22 against the game's texels for the same number, and its average alpha
  from 32 to 102 (the ROM's is 94 over a 1 bit alpha).

Both decoders carry it: `x360PackedMipOffset()` in `port/src/x360.c` and
`packed_mip_offset()` in `tools/texpack/x360.py`. **A pack converted before
this has 400 wrong files in it** and wants converting again; the port's own
decode needs nothing, since it reads the package every time.

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

#### The pane in an atlas (2026-09-12)

Report: "XBLA models have broken transparency on some objects such as tables
(pitch dark shadow, no glass transparency)". The Villa's tables
(`Pcv_table` slot 2173, `Pcv_coffee_table` 2169) and its chairs and sofas.
Their nodes are `mcount` 4 with **no** translucent list of their own, so every
alpha draw went to the opaque pass as a cutout, and the soft-picture rule did
not rescue them because it asks the question of the whole record: record 4148
is a wood atlas whose top left corner is the glass, a dark pane at a flat 140,
and record 4117 is a leather atlas whose bottom right corner is the shadow, a
black square under a soft blob of alpha. Both records are nine tenths opaque.
A cutout of a pane at 140 is a solid pane; of the blob, a black square - under
every table, chair and sofa in the house.

So the question is asked under each triangle. `xblaTexRecordAlphaMap()`
(xblatex.c) keeps a 256x256 point-sampled alpha map of a record with at least a
tenth of a percent of its texels between 0x10 and 0xf0 (point sampled, so a
cutout's hard edge is not averaged into a pane), and `xblaMeshTriIsPane()`
samples seven barycentric points of a cutout draw's triangle at the UVs the
builder writes: a pane when the middle sample is between 0x10 and 0xe0, or when
no sample is opaque and one is a pane (the thin triangles round a shadow's rim
sample mostly clear, and as cutouts drew a black sliver along it). A pane
triangle goes to the **fading span** (blended, translucent pass); the rest of
the draw stays a cutout. The material is now written at a draw's first
triangle *taken*, since a draw can be split between two spans.

**Rigid meshes only.** Run over the whole release (`tritrans.py` in the
session), the same test finds the hair (4770, 4901) and the sunglasses' lenses
(4870) on seventy-odd heads, which must stay cutouts. The skinned meshes are
characters and guns, so the stride is the switch. `--xbla-mesh-verbose` says
`draw N: K of M cutout triangles sample a pane of record R - blended`; the Villa
tables give 22 of 26 (the shadow) and 8 of 8 (the glass top).

Checked on the GPU with a real before: `BEFORE=1` in `table2.py` writes `xor
eax,eax; ret` over `xblaTexRecordAlphaMap` before the level loads, which is the
old loader exactly. Villa, `--spectate`, intro skipped at 600, the spectator
teleported over each table: before, an opaque purple top and a black rectangle
under the table and each chair; after, the legs seen through the glass and a
soft shadow. **The camera drifts** a few units a frame after a teleport, so a
pair must be shot one or two frames apart, not six.

The N64 models draw their own shadows as hard black shapes in this port too
(meshes off) - that is not this loader.

A covered node that draws a pane of its own hands the translucent pass back to
the game only when the mesh has **no** translucent geometry at all - no cutout
span *and* no fading span (`m->allxlu < 0 && m->allfade < 0`). Asking the
cutout span alone was right until panes could be moved out of it: a mesh whose
every alpha triangle became a pane read as "has no pane" and the game drew its
stock glass over the release's.

#### Segment 5 is the model's, and must be handed back

The mesh's `G_COL` names its table through segment 5 now (`XBLAMESH_COLSEG`),
so a bruised copy can stand in, and `xblaMeshRenderNode()` puts back what the
game's own draw of the node leaves there (`rodata->dl.colours`, a gun list's
`baseaddr`) - every renderer of a model node sets it before drawing, so this is
hygiene rather than a fix.

**It was blamed for a crash it did not cause - and so were two other things.**
A Villa drive (`table2.py`: `--spectate`, intro skipped, the spectator
teleported over a table) stopped the renderer with `FATAL: Unknown GBI opcode
... w0 f2002002 w1 0103e03e` at the first table shot. It went away after the
restore, came back after the pane rule changed, and **reproduces on HEAD with
none of this work in it** - same frame, same room list, same bytes. The list is
a **room's** (`G_VTX` through segment 0x0e, `G_TRI4`, inline texture setup),
124 good commands and then words whose upper halves were never written this
frame. Three explanations were each "confirmed" by one run and then disproved:
the segment 5 leak (every model renderer sets it), `bgLoadRoom()`'s texture
rewrite overtaking the lists it copies to the end of the allocation (a copy to
a buffer of its own did not change a byte of the crash, and was reverted), and
the drive's `xblaMeshSetEnabled()` freeing rooms through `xblaStageSwitched()`
(the setter returns early when the value does not change, and the crash comes
before the first switch). **Not found.** What is known: it is room data read
after it stopped being valid, whether it shows depends on the heap, the log is
full of `memory pool ... is full` at the level load, and the drive teleports
the camera into rooms that load all at once. **It belongs to the release's
rooms**: the same drive with `Mod.XblaStages=0` (the ROM's rooms, the release's
meshes) runs all eight shots clean. Judge a mesh change on the Villa with the
stages off until this is found.

**One way to get it is found (2026-09-14): dyntex across the switch.** Three
v3.6.0 crash reports died with this signature (two with these exact bytes) the
frame after `xblaswitch: release assets off` on the Villa, and it reproduces at
once: `--boot-stage 0x2c --fixed-step --rng-seed 1`, the intro skipped at frame
600, `call xblaStageSetEnabled(0)` at a `videoEndFrame` stop at 1000, fatal at
1001. The bad word sat in room 59's freshly reloaded ROM list with its lower
half a good render mode and its upper half `0xff6707a7`. A `watch -l` on that
upper half, set once the room had reloaded, stopped in `dyntexUpdateOcean()`.
dyntex adds a room's animated vertices **once per level** and keeps the
offsets across an unload, which is right while a room always reloads from the
same data. The switch reloads it from the other copy with another layout, so
the release copy's offsets wrote wave texture coordinates into the ROM copy's
display lists. `xblaStageSwitched()` now calls `dyntexForgetRooms()` after it
unloads the rooms; F6 off, on and off again runs clean. ASan hides it, because
its quarantine gives the reloaded room fresh zeroed memory, so the mangled
upper halves read as zero.

This does **not** explain the drive above, which crashed before any switch.
But the bytes are the same and so is the shape (upper halves written by
something other than the list's builder), so dyntex on the release's rooms
is the first thing to look at if it comes back: the release gives each leaf
block its own vertex slice (xblastage.h, the fifth trap), and a room whose
vertex base the tick and the add disagree on would do exactly this.

How it was found, worth keeping: break on `*sysFatalError` (before its
prologue, so `rdx` is the command), walk the master list from `gfx_run`'s
`commands` for the last `G_DL` (6 in this port's F3DEX gbi, `G_ENDDL` 0xb8 -
not F3DEX2's 0xde/0xdf) whose target is below the fault, and dump that list
from its start: `G_VTX` through segment 0x0e and `G_TRI4` is a room, not a
model. Then build HEAD with `git stash` and run the same drive before blaming
the change.

#### Shots hit the release's triangles (2026-09-12)

Asked for after "the xbla meshes use the n64 hitboxes". Where the game looks:
`shotCalculateHits()` -> `chrTestHit()` -> `modelTestForHit()` (a bbox per part,
in the camera space the shot is traced in) -> `func0f06bea0()` (propobj.c), which
walks the model and hands every list under a hit box to `bgTestHitOnChr()` - so
the silhouette a shot could hit was the N64's, whatever was drawn. The part a
hit counts as is the last bbox the walk passed.

`func0f06bea0()` now calls `xblaMeshHitBegin()`, asks `xblaMeshHitSkipsNode()`
about each list (the draw's own decision: a covered list or the painted-on hair
is not tested, a list that draws a group of the mesh is noted), and after the
walk `xblaMeshHitTest()` poses the noted meshes from `model->matrices` - the
same floats `bgTestHitOnChr()` reads, `matrices[i] * invbind[i]` with no root
to take out - and tests their solid and cutout lists (not the fading span)
with the game's own `func0002f560()`. The nearer of that and any stock list
still drawn is the hit, and its bbox is made the only `g_Vars.hitnodes` entry
so `chrBruise()` lays the bruise in that part.

**The part comes from the bone.** A body is one mesh on one node, so the walk
order says nothing: the bone that moves the hit point most is a model matrix,
and the part is the bbox whose `modelFindNodeMtxIndex()` is that matrix. A
head shot does four times a body shot, so this is the part that matters.

Two refinements (2026-09-13). The bone is found from **the hit point**, not
the triangle's first vertex: the three corners' weights are blended by the
hit's barycentric position, so a triangle across a joint counts as the side
it was hit on. And a bone with no bbox (a G5 guard has 19 matrices, 15 to 18
carry none) takes the bbox **nearest the hit as a box**, not the one whose
matrix origin is nearest: an origin is the joint a part turns about, at one
end of it, so a hit high on a thigh stood nearer the pelvis pivot. The box is
measured in its matrix's space the way `modelTestBboxNodeForHit()` reads it
(local = `(p - m[3]) . m[i] / |m[i]|^2`), and a hit inside two boxes goes to
the nearer centre.

`chrTestHit()` and `func0f06c28c()` only reach `func0f06bea0()` after an N64
bbox is hit; a model with a drawn mesh (`xblaMeshModelHasMesh()`, cached per
frame) is let through without one, since the release's hair and shoulders are
outside the N64's boxes.

**Not changed**: with two or more human players `shotCalculateHits()` is
`cheap` and a chr is only ever tested as boxes (`func0f084594()`), and so is a
shielded chr. The matrices are floats from `chrTick()`'s
`modelSetMatricesWithAnim()` until `chrRender()`'s `mtxF2LBulk()`; a gdb test
has to stop inside the tick - `break handsTickAttack`, where the player's
shots are fired - not at `videoEndFrame`.

**Two traps that cost a run each.** A mesh list's triangle is read through
`words.w1`, never `gdl->tri.tri.v`: this port's `Gtri` is laid out for 32-bit
words, so its `tri` is the upper half of `w0`, which a list written with the
gbi macros leaves at zero - every triangle came out as vertex 0 three times and
nothing was ever hit. And **do not call `func0f06bea0()` from gdb**: it takes
eleven arguments, gdb passed the stack-borne ones wrong (`arg10` arrived as
`0xffffffffffffffe0`), and the first hit faulted writing through it at
`func0f06bea0+1080` - which reads as a crash in the game. Drive
`chrTestHit(prop, &shotdata, 0, 0)` instead, with a `calloc`'d `struct
shotdata` (gunpos2d at the camera, gundir2d the ray, `distance` 4294836224,
`penetration` 1) and read `hits[]`: four register arguments and the path a
real shot takes, the gate included.

Checked that way on the G5 Building (0x1e solo, `--fixed-step --rng-seed 1`,
stopped at `handsTickAttack` at frame 1700, the nearest guard): a 26x40 grid
of rays across the guard, through the release's mesh and then with
`xblaMeshSetEnabled(0)` through the N64's. The part maps agree where the models
agree - head over head, torso, biceps, forearms, hands, pelvis, thighs, shins,
feet, the held gun - and 204 of the 1040 cells differ, all at edges where the
shapes do: the release's head starts a row lower, more of the upper arm counts
as bicep, and the knee sits lower (44 left-thigh cells against 12). Hits
622/1040 through the mesh, 633/1040 through the N64 body.

The two refinements above, checked as an A/B of two binaries in one session
(the commit's and the change's, run side by side, each with its own
`--savedir`), on a 78x120 grid at the same guard: hit or miss identical in all
9360 cells, the part changed in 163, and where both the mesh and the N64 body
are hit the part agrees with the N64's in 1324 of 1618 cells against 1297
(the N64 sides with the change in 88 of the 163, with the old rule in 61).
Most of it is the pelvis/thigh line. The N64 body is a reference, not the
truth - where the shapes differ, either answer can be right.

**Three traps in re-running the grid.** The nearest chr by `prop->z` can have
**garbage matrices** at the stop - NaNs and 1e38s, a chr not posed this tick -
and the grid then shoots at nothing meaningful without an error; pick a chr
with `PROPFLAG_ONTHISSCREENTHISTICK`, not `CHRCFLAG_HIDDEN`, and
`matrices[0].m[3][3] == 1`. Write the ray with `%.9g`, since a `%f` of a
huge value is a literal gdb refuses ("Invalid cast"). And a grid from an
earlier session is **not a baseline**: a later build put the frame-1700 guard
at z 626 instead of 132, so build the commit (`git stash`) and run both.

#### Bruises (2026-09-12)

Report: "XBLA models don't have vertex painting yet for blood decals". The game
bruises a chr in `chrBruise()`: the stock vertex nearest the shot, and every
vertex at the same coordinates, gets an alpha of 20-70 in a copy of its node's
colour table (`VTXSTORETYPE_CHRCOL`), and the chr's combiner (`G_CC_CUSTOM_17`,
`(texel - env) * shade alpha + env`) takes it to the env tint. `chrDisfigure()`
darkens the same copies. The mesh's own vertex colours were none of those.

What is mirrored is the tables, not the shot (`struct xblameshbruise`,
`xblaMeshBruiseColours()`): made the first frame any of the model's covered
lists has a copied table, a map from each of the release's **solid** vertices
to its three nearest stock vertices in the rest pose (stock vertex + the rest
offset of the node its `G_MTX` names; the release's bind positions), searched
on the same bone first for a body (palette entry i is matrix i) and by position
for a grafted head; and each frame, a frame-arena copy of the mesh's colours
with every channel scaled by the weighted ratio of the stock entries' current
value to their original. Cutout vertices take none: a low shade alpha pushes a
cutout's clear texels towards opaque. A model with nothing copied costs one
pointer compare per covered list and draws the mesh's own table. The log says
`slot N takes the game's bruises: V of W vertices from S stock vertices in L
lists, matched on each bone`.

A grafted head's `G_MTX` names a matrix no position node of the head's own file
carries; the map asks `modelFindNodeByMtxIndex()` of the whole model the way
`chrBruise()` does, and falls back to the list node's own place.

Checked on the G5 Building (`bruise.py`: 288 `chrBruise()` calls over the
nearest guard's sixteen bbox nodes at frame 1700): 109 stock entries bruised,
449 of the body mesh's 6436 vertices drawn bruised. **A drive that calls
`xblaMeshSetEnabled(0)` saves `XblaMeshes=0` into the scratch pd.ini at exit**,
and the next run boots with the meshes off and looks exactly like a mirror that
does nothing - reset it before each run.

#### Shells and destroyed props (2026-09-14)

Report: "Rotation point of ejected shells on XBLA Falcon 2 is broken, sometimes
shells eject as two. Destroyed objects ... cameras mostly just disappear."

**A rigid mesh can carry its first part's rest offset.** A position node's
matrix stands at the node's own `rodata->pos` (`modelUpdatePositionNodeMtx()`),
and the node's lists are authored relative to it. `GcartridgeZ` is one position
node at z 31.95 over one **gun** list (`MODELNODETYPE_GUNDL`, 0x04 - not a
`DL`, 0x18) whose vertices span z -13..13; the release's shell spans z 19..45,
the model's space. Drawn under the part matrix it was 31.95 out, and
`casingRender()` spins that matrix about the node, so the release's shell
orbited a point three units off its middle - which reads as two shells. The game
makes exactly one per shot (counted in `g_Casings`). `xblaMeshRestShift()` asks
the geometry once per model and mesh - the mesh's box centre against the stock
lists' box with and without the offset - and takes the offset off a float copy of
the matrix only where the model's space fits better; the hit test takes the same
shift. Across a Combat Simulator match and Investigation's 107 mesh props it
fired on the shell alone, so nothing that drew right moved. The first version
read only `DL` nodes, found no stock vertices on the shell and answered "leave
it" - a `GUNDL` has its `vertices` and `numvertices` at the same offsets.

**A destroyed prop is `objDeform()`'s, and the door trim test read it as a
door.** `objDeform()` gives each list a copy of its vertices, jittered up to ten
units in all three axes and clamped to the bbox, clears every colour entry's
alpha but the first and points some vertices at that one, and squashes the
object's matrix along its upright axis. The trim test (`xblaMeshNodeTrim()`)
only asked whether the rw minimum x rose or the maximum y fell, which a jitter
nearly always does, and pressed the whole mesh onto that plane - "just
disappears". A trim now has to move vertices along its one axis onto the line and
nothing else. The deformation itself is mirrored through the bruise map, which
takes rigid meshes now (every vertex mapped by position, the solid ones
remembered for colour; each ref keeps its stock vertex and the list's colour
base): `xblaMeshDeformVertices()` moves each release vertex by its refs'
weighted `rw - ro`, and `xblaMeshBruiseColours()` reads the entry the rw vertex
names *now*. The matrix squash needed nothing. Checked on Investigation (0x33)
on the GPU: two lounge armchairs made destroyable from gdb (clear
`OBJFLAG_INVINCIBLE`, `objDamage(obj, 5000, ...)`) draw scorched and squashed
with the release's meshes, as the N64 pair does, and log `slot 2211 takes the
game's bruises: 438 of 438 vertices ... matched by position` and no trim line.
Security cameras were not found live on Defection, Investigation, Infiltration or
the G5 Building. A byte scan of every setup file for `06 00 af` (type, model)
hit Defection, Pelagic II, the Institute's training setup and Mr. Blonde's
Revenge, but only Defection was booted, and there no `OBJTYPE_CCTV` prop
existed in a `--spectate` boot and the camera model never loaded - the hits may
be coincidence. The camera itself is still to be seen.

Driving it: a frame-exact shell burst is a top-level gdb Python loop on a
`casingsRender` breakpoint with `casingCreateForHand(0, vv_ground,
&hands[0].posmtx)` at the first stop and `screenshotRequest()` per `continue`;
`bgunSetTriggerOn()` from gdb does not fire, since the input tick overwrites it.
A Combat Simulator simulant kills the player through `invincible = 1` and every
frame comes out "Press START": use `--mpsims 0`. Look direction is
`(-sin theta, 0, cos theta)`, so `vv_theta = 0` faces +z after a `prop->pos`
teleport under `--spectate`.

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

**Most of the release is still the ROM's size, and Enhance Textures has to
reach it (2026-09-12).** 2033 of the 3503 numbered records are the ROM's own
dimensions - 4J upscaled the rest - and the branch this hangs off uploads a
pack's image un-enhanced, since a pack's author chose its size. So with the
switch on, every one of those 2033 lost Enhance Textures and drew blockier than
with the switch off. Reported as "the create agent thumbnail is low res and
fuzzy": the file select's portrait (063c, "New Agent..." and "New Recruit") is
56x36 in the release while its 21 stage neighbours are 320x192 renders, so it
was the one picture on that screen where the difference showed. A release
picture no bigger than the padded tile is enhanced now, with the same edges and
clamp as the game's own texels (`gfx_set_import_enhance()`); a real upscale and
a player's pack go up as they are. Which means the pixel-identical check above
holds with Enhance Textures off: on, the switch now beats the converted pack on
those 2033. There is no hi-res New Agent picture anywhere in the release to
use instead - every record with a 56x36 source was listed.

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
- **Nor when the model draws a picture the mod supplies (2026-09-14).** A mod
  can keep the game's file and repaint it through its `textures/` (or a Stage
  Loader map through its own), and the release's mesh brings the release's own
  pictures, so the prop came out as 4J's and never the mod's - "the doors are
  rendered from the XBLA release in mods that don't use those textures".
  `xblaMeshMatchModel()` asks `modTextureExists()` for every texture config
  under `NUM_TEXTURES` and leaves the model alone on the first one the mod has.
  By number, because `modeldefLoad()` matches before `modeldef0f1a7560()` loads
  the configs, so the texture registry has nothing to say yet. On GE-X 5a's
  Defection it keeps 14 stock dataDyne props off the release (the lift, the
  fans, the hover cars and taxi, the sofa, the jumpship; 4 still pair); stock
  Defection pairs all 67 as before and the check never fires with no mod.
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

### The boot logos are the release's own intro (2026-09-13)

The one place where 4J's mesh is not a higher-poly version of the same object
is the boot sequence. Refused outright from 2026-09-10 (when the release's
logos turning up was reported as "the Rare logo, Microsoft logo and Nintendo 64
logo are all discoloured"), they are drawn on purpose since 2026-09-13, when
the user asked for the release's intro with the release's switch:

| file | N64 | release mesh |
| --- | --- | --- |
| 1376 `PrarelogoZ` | gold R on a blue plaque | the orange Rare logo (R, TM, RARE) |
| 221 `PnintendologoZ` | Nintendo wordmark | Microsoft Game Studios |
| 224 `PnlogoZ` | the cube the N64 logo morphs into | 4J's marble Perfect Dark cube |
| 222 `Pnlogo2Z` | the N64's first, coloured cube | 4J's logo: a red cube with the 4J emblem cut into its top and bottom |

There is no Xbox 360 logo in the package. The 4J Studios logo is file 222's
mesh, which this section first called "a flat red box": from the side the N64's
spin looks at, that is all it is (see "The release's boot sequence, recorded"
below). Five things it took, each of which looked like something else:

- **The Rare mesh is ten times the plaque** (7115 by 10146 against 773 by
  1041). At the game's scale it is an orange R too big to see, which is the
  "orange field to the edges of the screen" the 2026-09-10 note described.
  `titleRenderRareLogo()` scales by a tenth when the release draws it.
- **A mesh named under a toggle filed no covered lists.** `firstslot` is only
  set for a mesh on a list the game draws beside the others (the `CheadgreyZ`
  rule), and both the Rare logo's mesh (under toggle 0x0b) and the cube's
  (under 0x0001) are toggled, so the log said "N more lists the mesh covers"
  and nothing was suppressed: the N64's gold R drew over the Rare logo.
  `xblaMeshIsReleaseBootLogo()` lets these two set it, and covers their
  toggled lists too.
- **The cube's mesh is on the morph's target sides**, parts 0x0003-0x0009 under
  toggle 0x0001, which the N64 never shows: it copies the visible sides'
  vertices towards the targets instead, a morph no mesh can follow.
  `titleRenderPdLogoModel()` shows 0x0001 for the cube when the release draws
  it, and `titleRenderPdLogo()` starts on the finished cube at frac 1, so the
  N64's first cube and the morph out of it (Nintendo's four colours) never
  draw. The spin, stop and fade keep the N64's timeline.
- **That model's colour tables are normals.** Drawn as colours, the target
  sides are saturated green, yellow and blue; that is what "the Nintendo
  colours on 4J's cube" was, and it came from stock lists that were not
  covered, not from lighting or segment 5.
- **A toggle's child is only linked while it is visible**, so
  `xblaMeshModelHasMesh()` (which walks the tree) says no for both logos.
  `xblaMeshModeldefDrawsMesh()` asks the node table instead, and the title
  asks it once as each logo starts (`g_TitleXblaLogo`, `g_TitleXblaCube`).
  F6 is ignored while `titleIsBootSequence()` is true, so the answer cannot
  go stale mid-intro.

Not matched to the N64 look: the stock cube's sides fade by ambient light
through lists the mesh covers, so 4J's cube stays lit behind the title where
the game's goes dim.

Reproducing it: a scratch savedir from the tester's `pd.ini` **with `ModDir`
emptied** (it mounts GE-X, whose own `PnintendologoZ` and `PpdtwoZ` make the
matcher refuse them as a mod's) and a controller-free binding, then a gdb
script that stops `titleTick` at `g_TitleMode`/`g_TitleTimer` pairs (Rare 4,
Nintendo 3, PD logo 2) and calls `screenshotRequest()`. Run it with
`--fixed-step`: the spin advances by `lvupdate60freal`, so without it every
gdb stop is a long frame and two runs disagree about where the logo is.

### The release's boot sequence, recorded (2026-09-13)

Two references, neither of them memory of the N64 intro: a recording of the
release in Xenia Canary for the order and the look, and a log of the release's
own draw calls for 4J's exact animation. Xenia presents nothing on Xvfb (RADV
wants DRI3) unless `MESA_VK_WSI_DEBUG=sw` is set, it ignores SIGTERM, and
`--license_mask=1` unlocks the package. The release reaches its logos about
4 s after launch and replays the intro after its attract mode.

| stage | the release |
| --- | --- |
| XBOX LIVE arcade splash | ~1 s (Microsoft's platform art - not ported) |
| legal screen | "Xbox 360 Product Identification", the N64's length |
| Rare | the orange logo, the N64's animation and length (248 ticks logged) |
| Microsoft Game Studios | the whole Nintendo slot (246 ticks) |
| the Perfect Dark logo stage | 4J's own animation, below |

4J's stage, in ticks since it started (60 a second), read out of the draw log;
`port/include/xbla4jintro.h` is generated from it:

| ticks | |
| --- | --- |
| 0 - 114 | **the 4J Studios card**: the red cube face on (pitch exactly 90), still; "4J STUDIOS" fades out 55.7 - 112.0 |
| 0 - 560 | both cubes' scale grows linearly from 0.3 to exactly 0.5 (0.000357 a tick) |
| 114.7 - 560.5 | the spin, from rest to exactly 720 degrees (a per-tick table: no simple curve) |
| 227 - 449 | the pitch, easing from 90 to exactly 0 (per-tick table) |
| 308 - 448 | the Perfect Dark cube stretches from half its height to its own and fades in, linearly together |
| 336 - 420 | the red cube fades out, linearly |
| 544 | PERFECT DARK first drawn |
| ~780 | cut to black (the cube draws in curves.csv run to 777, and the recording holds the whole logo to 30.4 s; this row said ~735 until 2026-09-14) |

**4J does not morph the geometry.** Both cubes are drawn in one pose; the
Perfect Dark cube is squashed to half its height (the red cube's shape) and
stretched back as it fades in, which stands in for the N64's morph from cube to
logo. And **4J kept the N64's title camera**: vertical fov 46, 4:3 (stretched to
16:9 on screen), looking at the origin from 4000, so 4J's scale is the port's.

**How it was read** (`tools/xblaintro/`, with a README). Xenia's GPU trace is
compiled out of release builds, so Xenia was built from source with a small
hook (`xenia-drawlog.patch`): one record per draw - frame, index count, the
first 64 vertex and 16 pixel constants, blend state - and a host timestamp per
swap. The boot meshes are picked out by index count (the release's own draw
tables: red cube 1185, Perfect Dark cube 600/168/192, Rare 19620, Microsoft
14868). Each draw's world-view-projection is vertex constants c2-c5, row major
(`clip = [x y z 1] . M`), and dividing out the N64 camera leaves an exact
rotation times a uniform scale (orthogonality error 0). The colour pass's alpha
is pixel constant c1; the name's glyph quads carry theirs in c0.

**What the port draws with the switch on** (title.c):

- The legal screen is `g_LegalElementsXbla`: the release's lines, with the
  port's own branch, ROM and build in the three rows the port fills in, and
  `(c)` for the copyright sign the fonts do not have.
- Microsoft Game Studios and the N64 Nintendo logo share the Nintendo slot,
  half each, each with the whole N64 animation at double speed
  (`g_TitleXblaNintendoSplit`). **The user asked for this** so every company is
  credited; the release shows Microsoft alone. The Nintendo half is the same
  model with `xblaMeshSetBypass()` on.
- The 4J stage is `g_TitleXbla4J`: both cubes posed from `xbla4jintro.h` on
  stage ticks (`g_PdLogo4JCardTimer + g_PdLogo4JSpinTimer`), the Perfect Dark
  cube stretched along its own up axis. The card holds the N64's timeline, and
  `g_TitleTimer` with it, for `XBLA4J_CARD_TICKS`; the fades are the game's own
  (`renderdata.unk30 = 5`, the alpha in `envcolour`, which the mesh's lists take
  from the node).
- The N64's timeline still presents PERFECT DARK and exits. On the release's
  timeline the side-darkening wait runs four times as fast, which puts the
  title on tick 544 in both, and the wait after the last presentation step is
  146 ticks instead of 60, which puts the cut on about 780.
- **PERFECT DARK was presented twice until 2026-09-14** ("pops up twice at the
  end"). `titleRenderPdLogo()` starts the darkening twice, 100 ticks after the
  morph and again when the spin comes to rest. On the N64 the second start
  lands inside the first; the release's four-times darkening had already
  finished, so the second armed the pre-title timer again and the title
  started over from step 1 (t431, then t502). The timer is now armed only
  before the first presentation. The old exit wait of 30 ticks had been tuned
  against that accidental second presentation, which is how the cut came out
  near 735 and looked right.
- **Microsoft Game Studios draws into a depth buffer with the release's
  reflection** (2026-09-14, "the microsoft logo is off"). The release binds a
  512x256 brushed texture and a cube map to it, the 4J cubes' shader. Drawn
  the N64's way, with no depth buffer, the letters' extruded walls painted over
  their faces and no reflection was drawn at all, so the logo came out hollow
  and navy. **Its fade is the dark room's blend.** The mesh's colours are
  baked and its node is mode 0, so the N64's light never faded it: it popped
  in and out at full brightness (true before this change as well), and the
  added reflection would not have faded with the light either. The half is
  drawn with `unk30 = 4` and the environment colour's alpha at 255 minus the
  light's level, which blends the lists towards black and, through
  `xblaMeshEnvironmentLight()`, scales the reflection by the same amount.

**A mod owns its intro.** Any of the Perfect Dark logo's files shipped by a mod
(GoldenEye X ships them all) turns the release's version of that stage off, and
the release's meshes stay off the cube files it left alone; a mod's options
language file keeps its own legal screen. File 222 stopped being refused for
this, and at once 4J's red box drew in place of GoldenEye X's first cube -
which is what the bypass round the cube draw is for.

Traps, each of which cost a run:

- **Swaps are not ticks.** The game's clock follows vblanks, which Xenia raises
  at 60 Hz of real time, and it drew at about 42 fps: the Rare logo is 180
  frames and 248 ticks. The hook's swap timestamps are the clock.
- **Shader objects are host pointers**: a shader id from one run means nothing
  in the next. The name's glyphs are found as the vertex shader with the most
  six-index draws in the frame (nine: one per letter).
- Each frame is rendered three times with the same constants, and each cube
  twice a pass - depth only (colour mask 0), then colour with SRC_ALPHA blend
  and no depth write. That pre-pass is how 4J fades a closed box without its
  inside showing, and the port does the same (below).
- **The title draws with no depth buffer**, and every mesh draws both faces:
  the red cube tipped back as an open cup. Culling alone (its lists are built
  with `xblaMeshBuildCullBack`, and the title clears the culling after) is not
  enough for the tray: its rim is concave, so as it tipped back the far walls
  of the recess still painted over the emblem. Since 2026-09-13
  `titleRenderPdLogo4JCube()` clears a depth buffer for each cube and draws it
  with `G_ZBUFFER` (`g_TitleXblaModelZbuf` makes the node write its z-buffered
  render modes), and a cube that is fading is drawn twice, the release's way:
  a depth-only pass (`TITLE_RM_DEPTH_ONLY`, which gfx_pc draws "invisible", at
  alpha zero, while still writing depth), then the blended pass in
  `G_RM_AA_ZB_XLU_INTER2`. The blended pass must be ZMODE_INTER, because the GL
  backend compares ZMODE_OPA and ZMODE_XLU with GL_LESS and would reject a
  surface at its own pre-pass depth. The mesh takes both modes through
  `xblaMeshSetOpaqueMode()`. **The release winds its triangles the other way
  round from the game:** it is `G_CULL_FRONT` that drops the faces turned away.
- **The marble cube is the right size.** A reading of "18% too small" came
  from a blue colour mask that missed the cube's dim tips (and the release
  frames are 1280x695, cropped from 720 at the bottom). Its lit extent at stage
  tick 711 is 0.725 of the frame height in the port, 0.735 in the release and
  0.737 when the mesh is projected with 4J's pose. The draw log gives both
  cubes the same scale at every frame.
- **`text0f1552d4()` is the credits' 3D text**: its glyphs are vertices for the
  credits' projection, and on the title it draws nothing. `var80080108jf` (the
  height scale) is Japanese only, and `var8007fad0` alone only widens a glyph.
  `textRenderScaled()` (game_1531a0.c) draws a line at a whole multiple of the
  font's size.
- **Building Xenia for it**: `git submodule update --depth 1` left 26
  submodules on branch tips rather than the recorded commits (check each out
  at `git ls-tree HEAD <path>`); `xb build`'s configure step fails before it
  writes `build/version.h` (write it by hand and build with `cmake --build`);
  the link wants `lld`; the shaders want the LunarG SDK (the system's
  SPIRV-Tools 2025.1 is too old).

Not matched: the release's lighting on both cubes (4J's shaders; see the
lighting task), 4J's own lettering for the name (the port uses the large Handel
Gothic at twice its size), and the splash. Whether 4J clears depth between the
two cubes is not in the draw log; the port gives each cube its own, so the
crossfade is two whole pictures laid over each other.

**The recording's stills are not on the stage clock; the port is (2026-09-13).**
The comparison sheets paired port ticks with stills from the Xenia screen
recording (`boot.mkv`, 30 fps), anchored by eye at 18.0 s = stage tick 33 and
counted at 60 ticks a second. That made the port's tray look like it tilted
late at tick 311, and it does not. Fitting the red cube's silhouette along
4J's own pose curve (the red mesh projected with curves.csv's scale, pitch and
spin; IoU 0.95-0.99) gives:

- **The port lands on its nominal tick** to within a tick at 141, 201, 261,
  311 and 351.
- **The recording shows a tick about 5 earlier than its label** (94 frames:
  mean -4.9, sd 2.3, no drift). A 30 fps recording of a game Xenia presented at
  an uneven ~42 fps jitters by that much, and at tick 311 the pitch moves half
  a degree a tick. The still labelled 22.63 s is tick 303; tick 311 is the
  frame at 22.767 s.

So judge the stage against a still picked by its **fitted** tick, never by the
recording's time. The fit is only well-posed while the red cube is moving and
opaque (stage ticks ~120-335): on the card it holds still, and from 336 it
fades out. Do not move the port's timing to match a still. The tables come from
4J's exact draw matrices, and the silhouette fit agrees with them.

**The red cube's tray: its red matches, but the release adds a highlight
(2026-09-13).** Asked to match "the emblem lighting inside the tray". What the
release gives this mesh (slot 2281, texture record 4384 = dump 1120, material
word 0x010f1120: byte 16 is 15 and byte 24 is 1):

- **No light at all in its shader constants.** Vertex c0 is a fixed
  (1, 0, 0, -0.99) and c2-c5 the world-view-projection. Pixel c0 is only the
  "4J STUDIOS" name's fade (alpha 1 to 0 over ticks 54-110), c1 the cube's fade
  alpha, and c2 and c3 fixed.
- **Its shading is baked into the vertex colours.** The texture is a flat
  64x64 (139, 13, 13). The vertex colours are ten greys from 116 to 255, and
  every up-facing vertex (rim top, tray floor and emblem top alike, all
  between heights 1151 and 1218: the tray is shallow) is 255.

A z-buffered numpy raster of the mesh in 4J's pose, interpolating colour and
normal, was checked against the port's own shots first: red = 139.2 x vertex
colour explains 96% of the port's pixels. Against the release frames at fitted
ticks 201, 261 and 311, each region's red divided by the same frame's rim top
(so the recording's levels cancel) comes out as:

| region | release | port |
| --- | --- | --- |
| emblem top | 1.01 | 1.00 |
| tray floor | 0.99 | 1.00 |
| inner walls | 0.94 | 0.94 |
| outer walls | 0.63 | 0.67 |

That table is **red only**, and its "emblem top" and "tray floor" rows are
both halves of the emblem: the mask split up-facing pixels by height, and the
emblem's top is one plane sloping from 1211 to 1158. A label image showed it.
So the red channel matches, and the table says nothing about emblem against
tray.

**The highlight is in green and blue.** Per region at tick 311, over the rim
top: the release's emblem top is red 1.02, green 1.13, blue 1.13 (the port's
1.00, 1.03, 1.03), and the whitish excess (G+B)/2 - (13/139)R is +5.4 on the
emblem against +0.7 in the port. At 261, with the emblem face-on, it is 0.0;
at 201 it is slightly negative. The recording is yuv444p, so this is not
chroma smear.

- A mesh-fixed light explains none of it, and a view-fixed diffuse light fails
  tick by tick.
- A Fresnel term (1 - n.v)^k is ruled out: it predicts +12.6 on the ring and
  +22 on the outer walls, where the release has about 0.
- **A reflected-ray (Phong) highlight fits.** max(0, r.L)^16, with r the eye
  ray reflected per pixel and L ~ (-0.09, 0.68, 0.73) in view space (above and
  behind the camera), explains 59% of the emblem's excess across all three
  ticks. It gets each tick's mean right (-2.9 / -0.7 / +5.4 against -3.7 /
  0.0 / +5.4) and predicts the other surfaces roughly.

**Superseded the same day by "The release's reflections" (below).** Kept for
how the fit went and what misled it.

**Drawn since 2026-09-13.** The texture is flat red, so the highlight cannot
go in through the vertex colours (red times anything stays red); it is added
after the texture:

- The builder keeps each vertex's normal for files 222 and 224 only
  (`xblaMeshBuildKeepNormals`, set beside `xblaMeshBuildCullBack`). A vertex
  of a material whose byte 16 is zero keeps a zero normal, which takes no
  weight (`highlit`, set in `xblaMeshSetMaterial()`).
- `titleRenderPdLogo4JCube()` calls `xblaMeshSetHighlight()` with the cube's
  own model-view matrix round each cube's draw, and clears it after.
- Each frame, `xblaMeshHighlightColours()` binds a copy of the vertex colours
  whose alpha is strength x max(0, r.L)^power, per vertex in view space.
- The opaque list's combiner becomes TEXEL0 x SHADE, then
  ENVIRONMENT x SHADE_ALPHA + COMBINED, with the highlight's colour written
  as the environment's RGB just before the list. The alpha is the texel's, or
  the environment's while the title fades the cube (the env write keeps the
  fade alpha the node set), and is passed straight through the second cycle.
  The environment is used rather than the primitive colour, which the title
  uses for other things; the node's own fade combiner only reads env alpha.
- gfx_pc keeps a vertex's alpha whether or not G_FOG is on, and computes fog
  from z, so the weight is safe in shade alpha.

**The constants are fitted to what the recording can measure once its levels
cancel**: how much more whitish excess the emblem top gets than the rim top at
ticks 201 / 261 / 311, and how much the rim gains between them. In port levels
the release has -1.0 / +0.4 / +2.1 and +3.0 / +4.8. A search over light
direction and power, with the strength solved by least squares, chose L =
(-0.024, 0.686, 0.727), power 8, strength 0.077. The port with it measures
0.0 / +0.6 / +1.5 and +2.4 / +4.6; before it, 0.0 / 0.0 / +0.5 and 0.0 / +0.2.

Three things that looked right and were not:

- The first fit (power 16, strength 0.15) had an offset absorbing the
  recording's colour shift. Drawn, it doubled the rim's gain and gave the
  emblem no margin over the rim.
- A per-pixel, per-channel fit over every surface liked a sharp lobe (power
  100, L (-0.04, 0.87, 0.49)). Against the offset-free targets it scores
  1.9 rms against the broad lobe's 0.6: it lights nothing until the tray has
  tipped, then too much.
- Per-vertex evaluation is not what limits the match: on the tray's big flat
  triangles per-vertex and per-pixel give the same region means to within
  half a level, for either lobe.

**The release's reflections (2026-09-13).** What the tray's "highlight" and
the marble cube's bright bevels really are. Asked to "match the marble bevels
to the release", after a pale blue Phong highlight had been fitted to the tray
and put on both cubes. The bevels stayed a dark gradient against the
release's light metallic grey (port 10 against release 60, port levels), and
no highlight fit closed it.

**Found in the draw log, not fitted.** The Xenia hook's second record version
(PDD2, tools/xblaintro) logs the texture fetch constants and each shader's
bindings. The pixel shader the red cube and the marble cube's first two draws
share samples two textures: the material's own at fetch 0 (mips 0-6, linear
mip filter) and a **cube map at fetch 2** (256x256 DXT1, six faces). Material
byte 24 picks the cube: the bevels are 0, the red cube and the marble's faces
1, the Rare logo 2, Microsoft Game Studios 3. Those four sit at consecutive
addresses in that order, 0x60000 apart, so index i is record 0e93 + i: 0e93 a
grey studio, 0e94 blue, 0e95 orange, 0e96 white, then 0e97, 0e98 and 0e9c.
Byte 16 is the percentage. The marble's third draw (record 1118, the black
strips) has byte 16 zero and shaders of its own.

**The model:** colour = texture x vertex colour x (1 - k) + cube(r) x k, with
k = byte 16 / 100 and r the eye's ray reflected in the normal, in view space.
The lookup is Direct3D's cube convention (+X (-z,-y), -X (z,-y), +Y (x,z),
-Y (x,-z), +Z (x,-y), -Z (-x,-y)), with the faces in the record's row order.
Checked per pixel against the release at the still pose (tick 700):

- **Bevels** (cube 0e93, 50%): R² 0.39 with the texture alone, 0.93 with the
  cube.
- **Faces** (0e94, 40%): R² 0.88 alone, 0.98 with the cube.
- **A free fit on the faces** gives texture x 0.63/0.65/0.75 and cube x
  0.38/0.40/0.42: a blend, not an addition. Added, the faces drew 10-15
  levels too bright.
- **The tray** (0e94, 15%) matches in structure: its emblem is darker than
  its rim at 201 and brighter at 311, which the Phong lobe could not do.
- **The "darker recording" was mostly this blend.** Red at 0.85 of its
  texture is 118 against the 139 the port drew. The recording still crushes
  its dark end by about 5 port levels: the black strips, with no reflection,
  model 16-20 and record 6.

**Drawn since 2026-09-13:**

- **Build** (`xblaMeshBuildEnvironment()`, files 222 and 224 only, the builds
  that keep normals). Each vertex keeps its material's cube index and amount.
  Each cube the mesh reflects becomes a 256x256 sphere map for a viewer
  looking down -z, one cell in one atlas (`xblaTexDecodeCube()`,
  `xblaTexBindImage()`). The mesh's lists are copied with every `G_SETTIMG`
  pointed at the atlas and every `G_DL` into the lists relocated.
- **Each frame** (`xblaMeshEnvironmentVertices()`): each vertex gets
  sphere-map UVs for its reflected ray and the amount in its alpha. The main
  list's colours are scaled by 1 - k, and the copy is drawn over it with
  `G_RM_AA_ZB_XLU_INTER` (LEQUAL, no depth write) and `G_ADDITIVE_EXT`.
- **The renderer's additive blend is new** (`gbiex.h`, gfx_pc, gfx_opengl:
  SRC_ALPHA, ONE). Before it the renderer had only alpha and modulate.
- **The title** sets `xblaMeshSetEnvironment()` round the colour pass only,
  never the depth-only pre-pass. The Phong highlight is gone.

**Result** (port levels, release through the recording's levels):

| | Release | Port before | Port now |
| --- | --- | --- | --- |
| Bevels, tick 700 | 59.7 / 62.5 / 67.5 | 10.3 / 10.9 / 10.4 | 57.8 / 60.2 / 64.3 |
| Faces, tick 700 | 18.5 / 24.8 / 47.1 | 18.5 / 25.3 / 40.7 | 22.1 / 28.0 / 48.0 |
| Tray emblem/rim G, 201/261/311 | 0.92 / 0.99 / 1.13 | 1.00 / 1.04 / 1.07 | 0.95 / 1.05 / 1.13 |

**Not matched:** the sphere-map UVs are interpolated per vertex where the
release reflects per pixel; the cube's mips are not used; and the atlas is
built for the title's fixed camera looking down -z.

**Traps, each of which misled once:**

- **The cube records are DXT1** (format 18), with six 0x8000 base faces and
  then 0x30000 of packed mips. Split as six 0x10000 faces, the last three
  decode as mosaics of mips. The texture dump skips these records.
- **Numpy namespaces collide.** Executing the red cube's raster setup after
  the marble's rebinds the mesh globals, so "the marble" was the red cube
  with every triangle labelled draw 0 (negative amounts, an empty draw 1). Set
  each mesh up in its own namespace.
- **There is no Fresnel or angle term** (amount by cosine bins wanders with no
  trend) and **no gloss mask** (alpha is 255 on 1116, 1117, 1118 and 1120).
- **Rotating stills cannot tell view space from object space** (R² under
  0.4 either way, from the recording's jitter). View space is the standard
  lookup and makes the face pattern slide as the cube turns, as it does in
  the recording.
- **Earlier conclusions that were wrong:** "not sphere maps" was right about
  the textures 1116 and 1117 but missed the second sampler, and "base shading,
  not the highlight" was the reflection itself.

**Beyond the title:** 438 of the release's mesh draws have byte 16 non-zero
with byte 24 zero, and 40 more with byte 24 1-4. This is how the release
lights its shiny materials, and it is the route for the top lighting task.

**Capture trap:** `screenshots/pd-<stamp>.png` is named to the second, and
under `--fixed-step` shots 25 frames apart overwrite each other (seven of
eight were lost). Move each file away before the next can be taken. Doing it
with two `continue`s a frame put shots a tick off, alternately early and
late, and one tick at 311 moved the rim gain by 30%. One `continue`, then
collect. Check the shot's `spin` against the reference run (89 / 149 / 199
at 201 / 261 / 311) before comparing numbers.

Absolute levels in the recording are still not comparable: its rim top is
111-115 against the port's 139, and the legal screen's unlit text is darker
too (median 81 against 127). Compare by ratios within one picture, every
channel, and check any region mask with a label image. Scripts:
`$SCRATCH/raster.py`, `regions.py` and `levels311.py` in the session that
wrote this (a numpy rasteriser over tools/texpack/xblamesh.py). Rebuild from
this description if they are gone.

### The reflections on every mesh (2026-09-13)

The title's two cubes were the only meshes drawn with the release's
reflections. Every mesh has them now, behind **Mod.XblaReflections** ("Enable
Reflections" on the XBLA page, on by default like Enable Explosions, and part
of F6's whole-release switch).

**Scope, from the package** (`tools/texpack/xblamesh.py`, every mesh's draws):
231 meshes have a material with byte 16 non-zero, 131 of them skinned. Cube 0
(the grey studio) is 438 of the 478 reflecting draws; cubes 1-4 are the rest.
The percentages run 1 to 100, mostly 10-50. The reflecting slots include the
first-person guns (the `G` models sort between the chrs and the props), the
third-person guns a guard holds, doors, cars and consoles.

**What changed from the title-only version:**

- **Every build reads its normals** (`b.keepnormals = 1`); a mesh with no
  reflecting material frees them again in `xblaMeshBuildEnvironment()`.
  `xblaMeshBuildKeepNormals` is gone.
- **An atlas is made once per set of cubes** (`envAtlasKey`/`envAtlasTile`):
  before, a second mesh reflecting the grey studio decoded the cube and built
  the sphere map again, only for `xblaTexBindImage()` to free it.
- **The space is the node's own float matrix** (`root`), not a matrix the
  caller hands in. A posed copy is `posedfine` steps to a unit, so its
  positions are divided back before `root` takes them to the view. The lookup
  is in view space, so the sphere map made for an eye looking down -z is right
  for any camera: in its own space the eye always looks down -z. **Nothing
  about the atlas is tied to the title.**
- **A skinned mesh's normals are posed** in `xblaMeshPose()` by the same blend
  of the palette's rotations as its positions (`posednrm`, frame arena). Bind
  normals under a posed body point wherever the bind pose had the limb.
- **The room's light reaches the reflection.** The lists' colours are blended
  towards the node's fog colour by its alpha in the first cycle, so the
  reflection's share is `amount x (255 - fog alpha)`, baked into the pass's
  vertex alpha (`xblaMeshEnvironmentLight()`). Which colour is the fog colour
  follows `modelApplyRenderModeType3/4()` by `unk30`: the environment
  colour's for 4, the fog colour's for 5 and 7, none for the other values or
  for node modes 1 and 2. Modes 8 and 9 (cloak, shimmer) take no reflection.
- **No depth buffer, no reflection.** The pass is `ZB_XLU_INTER`: without
  depth it would add to the back faces too. This leaves out the boot
  sequence's Rare and Microsoft logos, which draw without depth although the
  release reflects cubes 2 and 3 on them.
- **One copy per model per frame.** A skinned model's fifteen parts all draw
  the whole mesh under the first part's matrix, so the reflection vertices
  and the scaled colours are kept for (model, frame, source), as the pose is.
  Both are made **before** the colours are bound: a frame arena with no room
  for the vertices leaves the colours unscaled instead of drawing the material
  darker with nothing added back.
- **The title** calls `xblaMeshSetEnvironment(XBLAMESH_ENV_OFF)` round the
  depth-only pass and `XBLAMESH_ENV_ON` round the colour pass, then
  `XBLAMESH_ENV_SETTING`. It is the release's intro, so its cubes reflect
  whatever the setting says.
- **The copy draws only the batches that reflect.** A batch is one material
  and opens with its `G_COL` then `G_VTX` (`xblaMeshWriteBatches()`), so a
  batch whose vertices all have amount 0 has its head and its `G_TRI1`/`G_TRI4`
  turned into `G_NOOP` in the copy. The offset is `(w1 & 0xfffffe) /
  sizeof(Col)`: `SEGADDR` sets the low bit on PC. The two guns in the Combat
  Simulator keep 82 of 131 and 87 of 273 batches (the build's log line says
  so per mesh). The per-frame work skips the same vertices: no reflection
  vertex is written, and no normal posed, where the amount is 0.
- **Posed normals only while reflections are wanted** (`XBLAMESH_ENV_WANTED()`).
  Tested on the copy alone, the pose blended normals with the setting off,
  which inflated the first "off" measurement.

**The first version cost 40% of the main thread** on the seeded 80-simulant
Combat Simulator match (`$SCRATCH/perfrefl.sh`, perfframes.sh without the mod
dir): 56.0M instructions/frame on against 40.0M off, 160 draws against 118.
A profile put `xblaMeshPose` at 21% flat (normals for every vertex),
`xblaMeshEnvironmentVertices` at 10% and `xblaMeshRound` at 4% (every vertex,
reflecting or not), and `gfx_sp_tri_emit` at 9% (whole lists drawn twice).
The two bullets above are the answer to that profile.

**After them**, same match (instructions/frame, main thread): HEAD 30.5M;
reflections off 31.8M; reflections on 46.9M, with 139 draws against 118. The off
path went from +9.5M to +1.3M. Culling took only about 1M off the on path,
so **reflections still cost about 15M a frame with 80 simulants**. What is
left is per vertex, per model, per frame: every simulant's gun is its own
model, with its own posed normals and reflection vertices (a few sqrt and a
division each). It scales with the number of reflecting models on screen, so
a normal match of a few simulants pays a small fraction of this. Both levers are taken in "The cutoff and the faster
loop" below. Culling left the
picture pixel-identical: the title at 201/311/540 against HEAD, and Combat
Simulator frames 300 and 600 against the build before it (300 differs only
inside the fps counter).

**Checked on the card** (offscreen, `--fixed-step --rng-seed 1`, gdb stopping
`videoEndFrame` on a condition, `$SCRATCH/shoot.py`):

- **Title:** at `g_PdLogo4JSpinTimer` 201, 311 and 540 the new build is
  pixel-identical to HEAD's. The spin table is 563 ticks (`XBLA4J_TABLE_TICKS`),
  so a condition of 700 never fires and the run sits out its timeout. The
  "tick 700" in the section above is on the recording's clock.
- **Chicago** (0x1d), level frames 600 and 900, on against off: frames 600-900
  are still the opening cutscene. 600 is identical. 900 differs in 526 pixels:
  the fps digits, and a faint grey sheen on a dark model behind the far car.
  Slots 2290 and 2337 built reflections there.

**The cutoff and the faster loop (2026-09-13).**

- **Reflection Cutoff** (the Xbox 360 (XBLA) page under Reflection Style,
  hidden while reflections are off; it was "XBLA Reflection Cutoff" on Dab's
  Mod Options' Display page until later on 2026-09-13;
  `g_ModOptions.xblareflectcutoff`, pd.ini `Mod.XblaReflectCutoff`, on in
  every preset and by default). With it on, `xblaMeshEnvironmentReach()` gives
  the reflection all its share within three quarters of
  `Mod.XblaReflectDistance` (metres, default 15, pd.ini only) and none past
  it, fading straight between. The distance runs from the eye to the mesh's
  nearest side: the root's view-space translation less `envradius` (the
  furthest vertex from the mesh origin, built once) times the matrix's scale.
  100 units are a metre (an Area 51 crate is 100 across). A zoomed view counts
  as nearer by its field of view against 60 degrees; a wider one never counts
  as further. The title (`XBLAMESH_ENV_ON`) is never cut.
- **The fade has to reach the base colours too.** They are dimmed by the
  reflection's share before the pass adds it back. Fading only the pass left
  them dim until the cutoff and then bright past it, a 15-50% pop at exactly
  the cutoff distance. The scaled copy is dimmed by `amount x reach` and kept
  per reach (`keptreach`). The room's light stays out of that: it darkens base
  and reflection alike.
- **Posed normals only for a draw that will reflect:** the opaque pass,
  within the cutoff (`xblaMeshPose(..., normals)`).
- **`xblamesh.c` at `-O2 -finline-functions`** (CMakeLists, beside
  port/fast3d). At `-Og` with the global `-fno-inline-functions`,
  `xblaMeshRound` and the matrix helpers stayed calls (4% flat on their own).
  There is no `-march` and no fast maths, so the floats, and the shot test
  that reads posed triangles, are unchanged.
- **`envidx`**: the reflecting vertices, listed at build. The posed normals,
  the reflection vertices and the dimmed colours visit only those.
- **One square root per call, not per vertex, for a rigid mesh's normals**,
  whose file normals are unit length: they come out as long as the matrix's
  x axis. Only when all three axes are that length (to 0.1%). The title
  squashes the marble cube in height while it morphs, and the shortcut there
  streaked the bevels' reflection by up to 55 levels at tick 311. A posed
  normal is a blend and is always measured. With the check, the title is
  pixel-identical to HEAD again at spin 201, 311 and 540 (without it, 201
  was up to 3 levels off too).
- **One eye ray for a distant model.** A profile of the no-cutoff run put
  `xblaMeshEnvironmentVertices` at 11% (inlined into `xblaMeshRenderNode`,
  whose flat time it becomes at `-O2`). A model more than
  `XBLAMESH_ENV_FAREYE` (20) times its own reach away (`envradius` times the
  matrix's largest axis) is seen along one ray to within three degrees. Every
  vertex takes the ray to its origin, and none is carried into the view: no
  position transform, no square root, no three divisions. The title's cubes
  (`envforce > 0`) never take it, and a gun in the player's hands is always
  nearer than that, so both keep the exact per-vertex ray.
- **`dimcol`**: the mesh's colours dimmed by each vertex's full amount, made
  once at build. A draw with no bruise within the cutoff's full share, which
  is most draws, binds it directly. Only a bruised or fading model makes the
  per-frame copy (3% in `memmove` before). With both, the title is still
  pixel-identical to HEAD at 201/311/540. Combat Simulator frame 300 is
  identical to the build before them, and frame 600 differs only inside the
  fps counter.

**Measured** on the same seeded 80-simulant match (instructions/frame, main
thread):

| | Instructions | Draws |
| --- | --- | --- |
| HEAD | 30.5M | 118 |
| Reflections off | 30.0M | 118 |
| On, cutoff on (15 m) | 37.8M | 129 |
| On, cutoff off | 43.4M | 139 |
| On, before this round | 46.9M | 139 |
| Then one eye ray far off + `dimcol`: off | 30.8M | 118 |
| ... on, cutoff on | 37.2M | 129 |
| ... on, cutoff off | 42.6M | 139 |

Reflections off is the same code in the last two rows' run and the one above
them, and measured 30.0M against 30.8M. Read each run's on and off against
each other, not across runs. Within its own run, the last round took the
cutoff-on cost from +7.8M to +6.4M and the cutoff-off cost from +13.4M to
+11.8M. Its seeded replay matches the build before it in all 80 `gfx:`
samples. What is left is mostly the renderer drawing the reflecting batches a
second time (`gfx_sp_tri_emit`, `gfx_run_dl`), which per-vertex maths cannot
reach.

The cutoff halves what the reflections add (+15.1M to +7.9M over off). The
loop work alone took +15.1M to +13.5M, and `-O2` made off cheaper than HEAD
(the pose itself is faster). The seeded replay is unchanged by `-O2`: all 80
`gfx:` samples (draws, tris, verts) match the build before it, on and off.
The Combat Simulator shot at frame 300 is identical to it. Frame 600 differs
only on a simulant past the fade's start, which is lighter as its reflection
goes. Chr frames of an on run and an off run are not comparable: at frame 600
the simulant standing there is a different model in each.

**Not done:** the cubes' mips are unused. The pass was per vertex until "Per
pixel" below, and a sphere map's rim smeared across a triangle whose vertices
reflected to opposite sides of it (grazing angles, silhouettes).

**View space is confirmed** for the held gun (2026-09-13), from the release's
own draw log in Xenia. The method is written up below: the title's fit could
not separate view space from object space, and a turning gun can.

### The release's reflections are in view space: the turning-gun check

**The capture.** The self-built Xenia with `PD_DRAWLOG` set was driven to
dataDyne Defection and turned on the spot with the Falcon 2 in hand, three
settled headings about 60 degrees apart. The log was 11 GB by then, so the
three windows were cut out to `.xbla-work/gunturn/windows.npy`
(`cubes.py`, `yaw2.py` beside it). The rig (`.xbla-work/gunturn/`):
- `pad.py`: a virtual Xbox 360 pad through `/dev/uinput`, fed from a FIFO.
  sdg has an ACL on `/dev/uinput`.
- Xenia runs `--hid=sdl` with the DualSense hidden
  (`SDL_GAMECONTROLLER_IGNORE_DEVICES=0x054c/0x0ce6`).
- xdotool clicks Xenia's own sign-in dialogs, holding each click 0.3 s.

**The gun's draw** is 303 and 756 indices with vertex shader `880e37e0`,
pixel shader `880dd800`, its texture at fetch 0 and a real cube map at fetch
2. A fetch is a cube when `dword_5` bits 9-10 are 3 (`xenos.h`,
`DataDimension::kCube`). Filtering on a 256x256 DXT1 texture instead catches
particles and quads. The release draws nothing under one camera matrix: c2-c5
are the projection alone, and each object's palette (c6 on, 3x4 blocks at a
tenth scale) already carries the view.

**What turns with the camera, and what does not:**
- c2-c5 are identical at all three headings.
- Every palette slot the gun uses (0-9, 11-18) keeps its rotation to 0.0043 of
  0.1, about 2.5 degrees of sway, and its translation's length to a unit.
- Slot 10 (c36-c38) is the only block that turns, and it is left over from
  other draws: a hand bone 41 units off at the first heading, a world object
  839 units off at the other two. Skinning the gun with it would throw
  vertices 8 metres, and the gun draws whole.
- The pixel constants ps c6-c13 (eight world-sized points) move by under a
  unit between headings. They do not rotate.

No input to the gun's draw rotates with the camera, so its cube lookup cannot
depend on heading: the environment is fixed to the eye, as the port draws it.
The props share the shader pair and the constant layout, with no world
rotation for them to use either.

**Traps:**
- The same-heading stills of the gun differ in most pixels from idle sway, so
  judging the sheen by eye or by crop difference proves nothing. The
  constants are what settle it.
- Grouping draws by index count and shaders alone mixes meshes.
- ps c0.w went from 1 to 0 between the first heading and the others. It is a
  flag, not a rotation.
- Xenia's `ConstantRegisterMap.float_bitmap` would say which constants a
  shader reads, but a skinning shader addresses its palette dynamically and
  marks all of them.

### Per pixel (2026-09-13)

The reflection is now looked up per pixel by the renderer, not per vertex by
the loader. One sphere-map coordinate per vertex, interpolated, is not a
sphere map sampled per pixel: the mapping is non-linear, and a triangle whose
vertices reflect to opposite sides of the rim interpolated across the whole
map.

**The renderer** (`G_ENVMAP_EXT` in gbiex.h, `SHADER_OPT_ENVMAP` in gfx_cc):
- **The normal rides in the vertex colour.** While the mode is on,
  `gfx_sp_load_vertex()` reads the vertex's colour as the signed normal an
  RSP light would read (`NormalColor`), and puts it and the position through
  the top of the modelview stack. The projection and the aspect adjustment
  come after eye space.
- **Six more floats a vertex** go out in `LoadedVertex.env`, an
  `EMIT_SLOT_ENV` slot after the grayscale colour, the same order the GL
  backend declares `aEnvNormal`/`aEnvPos` in. There is no CPU clipper, so
  nothing else has to interpolate them.
- **The fragment shader**, for texel 0:
  - It normalises both, reflects the view ray in the normal, and maps the ray
    to the cell's sphere map (r.xy / (2 |r + z|) + 1/2), held half a texel
    inside.
  - It samples at level 0 with `textureLod`. Where the ray turns away from the
    eye the coordinate wraps round the rim, and a mip chosen from that jump
    would draw a seam.
- **The cell is in the texture coordinates**, the same for a whole batch: s is
  (cell + 1/2) / cells and t is 1 / cells. The shader rounds both back to
  whole cells, so the pipeline's rounding (`texture_scaling_factor`) and its
  half-texel offset cannot move a lookup into the next cell.

**The loader** (`xblaMeshEnvironmentVertices()`) now writes only each
reflecting vertex's quantised normal (x127, a posed one normalised first),
its amount times the room light in alpha, and the cell code. The per-vertex
view transform, eye ray, square roots and the far-eye shortcut are gone,
since the renderer does the transform. The pass sets `G_ADDITIVE_EXT |
G_ENVMAP_EXT`. The combiner reads TEXEL0 and shade alpha only, so shade RGB is
free to be a normal.

**Checked on the card:**
- **Title:** against the per-vertex build, 7.7-8.4% of the pixels differ, by
  up to 31-48 levels, with the mean change under a level. The marble cube's
  facets and bevels carry finer detail and nothing else moves.
- **Combat Simulator:** the held guns keep their sheen with smoother
  gradients (11% of the frame, the guns, mean change under a level).
- **No faults:** no seams at the rim, no speckle, no neighbouring cell's
  colours.
- **Cost:** about what the per-vertex version cost, since the transform moved
  into the renderer. On the seeded 80-simulant match: off 30.0M, on 36.3M
  with the cutoff, 42.2M without (instructions/frame).

### The N64 sheen (2026-09-13)

A second look for the same materials: **Mod.XblaReflectStyle**, "Reflection
Style" under Enable Reflections, Xbox 360 (0) or K7 Sheen (1), live. The row
read "N64 Sheen" until the user renamed it on 2026-09-13; the code still says
`XBLAMESH_REFLECT_N64`. K7 Sheen is the default since later on 2026-09-13, after the user played with both
and found it much the better look; a pd.ini that already holds the key keeps
its value, and the game writes the key on exit, so anyone who ran the build
before keeps Xbox 360 until they pick the sheen.
The idea was the user's, after a frame-exact comparison of the K7 Avenger
drawn both ways.

**How the stock gun draws its shine.** The K7's lists (`Gk7avengerZ`, inflated)
are baked vertex colour except for three spans that set `G_LIGHTING |
G_TEXTURE_GEN` with `G_TEXTURE` at `0x0800` and texture `0x3eb` (a fourth reads
`0xb54`, within 17 levels of it). The K7 has no `WEAPONFLAG_00008000` ("special
environment mapping"), so `bgunRender()` sets no gun light, and what is live is
`lightsSetDefault()` from `bgRender()`: ambient 0x96, white along (0x4d, 0x4d,
0x2e), LookAt from the camera. gfx_pc's texgen takes (N·lookat + 1) / 4 per
vertex, Gouraud across big triangles, which is the hard banding.

**What the style draws** (`xblaMeshBuildSheen()`): a copy of `envgdl` - the same
batches, the same skipped ones - with `G_SETTIMG` pointing at `0x3eb` bound by
number (`xblaTexBindTexture()`, so a texture pack repaints it; the decoder is
`modelpackDecodeN64Texture()`, public now), every `G_TEXTURE` at `0x0800` (the
stand-in tile is 32x32, `0x3eb`'s own size, so the scale means what it did), and
the lists' own head clear of `G_LIGHTING | G_TEXTURE_GEN` taken out. The pass
writes `lightsSetDefault()` after the node's `G_MTX` (LookAt marks the
coefficients changed, so they are worked out under the mesh's own matrix),
`TEXEL0 × SHADE` with shade alpha, `G_LIGHTING | G_TEXTURE_GEN` and
`G_ADDITIVE_EXT`. The vertices are `xblaMeshEnvironmentVertices()`'s - the
normal in the colour is what an RSP light reads anyway - with the sheen's share
in alpha (`envsheen` keys the cache). Never on a forced draw: the title's cubes
stay the release's.

**Added, not blended - the finding that set the strength.** The first version
dimmed 4J's colours by the share and added the sheen back, as the cube pass
does. Every such variant drew darker than both games, from the release's own
amounts up to the whole (seed 2 gun box mean 66.9-71.6 against N64 77.4 and
Xbox 360 73.1). It was not the alpha: a white × shade-alpha combiner
saturated, the same as white × 1. `0x3eb` × the lit shade is mostly navy (4, 9,
28 on the body), and the N64's pale streaks are highlights on its paint. So
the sheen is added over the undimmed colours at `XBLAMESH_SHEEN_SHARE()`, 2.5x
the release's amount, capped: the K7's metal is 4J's 40% and becomes the
whole; its 15% becomes 37.5%. Result: seed 2 mean 88.0, seed 6 39.2, and seed
1 (gun shade alpha 229) 14.2, since the room's light still scales it. At the
full share the body washes pale blue, and dimmed to black it shows dotted seams
where the depth-equal pass misses AA edge pixels.

**Checked on the card** (`--rng-seed --fixed-step`, the K7 given at frame 60):
Xbox 360 style differs from the build before only in the fps digits; the N64
path is identical. Not measured: instructions/frame. The pass count is the
cube's, with gfx_pc's per-vertex lighting in place of the envmap transform.
Not in the preset table.

**Third-person guns borrow the first-person gun's amount (2026-09-14).** A
tester's F3 traces: guns shone in first person and on the menu but not in a
character's hands. The pass was never the problem - for the AR34's
`Pchrar34Z` (slot 2089) every draw gate passes in third person, and a replay of
the texgen over its normals spreads the streaks over the whole tile at 1.4 m
and 6 m as at arm's length (gfx_pc normalises the LookAt under the chr's tenth
scale, and the eye-ray bend normalises too). It is 4J's data: force-building
every gun slot (`call xblaMeshBuild(slot)` from gdb after `xblaMeshOpen(1)`,
slots from line 2 of `model-dumps/xbla/<name>.obj`) shows many `Pchr*Z` meshes
with no byte 16 in any draw while their `G*Z` mesh has one - cmp150, crossbow,
cyclone, devastator, druggun, dyrocket, fnp90, m16, maianpistol, maiansmg,
rcp120, shotgun, uzi, and `PchravengerZ` against `Gk7avengerZ`. So
`xblaMeshBorrowEnvironment()` gives such a mesh, over every vertex, the lowest
nonzero amount (and its cube) of the weapon's `hi_model` meshes, paired through
`playermgrGetModelOfWeapon()` and `g_ModelStates[].fileid` rather than by name;
the two atlases are different pictures, so no per-material pairing exists, and
the lowest amount keeps the grip from out-shining the barrel. A `Pchr*Z` with
reflections of its own keeps them, and a model pack's file borrows nothing.
Only weapons are walked (Falcon 2 to Psychosis Gun, less Combat Boost): the
items past them name a stand-in `hi_model`, and the briefcase paired that way
with `Gfalcon2lodZ` and shone whole at 60%.
Both Reflection Styles see it, since it is the material data they share.

Two traps from that hunt: `--savedir` reads `$S/pd.ini`, so a scratch save dir
with no ini runs on defaults (`XblaMeshes=0`, nothing matches); and setting
`players[0]->thirdperson` from gdb on an offscreen MP arena never produced a
third-person screenshot, even with `haschrbody` and `playerIsThirdPerson()`
both 1.

### Level Metal, the third Reflection Style (2026-09-14)

The user asked how the K7 sheen differs from the levels' metal and windows
(level-sheen.md, "The levels mark their own reflective surfaces") and wanted
the guns to have a mode like those. **Mod.XblaReflectStyle 2**, "Level Metal".
The default since later on 2026-09-14, at the user's request so testers have
it; a pd.ini written before that is moved to it once (`Mod.SettingsRevision`
in main.c), and left alone after.
It is the K7 pass with three things changed, and nothing else:

- **Picture:** `0x006d`, Defection's grey environment map (the user's pick over
  the blue 0042 and the lift chrome 038c), bound by number so a pack repaints
  it. `xblaMeshBuildSheen()` now makes two copies of `envgdl` through
  `xblaMeshCopySheen()`: `sheengdl` and `metalgdl`.
- **Scale:** read off the ROM, not guessed. A static walk of `bg_ame`'s rooms
  (the room walk of `tools/texpack/bgtexscan.py`, with `tools/extract`'s class
  exec'd *without* its tail, which extracts the whole ROM into the tree) finds
  all 74 texgen triangles on 006d at `G_TEXTURE 0x1000`, lit, not linear, on a
  64x64 picture: one sphere across the tile. The stand-in is 32x32, so
  `0x0800`. The other spans for reference: 0042/0043 at 0xb00, 0059 at 0xd80,
  009b/027e at 0x1000.
- **Walking turns it** (`roomSheenTexgenTurn()`, `G_TEXGEN_EYE_EXT |
  G_TEXGEN_TURN_EXT`), as on the rooms, where the K7's tiling streaks scroll. A
  scroll would run off the round map.

Kept from K7, on purpose: added over the undimmed paint at
`XBLAMESH_SHEEN_SHARE()` (the user chose "on top of paint"), and
`lightsSetDefault()`, which is `lightsSetForRoom()` at brightness 255 exactly
(ambient 150 = 0.588 x 255), the room's light already being in the alpha. It
covers every reflecting release mesh, like the other two styles.

**Checked on the card** (`--boot-stage 0x32 --mpsims 1 --rng-seed 1
--fixed-step`, the K7 given at frame 60, scratchpad `gunshot.py`): against K7,
0.33% of frame 500 changes, all on the gun, mean elsewhere identical; the rail
reads grey where K7's reads navy. A 120 unit move after frame 500 changes 29.7%
of the gun's shiny pixels at 510, against 5.2% for the sway alone.

### The classic guns: the sheen weighted by the paint (2026-09-14)

Report: "the xbla goldeneye classic guns, like ccmp, pp7i, kf7, etc colors are
off, the pp7i for example should be black". The PP9i drew as chrome under Level
Metal and as its own black with reflections off. The art was never wrong: the
atlas (record 1660) is a black Walther, and the texels under the main draw
average 68.

**It is 4J's material data meeting an added sheen.** A Perfect Dark gun marks
only its bare metal (the Falcon 2's dark draws are 0%, its metal 60%; the K7's
dark body 15%). 4J gave each classic gun **one reflecting material over the
whole gun**: PP9i 30% (50% on a small part), CC13 50%, KF7 10/30%, KL01313 25%,
ZZT 30%, DMC 20%, AR53 20%, RC-P45 40/20%. The Xbox 360 style blends the cube in
at that amount, and black stays nearly black. The K7 and Level Metal styles
*add* 2.5x the amount over the undimmed paint (`XBLAMESH_SHEEN_SHARE()`), which
is 75% of Defection's grey metal over black: chrome.

**The fix is `xblaMeshInkFile()`.** A mesh whose file is a classic gun's
`hi_model`/`lo_model`, or the file of `playermgrGetModelOfWeapon()`'s model
(WEAPON_PP9I to WEAPON_RCP45, so the third-person meshes too), keeps a byte a
vertex (`vink`). The byte is the mean luminance of the 7x7 texels about the
vertex's UV in its material's picture, decoded once per record while the lists
are built. The t row is the decode's own order, as uploaded. The sheen's
share is multiplied by it, in `xblaMeshEnvironmentVertices()` only. The Xbox
360 style, `dimcol` and every other mesh are untouched. The ink is taken
whether or not the material reflects, since a third-person mesh with nothing of
its own borrows an amount for every vertex after the build. The log names each
mesh: `xblamesh: slot N is a classic gun, its sheen weighted by its paint`.

**Checked on the card** (`--boot-stage 0x32 --mpsims 1 --rng-seed 2
--fixed-step`, `--mp-weapons 39,...` with `StartArmed=1`, frame 300, gdb
`'xblamesh.c'::optReflect = 0` for the off shot). The PP9i's box mean
(370-470 x 300-420) was 87.2 before, 76.8 after, and 71.2 with reflections off.
At seed 5 it was 31.2 / 25.3 / 22.2. All eight guns were looked at on and off:
the paint reads as the art, and the sheen is a highlight. Seed 1 spawns in a
room too dark to judge a black gun.

What this does not change: the CC13 is pale steel in 4J's own picture (160c),
and the RC-P45's long gold panel is its magazine (14d6 is a gold picture
labelled "P90 - 50 ROUND MAGAZINE"). Both look the same with reflections off as on.

### Logo Material: the title's marble logo in the levels' blue and metal (2026-09-14)

The user pressed F3 in front of the Carrington Institute's blue crystal statue
and asked for the spinning Perfect Dark logo in "the same blue material as
this statue", with "the metal the gray metal so it looks shiny and HD".
**Mod.XblaLogoMaterial**, "Logo Material" on the XBLA page: Xbox 360 (0, the
release's cube maps as before) or Statue & Metal (1, the default at the
user's request). Live.

- **The statue is room geometry, not a prop:** `bg_dish` (stage 0x26 is
  `STAGE_CITRAINING`) room 5 draws 28 translucent-layer texgen triangles on
  `0x0042`, the blue sphere map, at `G_TEXTURE 0xb00`, lit. Found with a
  per-room variant of `texgenscan.py` (level-sheen.md), keyed on the trace's
  `rooms 5*`. The trace's object list only shows props with a release mesh.
- **The logo's materials are baked sphere maps.** 4J's marble cube
  (`PnlogoZ`'s mesh) has three: record 1117 is a blue marble sphere (the
  faces), 1116 a grey one (the bevels), 1118 flat dark grey (left alone).
- **What draws** (`xblaMeshBuildLogo()`, `XBLAMESH_ENV_LOGO`): two copies of the
  lists on the first draw that asks. `logobase` no-ops the two materials'
  batches, `logogdl` keeps only them, with 1117 bound to `0x0042` (s 0x0755, t
  0x0800 on the 32x32 stand-in: the share of the 48x44 picture the room's
  0xb00 samples) and 1116 to `0x006d` (0x0800). Both by number, so Enable
  Textures serves the release's 256x256 pictures. The pass is `G_LIGHTING |
  G_TEXTURE_GEN | G_TEXGEN_EYE_EXT` from colours holding the bind normals,
  opaque, or `XLU_INTER` by the environment alpha while the cube fades in over
  its depth-only pass. The red tray has neither material and keeps the
  release's reflections.
- **Trap: the title has no LookAt.** `lightsSetDefault()` reads
  `camGetLookAt()`, which dereferences `g_Vars.currentplayer`. The pass writes
  its own light and an eye-space LookAt (right +x, up +y), which is the space
  gfx_pc's `calculate_normal_dir()` reads it in.

**Checked on the card** (offscreen, `--fixed-step --rng-seed 1`, the boot
capture at mode-2 ticks 455/540/600/700, one run per setting): the log says
"the title logo draws 26 of its 35 batches in the levels' blue and metal". The
faces are the statue's blue, the bevels grey metal, and the pattern moves
between ticks as the cube spins. The PERFECT DARK letters and the tray are
unchanged.

**Brighter bevels, and the Rare logo (later the same day).** The user asked
for "the metal bevels brighter and shinier" and "the rare logo shiny", then
said the guns' Level Metal look "would make the rare logo look good". The HD
006d is dark, so lit it read gunmetal. Now:

- **The table** (`xblaMeshLogoMats`) says per material whether the level's
  picture replaces the record (`texnum`, -1 keeps the release's), whether it
  is `metal`, and what is added over it (`XBLAMESH_LOGO_ADD_GLINT` or
  `_METAL`). The copies are a base, one per replaced material, a glint copy
  and a metal copy (`xblaMeshLogoCopy()`).
- **Bevels (1116):** unlit texel x (1 + `xblaLogoMetalGain` 0x80), plus the
  glint: 006d's own picture, brightness against its top half percent raised
  to the 4th power (`xblaMeshLogoGlint()`, bound as an image), added at
  `xblaLogoGlintShare` 0xc0 x shade. Result is bright chrome with moving
  highlights.
- **Rare R (1160, flat orange):** the guns' Level Metal, 006d added over the
  paint at `xblaLogoAddMetalShare` 0xff. It is unlit: the Rare stage's light
  swings off the R as it settles facing the camera, and a lit pass went out
  completely at tick 235. `xblaMeshSetLogoFade()` ramps it in with the
  stage's light (fracdone / 0.2).
- **Rare needs a depth buffer:** `titleRenderRareLogo()` drew with none, and
  the pass adds only where it matches depth. With the setting on, both its
  `modelRender()` calls use a cleared depth buffer and `XBLAMESH_ENV_LOGO`.
- **Normals:** `xblaMeshBuildEnvironment()` frees the normals of a mesh that
  reflects nothing of the release's. The Rare mesh is one, so a mesh with a
  logo material now keeps them (`xblaMeshHasLogoMaterial()`).
- **Tunables** are plain statics, so gdb can set them at the first
  `titleTick` stop, before the glint picture is made.

Checked on the card, same capture plus the Rare stage at mode-4 ticks
60/120/180/235: the Rare R draws "0 batches in the levels' blue and metal,
426 with a glint or the metal added". It is polished gold throughout, near
pale yellow once it settles.

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

**Invisible with Enable Textures on until 2026-09-12.** The ROM's colour ramp
on tile 1 is 14x14, which put every one of its records in the packed mip tail
(above), and a ramp decoded as its own mip levels multiplies the explosion away
to nothing - the ROM's puff as much as the release's fireball. Nothing below
was wrong; the picture behind it was. If an explosion goes missing again, check
that switch first: `xblaTexSetEnabled(0)` from gdb answers it in one run.

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

## The skies (2026-09-14)

`port/src/xblasky.c`, **Mod.XblaSkies** ("Enable Skies", on by default like
the explosions, and part of F6). The N64 draws its sky in `skyRender()` as a
colour, a tiled cloud plane and a water plane; 4J replaced all of that with a
**cube skybox and a cloud layer**, and none of it is a mesh - it is eight sets
of records and a few numbers, all of them read off the release's own draws.

**The records.** 0e56-0e92 are eight sets of seven: a 512x512 cloud picture,
then six faces in **Direct3D's cube order (+X -X +Y -Y +Z -Z), rows stored
bottom first**. The first faces are 0e57, 0e5e, 0e65, 0e6c, 0e73, 0e7a, 0e81,
0e8d (a grey storm, a red sunset, space with a planet, brown dusk, a fiery
evening, Defection's night skyline, a planet's rim from orbit, a blue day
above the clouds). The -Y face is often a 128x128 flat colour. 0e87-0e8b are
five loose pictures, not a cube, and 0e53-0e55 are three more noise layers;
nothing draws either yet. The layout was found by **joining the faces up**: a
numpy seam test over all twelve edges scores 0.9-4.9 with the rows turned
over and 6-60 for any other orientation or any other run of six.

**What the release draws** (Xenia, Defection, `.xbla-work/skycap/`): the six
faces first, depth test and write off, opaque; then the cloud picture as 64
quads, alpha blended, still with no depth. **4J transforms the sky on the
CPU**: every sky vertex carries its clip position (x y z w), UVs in 1/16384ths
(a face runs 0 to 16352, a hair short of one) and an ARGB colour, and the
shader passes it through. So the hook's buffer dump (tools/xblaintro README)
is the geometry, and fitting a 4x4 from the faces' corners (whose directions
the UVs give) to their clip positions is exact to 0.9 in 588, which inverts
the cloud quads back into a shape:

- **The cloud layer is flat**: 8 by 8 quads of 0.116 (in half-cubes) at 0.119
  above the eye, no curve with distance. The picture repeats 0.748 times a
  quad, u along one horizontal axis and v along the other. Vertex alpha is 0
  on the rim, 60 on the next ring in and 120 inside; colour white.
- **It drifts** 0.0245 and 0.0122 of a repeat a second, the same at 7.8 s and
  16.3 s of real time. **Xenia's swaps are not the game's ticks** - it drew 35
  a second there - so rates come from `draws.bin.swaps`, never frame counts.
- **The cube is mirrored against the game's world**: at Defection's spawn the
  fitted view looks down -z of the lookup with -x to screen right, and the
  game's camera (`camGetWorldToScreenMtxf()` from gdb at level frame 5400)
  looks down world +x with +z to its right, so world = (-z, y, -x) of the
  lookup. Without it the port drew the skyline backwards; with it the spawn
  view matches the release's first gameplay frame building for building.

**Drawn** at the top of `skyRender()` in place of the game's sky: the current
camera's rotation and none of its position under a `G_MTX_FLOATS` modelview
(so the stage scale stays out of it), the cube sized to half the far plane,
faces clamped, record stand-ins through `xblaTexBind()` (so a sky costs an
upload and no memory kept, and follows Enable Textures), then the clouds a
row at a time (18 vertices, under the renderer's 25) with `gSPColor` giving
each row its alpha. Suns, flares and everything after `skyRender()` are
untouched.

**The sky must hand `G_ZBUFFER` back** (fixed 2026-09-14, the day it shipped).
The frame turns the depth test on once, in `zbufSaveArtifactDepths()`, before
`skyRender()`, and nothing drawn after the sky turns it on again - the renderer
tests depth only while that geometry flag is set (`gfx_emit_prepare()`). The
first build cleared it for the cube and left it off, so every room, prop and
gun went out in list order: a tester's F3 on Defection had "most textures
transparent", the far towers over the near and the gun see-through. The stock
sky never touches the flag; the depth-less draw is the render mode's job. The
checks that shipped it looked at the cube and not at what stood in front of it.
Night levels' **star field** (`starsRender()` in bg.c: Defection, Extraction,
Infiltration, Escape, Attack Ship) is the N64's square coloured points and
drew over the cube's own painted stars, so it is skipped while
`xblaSkyIsDrawn()`. X-ray keeps the game's sky. Looking straight down in Defection's
intro is black, which is the release's own -Y face.

**Which level has which cube is the open question**: it is in the xex, which
is encrypted. Only what a capture shows is listed as recorded in
`xblaSkyStages[]`; see the table's comments for what is recorded and what was
chosen by the picture.

**Recording more of it needs no draw log - a still is enough**, since each
cube is plain to see (a labelled sheet of every cube's faces, rows turned the
right way up, is what the stills are matched against). A fresh profile has
only Defection among the missions, but **the release's Combat Simulator
offers every arena from the start**, so the arenas are the evidence:

- Skedar (0x32) is **0e81**: straight up is its +Y face, blue with stars, and
  its sides carry three suns - the N64 table gives Skedar three. So 0e81 is
  the Skedar planet, not a view from orbit, and it is the best guess for
  Skedar Ruins and War too.
- Ravine (0x17) is **0e6c**, the dim brown overcast, which matches the N64
  table's brown cloud colour and so backs Chicago's guess.
- Ruins (0x41) is **0e6c** too, seen through a gap in its roof, and Villa's
  arena (0x45) is **0e8d**, the blue day - which records the Villa mission,
  the same map, as well.
- Temple's arena (0x25) is **0e8d**, on thin evidence: the only sky in any
  still is a small flat-blue patch up a shaft, which is the day cube's +Y
  face (0e8f, a plain blue) and much lighter than the N64 table's navy.
- Grid, Area 52, Base, Fortress and Pipes showed only ceilings in every still
  (spawn, straight up, and four walks), and the Carrington Institute menu
  drew no sky cube at all, so all of them keep the game's sky.

**The missions are chosen by the picture** (2026-09-14, at the user's
request once the arenas were in), each row commented as such in
`xblaSkyStages[]`: Crash Site the sunset (red sky, the only sun on a
horizon), Attack Ship space, Skedar Ruins and War the Skedar cube, Chicago
and G5 the brown dusk (Ravine's cloud colour), Extraction and Mr. Blonde's
Revenge the city, Air Base and the three Area 51 missions the fiery evening,
Air Force One and Pelagic II the grey storm. Deep Sea, Investigation and the
Institute's levels have no row. A capture of any of them overrides its row -
nothing here was seen in the release.

The drive, scripted in `.xbla-work/gunturn/arena2.sh` / `arena3.sh`: Combat
Simulator, Game Settings, Advanced Setup, Arena, then Ready. Two traps: **the
arena list wraps** (17 entries), so "press up until the top" lands somewhere
else - move by a count from the arena already selected; and a match is left
by the pause menu's Exit Game, Yes, then Back on Game Over, which comes back
to the Combat Simulator menu with the cursor on Ready. Some arenas spawn the
player under a roof (Ruins did), so `arena3.sh` walks and looks up four times
before giving up. Keep anything heavy off the machine while a drive runs: the
presses are timed by the clock, and a slower Xenia takes them on the wrong
screen.

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
them calling `xblaMeshSetEnabled(0)` at a `break xblaSwitchTick if
g_Vars.lvframenum >= 450` (the tick is where F6 acts; never call it from a
stop at an arbitrary instruction, it frees the rooms) and `(1)` at 750,
with a gdb Python sum of `g_Rooms[i].gfxdata->numvertices` over the loaded
rooms at `videoEndFrame` on frames 600 and 900: the plain run has 7228
vertices in 5 rooms at both frames, the flipped run 7090 at 600 and 7228 at
900. The city's exterior rooms are nearly the same in both copies, which is
why the frame-600 screenshots differ by 0.13% of pixels (the dropship, a
model); inside a building the difference is plain.

### Two texture words the release gets wrong (2026-09-14)

Both were a tester's F3 shots, and both were found by diffing a room's spans
(texture, render mode, combiner per triangle run) between the ROM's copy and
the release's, rather than in the renderer. Do that first for any report that
only shows with the release's rooms on.

- **The detail texture is gone.** A subcmd 1 texture word keeps its second
  (detail) texture in bits 12-23; the release widened the number to 16 bits
  over them, so every two-texture surface it kept names detail texture **0**.
  That is 314 triangles in `bg_ame` (Defection's blue carpet) and 455 in
  `bg_mp15`, nowhere else, and no ROM room names 0 - all of them name 0x074.
  Texture 0's window grid was blended into the carpet as its detail ("small
  window shadows"). `texLoadFromGdl()` puts 0x074 back. Tried first and
  rejected: the base texture as its own detail, and the surface drawn through
  `texHandleType0()` - both draw horizontal streaks in the band where the LOD
  blend hands over.
- **Carrington's lift side panels were repointed.** 24 triangles of room 1
  bind 027b (the ROM's chrome strip) in the ROM and 0671 (a Dam texture,
  brushed steel, which Dam binds in both copies) in the release; they read as
  see-through. Every record involved is opaque, and the room texgen spans,
  vertex colours, `Pci_liftZ`'s mesh and `Pci_liftdoorZ` (038c) were ruled out
  on the way. They draw the ROM's own chrome strip again, 027b
  (`g_TexCiLiftSideTexture`), flat, as the N64 drew them - the user's pick on
  2026-09-14 after three tries that day:
  - Defection's metal (0042) flat was "onyx and no reflection": a sphere map
    laid over a panel's own UVs is a dark smear.
  - The lift door's chrome 038c, reflected off each triangle's face normal
    (`G_TEXGEN_FACE_EXT`, per-triangle lighting and texgen in
    `gfx_sp_tri_emit()`), was "too obvious and ugly": a flat panel's normal
    barely changes, so a few of 038c's 32x32 streak texels were stretched over
    the whole pillar.
  - Rendered at the tester's F3 camera with the span's texture, scale and
    texgen set from gdb: 006d at the rooms' sphere scale 0x1000 drew dark and
    see-through, 038c at the door's 0x800 one flat navy, and 027b flat a clean
    chrome gradient.

  The span code went with the choice; `G_TEXGEN_FACE_EXT` is still in the
  renderer and nothing sets it.

Headless Carrington under `--boot-stage 0x26`: the level stops at frame 302
behind a dialog, so break on `'video.c'::frames` (which keeps counting) and
`call (void)menuPopDialog()`, then teleport the `--spectate` prop. And never
run `xblaconvert.py` into the scratchpad for one picture - a whole pack filled
`/tmp`; decode single records with `read_records()` and
`x360.decode_texture()`.

### The spectator z-fights where two rooms' surfaces coincide (2026-09-14)

The spectator draws every room over the whole screen on purpose
(`bgTickPortalsSpectate()`; the user wants to see everything). A camera in
the level never draws two rooms over the same pixels, since each room is
scissored to what its portal leaves on screen, so surfaces two rooms share
never fight in play. The ROM has many such pairs (Chicago's rooms 72 and 76
share 112 coplanar triangles), and the release's Defection has the worst:
the ROM splits the outside city into an upper band of rooms (5-12) and a
lower one (13-20) that meet at y -4200, and 4J stretched both until they
share 3300 units of the same buildings (6 & 16, 8 & 18, 12 & 17 ... up to
130 coplanar triangle pairs). Neither copy can be dropped: each has
triangles the other does not.

The fix nudges each room back in depth while spectating
(`bgSpectateDepthBiasBegin()`, a new `G_SETDEPTHBIAS_EXT` - polygon offset
units, part of the renderer's depth-mode key so it flushes a batch like any
other depth change). Each room gets a colour once per stage, in room order,
so that no two rooms whose boxes touch or overlap share one; the nudge is
colour times `g_BgSpectateDepthStep` (4). Where two rooms' surfaces coincide
the lower colour always wins; elsewhere four depth steps change nothing. Every
stock and release level colours in at most nine (Felicity), and every
coplanar pair of any size lands on different colours except two in Air Force
One whose boxes do not touch.

Found statically: vertex-position matches found nothing because the copies
share planes, not vertices, so the scan that works buckets triangles by plane
and tests overlap. A box-overlap threshold does not separate 4J's copies from
the ROM's seams. Checked at the tester's pose on the GPU: step 0 against 4 and
16 at the same camera changes 4% of the frame, all of it on the fighting
bands; 4 and 16 change the same pixels.

## The whole release from one key (2026-09-12)

F6 was the meshes' own switch and is the release's now: `port/src/xblaswitch.c`
calls the five setters - meshes, textures, rooms, font, explosion - one after
another, and everything the port can draw of the release moves together.

**Two states, and no memory of a mixed one.** With every part on a press takes
every part off; any other arrangement is taken to the whole release. That is
deliberate: the page is where a mixed setting is made and lives (the meshes
without their art is the one worth having, xblamesh.h), and a key that has to
be looked at to know what it will do is no use for the one thing a key is for,
which is flipping between two pictures without leaving the level. Starting
from the shipped defaults - the meshes off, the other four on - the first
press is therefore the whole release and the second is stock Perfect Dark.

Nothing new is live about it: each part was already a live toggle for its own
checkbox, and this only sets them in one place. The rooms go **before** the
models, because both setters drop the rooms loaded under the old setting
(`xblaStageSwitched()`) and the rooms count only while the meshes are on
(xblastage.h) - so setting them this way round leaves the models with the last
word and the drop that matters as the last one.

The pd.ini key is still `Mod.XblaMeshKey` and the menu row is now "XBLA Assets
On/Off". Renaming the config key would silently put every player who has bound
their own key back on F6, which is worth more than the name being tidy.

Checked headlessly on Chicago (`--boot-stage 0x1d`, Xvfb, `xdotool` held
100ms), pressing F6 twice: the log says `xblaswitch: release assets off` then
`on`, the shots either side are the release's art and geometry, then the ROM's,
then the release's again, and the pd.ini written on exit has all five flags
moved together. That last part is the cheap check - the game writes pd.ini on
exit, so a headless press can be read back as five numbers.

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
