#ifndef _IN_TEXPACK_H
#define _IN_TEXPACK_H

#include <PR/ultratypes.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Texture identity registry.
 *
 * The renderer only ever sees a texture as the pointer gDPSetTextureImage was
 * handed, which is struct tex's decompressed data. texLoad() knows the texture
 * number that pointer belongs to and nothing downstream does, so it records the
 * pairing here and the renderer looks it up again.
 *
 * Pool memory is reused between levels, so an address only means what it means
 * until the pool holding it is dropped. The forget calls are wired alongside
 * the ones that drop the renderer's own texture cache - see video.c - because
 * an entry outliving its pool would name the wrong texture, not merely a
 * missing one.
 */
void texpackRegisterTexture(const void *data, s32 texturenum);
void texpackForgetTexture(const void *data);
void texpackForgetRange(const void *start, const void *end);
void texpackForgetAll(void);

/**
 * The texture number data belongs to, or -1 when nothing registered it.
 */
s32 texpackGetTextureNum(const void *data);

/**
 * Looks for a replacement image for whatever texture lives at data, decodes it,
 * and hands back an RGBA32 buffer for the renderer to upload in place of the
 * game's own texels. Returns NULL when there is no replacement, which is the
 * usual answer and costs one array lookup.
 *
 * The buffer is the caller's until it passes it to texpackFreeReplacement().
 * Rows come back in the same bottom-up order the game's texture data uses, so
 * the renderer uploads it exactly as it would the real thing.
 *
 * A pack is a "textures" directory of <texnum>.png - four lowercase hex digits,
 * optionally followed by _<anything> so the dumper's own filenames can be
 * edited and dropped straight back in. Mod directories are searched ahead of
 * the base directory, and in mod order, so packs layer.
 */
u8 *texpackLoadReplacement(const void *data, s32 *outWidth, s32 *outHeight);
void texpackFreeReplacement(u8 *rgba);

/**
 * Whether any replacement pack was found at all. The renderer checks this
 * before it bothers looking a texture up.
 */
s32 texpackHaveReplacements(void);

/**
 * Writes the raw texel bytes and tile geometry of every texture in the ROM,
 * then exits. Does nothing unless --dump-textures was passed.
 *
 * The per-texture dump above only sees what the game actually draws, which for
 * a converter means playing through every room of every level. This walks the
 * texture table instead, so one run covers all of them.
 */
void texpackDumpAll(void);

/**
 * Texture packs, as the Extended Options menu sees them.
 *
 * A pack is a directory or an archive inside a "texturepacks" folder, in the
 * base directory, any mod directory or the save directory. Selecting an archive
 * unpacks it once into a cache beside it and uses that - see archiveExtract()
 * for why it is not read where it lies.
 *
 * The chosen pack outranks any textures/ a mod ships, because it is the one the
 * player picked.
 */
void texpackRefreshPacks(void);
s32 texpackGetNumPacks(void);
const char *texpackGetPackName(s32 index);

/** The index into the pack list, or -1 for none. */
s32 texpackGetSelectedPack(void);
void texpackSetSelectedPack(s32 index);

/** Whether packs are used at all - Mod.LoadTextures. */
s32 texpackLoadEnabled(void);
void texpackSetLoadEnabled(s32 enabled);

/** How many textures the current pack actually replaces. */
s32 texpackGetNumReplacements(void);

/**
 * Re-reads the selected pack and drops the renderer's texture cache, so an
 * edited image shows up without restarting. Bound to a key for exactly that.
 */
void texpackReload(void);

/**
 * Dumping, as a thing that can be turned on mid-game. Switching it on clears
 * the texture cache, so everything on screen is written out as it comes back
 * rather than only what happens to be loaded next.
 */
s32 texpackGetDumpEnabled(void);
void texpackSetDumpEnabled(s32 enabled);

/** Hotkeys: F7 dumps, F8 reloads. Same shape as the screenshot bind. */
s32 texpackDumpGetKey(void);
void texpackDumpSetKey(s32 vk);
s32 texpackReloadGetKey(void);
void texpackReloadSetKey(s32 vk);

/** Polled once a frame from the scheduler, like screenshotTick(). */
void texpackTick(void);

/**
 * Whether Mod.DumpTextures is on. Checked before the renderer bothers working
 * out what it would pass to texpackDumpTexture().
 */
s32 texpackDumpEnabled(void);

/**
 * What the renderer knows about a texture besides its finished pixels: the N64
 * texel bytes as the game stored them, the tile geometry they are laid out
 * under, and the palette a CI texture indexes into.
 *
 * Only Mod.DumpTextureData uses it, to write the input a pack converter needs -
 * existing packs name their files after a checksum of exactly this.
 */
struct texpackrawinfo {
	const u8 *data;
	u32 sizeBytes;
	u32 lineSizeBytes;
	u32 tileWidth;
	u32 tileHeight;
	u32 paletteIndex;
	const u16 *palette; // 256 entries in host order, or NULL when not CI
};

/**
 * Writes a texture out as a PNG, once per texture number per run.
 *
 * rgba32 is the converted image the renderer is about to upload, so every N64
 * format arrives here already expanded to 8-bit RGBA; fmt and siz are the
 * G_IM_FMT_* and G_IM_SIZ_* the texture came in as, and go in the filename for
 * whoever is drawing the replacement.
 */
void texpackDumpTexture(const u8 *rgba32, u32 width, u32 height, u32 fmt, u32 siz,
		const struct texpackrawinfo *raw);

#ifdef __cplusplus
}
#endif

#endif
