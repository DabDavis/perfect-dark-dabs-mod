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
