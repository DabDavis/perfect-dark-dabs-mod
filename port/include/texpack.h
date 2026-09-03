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
 * Whether Mod.DumpTextures is on. Checked before the renderer bothers working
 * out what it would pass to texpackDumpTexture().
 */
s32 texpackDumpEnabled(void);

/**
 * Writes a texture out as a PNG, once per texture number per run.
 *
 * rgba32 is the converted image the renderer is about to upload, so every N64
 * format arrives here already expanded to 8-bit RGBA; fmt and siz are the
 * G_IM_FMT_* and G_IM_SIZ_* the texture came in as, and go in the filename for
 * whoever is drawing the replacement.
 */
void texpackDumpTexture(const void *data, const u8 *rgba32, u32 width, u32 height, u32 fmt, u32 siz);

#ifdef __cplusplus
}
#endif

#endif
