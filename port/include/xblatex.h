#ifndef _IN_XBLATEX_H
#define _IN_XBLATEX_H

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
 * Whether anything has been bound and the art is wanted, which is the
 * renderer's early out. Every texture upload in the game goes past this.
 */
s32 xblaTexHaveTextures(void);

/**
 * Mod.XblaMeshTextures, the menu's "Enable Textures".
 *
 * A live toggle: a material always binds its stand-in and this decides whether
 * a picture arrives in its place, so turning it off draws the stand-in's own
 * white texels times shade - the flat solid an untextured mesh always was -
 * with nothing rebuilt. The setter drops the texture cache, which is what
 * makes the change show up on textures already uploaded.
 */
s32 xblaTexGetEnabled(void);
void xblaTexSetEnabled(s32 enabled);

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
