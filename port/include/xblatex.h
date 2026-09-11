#ifndef _IN_XBLATEX_H
#define _IN_XBLATEX_H

#include <stdio.h>
#include <PR/ultratypes.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * The XBLA release's own textures, for the meshes that draw with them.
 *
 * The pack importer (xblaimport.c) converts the first NUM_TEXTURES records of
 * Textures.raw, because those are the ones that carry a texture number. A mesh
 * never uses one of them: its materials name records 3741 to 5746, which are
 * the console release's own art and have no number to go to. So they cannot
 * arrive as a texture pack, and the renderer cannot be asked for them by
 * number either.
 *
 * What the renderer does have is the address a display list binds, which is
 * how texpackLoadReplacement() finds a replacement. This plays the same trick
 * one registry along: a mesh's list binds a stand-in tile that means nothing
 * on its own, and the address of that tile is what says which record to decode
 * and hand over instead.
 *
 * A pack can still replace one, just not by texture number: an `xbla` folder
 * inside it holds an image per record, and the record found here is what is
 * looked up in it (texpackHaveXblaReplacement()). The release's own picture is
 * what is drawn until that image has been decoded, and is what F7 writes out
 * for somebody to paint over.
 *
 * The stand-in is the same size for every texture, and small - 32x32. That is
 * not the size of the picture: the renderer normalises texture coordinates by
 * the tile and samples whatever was uploaded across it, so a 512x512
 * replacement on a 32x32 tile is the ordinary texture pack case. What the size
 * does decide is the coordinates a vertex can hold, since a Perfect Dark
 * vertex measures s and t in texels of the tile as 10.5 fixed point: 32 texels
 * puts a UV of one at 1024, so a UV runs to 32 before it overflows and a
 * coordinate is good to a thousandth. Bigger tiles are more precise and wrap
 * sooner, and 32 is the compromise the release's UVs sit inside.
 */

#define XBLATEX_TILE      32
#define XBLATEX_TILE_MASK 5   // log2 of the above, for gDPLoadTextureBlock
#define XBLATEX_TILE_SCALE (XBLATEX_TILE * 32.0f) // a UV of one, in what a Vtx holds

/**
 * The stand-in tile for one Textures.raw record, allocated on first ask.
 *
 * NULL if there is no package, no such record, or no memory - a list that gets
 * NULL back draws that material shaded and untextured rather than not at all.
 * The buffer lives as long as the game does, so a display list built once can
 * keep binding it.
 */
const void *xblaTexBind(u32 record);

/**
 * A stand-in for a picture that is not one of the release's records: a model
 * pack's own PNG, or one of the ROM's numbered textures decoded for a mesh
 * that names it. The picture is RGBA32 in the game's row order (first
 * uploaded row first) and is taken over - freed here, never by the caller -
 * and kept for the life of the game beside the tile. key is what the picture
 * is bound as: the same key binds the same tile, and the picture handed in
 * for it the second time is freed unused.
 *
 * Not subject to Mod.XblaMeshTextures - that switch is about the release's
 * art - and not replaceable by a texture pack, which has nothing to key on.
 */
const void *xblaTexBindImage(const char *key, u8 *rgba, s32 width, s32 height);

/**
 * The same, for a picture that *is* one of the ROM's numbered textures - a
 * model pack's material named `n64_0a9a`.
 *
 * The picture handed in is the ROM's own and is the fallback; the texture
 * pack's picture for that number is asked for at the point the renderer wants
 * the tile, so a pack switched on, off or changed under a model pack's mesh
 * repaints it without anything being built again. Bound once per number.
 */
const void *xblaTexBindTexture(s32 texturenum, u8 *rgba, s32 width, s32 height);

/**
 * What the mesh builder needs to know about a picture bound above: whether
 * any texel is not opaque (the material draws with its alpha) and whether
 * next to none are (the material fades, see xblaTexRecordIsSoft()). Zero when
 * addr is not such a stand-in.
 */
s32 xblaTexImageInfo(const void *addr, s32 *outAlpha, s32 *outSoft);

/**
 * Whether anything has been bound, which is the renderer's early out. Every
 * texture upload in the game goes past this.
 */
s32 xblaTexHaveTextures(void);

/**
 * For the asset dump: how many records the package has (opening it, and
 * unpacking the archive if it has to; zero with no package), and one of them
 * decoded to RGBA32 in the game's row order, freed by the caller. Not gated
 * on the switch and never dumped from - the dumper does its own writing.
 */
u32 xblaTexGetNumRecords(void);
u8 *xblaTexDecodeRecord(u32 record, s32 *outWidth, s32 *outHeight);

/**
 * Mod.XblaMeshTextures, the menu's "Enable Textures".
 *
 * A live toggle: a material always binds its stand-in and this decides whether
 * a picture arrives in its place, so turning it off draws the stand-in's own
 * white texels times shade - the flat solid an untextured mesh always was -
 * with nothing rebuilt. The setter drops the texture cache, which is what
 * makes the change show up on textures already uploaded.
 *
 * It covers the numbered textures below as well, so the switch is the whole of
 * the release's art rather than only the part a mesh draws with.
 */
s32 xblaTexGetEnabled(void);
void xblaTexSetEnabled(s32 enabled);

/**
 * The release's art on the game's *own* textures, which is the texture pack
 * the conversion writes, served straight out of the package instead.
 *
 * Record N is texture N for the first NUM_TEXTURES records (see xbla.md), so
 * there is nothing to match and nothing to convert: what the pack's
 * <texnum>.png holds is this decode, written to disk and read back. With the
 * switch on, the renderer asks here for any texture that carries a number and
 * gets the release's picture for it, so the whole game is retextured without
 * a pack being built, selected, or taking up 600MB of disk.
 *
 * The same textures the pack leaves out are left out here
 * (xblaImportTextureIsLeftOut()): the ROM's own picture is the one that draws
 * right for those. A pack the player has selected outranks this, so painting
 * over one texture does not mean giving up the rest - the renderer asks the
 * pack first and only comes here for what it has no file for.
 *
 * Have() is the renderer's early out and costs a flag read. Load() returns a
 * buffer the caller frees with xblaTexFreeReplacement(), in the game's row
 * order like everything else here, or NULL when this texture has no record,
 * is left out, or does not decode.
 */
s32 xblaTexHaveNumbered(void);
u8 *xblaTexLoadNumbered(s32 texturenum, s32 *outWidth, s32 *outHeight);

/**
 * The picture for whatever record was bound at addr, decoded to RGBA32, or
 * NULL when that address is not one of ours.
 *
 * Rows come back in the order the console stored them, which is the order the
 * game's own texture data uses and the order the renderer uploads - the flip
 * that xblaimport.c does on its way to a PNG is for the PNG's benefit and has
 * no place here.
 *
 * Called from the render thread while the meshes are built on the game thread,
 * so the registry and the package are held under a lock.
 */
u8 *xblaTexLoadReplacement(const void *addr, s32 *outWidth, s32 *outHeight);
void xblaTexFreeReplacement(u8 *rgba);

/**
 * The record bound at addr, or -1 when that address is not one of ours.
 *
 * A player's own picture for a record arrives through the texture pack
 * (texpackHaveXblaReplacement()), and the renderer's cache entry for it is
 * keyed on this address - so when such a decode lands, this is what says which
 * entry is holding the release's own art and has to go.
 */
s32 xblaTexRecordOf(const void *addr);

/**
 * The picture's own width and height for a record, without decoding it -
 * what a room list has to know to scale its coordinates onto the stand-in.
 * Zero when there is no package or no such record.
 */
s32 xblaTexRecordSize(u32 record, s32 *outWidth, s32 *outHeight);

// The N64 size the record stands in for: its own for the release's own art,
// the ROM tile's for a replacement. Zero if the record is not readable.
s32 xblaTexRecordSrcSize(u32 record, s32 *outWidth, s32 *outHeight);

/**
 * Whether a record's alpha is soft - fewer than one texel in a hundred opaque
 * - so that there is no edge in it for a cutout to cut at. A screen's glow,
 * a tinted pane, a haze: drawn as a cutout such a picture is a solid where its
 * alpha clears the threshold and nothing where it does not, which is the white
 * half-disc the comhub's screens showed. The mesh builder asks this once per
 * alpha material and sorts a soft one into the fading span (blended, no depth
 * write, translucent pass). Decodes the record the first time it is asked and
 * remembers the answer; 0 when there is no package or the record will not
 * decode, which leaves the material a cutout as before.
 */
s32 xblaTexRecordIsSoft(u32 record);

/** Drops the package handle. The stand-in tiles stay, since lists hold them. */
void xblaTexShutdown(void);

#ifdef __cplusplus
}
#endif

#endif

/** The record store's state, for the F3 trace dump. */
void xblaTexTrace(FILE *f);
