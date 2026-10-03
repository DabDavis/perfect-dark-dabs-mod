#ifndef GFX_PC_H
#define GFX_PC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <unordered_map>
#include <list>
#include <cstddef>

#include <PR/gbi.h>

#include "system.h"

#define SCREEN_WIDTH ((int32_t)gfx_current_native_viewport.width)
#define SCREEN_HEIGHT ((int32_t)gfx_current_native_viewport.height)

extern uintptr_t gfxFramebuffer;

struct GfxRenderingAPI;
struct GfxWindowManagerAPI;

struct TextureCacheKey {
    const uint8_t* texture_addr;
    const uint8_t* palette_addrs[2];
    uint8_t fmt, siz;
    uint8_t palette_index;
    // Which font glyph this is, 0 for anything that is not one. The fill and
    // the outline of a glyph are uploaded from the same pixeldata, so without
    // this they would share one entry and whichever drew first would win.
    uint32_t glyph;
    // An animated picture's frame plus one, 0 for anything else: a frame is
    // an entry of its own, uploaded once (xblaTexAnimFrame()).
    uint16_t anim_frame;
    // Uploaded with the levels of detail its data holds (G_TEX_OWN_LODS_EXT),
    // not ones made from the first
    uint8_t own_lods;

    bool operator==(const TextureCacheKey&) const noexcept = default;

    struct Hasher {
        size_t operator()(const TextureCacheKey& key) const noexcept {
            // The address alone: gfx_texture_cache_delete() finds every entry
            // for an address by looking in that address's bucket, and a glyph
            // mixed into the hash put the glyph entries in other buckets, where
            // a freed address's glyphs outlived it.
            uintptr_t addr = (uintptr_t)key.texture_addr;
            return (size_t)(addr ^ (addr >> 5));
        }
    };
};

typedef std::unordered_map<TextureCacheKey, struct TextureCacheValue, TextureCacheKey::Hasher> TextureCacheMap;
typedef std::pair<const TextureCacheKey, struct TextureCacheValue> TextureCacheNode;

struct TextureCacheValue {
    uint32_t texture_id;
    uint8_t cms, cmt;
    bool linear_filter;
    // Holds a texture pack's image rather than the game's own. The drop that
    // follows a decode leaves these alone: they are already showing it.
    bool replaced = false;
    // The picture was authored for a sampler whose texel centres sit at half
    // integers (the XBLA release's own art, drawn under Direct3D), so its
    // coordinates are exact as written and get none of the half texel the
    // N64's bilinear filter is owed. See gfx_derive_batch_state's uv_ofs.
    bool exact_uv = false;
    // The frame it was last drawn in, which tells a texture the frame still
    // wants from one it has left behind (see gfx_texture_cache_lookup), and
    // what it holds on the GPU, for the cache's byte budget.
    uint32_t last_frame = 0;
    uint32_t bytes = 0;
    // For the F3 trace (gfx_trace_texture_entries()): the frame it was
    // uploaded in, the size that went up, and where the texels came from -
    // 'g' the game's own, 'p' a texture pack, 'X' the release's picture for
    // the number, 'x' a stand-in or a picture bound at the address
    // (xblatex.c), 'f' a font glyph's, 'm' a menu image.
    uint32_t upload_frame = 0;
    uint16_t width = 0, height = 0;
    char source = 'g';

    std::list<struct TextureCacheMapIter>::iterator lru_location;
};

struct TextureCacheMapIter {
    TextureCacheMap::iterator it;
};

extern "C" {

#include "gfx_api.h"

}

#endif
