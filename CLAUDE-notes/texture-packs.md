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
and is unrelated. `tools/texpack/riceconvert.py` writes the first kind.

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

**Check it on a menu, not the HUD.** The ammo counter is a handful of digits that
may not be replaced at the moment you look, and reading it cost a long detour
here; the file select screen is dense with text in three fonts and is the same
every time.

## Texture pack keys

`Mod.DumpTexturesKey` (F7), `Mod.TexturePackKey` (F8, packs on/off),
`Mod.TexturePackReloadKey` (F9, re-read the pack where you stand) and
`Mod.TexturePackCycleKey` (F10, next pack, round through "none"). All four are in
`texpackTick()` and bindable in Extended Options. F9 and F10 exist because
comparing packs otherwise meant quitting, swapping folders and relaunching.

Both are safe with the decode worker running - the reload path stops it first -
and both are worth re-testing on Windows, threads being what they are.
