/**
 * The Vulkan renderer: the same GfxRenderingAPI the OpenGL one is, drawing
 * the same batches through the same combiner shaders.
 *
 * The one rule that keeps it simple is that every image here is laid out the
 * way OpenGL lays out a framebuffer: row 0 is the bottom of the picture. Then
 * a viewport, a scissor, a blit rectangle, a readback and the invert_y flag
 * gfx_pc.cpp hands the offscreen targets all mean exactly what they mean to
 * the OpenGL renderer, with no flip anywhere - Vulkan measures a viewport's y
 * from row 0 and maps NDC -1 to it, which is what OpenGL does from the bottom.
 * The window is the exception, since a swapchain image is shown row 0 at the
 * top: the game draws into an image of its own (the "window image", fb 0) and
 * it is blitted upside down into the swapchain as it is presented.
 *
 * There are two window images, used in turn, because the game reads the
 * *front* buffer - the frame on screen - for its blur and its motion trail
 * (gfx_copy_framebuffer() with use_back 0), and here that is simply the one
 * drawn last frame.
 *
 * Depth is OpenGL's too: the shader writes z as (z + w) / 2 so a 0..1 depth
 * buffer holds what OpenGL's -1..1 would have mapped to, while the fog, which
 * reads z/w, is handed the untouched value.
 *
 * Shaders are the OpenGL renderer's GLSL written for Vulkan (explicit
 * locations, a push constant block instead of the loose uniforms) and compiled
 * to SPIR-V with shaderc when a combiner is first seen, as OpenGL compiles its
 * own. A pipeline is made per shader and per blend, depth and target state,
 * the first time that combination draws.
 *
 * Synchronisation is deliberately blunt - every layout change is a full
 * barrier - because the frame is a few dozen framebuffer switches at most and
 * the cost that matters is in gfx_pc.cpp's vertex work, not here.
 */

#ifdef PD_HAVE_VULKAN

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include <SDL.h>

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include <SDL_vulkan.h>
#include <shaderc/shaderc.h>

#ifndef _LANGUAGE_C
#define _LANGUAGE_C
#endif
#include <PR/gbi.h>

#include "gfx_cc.h"
#include "gfx_rendering_api.h"
#include "gfx_pc.h"
#include "gfx_api.h"
#include "gfx_vulkan.h"
#include "gfx_post.h"

extern "C" {
#include "fs.h"
}

// gfx_sdl2.cpp: the window, and the hooks it presents and sets vsync through
SDL_Window *gfx_sdl_window(void);
void gfx_sdl_set_vulkan_hooks(void (*present)(void), int (*get_interval)(void), bool (*set_interval)(int));

using namespace std;

// Everything here has internal linkage: the OpenGL renderer has types and
// std::map instantiations of its own under some of the same names.
namespace {

/*
 * Entry points, all loaded through SDL's vkGetInstanceProcAddr so that
 * nothing links against a Vulkan library: a machine without one still starts,
 * on OpenGL.
 */
#define VK_GLOBAL_FUNCS(X) \
    X(vkCreateInstance) \
    X(vkEnumerateInstanceExtensionProperties) \
    X(vkEnumerateInstanceLayerProperties)

#define VK_INSTANCE_FUNCS(X) \
    X(vkDestroyInstance) \
    X(vkEnumeratePhysicalDevices) \
    X(vkGetPhysicalDeviceProperties) \
    X(vkGetPhysicalDeviceFeatures) \
    X(vkGetPhysicalDeviceFeatures2) \
    X(vkGetPhysicalDeviceProperties2) \
    X(vkGetPhysicalDeviceQueueFamilyProperties) \
    X(vkGetPhysicalDeviceMemoryProperties) \
    X(vkGetPhysicalDeviceFormatProperties) \
    X(vkEnumerateDeviceExtensionProperties) \
    X(vkCreateDevice) \
    X(vkGetDeviceProcAddr) \
    X(vkDestroySurfaceKHR) \
    X(vkGetPhysicalDeviceSurfaceSupportKHR) \
    X(vkGetPhysicalDeviceSurfaceCapabilitiesKHR) \
    X(vkGetPhysicalDeviceSurfaceFormatsKHR) \
    X(vkGetPhysicalDeviceSurfacePresentModesKHR)

#define VK_DEVICE_FUNCS(X) \
    X(vkDestroyDevice) \
    X(vkGetDeviceQueue) \
    X(vkDeviceWaitIdle) \
    X(vkCreateCommandPool) \
    X(vkDestroyCommandPool) \
    X(vkResetCommandPool) \
    X(vkAllocateCommandBuffers) \
    X(vkBeginCommandBuffer) \
    X(vkEndCommandBuffer) \
    X(vkQueueSubmit) \
    X(vkCreateFence) \
    X(vkDestroyFence) \
    X(vkWaitForFences) \
    X(vkGetFenceStatus) \
    X(vkResetFences) \
    X(vkCreateSemaphore) \
    X(vkDestroySemaphore) \
    X(vkCreateSwapchainKHR) \
    X(vkDestroySwapchainKHR) \
    X(vkGetSwapchainImagesKHR) \
    X(vkAcquireNextImageKHR) \
    X(vkQueuePresentKHR) \
    X(vkCreateImage) \
    X(vkDestroyImage) \
    X(vkGetImageMemoryRequirements) \
    X(vkBindImageMemory) \
    X(vkCreateImageView) \
    X(vkDestroyImageView) \
    X(vkCreateBuffer) \
    X(vkDestroyBuffer) \
    X(vkGetBufferMemoryRequirements) \
    X(vkBindBufferMemory) \
    X(vkAllocateMemory) \
    X(vkFreeMemory) \
    X(vkMapMemory) \
    X(vkCreateSampler) \
    X(vkDestroySampler) \
    X(vkCreateShaderModule) \
    X(vkDestroyShaderModule) \
    X(vkCreatePipelineLayout) \
    X(vkDestroyPipelineLayout) \
    X(vkCreatePipelineCache) \
    X(vkGetPipelineCacheData) \
    X(vkDestroyPipelineCache) \
    X(vkCreateGraphicsPipelines) \
    X(vkDestroyPipeline) \
    X(vkCreateDescriptorSetLayout) \
    X(vkDestroyDescriptorSetLayout) \
    X(vkCreateDescriptorPool) \
    X(vkDestroyDescriptorPool) \
    X(vkResetDescriptorPool) \
    X(vkAllocateDescriptorSets) \
    X(vkUpdateDescriptorSets) \
    X(vkCmdBindPipeline) \
    X(vkCmdBindDescriptorSets) \
    X(vkCmdBindVertexBuffers) \
    X(vkCmdDraw) \
    X(vkCmdSetViewport) \
    X(vkCmdSetScissor) \
    X(vkCmdSetDepthBias) \
    X(vkCmdPushConstants) \
    X(vkCmdPipelineBarrier) \
    X(vkCmdCopyBufferToImage) \
    X(vkCmdCopyImageToBuffer) \
    X(vkCmdCopyImage) \
    X(vkCmdBlitImage) \
    X(vkCmdResolveImage) \
    X(vkCmdClearAttachments) \
    X(vkCmdClearColorImage) \
    X(vkCmdClearDepthStencilImage) \
    X(vkCreateQueryPool) \
    X(vkDestroyQueryPool) \
    X(vkCmdResetQueryPool) \
    X(vkCmdBeginQuery) \
    X(vkCmdEndQuery) \
    X(vkGetQueryPoolResults)

#define VK_DECLARE(name) static PFN_##name name;
VK_GLOBAL_FUNCS(VK_DECLARE)
VK_INSTANCE_FUNCS(VK_DECLARE)
VK_DEVICE_FUNCS(VK_DECLARE)
#undef VK_DECLARE

static PFN_vkGetInstanceProcAddr vk_get_instance_proc;
static PFN_vkEnumerateInstanceVersion vk_enumerate_instance_version;
// Core in 1.3, VK_KHR_dynamic_rendering on 1.2: the same function either way
static PFN_vkCmdBeginRendering vk_cmd_begin_rendering;
static PFN_vkCmdEndRendering vk_cmd_end_rendering;
static PFN_vkCreateDebugUtilsMessengerEXT vk_create_debug_messenger;
static PFN_vkDestroyDebugUtilsMessengerEXT vk_destroy_debug_messenger;

#define VK_FRAMES 2
#define VK_BLOCK_SIZE ((VkDeviceSize)64 << 20)
#define VK_VERTEX_CHUNK ((VkDeviceSize)8 << 20)
#define VK_STAGING_CHUNK ((VkDeviceSize)16 << 20)
#define VK_COLOR_FORMAT VK_FORMAT_B8G8R8A8_UNORM
#define VK_TEXTURE_FORMAT VK_FORMAT_R8G8B8A8_UNORM

static bool vk_failed;
static char vk_device_desc[256];

static VkInstance vk_instance;
static VkDebugUtilsMessengerEXT vk_messenger;
static VkSurfaceKHR vk_surface;
static VkPhysicalDevice vk_phys;
static VkPhysicalDeviceProperties vk_props;
static VkPhysicalDeviceMemoryProperties vk_memprops;
static VkDevice vk_dev;
static uint32_t vk_queue_family;
static VkQueue vk_queue;
static VkFormat vk_depth_format;
static bool vk_depth_has_stencil;
static bool vk_have_depth_clamp;
static bool vk_have_mirror_clamp;
static bool vk_have_anisotropy;
static uint32_t vk_max_msaa = 1;
static VkSampleCountFlags vk_sample_counts = VK_SAMPLE_COUNT_1_BIT;

static VkDescriptorSetLayout vk_set_layout;
static VkPipelineLayout vk_pipeline_layout;
static VkPipelineCache vk_pipeline_cache;
static shaderc_compiler_t vk_shaderc;

/*
 * Memory: images are carved out of 64 MB blocks so that a texture cache of a
 * thousand entries is not a thousand allocations (drivers cap them at 4096),
 * and buffers, of which there are a handful, get an allocation each.
 */
struct VkAlloc {
    VkDeviceMemory mem = VK_NULL_HANDLE;
    VkDeviceSize offset = 0, size = 0;
    int block = -1;
    uint8_t *mapped = nullptr;
};

struct VkMemBlock {
    VkDeviceMemory mem;
    uint32_t type;
    std::map<VkDeviceSize, VkDeviceSize> free; // offset -> size
};

static std::vector<VkMemBlock> vk_blocks;

static int vk_find_memory_type(uint32_t bits, VkMemoryPropertyFlags want) {
    for (uint32_t i = 0; i < vk_memprops.memoryTypeCount; i++) {
        if ((bits & (1u << i)) && (vk_memprops.memoryTypes[i].propertyFlags & want) == want) {
            return (int)i;
        }
    }
    return -1;
}

static bool vk_block_carve(VkMemBlock &b, VkDeviceSize size, VkDeviceSize align, VkDeviceSize *out) {
    for (auto it = b.free.begin(); it != b.free.end(); ++it) {
        const VkDeviceSize o = it->first, s = it->second;
        const VkDeviceSize a = (o + align - 1) / align * align;
        if (a + size <= o + s) {
            b.free.erase(it);
            if (a > o) {
                b.free[o] = a - o;
            }
            if (a + size < o + s) {
                b.free[a + size] = o + s - (a + size);
            }
            *out = a;
            return true;
        }
    }
    return false;
}

static bool vk_alloc(const VkMemoryRequirements &req, bool host, VkAlloc *out) {
    const VkMemoryPropertyFlags want = host ? (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)
                                            : VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    int type = vk_find_memory_type(req.memoryTypeBits, want);
    if (type < 0 && !host) {
        type = vk_find_memory_type(req.memoryTypeBits, 0);
    }
    if (type < 0) {
        return false;
    }

    *out = VkAlloc();

    if (host || req.size > VK_BLOCK_SIZE / 4) {
        VkMemoryAllocateInfo ai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
        ai.allocationSize = req.size;
        ai.memoryTypeIndex = (uint32_t)type;
        if (vkAllocateMemory(vk_dev, &ai, NULL, &out->mem) != VK_SUCCESS) {
            return false;
        }
        out->size = req.size;
        if (host) {
            void *p = NULL;
            if (vkMapMemory(vk_dev, out->mem, 0, VK_WHOLE_SIZE, 0, &p) != VK_SUCCESS) {
                vkFreeMemory(vk_dev, out->mem, NULL);
                out->mem = VK_NULL_HANDLE;
                return false;
            }
            out->mapped = (uint8_t *)p;
        }
        return true;
    }

    for (size_t i = 0; i < vk_blocks.size(); i++) {
        VkDeviceSize off;
        if (vk_blocks[i].type == (uint32_t)type && vk_block_carve(vk_blocks[i], req.size, req.alignment, &off)) {
            out->mem = vk_blocks[i].mem;
            out->offset = off;
            out->size = req.size;
            out->block = (int)i;
            return true;
        }
    }

    VkMemBlock b;
    VkMemoryAllocateInfo ai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    ai.allocationSize = VK_BLOCK_SIZE;
    ai.memoryTypeIndex = (uint32_t)type;
    if (vkAllocateMemory(vk_dev, &ai, NULL, &b.mem) != VK_SUCCESS) {
        return false;
    }
    b.type = (uint32_t)type;
    b.free[0] = VK_BLOCK_SIZE;
    vk_blocks.push_back(b);

    VkDeviceSize off = 0;
    vk_block_carve(vk_blocks.back(), req.size, req.alignment, &off);
    out->mem = b.mem;
    out->offset = off;
    out->size = req.size;
    out->block = (int)vk_blocks.size() - 1;
    return true;
}

static void vk_free(const VkAlloc &a) {
    if (a.mem == VK_NULL_HANDLE) {
        return;
    }
    if (a.block < 0) {
        vkFreeMemory(vk_dev, a.mem, NULL);
        return;
    }

    auto &fr = vk_blocks[a.block].free;
    VkDeviceSize o = a.offset, s = a.size;
    auto next = fr.lower_bound(o);
    if (next != fr.end() && o + s == next->first) {
        s += next->second;
        next = fr.erase(next);
    }
    if (next != fr.begin()) {
        auto prev = std::prev(next);
        if (prev->first + prev->second == o) {
            prev->second += s;
            return;
        }
    }
    fr[o] = s;
}

/*
 * Serials: every submission gets the next number, and anything the GPU may
 * still be reading is destroyed only once the submission it was last recorded
 * into is known to have finished.
 */
static uint64_t vk_submitted;
static uint64_t vk_completed;
static std::deque<std::pair<uint64_t, std::function<void()>>> vk_graveyard;

static void vk_defer(std::function<void()> fn) {
    // Whatever is being recorded now goes out as the next submission.
    vk_graveyard.push_back({ vk_submitted + 1, std::move(fn) });
}

static void vk_collect(void) {
    while (!vk_graveyard.empty() && vk_graveyard.front().first <= vk_completed) {
        vk_graveyard.front().second();
        vk_graveyard.pop_front();
    }
}

struct VkBuf {
    VkBuffer buf = VK_NULL_HANDLE;
    VkAlloc mem;
    VkDeviceSize size = 0;
};

static bool vk_buffer_create(VkBuf &b, VkDeviceSize size, VkBufferUsageFlags usage) {
    VkBufferCreateInfo bi = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    bi.size = size;
    bi.usage = usage;
    bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(vk_dev, &bi, NULL, &b.buf) != VK_SUCCESS) {
        return false;
    }
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(vk_dev, b.buf, &req);
    if (!vk_alloc(req, true, &b.mem) || vkBindBufferMemory(vk_dev, b.buf, b.mem.mem, b.mem.offset) != VK_SUCCESS) {
        vkDestroyBuffer(vk_dev, b.buf, NULL);
        b.buf = VK_NULL_HANDLE;
        return false;
    }
    b.size = size;
    return true;
}

static void vk_buffer_destroy_now(VkBuf &b) {
    if (b.buf) {
        vkDestroyBuffer(vk_dev, b.buf, NULL);
        vk_free(b.mem);
    }
    b = VkBuf();
}

/*
 * Images. A framebuffer's colour image has two views: the one it is drawn
 * through, and the one it is sampled through, which reads alpha as 1 because
 * OpenGL's framebuffer textures are RGB and the game's blur and trail draws
 * blend by what they sample.
 */
struct VkImg {
    VkImage image = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkImageView sample_view = VK_NULL_HANDLE;
    VkAlloc mem;
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkImageAspectFlags aspect = 0;
    uint32_t width = 0, height = 0, mips = 1;
    VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
    // A framebuffer's image is cleared the first time it is used, where
    // OpenGL's new storage is usually zeros; garbage would show in a blur
    // drawn from a target the game has not drawn into yet.
    bool clear_first = false;
    // Where sample_view sits in the bindless texture table, -1 for none
    int32_t slot = -1;
};

/*
 * Bindless textures: every sampled image and every sampler the renderer has
 * is in one descriptor set, bound once per command buffer, and a draw names
 * the two it reads by index in its push constants. Nothing is allocated,
 * written or bound per draw, which is most of what a draw cost here - the
 * game changes texture on nearly every one.
 *
 * An image's entry is written when the image is made and handed back to the
 * free list only once the GPU is done with the image (update-after-bind and
 * update-unused-while-pending make both legal while the set is bound).
 */
static VkDescriptorPool vk_bindless_pool;
static VkDescriptorSet vk_bindless_set;
static uint32_t vk_max_texture_slots = 4096;
#define VK_MAX_SAMPLER_SLOTS 256
static std::vector<int32_t> vk_free_slots;
static int32_t vk_next_slot;

static int32_t vk_slot_alloc(VkImageView view) {
    int32_t slot;
    if (!vk_free_slots.empty()) {
        slot = vk_free_slots.back();
        vk_free_slots.pop_back();
    } else if ((uint32_t)vk_next_slot < vk_max_texture_slots) {
        slot = vk_next_slot++;
    } else {
        static bool warned;
        if (!warned) {
            sysLogPrintf(LOG_WARNING, "Vulkan: all %u texture slots are in use", vk_max_texture_slots);
            warned = true;
        }
        return 0; // the black stand-in, which is slot 0
    }

    VkDescriptorImageInfo ii = { VK_NULL_HANDLE, view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
    VkWriteDescriptorSet w = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
    w.dstSet = vk_bindless_set;
    w.dstBinding = 0;
    w.dstArrayElement = (uint32_t)slot;
    w.descriptorCount = 1;
    w.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    w.pImageInfo = &ii;
    vkUpdateDescriptorSets(vk_dev, 1, &w, 0, NULL);
    return slot;
}

static bool vk_image_create(VkImg &img, uint32_t w, uint32_t h, VkFormat fmt, VkImageUsageFlags usage,
                            VkSampleCountFlagBits samples, uint32_t mips, bool opaque_view) {
    img = VkImg();

    const bool depth = (fmt == VK_FORMAT_D16_UNORM || fmt == VK_FORMAT_D32_SFLOAT || fmt == VK_FORMAT_D24_UNORM_S8_UINT ||
                        fmt == VK_FORMAT_D32_SFLOAT_S8_UINT || fmt == VK_FORMAT_D16_UNORM_S8_UINT);
    const bool stencil = (fmt == VK_FORMAT_D24_UNORM_S8_UINT || fmt == VK_FORMAT_D32_SFLOAT_S8_UINT ||
                          fmt == VK_FORMAT_D16_UNORM_S8_UINT);

    VkImageCreateInfo ci = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = fmt;
    ci.extent = { w, h, 1 };
    ci.mipLevels = mips;
    ci.arrayLayers = 1;
    ci.samples = samples;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = usage;
    ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(vk_dev, &ci, NULL, &img.image) != VK_SUCCESS) {
        img.image = VK_NULL_HANDLE;
        return false;
    }

    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(vk_dev, img.image, &req);
    if (!vk_alloc(req, false, &img.mem) ||
        vkBindImageMemory(vk_dev, img.image, img.mem.mem, img.mem.offset) != VK_SUCCESS) {
        vkDestroyImage(vk_dev, img.image, NULL);
        img = VkImg();
        return false;
    }

    img.format = fmt;
    img.aspect = depth ? (VK_IMAGE_ASPECT_DEPTH_BIT | (stencil ? VK_IMAGE_ASPECT_STENCIL_BIT : 0)) : VK_IMAGE_ASPECT_COLOR_BIT;
    img.width = w;
    img.height = h;
    img.mips = mips;
    img.samples = samples;

    VkImageViewCreateInfo vi = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    vi.image = img.image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = fmt;
    vi.subresourceRange = { img.aspect, 0, mips, 0, 1 };
    vkCreateImageView(vk_dev, &vi, NULL, &img.view);

    if (depth && (usage & VK_IMAGE_USAGE_SAMPLED_BIT)) {
        // a shader reads one aspect: TAA's copy of the depth
        vi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
        vkCreateImageView(vk_dev, &vi, NULL, &img.sample_view);
    } else if (opaque_view) {
        vi.components.a = VK_COMPONENT_SWIZZLE_ONE;
        vkCreateImageView(vk_dev, &vi, NULL, &img.sample_view);
    } else {
        img.sample_view = img.view;
    }

    if ((usage & VK_IMAGE_USAGE_SAMPLED_BIT) && samples == VK_SAMPLE_COUNT_1_BIT && vk_bindless_set) {
        img.slot = vk_slot_alloc(img.sample_view);
    }

    return true;
}

static void vk_image_destroy(VkImg &img) {
    if (!img.image) {
        img = VkImg();
        return;
    }
    const VkImage image = img.image;
    const VkImageView view = img.view;
    const VkImageView sview = img.sample_view != img.view ? img.sample_view : VK_NULL_HANDLE;
    const VkAlloc mem = img.mem;
    const int32_t slot = img.slot;
    vk_defer([=]() {
        if (slot > 0) {
            vk_free_slots.push_back(slot);
        }
        if (sview) {
            vkDestroyImageView(vk_dev, sview, NULL);
        }
        if (view) {
            vkDestroyImageView(vk_dev, view, NULL);
        }
        vkDestroyImage(vk_dev, image, NULL);
        vk_free(mem);
    });
    img = VkImg();
}

/*
 * The frame's command buffer is recorded on a worker thread. The game's
 * thread keeps every piece of state - layouts, what is bound, the texture
 * table - and writes each command as a small packet into the frame's stream;
 * at the end of the frame the stream goes to the worker, which records the
 * real command buffer from it, submits and presents while the game gets on
 * with the next frame. That takes the driver's recording off the game's
 * thread, which is the one the frame rate is bound by.
 *
 * VK_MAIN_CB stands for the frame's command buffer: every rcCmd* below takes
 * the same arguments as its vkCmd*, packetises for VK_MAIN_CB and calls
 * straight through for a real one (texture uploads record their own command
 * buffer on the game's thread, as before). Only the counts the renderer uses
 * are carried: one barrier, one region, up to two clear attachments.
 */
#define VK_MAIN_CB ((VkCommandBuffer)(uintptr_t)1)

enum : uint8_t {
    VKP_BARRIER,
    VKP_CLEAR_COLOR,
    VKP_CLEAR_DS,
    VKP_VIEWPORT,
    VKP_SCISSOR,
    VKP_BIAS,
    VKP_BEGIN_RENDERING,
    VKP_END_RENDERING,
    VKP_BIND_PIPELINE,
    VKP_PUSH,
    VKP_PUSH_BIG,
    VKP_BIND_VB,
    VKP_DRAW,
    VKP_CLEAR_ATT,
    VKP_BLIT,
    VKP_RESOLVE,
    VKP_COPY_IMAGE,
    VKP_COPY_TO_BUFFER,
    VKP_BIND_SET,
    VKP_BEGIN_QUERY,
    VKP_END_QUERY,
};

struct VkStream {
    uint8_t *data = nullptr;
    size_t size = 0, cap = 0;
};

static VkStream *vk_stream; // the frame being recorded

template <typename T> static inline void vk_put(uint8_t type, const T &v) {
    VkStream &st = *vk_stream;
    if (st.size + 1 + sizeof(T) > st.cap) {
        st.cap = std::max<size_t>(st.cap * 2, 1 << 16);
        st.data = (uint8_t *)realloc(st.data, st.cap);
    }
    st.data[st.size] = type;
    memcpy(st.data + st.size + 1, &v, sizeof(T));
    st.size += 1 + sizeof(T);
}

struct VkpClearColor { VkImage image; VkClearColorValue value; VkImageSubresourceRange range; };
struct VkpClearDs { VkImage image; VkClearDepthStencilValue value; VkImageSubresourceRange range; };
struct VkpBias { float constant, clamp, slope; };
struct VkpBeginRendering { VkRect2D area; VkRenderingAttachmentInfo color, depth; uint8_t has_depth, has_stencil; };
struct VkpPush { uint32_t size; uint8_t data[32]; };
// the post passes' block with TAA's parameters; kept apart so every draw's
// push stays small in the stream
struct VkpPushBig { uint32_t size; uint8_t data[112]; };
struct VkpBindVb { VkBuffer buffer; VkDeviceSize offset; };
struct VkpDraw { uint32_t count, first; };
struct VkpClearAtt { uint32_t n; VkClearAttachment att[2]; VkClearRect rect; };
struct VkpBlit { VkImage src; VkImageLayout src_layout; VkImage dst; VkImageLayout dst_layout; VkImageBlit region; VkFilter filter; };
struct VkpResolve { VkImage src; VkImageLayout src_layout; VkImage dst; VkImageLayout dst_layout; VkImageResolve region; };
struct VkpCopyImage { VkImage src; VkImageLayout src_layout; VkImage dst; VkImageLayout dst_layout; VkImageCopy region; };
struct VkpCopyToBuffer { VkImage src; VkImageLayout src_layout; VkBuffer dst; VkBufferImageCopy region; };

static void rcCmdPipelineBarrier(VkCommandBuffer cb, VkPipelineStageFlags src, VkPipelineStageFlags dst,
                                 VkDependencyFlags dep, uint32_t nm, const VkMemoryBarrier *m, uint32_t nb,
                                 const VkBufferMemoryBarrier *b, uint32_t ni, const VkImageMemoryBarrier *i) {
    if (cb != VK_MAIN_CB) {
        vkCmdPipelineBarrier(cb, src, dst, dep, nm, m, nb, b, ni, i);
        return;
    }
    vk_put(VKP_BARRIER, *i);
}

static void rcCmdClearColorImage(VkCommandBuffer cb, VkImage image, VkImageLayout layout, const VkClearColorValue *v,
                                 uint32_t n, const VkImageSubresourceRange *r) {
    if (cb != VK_MAIN_CB) {
        vkCmdClearColorImage(cb, image, layout, v, n, r);
        return;
    }
    vk_put(VKP_CLEAR_COLOR, VkpClearColor{ image, *v, *r });
}

static void rcCmdClearDepthStencilImage(VkCommandBuffer cb, VkImage image, VkImageLayout layout,
                                        const VkClearDepthStencilValue *v, uint32_t n, const VkImageSubresourceRange *r) {
    if (cb != VK_MAIN_CB) {
        vkCmdClearDepthStencilImage(cb, image, layout, v, n, r);
        return;
    }
    vk_put(VKP_CLEAR_DS, VkpClearDs{ image, *v, *r });
}

static void rcCmdSetViewport(VkCommandBuffer cb, uint32_t first, uint32_t n, const VkViewport *v) {
    vk_put(VKP_VIEWPORT, *v);
}

static void rcCmdSetScissor(VkCommandBuffer cb, uint32_t first, uint32_t n, const VkRect2D *r) {
    vk_put(VKP_SCISSOR, *r);
}

static void rcCmdSetDepthBias(VkCommandBuffer cb, float constant, float clamp, float slope) {
    vk_put(VKP_BIAS, VkpBias{ constant, clamp, slope });
}

static void rcCmdBeginRendering(VkCommandBuffer cb, const VkRenderingInfo *ri) {
    VkpBeginRendering p = {};
    p.area = ri->renderArea;
    p.color = ri->pColorAttachments[0];
    p.has_depth = ri->pDepthAttachment != NULL;
    p.has_stencil = ri->pStencilAttachment != NULL;
    if (p.has_depth) {
        p.depth = *ri->pDepthAttachment;
    }
    vk_put(VKP_BEGIN_RENDERING, p);
}

static void rcCmdEndRendering(VkCommandBuffer cb) {
    vk_put(VKP_END_RENDERING, (uint8_t)0);
}

static void rcCmdBindPipeline(VkCommandBuffer cb, VkPipelineBindPoint bp, VkPipeline p) {
    vk_put(VKP_BIND_PIPELINE, p);
}

static void rcCmdPushConstants(VkCommandBuffer cb, VkPipelineLayout layout, VkShaderStageFlags stages, uint32_t off,
                               uint32_t size, const void *data) {
    if (size > sizeof(VkpPush::data)) {
        VkpPushBig p;
        p.size = std::min<uint32_t>(size, sizeof(p.data));
        memcpy(p.data, data, p.size);
        vk_put(VKP_PUSH_BIG, p);
        return;
    }
    VkpPush p;
    p.size = size;
    memcpy(p.data, data, p.size);
    vk_put(VKP_PUSH, p);
}

static void rcCmdBindVertexBuffers(VkCommandBuffer cb, uint32_t first, uint32_t n, const VkBuffer *b,
                                   const VkDeviceSize *o) {
    vk_put(VKP_BIND_VB, VkpBindVb{ *b, *o });
}

static inline void rcCmdDraw(VkCommandBuffer cb, uint32_t count, uint32_t instances, uint32_t first, uint32_t fi) {
    vk_put(VKP_DRAW, VkpDraw{ count, first });
}

static void rcCmdClearAttachments(VkCommandBuffer cb, uint32_t n, const VkClearAttachment *a, uint32_t nr,
                                  const VkClearRect *r) {
    VkpClearAtt p = {};
    p.n = std::min<uint32_t>(n, 2);
    memcpy(p.att, a, p.n * sizeof(*a));
    p.rect = *r;
    vk_put(VKP_CLEAR_ATT, p);
}

static void rcCmdBlitImage(VkCommandBuffer cb, VkImage src, VkImageLayout sl, VkImage dst, VkImageLayout dl,
                           uint32_t n, const VkImageBlit *r, VkFilter f) {
    if (cb != VK_MAIN_CB) {
        vkCmdBlitImage(cb, src, sl, dst, dl, n, r, f);
        return;
    }
    vk_put(VKP_BLIT, VkpBlit{ src, sl, dst, dl, *r, f });
}

static void rcCmdResolveImage(VkCommandBuffer cb, VkImage src, VkImageLayout sl, VkImage dst, VkImageLayout dl,
                              uint32_t n, const VkImageResolve *r) {
    vk_put(VKP_RESOLVE, VkpResolve{ src, sl, dst, dl, *r });
}

static void rcCmdCopyImage(VkCommandBuffer cb, VkImage src, VkImageLayout sl, VkImage dst, VkImageLayout dl,
                           uint32_t n, const VkImageCopy *r) {
    vk_put(VKP_COPY_IMAGE, VkpCopyImage{ src, sl, dst, dl, *r });
}

static void rcCmdCopyImageToBuffer(VkCommandBuffer cb, VkImage src, VkImageLayout sl, VkBuffer dst, uint32_t n,
                                   const VkBufferImageCopy *r) {
    vk_put(VKP_COPY_TO_BUFFER, VkpCopyToBuffer{ src, sl, dst, *r });
}

static void rcCmdCopyBufferToImage(VkCommandBuffer cb, VkBuffer src, VkImage dst, VkImageLayout dl, uint32_t n,
                                   const VkBufferImageCopy *r) {
    // only ever the upload command buffer
    vkCmdCopyBufferToImage(cb, src, dst, dl, n, r);
}

static VkPipelineLayout vk_pipeline_layout_for_replay;

static void rcCmdBindDescriptorSets(VkCommandBuffer cb, VkPipelineBindPoint bp, VkPipelineLayout layout, uint32_t first,
                                    uint32_t n, const VkDescriptorSet *sets, uint32_t nd, const uint32_t *d) {
    vk_pipeline_layout_for_replay = layout;
    vk_put(VKP_BIND_SET, *sets);
}

// The occlusion queries' pool (gfx_vk_occlusion_begin()), made with the device
static VkQueryPool vk_query_pool;

static void rcCmdBeginQuery(VkCommandBuffer cb, uint32_t query) {
    vk_put(VKP_BEGIN_QUERY, query);
}

static void rcCmdEndQuery(VkCommandBuffer cb, uint32_t query) {
    vk_put(VKP_END_QUERY, query);
}

// The worker's half: the stream into a real command buffer
static void vk_replay(VkCommandBuffer cb, const VkStream &st) {
    size_t pos = 0;
    VkPipelineLayout layout = vk_pipeline_layout_for_replay;
    while (pos < st.size) {
        const uint8_t type = st.data[pos++];
        const uint8_t *p = st.data + pos;
#define VKP_TAKE(T, name) T name; memcpy(&name, p, sizeof(T)); pos += sizeof(T)
        switch (type) {
            case VKP_DRAW: {
                VKP_TAKE(VkpDraw, d);
                vkCmdDraw(cb, d.count, 1, d.first, 0);
                break;
            }
            case VKP_BIND_PIPELINE: {
                VKP_TAKE(VkPipeline, pl);
                vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pl);
                break;
            }
            case VKP_PUSH: {
                VKP_TAKE(VkpPush, pc);
                vkCmdPushConstants(cb, layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, pc.size, pc.data);
                break;
            }
            case VKP_PUSH_BIG: {
                VKP_TAKE(VkpPushBig, pc);
                vkCmdPushConstants(cb, layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, pc.size, pc.data);
                break;
            }
            case VKP_BIND_VB: {
                VKP_TAKE(VkpBindVb, vb);
                vkCmdBindVertexBuffers(cb, 0, 1, &vb.buffer, &vb.offset);
                break;
            }
            case VKP_VIEWPORT: {
                VKP_TAKE(VkViewport, v);
                vkCmdSetViewport(cb, 0, 1, &v);
                break;
            }
            case VKP_SCISSOR: {
                VKP_TAKE(VkRect2D, r);
                vkCmdSetScissor(cb, 0, 1, &r);
                break;
            }
            case VKP_BIAS: {
                VKP_TAKE(VkpBias, b);
                vkCmdSetDepthBias(cb, b.constant, b.clamp, b.slope);
                break;
            }
            case VKP_BARRIER: {
                VKP_TAKE(VkImageMemoryBarrier, b);
                vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0,
                                     NULL, 0, NULL, 1, &b);
                break;
            }
            case VKP_CLEAR_COLOR: {
                VKP_TAKE(VkpClearColor, c);
                vkCmdClearColorImage(cb, c.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &c.value, 1, &c.range);
                break;
            }
            case VKP_CLEAR_DS: {
                VKP_TAKE(VkpClearDs, c);
                vkCmdClearDepthStencilImage(cb, c.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &c.value, 1, &c.range);
                break;
            }
            case VKP_BEGIN_RENDERING: {
                VKP_TAKE(VkpBeginRendering, b);
                VkRenderingInfo ri = { VK_STRUCTURE_TYPE_RENDERING_INFO };
                ri.renderArea = b.area;
                ri.layerCount = 1;
                ri.colorAttachmentCount = 1;
                ri.pColorAttachments = &b.color;
                ri.pDepthAttachment = b.has_depth ? &b.depth : NULL;
                ri.pStencilAttachment = b.has_stencil ? &b.depth : NULL;
                vk_cmd_begin_rendering(cb, &ri);
                break;
            }
            case VKP_END_RENDERING: {
                pos += 1;
                vk_cmd_end_rendering(cb);
                break;
            }
            case VKP_CLEAR_ATT: {
                VKP_TAKE(VkpClearAtt, c);
                vkCmdClearAttachments(cb, c.n, c.att, 1, &c.rect);
                break;
            }
            case VKP_BLIT: {
                VKP_TAKE(VkpBlit, b);
                vkCmdBlitImage(cb, b.src, b.src_layout, b.dst, b.dst_layout, 1, &b.region, b.filter);
                break;
            }
            case VKP_RESOLVE: {
                VKP_TAKE(VkpResolve, r);
                vkCmdResolveImage(cb, r.src, r.src_layout, r.dst, r.dst_layout, 1, &r.region);
                break;
            }
            case VKP_COPY_IMAGE: {
                VKP_TAKE(VkpCopyImage, c);
                vkCmdCopyImage(cb, c.src, c.src_layout, c.dst, c.dst_layout, 1, &c.region);
                break;
            }
            case VKP_COPY_TO_BUFFER: {
                VKP_TAKE(VkpCopyToBuffer, c);
                vkCmdCopyImageToBuffer(cb, c.src, c.src_layout, c.dst, 1, &c.region);
                break;
            }
            case VKP_BIND_SET: {
                VKP_TAKE(VkDescriptorSet, set);
                vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &set, 0, NULL);
                break;
            }
            case VKP_BEGIN_QUERY: {
                VKP_TAKE(uint32_t, q);
                vkCmdBeginQuery(cb, vk_query_pool, q, 0);
                break;
            }
            case VKP_END_QUERY: {
                VKP_TAKE(uint32_t, q);
                vkCmdEndQuery(cb, vk_query_pool, q);
                break;
            }
            default:
                sysFatalError("Vulkan: bad command packet %u", type);
        }
#undef VKP_TAKE
    }
}

static void vk_barrier(VkCommandBuffer cb, VkImage image, VkImageAspectFlags aspect, uint32_t base_mip, uint32_t mips,
                       VkImageLayout from, VkImageLayout to) {
    VkImageMemoryBarrier b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    b.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    b.oldLayout = from;
    b.newLayout = to;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange = { aspect, base_mip, mips, 0, 1 };
    rcCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0,
                         NULL, 1, &b);
}

/*
 * Frames in flight, each with its own command buffers and its own vertex and
 * staging memory. Texture uploads are recorded into a command buffer of their
 * own that is submitted ahead of the frame's, because a texture arrives in the
 * middle of drawing and a copy cannot go inside a render pass.
 */
struct VkChunkRing {
    std::vector<VkBuf> chunks;
    size_t chunk = 0;
    VkDeviceSize offset = 0;
    std::vector<VkBuf> oversize; // gone when the slot comes round again
};

struct VkSlot {
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkCommandBuffer upload = VK_NULL_HANDLE;
    bool upload_used = false;
    VkFence fence = VK_NULL_HANDLE;
    VkSemaphore acquired = VK_NULL_HANDLE;
    uint64_t serial = 0;
    VkChunkRing vertex;
    VkChunkRing staging;
    VkStream stream;
};

static VkSlot vk_slots[VK_FRAMES];
static int vk_slot;
static bool vk_recording;

static bool vk_ring_alloc(VkChunkRing &r, VkDeviceSize size, VkDeviceSize chunk_size, VkBufferUsageFlags usage,
                          VkBuffer *buf, VkDeviceSize *offset, uint8_t **ptr) {
    size = (size + 15) & ~(VkDeviceSize)15;

    if (size > chunk_size) {
        VkBuf b;
        if (!vk_buffer_create(b, size, usage)) {
            return false;
        }
        r.oversize.push_back(b);
        *buf = b.buf;
        *offset = 0;
        *ptr = b.mem.mapped;
        return true;
    }

    while (true) {
        if (r.chunk < r.chunks.size()) {
            VkBuf &b = r.chunks[r.chunk];
            if (r.offset + size <= b.size) {
                *buf = b.buf;
                *offset = r.offset;
                *ptr = b.mem.mapped + r.offset;
                r.offset += size;
                return true;
            }
            r.chunk++;
            r.offset = 0;
            continue;
        }
        VkBuf b;
        if (!vk_buffer_create(b, chunk_size, usage)) {
            return false;
        }
        r.chunks.push_back(b);
    }
}

// A batch's vertices, placed on a multiple of its stride so that the chunk
// can stay bound and the draw names them by firstVertex
static bool vk_vertex_alloc(VkChunkRing &r, VkDeviceSize size, uint32_t stride, VkBuffer *buf, uint32_t *first,
                            uint8_t **ptr) {
    if (size > VK_VERTEX_CHUNK) {
        VkDeviceSize off;
        if (!vk_ring_alloc(r, size, VK_VERTEX_CHUNK, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, buf, &off, ptr)) {
            return false;
        }
        *first = 0;
        return true;
    }
    while (true) {
        if (r.chunk < r.chunks.size()) {
            VkBuf &b = r.chunks[r.chunk];
            const VkDeviceSize at = (r.offset + stride - 1) / stride * stride;
            if (at + size <= b.size) {
                *buf = b.buf;
                *first = (uint32_t)(at / stride);
                *ptr = b.mem.mapped + at;
                r.offset = at + size;
                return true;
            }
            r.chunk++;
            r.offset = 0;
            continue;
        }
        VkBuf b;
        if (!vk_buffer_create(b, VK_VERTEX_CHUNK, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT)) {
            return false;
        }
        r.chunks.push_back(b);
    }
}

static void vk_ring_reset(VkChunkRing &r) {
    r.chunk = 0;
    r.offset = 0;
    for (VkBuf &b : r.oversize) {
        vk_buffer_destroy_now(b);
    }
    r.oversize.clear();
}

/*
 * Textures, by the id gfx_pc.cpp holds. Id 0 is never handed out: it is the
 * black stand-in for a sampler the combiner does not read, or a texture that
 * has been created but not yet uploaded (which OpenGL reads as black too).
 */
struct VkTex {
    VkImg img;
    uint32_t sampler_key = 0;
    int32_t sampler = 0; // its index in the sampler table
    bool live = false;
};

static std::vector<VkTex> vk_textures;
static std::vector<uint32_t> vk_free_texture_ids;

enum { VK_BIND_NONE, VK_BIND_TEXTURE, VK_BIND_FB };

struct VkBinding {
    int kind = VK_BIND_NONE;
    uint32_t id = 0;
};

static VkBinding vk_bound[2];
static int vk_active_tile;

/*
 * Framebuffers, by the id gfx_pc.cpp holds. fb 0 is the window image, drawn
 * into in place of the window and blitted to the swapchain when presented.
 */
struct VkFb {
    uint32_t width = 0, height = 0, msaa = 1;
    bool has_depth = false;
    bool invert_y = false;
    VkImg color[2]; // [1] only for fb 0: the front buffer
    VkImg color_msaa;
    VkImg depth;
    int cur = 0;
    uint32_t sampler_key = 0;
    int32_t sampler = 0;
};

static std::vector<VkFb> vk_fbs;
static int vk_cur_fb;
static bool vk_rendering;
static int vk_rendering_fb = -1;

struct VkProgram {
    VkShaderModule vs = VK_NULL_HANDLE;
    VkShaderModule fs = VK_NULL_HANDLE;
    uint8_t num_inputs = 0;
    bool used_textures[2] = { false, false };
    uint8_t num_floats = 0;
    uint8_t attrib_sizes[16];
    uint8_t num_attribs = 0;
    // by pipeline state key; a combiner is drawn under a handful at most
    std::vector<std::pair<uint32_t, VkPipeline>> pipelines;
};

static std::map<std::pair<uint64_t, uint32_t>, VkProgram> vk_programs;
static VkProgram *vk_cur_prg;

// The pipeline state OpenGL keeps loose, set by gfx_pc.cpp between draws
static int vk_blend; // 0 none, 1 alpha, 2 modulate, 3 additive
static bool vk_depth_test, vk_depth_write;
static VkCompareOp vk_depth_op = VK_COMPARE_OP_LESS_OR_EQUAL;
static float vk_bias_slope, vk_bias_const;
static int vk_viewport[4], vk_scissor[4];
static float vk_depth_near = 0.f, vk_depth_far = 1.f;

// What is bound in the command buffer now
static VkPipeline vk_bound_pipeline;
static VkBuffer vk_bound_vbuf;
static bool vk_push_valid;

static uint32_t vk_frame_count;
static float vk_noise_scale = 1.f;
static bool vk_textures_linear[2];
static FilteringMode vk_filter_mode = FILTER_LINEAR;
static MipmapFilteringMode vk_mipmap_mode = MIPMAP_LINEAR;
static int vk_anisotropy = 1;

struct VkPush {
    int32_t frame_count;
    float noise_scale;
    int32_t three_point_filter0;
    int32_t three_point_filter1;
    int32_t tex[2];     // slots in the texture table
    int32_t sampler[2]; // and in the sampler table
};

static VkPush vk_pushed;

struct VkSamplerEntry {
    VkSampler sampler;
    int32_t index;
};
static std::map<uint32_t, VkSamplerEntry> vk_samplers;

// Swapchain
static VkSwapchainKHR vk_swapchain;
static VkFormat vk_swap_format;
static VkExtent2D vk_swap_extent;
// The window's drawable size when the swapchain was made, which is what a
// resize is noticed by (the surface's own extent can lag it by a frame)
static int vk_swap_drawable_w, vk_swap_drawable_h;
static std::vector<VkImage> vk_swap_images;
static std::vector<VkSemaphore> vk_swap_done;
static bool vk_swap_dirty;
// --vk-no-present: frames are drawn and submitted but never shown, which is
// what a benchmark wants on a display (Xvfb) that can only present through
// a CPU copy
static bool vk_no_present;
static int vk_swap_interval = 1;
static std::vector<VkPresentModeKHR> vk_present_modes;

// Vivid Colours
static VkPipeline vk_grade_pipeline;
static VkImg vk_grade_image;
static bool vk_grade_failed;

// Readback: one buffer for screenshots, a ring of two for the recorder
static VkBuf vk_readback;

#define VK_CAPTURE_BUFS 2
static VkBuf vk_capture_bufs[VK_CAPTURE_BUFS];
static uint64_t vk_capture_serial[VK_CAPTURE_BUFS];
static int vk_capture_width, vk_capture_height;
static int vk_capture_next, vk_capture_pending;

static void vk_fail(const char *fmt, ...) {
    char msg[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    sysLogPrintf(LOG_ERROR, "Vulkan: %s", msg);
}

/*
 * Recording
 */

// --gfxstats: how long the CPU sat waiting for the GPU, which is the one
// place this renderer blocks
static uint64_t vk_stat_wait_ns;
static uint32_t vk_stat_frames;

static void vk_wait_worker(uint64_t serial);

static void vk_wait_slot(int s) {
    VkSlot &sl = vk_slots[s];
    if (sl.serial > vk_completed) {
        vk_wait_worker(sl.serial);
        if (vkGetFenceStatus(vk_dev, sl.fence) != VK_SUCCESS) {
            const uint64_t t0 = SDL_GetPerformanceCounter();
            vkWaitForFences(vk_dev, 1, &sl.fence, VK_TRUE, UINT64_MAX);
            vk_stat_wait_ns += (SDL_GetPerformanceCounter() - t0) * 1000000000ull / SDL_GetPerformanceFrequency();
        }
        vk_completed = sl.serial;
    }
}

static void vk_reset_bound_state(void) {
    vk_rendering = false;
    vk_rendering_fb = -1;
    vk_bound_pipeline = VK_NULL_HANDLE;
    vk_bound_vbuf = VK_NULL_HANDLE;
    vk_push_valid = false;
}

static void vk_begin_command_buffers(VkSlot &sl) {
    vkResetCommandPool(vk_dev, sl.pool, 0);
    VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(sl.upload, &bi);
    vkBeginCommandBuffer(sl.cmd, &bi);
    sl.upload_used = false;
    vk_stream = &sl.stream;
    sl.stream.size = 0;
    vk_recording = true;
    vk_reset_bound_state();
    rcCmdBindDescriptorSets(VK_MAIN_CB, VK_PIPELINE_BIND_POINT_GRAPHICS, vk_pipeline_layout, 0, 1, &vk_bindless_set, 0,
                            NULL);
}

static void vk_begin_recording(void) {
    VkSlot &sl = vk_slots[vk_slot];

    vk_wait_slot(vk_slot);
    vk_collect();

    vk_ring_reset(sl.vertex);
    vk_ring_reset(sl.staging);

    vk_begin_command_buffers(sl);
}

static inline void vk_ensure_recording(void) {
    if (!vk_recording) {
        vk_begin_recording();
    }
}

static inline VkCommandBuffer vk_cmd(void) {
    vk_ensure_recording();
    return VK_MAIN_CB;
}

static void vk_end_rendering(void) {
    if (vk_rendering) {
        rcCmdEndRendering(VK_MAIN_CB);
        vk_rendering = false;
        vk_rendering_fb = -1;
    }
}

/*
 * The worker. One frame is handed over at a time; the game's thread waits for
 * the worker only when it hands over the next before the last is done, or
 * when it needs a finished frame (a screenshot, reusing a frame's memory).
 */
struct VkJob {
    int slot;
    uint64_t serial;
    bool present;
    VkImage src;
    uint32_t width, height;
};

static std::thread *vk_worker;
// Made once and never destroyed, like the thread: at exit a static condition
// variable's destructor waits for its waiters, and the worker is one for good
static std::mutex &vk_worker_mtx = *new std::mutex;
static std::condition_variable &vk_worker_cv = *new std::condition_variable;
static bool vk_job_pending;
static VkJob vk_job;
static uint64_t vk_worker_done; // the last serial the worker has submitted
static bool vk_threaded;
static std::atomic<bool> vk_swap_dirty_async;

static void vk_run_job(const VkJob &job) {
    VkSlot &sl = vk_slots[job.slot];
    VkCommandBuffer cb = sl.cmd;

    vk_replay(cb, sl.stream);

    uint32_t index = 0;
    bool acquired = false;
    VkResult r = VK_ERROR_OUT_OF_DATE_KHR;
    if (job.present) {
        r = vkAcquireNextImageKHR(vk_dev, vk_swapchain, UINT64_MAX, sl.acquired, VK_NULL_HANDLE, &index);
        acquired = r == VK_SUCCESS || r == VK_SUBOPTIMAL_KHR;
        if (!acquired) {
            vk_swap_dirty_async = true;
        }
    }

    if (acquired) {
        VkImage dst = vk_swap_images[index];
        vk_barrier(cb, dst, VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, VK_IMAGE_LAYOUT_UNDEFINED,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

        // Row 0 of the window image is the bottom of the picture
        VkImageBlit blit = {};
        blit.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        blit.srcOffsets[0] = { 0, (int32_t)job.height, 0 };
        blit.srcOffsets[1] = { (int32_t)job.width, 0, 1 };
        blit.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        blit.dstOffsets[1] = { (int32_t)vk_swap_extent.width, (int32_t)vk_swap_extent.height, 1 };
        const bool same = job.width == vk_swap_extent.width && job.height == vk_swap_extent.height;
        vkCmdBlitImage(cb, job.src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                       &blit, same ? VK_FILTER_NEAREST : VK_FILTER_LINEAR);

        vk_barrier(cb, dst, VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                   VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
    }

    vkEndCommandBuffer(sl.upload);
    vkEndCommandBuffer(cb);

    VkCommandBuffer cbs[2];
    uint32_t n = 0;
    if (sl.upload_used) {
        cbs[n++] = sl.upload;
    }
    cbs[n++] = cb;

    const VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    si.commandBufferCount = n;
    si.pCommandBuffers = cbs;
    if (acquired) {
        si.waitSemaphoreCount = 1;
        si.pWaitSemaphores = &sl.acquired;
        si.pWaitDstStageMask = &wait_stage;
        si.signalSemaphoreCount = 1;
        si.pSignalSemaphores = &vk_swap_done[index];
    }

    vkResetFences(vk_dev, 1, &sl.fence);
    const VkResult sr = vkQueueSubmit(vk_queue, 1, &si, sl.fence);
    if (sr != VK_SUCCESS) {
        sysFatalError("Vulkan: queue submit failed (%d).\nThe driver may have lost the device.", (int)sr);
    }

    if (acquired) {
        VkPresentInfoKHR pi = { VK_STRUCTURE_TYPE_PRESENT_INFO_KHR };
        pi.waitSemaphoreCount = 1;
        pi.pWaitSemaphores = &vk_swap_done[index];
        pi.swapchainCount = 1;
        pi.pSwapchains = &vk_swapchain;
        pi.pImageIndices = &index;
        const VkResult pr = vkQueuePresentKHR(vk_queue, &pi);
        if (pr == VK_ERROR_OUT_OF_DATE_KHR || pr == VK_SUBOPTIMAL_KHR || r == VK_SUBOPTIMAL_KHR) {
            vk_swap_dirty_async = true;
        }
    }
}

static void vk_worker_main(void) {
    while (true) {
        VkJob job;
        {
            std::unique_lock<std::mutex> lk(vk_worker_mtx);
            vk_worker_cv.wait(lk, [] { return vk_job_pending; });
            job = vk_job;
        }
        vk_run_job(job);
        {
            std::lock_guard<std::mutex> lk(vk_worker_mtx);
            vk_worker_done = job.serial;
            vk_job_pending = false;
        }
        vk_worker_cv.notify_all();
    }
}

// Until the worker has submitted everything up to serial
static void vk_wait_worker(uint64_t serial) {
    if (!vk_threaded) {
        return;
    }
    std::unique_lock<std::mutex> lk(vk_worker_mtx);
    vk_worker_cv.wait(lk, [serial] { return vk_worker_done >= serial && !vk_job_pending; });
}

static void vk_drain(void) {
    vk_wait_worker(vk_submitted);
}

// The frame recorded so far goes out: to the worker, or submitted here
static void vk_handoff(bool present, VkImage src, uint32_t width, uint32_t height) {
    VkSlot &sl = vk_slots[vk_slot];

    vk_end_rendering();
    sl.serial = ++vk_submitted;
    vk_recording = false;

    const VkJob job = { vk_slot, sl.serial, present, src, width, height };
    if (!vk_threaded) {
        vk_run_job(job);
        vk_worker_done = job.serial;
        return;
    }

    {
        std::unique_lock<std::mutex> lk(vk_worker_mtx);
        vk_worker_cv.wait(lk, [] { return !vk_job_pending; });
        vk_job = job;
        vk_job_pending = true;
    }
    vk_worker_cv.notify_all();
}

// Everything so far out to the GPU and back, carrying on in the same frame:
// for a screenshot, which has to have the finished pixels in hand.
static void vk_flush_and_wait(void) {
    if (!vk_recording) {
        return;
    }
    vk_handoff(false, VK_NULL_HANDLE, 0, 0);
    vk_wait_slot(vk_slot);
    vk_collect();
    vk_begin_command_buffers(vk_slots[vk_slot]);
}

static void vk_wait_serial(uint64_t serial) {
    if (serial > vk_submitted) {
        vk_flush_and_wait();
        return;
    }
    for (int s = 0; s < VK_FRAMES; s++) {
        if (vk_slots[s].serial >= serial) {
            vk_wait_slot(s);
        }
    }
}

// Moves an image into a layout on the frame's command buffer, stepping out of
// the render pass first if it has to (a barrier cannot go inside one).
static void vk_image_to(VkImg &img, VkImageLayout layout) {
    if (!img.image || img.layout == layout) {
        return;
    }
    VkCommandBuffer cb = vk_cmd();
    vk_end_rendering();

    if (img.layout == VK_IMAGE_LAYOUT_UNDEFINED && img.clear_first) {
        vk_barrier(cb, img.image, img.aspect, 0, img.mips, VK_IMAGE_LAYOUT_UNDEFINED,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkImageSubresourceRange range = { img.aspect, 0, img.mips, 0, 1 };
        if (img.aspect & VK_IMAGE_ASPECT_DEPTH_BIT) {
            VkClearDepthStencilValue v = { 1.f, 0 };
            rcCmdClearDepthStencilImage(cb, img.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &v, 1, &range);
        } else {
            VkClearColorValue v = { { 0.f, 0.f, 0.f, 1.f } };
            rcCmdClearColorImage(cb, img.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &v, 1, &range);
        }
        img.layout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        img.clear_first = false;
        // and a barrier after the clear even into the same layout, or a copy
        // into the image next races it
    }

    vk_barrier(cb, img.image, img.aspect, 0, img.mips, img.layout, layout);
    img.layout = layout;
}

/*
 * Samplers, by a key packed from what OpenGL would have set on the texture
 */
enum { VK_WRAP_REPEAT, VK_WRAP_MIRROR, VK_WRAP_CLAMP, VK_WRAP_MIRROR_CLAMP };

static uint32_t vk_sampler_key(bool min_linear, bool mag_linear, int mipmode, int wrap_s, int wrap_t, int aniso) {
    // mipmode: 0 none, 1 nearest, 2 linear
    return (min_linear ? 1 : 0) | (mag_linear ? 2 : 0) | (mipmode << 2) | (wrap_s << 4) | (wrap_t << 6) |
           ((uint32_t)aniso << 8);
}

static VkSamplerAddressMode vk_wrap_mode(int w) {
    switch (w) {
        case VK_WRAP_MIRROR:
            return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
        case VK_WRAP_CLAMP:
            return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        case VK_WRAP_MIRROR_CLAMP:
            return vk_have_mirror_clamp ? VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE
                                        : VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
        default:
            return VK_SAMPLER_ADDRESS_MODE_REPEAT;
    }
}

static int32_t vk_get_sampler(uint32_t key) {
    auto it = vk_samplers.find(key);
    if (it != vk_samplers.end()) {
        return it->second.index;
    }
    if (vk_samplers.size() >= VK_MAX_SAMPLER_SLOTS) {
        return 0;
    }

    const int mipmode = (key >> 2) & 3;
    const int aniso = (int)(key >> 8);

    VkSamplerCreateInfo ci = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    ci.minFilter = (key & 1) ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    ci.magFilter = (key & 2) ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    ci.mipmapMode = mipmode == 2 ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
    ci.addressModeU = vk_wrap_mode((key >> 4) & 3);
    ci.addressModeV = vk_wrap_mode((key >> 6) & 3);
    ci.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    ci.minLod = 0.f;
    // No mipmapping is OpenGL's GL_NEAREST/GL_LINEAR minification: level 0 only
    ci.maxLod = mipmode ? VK_LOD_CLAMP_NONE : 0.f;
    if (mipmode && aniso > 1 && vk_have_anisotropy) {
        ci.anisotropyEnable = VK_TRUE;
        ci.maxAnisotropy = std::min((float)aniso, vk_props.limits.maxSamplerAnisotropy);
    }

    VkSampler s = VK_NULL_HANDLE;
    vkCreateSampler(vk_dev, &ci, NULL, &s);
    const int32_t index = (int32_t)vk_samplers.size();
    vk_samplers[key] = { s, index };

    VkDescriptorImageInfo ii = { s, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED };
    VkWriteDescriptorSet w = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
    w.dstSet = vk_bindless_set;
    w.dstBinding = 1;
    w.dstArrayElement = (uint32_t)index;
    w.descriptorCount = 1;
    w.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
    w.pImageInfo = &ii;
    vkUpdateDescriptorSets(vk_dev, 1, &w, 0, NULL);
    return index;
}

/*
 * Shaders: the OpenGL renderer's, in Vulkan's GLSL
 */

#define RAND_NOISE "((random(vec3(floor(gl_FragCoord.xy * noise_scale), float(frame_count))) + 1.0) / 2.0)"

static const char *shader_item_to_str(uint32_t item, bool with_alpha, bool only_alpha, bool inputs_have_alpha,
                                      bool hint_single_element) {
    if (!only_alpha) {
        switch (item) {
            case SHADER_0:
                return with_alpha ? "vec4(0.0, 0.0, 0.0, 0.0)" : "vec3(0.0, 0.0, 0.0)";
            case SHADER_1:
                return with_alpha ? "vec4(1.0, 1.0, 1.0, 1.0)" : "vec3(1.0, 1.0, 1.0)";
            case SHADER_INPUT_1:
                return with_alpha || !inputs_have_alpha ? "vInput1" : "vInput1.rgb";
            case SHADER_INPUT_2:
                return with_alpha || !inputs_have_alpha ? "vInput2" : "vInput2.rgb";
            case SHADER_INPUT_3:
                return with_alpha || !inputs_have_alpha ? "vInput3" : "vInput3.rgb";
            case SHADER_INPUT_4:
                return with_alpha || !inputs_have_alpha ? "vInput4" : "vInput4.rgb";
            case SHADER_TEXEL0:
                return with_alpha ? "texVal0" : "texVal0.rgb";
            case SHADER_TEXEL0A:
                return hint_single_element ? "texVal0.a"
                                           : (with_alpha ? "vec4(texVal0.a, texVal0.a, texVal0.a, texVal0.a)"
                                                         : "vec3(texVal0.a, texVal0.a, texVal0.a)");
            case SHADER_TEXEL1A:
                return hint_single_element ? "texVal1.a"
                                           : (with_alpha ? "vec4(texVal1.a, texVal1.a, texVal1.a, texVal1.a)"
                                                         : "vec3(texVal1.a, texVal1.a, texVal1.a)");
            case SHADER_TEXEL1:
                return with_alpha ? "texVal1" : "texVal1.rgb";
            case SHADER_COMBINED:
                return with_alpha ? "texel" : "texel.rgb";
            case SHADER_NOISE:
                return with_alpha ? "vec4(" RAND_NOISE ", " RAND_NOISE ", " RAND_NOISE ", " RAND_NOISE ")"
                                  : "vec3(" RAND_NOISE ", " RAND_NOISE ", " RAND_NOISE ")";
        }
    } else {
        switch (item) {
            case SHADER_0:
                return "0.0";
            case SHADER_1:
                return "1.0";
            case SHADER_INPUT_1:
                return "vInput1.a";
            case SHADER_INPUT_2:
                return "vInput2.a";
            case SHADER_INPUT_3:
                return "vInput3.a";
            case SHADER_INPUT_4:
                return "vInput4.a";
            case SHADER_TEXEL0:
                return "texVal0.a";
            case SHADER_TEXEL0A:
                return "texVal0.a";
            case SHADER_TEXEL1A:
                return "texVal1.a";
            case SHADER_TEXEL1:
                return "texVal1.a";
            case SHADER_COMBINED:
                return "texel.a";
            case SHADER_NOISE:
                return RAND_NOISE;
        }
    }
    return "";
}

static void append_formula(std::string &s, uint8_t c[2][4], bool do_single, bool do_multiply, bool do_mix,
                           bool with_alpha, bool only_alpha, bool opt_alpha) {
    if (do_single) {
        s += shader_item_to_str(c[only_alpha][3], with_alpha, only_alpha, opt_alpha, false);
    } else if (do_multiply) {
        s += shader_item_to_str(c[only_alpha][0], with_alpha, only_alpha, opt_alpha, false);
        s += " * ";
        s += shader_item_to_str(c[only_alpha][2], with_alpha, only_alpha, opt_alpha, true);
    } else if (do_mix) {
        s += "mix(";
        s += shader_item_to_str(c[only_alpha][1], with_alpha, only_alpha, opt_alpha, false);
        s += ", ";
        s += shader_item_to_str(c[only_alpha][0], with_alpha, only_alpha, opt_alpha, false);
        s += ", ";
        s += shader_item_to_str(c[only_alpha][2], with_alpha, only_alpha, opt_alpha, true);
        s += ")";
    } else {
        s += "(";
        s += shader_item_to_str(c[only_alpha][0], with_alpha, only_alpha, opt_alpha, false);
        s += " - ";
        s += shader_item_to_str(c[only_alpha][1], with_alpha, only_alpha, opt_alpha, false);
        s += ") * ";
        s += shader_item_to_str(c[only_alpha][2], with_alpha, only_alpha, opt_alpha, true);
        s += " + ";
        s += shader_item_to_str(c[only_alpha][3], with_alpha, only_alpha, opt_alpha, false);
    }
}

static std::string strf(const char *fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    return buf;
}

static const char *vk_vec_type(int size) {
    static const char *types[] = { "float", "float", "vec2", "vec3", "vec4" };
    return types[size];
}

/*
 * The shader cache. A combiner's shaders are compiled from GLSL the first time
 * it is drawn, and a pipeline is built for each state it is drawn under, and
 * both are paid for in the frame that first draws it - a hitch, every run. The
 * SPIR-V is kept on disk under a hash of the GLSL it came from (so whatever
 * setting changed the source, a stale entry is simply never asked for) and the
 * driver's pipeline cache beside it (which the driver checks is its own), so
 * a combiner is compiled once per install instead of once per run.
 */
#define VK_SPV_CACHE_PATH "$S/vulkan-shaders.bin"
#define VK_PIPELINE_CACHE_PATH "$S/vulkan-pipelines.bin"
#define VK_SPV_CACHE_MAGIC 0x31565053564b4450ull // "PDKVSPV1"

static std::unordered_map<uint64_t, std::vector<uint32_t>> vk_spv_cache;
static bool vk_cache_dirty;
static uint32_t vk_cache_idle_frames;

// --gfxstats
static uint32_t vk_stat_compiles, vk_stat_cached, vk_stat_pipelines;
static double vk_stat_compile_ms, vk_stat_pipeline_ms;

static double vk_now_ms(void) {
    return SDL_GetPerformanceCounter() * 1000.0 / SDL_GetPerformanceFrequency();
}

static uint64_t vk_hash(const std::string &src, bool fragment) {
    uint64_t h = 0xcbf29ce484222325ull ^ (fragment ? 1 : 2);
    for (unsigned char c : src) {
        h = (h ^ c) * 0x100000001b3ull;
    }
    return h;
}

static void vk_cache_load(void) {
    u32 size = 0;
    uint8_t *data = (uint8_t *)fsFileLoad(VK_SPV_CACHE_PATH, &size);
    if (!data) {
        return;
    }
    size_t pos = 0;
    auto take = [&](void *dst, size_t n) {
        if (pos + n > size) {
            return false;
        }
        memcpy(dst, data + pos, n);
        pos += n;
        return true;
    };
    uint64_t magic = 0;
    if (take(&magic, 8) && magic == VK_SPV_CACHE_MAGIC) {
        uint64_t hash;
        uint32_t words;
        while (take(&hash, 8) && take(&words, 4) && words && pos + words * 4ull <= size) {
            std::vector<uint32_t> spv(words);
            take(spv.data(), words * 4);
            vk_spv_cache[hash] = std::move(spv);
        }
    }
    free(data);
    sysLogPrintf(LOG_NOTE, "Vulkan: %u shaders in the cache", (unsigned)vk_spv_cache.size());
}

// with_pipelines: the driver's pipeline cache too, which is a driver call and
// so not made at exit, where SDL may have unloaded the driver already
static void vk_cache_save_to(bool with_pipelines) {
    if (!vk_dev || !vk_cache_dirty) {
        return;
    }
    vk_cache_dirty = false;

    FILE *f = fsFileOpenWrite(VK_SPV_CACHE_PATH);
    if (f) {
        const uint64_t magic = VK_SPV_CACHE_MAGIC;
        fwrite(&magic, 8, 1, f);
        for (const auto &e : vk_spv_cache) {
            const uint32_t words = (uint32_t)e.second.size();
            fwrite(&e.first, 8, 1, f);
            fwrite(&words, 4, 1, f);
            fwrite(e.second.data(), 4, words, f);
        }
        fclose(f);
    }

    size_t size = 0;
    if (with_pipelines && vk_pipeline_cache && vkGetPipelineCacheData(vk_dev, vk_pipeline_cache, &size, NULL) == VK_SUCCESS && size) {
        std::vector<uint8_t> data(size);
        if (vkGetPipelineCacheData(vk_dev, vk_pipeline_cache, &size, data.data()) == VK_SUCCESS) {
            FILE *pf = fsFileOpenWrite(VK_PIPELINE_CACHE_PATH);
            if (pf) {
                fwrite(data.data(), 1, size, pf);
                fclose(pf);
            }
        }
    }
}

static void vk_cache_save(void) {
    vk_cache_save_to(true);
}

static void vk_drain(void);

static void vk_cache_save_at_exit(void) {
    // nothing may be in the driver while the process comes down around it
    vk_drain();
    vk_cache_save_to(false);
}

static VkShaderModule vk_compile(const std::string &src, bool fragment, const char *what, std::string *error) {
    const uint64_t hash = vk_hash(src, fragment);
    auto it = vk_spv_cache.find(hash);
    if (it != vk_spv_cache.end()) {
        VkShaderModuleCreateInfo ci = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
        ci.codeSize = it->second.size() * 4;
        ci.pCode = it->second.data();
        VkShaderModule mod = VK_NULL_HANDLE;
        vkCreateShaderModule(vk_dev, &ci, NULL, &mod);
        vk_stat_cached++;
        return mod;
    }

    const double t0 = vk_now_ms();
    shaderc_compile_options_t opts = shaderc_compile_options_initialize();
    shaderc_compile_options_set_target_env(opts, shaderc_target_env_vulkan, shaderc_env_version_vulkan_1_1);
    shaderc_compile_options_set_optimization_level(opts, shaderc_optimization_level_zero);

    shaderc_compilation_result_t res =
        shaderc_compile_into_spv(vk_shaderc, src.c_str(), src.size(),
                                 fragment ? shaderc_fragment_shader : shaderc_vertex_shader, what, "main", opts);
    shaderc_compile_options_release(opts);

    VkShaderModule mod = VK_NULL_HANDLE;
    if (shaderc_result_get_compilation_status(res) == shaderc_compilation_status_success) {
        VkShaderModuleCreateInfo ci = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
        ci.codeSize = shaderc_result_get_length(res);
        ci.pCode = (const uint32_t *)shaderc_result_get_bytes(res);
        vkCreateShaderModule(vk_dev, &ci, NULL, &mod);
        const uint32_t *words = (const uint32_t *)shaderc_result_get_bytes(res);
        vk_spv_cache[hash].assign(words, words + ci.codeSize / 4);
        vk_cache_dirty = true;
        vk_cache_idle_frames = 0;
    } else if (error) {
        *error = shaderc_result_get_error_message(res);
    }
    shaderc_result_release(res);
    vk_stat_compiles++;
    vk_stat_compile_ms += vk_now_ms() - t0;
    return mod;
}

static struct ShaderProgram *gfx_vk_create_and_load_new_shader(uint64_t shader_id0, uint32_t shader_id1) {
    struct CCFeatures cc_features = { 0 };
    gfx_cc_get_features(shader_id0, shader_id1, &cc_features);

    const bool three_point = vk_filter_mode == FILTER_THREE_POINT;

    // The vertex layout, in the order gfx_pc.cpp writes it
    std::vector<std::pair<std::string, int>> attrs;
    attrs.push_back({ "aVtxPos", 4 });
    for (int i = 0; i < 2; i++) {
        if (cc_features.used_textures[i]) {
            attrs.push_back({ strf("aTexCoord%d", i), 2 });
            for (int j = 0; j < 2; j++) {
                if (cc_features.clamp[i][j]) {
                    attrs.push_back({ strf("aTexClamp%s%d", j == 0 ? "S" : "T", i), 1 });
                }
            }
        }
    }
    if (cc_features.opt_fog) {
        attrs.push_back({ "aFog", 4 });
        attrs.push_back({ "aFogOffset", 1 });
    }
    if (cc_features.opt_grayscale) {
        attrs.push_back({ "aGrayscaleColor", 4 });
    }
    if (cc_features.opt_envmap) {
        attrs.push_back({ "aEnvNormal", 3 });
        attrs.push_back({ "aEnvPos", 3 });
    }
    for (int i = 0; i < cc_features.num_inputs; i++) {
        attrs.push_back({ strf("aInput%d", i + 1), cc_features.opt_alpha ? 4 : 3 });
    }

    // Everything but the position passes straight through to the fragment
    // shader under its v name, plus the fog's z and w
    std::vector<std::pair<std::string, int>> vars;
    for (size_t k = 1; k < attrs.size(); k++) {
        vars.push_back({ "v" + attrs[k].first.substr(1), attrs[k].second });
    }
    if (cc_features.opt_fog) {
        vars.push_back({ "vFogZW", 2 });
    }

    std::string vs = "#version 450\n";
    for (size_t k = 0; k < attrs.size(); k++) {
        vs += strf("layout(location = %d) in %s %s;\n", (int)k, vk_vec_type(attrs[k].second), attrs[k].first.c_str());
    }
    for (size_t k = 0; k < vars.size(); k++) {
        vs += strf("layout(location = %d) out %s %s;\n", (int)k, vk_vec_type(vars[k].second), vars[k].first.c_str());
    }
    vs += "void main() {\n";
    for (size_t k = 1; k < attrs.size(); k++) {
        vs += strf("    v%s = %s;\n", attrs[k].first.c_str() + 1, attrs[k].first.c_str());
    }
    if (cc_features.opt_fog) {
        vs += "    vFogZW = aVtxPos.zw;\n";
    }
    vs += "    gl_Position = aVtxPos;\n";
    if (!vk_have_depth_clamp) {
        // as the OpenGL renderer does without GL_DEPTH_CLAMP
        vs += "    gl_Position.z *= 0.3;\n";
    }
    // OpenGL's -1..1 depth into Vulkan's 0..1, the same value in the buffer
    vs += "    gl_Position.z = (gl_Position.z + gl_Position.w) * 0.5;\n";
    vs += "}\n";

    std::string fs = "#version 450\n";
    fs += "#define SAMPLE_TEX(t, s, uv) texture(sampler2D(t, s), uv)\n";
    fs += "#define WRAP(x, low, high) mod((x)-(low), (high)-(low)) + (low)\n";
    fs += "#define TEX_OFFSET(t, s, uv, texSize, off) SAMPLE_TEX(t, s, uv - (off)/texSize)\n";
    for (size_t k = 0; k < vars.size(); k++) {
        fs += strf("layout(location = %d) in %s %s;\n", (int)k, vk_vec_type(vars[k].second), vars[k].first.c_str());
    }
    fs += strf("layout(set = 0, binding = 0) uniform texture2D uTextures[%u];\n", vk_max_texture_slots);
    fs += strf("layout(set = 0, binding = 1) uniform sampler uSamplers[%d];\n", VK_MAX_SAMPLER_SLOTS);
    fs += "layout(push_constant) uniform Push {\n"
          "    int frame_count;\n"
          "    float noise_scale;\n"
          "    int three_point_filter0;\n"
          "    int three_point_filter1;\n"
          "    int tex0, tex1, smp0, smp1;\n"
          "};\n";
    // A texture unit is its image and its sampler, passed to a function as
    // the pair (a combined sampler cannot be), and combined where it is read
    fs += "#define TEX0 uTextures[tex0], uSamplers[smp0]\n"
          "#define TEX1 uTextures[tex1], uSamplers[smp1]\n"
          "#define uTex0 sampler2D(uTextures[tex0], uSamplers[smp0])\n"
          "#define uTex1 sampler2D(uTextures[tex1], uSamplers[smp1])\n";
    fs += "layout(location = 0) out vec4 outColor;\n";

    fs += "float random(in vec3 value) {\n"
          "    float random = dot(sin(value), vec3(12.9898, 78.233, 37.719));\n"
          "    return fract(sin(random) * 143758.5453);\n"
          "}\n";

    if (three_point) {
        fs += "vec4 filter3point(in texture2D tex, in sampler smp, in vec2 texCoord, in vec2 texSize) {\n"
              "    vec2 offset = fract(texCoord*texSize - vec2(0.5));\n"
              "    offset -= step(1.0, offset.x + offset.y);\n"
              "    vec4 c0 = TEX_OFFSET(tex, smp, texCoord, texSize, offset);\n"
              "    vec4 c1 = TEX_OFFSET(tex, smp, texCoord, texSize, vec2(offset.x - sign(offset.x), offset.y));\n"
              "    vec4 c2 = TEX_OFFSET(tex, smp, texCoord, texSize, vec2(offset.x, offset.y - sign(offset.y)));\n"
              "    return c0 + abs(offset.x)*(c1-c0) + abs(offset.y)*(c2-c0);\n"
              "}\n";
    }

    if (cc_features.opt_blur) {
        fs += three_point ? "vec4 hookTexture2D(in texture2D t, in sampler s, in vec2 uv, in vec2 texSize, in int three_point_filter) {\n"
                          : "vec4 hookTexture2D(in texture2D t, in sampler s, in vec2 uv, in vec2 texSize) {\n";
        fs += "    vec4 cw = vec4(0.0);\n"
              "    for (int i = 0; i < 16; ++i) {\n"
              "        vec2 xy = vec2(float(i & 3), float(i >> 2));\n"
              "        float w = 0.009947 - length(xy) * 0.001;\n"
              "        vec2 scaled_uv = uv + (vec2(-1.5) + xy) / texSize;\n";
        fs += three_point ? "        vec4 tex = mix(SAMPLE_TEX(t, s, scaled_uv), filter3point(t, s, scaled_uv, texSize), float(three_point_filter));\n"
                          : "        vec4 tex = SAMPLE_TEX(t, s, scaled_uv);\n";
        fs += "        cw += vec4(tex.rgb * w, w);\n"
              "    }\n"
              "    return vec4(cw.rgb / cw.a, 1.0);\n"
              "}\n";
    } else if (three_point) {
        fs += "vec4 hookTexture2D(in texture2D tex, in sampler smp, in vec2 uv, in vec2 texSize, in int three_point_filter) {\n"
              "    return mix(SAMPLE_TEX(tex, smp, uv), filter3point(tex, smp, uv, texSize), float(three_point_filter));\n"
              "}\n";
    } else {
        fs += "vec4 hookTexture2D(in texture2D tex, in sampler smp, in vec2 uv, in vec2 texSize) {\n"
              "    return SAMPLE_TEX(tex, smp, uv);\n"
              "}\n";
    }

    // Stretched Edges; see the OpenGL renderer
    if (cc_features.clamp[0][0] || cc_features.clamp[0][1] || cc_features.clamp[1][0] || cc_features.clamp[1][1]) {
        switch (gfx_clamped_edge_mode) {
            case CLAMPED_EDGE_MIRROR:
                fs += "float texEdge(float c, float lo, float hi) {\n"
                      "    float per = 2.0 * (hi + lo);\n"
                      "    float m = mod(c, per);\n"
                      "    return clamp(min(m, per - m), lo, hi);\n"
                      "}\n";
                break;
            case CLAMPED_EDGE_REPEAT:
                fs += "float texEdge(float c, float lo, float hi) {\n"
                      "    return clamp(mod(c, hi + lo), lo, hi);\n"
                      "}\n";
                break;
            default:
                fs += "float texEdge(float c, float lo, float hi) {\n"
                      "    return clamp(c, lo, hi);\n"
                      "}\n";
                break;
        }
    }

    fs += "void main() {\n";

    for (int i = 0; i < 2; i++) {
        // G_ENVMAP_EXT; see the OpenGL renderer
        if (i == 0 && cc_features.opt_envmap && cc_features.used_textures[0]) {
            fs += "    vec2 texSize0 = vec2(textureSize(uTex0, 0));\n"
                  "    vec3 envN = normalize(vEnvNormal);\n"
                  "    vec3 envE = normalize(vEnvPos);\n"
                  "    vec3 envR = envE - 2.0 * dot(envE, envN) * envN;\n"
                  "    float envM = 2.0 * sqrt(envR.x * envR.x + envR.y * envR.y + (envR.z + 1.0) * (envR.z + 1.0));\n"
                  "    vec2 envS = envM > 0.000001 ? envR.xy / envM + 0.5 : vec2(0.5);\n"
                  "    envS = clamp(envS, vec2(0.5 / 256.0), vec2(1.0 - 0.5 / 256.0));\n"
                  "    float envCells = max(1.0, floor(1.0 / max(vTexCoord0.t, 0.01) + 0.5));\n"
                  "    float envCell = clamp(floor(vTexCoord0.s * envCells), 0.0, envCells - 1.0);\n"
                  "    vec2 vTexCoordAdj0 = vec2((envCell + envS.x) / envCells, envS.y);\n"
                  "    vec4 texVal0 = textureLod(uTex0, vTexCoordAdj0, 0.0);\n";
            continue;
        }

        if (cc_features.used_textures[i]) {
            const bool s = cc_features.clamp[i][0], t = cc_features.clamp[i][1];

            fs += strf("    vec2 texSize%d = vec2(textureSize(uTex%d, 0));\n", i, i);

            if (!s && !t) {
                fs += strf("    vec2 vTexCoordAdj%d = vTexCoord%d;\n", i, i);
            } else if (s && t) {
                fs += strf("    vec2 vTexCoordAdj%d = vec2(texEdge(vTexCoord%d.s, 0.5 / texSize%d.s, vTexClampS%d), "
                           "texEdge(vTexCoord%d.t, 0.5 / texSize%d.t, vTexClampT%d));\n",
                           i, i, i, i, i, i, i);
            } else if (s) {
                fs += strf("    vec2 vTexCoordAdj%d = vec2(texEdge(vTexCoord%d.s, 0.5 / texSize%d.s, vTexClampS%d), "
                           "vTexCoord%d.t);\n",
                           i, i, i, i, i);
            } else {
                fs += strf("    vec2 vTexCoordAdj%d = vec2(vTexCoord%d.s, texEdge(vTexCoord%d.t, 0.5 / texSize%d.t, "
                           "vTexClampT%d));\n",
                           i, i, i, i, i);
            }

            if (three_point) {
                fs += strf("    vec4 texVal%d = hookTexture2D(TEX%d, vTexCoordAdj%d, texSize%d, three_point_filter%d);\n",
                           i, i, i, i, i);
            } else {
                fs += strf("    vec4 texVal%d = hookTexture2D(TEX%d, vTexCoordAdj%d, texSize%d);\n", i, i, i, i);
            }
        }
    }

    if (cc_features.opt_text_outline && cc_features.used_textures[0] && cc_features.used_textures[1]) {
        static const char *offs[8] = { " px.x, 0.0", "-px.x, 0.0", "0.0,  px.y", "0.0, -px.y",
                                       " px.x,  px.y", "-px.x,  px.y", " px.x, -px.y", "-px.x, -px.y" };
        fs += "    {\n"
              "        vec2 px = 0.5 / texSize1;\n"
              "        float o = min(1.0, texVal1.a * 2.5);\n";
        for (int k = 0; k < 8; k++) {
            fs += strf("        o = max(o, min(1.0, texture(uTex1, vTexCoordAdj1 + vec2(%s)).a * 2.5)%s);\n", offs[k],
                       k < 4 ? "" : " * 0.8");
        }
        fs += "        texVal0.a = min(texVal0.a, o);\n"
              "    }\n";
    }

    fs += cc_features.opt_alpha ? "    vec4 texel;\n" : "    vec3 texel;\n";
    for (int c = 0; c < (cc_features.opt_2cyc ? 2 : 1); c++) {
        fs += "    texel = ";
        if (!cc_features.color_alpha_same[c] && cc_features.opt_alpha) {
            fs += "vec4(";
            append_formula(fs, cc_features.c[c], cc_features.do_single[c][0], cc_features.do_multiply[c][0],
                           cc_features.do_mix[c][0], false, false, true);
            fs += ", ";
            append_formula(fs, cc_features.c[c], cc_features.do_single[c][1], cc_features.do_multiply[c][1],
                           cc_features.do_mix[c][1], true, true, true);
            fs += ")";
        } else {
            append_formula(fs, cc_features.c[c], cc_features.do_single[c][0], cc_features.do_multiply[c][0],
                           cc_features.do_mix[c][0], cc_features.opt_alpha, false, cc_features.opt_alpha);
        }
        fs += ";\n";

        if (c == 0) {
            fs += "    texel = WRAP(texel, -1.01, 1.01);\n";
        }
    }

    fs += "    texel = WRAP(texel, -0.51, 1.51);\n";
    fs += "    texel = clamp(texel, 0.0, 1.0);\n";
    if (cc_features.opt_fog) {
        if (cc_features.opt_fog_linear) {
            fs += "    float fogFactor = clamp(vFogZW.y * vFog.a + vFogOffset, 0.0, 255.0) / 255.0;\n";
        } else {
            fs += "    float fogW = (abs(vFogZW.y) < 0.0001) ? 0.0001 : vFogZW.y;\n"
                  "    float fogFactor = clamp((vFogZW.x / fogW) * vFog.a + vFogOffset, 0.0, 255.0) / 255.0;\n";
        }
        if (cc_features.opt_fog_fade) {
            fs += "    texel = vec4(texel.rgb * (1.0 - fogFactor), texel.a);\n";
        } else if (cc_features.opt_alpha) {
            fs += "    texel = vec4(mix(texel.rgb, vFog.rgb, fogFactor), texel.a);\n";
        } else {
            fs += "    texel = mix(texel, vFog.rgb, fogFactor);\n";
        }
    }

    if (cc_features.opt_texture_edge && cc_features.opt_alpha) {
        fs += "    if (texel.a > 0.19) texel.a = 1.0; else discard;\n";
    }

    if (cc_features.opt_alpha && cc_features.opt_noise) {
        fs += "    texel.a *= floor(clamp(random(vec3(floor(gl_FragCoord.xy * noise_scale), float(frame_count))) + "
              "texel.a, 0.0, 1.0));\n";
    }

    if (cc_features.opt_grayscale) {
        fs += "    float intensity = (texel.r + texel.g + texel.b) / 3.0;\n"
              "    vec3 new_texel = vGrayscaleColor.rgb * intensity;\n"
              "    texel.rgb = mix(texel.rgb, new_texel, vGrayscaleColor.a);\n";
    }

    if (cc_features.opt_alpha) {
        if (cc_features.opt_alpha_threshold) {
            fs += "    if (texel.a < 8.0 / 256.0) discard;\n";
        }
        if (cc_features.opt_invisible) {
            fs += "    texel.a = 0.0;\n";
        }
        fs += "    outColor = texel;\n";
    } else {
        fs += "    outColor = vec4(texel, 1.0);\n";
    }
    fs += "}\n";

    std::string err;
    char name[64];
    snprintf(name, sizeof(name), "cc_%016llx_%08x", (unsigned long long)shader_id0, shader_id1);

    VkShaderModule vsm = vk_compile(vs, false, name, &err);
    if (!vsm) {
        sysLogPrintf(LOG_ERROR, "Failed to compile this vertex shader (ID %llx, %x):\n%s",
                     (unsigned long long)shader_id0, shader_id1, vs.c_str());
        sysFatalError("Vertex shader compilation failed:\n%s", err.c_str());
    }
    VkShaderModule fsm = vk_compile(fs, true, name, &err);
    if (!fsm) {
        sysLogPrintf(LOG_ERROR, "Failed to compile this fragment shader (ID %llx, %x):\n%s",
                     (unsigned long long)shader_id0, shader_id1, fs.c_str());
        sysFatalError("Fragment shader compilation failed:\n%s", err.c_str());
    }

    VkProgram *prg = &vk_programs[make_pair(shader_id0, shader_id1)];
    prg->vs = vsm;
    prg->fs = fsm;
    prg->num_inputs = cc_features.num_inputs;
    prg->used_textures[0] = cc_features.used_textures[0];
    prg->used_textures[1] = cc_features.used_textures[1];
    prg->num_attribs = (uint8_t)attrs.size();
    prg->num_floats = 0;
    for (size_t k = 0; k < attrs.size(); k++) {
        prg->attrib_sizes[k] = (uint8_t)attrs[k].second;
        prg->num_floats += (uint8_t)attrs[k].second;
    }

    vk_cur_prg = prg;
    return (struct ShaderProgram *)prg;
}

static struct ShaderProgram *gfx_vk_lookup_shader(uint64_t shader_id0, uint32_t shader_id1) {
    auto it = vk_programs.find(make_pair(shader_id0, shader_id1));
    return it == vk_programs.end() ? nullptr : (struct ShaderProgram *)&it->second;
}

static void gfx_vk_shader_get_info(struct ShaderProgram *sp, uint8_t *num_inputs, bool used_textures[2]) {
    const VkProgram *prg = (const VkProgram *)sp;
    *num_inputs = prg->num_inputs;
    used_textures[0] = prg->used_textures[0];
    used_textures[1] = prg->used_textures[1];
}

static void gfx_vk_unload_shader(struct ShaderProgram *old_prg) {
}

static void gfx_vk_load_shader(struct ShaderProgram *new_prg) {
    vk_cur_prg = (VkProgram *)new_prg;
}

static void gfx_vk_clear_shaders(void) {
    for (auto &p : vk_programs) {
        for (auto &pl : p.second.pipelines) {
            const VkPipeline pipe = pl.second;
            vk_defer([=]() { vkDestroyPipeline(vk_dev, pipe, NULL); });
        }
        const VkShaderModule vs = p.second.vs, fs = p.second.fs;
        vk_defer([=]() {
            vkDestroyShaderModule(vk_dev, vs, NULL);
            vkDestroyShaderModule(vk_dev, fs, NULL);
        });
    }
    vk_programs.clear();
    vk_cur_prg = nullptr;
    vk_bound_pipeline = VK_NULL_HANDLE;
}

/*
 * Pipelines
 */

static uint32_t vk_samples_log2(uint32_t samples) {
    uint32_t l = 0;
    while ((1u << (l + 1)) <= samples) {
        l++;
    }
    return l;
}

static VkPipeline vk_create_pipeline(VkShaderModule vsm, VkShaderModule fsm, const VkProgram *prg, int blend,
                                     bool depth_test, bool depth_write, VkCompareOp op, uint32_t samples,
                                     bool has_depth) {
    VkPipelineShaderStageCreateInfo stages[2] = {};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vsm;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fsm;
    stages[1].pName = "main";

    static const VkFormat float_formats[5] = { VK_FORMAT_UNDEFINED, VK_FORMAT_R32_SFLOAT, VK_FORMAT_R32G32_SFLOAT,
                                               VK_FORMAT_R32G32B32_SFLOAT, VK_FORMAT_R32G32B32A32_SFLOAT };
    VkVertexInputBindingDescription binding = {};
    VkVertexInputAttributeDescription attrs[16];
    VkPipelineVertexInputStateCreateInfo vi = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    if (prg) {
        uint32_t off = 0;
        for (uint32_t k = 0; k < prg->num_attribs; k++) {
            attrs[k].location = k;
            attrs[k].binding = 0;
            attrs[k].format = float_formats[prg->attrib_sizes[k]];
            attrs[k].offset = off * sizeof(float);
            off += prg->attrib_sizes[k];
        }
        binding.binding = 0;
        binding.stride = prg->num_floats * sizeof(float);
        binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
        vi.vertexBindingDescriptionCount = 1;
        vi.pVertexBindingDescriptions = &binding;
        vi.vertexAttributeDescriptionCount = prg->num_attribs;
        vi.pVertexAttributeDescriptions = attrs;
    }

    VkPipelineInputAssemblyStateCreateInfo ia = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo vp = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
    vp.viewportCount = 1;
    vp.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo rs = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
    rs.depthClampEnable = (prg && vk_have_depth_clamp) ? VK_TRUE : VK_FALSE;
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    // gfx_pc.cpp culls on the CPU, as the RSP did
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.depthBiasEnable = prg ? VK_TRUE : VK_FALSE;
    rs.lineWidth = 1.f;

    VkPipelineMultisampleStateCreateInfo ms = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
    ms.rasterizationSamples = (VkSampleCountFlagBits)samples;

    VkPipelineDepthStencilStateCreateInfo ds = { VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
    ds.depthTestEnable = (has_depth && depth_test) ? VK_TRUE : VK_FALSE;
    ds.depthWriteEnable = (has_depth && depth_test && depth_write) ? VK_TRUE : VK_FALSE;
    ds.depthCompareOp = op;

    VkPipelineColorBlendAttachmentState ba = {};
    ba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT |
                        VK_COLOR_COMPONENT_A_BIT;
    if (blend) {
        ba.blendEnable = VK_TRUE;
        switch (blend) {
            case 2: // modulate
                ba.srcColorBlendFactor = ba.srcAlphaBlendFactor = VK_BLEND_FACTOR_DST_COLOR;
                ba.dstColorBlendFactor = ba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
                break;
            case 3: // additive
                ba.srcColorBlendFactor = ba.srcAlphaBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
                ba.dstColorBlendFactor = ba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
                break;
            default:
                ba.srcColorBlendFactor = ba.srcAlphaBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
                ba.dstColorBlendFactor = ba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
                break;
        }
        ba.colorBlendOp = ba.alphaBlendOp = VK_BLEND_OP_ADD;
    }
    VkPipelineColorBlendStateCreateInfo cb = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
    cb.attachmentCount = 1;
    cb.pAttachments = &ba;

    const VkDynamicState dyn[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_DEPTH_BIAS };
    VkPipelineDynamicStateCreateInfo dy = { VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
    dy.dynamicStateCount = prg ? 3 : 2;
    dy.pDynamicStates = dyn;

    const VkFormat color_format = VK_COLOR_FORMAT;
    VkPipelineRenderingCreateInfo ri = { VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO };
    ri.colorAttachmentCount = 1;
    ri.pColorAttachmentFormats = &color_format;
    ri.depthAttachmentFormat = has_depth ? vk_depth_format : VK_FORMAT_UNDEFINED;
    ri.stencilAttachmentFormat = (has_depth && vk_depth_has_stencil) ? vk_depth_format : VK_FORMAT_UNDEFINED;

    VkGraphicsPipelineCreateInfo pi = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
    pi.pNext = &ri;
    pi.stageCount = 2;
    pi.pStages = stages;
    pi.pVertexInputState = &vi;
    pi.pInputAssemblyState = &ia;
    pi.pViewportState = &vp;
    pi.pRasterizationState = &rs;
    pi.pMultisampleState = &ms;
    pi.pDepthStencilState = &ds;
    pi.pColorBlendState = &cb;
    pi.pDynamicState = &dy;
    pi.layout = vk_pipeline_layout;

    VkPipeline p = VK_NULL_HANDLE;
    if (vkCreateGraphicsPipelines(vk_dev, vk_pipeline_cache, 1, &pi, NULL, &p) != VK_SUCCESS) {
        return VK_NULL_HANDLE;
    }
    return p;
}

static VkPipeline vk_get_pipeline(void) {
    const VkFb &fb = vk_fbs[vk_rendering_fb];
    const uint32_t samples = fb.msaa > 1 ? fb.msaa : 1;
    const bool test = vk_depth_test && fb.has_depth;
    const uint32_t key = (uint32_t)vk_blend | (test ? 4 : 0) | ((test && vk_depth_write) ? 8 : 0) |
                         (test ? ((uint32_t)vk_depth_op << 4) : 0) | (vk_samples_log2(samples) << 8) |
                         (fb.has_depth ? 0x1000 : 0);

    for (const auto &e : vk_cur_prg->pipelines) {
        if (e.first == key) {
            return e.second;
        }
    }

    const double t0 = vk_now_ms();
    VkPipeline p = vk_create_pipeline(vk_cur_prg->vs, vk_cur_prg->fs, vk_cur_prg, vk_blend, test, vk_depth_write,
                                      vk_depth_op, samples, fb.has_depth);
    vk_stat_pipelines++;
    vk_stat_pipeline_ms += vk_now_ms() - t0;
    vk_cache_dirty = true;
    vk_cache_idle_frames = 0;
    if (!p) {
        sysFatalError("Vulkan: could not create a pipeline for a combiner.");
    }
    vk_cur_prg->pipelines.push_back({ key, p });
    return p;
}

/*
 * Rendering into a framebuffer
 */

static void vk_apply_dynamic_state(VkCommandBuffer cb) {
    const VkFb &fb = vk_fbs[vk_rendering_fb];

    VkViewport v;
    v.x = (float)vk_viewport[0];
    v.y = (float)vk_viewport[1];
    v.width = (float)std::max(vk_viewport[2], 1);
    v.height = (float)std::max(vk_viewport[3], 1);
    v.minDepth = vk_depth_near;
    v.maxDepth = vk_depth_far;
    rcCmdSetViewport(cb, 0, 1, &v);

    int x = vk_scissor[0], y = vk_scissor[1], w = vk_scissor[2], h = vk_scissor[3];
    if (x < 0) {
        w += x;
        x = 0;
    }
    if (y < 0) {
        h += y;
        y = 0;
    }
    w = std::max(0, std::min(w, (int)fb.width - x));
    h = std::max(0, std::min(h, (int)fb.height - y));
    VkRect2D s = { { x, y }, { (uint32_t)w, (uint32_t)h } };
    rcCmdSetScissor(cb, 0, 1, &s);

    rcCmdSetDepthBias(cb, vk_bias_const, 0.f, vk_bias_slope);
}

static void vk_begin_rendering_on(VkImg *color, VkImg *depth, uint32_t w, uint32_t h) {
    VkCommandBuffer cb = vk_cmd();

    vk_image_to(*color, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    if (depth) {
        vk_image_to(*depth, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
    }

    VkRenderingAttachmentInfo ca = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
    ca.imageView = color->view;
    ca.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    ca.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    ca.storeOp = VK_ATTACHMENT_STORE_OP_STORE;

    VkRenderingAttachmentInfo da = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
    if (depth) {
        da.imageView = depth->view;
        da.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        da.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        da.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    }

    VkRenderingInfo ri = { VK_STRUCTURE_TYPE_RENDERING_INFO };
    ri.renderArea = { { 0, 0 }, { w, h } };
    ri.layerCount = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &ca;
    ri.pDepthAttachment = depth ? &da : NULL;
    ri.pStencilAttachment = (depth && vk_depth_has_stencil) ? &da : NULL;

    rcCmdBeginRendering(cb, &ri);
    vk_rendering = true;
}

static void vk_begin_fb_rendering(void) {
    VkFb &fb = vk_fbs[vk_cur_fb];
    VkImg *color = fb.msaa > 1 ? &fb.color_msaa : &fb.color[fb.cur];
    VkImg *depth = fb.has_depth ? &fb.depth : NULL;

    if (!color->image) {
        return;
    }

    vk_begin_rendering_on(color, depth, fb.width, fb.height);
    vk_rendering_fb = vk_cur_fb;
    vk_apply_dynamic_state(VK_MAIN_CB);
}

/*
 * The GfxRenderingAPI
 */

static const char *gfx_vk_get_name(void) {
    return "Vulkan";
}

static int gfx_vk_get_max_texture_size(void) {
    if (vk_failed || !vk_dev) {
        return 8192;
    }
    return (int)vk_props.limits.maxImageDimension2D;
}

static struct GfxClipParameters gfx_vk_get_clip_parameters(void) {
    if (vk_cur_fb < 0 || (size_t)vk_cur_fb >= vk_fbs.size()) {
        return { false, false };
    }
    return { false, vk_fbs[vk_cur_fb].invert_y };
}

static uint32_t gfx_vk_new_texture(void) {
    uint32_t id;
    if (!vk_free_texture_ids.empty()) {
        id = vk_free_texture_ids.back();
        vk_free_texture_ids.pop_back();
    } else {
        id = (uint32_t)vk_textures.size();
        vk_textures.push_back(VkTex());
    }
    vk_textures[id] = VkTex();
    vk_textures[id].live = true;
    vk_textures[id].sampler_key = vk_sampler_key(false, true, 0, VK_WRAP_REPEAT, VK_WRAP_REPEAT, 1);
    vk_textures[id].sampler = vk_get_sampler(vk_textures[id].sampler_key);
    return id;
}

static void gfx_vk_delete_texture(uint32_t id) {
    if (id == 0 || id >= vk_textures.size() || !vk_textures[id].live) {
        return;
    }
    vk_image_destroy(vk_textures[id].img);
    vk_textures[id].live = false;
    vk_free_texture_ids.push_back(id);
    for (int t = 0; t < 2; t++) {
        if (vk_bound[t].kind == VK_BIND_TEXTURE && vk_bound[t].id == id) {
            vk_bound[t] = VkBinding();
        }
    }
}

static void gfx_vk_select_texture(int tile, uint32_t texture_id, bool linear_filter) {
    vk_active_tile = tile;
    vk_bound[tile].kind = VK_BIND_TEXTURE;
    vk_bound[tile].id = texture_id;
    vk_textures_linear[tile] = linear_filter;
}

static void gfx_vk_upload_texture(const uint8_t *rgba32_buf, uint32_t width, uint32_t height, bool gen_mipmaps) {
    const VkBinding b = vk_bound[vk_active_tile];
    if (b.kind != VK_BIND_TEXTURE || b.id == 0 || b.id >= vk_textures.size() || width == 0 || height == 0) {
        return;
    }

    vk_ensure_recording();
    VkSlot &sl = vk_slots[vk_slot];

    uint32_t mips = 1;
    if (gen_mipmaps || vk_filter_mode == FILTER_THREE_POINT) {
        while ((std::max(width, height) >> mips) > 0) {
            mips++;
        }
    }

    // A fresh image every time: the old one may be drawn from earlier in this
    // frame, and the upload goes to the GPU ahead of those draws.
    VkTex &tex = vk_textures[b.id];
    vk_image_destroy(tex.img);
    if (!vk_image_create(tex.img, width, height, VK_TEXTURE_FORMAT,
                         VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                         VK_SAMPLE_COUNT_1_BIT, mips, false)) {
        sysLogPrintf(LOG_WARNING, "Vulkan: could not create a %ux%u texture", width, height);
        return;
    }

    const VkDeviceSize bytes = (VkDeviceSize)width * height * 4;
    VkBuffer sbuf;
    VkDeviceSize soff;
    uint8_t *sptr;
    if (!vk_ring_alloc(sl.staging, bytes, VK_STAGING_CHUNK, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, &sbuf, &soff, &sptr)) {
        sysLogPrintf(LOG_WARNING, "Vulkan: out of staging memory for a %ux%u texture", width, height);
        vk_image_destroy(tex.img);
        return;
    }
    memcpy(sptr, rgba32_buf, bytes);

    VkCommandBuffer cb = sl.upload;
    sl.upload_used = true;
    VkImg &img = tex.img;

    vk_barrier(cb, img.image, img.aspect, 0, mips, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

    VkBufferImageCopy copy = {};
    copy.bufferOffset = soff;
    copy.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    copy.imageExtent = { width, height, 1 };
    rcCmdCopyBufferToImage(cb, sbuf, img.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);

    int32_t mw = (int32_t)width, mh = (int32_t)height;
    for (uint32_t i = 1; i < mips; i++) {
        vk_barrier(cb, img.image, img.aspect, i - 1, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                   VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        VkImageBlit blit = {};
        blit.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, i - 1, 0, 1 };
        blit.srcOffsets[1] = { mw, mh, 1 };
        mw = std::max(1, mw / 2);
        mh = std::max(1, mh / 2);
        blit.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, i, 0, 1 };
        blit.dstOffsets[1] = { mw, mh, 1 };
        rcCmdBlitImage(cb, img.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, img.image,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);
    }

    if (mips > 1) {
        vk_barrier(cb, img.image, img.aspect, 0, mips - 1, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }
    vk_barrier(cb, img.image, img.aspect, mips - 1, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
               VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    img.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
}

static int vk_wrap_from_cm(uint32_t val) {
    switch (val) {
        case G_TX_NOMIRROR | G_TX_CLAMP:
            // Stretched Edges; see the OpenGL renderer
            return gfx_clamped_edge_mode == CLAMPED_EDGE_MIRROR   ? VK_WRAP_MIRROR
                   : gfx_clamped_edge_mode == CLAMPED_EDGE_REPEAT ? VK_WRAP_REPEAT
                                                                  : VK_WRAP_CLAMP;
        case G_TX_MIRROR | G_TX_WRAP:
            return VK_WRAP_MIRROR;
        case G_TX_MIRROR | G_TX_CLAMP:
            return VK_WRAP_MIRROR_CLAMP;
        default:
            return VK_WRAP_REPEAT;
    }
}

static void gfx_vk_set_sampler_parameters(int tile, bool linear_filter, uint32_t cms, uint32_t cmt, bool mipmaps) {
    vk_active_tile = tile;

    // OpenGL's min filter table, and its mag filter: see the OpenGL renderer
    mipmaps = mipmaps && vk_mipmap_mode != MIPMAP_DISABLED;
    bool min_linear = false, mag_linear = false;
    int mipmode = 0;
    if (linear_filter && vk_filter_mode != FILTER_THREE_POINT) {
        min_linear = vk_filter_mode == FILTER_LINEAR;
        mipmode = mipmaps ? (vk_mipmap_mode == MIPMAP_LINEAR ? 2 : 1) : 0;
    }
    mag_linear = linear_filter && vk_filter_mode == FILTER_LINEAR;

    const uint32_t key =
        vk_sampler_key(min_linear, mag_linear, mipmode, vk_wrap_from_cm(cms), vk_wrap_from_cm(cmt), mipmaps ? vk_anisotropy : 1);

    const VkBinding b = vk_bound[tile];
    if (b.kind == VK_BIND_TEXTURE && b.id < vk_textures.size()) {
        vk_textures[b.id].sampler_key = key;
        vk_textures[b.id].sampler = vk_get_sampler(key);
    } else if (b.kind == VK_BIND_FB && b.id < vk_fbs.size()) {
        vk_fbs[b.id].sampler_key = key;
        vk_fbs[b.id].sampler = vk_get_sampler(key);
    }
}

// depth_bias: see the OpenGL renderer
static void gfx_vk_set_depth_mode(bool depth_test, bool depth_update, bool depth_compare, bool depth_source_prim,
                                  uint16_t zmode, int16_t depth_bias) {
    float slope = vk_bias_slope, units = vk_bias_const;

    if (depth_test) {
        vk_depth_test = true;
        vk_depth_write = depth_update;

        if (depth_compare) {
            switch (zmode) {
                case ZMODE_INTER:
                    vk_depth_op = VK_COMPARE_OP_LESS_OR_EQUAL;
                    slope = 0;
                    units = depth_bias;
                    break;
                case ZMODE_OPA:
                case ZMODE_XLU:
                    vk_depth_op = depth_source_prim ? VK_COMPARE_OP_LESS_OR_EQUAL : VK_COMPARE_OP_LESS;
                    slope = 0;
                    units = depth_bias;
                    break;
                case ZMODE_DEC:
                    vk_depth_op = VK_COMPARE_OP_LESS_OR_EQUAL;
                    slope = -2;
                    units = -2 + depth_bias;
                    break;
            }
        } else {
            vk_depth_op = VK_COMPARE_OP_ALWAYS;
            slope = 0;
            units = 0;
        }
    } else {
        vk_depth_test = false;
    }

    if (slope != vk_bias_slope || units != vk_bias_const) {
        vk_bias_slope = slope;
        vk_bias_const = units;
        if (vk_rendering) {
            rcCmdSetDepthBias(VK_MAIN_CB, vk_bias_const, 0.f, vk_bias_slope);
        }
    }
}

static void gfx_vk_set_depth_range(float znear, float zfar) {
    vk_depth_near = znear;
    vk_depth_far = zfar;
    if (vk_rendering) {
        vk_apply_dynamic_state(VK_MAIN_CB);
    }
}

static void gfx_vk_set_viewport(int x, int y, int width, int height) {
    vk_viewport[0] = x;
    vk_viewport[1] = y;
    vk_viewport[2] = width;
    vk_viewport[3] = height;
    if (vk_rendering) {
        vk_apply_dynamic_state(VK_MAIN_CB);
    }
}

static void gfx_vk_set_scissor(int x, int y, int width, int height) {
    vk_scissor[0] = x;
    vk_scissor[1] = y;
    vk_scissor[2] = width;
    vk_scissor[3] = height;
    if (vk_rendering) {
        vk_apply_dynamic_state(VK_MAIN_CB);
    }
}

static void gfx_vk_set_use_alpha(bool use_alpha, bool modulate, bool additive) {
    vk_blend = !use_alpha ? 0 : modulate ? 2 : additive ? 3 : 1;
}

static VkImg vk_dummy;

// Which image and sampler each of the two texture units reads, moving a
// framebuffer that is about to be sampled into the layout for it.
static void vk_resolve_textures(VkPush &push) {
    for (int t = 0; t < 2; t++) {
        push.tex[t] = 0;
        push.sampler[t] = 0;

        if (!vk_cur_prg->used_textures[t]) {
            continue;
        }

        const VkBinding b = vk_bound[t];
        if (b.kind == VK_BIND_TEXTURE && b.id < vk_textures.size()) {
            const VkTex &tex = vk_textures[b.id];
            if (tex.img.slot >= 0) {
                push.tex[t] = tex.img.slot;
                push.sampler[t] = tex.sampler;
            }
        } else if (b.kind == VK_BIND_FB && b.id < vk_fbs.size() && (int)b.id != vk_cur_fb) {
            VkFb &fb = vk_fbs[b.id];
            VkImg &img = fb.color[fb.cur];
            if (img.slot >= 0) {
                vk_image_to(img, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
                push.tex[t] = img.slot;
                push.sampler[t] = fb.sampler;
            }
        }
    }
}

static void gfx_vk_draw_triangles(float buf_vbo[], size_t buf_vbo_len, size_t buf_vbo_num_tris) {
    if (!vk_cur_prg || !buf_vbo_len || vk_cur_fb < 0 || (size_t)vk_cur_fb >= vk_fbs.size()) {
        return;
    }

    vk_ensure_recording();

    VkPush push;
    push.frame_count = (int32_t)vk_frame_count;
    push.noise_scale = vk_noise_scale;
    push.three_point_filter0 = vk_textures_linear[0];
    push.three_point_filter1 = vk_textures_linear[1];
    vk_resolve_textures(push);

    if (!vk_rendering) {
        vk_begin_fb_rendering();
        if (!vk_rendering) {
            return;
        }
    }

    VkSlot &sl = vk_slots[vk_slot];
    VkCommandBuffer cb = VK_MAIN_CB;

    VkPipeline p = vk_get_pipeline();
    if (p != vk_bound_pipeline) {
        rcCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, p);
        vk_bound_pipeline = p;
    }

    if (!vk_push_valid || memcmp(&push, &vk_pushed, sizeof(push)) != 0) {
        rcCmdPushConstants(cb, vk_pipeline_layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), &push);
        vk_pushed = push;
        vk_push_valid = true;
    }

    const VkDeviceSize bytes = buf_vbo_len * sizeof(float);
    const uint32_t stride = vk_cur_prg->num_floats * sizeof(float);
    VkBuffer vbuf;
    uint32_t first;
    uint8_t *vptr;
    if (!vk_vertex_alloc(sl.vertex, bytes, stride, &vbuf, &first, &vptr)) {
        return;
    }
    memcpy(vptr, buf_vbo, bytes);
    if (vbuf != vk_bound_vbuf) {
        const VkDeviceSize zero = 0;
        rcCmdBindVertexBuffers(cb, 0, 1, &vbuf, &zero);
        vk_bound_vbuf = vbuf;
    }
    rcCmdDraw(cb, (uint32_t)(3 * buf_vbo_num_tris), 1, first, 0);
}

/*
 * The swapchain
 */

static VkPresentModeKHR vk_pick_present_mode(void) {
    auto have = [](VkPresentModeKHR m) {
        return std::find(vk_present_modes.begin(), vk_present_modes.end(), m) != vk_present_modes.end();
    };

    if (vk_swap_interval == 0) {
        if (have(VK_PRESENT_MODE_IMMEDIATE_KHR)) {
            return VK_PRESENT_MODE_IMMEDIATE_KHR;
        }
        if (have(VK_PRESENT_MODE_MAILBOX_KHR)) {
            return VK_PRESENT_MODE_MAILBOX_KHR;
        }
    } else if (vk_swap_interval < 0 && have(VK_PRESENT_MODE_FIFO_RELAXED_KHR)) {
        return VK_PRESENT_MODE_FIFO_RELAXED_KHR;
    }
    return VK_PRESENT_MODE_FIFO_KHR;
}

static void vk_destroy_swapchain_semaphores(void) {
    for (VkSemaphore s : vk_swap_done) {
        vkDestroySemaphore(vk_dev, s, NULL);
    }
    vk_swap_done.clear();
}

static bool vk_create_swapchain(void) {
    vk_drain();
    vkDeviceWaitIdle(vk_dev);
    vk_swap_dirty_async = false;
    vk_completed = vk_submitted;

    VkSurfaceCapabilitiesKHR caps;
    if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(vk_phys, vk_surface, &caps) != VK_SUCCESS) {
        return false;
    }

    int w = 0, h = 0;
    SDL_Vulkan_GetDrawableSize(gfx_sdl_window(), &w, &h);
    vk_swap_drawable_w = w;
    vk_swap_drawable_h = h;

    VkExtent2D extent = caps.currentExtent;
    if (extent.width == 0xFFFFFFFF) {
        extent.width = std::max(caps.minImageExtent.width, std::min(caps.maxImageExtent.width, (uint32_t)w));
        extent.height = std::max(caps.minImageExtent.height, std::min(caps.maxImageExtent.height, (uint32_t)h));
    }

    vk_destroy_swapchain_semaphores();

    if (extent.width == 0 || extent.height == 0) {
        // minimised: nothing to present into until it comes back
        if (vk_swapchain) {
            vkDestroySwapchainKHR(vk_dev, vk_swapchain, NULL);
            vk_swapchain = VK_NULL_HANDLE;
        }
        vk_swap_images.clear();
        vk_swap_extent = extent;
        vk_swap_dirty = false;
        return true;
    }

    uint32_t count = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(vk_phys, vk_surface, &count, NULL);
    vk_present_modes.resize(count);
    vkGetPhysicalDeviceSurfacePresentModesKHR(vk_phys, vk_surface, &count, vk_present_modes.data());

    uint32_t images = std::max(caps.minImageCount + 1, 2u);
    if (caps.maxImageCount && images > caps.maxImageCount) {
        images = caps.maxImageCount;
    }

    VkSwapchainCreateInfoKHR ci = { VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR };
    ci.surface = vk_surface;
    ci.minImageCount = images;
    ci.imageFormat = vk_swap_format;
    ci.imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    ci.imageExtent = extent;
    ci.imageArrayLayers = 1;
    ci.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.preTransform = (caps.supportedTransforms & VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR)
                          ? VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR
                          : caps.currentTransform;
    ci.compositeAlpha = (caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR)
                            ? VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR
                            : (VkCompositeAlphaFlagBitsKHR)(caps.supportedCompositeAlpha & -caps.supportedCompositeAlpha);
    ci.presentMode = vk_pick_present_mode();
    ci.clipped = VK_TRUE;
    ci.oldSwapchain = vk_swapchain;

    VkSwapchainKHR sc = VK_NULL_HANDLE;
    const VkResult r = vkCreateSwapchainKHR(vk_dev, &ci, NULL, &sc);
    if (vk_swapchain) {
        vkDestroySwapchainKHR(vk_dev, vk_swapchain, NULL);
        vk_swapchain = VK_NULL_HANDLE;
    }
    if (r != VK_SUCCESS) {
        vk_fail("could not create the swapchain (%d)", (int)r);
        vk_swap_images.clear();
        return false;
    }
    vk_swapchain = sc;
    vk_swap_extent = extent;

    count = 0;
    vkGetSwapchainImagesKHR(vk_dev, vk_swapchain, &count, NULL);
    vk_swap_images.resize(count);
    vkGetSwapchainImagesKHR(vk_dev, vk_swapchain, &count, vk_swap_images.data());

    vk_swap_done.resize(count);
    for (uint32_t i = 0; i < count; i++) {
        VkSemaphoreCreateInfo si = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
        vkCreateSemaphore(vk_dev, &si, NULL, &vk_swap_done[i]);
    }

    vk_swap_dirty = false;
    return true;
}

// Called by gfx_sdl2.cpp where OpenGL would swap buffers
static void vk_present(void) {
    if (vk_failed || !vk_dev) {
        return;
    }

    vk_ensure_recording();
    vk_end_rendering();

    int dw = 0, dh = 0;
    SDL_Vulkan_GetDrawableSize(gfx_sdl_window(), &dw, &dh);
    if (vk_swap_dirty || vk_swap_dirty_async || dw != vk_swap_drawable_w || dh != vk_swap_drawable_h) {
        vk_create_swapchain();
    }

    VkFb &fb0 = vk_fbs[0];
    VkImg &src = fb0.color[fb0.cur];
    if (vk_swapchain && src.image && !vk_no_present) {
        vk_image_to(src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        vk_handoff(true, src.image, src.width, src.height);
    } else {
        vk_handoff(false, VK_NULL_HANDLE, 0, 0);
    }

    vk_slot = (vk_slot + 1) % VK_FRAMES;
    // What was just shown is the front buffer now
    fb0.cur ^= 1;
}

static int vk_get_swap_interval(void) {
    return vk_swap_interval;
}

static bool vk_set_swap_interval(int interval) {
    if (interval != vk_swap_interval) {
        vk_swap_interval = interval;
        vk_swap_dirty = true;
    }
    return true;
}

/*
 * Framebuffers
 */

static int gfx_vk_create_framebuffer(void) {
    const size_t i = vk_fbs.size();
    vk_fbs.resize(i + 1);
    vk_fbs[i].sampler_key = vk_sampler_key(true, true, 0, VK_WRAP_REPEAT, VK_WRAP_REPEAT, 1);
    vk_fbs[i].sampler = vk_get_sampler(vk_fbs[i].sampler_key);
    return (int)i;
}

static int gfx_vk_get_max_msaa_level(void) {
    return (int)vk_max_msaa;
}

static void gfx_vk_update_framebuffer_parameters(int fb_id, uint32_t width, uint32_t height, uint32_t msaa_level,
                                                 bool opengl_invert_y, bool render_target, bool has_depth_buffer,
                                                 bool can_extract_depth) {
    if (vk_failed || !vk_dev || fb_id < 0 || (size_t)fb_id >= vk_fbs.size()) {
        return;
    }

    VkFb &fb = vk_fbs[fb_id];
    width = std::max(width, 1u);
    height = std::max(height, 1u);
    msaa_level = std::max(1u, std::min(msaa_level, vk_max_msaa));
    while (msaa_level > 1 && !(vk_sample_counts & msaa_level)) {
        msaa_level /= 2;
    }
    if (fb_id == 0) {
        // the window: never multisampled, always a depth buffer, as OpenGL's
        msaa_level = 1;
        has_depth_buffer = true;
    }
    if (!gfx_framebuffers_enabled && fb_id != 0) {
        has_depth_buffer = false;
    }

    const bool resize = fb.width != width || fb.height != height || fb.msaa != msaa_level || !fb.color[0].image;
    const bool add_depth = has_depth_buffer && (!fb.depth.image || resize);

    if (resize || add_depth || (!has_depth_buffer && fb.depth.image)) {
        if (vk_rendering && vk_rendering_fb == fb_id) {
            vk_end_rendering();
        }
    }

    if (resize) {
        const VkImageUsageFlags usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                                        VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        for (int c = 0; c < 2; c++) {
            vk_image_destroy(fb.color[c]);
            if (c == 0 || fb_id == 0) {
                if (!vk_image_create(fb.color[c], width, height, VK_COLOR_FORMAT, usage, VK_SAMPLE_COUNT_1_BIT, 1, true)) {
                    sysFatalError("Vulkan: could not create a %ux%u framebuffer.", width, height);
                }
                fb.color[c].clear_first = true;
            }
        }
        vk_image_destroy(fb.color_msaa);
        if (msaa_level > 1) {
            if (!vk_image_create(fb.color_msaa, width, height, VK_COLOR_FORMAT,
                                 VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                     VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                                 (VkSampleCountFlagBits)msaa_level, 1, false)) {
                sysLogPrintf(LOG_WARNING, "Vulkan: %ux MSAA framebuffer could not be made, MSAA off", msaa_level);
                msaa_level = 1;
                gfx_msaa_level = 1;
            } else {
                fb.color_msaa.clear_first = true;
            }
        }
        fb.cur = 0;
    }

    if (has_depth_buffer && (add_depth || resize)) {
        vk_image_destroy(fb.depth);
        if (!vk_image_create(fb.depth, width, height, vk_depth_format, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
                             VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                             (VkSampleCountFlagBits)msaa_level, 1, false)) {
            sysFatalError("Vulkan: could not create a %ux%u depth buffer.", width, height);
        }
        fb.depth.clear_first = true;
    } else if (!has_depth_buffer && fb.depth.image) {
        vk_image_destroy(fb.depth);
    }

    fb.width = width;
    fb.height = height;
    fb.msaa = msaa_level;
    fb.has_depth = has_depth_buffer;
    fb.invert_y = opengl_invert_y;
}

static bool gfx_vk_start_draw_to_framebuffer(int fb_id, float noise_scale) {
    if (fb_id < 0 || (size_t)fb_id >= vk_fbs.size()) {
        return false;
    }
    if (!gfx_framebuffers_enabled && fb_id != 0) {
        return false;
    }

    if (noise_scale != 0.0f) {
        vk_noise_scale = 1.0f / noise_scale;
    }
    if (fb_id != vk_cur_fb) {
        vk_end_rendering();
        vk_cur_fb = fb_id;
    }
    return true;
}

static void gfx_vk_clear_framebuffer(bool clear_color, bool clear_depth) {
    if (vk_cur_fb < 0 || (size_t)vk_cur_fb >= vk_fbs.size()) {
        return;
    }
    vk_ensure_recording();
    if (!vk_rendering) {
        vk_begin_fb_rendering();
        if (!vk_rendering) {
            return;
        }
    }

    const VkFb &fb = vk_fbs[vk_cur_fb];
    VkClearAttachment att[2];
    uint32_t n = 0;
    if (clear_color) {
        att[n] = {};
        att[n].aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        att[n].colorAttachment = 0;
        att[n].clearValue.color = { { 0.f, 0.f, 0.f, 1.f } };
        n++;
    }
    if (clear_depth && fb.has_depth) {
        att[n] = {};
        att[n].aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT | (vk_depth_has_stencil ? VK_IMAGE_ASPECT_STENCIL_BIT : 0);
        att[n].clearValue.depthStencil = { 1.f, 0 };
        n++;
    }
    if (!n) {
        return;
    }

    // The whole target, whatever the scissor: OpenGL's clear here turns the
    // scissor test off first
    VkClearRect rect = { { { 0, 0 }, { fb.width, fb.height } }, 0, 1 };
    rcCmdClearAttachments(VK_MAIN_CB, n, att, 1, &rect);
}

// A single-sampled image holding what the framebuffer shows, resolving it
// first if it was drawn multisampled.
static VkImg &vk_resolved_color(VkFb &fb, int which) {
    if (fb.msaa <= 1 || !fb.color_msaa.image) {
        return fb.color[which];
    }

    VkCommandBuffer cb = vk_cmd();
    vk_image_to(fb.color_msaa, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    vk_image_to(fb.color[0], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

    VkImageResolve res = {};
    res.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    res.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    res.extent = { fb.width, fb.height, 1 };
    rcCmdResolveImage(cb, fb.color_msaa.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, fb.color[0].image,
                      VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &res);
    return fb.color[0];
}

static void vk_blit(VkImg &src, VkImg &dst, int sx0, int sy0, int sx1, int sy1, int dx0, int dy0, int dx1, int dy1) {
    if (&src == &dst || !src.image || !dst.image) {
        return;
    }
    VkCommandBuffer cb = vk_cmd();
    vk_image_to(src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    vk_image_to(dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

    VkImageBlit blit = {};
    blit.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    blit.srcOffsets[0] = { sx0, sy0, 0 };
    blit.srcOffsets[1] = { sx1, sy1, 1 };
    blit.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    blit.dstOffsets[0] = { dx0, dy0, 0 };
    blit.dstOffsets[1] = { dx1, dy1, 1 };
    rcCmdBlitImage(cb, src.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                   1, &blit, VK_FILTER_NEAREST);
}

static void gfx_vk_resolve_msaa_color_buffer(int fb_id_target, int fb_id_source) {
    if (fb_id_target < 0 || (size_t)fb_id_target >= vk_fbs.size() || fb_id_source < 0 ||
        (size_t)fb_id_source >= vk_fbs.size() || fb_id_target == fb_id_source) {
        return;
    }
    vk_ensure_recording();
    vk_end_rendering();

    VkFb &src = vk_fbs[fb_id_source];
    VkFb &dst = vk_fbs[fb_id_target];
    VkImg &target = dst.color[dst.cur];

    if (src.msaa > 1 && src.color_msaa.image && src.width == target.width && src.height == target.height) {
        VkCommandBuffer cb = vk_cmd();
        vk_image_to(src.color_msaa, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        vk_image_to(target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkImageResolve res = {};
        res.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        res.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        res.extent = { src.width, src.height, 1 };
        rcCmdResolveImage(cb, src.color_msaa.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, target.image,
                          VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &res);
        return;
    }

    VkImg &from = vk_resolved_color(src, src.cur);
    vk_blit(from, target, 0, 0, (int)from.width, (int)from.height, 0, 0, (int)target.width, (int)target.height);
}

static void *gfx_vk_get_framebuffer_texture_id(int fb_id) {
    return (void *)(uintptr_t)fb_id;
}

static void gfx_vk_select_texture_fb(int fb_id) {
    // See the OpenGL renderer: an id from before a renderer re-init
    if (fb_id < 0 || (size_t)fb_id >= vk_fbs.size()) {
        return;
    }
    vk_active_tile = 0;
    vk_bound[0].kind = VK_BIND_FB;
    vk_bound[0].id = (uint32_t)fb_id;
    vk_textures_linear[0] = true;
}

static void gfx_vk_copy_framebuffer(int fb_dst, int fb_src, int left, int top, bool flip_y, bool use_back) {
    if (fb_dst < 0 || (size_t)fb_dst >= vk_fbs.size() || fb_src < 0 || (size_t)fb_src >= vk_fbs.size() ||
        fb_dst == fb_src) {
        return;
    }
    if (!gfx_framebuffers_enabled) {
        return;
    }

    vk_ensure_recording();
    vk_end_rendering();

    VkFb &src = vk_fbs[fb_src];
    VkFb &dst = vk_fbs[fb_dst];
    // The window's front buffer is the frame on screen, which is the other
    // window image
    VkImg &from = vk_resolved_color(src, fb_src == 0 && !use_back ? src.cur ^ 1 : src.cur);
    VkImg &to = dst.color[dst.cur];
    if (!from.image || !to.image) {
        return;
    }

    int sx0, sy0, sx1, sy1;
    int dx0 = 0, dy0 = 0, dx1 = (int)to.width, dy1 = (int)to.height;

    if (left >= 0 && top >= 0) {
        // unscaled rect copy, kept inside the source (OpenGL clips it)
        sx0 = left;
        sy0 = top;
        sx1 = left + (int)to.width;
        sy1 = top + (int)to.height;
        if (sx1 > (int)from.width) {
            dx1 -= sx1 - (int)from.width;
            sx1 = (int)from.width;
        }
        if (sy1 > (int)from.height) {
            dy1 -= sy1 - (int)from.height;
            sy1 = (int)from.height;
        }
        if (sx0 >= sx1 || sy0 >= sy1 || dx1 <= 0 || dy1 <= 0) {
            return;
        }
    } else {
        sx0 = 0;
        sy0 = 0;
        sx1 = (int)from.width;
        sy1 = (int)from.height;
    }

    if (flip_y) {
        std::swap(dy0, dy1);
    }

    vk_blit(from, to, sx0, sy0, sx1, sy1, dx0, dy0, dx1, dy1);
}

static void gfx_vk_set_texture_filter(enum FilteringMode mode) {
    vk_filter_mode = mode;
}

static enum FilteringMode gfx_vk_get_texture_filter(void) {
    return vk_filter_mode;
}

static void gfx_vk_set_mipmap_filter(enum MipmapFilteringMode mode) {
    vk_mipmap_mode = mode;
}

static void gfx_vk_set_anisotropy_level(int level) {
    vk_anisotropy = std::max(1, level);
}

static int gfx_vk_get_max_anisotropy_level(void) {
    if (vk_failed || !vk_dev || !vk_have_anisotropy) {
        return 1;
    }
    return (int)vk_props.limits.maxSamplerAnisotropy;
}

/*
 * Vivid Colours and Black Level; see the OpenGL renderer. The window image is
 * copied aside and drawn back over itself through the grading shader.
 */
static const char *vk_grade_vs =
    "#version 450\n"
    "layout(location = 0) out vec2 vUV;\n"
    "void main() {\n"
    "    vec2 p = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));\n"
    "    vUV = p;\n"
    "    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);\n"
    "}\n";

static const char *vk_grade_fs =
    "#version 450\n"
    "layout(set = 0, binding = 0) uniform texture2D uTextures[1];\n"
    "layout(set = 0, binding = 1) uniform sampler uSamplers[1];\n"
    "layout(push_constant) uniform Push { float uSaturation; float uContrast; float uBlack; float pad; int tex; int smp; };\n"
    "layout(location = 0) in vec2 vUV;\n"
    "layout(location = 0) out vec4 oCol;\n"
    "void main() {\n"
    "    vec3 c = texture(sampler2D(uTextures[tex], uSamplers[smp]), vUV).rgb;\n"
    "    c = max(c - uBlack, 0.0) / (1.0 - uBlack);\n"
    "    float l = dot(c, vec3(0.2126, 0.7152, 0.0722));\n"
    "    c = mix(vec3(l), c, uSaturation);\n"
    "    c = (c - 0.5) * uContrast + 0.5;\n"
    "    oCol = vec4(clamp(c, 0.0, 1.0), 1.0);\n"
    "}\n";

static bool vk_grade_init(void) {
    std::string err;
    VkShaderModule vs = vk_compile(vk_grade_vs, false, "grade", &err);
    VkShaderModule fs = vs ? vk_compile(vk_grade_fs, true, "grade", &err) : VK_NULL_HANDLE;
    if (vs && fs) {
        vk_grade_pipeline = vk_create_pipeline(vs, fs, NULL, 0, false, false, VK_COMPARE_OP_ALWAYS, 1, false);
    }
    if (!vk_grade_pipeline) {
        sysLogPrintf(LOG_WARNING, "Vulkan: colour grade shader failed: %s", err.c_str());
    }
    if (vs) {
        vkDestroyShaderModule(vk_dev, vs, NULL);
    }
    if (fs) {
        vkDestroyShaderModule(vk_dev, fs, NULL);
    }
    return vk_grade_pipeline != VK_NULL_HANDLE;
}

static void vk_grade_frame(void) {
    if (vk_grade_failed || vk_fbs.empty()) {
        return;
    }
    if (gfx_color_saturation == 1.0f && gfx_color_contrast == 1.0f && gfx_color_black_level == 0.0f) {
        return;
    }
    if (!vk_grade_pipeline && !vk_grade_init()) {
        vk_grade_failed = true;
        return;
    }

    VkFb &fb0 = vk_fbs[0];
    VkImg &win = fb0.color[fb0.cur];
    if (!win.image) {
        return;
    }

    if (vk_grade_image.width != win.width || vk_grade_image.height != win.height) {
        vk_image_destroy(vk_grade_image);
        if (!vk_image_create(vk_grade_image, win.width, win.height, VK_COLOR_FORMAT,
                             VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_SAMPLE_COUNT_1_BIT, 1,
                             false)) {
            vk_grade_failed = true;
            return;
        }
    }

    VkCommandBuffer cb = vk_cmd();
    vk_end_rendering();

    vk_image_to(win, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    vk_image_to(vk_grade_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkImageCopy copy = {};
    copy.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    copy.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    copy.extent = { win.width, win.height, 1 };
    rcCmdCopyImage(cb, win.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, vk_grade_image.image,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    vk_image_to(vk_grade_image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

    const int32_t smp = vk_get_sampler(vk_sampler_key(false, false, 0, VK_WRAP_CLAMP, VK_WRAP_CLAMP, 1));

    vk_begin_rendering_on(&win, NULL, win.width, win.height);
    VkViewport v = { 0.f, 0.f, (float)win.width, (float)win.height, 0.f, 1.f };
    VkRect2D s = { { 0, 0 }, { win.width, win.height } };
    rcCmdSetViewport(cb, 0, 1, &v);
    rcCmdSetScissor(cb, 0, 1, &s);
    rcCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, vk_grade_pipeline);
    struct {
        float saturation, contrast, black, pad;
        int32_t tex, smp;
    } push = { gfx_color_saturation, gfx_color_contrast, gfx_color_black_level, 0.f, vk_grade_image.slot, smp };
    rcCmdPushConstants(cb, vk_pipeline_layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), &push);
    rcCmdDraw(cb, 3, 1, 0, 0);
    vk_end_rendering();
    vk_bound_pipeline = VK_NULL_HANDLE;
    vk_push_valid = false;
}

/*
 * SMAA and FSR 1; see gfx_post.h. Each pass renders into an image of its own
 * (or the window image) through a pipeline with no vertex input, reading
 * the images before it out of the bindless table.
 */
static VkPipeline vk_post_pipelines[GFX_POST_NUM_PASSES];
static bool vk_post_pipeline_failed[GFX_POST_NUM_PASSES];
static VkImg vk_post_edges, vk_post_weights, vk_post_smaa_out, vk_post_easu;
static uint32_t vk_post_area_tex, vk_post_search_tex;
static bool vk_post_failed;

static VkPipeline vk_post_pipeline(GfxPostPass pass) {
    if (vk_post_pipelines[pass] || vk_post_pipeline_failed[pass]) {
        return vk_post_pipelines[pass];
    }
    vk_post_pipeline_failed[pass] = true;

    const GfxPostLang lang = { true, "450", vk_max_texture_slots, VK_MAX_SAMPLER_SLOTS };
    std::string err;
    VkShaderModule vs = vk_compile(gfx_post_vertex_shader(lang), false, "post", &err);
    VkShaderModule fs = vs ? vk_compile(gfx_post_fragment_shader(lang, pass), true, gfx_post_pass_name(pass), &err)
                           : VK_NULL_HANDLE;
    if (vs && fs) {
        vk_post_pipelines[pass] = vk_create_pipeline(vs, fs, NULL, 0, false, false, VK_COMPARE_OP_ALWAYS, 1, false);
    }
    if (!vk_post_pipelines[pass]) {
        sysLogPrintf(LOG_WARNING, "Vulkan: %s shader failed: %s", gfx_post_pass_name(pass), err.c_str());
    } else {
        vk_post_pipeline_failed[pass] = false;
    }
    if (vs) {
        vkDestroyShaderModule(vk_dev, vs, NULL);
    }
    if (fs) {
        vkDestroyShaderModule(vk_dev, fs, NULL);
    }
    return vk_post_pipelines[pass];
}

static bool vk_post_target(VkImg &img, uint32_t width, uint32_t height) {
    if (img.image && img.width == width && img.height == height) {
        return true;
    }
    vk_image_destroy(img);
    return vk_image_create(img, width, height, VK_COLOR_FORMAT,
                           VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VK_SAMPLE_COUNT_1_BIT, 1,
                           false);
}

// SMAA's lookup tables, as textures of the renderer's own. The upload goes
// through the texture unit, which gfx_pc.cpp believes it alone moves.
static uint32_t vk_post_lut(const uint8_t *rgba, uint32_t width, uint32_t height) {
    const int tile = vk_active_tile;
    const VkBinding bound = vk_bound[0];
    const bool linear = vk_textures_linear[0];
    const uint32_t id = gfx_vk_new_texture();
    gfx_vk_select_texture(0, id, true);
    gfx_vk_upload_texture(rgba, width, height, false);
    vk_bound[0] = bound;
    vk_textures_linear[0] = linear;
    vk_active_tile = tile;
    return id;
}

static int32_t vk_post_slot(uint32_t tex_id) {
    return tex_id < vk_textures.size() ? vk_textures[tex_id].img.slot : 0;
}

// One pass: a triangle over the whole of target, reading up to three images
static bool vk_post_draw(GfxPostPass pass, VkImg &target, VkImg *in0, VkImg *in1, VkImg *in2, int32_t lut1,
                         int32_t lut2, float p0, float p1, bool clear) {
    const VkPipeline p = vk_post_pipeline(pass);
    if (!p || !target.image) {
        return false;
    }

    VkImg *ins[3] = { in0, in1, in2 };
    for (VkImg *in : ins) {
        if (in) {
            vk_image_to(*in, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        }
    }

    struct {
        int32_t t0, t1, t2, smp;
        float params[4];
    } push = {
        in0 ? in0->slot : 0,
        in1 ? in1->slot : lut1,
        in2 ? in2->slot : lut2,
        vk_get_sampler(vk_sampler_key(true, true, 0, VK_WRAP_CLAMP, VK_WRAP_CLAMP, 1)),
        { p0, p1, 0.f, 0.f },
    };

    VkCommandBuffer cb = vk_cmd();
    vk_begin_rendering_on(&target, NULL, target.width, target.height);
    VkViewport v = { 0.f, 0.f, (float)target.width, (float)target.height, 0.f, 1.f };
    VkRect2D s = { { 0, 0 }, { target.width, target.height } };
    rcCmdSetViewport(cb, 0, 1, &v);
    rcCmdSetScissor(cb, 0, 1, &s);
    if (clear) {
        VkClearAttachment att = {};
        att.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        att.clearValue.color = { { 0.f, 0.f, 0.f, 0.f } };
        VkClearRect rect = { s, 0, 1 };
        rcCmdClearAttachments(cb, 1, &att, 1, &rect);
    }
    rcCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, p);
    rcCmdPushConstants(cb, vk_pipeline_layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), &push);
    rcCmdDraw(cb, 3, 1, 0, 0);
    vk_end_rendering();
    return true;
}

static bool gfx_vk_post_process(int fb_src, bool smaa, bool fsr, float sharpness) {
    if (vk_failed || vk_post_failed || fb_src <= 0 || (size_t)fb_src >= vk_fbs.size()) {
        return false;
    }

    vk_ensure_recording();
    vk_end_rendering();

    VkFb &src = vk_fbs[fb_src];
    VkFb &fb0 = vk_fbs[0];
    VkImg &win = fb0.color[fb0.cur];
    VkImg *in = &src.color[src.cur];
    if (!in->image || !win.image) {
        return false;
    }

    const uint32_t sw = in->width, sh = in->height;
    const bool scaled = sw != win.width || sh != win.height;
    bool ok = true;

    if (smaa) {
        if (!vk_post_area_tex) {
            vk_post_area_tex = vk_post_lut(gfx_post_area_rgba(), GFX_POST_AREA_WIDTH, GFX_POST_AREA_HEIGHT);
            vk_post_search_tex = vk_post_lut(gfx_post_search_rgba(), GFX_POST_SEARCH_WIDTH, GFX_POST_SEARCH_HEIGHT);
        }
        ok = vk_post_target(vk_post_edges, sw, sh) && vk_post_target(vk_post_weights, sw, sh) &&
             (!scaled || vk_post_target(vk_post_smaa_out, sw, sh));
        ok = ok && vk_post_draw(GFX_POST_SMAA_EDGES, vk_post_edges, in, NULL, NULL, 0, 0, 0.f, 0.f, true);
        ok = ok && vk_post_draw(GFX_POST_SMAA_WEIGHTS, vk_post_weights, &vk_post_edges, NULL, NULL,
                                vk_post_slot(vk_post_area_tex), vk_post_slot(vk_post_search_tex), 0.f, 0.f, false);
        ok = ok && vk_post_draw(GFX_POST_SMAA_BLEND, scaled ? vk_post_smaa_out : win, in, &vk_post_weights, NULL, 0, 0,
                                0.f, 0.f, false);
        in = &vk_post_smaa_out;
    }

    if (ok && scaled) {
        if (fsr && vk_post_target(vk_post_easu, win.width, win.height) &&
            vk_post_draw(GFX_POST_EASU, vk_post_easu, in, NULL, NULL, 0, 0, (float)win.width, (float)win.height,
                         false)) {
            ok = vk_post_draw(GFX_POST_RCAS, win, &vk_post_easu, NULL, NULL, 0, 0, sharpness, 0.f, false);
        } else {
            ok = vk_post_draw(GFX_POST_COPY, win, in, NULL, NULL, 0, 0, 0.f, 0.f, false);
        }
    } else if (ok && !smaa) {
        ok = vk_post_draw(GFX_POST_COPY, win, in, NULL, NULL, 0, 0, 0.f, 0.f, false);
    }

    vk_bound_pipeline = VK_NULL_HANDLE;
    vk_push_valid = false;

    if (!ok) {
        sysLogPrintf(LOG_WARNING, "Vulkan: SMAA/FSR could not run, off");
        vk_post_failed = true;
    }
    return ok;
}

/*
 * TAA's resolve (gfx_rendering_api.h): the rect's depth copied into a
 * sampled depth image of TAA's own, the pass drawn into history image `out`
 * over the rect alone, and the rect copied back into the framebuffer.
 */
static VkImg vk_taa_hist[2], vk_taa_depth;

static bool gfx_vk_taa_resolve(int fb_id, const float *params, int out, int x, int y, int width, int height) {
    if (vk_failed || vk_post_failed || fb_id <= 0 || (size_t)fb_id >= vk_fbs.size()) {
        return false;
    }
    VkFb &fb = vk_fbs[fb_id];
    VkImg &color = fb.color[fb.cur];
    if (fb.msaa > 1 || !fb.has_depth || !fb.depth.image || !color.image) {
        return false;
    }
    const VkPipeline p = vk_post_pipeline(GFX_POST_TAA);
    if (!p) {
        return false;
    }

    vk_ensure_recording();
    vk_end_rendering();

    if (!vk_post_target(vk_taa_hist[0], fb.width, fb.height) || !vk_post_target(vk_taa_hist[1], fb.width, fb.height)) {
        return false;
    }
    if (!vk_taa_depth.image || vk_taa_depth.width != fb.width || vk_taa_depth.height != fb.height) {
        vk_image_destroy(vk_taa_depth);
        if (!vk_image_create(vk_taa_depth, fb.width, fb.height, vk_depth_format,
                             VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VK_SAMPLE_COUNT_1_BIT, 1,
                             false)) {
            return false;
        }
    }

    VkCommandBuffer cb = vk_cmd();
    const VkOffset3D at = { x, y, 0 };
    const VkExtent3D size = { (uint32_t)width, (uint32_t)height, 1 };

    vk_image_to(fb.depth, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    vk_image_to(vk_taa_depth, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkImageCopy dc = {};
    dc.srcSubresource = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1 };
    dc.dstSubresource = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1 };
    dc.srcOffset = at;
    dc.dstOffset = at;
    dc.extent = size;
    rcCmdCopyImage(cb, fb.depth.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, vk_taa_depth.image,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &dc);

    VkImg &dst = vk_taa_hist[out];
    VkImg &hist = vk_taa_hist[1 - out];
    vk_image_to(vk_taa_depth, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    vk_image_to(color, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    vk_image_to(hist, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

    struct {
        int32_t t0, t1, t2, smp;
        float params[4];
        float taa[GFX_POST_TAA_PARAMS];
    } push = {};
    push.t0 = color.slot;
    push.t1 = hist.slot;
    push.t2 = vk_taa_depth.slot;
    push.smp = vk_get_sampler(vk_sampler_key(true, true, 0, VK_WRAP_CLAMP, VK_WRAP_CLAMP, 1));
    memcpy(push.taa, params, sizeof(push.taa));

    vk_begin_rendering_on(&dst, NULL, dst.width, dst.height);
    VkViewport v = { 0.f, 0.f, (float)dst.width, (float)dst.height, 0.f, 1.f };
    VkRect2D s = { { x, y }, { (uint32_t)width, (uint32_t)height } };
    rcCmdSetViewport(cb, 0, 1, &v);
    rcCmdSetScissor(cb, 0, 1, &s);
    rcCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, p);
    rcCmdPushConstants(cb, vk_pipeline_layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), &push);
    rcCmdDraw(cb, 3, 1, 0, 0);
    vk_end_rendering();

    vk_image_to(dst, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    vk_image_to(color, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkImageCopy cc = {};
    cc.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    cc.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    cc.srcOffset = at;
    cc.dstOffset = at;
    cc.extent = size;
    rcCmdCopyImage(cb, dst.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, color.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                   1, &cc);

    vk_bound_pipeline = VK_NULL_HANDLE;
    vk_push_valid = false;
    return true;
}

static void gfx_vk_on_resize(void) {
}

static void gfx_vk_start_frame(void) {
    vk_frame_count++;
    vk_ensure_recording();

    if (g_GfxLogStats && ++vk_stat_frames >= g_GfxLogStats) {
        sysLogPrintf(LOG_NOTE,
                     "vulkan: %.3f ms a frame waiting for the GPU; %u shaders compiled (%.1f ms), %u from the cache, "
                     "%u pipelines built (%.1f ms)",
                     vk_stat_wait_ns / 1e6 / vk_stat_frames, vk_stat_compiles, vk_stat_compile_ms, vk_stat_cached,
                     vk_stat_pipelines, vk_stat_pipeline_ms);
        vk_stat_frames = 0;
        vk_stat_wait_ns = 0;
        vk_stat_compiles = vk_stat_cached = vk_stat_pipelines = 0;
        vk_stat_compile_ms = vk_stat_pipeline_ms = 0;
    }

    // The cache goes to disk once nothing new has been compiled for five
    // seconds or so, and at exit
    if (vk_cache_dirty && ++vk_cache_idle_frames >= 300) {
        vk_cache_save();
    }
}

static void gfx_vk_end_frame(void) {
    vk_grade_frame();
    vk_end_rendering();
}

static void gfx_vk_finish_render(void) {
}

/*
 * Reading the frame back. The window image is bottom row first, which is what
 * both callers want.
 */
static bool vk_readback_reserve(VkBuf &b, VkDeviceSize bytes) {
    if (b.buf && b.size >= bytes) {
        return true;
    }
    if (b.buf) {
        vk_wait_serial(vk_submitted);
        vk_buffer_destroy_now(b);
    }
    return vk_buffer_create(b, bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
}

static void vk_copy_window_to(VkBuf &b, int x, int y, int width, int height) {
    VkFb &fb0 = vk_fbs[0];
    VkImg &win = fb0.color[fb0.cur];
    VkCommandBuffer cb = vk_cmd();
    vk_end_rendering();
    vk_image_to(win, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);

    VkBufferImageCopy copy = {};
    copy.bufferRowLength = (uint32_t)width;
    copy.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    copy.imageOffset = { x, y, 0 };
    copy.imageExtent = { (uint32_t)width, (uint32_t)height, 1 };
    rcCmdCopyImageToBuffer(cb, win.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, b.buf, 1, &copy);
}

static bool gfx_vk_read_screen_pixels(int x, int y, int width, int height, void *rgb) {
    if (vk_failed || vk_fbs.empty() || width <= 0 || height <= 0) {
        return false;
    }
    VkFb &fb0 = vk_fbs[0];
    if (!fb0.color[fb0.cur].image || x < 0 || y < 0 || x + width > (int)fb0.width || y + height > (int)fb0.height) {
        return false;
    }

    const VkDeviceSize bytes = (VkDeviceSize)width * height * 4;
    if (!vk_readback_reserve(vk_readback, bytes)) {
        return false;
    }

    vk_copy_window_to(vk_readback, x, y, width, height);
    vk_flush_and_wait();

    // BGRA to tightly packed RGB
    const uint8_t *src = vk_readback.mem.mapped;
    uint8_t *dst = (uint8_t *)rgb;
    const size_t n = (size_t)width * height;
    for (size_t i = 0; i < n; i++) {
        dst[i * 3 + 0] = src[i * 4 + 2];
        dst[i * 3 + 1] = src[i * 4 + 1];
        dst[i * 3 + 2] = src[i * 4 + 0];
    }
    return true;
}

static void gfx_vk_capture_stop(void) {
    if (vk_capture_width) {
        vk_wait_serial(vk_submitted);
    }
    for (int i = 0; i < VK_CAPTURE_BUFS; i++) {
        vk_buffer_destroy_now(vk_capture_bufs[i]);
        vk_capture_serial[i] = 0;
    }
    vk_capture_width = vk_capture_height = 0;
    vk_capture_next = vk_capture_pending = 0;
}

static int gfx_vk_capture_start(int width, int height) {
    gfx_vk_capture_stop();

    if (vk_failed || width <= 0 || height <= 0) {
        return GFX_CAPTURE_NONE;
    }
    for (int i = 0; i < VK_CAPTURE_BUFS; i++) {
        if (!vk_buffer_create(vk_capture_bufs[i], (VkDeviceSize)width * height * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT)) {
            gfx_vk_capture_stop();
            return GFX_CAPTURE_NONE;
        }
    }
    vk_capture_width = width;
    vk_capture_height = height;
    sysLogPrintf(LOG_NOTE, "Vulkan: capturing BGRA, %d frames behind", VK_CAPTURE_BUFS - 1);
    return GFX_CAPTURE_BGRA;
}

static bool vk_capture_collect(void *dst) {
    const int slot = (vk_capture_next - vk_capture_pending + VK_CAPTURE_BUFS) % VK_CAPTURE_BUFS;
    vk_wait_serial(vk_capture_serial[slot]);
    memcpy(dst, vk_capture_bufs[slot].mem.mapped, (size_t)vk_capture_width * vk_capture_height * 4);
    vk_capture_pending--;
    return true;
}

static bool gfx_vk_capture_read(void *dst) {
    if (!vk_capture_width || !dst || vk_fbs.empty()) {
        return false;
    }

    VkFb &fb0 = vk_fbs[0];
    const int w = std::min(vk_capture_width, (int)fb0.width);
    const int h = std::min(vk_capture_height, (int)fb0.height);
    if (w <= 0 || h <= 0) {
        return false;
    }

    // Rows are the capture's width apart whatever part of it the window fills
    VkBuf &b = vk_capture_bufs[vk_capture_next];
    VkCommandBuffer cb = vk_cmd();
    vk_end_rendering();
    VkImg &win = fb0.color[fb0.cur];
    vk_image_to(win, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    VkBufferImageCopy copy = {};
    copy.bufferRowLength = (uint32_t)vk_capture_width;
    copy.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    copy.imageExtent = { (uint32_t)w, (uint32_t)h, 1 };
    rcCmdCopyImageToBuffer(cb, win.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, b.buf, 1, &copy);

    vk_capture_serial[vk_capture_next] = vk_submitted + 1;
    vk_capture_next = (vk_capture_next + 1) % VK_CAPTURE_BUFS;
    vk_capture_pending++;

    if (vk_capture_pending < VK_CAPTURE_BUFS) {
        return false;
    }
    return vk_capture_collect(dst);
}

static bool gfx_vk_capture_drain(void *dst) {
    bool got = false;
    if (!vk_capture_width || !vk_capture_pending || !dst) {
        return false;
    }
    while (vk_capture_pending > 0) {
        got = vk_capture_collect(dst) || got;
    }
    return got;
}

/*
 * Starting up
 */

static VKAPI_ATTR VkBool32 VKAPI_CALL vk_debug_callback(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                                         VkDebugUtilsMessageTypeFlagsEXT type,
                                                         const VkDebugUtilsMessengerCallbackDataEXT *data,
                                                         void *user) {
    sysLogPrintf(severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT ? LOG_ERROR : LOG_WARNING, "Vulkan: %s",
                 data->pMessage);
    return VK_FALSE;
}

static bool vk_has_ext(const std::vector<VkExtensionProperties> &exts, const char *name) {
    for (const VkExtensionProperties &e : exts) {
        if (!strcmp(e.extensionName, name)) {
            return true;
        }
    }
    return false;
}

static bool vk_format_ok(VkFormat fmt, VkFormatFeatureFlags want) {
    VkFormatProperties fp;
    vkGetPhysicalDeviceFormatProperties(vk_phys, fmt, &fp);
    return (fp.optimalTilingFeatures & want) == want;
}

static void vk_shutdown(void) {
    vk_drain();
    if (vk_dev) {
        vkDeviceWaitIdle(vk_dev);
        vk_completed = vk_submitted;
        vk_collect();
        if (vk_query_pool) {
            vkDestroyQueryPool(vk_dev, vk_query_pool, NULL);
            vk_query_pool = VK_NULL_HANDLE;
        }
        vkDestroyDevice(vk_dev, NULL);
        vk_dev = VK_NULL_HANDLE;
    }
    if (vk_surface) {
        vkDestroySurfaceKHR(vk_instance, vk_surface, NULL);
        vk_surface = VK_NULL_HANDLE;
    }
    if (vk_messenger && vk_destroy_debug_messenger) {
        vk_destroy_debug_messenger(vk_instance, vk_messenger, NULL);
        vk_messenger = VK_NULL_HANDLE;
    }
    if (vk_instance) {
        vkDestroyInstance(vk_instance, NULL);
        vk_instance = VK_NULL_HANDLE;
    }
    if (vk_shaderc) {
        shaderc_compiler_release(vk_shaderc);
        vk_shaderc = NULL;
    }
}

static bool vk_init_instance(SDL_Window *wnd, bool debug) {
    vk_get_instance_proc = (PFN_vkGetInstanceProcAddr)SDL_Vulkan_GetVkGetInstanceProcAddr();
    if (!vk_get_instance_proc) {
        vk_fail("no Vulkan loader: %s", SDL_GetError());
        return false;
    }

#define VK_LOAD_GLOBAL(name) name = (PFN_##name)vk_get_instance_proc(VK_NULL_HANDLE, #name);
    VK_GLOBAL_FUNCS(VK_LOAD_GLOBAL)
#undef VK_LOAD_GLOBAL
    vk_enumerate_instance_version =
        (PFN_vkEnumerateInstanceVersion)vk_get_instance_proc(VK_NULL_HANDLE, "vkEnumerateInstanceVersion");

    uint32_t loader_version = VK_API_VERSION_1_0;
    if (vk_enumerate_instance_version) {
        vk_enumerate_instance_version(&loader_version);
    }
    if (!vkCreateInstance || loader_version < VK_API_VERSION_1_1) {
        vk_fail("the Vulkan loader is older than 1.1");
        return false;
    }

    unsigned int n = 0;
    if (!SDL_Vulkan_GetInstanceExtensions(wnd, &n, NULL)) {
        vk_fail("SDL has no Vulkan surface for this window: %s", SDL_GetError());
        return false;
    }
    std::vector<const char *> exts(n);
    SDL_Vulkan_GetInstanceExtensions(wnd, &n, exts.data());

    std::vector<const char *> layers;
    if (debug) {
        uint32_t count = 0;
        vkEnumerateInstanceExtensionProperties(NULL, &count, NULL);
        std::vector<VkExtensionProperties> avail(count);
        vkEnumerateInstanceExtensionProperties(NULL, &count, avail.data());
        if (vk_has_ext(avail, VK_EXT_DEBUG_UTILS_EXTENSION_NAME)) {
            exts.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
        }
        count = 0;
        vkEnumerateInstanceLayerProperties(&count, NULL);
        std::vector<VkLayerProperties> lp(count);
        vkEnumerateInstanceLayerProperties(&count, lp.data());
        for (const VkLayerProperties &l : lp) {
            if (!strcmp(l.layerName, "VK_LAYER_KHRONOS_validation")) {
                layers.push_back("VK_LAYER_KHRONOS_validation");
                sysLogPrintf(LOG_NOTE, "Vulkan: validation layer on");
            }
        }
    }

    VkApplicationInfo app = { VK_STRUCTURE_TYPE_APPLICATION_INFO };
    app.pApplicationName = "Perfect Dark";
    app.pEngineName = "fast3d";
    app.apiVersion = loader_version >= VK_API_VERSION_1_3 ? VK_API_VERSION_1_3 : VK_API_VERSION_1_2;
    if (loader_version < VK_API_VERSION_1_2) {
        app.apiVersion = VK_API_VERSION_1_1;
    }

    VkInstanceCreateInfo ci = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
    ci.pApplicationInfo = &app;
    ci.enabledExtensionCount = (uint32_t)exts.size();
    ci.ppEnabledExtensionNames = exts.data();
    ci.enabledLayerCount = (uint32_t)layers.size();
    ci.ppEnabledLayerNames = layers.data();

    const VkResult r = vkCreateInstance(&ci, NULL, &vk_instance);
    if (r != VK_SUCCESS) {
        vk_fail("could not create an instance (%d)", (int)r);
        vk_instance = VK_NULL_HANDLE;
        return false;
    }

#define VK_LOAD_INSTANCE(name) name = (PFN_##name)vk_get_instance_proc(vk_instance, #name);
    VK_INSTANCE_FUNCS(VK_LOAD_INSTANCE)
#undef VK_LOAD_INSTANCE

    if (debug) {
        vk_create_debug_messenger =
            (PFN_vkCreateDebugUtilsMessengerEXT)vk_get_instance_proc(vk_instance, "vkCreateDebugUtilsMessengerEXT");
        vk_destroy_debug_messenger =
            (PFN_vkDestroyDebugUtilsMessengerEXT)vk_get_instance_proc(vk_instance, "vkDestroyDebugUtilsMessengerEXT");
        if (vk_create_debug_messenger) {
            VkDebugUtilsMessengerCreateInfoEXT mi = { VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT };
            mi.messageSeverity =
                VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
            mi.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                             VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                             VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
            mi.pfnUserCallback = vk_debug_callback;
            vk_create_debug_messenger(vk_instance, &mi, NULL, &vk_messenger);
        }
    }

    if (!SDL_Vulkan_CreateSurface(wnd, vk_instance, &vk_surface)) {
        vk_fail("could not create a surface: %s", SDL_GetError());
        vk_surface = VK_NULL_HANDLE;
        return false;
    }

    return true;
}

// Whether a device can run this renderer, and how well: -1 for not at all
static int vk_rate_device(VkPhysicalDevice pd, uint32_t *family, const char **why) {
    VkPhysicalDeviceProperties p;
    vkGetPhysicalDeviceProperties(pd, &p);

    if (p.apiVersion < VK_API_VERSION_1_2) {
        *why = "older than Vulkan 1.2";
        return -1;
    }

    uint32_t count = 0;
    vkEnumerateDeviceExtensionProperties(pd, NULL, &count, NULL);
    std::vector<VkExtensionProperties> exts(count);
    vkEnumerateDeviceExtensionProperties(pd, NULL, &count, exts.data());
    if (!vk_has_ext(exts, VK_KHR_SWAPCHAIN_EXTENSION_NAME)) {
        *why = "no swapchain";
        return -1;
    }
    if (p.apiVersion < VK_API_VERSION_1_3 && !vk_has_ext(exts, VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME)) {
        *why = "no dynamic rendering";
        return -1;
    }

    count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &count, NULL);
    std::vector<VkQueueFamilyProperties> qf(count);
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &count, qf.data());
    int found = -1;
    for (uint32_t i = 0; i < count; i++) {
        VkBool32 present = VK_FALSE;
        vkGetPhysicalDeviceSurfaceSupportKHR(pd, i, vk_surface, &present);
        if ((qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && present) {
            found = (int)i;
            break;
        }
    }
    if (found < 0) {
        *why = "cannot draw to this window";
        return -1;
    }
    *family = (uint32_t)found;

    switch (p.deviceType) {
        case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:
            return 4;
        case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU:
            return 3;
        case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU:
            return 2;
        case VK_PHYSICAL_DEVICE_TYPE_CPU:
            return 0;
        default:
            return 1;
    }
}

static bool vk_init_device(void) {
    uint32_t count = 0;
    vkEnumeratePhysicalDevices(vk_instance, &count, NULL);
    std::vector<VkPhysicalDevice> devs(count);
    vkEnumeratePhysicalDevices(vk_instance, &count, devs.data());
    if (!count) {
        vk_fail("no Vulkan devices");
        return false;
    }

    // --vk-device N picks one by its place in the list the log prints
    const int want = sysArgGetInt("--vk-device", -1);
    int best = -1, best_score = -1;
    uint32_t best_family = 0;
    for (uint32_t i = 0; i < count; i++) {
        VkPhysicalDeviceProperties p;
        vkGetPhysicalDeviceProperties(devs[i], &p);
        uint32_t family = 0;
        const char *why = "";
        int score = vk_rate_device(devs[i], &family, &why);
        sysLogPrintf(LOG_NOTE, "Vulkan: device %u: %s%s%s", i, p.deviceName, score < 0 ? " - unusable, " : "",
                     score < 0 ? why : "");
        if (score >= 0 && want == (int)i) {
            score = 100;
        }
        if (score > best_score) {
            best = (int)i;
            best_score = score;
            best_family = family;
        }
    }
    if (best < 0) {
        vk_fail("no device can run this renderer (it needs Vulkan 1.2 with dynamic rendering)");
        return false;
    }

    vk_phys = devs[best];
    vk_queue_family = best_family;
    vkGetPhysicalDeviceProperties(vk_phys, &vk_props);
    vkGetPhysicalDeviceMemoryProperties(vk_phys, &vk_memprops);

    // Features
    VkPhysicalDeviceDynamicRenderingFeatures dr = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES };
    VkPhysicalDeviceVulkan12Features v12 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES };
    v12.pNext = &dr;
    VkPhysicalDeviceFeatures2 f2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };
    f2.pNext = &v12;
    vkGetPhysicalDeviceFeatures2(vk_phys, &f2);

    if (!dr.dynamicRendering) {
        vk_fail("%s has no dynamic rendering", vk_props.deviceName);
        return false;
    }
    if (!v12.descriptorBindingPartiallyBound || !v12.descriptorBindingSampledImageUpdateAfterBind ||
        !v12.descriptorBindingUpdateUnusedWhilePending || !f2.features.shaderSampledImageArrayDynamicIndexing) {
        vk_fail("%s cannot index its textures (descriptor indexing)", vk_props.deviceName);
        return false;
    }

    // The texture table, as big as the device lets a stage see after bind
    VkPhysicalDeviceVulkan12Properties p12 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES };
    VkPhysicalDeviceProperties2 pp2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2 };
    pp2.pNext = &p12;
    vkGetPhysicalDeviceProperties2(vk_phys, &pp2);
    vk_max_texture_slots = std::min<uint32_t>(16384, std::min(p12.maxPerStageDescriptorUpdateAfterBindSampledImages,
                                                              p12.maxDescriptorSetUpdateAfterBindSampledImages));
    if (vk_max_texture_slots < 1024 || p12.maxDescriptorSetUpdateAfterBindSamplers < VK_MAX_SAMPLER_SLOTS) {
        vk_fail("%s has room for only %u textures after bind", vk_props.deviceName, vk_max_texture_slots);
        return false;
    }

    vk_have_depth_clamp = f2.features.depthClamp;
    vk_have_anisotropy = f2.features.samplerAnisotropy;
    vk_have_mirror_clamp = v12.samplerMirrorClampToEdge;

    VkPhysicalDeviceDynamicRenderingFeatures dr_on = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES };
    dr_on.dynamicRendering = VK_TRUE;
    VkPhysicalDeviceVulkan12Features v12_on = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES };
    v12_on.pNext = &dr_on;
    v12_on.samplerMirrorClampToEdge = vk_have_mirror_clamp;
    v12_on.descriptorBindingPartiallyBound = VK_TRUE;
    v12_on.descriptorBindingSampledImageUpdateAfterBind = VK_TRUE;
    v12_on.descriptorBindingUpdateUnusedWhilePending = VK_TRUE;
    VkPhysicalDeviceFeatures2 f2_on = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };
    f2_on.features.shaderSampledImageArrayDynamicIndexing = VK_TRUE;
    f2_on.pNext = &v12_on;
    f2_on.features.depthClamp = vk_have_depth_clamp;
    f2_on.features.samplerAnisotropy = vk_have_anisotropy;

    std::vector<const char *> exts;
    exts.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
    if (vk_props.apiVersion < VK_API_VERSION_1_3) {
        exts.push_back(VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME);
    }

    const float prio = 1.f;
    VkDeviceQueueCreateInfo qi = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
    qi.queueFamilyIndex = vk_queue_family;
    qi.queueCount = 1;
    qi.pQueuePriorities = &prio;

    VkDeviceCreateInfo di = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
    di.pNext = &f2_on;
    di.queueCreateInfoCount = 1;
    di.pQueueCreateInfos = &qi;
    di.enabledExtensionCount = (uint32_t)exts.size();
    di.ppEnabledExtensionNames = exts.data();

    const VkResult r = vkCreateDevice(vk_phys, &di, NULL, &vk_dev);
    if (r != VK_SUCCESS) {
        vk_fail("could not create a device on %s (%d)", vk_props.deviceName, (int)r);
        vk_dev = VK_NULL_HANDLE;
        return false;
    }

#define VK_LOAD_DEVICE(name)                                        \
    name = (PFN_##name)vkGetDeviceProcAddr(vk_dev, #name);         \
    if (!name) {                                                    \
        vk_fail("the driver has no %s", #name);                     \
        return false;                                               \
    }
    VK_DEVICE_FUNCS(VK_LOAD_DEVICE)
#undef VK_LOAD_DEVICE

    vk_cmd_begin_rendering = (PFN_vkCmdBeginRendering)vkGetDeviceProcAddr(vk_dev, "vkCmdBeginRendering");
    vk_cmd_end_rendering = (PFN_vkCmdEndRendering)vkGetDeviceProcAddr(vk_dev, "vkCmdEndRendering");
    if (!vk_cmd_begin_rendering || !vk_cmd_end_rendering) {
        vk_cmd_begin_rendering = (PFN_vkCmdBeginRendering)vkGetDeviceProcAddr(vk_dev, "vkCmdBeginRenderingKHR");
        vk_cmd_end_rendering = (PFN_vkCmdEndRendering)vkGetDeviceProcAddr(vk_dev, "vkCmdEndRenderingKHR");
    }
    if (!vk_cmd_begin_rendering || !vk_cmd_end_rendering) {
        vk_fail("the driver has no vkCmdBeginRendering");
        return false;
    }

    vkGetDeviceQueue(vk_dev, vk_queue_family, 0, &vk_queue);

    snprintf(vk_device_desc, sizeof(vk_device_desc), "%s, Vulkan %u.%u.%u", vk_props.deviceName,
             VK_API_VERSION_MAJOR(vk_props.apiVersion), VK_API_VERSION_MINOR(vk_props.apiVersion),
             VK_API_VERSION_PATCH(vk_props.apiVersion));
    return true;
}

static bool vk_init_formats(void) {
    if (!vk_format_ok(VK_COLOR_FORMAT, VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT |
                                           VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT |
                                           VK_FORMAT_FEATURE_BLIT_DST_BIT)) {
        vk_fail("B8G8R8A8 cannot be drawn into and blitted");
        return false;
    }
    if (!vk_format_ok(VK_TEXTURE_FORMAT, VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
                                             VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT |
                                             VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT)) {
        vk_fail("R8G8B8A8 textures cannot be filtered and mipmapped");
        return false;
    }

    // OpenGL's is D24S8; AMD has no D24, where the float buffer is as fine
    // over the far half of the range, which is where N64 depth lives
    static const VkFormat depths[] = { VK_FORMAT_D24_UNORM_S8_UINT, VK_FORMAT_D32_SFLOAT_S8_UINT, VK_FORMAT_D32_SFLOAT,
                                       VK_FORMAT_D16_UNORM };
    vk_depth_format = VK_FORMAT_UNDEFINED;
    for (VkFormat f : depths) {
        if (vk_format_ok(f, VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT)) {
            vk_depth_format = f;
            break;
        }
    }
    if (vk_depth_format == VK_FORMAT_UNDEFINED) {
        vk_fail("no depth format");
        return false;
    }
    vk_depth_has_stencil = vk_depth_format == VK_FORMAT_D24_UNORM_S8_UINT || vk_depth_format == VK_FORMAT_D32_SFLOAT_S8_UINT;

    // The most samples a target can have; a count between the ones offered
    // (llvmpipe has 4x and no 2x) is taken down to one that is, below
    vk_sample_counts = vk_props.limits.framebufferColorSampleCounts & vk_props.limits.framebufferDepthSampleCounts;
    vk_max_msaa = 1;
    for (uint32_t n = 2; n <= 16; n *= 2) {
        if (vk_sample_counts & n) {
            vk_max_msaa = n;
        }
    }

    // The swapchain: a UNORM format the blit can write, so it is not
    // converted to sRGB on the way (the game's colours are already gamma)
    uint32_t count = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(vk_phys, vk_surface, &count, NULL);
    std::vector<VkSurfaceFormatKHR> fmts(count);
    vkGetPhysicalDeviceSurfaceFormatsKHR(vk_phys, vk_surface, &count, fmts.data());
    vk_swap_format = VK_FORMAT_UNDEFINED;
    for (VkFormat want : { VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_A2B10G10R10_UNORM_PACK32 }) {
        for (const VkSurfaceFormatKHR &f : fmts) {
            if (f.format == want && f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR &&
                vk_format_ok(want, VK_FORMAT_FEATURE_BLIT_DST_BIT)) {
                vk_swap_format = want;
                break;
            }
        }
        if (vk_swap_format != VK_FORMAT_UNDEFINED) {
            break;
        }
    }
    if (vk_swap_format == VK_FORMAT_UNDEFINED) {
        vk_fail("the window offers no UNORM format to present in");
        return false;
    }

    VkSurfaceCapabilitiesKHR caps;
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(vk_phys, vk_surface, &caps);
    if (!(caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT)) {
        vk_fail("the window cannot be blitted into");
        return false;
    }

    return true;
}

static bool vk_init_objects(void) {
    VkDescriptorSetLayoutBinding b[2] = {};
    b[0].binding = 0;
    b[0].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    b[0].descriptorCount = vk_max_texture_slots;
    b[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    b[1].binding = 1;
    b[1].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
    b[1].descriptorCount = VK_MAX_SAMPLER_SLOTS;
    b[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    const VkDescriptorBindingFlags bf[2] = {
        VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT | VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT |
            VK_DESCRIPTOR_BINDING_UPDATE_UNUSED_WHILE_PENDING_BIT,
        VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT | VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT |
            VK_DESCRIPTOR_BINDING_UPDATE_UNUSED_WHILE_PENDING_BIT,
    };
    VkDescriptorSetLayoutBindingFlagsCreateInfo bfi = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO };
    bfi.bindingCount = 2;
    bfi.pBindingFlags = bf;
    VkDescriptorSetLayoutCreateInfo li = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    li.pNext = &bfi;
    li.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
    li.bindingCount = 2;
    li.pBindings = b;
    if (vkCreateDescriptorSetLayout(vk_dev, &li, NULL, &vk_set_layout) != VK_SUCCESS) {
        vk_fail("could not create the descriptor set layout");
        return false;
    }

    VkDescriptorPoolSize ps[2] = { { VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, vk_max_texture_slots },
                                   { VK_DESCRIPTOR_TYPE_SAMPLER, VK_MAX_SAMPLER_SLOTS } };
    VkDescriptorPoolCreateInfo dpi = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
    dpi.flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT;
    dpi.maxSets = 1;
    dpi.poolSizeCount = 2;
    dpi.pPoolSizes = ps;
    if (vkCreateDescriptorPool(vk_dev, &dpi, NULL, &vk_bindless_pool) != VK_SUCCESS) {
        vk_fail("could not create the texture table");
        return false;
    }
    VkDescriptorSetAllocateInfo dai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
    dai.descriptorPool = vk_bindless_pool;
    dai.descriptorSetCount = 1;
    dai.pSetLayouts = &vk_set_layout;
    if (vkAllocateDescriptorSets(vk_dev, &dai, &vk_bindless_set) != VK_SUCCESS) {
        vk_fail("could not allocate the texture table");
        return false;
    }

    // The largest block any shader pushes: the post passes' with TAA's
    // parameters (gfx_post.h), 112 bytes, inside the 128 every device has
    VkPushConstantRange pc = { VK_SHADER_STAGE_FRAGMENT_BIT, 0, (uint32_t)std::max<size_t>(sizeof(VkPush), 112) };
    VkPipelineLayoutCreateInfo pli = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    pli.setLayoutCount = 1;
    pli.pSetLayouts = &vk_set_layout;
    pli.pushConstantRangeCount = 1;
    pli.pPushConstantRanges = &pc;
    if (vkCreatePipelineLayout(vk_dev, &pli, NULL, &vk_pipeline_layout) != VK_SUCCESS) {
        vk_fail("could not create the pipeline layout");
        return false;
    }

    vk_cache_load();
    u32 pc_size = 0;
    void *pc_data = fsFileLoad(VK_PIPELINE_CACHE_PATH, &pc_size);
    VkPipelineCacheCreateInfo pci = { VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO };
    pci.initialDataSize = pc_data ? pc_size : 0;
    pci.pInitialData = pc_data;
    if (vkCreatePipelineCache(vk_dev, &pci, NULL, &vk_pipeline_cache) != VK_SUCCESS) {
        // another driver's, or damaged: start empty
        pci.initialDataSize = 0;
        pci.pInitialData = NULL;
        vkCreatePipelineCache(vk_dev, &pci, NULL, &vk_pipeline_cache);
    }
    free(pc_data);
    atexit(vk_cache_save_at_exit);

    for (int s = 0; s < VK_FRAMES; s++) {
        VkSlot &sl = vk_slots[s];
        VkCommandPoolCreateInfo cpi = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
        cpi.queueFamilyIndex = vk_queue_family;
        cpi.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
        if (vkCreateCommandPool(vk_dev, &cpi, NULL, &sl.pool) != VK_SUCCESS) {
            vk_fail("could not create a command pool");
            return false;
        }
        VkCommandBuffer cbs[2];
        VkCommandBufferAllocateInfo ai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
        ai.commandPool = sl.pool;
        ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount = 2;
        if (vkAllocateCommandBuffers(vk_dev, &ai, cbs) != VK_SUCCESS) {
            vk_fail("could not allocate command buffers");
            return false;
        }
        sl.cmd = cbs[0];
        sl.upload = cbs[1];

        VkFenceCreateInfo fi = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
        fi.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        vkCreateFence(vk_dev, &fi, NULL, &sl.fence);
        VkSemaphoreCreateInfo si = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
        vkCreateSemaphore(vk_dev, &si, NULL, &sl.acquired);
    }

    // Without it the glares fall back to their line of sight test
    VkQueryPoolCreateInfo qpi = { VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO };
    qpi.queryType = VK_QUERY_TYPE_OCCLUSION;
    qpi.queryCount = GFX_OCCLUSION_SLOTS;
    if (vkCreateQueryPool(vk_dev, &qpi, NULL, &vk_query_pool) != VK_SUCCESS) {
        vk_query_pool = VK_NULL_HANDLE;
    }

    vk_shaderc = shaderc_compiler_initialize();
    if (!vk_shaderc) {
        vk_fail("could not start the shader compiler");
        return false;
    }

    // Texture id 0 and the stand-in for an unread sampler: opaque black, as
    // OpenGL samples a texture that has nothing in it
    vk_textures.resize(1);
    vk_begin_recording();
    if (!vk_image_create(vk_dummy, 1, 1, VK_TEXTURE_FORMAT, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                         VK_SAMPLE_COUNT_1_BIT, 1, false)) {
        vk_fail("could not create a texture");
        return false;
    }
    VkCommandBuffer cb = VK_MAIN_CB;
    vk_barrier(cb, vk_dummy.image, VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, VK_IMAGE_LAYOUT_UNDEFINED,
               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkClearColorValue black = { { 0.f, 0.f, 0.f, 1.f } };
    VkImageSubresourceRange range = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    rcCmdClearColorImage(cb, vk_dummy.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &range);
    vk_barrier(cb, vk_dummy.image, VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
               VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    vk_dummy.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    return true;
}

static void gfx_vk_init(void) {
    SDL_Window *wnd = gfx_sdl_window();
    const bool debug = sysArgCheck("--debug-vk");

    if (!wnd) {
        vk_fail("no window was made for Vulkan");
        vk_failed = true;
        return;
    }

    if (!vk_init_instance(wnd, debug) || !vk_init_device() || !vk_init_formats() || !vk_init_objects() ||
        !vk_create_swapchain()) {
        vk_failed = true;
        vk_shutdown();
        return;
    }

    sysLogPrintf(LOG_NOTE, "Vulkan: using %s", vk_device_desc);
    sysLogPrintf(LOG_NOTE, "Vulkan: depth %s, depth clamp %s, mirror clamp %s, anisotropy %s, MSAA up to %ux",
                 vk_depth_format == VK_FORMAT_D24_UNORM_S8_UINT     ? "D24S8"
                 : vk_depth_format == VK_FORMAT_D32_SFLOAT_S8_UINT ? "D32FS8"
                 : vk_depth_format == VK_FORMAT_D32_SFLOAT         ? "D32F"
                                                                   : "D16",
                 vk_have_depth_clamp ? "yes" : "no", vk_have_mirror_clamp ? "yes" : "no",
                 vk_have_anisotropy ? "yes" : "no", vk_max_msaa);

    gfx_sdl_set_vulkan_hooks(vk_present, vk_get_swap_interval, vk_set_swap_interval);
    vk_no_present = sysArgCheck("--vk-no-present");

    // --vk-no-thread records on the game's thread, for comparison
    vk_threaded = !sysArgCheck("--vk-no-thread");
    if (vk_threaded) {
        // never joined or destroyed: it waits on its condition until the end
        vk_worker = new std::thread(vk_worker_main);
    }
    sysLogPrintf(LOG_NOTE, "Vulkan: commands recorded %s", vk_threaded ? "on a worker thread" : "on the game's thread");

    vk_fbs.resize(1); // the window
    vk_fbs[0].sampler_key = vk_sampler_key(true, true, 0, VK_WRAP_REPEAT, VK_WRAP_REPEAT, 1);
    vk_fbs[0].sampler = vk_get_sampler(vk_fbs[0].sampler_key);
    vk_cur_fb = 0;
}

} // namespace

extern "C" int gfx_vulkan_failed(void) {
    return vk_failed ? 1 : 0;
}

extern "C" const char *gfx_vulkan_device_name(void) {
    return vk_device_desc;
}

/*
 * Occlusion queries (gfx_rendering_api.h). A query is reset in the frame's
 * upload command buffer, which is submitted ahead of the frame's own and is
 * never inside a render pass, then begun and ended round its one draw inside
 * the rendering already under way. Each slot remembers the submission it went
 * out in, so a read waits for exactly that one - which the frame two before
 * this has always finished anyway (vk_begin_recording()).
 */
static uint64_t vk_query_serial[GFX_OCCLUSION_SLOTS];

static bool gfx_vk_occlusion_begin(int slot) {
    if (vk_failed || !vk_dev || !vk_query_pool || slot < 0 || slot >= GFX_OCCLUSION_SLOTS || vk_cur_fb < 0 ||
        (size_t)vk_cur_fb >= vk_fbs.size()) {
        return false;
    }

    vk_ensure_recording();
    if (!vk_rendering) {
        vk_begin_fb_rendering();
        if (!vk_rendering) {
            return false;
        }
    }

    VkSlot &sl = vk_slots[vk_slot];
    vkCmdResetQueryPool(sl.upload, vk_query_pool, (uint32_t)slot, 1);
    sl.upload_used = true;
    rcCmdBeginQuery(VK_MAIN_CB, (uint32_t)slot);
    vk_query_serial[slot] = vk_submitted + 1;
    return true;
}

static void gfx_vk_occlusion_end(int slot) {
    rcCmdEndQuery(VK_MAIN_CB, (uint32_t)slot);
}

static int gfx_vk_occlusion_result(int slot) {
    if (vk_failed || !vk_dev || !vk_query_pool || slot < 0 || slot >= GFX_OCCLUSION_SLOTS) {
        return -1;
    }

    const uint64_t serial = vk_query_serial[slot];
    if (serial == 0 || serial > vk_submitted) {
        // never drawn, or drawn into the frame still being recorded
        return -1;
    }
    if (serial > vk_completed) {
        // that submission alone: vk_wait_serial() would wait for the newer
        // frame in flight as well
        for (int s = 0; s < VK_FRAMES; s++) {
            if (vk_slots[s].serial == serial) {
                vk_wait_slot(s);
            }
        }
    }

    uint64_t result[2] = { 0, 0 };
    const VkResult r = vkGetQueryPoolResults(vk_dev, vk_query_pool, (uint32_t)slot, 1, sizeof(result), result,
                                             sizeof(result), VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
    if ((r != VK_SUCCESS && r != VK_NOT_READY) || !result[1]) {
        return -1;
    }
    return result[0] > INT32_MAX ? INT32_MAX : (int)result[0];
}

struct GfxRenderingAPI gfx_vulkan_api = {
    gfx_vk_get_name,
    gfx_vk_get_max_texture_size,
    gfx_vk_get_clip_parameters,
    gfx_vk_unload_shader,
    gfx_vk_load_shader,
    gfx_vk_create_and_load_new_shader,
    gfx_vk_lookup_shader,
    gfx_vk_shader_get_info,
    gfx_vk_clear_shaders,
    gfx_vk_new_texture,
    gfx_vk_select_texture,
    gfx_vk_upload_texture,
    gfx_vk_set_sampler_parameters,
    gfx_vk_set_depth_mode,
    gfx_vk_set_depth_range,
    gfx_vk_set_viewport,
    gfx_vk_set_scissor,
    gfx_vk_set_use_alpha,
    gfx_vk_draw_triangles,
    gfx_vk_init,
    gfx_vk_on_resize,
    gfx_vk_start_frame,
    gfx_vk_end_frame,
    gfx_vk_finish_render,
    gfx_vk_create_framebuffer,
    gfx_vk_update_framebuffer_parameters,
    gfx_vk_start_draw_to_framebuffer,
    gfx_vk_copy_framebuffer,
    gfx_vk_clear_framebuffer,
    gfx_vk_resolve_msaa_color_buffer,
    gfx_vk_get_framebuffer_texture_id,
    gfx_vk_select_texture_fb,
    gfx_vk_delete_texture,
    gfx_vk_set_texture_filter,
    gfx_vk_get_texture_filter,
    gfx_vk_set_mipmap_filter,
    gfx_vk_set_anisotropy_level,
    gfx_vk_get_max_anisotropy_level,
    gfx_vk_get_max_msaa_level,
    gfx_vk_post_process,
    gfx_vk_taa_resolve,
    gfx_vk_read_screen_pixels,
    gfx_vk_capture_start,
    gfx_vk_capture_read,
    gfx_vk_capture_drain,
    gfx_vk_capture_stop,
    gfx_vk_occlusion_begin,
    gfx_vk_occlusion_end,
    gfx_vk_occlusion_result,
};

#endif // PD_HAVE_VULKAN
