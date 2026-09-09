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

    bool operator==(const TextureCacheKey&) const noexcept = default;

    struct Hasher {
        size_t operator()(const TextureCacheKey& key) const noexcept {
            uintptr_t addr = (uintptr_t)key.texture_addr;
            return (size_t)(addr ^ (addr >> 5) ^ ((size_t)key.glyph << 3));
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

    std::list<struct TextureCacheMapIter>::iterator lru_location;
};

struct TextureCacheMapIter {
    TextureCacheMap::iterator it;
};

extern "C" {

#include "gfx_api.h"

}

#endif
