# Texture packs

## Where a texture pack goes

Four directories are involved and only one of them puts a pack in the menu.

- `texture-packs/<name>/` **beside the executable** (or in the save directory if
  that is not writable) is the pack list — the two `fsChooseOutputDir()` tries.
  A `.zip` or `.7z` counts, and is unpacked once into `texture-packs/.cache/`.
- `textures/` in the **base** directory (the one holding the ROM, so `data/` in a
  normal build) and in **every mod directory** is read as well, by
  `texpackScan()`, but is not a pack: it cannot be chosen or switched off from
  the menu, and the chosen pack outranks it.
- `textures/` **beside the executable is read by nothing.** It is the obvious
  place to put one and it silently does nothing.

All of it needs `Mod.LoadTextures=1`. The images are `<texnum>.png`, four
lowercase hex digits; a mod's `textures/*.bin` is the raw-N64-data path instead
and is unrelated. `tools/texpack/riceconvert.py` writes the first kind. Two
folder names inside a pack mean something other than texture numbers: one named
after a font holds glyphs, and `xbla` holds the XBLA release's own texture
records - see those sections below.

## Replacement textures are decoded off the render thread

`texpackLoadReplacement()` used to decode where it was called, which is inside
`import_texture()` in `gfx_pc.cpp` - so the first draw of each texture paid for a
whole PNG on the render thread. That is the stutter testers describe as a pack
"streaming in": measured over the PD Plus pack, 57.9 Mpx/s, about 4.6ms for an
average texture and roughly a whole frame for a 1024x1024.

**The decoder was never the slow part.** `pngread.c` and stb_image measure the
same to within a fraction of a percent on PNG (57.9 vs 57.8 Mpx/s), so swapping
in a vendored library buys nothing. Being on the render thread is the whole cost.

So a request queues the work and returns NULL, the caller draws the original
exactly as it does for a texture no pack replaces, and `gfx_texpack_poll()` at
the top of the next frame drops the cache entries holding the original so the
draw after that asks again and gets the replacement. Two things this depends on:

- `gfx_texture_cache_lookup()` answers **before** the pack is ever consulted, so
  the entry has to be erased or the original stays on screen forever. Eviction
  goes by `texpackGetTextureNum(key.texture_addr)`, not by address: one texture
  number can sit at several addresses.
- `rendering_state.textures` holds pointers into that map, so erasing anything
  means clearing it and setting `textures_changed`, the same as
  `gfx_texture_cache_clear()` does.

Decoded images wait in their slot to be claimed - normally one frame - under a
byte budget, because a texture that goes off screen may never ask again. Losing
one costs a re-decode and nothing else. `texpackFreeIndex()` stops the worker
before freeing anything: the worker reads the index and only stops between jobs.

**A request the full queue turns away is not dropped.** The renderer caches the
original it draws in the meantime and does not ask again until something evicts
that entry, so a texture refused for want of a slot would stay the game's own
for as long as it stayed on screen. The queue is 32 slots and a screen of text
asks for hundreds of glyphs in one frame, which is how it showed: F9 on the main
menu came back with the small font replaced and the portrait and large font not.
Refused ids go in `jobBacklog`, a bit per job id, and `texpackPollDecoded()`
moves them into slots as they free - so the queue's size now bounds how much is
decoding at once, not what gets decoded.

JPEG is decoded the same way, on the same thread - see the format note below.

## Decoded stage textures are kept, not handed over

A decoded image used to be handed to the renderer and forgotten, so every miss
in the renderer's cache on a replaced texture was a fresh decode - and, the
decode being off the render thread, **one frame of the game's own texture**
while it ran. Misses are not rare: the renderer keys by address and holds 1024
entries, so a prop spawning in, the same texture number loaded a second time
for another model, and a room whose working set is near the cap all miss. With
the PD Plus pack it showed as textures popping between the pack's image and the
original while walking through a stage.

The two-address case was worse than a frame. Both entries for one texture
number are dropped when its decode lands, whichever asked first got the buffer,
and the other queued the decode again - every frame, for as long as both were
on screen.

So `kept[]` in `texpack.c` holds decoded stage textures the way `fontDecoded`
holds glyphs, one slot per texture number, and a claim is a copy out. The PD
Plus pack is 2.2GB decoded, so it is held to `Mod.TexturePackCacheMB` (512 by
default), least recently claimed out first; going over the budget costs the
re-decode it always cost. It survives a stage change on purpose and is emptied
with the index. Two details:

- `TextureCacheValue::replaced` marks a renderer entry that already shows the
  pack's image, and `gfx_texture_cache_drop_texnum()` leaves those alone. That
  is what stops the two-address ping-pong; the store alone only made it cheap.
- A claim that finds its job READY mid-frame keeps the image itself, and puts
  the number in `keptReport` for the next `texpackPollDecoded()` to report,
  because the same number at another address may still be showing the original.

The shutdown log line `texpack: kept store holds N images ...` says how many
repeat requests the store answered - each one a decode, and a frame of the
original, that did not happen - and how many the budget threw out. If the
second number climbs on a normal stage, raise the budget.

## A pack's image is the tile; the renderer maps the padded row

The N64 loads a texture as whole 8-byte lines, so a 33-texel-wide CI4 tile is
48 texels of data, and the renderer uploads all 48 with the tile in the left 33
and normalises every UV by the padded width. Our own dumps are that padded row
(the header over `texpackTexToRgba()` says so). A pack built for an emulator
dumped the **tile** - 33 wide, scaled - and uploaded as it comes, the tile's
UVs show the left 33/48 of it. That is the "a few textures look stretched"
report: 278 of the PD Plus pack's 3395 images, every one a texture whose width
is not a multiple of the line (54 of 56, 28 of 32, 8 of 16 ...). The height
never differs; only the width is padded.

`gfx_pad_replacement()` in `gfx_pc.cpp` fixes it on upload: an image that fits
the padded shape is left alone, anything else is taken to be the tile and put
at the origin of a canvas of the padded shape at the same scale, with the last
column repeated across the padding so the filter does not pull black into the
tile's edge. One image in the pack is ambiguous (`08B1`, a 59-wide tile whose
pow2-upscaled image happens to fit 64:32) and is taken as padded. Glyphs are
not touched: their image is the whole 16-wide block already.

The stretch is easy to reproduce from the manifest: `tilewidth` against
`linesize * 2 >> siz` is the padded width, and the script that found the 278
compared each image's aspect to both.

## The other stretch: a tile sampled past its edge (2026-09-09)

There are **two** unrelated "this texture is stretched" reports and the section
above is only the first. That one is a pack's image being uploaded into a
padded row, is a bug, and is fixed. This one is the game.

Where a surface's texture coordinates run past the tile, the RDP repeats the
tile's last row or column for ever - `masks`/`maskt` of 0 is what makes a
coordinate saturate rather than wrap, and `gfx_dp_set_tile()` turns that into
`G_TX_CLAMP` (the line that reads `if (cms == G_TX_WRAP && masks ==
G_TX_NOMASK) cms = G_TX_CLAMP`). The levels lean on it: a wall was built
larger than its texture and the last column of texels fills the rest. On a CRT
at 320x240, with 32 texels of blur, nobody saw it. At a monitor's resolution,
and far worse with a pack's sharp 512x1024 over the same tile, it is a band of
smeared pixels - and because it is the *game's* geometry doing it, no amount of
editing the image fixes it. Blending the edge row only makes the smear a
smoother smear.

How common it is, measured in the Institute by instrumenting
`gfx_sp_tri_emit()` with the tile's `cms`/`cmt` and the min/max of the emitted
UVs: about twenty textures a level overrun, nearly all of them by 5-35% (`0300`
to v 1.35, `0321` to u 1.31, `0600` both ways), a few by much more (`027b` down
to v -2.73). So most of the time it is a thin band that no one notices, and
now and then it is the whole upper half of a wall.

**Stretched Edges** (`Video.StretchedEdges`, and a dropdown on the Dab's Mod
Options page) is the switch: *Original* keeps the smear, *Mirror* folds the
tile back on itself at its edge, *Repeat* tiles it. Mirroring is the one to
reach for - the fold meets the edge exactly, so it cannot seam whatever the
picture is, where repeating only suits a texture drawn to tile.

The property that makes it safe to leave on: **a fragment whose coordinates lie
inside the tile samples the same texel under all three**, so nothing that was
not already stretched can change. Two runs of the Institute lobby a frame apart
differ by 11 pixels, and six other stages' spawn views by none at all; what
changes is exactly the smear. That also means a stage can look identical with
it on and the switch still be working - to see anything you have to be standing
where a big overrun is on screen.

It goes in in **two places**, because the port clamps in two:

- `gfx_cm_to_opengl()` in `gfx_opengl.cpp` maps `G_TX_CLAMP` to
  `GL_CLAMP_TO_EDGE`, and now to `GL_MIRRORED_REPEAT`/`GL_REPEAT` instead.
  This is the usual path - the tile is the whole uploaded texture.
- The fragment shader does it when the tile is *smaller* than what was
  uploaded (a padded row, or the mip levels stacked underneath), because a
  sampler wrapping there would wrap into the padding. That is the
  `vTexClampS`/`vTexClampT` clamp, now a `texEdge()` helper emitted in the
  chosen mode. `lo` is the first texel's centre and `hi` the last, so the tile
  spans `hi + lo` and folding about that is exact.

`gfx_set_clamped_edge_mode()` calls `reset_texture_state()`, which is needed
both times: the sampler's wrap mode is cached per texture in
`rendering_state`, and the shader has the mode baked in. It can be flipped
live from gdb (`call (void)videoSetClampedEdgeMode(1)`) without restarting.

Telling the two stretches apart for a given texture: read `tilewidth` and
`linesize` out of a dump's `manifest.csv`. If `linesize * 2 >> siz` is bigger
than `tilewidth` the row is padded and it is the section above; if they are
equal - `0281`, the Institute rock, is 32x64 with a 16-byte line, so 32 = 32 -
the pack image is being mapped correctly and any smear is this one.

## Pack image formats, and which way up they go

**Formats.** `<texnum>.png` goes through `pngread.c`, which is ours because PNG is
a zlib stream in chunks and zlib was already linked. `<texnum>.jpg` / `.jpeg` goes
through `jpegread.c`, which is stb_image and the only thing in the port that uses
it - baseline JPEG is Huffman tables, an inverse DCT and chroma upsampling, and
progressive is more again, none of it worth writing. `STBI_ONLY_JPEG` keeps that
vendored header to the one decoder, so the two can never disagree about a file.
It is also the fallback for a PNG that `pngread.c` declines: that decoder handles
what image editors write and refuses the rest rather than guessing. Adam7
interlacing was one of the refusals until the PD Plus pack turned out to ship
several hundred interlaced files among its font glyphs, and a log line for each
was most of the log; `pngread.c` reads Adam7 itself now, checked byte for byte
against stb_image over that pack. `pngRead()` is still tried first, so nothing
that worked before changes hands. JPEG has no alpha: stb fills it with 255, and a
texture needing transparency has to ship as PNG. The Rice naming (`_all`, `_rgb`, `_a`) is still PNG-only, those
packs being PNG by convention.

**Row order.** A texture's data in the port has its first row at the bottom, and
two conventions exist that a filename cannot tell apart:

- Our own dumps are written the right way up, for editing, and are turned over on
  load. This is the default for `<texnum>.png`.
- Emulator (Rice) packs, and packs built for the VR fork, are already in N64 row
  order and must **not** be turned over.

So the folder says which it is, and `replaceFlip[]` records it per texture:

- a folder named **`ext_tex`** - what the VR fork reads, so a pack built for it
  works unpacked and dropped in as it comes.
- a folder holding a **`bottomup.txt`** - the same, for a pack under any other
  name.

Inherited by subfolders, so the marker goes at the top of the pack once. It is
logged when it fires, because getting it wrong means every texture in the pack is
upside down and nothing else says so. The trap in `pd-texture-data-is-bottom-up`
applies to checking this by eye: pick a texture with lettering, not a symmetric
one.

## An opaque picture for a texture with alpha (2026-09-10)

The XBLA release's art carries no alpha for most of the textures whose N64
original has some. Measured over the 2227 dumped stage textures against the
imported pack: 181 of 195 I4, 124 of 153 I8, all 30 IA4, 61 of 63 IA8 and 87
CI8 replacements are 255 in every pixel where the game's texels are not - the
records are DXT1 or 8888 with nothing in the channel - and the console's own
renderer must have taken the shape from the game's texels. Uploaded as they
came, an I8 light beam (whose alpha on the N64 *is* its intensity) was a solid
grey sheet and every smoke puff a square.

`gfx_replacement_alpha()` in gfx_pc.cpp now runs on the pack path after
`gfx_pad_replacement()`: a replacement that is opaque in every pixel, standing
in for a texture whose own texels are not (decoded through the normal
`import_texture_*` with `import_decode_only` set, so nothing reaches the GPU),
is given an alpha - an intensity texture's from the picture's own luminance,
since that is what the format means and it follows a repaint where the
original's would not; anything else the original's alpha, bilinear onto the
picture. A picture with any alpha of its own, or a texture the game keeps at
255 throughout, is untouched, so an opaque wall stays one. A Rice pack
missing the `_a` half of a split image gets the same repair, and so does the
release's art served straight out of the package by "Enable Textures"
(`xblaTexLoadNumbered()`, xbla.md) - it is on this branch for exactly that
reason. Not applied to the menu images or the XBLA meshes' own records, which
have no N64 original.

## Replacing font glyphs

A pack can replace the font as well, one image per character in a folder named
after the font (`fonthandelgothicsm`, `md`, `xs`, `lg`, `fontnumeric`), named by
the character's index in hex, with an `outlines/` inside it for the same
characters as the outline renderer draws them. The PD Plus pack has 722 of them.

**A glyph has no texture number.** It is uploaded straight out of the font by
`gDPSetTextureImage(..., curchar->pixeldata)`, so nothing in the texture registry
can name it. `gDPSetFontGlyphEXT` says what it is in the display list instead -
which is also the only place it can be said, the list being built before it runs.
Emitted inside the three renderers that draw a glyph rather than at their call
sites: `text0f154f38` and `text0f15568c` are the fill, `textRenderChar` is the
outline (it is only reachable from `textRender`). The fill and the outline come
from the same `pixeldata`, so the address cannot tell them apart and a pack ships
a different image for each.

**The image is of the tile, not the character.** A glyph is loaded as a 16-texel
wide CI4 block, `height + 2` rows tall, with the character in its top-left corner
(a `fontnumeric` digit is 3x5 in a 16x7 tile), and every image in the PD Plus
pack is exactly that block at an integer scale: 128x56 for the digits, 128x72,
144x45, 112x63 for the small font - all a multiple of 16 wide, all `16:(h+2)`.
Open one over black and the character sits in the left part of a wide image. So
the renderer maps it as it maps the game's own texels, normalised by the tile,
and **nothing is corrected**. A first version of this took the image to be the
character alone and rescaled the UVs to the glyph's own size; that shows the
corner of the image blown up, and because the flag driving it was only set on a
cache miss it came and went with the cache, which is a bug that looks like
flicker and reads like a scale error. Do not reintroduce it.

**Three more things this got wrong first:**

- The renderer's cache key **keeps the palette** for a glyph; the glyph id is
  added to the stock key, not substituted for it. The fonts are CI4 through a
  16-entry TLUT bank picked by the tile's palette index, and `textRender` draws
  the same pixel data through palette 0 and palette 1 in one two-cycle pass -
  that is how the outline is made. Keyed on the glyph alone, whichever palette
  drew a character first was what every later draw got, and with no pack at all
  the numeric font rendered as solid blocks. The two-entries-per-glyph cost that
  once argued for dropping the palette was only a problem while each entry
  re-decoded the image; with `fontDecoded` a second entry is a memcpy.
- **The outline pass wants both images, one per tile.** `textRender`'s two-cycle
  combiner takes its shape from texel 0 and colours the body from texel 1's
  alpha; `var8007fb5c` is the TLUT, and its bank 0 (tile 0) is body plus border
  while bank 1 (tile 1) is the body alone. So tile 0 gets the pack's
  `outlines/` image and tile 1 the plain one - `import_texture` swaps the
  outline bit off for palette index 1. Giving both tiles the outline image, as
  the VR fork does, makes every highlighted menu item and the FPS counter a
  bold glowing blob; the pack was drawn against that look, so it is not the
  pack that is wrong.
- A decoded glyph is **kept** rather than handed over. There are hundreds of them
  and the renderer's cache is not big enough to hold them all against a stage's
  textures, so one gets evicted, asked for again, and would be decoded again -
  showing the game's own glyph for a frame each time, which is a screen of text
  flickering. `fontDecoded` in `texpack.c` holds them and a claim copies out.
- They cannot be kept **in the decode queue**. It has 32 slots and a ready image
  holds its slot until claimed; leaving glyphs there as "claimed but kept" meant
  the first 32 glyphs drawn owned every slot for good, and no further decode -
  glyph or stage texture - ever ran. A glyph now leaves its slot the moment
  `texpackPollDecoded()` or a claim sees it ready.

**The XBLA release's own glyphs go up this same branch** (xbla.md, "The font"):
`xblafont.c` answers for a glyph the pack has no image for, and is asked what
the pack *has* (`texpackHaveFontReplacementFor()`) rather than what it
returned, since a queued decode also answers NULL.

**Check it on a menu, not the HUD.** The ammo counter is a handful of digits that
may not be replaced at the moment you look, and reading it cost a long detour
here; the file select screen is dense with text in three fonts and is the same
every time.

## A texture number belongs to whatever supplied the texels (2026-09-12)

A replacement is chosen by texture number, and a texture number only names a
picture *within the copy of the game that numbered it*. The Stage Loader breaks
that assumption routinely: it mounts a mod for its maps alone, and `texLoad()`
asks the mods before the ROM table, so a mod's map draws hundreds of textures
out of that mod's `textures/` at stock numbers. GoldenEye X ships 2104 of them
and **every one** is below `NUM_TEXTURES`, so every one had a pack file and an
XBLA record waiting for it.

The result is not just the wrong subject, it is the wrong *shape*, and the shape
is what a player sees. A pack replaces pixels and nothing else - the tile
geometry stays the N64's (`gfx_pad_replacement()`) - so a picture cut for the
ROM's tile is stretched across whatever tile the mod's texture set. Complex came
up wearing Perfect Dark's tan wall panels, its diamond-plate catwalk and a tiled
floor, none of them fitting the surface they were on.

So `texpackRegisterTexture()` records **what supplied the texels** alongside the
number, and `texpackTextureArt()` answers it:

- `TEXPACK_ART_ROM` - the ROM's table. Everything applies.
- `TEXPACK_ART_MOD` - a mod's `textures/%04x.bin`, or the base directory's,
  under the overlay. The numbering is still the game's, that mod's `textures/`
  is in the pack index and a pack shipped with it is meant to repaint the whole
  game, so **a pack still applies**; the XBLA release's own record for the
  number does not, because texels someone put there deliberately outrank a
  picture nobody chose - the same precedence a pack file already had over it.
- `TEXPACK_ART_MODSTAGE` - the running stage's own mod
  (`modloaderGetStageModDir()`, which is only ever a maps-only mount). The
  number means what that mod says and nothing else, so **nothing replaces it**.

Read where the picture is handed over, so it needs no rebuild: declined in
`texpackLoadReplacement()` and beside `xblaTexLoadNumbered()` in `gfx_pc.cpp`.
The registry is keyed on the texels' address, not the number, because the two
can be live at once - `modSetTextureFromStage(0)` gives a stock prop's model the
ROM's texture N while the room around it draws the mod's texture N.

This is the same rule `xblastage.c` applies to a mod's *level* under a stock
file name (`romdataFileIsStock()`), and `xblamesh.c` to a mod's *model*.

### The mod gets an index of its own (2026-09-12)

The other half of the same fact: a maps-only mod's *own* pack could not be read
either. `texpackScan()` indexes the base directory, the overlay's `textures/`
and the selected pack - all of them the game's numbering - and a maps-only mount
cannot join them, or a mod mounted for its maps would repaint the whole game.
So that mod gets a second numbered index, `modReplacePaths` and friends, built
from its own `textures/` the first time one of its stages asks for a
replacement (`texpackModUse()`), and thrown away when the running stage belongs
to a different mod (`texpackModDrop()`).

**One at a time**, not one cached per mod: a mod's emulator cache is held in
memory whole (GoldenEye X's is 20MB), only one stage runs, and what a rebuild
costs is a directory walk against a stage load. Keyed on the *mounted directory*
rather than the stage, so hopping between two maps of one mod rebuilds nothing.

**Which index a texture takes is read off the registry, not off the running
stage**, because both numberings are live inside one stage:
`modSetTextureFromStage(0)` gives a stock prop the ROM's texture N while the
room around it draws the mod's. So `texLoad()` records the mod a texture came
from beside its number (`modTextureLoad()`'s `outstagemod`,
`modloaderGetStageModDirIndex()`), and the draw asks the entry.

For the same reason a mod's textures cannot share the **decode queue** or the
**kept store** with the stock ones - both would be asked for number N in the
same frame - so the mod index has an id space of its own past the glyphs and
the release's records (`TEXPACK_MOD_ID_BASE`), and its own half of the kept
store. Two things fall out of that and both bit once:

- Every id test in the queue is "at or above", so the **highest base has to be
  tested first**. Left in the old order, a mod's id was read as a record of the
  release's, every decode failed silently, and the pack looked like it was not
  being read at all.
- What the poll hands the renderer is a **texture number**, not a job id
  (`gfx_texture_cache_drop_texnum`), so a mod id is reported as `id -
  TEXPACK_MOD_ID_BASE`. It drops the stock entry for that number too, which
  costs one re-upload and is right anyway.

**What the mod's pack is allowed to be.** Its numbered files and the records of
its emulator cache that match a texture by checksum, and nothing else: the
**font** is the game's, not the map's, and so are the **XBLA release's records**,
so `texpackIndexGlyph()`, `texpackIndexXbla()` and both glyph passes of
`texpackIndexHtc()` skip a mod scan. This is most of what GoldenEye X's cache
actually is - 708 records, of which 3 are textures, 722 are glyph images and 35
are matched when drawn - so the line is worth drawing carefully.

**Texel-matched files are the exception that needs no rule.** A checksum names
the picture itself, so a hit is the same picture whoever shipped it and the two
tables are searched one after the other (`texpackFindUnplaced()`). The mod's
table is still its own, because its entries are `htc://<entry>` paths into that
mod's cache file and would dangle when it goes.

**`texpackHaveReplacements()` had to stop meaning "a stock pack exists"**, or a
mod's own pack would never be asked for by a player who has no pack of their
own - which is the common case. It now also answers yes when texture loading is
on and anything is mounted for its maps; the cost is one registry probe per
texture the renderer uploads.

**Testing.** Two boots of one GE-X arena, `Mod.XblaMeshTextures` 1 then 0, are
the whole test - before the fix the pictures differ, after it they are the same
file:

```sh
cd build && timeout -k 5 240 xvfb-run -a ./pd.x86_64 --savedir /tmp/pdsave \
    --skip-intro --no-sound --boot-stage 0x52 --mpsims 1 --rng-seed 1 \
    --fixed-step --screenshot-frame 400 --exit-frame 410 --log
```

with `Mod.MapMods=GE-X_6a_01-19-25` in the scratch ini (0x52 is its Complex;
`grep modloader` the log for the ids, they move when the mod list does). Then
the same on a stock level - Chicago at 0x1d, frame 900 - which must still come
back with the release's "PARKING" sign and neon, and does, byte for byte.

For the mod's own index, a pack file is worth making rather than looking for:
dump what the map draws (`Mod.DumpTextures=1`), pick a number that is one of the
**mod's** (`build/mods/<mod>/textures/<num>.bin` exists - a number the map draws
that has no `.bin` is the ROM's and must *not* be replaced), drop a garish
`<num>.png` into that same `textures/`, and boot the map: the log says `<mod>
brings N texture(s) ...` and the picture is on the wall at the size the file
was, not stretched. Then boot another mod's map and a stock level with the file
still there - neither may show it, and neither does, because the stock scan
never saw it. `texpack mod:` in the F3 trace says which mod's index is in hand.

## Replacing the XBLA release's own textures (2026-09-11)

A pack can repaint the Xbox 360 release's models and rooms as well, from a
folder named **`xbla`** holding one image per `Textures.raw` record - `1156.png`,
four hex digits, `_anything` after them allowed, exactly the `<texnum>.png`
naming one number space along. Anywhere in the pack; the mark is inherited by
subfolders like the row-order one.

**Why a folder and not a number.** A material of the release's geometry names a
record, not a texture number. The two spaces overlap - record 0x13 and texture
0x13 are different pictures - and a filename cannot say which it means, so the
folder does. Nothing else in a pack changes: `textures/` still means texture
numbers, and a pack with both repaints the game's own art and the release's.

The overlap is not hypothetical: the release's rooms draw the **reused slots**
(`XBLA_REUSED_SLOTS`, xbla.md) through a stand-in as well, and a reused slot's
record number is the texture number it shadows. Slot 0222 is a police car's
light bar in the ROM and a Villa cliff in the release, so `textures/0222.png`
repaints the light bar and `xbla/0222.png` repaints the cliff.

**It is xblatex that asks.** The renderer's hook is unchanged: the XBLA branch
of `import_texture()` calls `xblaTexLoadReplacement()`, which now asks
`texpackHaveXblaReplacement(record)` before it decodes the release's own
picture. So a record's image goes up through the same stand-in tile and the
same `exact_uv` as the art it replaces, and until it has decoded the release's
own picture is what is drawn - not a frame of white.

The decode is this file's queue and kept store: a record takes a job id past the
glyphs' and a keep slot past the texture numbers', so one worker, one backlog
and one byte budget serve numbered textures, glyphs and records alike. What that
buys is what it buys everywhere else - no PNG on the render thread, and no
re-decode when the renderer's cache evicts one.

**The one thing that had to change in the renderer.**
`gfx_texture_cache_drop_texnum()` leaves an entry alone if it is already
`replaced`; an XBLA entry is `replaced` while it holds *the release's* art,
which is the entry the pack's decode has to evict. The `replaced` test is
skipped when the decoded id names a record. Safe because a record has exactly
one stand-in address, so the two-entries-taking-turns case the flag exists for
cannot arise.

**Getting the pictures to paint over.** `Mod.DumpTextures` (F7) writes every
record the game draws into `texture-dumps/<romid>/xbla/`, once per record per run
and the right way up - which is the layout a pack reads back, so a dump can be
edited and dropped in as it is. It is also the only way to learn *which* record
a particular surface uses: stand in front of it and dump. All of them at once
come from `tools/texpack/xblaconvert.py --mesh-textures`, which writes the same
folder out of the package.

**Row order is ours**: the image is written the right way up and turned over on
load, unless the folder says it is already in N64 order. The release's record is
stored in the game's own order and is *not* turned over when it is drawn - so
the dump flips on the way out and the load flips back, and the round trip is
byte for byte (checked against `x360.decode_texture()` on record 0x1156).

**What it does not reach**: whether a material is a cutout or is drawn blended
is decided from the release's own picture as the display list is built, on the
game thread, which may not touch the pack index - see `xblaTexRecordIsSoft()`.
Painting a glow over a solid therefore still draws as a cutout.

## Texture pack keys

`Mod.DumpTexturesKey` (F7), `Mod.TexturePackKey` (F8, packs on/off),
`Mod.TexturePackReloadKey` (F9, re-read the pack where you stand) and
`Mod.TexturePackCycleKey` (F10, next pack, round through "none"). All four are in
`texpackTick()` and bindable in Extended Options. F9 and F10 exist because
comparing packs otherwise meant quitting, swapping folders and relaunching.
`Mod.XblaMeshKey` (F6, `xblaSwitchTick()` in xblaswitch.c) is the same idea for
the XBLA release, bound on the same page - and it is the whole of it, not the
models alone: one press moves the meshes, their textures, the rooms, the font
and the explosion together (xbla.md, "The whole release from one key"). The
pd.ini key keeps the name it had when it was the meshes' own, because renaming
it would put everyone who has bound their own key back on F6.

Both are safe with the decode worker running - the reload path stops it first -
and both are worth re-testing on Windows, threads being what they are.

## Emulator cache files, and matching font glyphs by checksum (2026-09-04)

GE-X ships its text pack as two plugin cache files, `1964_HIRES_Files/GoldenEye
X_HIRESTEXTURES.htc` and a `.dat` beside it. Both are one gzip stream: a config
word, then a record per texture, and the two layouts share nothing but their
start. **GLideN64's** (the `.htc`): 64-bit Rice checksum, width, height, the
GL format (`0x8058` RGBA8, top bit = zlib-compressed), texture format and
pixel type, a hires flag, data length, data. **Glide64's** (the `.dat`, and
the `.htc` of a pack made with that plugin - the extension does not say):
checksum, width, height, a 16-bit `GR_TEXFMT` (0x8000 = zlib-compressed),
smallLodLog2, largeLodLog2, aspectRatioLog2, tiles, untiled width and height,
hires flag, data length, data - a 47 byte header, and the first attempt read
the format at the wrong offset, which the layout walk did not catch because
the field it read instead was small enough to pass. `texpackHtcWalk()` tries
each layout over the whole file and takes the one that reaches the end with
sane fields; `texpackIndexHtc()` inflates the file into memory once, indexes
every record as if it were a Rice-named file, and hands out a pseudo path
`htc://<record>` that `texpackLoadImage()` decodes: `uncompress()` and, for
a Glide format, `texpackGlideToRgba()` - ARGB8888/4444/1555, RGB565, the
intensity and alpha-intensity forms, and DXT1/3/5 through `texpackDxtBlock()`
(GE-X's `.dat` is all DXT5 glyphs and DXT1 textures, and draws the same as
the `.htc`). The records are in the pack's own row order, like a Rice PNG, so
nothing is turned over. A `.dat` with an `.htc` of the same name beside it is
skipped, the `.htc` being lossless. `mod.c` copies any `.htc`, and any
`_HIRESTEXTURES.dat`, found beside an imported patch into the mod's
`textures/`, which the scan reads - behind `Mod.LoadTextures`, as everything
is.

**The pack is the fonts.** 708 records, 675 of them 256x256; 361 distinct
texel checksums are glyphs of the five fonts. Two things it took to match them:

- **The tile is 32 by 32 at an 8 byte stride.** The text renderer's
  `gDPSetTileSize` says 0x7c, so the emulator hashed 32 rows of 16 bytes at
  the 8 byte line the LoadBlock set - which reads on past the glyph's own
  `height + 2` rows into the data after it. `texpackGlyphIndexBuild()` hashes
  every character of every font that way, from the ROM's bytes where the
  segment is the stock one (the tail past a font's last glyph is the next
  thing in the ROM, which the preprocessed segment does not have). The
  experiment that found this tried widths 16 and 32, heights h, h+1, h+2, 16
  and 32, swizzled or not, strides 8 and 16: exactly one variant matched.
- **The palette checksum is the outline pass.** Rice's CI checksum puts
  `RiceCRC32(palette, cimax + 1, 1, 16-bit, 32)` in the high word, cimax
  being the largest index used in the hashed window. Every palette-tagged
  record in GE-X's pack hashes through TLUT bank 0 - the body-plus-border
  palette `textRender` draws tile 0 with - and none through bank 1. So a
  record with a palette is the `outlines/` image and one without is the
  plain glyph, which also fills the outline slot where the pack has no
  palette record, as the plugin falls back to the checksum alone.

**The image is the emulator's tile, not our block.** Our glyph images are the
16-wide, `height + 2` block the game loads, at a scale; the emulator's are the
declared 32x32 tile at a scale, with the glyph in the top-left. Handed over
whole, every character drew at half width and squashed (the first screenshot
looked like widely spaced tiny letters). `texpackHtcCropGlyph()` records the
left half and the top `(height + 2) * scale` rows for each glyph record, and
`texpackHtcLoad()` cuts the image to that. Checked on the file-select screen
under Xvfb: "GoldenEye X", "Select File", "New Agent..." in the pack's font.

Several characters can share one tile (and so one checksum); the pack has one
image for all of them, and `texpackGlyphMatches()` returns every one.

## Community Packs: installing one from inside the game (2026-09-09)

Extended Options -> Texture Packs -> **Community Packs** is a short list of
packs other people make, with the pack's own cover art, that downloads,
verifies, unpacks and selects one. `port/src/community.c` is the work and
`port/src/communitymenu.c` is the page; the shape is `update.c`'s, one worker
and a menu that polls, because the problem is `update.c`'s.

**What is in the binary is the pack, not the release.** The catalogue holds who
made it, what it looks like and which file in a release is the one for this
port; the version, the size, the URL and the hash come from
`api.github.com/repos/<repo>/releases/latest` every time the page opens. A
table of URLs would be wrong the week after it shipped, and a player on an old
build would install an old pack. The file is picked out of the release by
substring (`match`, and `avoid` for the ones that look like it - the PD Plus
release carries a Quest build and a spare set of textures beside the pack);
largest wins among equals, and if nothing matches at all the largest archive is
taken and the log says so, so a rename is a wrong guess rather than a dead
page.

**It is unpacked at install time rather than left as an archive**, which is the
other way a pack can be installed (`texpackResolveSelected()` unpacks one into
`texture-packs/.cache/` on first use). Two reasons: the row order marker below
has to go somewhere the scan will read, and that cache copy does not exist
until after the pack has been selected; and an archive left in place costs its
own size on disk for ever beside the copy that was unpacked from it.

**The marker is the point.** `bottomup.txt` is written into the installed folder
whenever the catalogue says a pack is in N64 row order - which is every pack
built for an emulator or the VR fork, so all of them so far. Until v0.09 the PD
Plus pack's top folder was called `ext_tex`, which the loader recognises by
itself; v0.09 renamed it to `PD Plus HD`, and **the pack downloaded by hand
from that release loads upside down**. Nothing else on disk says which way up a
pack is, the catalogue knows, and this is where it gets written down.

**The cover art is a picture, and the menus cannot draw one.** Everything the
menus put on screen is a texture number and a texture number is a record in the
ROM, so `port/src/menuimage.c` does what `xblatex.c` does: the display list
binds a 16x16 stand-in tile, and `import_texture()` swaps the real picture in
against that tile's *address* (the hook is first of the three, ahead of the
XBLA meshes' and the pack's). The tile's texels are never read; the tile it
declares is what the texture coordinates are measured against, so a rectangle
over the whole tile is the whole picture whatever size the picture is.

Two things that were got wrong writing it:

- **The picture is not turned over.** A pack's PNG is flipped on load, so the
  first version of this flipped too and drew Joanna standing on her head. The
  flip in `texpack.c` is because a pack is drawn from a *dump*, and a dump is
  written the right way up for editing; what the renderer uploads is the top
  row first. `pd-texture-data-is-bottom-up` is about the dump end, not this
  end.
- **The row's height comes from `textMeasure()`, not from arithmetic.** The
  poster sits in a `MENUITEMTYPE_LABEL` of nine blank lines with
  `MENUITEMFLAG_LIST_CUSTOMRENDER`, which is the menu's own way of handing an
  item's rectangle to something that draws. `renderdata` gives x, y and width
  but no height, and `lines * LINEHEIGHT` is two lines more than the label
  actually got - the picture drew over the button underneath it, which reads
  as a rendering fault rather than a layout one. Measuring the same string the
  item holds is exact and cannot drift.

The picture is a PNG compiled in (`port/src/communityart.c`, written by
`tools/mkmenuimage`, 256 colours at 256x384 - a third of the bytes of
truecolour and no difference on screen), decoded once on the render thread and
kept: the renderer's cache is dropped every time a pack is switched, so a
decode that was handed over would happen again each time.

**Two things moved out of the way to be shared**: SHA-256 is
`port/src/sha256.c` now rather than a static in `update.c`, and
`ghostnetJsonField()` is declared in `ghostnet.h` - a GitHub release is JSON
and the ghost server was the only thing that had needed a reader.

The worker stops at "the files are on disk". `communityTick()`, from the
scheduler beside `texpackTick()`, does the rest - rescanning the folder,
selecting the new pack, switching packs on if they were off - because all of
that is the game thread's.
