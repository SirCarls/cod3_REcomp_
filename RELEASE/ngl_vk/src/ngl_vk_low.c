/* NGL on Vulkan, the lower layer (ngl_vk_low.h): for renderers that bring their own shaders. [acct2-waw, 2026-11-07]
 *
 * Written against ngl_vk's backend header (ngl_vk/include/ngl_vk_backend.h, acct2-bro, landed 11-08: ngl_vk_backend(),
 * ngl_vk_set_device_features(), ngl_vk_begin_frame(), ngl_vk_end_frame()). Built with -DNGL_VK_HAVE_BACKEND it
 * uses them; without it the layer reports that it can't start. The block below is the only place that touches
 * ngl_vk. The host that borrows the SDK's device calls ngl_vk_set_device_features() with what the SDK enabled
 * (before ngl_vk_low_init), so the layer gets BC textures and anisotropy.
 *
 * How it works:
 *  - Every object lives in a growable table; a handle is its index + 1.
 *  - Memory: one allocation per buffer or image for now (a sub-allocator comes when the counts call for it).
 *  - Writes into kept buffers and textures go through the frame slot's host-visible staging ring. Their copies are
 *    recorded into the same frame, before its next pass and at its end (and at the first frame for writes made before
 *    it), so a staging ring is only reused once its frame has finished.
 *  - Per frame: a ring of host-visible memory for this frame's vertices, indices and constants, and a descriptor pool;
 *    both are reused when the same frame slot comes around (ngl_vk waits for that frame's fence first).
 *  - One descriptor set layout for every pipeline (the binding convention in ngl_vk_low.h) and one pipeline layout.
 *  - Passes use render pass objects (acct2-bro 20:17: ngl_vk's own device is Vulkan 1.0, and the SDK's device has
 *    dynamic rendering only when the driver does). Render passes are kept by their formats and load operations,
 *    framebuffers by their pass, views and size. Every attachment starts and ends in its attachment layout; the
 *    layer's own barriers move targets in and out (after a pass its targets are left ready to be sampled).
 *  - Device features: only what the backend says is enabled is used (sampler anisotropy, BC textures); until the
 *    backend reports them, neither is used. */
#include "ngl_vk_low.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <vulkan/vulkan.h>

/* ---------------------------------------------------------------------------------------------------------------
 * What ngl_vk lends this layer */

struct low_device {
    VkInstance instance;
    VkPhysicalDevice physical;
    VkDevice device;
    VkQueue queue;
    uint32_t queue_family;
    uint32_t frames_in_flight;
    VkImage main_color, main_depth;
    VkImageView main_color_view, main_depth_view;
    VkFormat main_color_format, main_depth_format;
    uint32_t width, height;
    VkPhysicalDeviceFeatures features;   /* what the device was made with; all off until the backend reports them */
};

#ifdef NGL_VK_HAVE_BACKEND
#include "ngl_vk_backend.h"
static VkImageLayout g_target_layout_at_end = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
static int low_take_device(struct low_device *out) {
    struct ngl_vk_backend b;
    if (ngl_vk_backend(&b) != 0) return -1;
    out->instance = b.instance;
    out->physical = b.gpu;
    out->device = b.device;
    out->queue = b.queue;
    out->queue_family = b.queue_family;
    out->frames_in_flight = b.frames_in_flight ? b.frames_in_flight : 1;
    out->main_color = b.target;
    out->main_depth = b.depth;
    out->main_color_view = b.target_view;
    out->main_depth_view = b.depth_view;
    out->main_color_format = b.target_format;
    out->main_depth_format = b.depth_format;
    out->width = b.width;
    out->height = b.height;
    g_target_layout_at_end = b.target_layout_at_end;
#ifdef NGL_VK_BACKEND_FEATURES
    out->features = b.enabled_features;   /* none on ngl_vk's own device; a borrowed device's as its owner set them */
#else
    memset(&out->features, 0, sizeof(out->features));
#endif
    return 0;
}
/* The frame slot: ngl_vk has one frame in flight today, so the slot is counted here; asked of ngl_vk for when it has
 * more (board, 11-07). */
static VkCommandBuffer low_frame(uint32_t *frame_slot, uint32_t frames_in_flight) {
    static uint32_t frame_count;
    *frame_slot = frames_in_flight ? frame_count++ % frames_in_flight : 0;
    return ngl_vk_begin_frame();
}
static int low_frame_done(void) { return ngl_vk_end_frame(); }
static VkImageLayout low_target_layout_at_end(void) { return g_target_layout_at_end; }
#else
static int low_take_device(struct low_device *out) {
    (void)out;
    fprintf(stderr, "ngl_vk_low: ngl_vk has no backend access yet; the lower layer is off\n");
    return -1;
}
static VkCommandBuffer low_frame(uint32_t *frame_slot, uint32_t frames_in_flight) {
    (void)frames_in_flight;
    *frame_slot = 0;
    return VK_NULL_HANDLE;
}
static int low_frame_done(void) { return -1; }
static VkImageLayout low_target_layout_at_end(void) { return VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL; }
#endif

/* ---------------------------------------------------------------------------------------------------------------
 * Tables */

#define LOW_FRAME_SLOTS_MAX 4
#define LOW_FRAME_ROOM (32u * 1024u * 1024u)   /* per frame slot: vertices, indices and constants */
#define LOW_STAGING_ROOM (64u * 1024u * 1024u) /* per frame slot: writes into kept buffers and textures */
#define LOW_DESCRIPTOR_SETS 16384u             /* per frame slot: one set per draw */
#define LOW_TEXTURE_BINDING 8u
#define LOW_CONSTANT_BLOCK (256u * 16u)        /* 256 float4 registers */
#define LOW_BOOL_LOOP_BLOCK (40u * 4u)         /* 8 bool words, 32 loop words */

struct low_shader { VkShaderModule module; VkShaderStageFlagBits stage; };

struct low_pipeline_key {
    uint32_t vertex_shader, pixel_shader;
    struct ngl_vk_low_layout layout;
    struct ngl_vk_low_state state;
    struct ngl_vk_low_formats formats;
};
struct low_pipeline { struct low_pipeline_key key; VkPipeline pipeline; };

/* A render pass is fixed by its attachments' formats, samples and load operations. Pipelines are made against the
 * one with load operations (any pass with the same formats and samples is compatible with it). */
struct low_pass_key {
    uint32_t color_format[4], color_count, depth_format, samples;
    uint32_t clear_color, clear_depth;
};
struct low_render_pass { struct low_pass_key key; VkRenderPass pass; };

/* A framebuffer: a render pass, its attachments' views (colors, then depth) and the size. */
struct low_framebuffer {
    VkRenderPass pass;
    VkImageView views[5];
    uint32_t view_count, width, height;
    VkFramebuffer framebuffer;
};

struct low_buffer { VkBuffer buffer; VkDeviceMemory memory; size_t size; enum ngl_vk_low_buffer_use use; };

struct low_texture {
    VkImage image;
    VkDeviceMemory memory;
    VkImageView view;
    VkFormat format;
    uint32_t width, height, depth, levels, layers, samples;
    int is_depth, is_target, borrowed;   /* borrowed: ngl_vk's main targets, not freed here */
    VkImageLayout layout;
};

struct low_sampler_key {
    uint32_t filter, mip, address_u, address_v, address_w, compare;
    float anisotropy;
};
struct low_sampler { struct low_sampler_key key; VkSampler sampler; };

/* A host-visible ring: a buffer mapped once, handed out front to back, reset when its frame slot comes around. */
struct low_ring { VkBuffer buffer; VkDeviceMemory memory; uint8_t *mapped; size_t size, used; uint32_t handle; };

struct low_copy {                      /* a write waiting to be recorded (outside a pass, in a frame) */
    uint32_t target;                   /* a buffer handle, or a texture handle when texture is set */
    int texture;
    uint32_t ring;                     /* the frame slot whose staging ring holds the data */
    size_t staging_offset, size, offset;
    uint32_t level, layer, row_pitch;
};

#define LOW_TABLE(type, name) static struct { type *items; uint32_t count, capacity; } name

LOW_TABLE(struct low_shader, g_shaders);
LOW_TABLE(struct low_pipeline, g_pipelines);
LOW_TABLE(struct low_buffer, g_buffers);
LOW_TABLE(struct low_texture, g_textures);
LOW_TABLE(struct low_sampler, g_samplers);
LOW_TABLE(struct low_copy, g_copies);
LOW_TABLE(struct low_render_pass, g_render_passes);
LOW_TABLE(struct low_framebuffer, g_framebuffers);

/* Grows a table by one and returns the new item's index, or -1. */
#define LOW_PUSH(table)                                                                                           \
    ((table).count < (table).capacity || low_grow((void **)&(table).items, &(table).capacity, sizeof(*(table).items)) \
         ? (int)(table).count++                                                                                   \
         : -1)

static int low_grow(void **items, uint32_t *capacity, size_t item_size) {
    const uint32_t wanted = *capacity ? *capacity * 2 : 64;
    void *grown = realloc(*items, (size_t)wanted * item_size);
    if (!grown) return 0;
    memset((uint8_t *)grown + (size_t)*capacity * item_size, 0, (size_t)(wanted - *capacity) * item_size);
    *items = grown;
    *capacity = wanted;
    return 1;
}

/* Things freed while the GPU may still use them: destroyed frames_in_flight frames later. */
struct low_dead {
    uint64_t frame;
    VkBuffer buffer;
    VkImage image;
    VkImageView view;
    VkDeviceMemory memory;
    VkFramebuffer framebuffer;
};
LOW_TABLE(struct low_dead, g_dead);

static struct {
    int ready;
    uint64_t frame_number;
    struct low_device dev;
    VkPhysicalDeviceMemoryProperties memory_types;
    VkDescriptorSetLayout set_layout;
    VkPipelineLayout pipeline_layout;
    uint32_t frame_slot;
    VkCommandBuffer commands;          /* this frame's, from ngl_vk */
    struct low_ring room[LOW_FRAME_SLOTS_MAX];
    struct low_ring staging[LOW_FRAME_SLOTS_MAX];
    VkDescriptorPool descriptors[LOW_FRAME_SLOTS_MAX];
    uint32_t main_color, main_depth;   /* texture handles of ngl_vk's targets */
    uint32_t blank_texture, blank_sampler;
    int in_pass;
    uint32_t pass_color[4], pass_color_count, pass_depth;
    VkPipeline bound_pipeline;
    PFN_vkCmdBeginDebugUtilsLabelEXT label_begin;
    PFN_vkCmdEndDebugUtilsLabelEXT label_end;
    struct ngl_vk_low_counts counts;
} g;

/* ---------------------------------------------------------------------------------------------------------------
 * Helpers */

static int low_memory_type(uint32_t type_bits, VkMemoryPropertyFlags wanted, uint32_t *index) {
    for (uint32_t i = 0; i < g.memory_types.memoryTypeCount; ++i) {
        if ((type_bits & (1u << i)) && (g.memory_types.memoryTypes[i].propertyFlags & wanted) == wanted) {
            *index = i;
            return 1;
        }
    }
    return 0;
}

static int low_make_buffer(VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags memory_flags,
                           VkBuffer *buffer, VkDeviceMemory *memory) {
    VkBufferCreateInfo info = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    info.size = size;
    info.usage = usage;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(g.dev.device, &info, NULL, buffer) != VK_SUCCESS) return 0;
    VkMemoryRequirements needs;
    vkGetBufferMemoryRequirements(g.dev.device, *buffer, &needs);
    VkMemoryAllocateInfo alloc = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    alloc.allocationSize = needs.size;
    if (!low_memory_type(needs.memoryTypeBits, memory_flags, &alloc.memoryTypeIndex) ||
        vkAllocateMemory(g.dev.device, &alloc, NULL, memory) != VK_SUCCESS) {
        vkDestroyBuffer(g.dev.device, *buffer, NULL);
        *buffer = VK_NULL_HANDLE;
        return 0;
    }
    vkBindBufferMemory(g.dev.device, *buffer, *memory, 0);
    return 1;
}

static int low_make_ring(struct low_ring *ring, size_t size, VkBufferUsageFlags usage) {
    if (!low_make_buffer(size, usage, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                         &ring->buffer, &ring->memory))
        return 0;
    if (vkMapMemory(g.dev.device, ring->memory, 0, size, 0, (void **)&ring->mapped) != VK_SUCCESS) return 0;
    ring->size = size;
    ring->used = 0;
    /* The ring is also a buffer handle draws can name. */
    const int index = LOW_PUSH(g_buffers);
    if (index < 0) return 0;
    g_buffers.items[index].buffer = ring->buffer;
    g_buffers.items[index].size = size;
    ring->handle = (uint32_t)index + 1;
    return 1;
}

static void *low_ring_take(struct low_ring *ring, size_t size, size_t alignment, size_t *offset) {
    const size_t start = (ring->used + alignment - 1) & ~(alignment - 1);
    if (start + size > ring->size) return NULL;
    ring->used = start + size;
    *offset = start;
    return ring->mapped + start;
}

static int low_is_depth_format(VkFormat format) {
    return format == VK_FORMAT_D16_UNORM || format == VK_FORMAT_D32_SFLOAT || format == VK_FORMAT_D24_UNORM_S8_UINT ||
           format == VK_FORMAT_D32_SFLOAT_S8_UINT || format == VK_FORMAT_X8_D24_UNORM_PACK32;
}

static int low_has_stencil(VkFormat format) {
    return format == VK_FORMAT_D24_UNORM_S8_UINT || format == VK_FORMAT_D32_SFLOAT_S8_UINT;
}

static VkImageAspectFlags low_aspect(const struct low_texture *t) {
    if (!t->is_depth) return VK_IMAGE_ASPECT_COLOR_BIT;
    return VK_IMAGE_ASPECT_DEPTH_BIT | (low_has_stencil(t->format) ? VK_IMAGE_ASPECT_STENCIL_BIT : 0);
}

/* Moves a texture to a new layout with a full barrier (simple and correct; narrowed later if the counts say so). */
static void low_transition(struct low_texture *t, VkImageLayout layout) {
    if (t->layout == layout || g.commands == VK_NULL_HANDLE) return;
    VkImageMemoryBarrier barrier = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    barrier.oldLayout = t->layout;
    barrier.newLayout = layout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = t->image;
    barrier.subresourceRange.aspectMask = low_aspect(t);
    barrier.subresourceRange.levelCount = VK_REMAINING_MIP_LEVELS;
    barrier.subresourceRange.layerCount = VK_REMAINING_ARRAY_LAYERS;
    vkCmdPipelineBarrier(g.commands, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0,
                         NULL, 0, NULL, 1, &barrier);
    t->layout = layout;
}

static struct low_texture *low_texture_at(uint32_t handle) {
    return handle && handle <= g_textures.count && g_textures.items[handle - 1].image ? &g_textures.items[handle - 1]
                                                                                     : NULL;
}

static struct low_buffer *low_buffer_at(uint32_t handle) {
    return handle && handle <= g_buffers.count && g_buffers.items[handle - 1].buffer ? &g_buffers.items[handle - 1]
                                                                                    : NULL;
}

/* Destroys what was freed at least frames_in_flight frames ago (everything when all is set). */
static void low_free_deferred(int all) {
    uint32_t kept = 0;
    for (uint32_t i = 0; i < g_dead.count; ++i) {
        struct low_dead *d = &g_dead.items[i];
        if (!all && g.frame_number < d->frame + g.dev.frames_in_flight + 1) {
            g_dead.items[kept++] = *d;
            continue;
        }
        if (d->framebuffer) vkDestroyFramebuffer(g.dev.device, d->framebuffer, NULL);
        if (d->view) vkDestroyImageView(g.dev.device, d->view, NULL);
        if (d->image) vkDestroyImage(g.dev.device, d->image, NULL);
        if (d->buffer) vkDestroyBuffer(g.dev.device, d->buffer, NULL);
        if (d->memory) vkFreeMemory(g.dev.device, d->memory, NULL);
    }
    g_dead.count = kept;
}

static void low_free_later(VkBuffer buffer, VkImage image, VkImageView view, VkDeviceMemory memory) {
    const int index = LOW_PUSH(g_dead);
    if (index < 0) return;   /* out of memory: leaked rather than freed while in use */
    struct low_dead *d = &g_dead.items[index];
    d->frame = g.frame_number;
    d->buffer = buffer;
    d->image = image;
    d->view = view;
    d->memory = memory;
}

/* Drops the framebuffers made with this view (they go when their last frame is done). */
static void low_forget_view(VkImageView view) {
    uint32_t kept = 0;
    for (uint32_t i = 0; i < g_framebuffers.count; ++i) {
        struct low_framebuffer *f = &g_framebuffers.items[i];
        int uses = 0;
        for (uint32_t v = 0; v < f->view_count; ++v) uses |= f->views[v] == view;
        if (!uses) {
            g_framebuffers.items[kept++] = *f;
            continue;
        }
        const int index = LOW_PUSH(g_dead);
        if (index >= 0) {
            memset(&g_dead.items[index], 0, sizeof(g_dead.items[index]));
            g_dead.items[index].frame = g.frame_number;
            g_dead.items[index].framebuffer = f->framebuffer;
        }
    }
    g_framebuffers.count = kept;
}

/* The render pass for these formats, samples and load operations, made on first use. */
static VkRenderPass low_render_pass(const struct low_pass_key *key) {
    for (uint32_t i = 0; i < g_render_passes.count; ++i) {
        if (memcmp(&g_render_passes.items[i].key, key, sizeof(*key)) == 0) return g_render_passes.items[i].pass;
    }
    const VkSampleCountFlagBits samples = key->samples > 1 ? (VkSampleCountFlagBits)key->samples : VK_SAMPLE_COUNT_1_BIT;
    VkAttachmentDescription attachments[5];
    VkAttachmentReference colors[4];
    memset(attachments, 0, sizeof(attachments));
    const uint32_t color_count = key->color_count < 4 ? key->color_count : 4;
    for (uint32_t i = 0; i < color_count; ++i) {
        VkAttachmentDescription *a = &attachments[i];
        a->format = (VkFormat)key->color_format[i];
        a->samples = samples;
        a->loadOp = key->clear_color ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
        a->storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        a->stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        a->stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        a->initialLayout = a->finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        colors[i].attachment = i;
        colors[i].layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    }
    VkAttachmentReference depth = {VK_ATTACHMENT_UNUSED, VK_IMAGE_LAYOUT_UNDEFINED};
    uint32_t count = color_count;
    if (key->depth_format) {
        VkAttachmentDescription *a = &attachments[count];
        a->format = (VkFormat)key->depth_format;
        a->samples = samples;
        a->loadOp = key->clear_depth ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
        a->storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        const int stencil = low_has_stencil((VkFormat)key->depth_format);
        a->stencilLoadOp = stencil ? a->loadOp : VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        a->stencilStoreOp = stencil ? VK_ATTACHMENT_STORE_OP_STORE : VK_ATTACHMENT_STORE_OP_DONT_CARE;
        a->initialLayout = a->finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        depth.attachment = count++;
        depth.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    }
    VkSubpassDescription subpass = {0};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = color_count;
    subpass.pColorAttachments = colors;
    subpass.pDepthStencilAttachment = key->depth_format ? &depth : NULL;
    /* The layer's barriers before and after each pass order it against everything else in the frame. */
    VkRenderPassCreateInfo info = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    info.attachmentCount = count;
    info.pAttachments = attachments;
    info.subpassCount = 1;
    info.pSubpasses = &subpass;
    VkRenderPass pass;
    if (vkCreateRenderPass(g.dev.device, &info, NULL, &pass) != VK_SUCCESS) return VK_NULL_HANDLE;
    const int index = LOW_PUSH(g_render_passes);
    if (index < 0) {
        vkDestroyRenderPass(g.dev.device, pass, NULL);
        return VK_NULL_HANDLE;
    }
    g_render_passes.items[index].key = *key;
    g_render_passes.items[index].pass = pass;
    return pass;
}

/* The framebuffer for this pass, these views and this size, made on first use. */
static VkFramebuffer low_framebuffer(VkRenderPass pass, const VkImageView *views, uint32_t view_count, uint32_t width,
                                     uint32_t height) {
    for (uint32_t i = 0; i < g_framebuffers.count; ++i) {
        const struct low_framebuffer *f = &g_framebuffers.items[i];
        if (f->pass == pass && f->view_count == view_count && f->width == width && f->height == height &&
            memcmp(f->views, views, view_count * sizeof(*views)) == 0)
            return f->framebuffer;
    }
    VkFramebufferCreateInfo info = {.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    info.renderPass = pass;
    info.attachmentCount = view_count;
    info.pAttachments = views;
    info.width = width;
    info.height = height;
    info.layers = 1;
    VkFramebuffer framebuffer;
    if (vkCreateFramebuffer(g.dev.device, &info, NULL, &framebuffer) != VK_SUCCESS) return VK_NULL_HANDLE;
    const int index = LOW_PUSH(g_framebuffers);
    if (index < 0) {
        vkDestroyFramebuffer(g.dev.device, framebuffer, NULL);
        return VK_NULL_HANDLE;
    }
    struct low_framebuffer *f = &g_framebuffers.items[index];
    f->pass = pass;
    memcpy(f->views, views, view_count * sizeof(*views));
    f->view_count = view_count;
    f->width = width;
    f->height = height;
    f->framebuffer = framebuffer;
    return framebuffer;
}

/* ---------------------------------------------------------------------------------------------------------------
 * Start and stop */

static uint32_t low_borrow_target(VkImage image, VkImageView view, VkFormat format, int is_depth) {
    const int index = LOW_PUSH(g_textures);
    if (index < 0) return 0;
    struct low_texture *t = &g_textures.items[index];
    t->image = image;
    t->view = view;
    t->format = format;
    t->width = g.dev.width;
    t->height = g.dev.height;
    t->depth = t->levels = t->layers = t->samples = 1;
    t->is_depth = is_depth;
    t->is_target = 1;
    t->borrowed = 1;
    t->layout = is_depth ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    return (uint32_t)index + 1;
}

int ngl_vk_low_init(void) {
    if (g.ready) return 0;
    if (low_take_device(&g.dev) != 0) return -1;
    if (g.dev.frames_in_flight == 0 || g.dev.frames_in_flight > LOW_FRAME_SLOTS_MAX) return -1;
    vkGetPhysicalDeviceMemoryProperties(g.dev.physical, &g.memory_types);

    VkDescriptorSetLayoutBinding bindings[3 + NGL_VK_LOW_MAX_TEXTURES];
    memset(bindings, 0, sizeof(bindings));
    for (uint32_t i = 0; i < 3; ++i) {
        bindings[i].binding = i;
        bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
        bindings[i].descriptorCount = 1;
        bindings[i].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    }
    for (uint32_t i = 0; i < NGL_VK_LOW_MAX_TEXTURES; ++i) {
        bindings[3 + i].binding = LOW_TEXTURE_BINDING + i;
        bindings[3 + i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[3 + i].descriptorCount = 1;
        bindings[3 + i].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    }
    VkDescriptorSetLayoutCreateInfo set_info = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    set_info.bindingCount = 3 + NGL_VK_LOW_MAX_TEXTURES;
    set_info.pBindings = bindings;
    if (vkCreateDescriptorSetLayout(g.dev.device, &set_info, NULL, &g.set_layout) != VK_SUCCESS) return -1;
    VkPipelineLayoutCreateInfo layout_info = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    layout_info.setLayoutCount = 1;
    layout_info.pSetLayouts = &g.set_layout;
    if (vkCreatePipelineLayout(g.dev.device, &layout_info, NULL, &g.pipeline_layout) != VK_SUCCESS) return -1;

    for (uint32_t slot = 0; slot < g.dev.frames_in_flight; ++slot) {
        if (!low_make_ring(&g.room[slot], LOW_FRAME_ROOM,
                           VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                               VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT) ||
            !low_make_ring(&g.staging[slot], LOW_STAGING_ROOM, VK_BUFFER_USAGE_TRANSFER_SRC_BIT))
            return -1;
        VkDescriptorPoolSize sizes[2] = {
            {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 3 * LOW_DESCRIPTOR_SETS},
            {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, NGL_VK_LOW_MAX_TEXTURES * LOW_DESCRIPTOR_SETS},
        };
        VkDescriptorPoolCreateInfo pool_info = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        pool_info.maxSets = LOW_DESCRIPTOR_SETS;
        pool_info.poolSizeCount = 2;
        pool_info.pPoolSizes = sizes;
        if (vkCreateDescriptorPool(g.dev.device, &pool_info, NULL, &g.descriptors[slot]) != VK_SUCCESS) return -1;
    }

    g.main_color = low_borrow_target(g.dev.main_color, g.dev.main_color_view, g.dev.main_color_format, 0);
    g.main_depth = g.dev.main_depth
                       ? low_borrow_target(g.dev.main_depth, g.dev.main_depth_view, g.dev.main_depth_format, 1)
                       : 0;

    g.label_begin = (PFN_vkCmdBeginDebugUtilsLabelEXT)vkGetInstanceProcAddr(g.dev.instance,
                                                                          "vkCmdBeginDebugUtilsLabelEXT");
    g.label_end = (PFN_vkCmdEndDebugUtilsLabelEXT)vkGetInstanceProcAddr(g.dev.instance, "vkCmdEndDebugUtilsLabelEXT");

    g.ready = 1;
    /* A 1x1 white texture and a plain sampler for fetch slots a draw leaves empty. */
    g.blank_texture = ngl_vk_low_texture(NGL_VK_LOW_2D, VK_FORMAT_R8G8B8A8_UNORM, 1, 1, 1, 1);
    const uint32_t white = 0xFFFFFFFFu;
    ngl_vk_low_texture_write(g.blank_texture, 0, 0, &white, sizeof(white), 4);
    g.blank_sampler = ngl_vk_low_sampler(1, 2, 0, 0, 0, 1.0f, 0);
    return g.blank_texture && g.blank_sampler ? 0 : -1;
}

/* Called after ngl_vk has waited for its last frame (the shared queue isn't touched here). */
void ngl_vk_low_shutdown(void) {
    if (!g.ready) return;
    VkDevice d = g.dev.device;
    low_free_deferred(1);
    for (uint32_t i = 0; i < g_pipelines.count; ++i) vkDestroyPipeline(d, g_pipelines.items[i].pipeline, NULL);
    for (uint32_t i = 0; i < g_framebuffers.count; ++i) vkDestroyFramebuffer(d, g_framebuffers.items[i].framebuffer, NULL);
    for (uint32_t i = 0; i < g_render_passes.count; ++i) vkDestroyRenderPass(d, g_render_passes.items[i].pass, NULL);
    for (uint32_t i = 0; i < g_shaders.count; ++i) vkDestroyShaderModule(d, g_shaders.items[i].module, NULL);
    for (uint32_t i = 0; i < g_samplers.count; ++i) vkDestroySampler(d, g_samplers.items[i].sampler, NULL);
    for (uint32_t i = 0; i < g_textures.count; ++i) {
        struct low_texture *t = &g_textures.items[i];
        if (!t->image || t->borrowed) continue;
        vkDestroyImageView(d, t->view, NULL);
        vkDestroyImage(d, t->image, NULL);
        vkFreeMemory(d, t->memory, NULL);
    }
    for (uint32_t i = 0; i < g_buffers.count; ++i) {
        struct low_buffer *b = &g_buffers.items[i];
        if (!b->buffer) continue;
        vkDestroyBuffer(d, b->buffer, NULL);
        if (b->memory) vkFreeMemory(d, b->memory, NULL);
    }
    for (uint32_t slot = 0; slot < g.dev.frames_in_flight; ++slot) {
        /* The rings' buffers were destroyed with the buffer table (they are in it); their memory is freed here. */
        if (g.room[slot].memory) vkFreeMemory(d, g.room[slot].memory, NULL);
        if (g.staging[slot].memory) vkFreeMemory(d, g.staging[slot].memory, NULL);
        if (g.descriptors[slot]) vkDestroyDescriptorPool(d, g.descriptors[slot], NULL);
    }
    vkDestroyPipelineLayout(d, g.pipeline_layout, NULL);
    vkDestroyDescriptorSetLayout(d, g.set_layout, NULL);
    free(g_shaders.items);
    free(g_pipelines.items);
    free(g_buffers.items);
    free(g_textures.items);
    free(g_samplers.items);
    free(g_copies.items);
    free(g_framebuffers.items);
    free(g_render_passes.items);
    memset(&g_framebuffers, 0, sizeof(g_framebuffers));
    memset(&g_render_passes, 0, sizeof(g_render_passes));
    free(g_dead.items);
    memset(&g_dead, 0, sizeof(g_dead));
    memset(&g_shaders, 0, sizeof(g_shaders));
    memset(&g_pipelines, 0, sizeof(g_pipelines));
    memset(&g_buffers, 0, sizeof(g_buffers));
    memset(&g_textures, 0, sizeof(g_textures));
    memset(&g_samplers, 0, sizeof(g_samplers));
    memset(&g_copies, 0, sizeof(g_copies));
    memset(&g, 0, sizeof(g));
}

/* Bytes per texel, or per 4x4 block for the block formats (*block set), of the formats IW3 textures use; 0 when
 * unknown (the rows are then taken as packed). */
static uint32_t low_texel_bytes(VkFormat format, int *block) {
    *block = 0;
    switch (format) {
    case VK_FORMAT_R8_UNORM: return 1;
    case VK_FORMAT_R8G8_UNORM: case VK_FORMAT_R5G6B5_UNORM_PACK16: case VK_FORMAT_B5G6R5_UNORM_PACK16:
    case VK_FORMAT_A1R5G5B5_UNORM_PACK16: case VK_FORMAT_R4G4B4A4_UNORM_PACK16: case VK_FORMAT_R16_SFLOAT: return 2;
    case VK_FORMAT_R8G8B8A8_UNORM: case VK_FORMAT_B8G8R8A8_UNORM: case VK_FORMAT_R8G8B8A8_SRGB:
    case VK_FORMAT_B8G8R8A8_SRGB: case VK_FORMAT_A2B10G10R10_UNORM_PACK32: case VK_FORMAT_R32_SFLOAT:
    case VK_FORMAT_R16G16_SFLOAT: case VK_FORMAT_B10G11R11_UFLOAT_PACK32: return 4;
    case VK_FORMAT_R16G16B16A16_SFLOAT: case VK_FORMAT_R32G32_SFLOAT: return 8;
    case VK_FORMAT_R32G32B32A32_SFLOAT: return 16;
    case VK_FORMAT_BC1_RGB_UNORM_BLOCK: case VK_FORMAT_BC1_RGBA_UNORM_BLOCK: case VK_FORMAT_BC4_UNORM_BLOCK:
        *block = 1; return 8;
    case VK_FORMAT_BC2_UNORM_BLOCK: case VK_FORMAT_BC3_UNORM_BLOCK: case VK_FORMAT_BC5_UNORM_BLOCK:
        *block = 1; return 16;
    default: return 0;
    }
}

/* Records the queued writes' copies into this frame (outside a pass). Returns a mask of the staging rings they used. */
static uint32_t low_flush_copies(void) {
    if (g.commands == VK_NULL_HANDLE || g.in_pass) return 0;
    uint32_t rings = 0;
    for (uint32_t i = 0; i < g_copies.count; ++i) {
        const struct low_copy *c = &g_copies.items[i];
        const VkBuffer staging = g.staging[c->ring].buffer;
        rings |= 1u << c->ring;
        if (!c->texture) {
            struct low_buffer *b = low_buffer_at(c->target);
            if (!b) continue;
            VkBufferCopy region = {c->staging_offset, c->offset, c->size};
            vkCmdCopyBuffer(g.commands, staging, b->buffer, 1, &region);
            continue;
        }
        struct low_texture *t = low_texture_at(c->target);
        if (!t) continue;
        low_transition(t, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        int block;
        const uint32_t texel = low_texel_bytes(t->format, &block);
        VkBufferImageCopy region;
        memset(&region, 0, sizeof(region));
        region.bufferOffset = c->staging_offset;
        region.bufferRowLength = texel && c->row_pitch ? (block ? c->row_pitch / texel * 4 : c->row_pitch / texel) : 0;
        region.imageSubresource.aspectMask = low_aspect(t);
        region.imageSubresource.mipLevel = c->level;
        region.imageSubresource.baseArrayLayer = c->layer;
        region.imageSubresource.layerCount = 1;
        region.imageExtent.width = t->width >> c->level ? t->width >> c->level : 1;
        region.imageExtent.height = t->height >> c->level ? t->height >> c->level : 1;
        region.imageExtent.depth = t->depth >> c->level ? t->depth >> c->level : 1;
        vkCmdCopyBufferToImage(g.commands, staging, t->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
        low_transition(t, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }
    g_copies.count = 0;
    return rings;
}

void ngl_vk_low_frame_begin(void) {
    if (!g.ready) return;
    g.commands = low_frame(&g.frame_slot, g.dev.frames_in_flight);
    if (g.commands == VK_NULL_HANDLE) return;
    ++g.frame_number;
    low_free_deferred(0);
    memset(&g.counts, 0, sizeof(g.counts));
    g.bound_pipeline = VK_NULL_HANDLE;
    /* Writes made before this frame (at start, between frames) are recorded first; their ring is kept this time. */
    const uint32_t kept = low_flush_copies();
    if (!(kept & (1u << g.frame_slot))) g.staging[g.frame_slot].used = 0;
    g.room[g.frame_slot].used = 0;
    vkResetDescriptorPool(g.dev.device, g.descriptors[g.frame_slot], 0);
}

int ngl_vk_low_frame_end(void) {
    if (!g.ready || g.commands == VK_NULL_HANDLE) return -1;
    if (g.in_pass) ngl_vk_low_end_pass();
    low_flush_copies();
    /* ngl_vk shows its target from the layout it asks for. */
    struct low_texture *main_color = low_texture_at(g.main_color);
    if (main_color) low_transition(main_color, low_target_layout_at_end());
    g.commands = VK_NULL_HANDLE;
    const int result = low_frame_done();
    /* ngl_vk's next frame starts its target in the color attachment layout and its depth in the depth attachment
     * layout, both with undefined contents (ngl_vk_backend.h), whatever this frame left them in. */
    if (main_color) main_color->layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    struct low_texture *main_depth = low_texture_at(g.main_depth);
    if (main_depth) main_depth->layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    return result;
}

/* ---------------------------------------------------------------------------------------------------------------
 * Shaders and pipelines */

uint32_t ngl_vk_low_shader(enum ngl_vk_low_stage stage, const uint32_t *spirv, size_t word_count) {
    if (!g.ready || !spirv || !word_count) return 0;
    VkShaderModuleCreateInfo info = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    info.codeSize = word_count * 4;
    info.pCode = spirv;
    VkShaderModule module;
    if (vkCreateShaderModule(g.dev.device, &info, NULL, &module) != VK_SUCCESS) return 0;
    const int index = LOW_PUSH(g_shaders);
    if (index < 0) {
        vkDestroyShaderModule(g.dev.device, module, NULL);
        return 0;
    }
    g_shaders.items[index].module = module;
    g_shaders.items[index].stage = (VkShaderStageFlagBits)stage;
    return (uint32_t)index + 1;
}

uint32_t ngl_vk_low_pipeline(uint32_t vertex_shader, uint32_t pixel_shader, const struct ngl_vk_low_layout *layout,
                             const struct ngl_vk_low_state *state, const struct ngl_vk_low_formats *formats) {
    if (!g.ready || !vertex_shader || vertex_shader > g_shaders.count || !pixel_shader ||
        pixel_shader > g_shaders.count || !layout || !state || !formats)
        return 0;
    struct low_pipeline_key key;
    memset(&key, 0, sizeof(key));   /* padding too, so keys compare with memcmp */
    key.vertex_shader = vertex_shader;
    key.pixel_shader = pixel_shader;
    key.layout = *layout;
    key.state = *state;
    key.formats = *formats;
    for (uint32_t i = 0; i < g_pipelines.count; ++i) {
        if (memcmp(&g_pipelines.items[i].key, &key, sizeof(key)) == 0) return i + 1;
    }

    VkPipelineShaderStageCreateInfo stages[2] = {{.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO},
                                                 {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}};
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = g_shaders.items[vertex_shader - 1].module;
    stages[0].pName = "main";
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = g_shaders.items[pixel_shader - 1].module;
    stages[1].pName = "main";

    VkVertexInputBindingDescription streams[NGL_VK_LOW_MAX_STREAMS];
    VkVertexInputAttributeDescription attributes[NGL_VK_LOW_MAX_ATTRIBUTES];
    const uint32_t stream_count = layout->stream_count < NGL_VK_LOW_MAX_STREAMS ? layout->stream_count
                                                                                : NGL_VK_LOW_MAX_STREAMS;
    const uint32_t attribute_count = layout->attribute_count < NGL_VK_LOW_MAX_ATTRIBUTES ? layout->attribute_count
                                                                                         : NGL_VK_LOW_MAX_ATTRIBUTES;
    for (uint32_t i = 0; i < stream_count; ++i) {
        streams[i].binding = i;
        streams[i].stride = layout->stride[i];
        streams[i].inputRate = layout->per_instance[i] ? VK_VERTEX_INPUT_RATE_INSTANCE : VK_VERTEX_INPUT_RATE_VERTEX;
    }
    for (uint32_t i = 0; i < attribute_count; ++i) {
        attributes[i].location = layout->attributes[i].location;
        attributes[i].binding = layout->attributes[i].stream;
        attributes[i].format = (VkFormat)layout->attributes[i].format;
        attributes[i].offset = layout->attributes[i].offset;
    }
    VkPipelineVertexInputStateCreateInfo vertex_input = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vertex_input.vertexBindingDescriptionCount = stream_count;
    vertex_input.pVertexBindingDescriptions = streams;
    vertex_input.vertexAttributeDescriptionCount = attribute_count;
    vertex_input.pVertexAttributeDescriptions = attributes;

    static const VkPrimitiveTopology topologies[4] = {VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
                                                      VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP,
                                                      VK_PRIMITIVE_TOPOLOGY_LINE_LIST, VK_PRIMITIVE_TOPOLOGY_POINT_LIST};
    VkPipelineInputAssemblyStateCreateInfo assembly = {.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    assembly.topology = topologies[(uint32_t)state->topology < 4 ? (uint32_t)state->topology : 0];

    VkPipelineViewportStateCreateInfo viewport = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    viewport.viewportCount = 1;
    viewport.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo raster = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.cullMode = (VkCullModeFlags)state->cull;
    raster.frontFace = VK_FRONT_FACE_CLOCKWISE;   /* Direct3D's */
    raster.depthBiasEnable = state->depth_bias != 0.0f || state->depth_bias_slope != 0.0f;
    raster.depthBiasConstantFactor = state->depth_bias;
    raster.depthBiasSlopeFactor = state->depth_bias_slope;
    raster.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo multisample = {.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    multisample.rasterizationSamples = formats->samples > 1 ? (VkSampleCountFlagBits)formats->samples
                                                            : VK_SAMPLE_COUNT_1_BIT;
    multisample.alphaToCoverageEnable = state->alpha_to_coverage ? VK_TRUE : VK_FALSE;

    VkPipelineDepthStencilStateCreateInfo depth = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    depth.depthTestEnable = state->depth_test ? VK_TRUE : VK_FALSE;
    depth.depthWriteEnable = state->depth_write ? VK_TRUE : VK_FALSE;
    depth.depthCompareOp = state->depth_test ? (VkCompareOp)state->depth_compare : VK_COMPARE_OP_ALWAYS;
    depth.stencilTestEnable = state->stencil ? VK_TRUE : VK_FALSE;
    depth.front.failOp = (VkStencilOp)state->stencil_fail;
    depth.front.depthFailOp = (VkStencilOp)state->stencil_depth_fail;
    depth.front.passOp = (VkStencilOp)state->stencil_pass;
    depth.front.compareOp = (VkCompareOp)state->stencil_compare;
    depth.front.compareMask = state->stencil_read_mask;
    depth.front.writeMask = state->stencil_write_mask;
    depth.front.reference = state->stencil_reference;
    depth.back = depth.front;

    VkPipelineColorBlendAttachmentState blends[4];
    memset(blends, 0, sizeof(blends));
    const uint32_t color_count = formats->color_count < 4 ? formats->color_count : 4;
    for (uint32_t i = 0; i < color_count; ++i) {
        VkPipelineColorBlendAttachmentState *b = &blends[i];
        b->blendEnable = state->blend ? VK_TRUE : VK_FALSE;
        b->srcColorBlendFactor = (VkBlendFactor)state->color_src;
        b->dstColorBlendFactor = (VkBlendFactor)state->color_dst;
        b->colorBlendOp = (VkBlendOp)state->color_op;
        b->srcAlphaBlendFactor = (VkBlendFactor)state->alpha_src;
        b->dstAlphaBlendFactor = (VkBlendFactor)state->alpha_dst;
        b->alphaBlendOp = (VkBlendOp)state->alpha_op;
        b->colorWriteMask = state->no_color_write ? 0 : (state->color_mask ? state->color_mask : 0xF);
    }
    VkPipelineColorBlendStateCreateInfo blend = {.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    blend.attachmentCount = color_count;
    blend.pAttachments = blends;

    const VkDynamicState dynamic_states[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dynamic.dynamicStateCount = 2;
    dynamic.pDynamicStates = dynamic_states;

    struct low_pass_key pass_key;
    memset(&pass_key, 0, sizeof(pass_key));
    for (uint32_t i = 0; i < color_count; ++i) pass_key.color_format[i] = formats->color[i];
    pass_key.color_count = color_count;
    pass_key.depth_format = formats->depth_format;
    pass_key.samples = formats->samples > 1 ? formats->samples : 1;
    const VkRenderPass compatible = low_render_pass(&pass_key);   /* load operations: any pass with these formats */
    if (compatible == VK_NULL_HANDLE) return 0;

    VkGraphicsPipelineCreateInfo info = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    info.renderPass = compatible;
    info.subpass = 0;
    info.stageCount = 2;
    info.pStages = stages;
    info.pVertexInputState = &vertex_input;
    info.pInputAssemblyState = &assembly;
    info.pViewportState = &viewport;
    info.pRasterizationState = &raster;
    info.pMultisampleState = &multisample;
    info.pDepthStencilState = &depth;
    info.pColorBlendState = &blend;
    info.pDynamicState = &dynamic;
    info.layout = g.pipeline_layout;
    VkPipeline pipeline;
    if (vkCreateGraphicsPipelines(g.dev.device, VK_NULL_HANDLE, 1, &info, NULL, &pipeline) != VK_SUCCESS) return 0;
    const int index = LOW_PUSH(g_pipelines);
    if (index < 0) {
        vkDestroyPipeline(g.dev.device, pipeline, NULL);
        return 0;
    }
    g_pipelines.items[index].key = key;
    g_pipelines.items[index].pipeline = pipeline;
    ++g.counts.pipelines_made;
    return (uint32_t)index + 1;
}

/* ---------------------------------------------------------------------------------------------------------------
 * Buffers */

uint32_t ngl_vk_low_buffer(enum ngl_vk_low_buffer_use use, size_t size) {
    if (!g.ready || !size) return 0;
    VkBufferUsageFlags usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                               (use == NGL_VK_LOW_INDICES ? VK_BUFFER_USAGE_INDEX_BUFFER_BIT
                                                          : VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
    VkBuffer buffer;
    VkDeviceMemory memory;
    if (!low_make_buffer(size, usage, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &buffer, &memory)) return 0;
    const int index = LOW_PUSH(g_buffers);
    if (index < 0) {
        vkDestroyBuffer(g.dev.device, buffer, NULL);
        vkFreeMemory(g.dev.device, memory, NULL);
        return 0;
    }
    g_buffers.items[index].buffer = buffer;
    g_buffers.items[index].memory = memory;
    g_buffers.items[index].size = size;
    g_buffers.items[index].use = use;
    return (uint32_t)index + 1;
}

int ngl_vk_low_buffer_write(uint32_t buffer, size_t offset, const void *data, size_t size) {
    struct low_buffer *b = low_buffer_at(buffer);
    if (!g.ready || !b || !b->memory || !data || offset + size > b->size) return -1;
    size_t staging_offset;
    void *room = low_ring_take(&g.staging[g.frame_slot], size, 16, &staging_offset);
    const int index = room ? LOW_PUSH(g_copies) : -1;
    if (index < 0) return -1;
    memcpy(room, data, size);
    struct low_copy *c = &g_copies.items[index];
    memset(c, 0, sizeof(*c));
    c->target = buffer;
    c->ring = g.frame_slot;
    c->staging_offset = staging_offset;
    c->size = size;
    c->offset = offset;
    return 0;
}

void ngl_vk_low_buffer_free(uint32_t buffer) {
    struct low_buffer *b = low_buffer_at(buffer);
    if (!g.ready || !b || !b->memory) return;   /* the rings aren't freed this way */
    low_free_later(b->buffer, VK_NULL_HANDLE, VK_NULL_HANDLE, b->memory);
    memset(b, 0, sizeof(*b));
}

void *ngl_vk_low_frame_room(enum ngl_vk_low_buffer_use use, size_t size, uint32_t *buffer, size_t *offset) {
    (void)use;
    if (!g.ready || !buffer || !offset) return NULL;
    void *room = low_ring_take(&g.room[g.frame_slot], size, 16, offset);
    if (!room) return NULL;
    *buffer = g.room[g.frame_slot].handle;
    g.counts.frame_room_used = g.room[g.frame_slot].used;
    return room;
}

/* ---------------------------------------------------------------------------------------------------------------
 * Textures, samplers and targets */

static uint32_t low_make_texture(VkImageType type, VkImageViewType view_type, VkFormat format, uint32_t width,
                                 uint32_t height, uint32_t depth, uint32_t levels, uint32_t layers, uint32_t samples,
                                 int is_target, int is_depth, VkImageCreateFlags flags) {
    VkImageCreateInfo info = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    info.flags = flags;
    info.imageType = type;
    info.format = format;
    info.extent.width = width ? width : 1;
    info.extent.height = height ? height : 1;
    info.extent.depth = depth ? depth : 1;
    info.mipLevels = levels ? levels : 1;
    info.arrayLayers = layers ? layers : 1;
    info.samples = samples > 1 ? (VkSampleCountFlagBits)samples : VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    if (is_target)
        info.usage |= is_depth ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImage image;
    if (vkCreateImage(g.dev.device, &info, NULL, &image) != VK_SUCCESS) return 0;
    VkMemoryRequirements needs;
    vkGetImageMemoryRequirements(g.dev.device, image, &needs);
    VkMemoryAllocateInfo alloc = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    alloc.allocationSize = needs.size;
    VkDeviceMemory memory;
    if (!low_memory_type(needs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &alloc.memoryTypeIndex) ||
        vkAllocateMemory(g.dev.device, &alloc, NULL, &memory) != VK_SUCCESS) {
        vkDestroyImage(g.dev.device, image, NULL);
        return 0;
    }
    vkBindImageMemory(g.dev.device, image, memory, 0);

    const int index = LOW_PUSH(g_textures);
    if (index < 0) {
        vkDestroyImage(g.dev.device, image, NULL);
        vkFreeMemory(g.dev.device, memory, NULL);
        return 0;
    }
    struct low_texture *t = &g_textures.items[index];
    t->image = image;
    t->memory = memory;
    t->format = format;
    t->width = info.extent.width;
    t->height = info.extent.height;
    t->depth = info.extent.depth;
    t->levels = info.mipLevels;
    t->layers = info.arrayLayers;
    t->samples = samples > 1 ? samples : 1;
    t->is_target = is_target;
    t->is_depth = is_depth;
    t->layout = VK_IMAGE_LAYOUT_UNDEFINED;

    VkImageViewCreateInfo view = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    view.image = image;
    view.viewType = view_type;
    view.format = format;
    view.subresourceRange.aspectMask = is_depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
    view.subresourceRange.levelCount = info.mipLevels;
    view.subresourceRange.layerCount = info.arrayLayers;
    if (vkCreateImageView(g.dev.device, &view, NULL, &t->view) != VK_SUCCESS) {
        vkDestroyImage(g.dev.device, image, NULL);
        vkFreeMemory(g.dev.device, memory, NULL);
        memset(t, 0, sizeof(*t));
        return 0;
    }
    return (uint32_t)index + 1;
}

uint32_t ngl_vk_low_texture(enum ngl_vk_low_texture_kind kind, uint32_t format, uint32_t width, uint32_t height,
                            uint32_t depth, uint32_t level_count) {
    if (!g.ready) return 0;
    /* BC textures (DXT1-5, the IW3 games' usual formats) only when the device was made with them: 0 tells the caller
     * to decompress first. */
    const int block_compressed = format >= VK_FORMAT_BC1_RGB_UNORM_BLOCK && format <= VK_FORMAT_BC7_SRGB_BLOCK;
    if (block_compressed && !g.dev.features.textureCompressionBC) return 0;
    switch (kind) {
    case NGL_VK_LOW_3D:
        return low_make_texture(VK_IMAGE_TYPE_3D, VK_IMAGE_VIEW_TYPE_3D, (VkFormat)format, width, height, depth,
                                level_count, 1, 1, 0, 0, 0);
    case NGL_VK_LOW_CUBE:
        return low_make_texture(VK_IMAGE_TYPE_2D, VK_IMAGE_VIEW_TYPE_CUBE, (VkFormat)format, width, height, 1,
                                level_count, 6, 1, 0, 0, VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT);
    default:
        return low_make_texture(VK_IMAGE_TYPE_2D, VK_IMAGE_VIEW_TYPE_2D, (VkFormat)format, width, height, 1,
                                level_count, 1, 1, 0, 0, 0);
    }
}

int ngl_vk_low_texture_write(uint32_t texture, uint32_t level, uint32_t layer, const void *data, size_t size,
                             uint32_t row_pitch) {
    struct low_texture *t = low_texture_at(texture);
    if (!g.ready || !t || t->borrowed || !data || level >= t->levels || layer >= t->layers) return -1;
    size_t staging_offset;
    void *room = low_ring_take(&g.staging[g.frame_slot], size, 16, &staging_offset);
    const int index = room ? LOW_PUSH(g_copies) : -1;
    if (index < 0) return -1;
    memcpy(room, data, size);
    struct low_copy *c = &g_copies.items[index];
    memset(c, 0, sizeof(*c));
    c->target = texture;
    c->texture = 1;
    c->ring = g.frame_slot;
    c->staging_offset = staging_offset;
    c->size = size;
    c->level = level;
    c->layer = layer;
    c->row_pitch = row_pitch;
    return 0;
}

void ngl_vk_low_texture_free(uint32_t texture) {
    struct low_texture *t = low_texture_at(texture);
    if (!g.ready || !t || t->borrowed) return;
    low_forget_view(t->view);
    low_free_later(VK_NULL_HANDLE, t->image, t->view, t->memory);
    memset(t, 0, sizeof(*t));
}

uint32_t ngl_vk_low_sampler(uint32_t filter, uint32_t mip, uint32_t address_u, uint32_t address_v, uint32_t address_w,
                            float anisotropy, uint32_t compare) {
    if (!g.ready) return 0;
    struct low_sampler_key key;
    memset(&key, 0, sizeof(key));
    key.filter = filter;
    key.mip = mip;
    key.address_u = address_u;
    key.address_v = address_v;
    key.address_w = address_w;
    key.compare = compare;
    key.anisotropy = anisotropy;
    for (uint32_t i = 0; i < g_samplers.count; ++i) {
        if (memcmp(&g_samplers.items[i].key, &key, sizeof(key)) == 0) return i + 1;
    }
    VkSamplerCreateInfo info = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    info.magFilter = info.minFilter = filter ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    info.mipmapMode = mip == 1 ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
    info.addressModeU = (VkSamplerAddressMode)address_u;
    info.addressModeV = (VkSamplerAddressMode)address_v;
    info.addressModeW = (VkSamplerAddressMode)address_w;
    const int anisotropic = anisotropy > 1.0f && g.dev.features.samplerAnisotropy;   /* only when the device has it */
    info.anisotropyEnable = anisotropic ? VK_TRUE : VK_FALSE;
    info.maxAnisotropy = anisotropic ? anisotropy : 1.0f;
    info.compareEnable = compare ? VK_TRUE : VK_FALSE;
    info.compareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
    info.maxLod = mip == 2 ? 0.0f : VK_LOD_CLAMP_NONE;
    info.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
    VkSampler sampler;
    if (vkCreateSampler(g.dev.device, &info, NULL, &sampler) != VK_SUCCESS) return 0;
    const int index = LOW_PUSH(g_samplers);
    if (index < 0) {
        vkDestroySampler(g.dev.device, sampler, NULL);
        return 0;
    }
    g_samplers.items[index].key = key;
    g_samplers.items[index].sampler = sampler;
    return (uint32_t)index + 1;
}

uint32_t ngl_vk_low_target(uint32_t format, uint32_t width, uint32_t height, uint32_t samples, uint32_t depth) {
    if (!g.ready) return 0;
    const int is_depth = depth || low_is_depth_format((VkFormat)format);
    return low_make_texture(VK_IMAGE_TYPE_2D, VK_IMAGE_VIEW_TYPE_2D, (VkFormat)format, width, height, 1, 1, 1, samples,
                            1, is_depth, 0);
}

uint32_t ngl_vk_low_main_color(void) { return g.main_color; }
uint32_t ngl_vk_low_main_depth(void) { return g.main_depth; }

/* ---------------------------------------------------------------------------------------------------------------
 * Passes */

int ngl_vk_low_begin_pass(const struct ngl_vk_low_pass *pass) {
    if (!g.ready || g.commands == VK_NULL_HANDLE || !pass || g.in_pass) return -1;
    low_flush_copies();
    struct low_pass_key key;
    memset(&key, 0, sizeof(key));
    VkImageView views[5];
    VkClearValue clears[5];
    memset(clears, 0, sizeof(clears));
    const uint32_t color_count = pass->color_count < 4 ? pass->color_count : 4;
    struct low_texture *first = NULL;
    for (uint32_t i = 0; i < color_count; ++i) {
        struct low_texture *t = low_texture_at(pass->color[i]);
        if (!t || t->is_depth) return -1;
        if (!first) first = t;
        low_transition(t, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        key.color_format[i] = (uint32_t)t->format;
        views[i] = t->view;
        memcpy(clears[i].color.float32, pass->clear_rgba, sizeof(pass->clear_rgba));
        g.pass_color[i] = pass->color[i];
    }
    key.color_count = color_count;
    key.clear_color = pass->clear_color ? 1 : 0;
    uint32_t view_count = color_count;
    struct low_texture *depth_target = low_texture_at(pass->depth);
    if (pass->depth && (!depth_target || !depth_target->is_depth)) return -1;
    if (depth_target) {
        if (!first) first = depth_target;
        low_transition(depth_target, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
        key.depth_format = (uint32_t)depth_target->format;
        key.clear_depth = pass->clear_depth ? 1 : 0;
        views[view_count] = depth_target->view;
        clears[view_count].depthStencil.depth = pass->clear_depth_value;
        clears[view_count].depthStencil.stencil = pass->clear_stencil_value;
        ++view_count;
    }
    if (!first) return -1;
    key.samples = first->samples > 1 ? first->samples : 1;
    const VkRenderPass render_pass = low_render_pass(&key);
    const VkFramebuffer framebuffer = render_pass != VK_NULL_HANDLE
                                          ? low_framebuffer(render_pass, views, view_count, first->width, first->height)
                                          : VK_NULL_HANDLE;
    if (framebuffer == VK_NULL_HANDLE) return -1;

    if (pass->label) ngl_vk_low_label_begin(pass->label);
    VkRenderPassBeginInfo info = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    info.renderPass = render_pass;
    info.framebuffer = framebuffer;
    info.renderArea.extent.width = first->width;
    info.renderArea.extent.height = first->height;
    info.clearValueCount = view_count;
    info.pClearValues = clears;
    vkCmdBeginRenderPass(g.commands, &info, VK_SUBPASS_CONTENTS_INLINE);
    g.in_pass = pass->label ? 2 : 1;   /* 2: a label to end with the pass */
    g.pass_color_count = color_count;
    g.pass_depth = pass->depth;
    ngl_vk_low_viewport(0, 0, (float)first->width, (float)first->height, 0.0f, 1.0f);
    ngl_vk_low_scissor(0, 0, first->width, first->height);
    ++g.counts.passes;
    return 0;
}

void ngl_vk_low_end_pass(void) {
    if (!g.in_pass) return;
    vkCmdEndRenderPass(g.commands);
    if (g.in_pass == 2) ngl_vk_low_label_end();
    g.in_pass = 0;
    /* Leave the pass's own targets ready to be sampled; ngl_vk's main targets stay as attachments. */
    for (uint32_t i = 0; i < g.pass_color_count; ++i) {
        struct low_texture *t = low_texture_at(g.pass_color[i]);
        if (t && !t->borrowed) low_transition(t, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }
    struct low_texture *d = low_texture_at(g.pass_depth);
    if (d && !d->borrowed) low_transition(d, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL);
}

void ngl_vk_low_viewport(float x, float y, float width, float height, float min_depth, float max_depth) {
    if (!g.in_pass) return;
    const VkViewport v = {x, y, width, height, min_depth, max_depth};
    vkCmdSetViewport(g.commands, 0, 1, &v);
}

void ngl_vk_low_scissor(int32_t x, int32_t y, uint32_t width, uint32_t height) {
    if (!g.in_pass) return;
    const VkRect2D r = {{x, y}, {width, height}};
    vkCmdSetScissor(g.commands, 0, 1, &r);
}

int ngl_vk_low_copy(uint32_t target, int32_t from_x, int32_t from_y, uint32_t width, uint32_t height, uint32_t texture,
                    int32_t to_x, int32_t to_y) {
    struct low_texture *from = low_texture_at(target);
    struct low_texture *to = low_texture_at(texture);
    if (!g.ready || g.in_pass || g.commands == VK_NULL_HANDLE || !from || !to || from == to) return -1;
    const VkImageLayout from_layout = from->layout;
    low_transition(from, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    low_transition(to, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkImageSubresourceLayers from_sub = {low_aspect(from), 0, 0, 1};
    VkImageSubresourceLayers to_sub = {low_aspect(to), 0, 0, 1};
    if (from->samples > 1) {
        VkImageResolve region = {from_sub, {from_x, from_y, 0}, to_sub, {to_x, to_y, 0}, {width, height, 1}};
        vkCmdResolveImage(g.commands, from->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, to->image,
                          VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    } else {
        VkImageCopy region = {from_sub, {from_x, from_y, 0}, to_sub, {to_x, to_y, 0}, {width, height, 1}};
        vkCmdCopyImage(g.commands, from->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, to->image,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    }
    low_transition(to, to->is_depth ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL
                                    : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    low_transition(from, from->borrowed ? from_layout : (from->is_depth ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL
                                                                         : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL));
    return 0;
}

/* ---------------------------------------------------------------------------------------------------------------
 * Draws */

/* Copies count float4s (or count words when words is set) into this frame's room for a constant block of block
 * bytes; returns its offset in *offset (zeros past count). */
static int low_constants(const void *data, size_t bytes, size_t block, uint32_t *offset) {
    size_t at;
    uint8_t *room = low_ring_take(&g.room[g.frame_slot], block, 256, &at);   /* 256: the usual UBO offset alignment */
    if (!room) return 0;
    if (bytes > block) bytes = block;
    if (data && bytes) memcpy(room, data, bytes);
    memset(room + bytes, 0, block - bytes);
    *offset = (uint32_t)at;
    return 1;
}

int ngl_vk_low_draw(const struct ngl_vk_low_draw *draw) {
    if (!g.ready || !g.in_pass || !draw || !draw->pipeline || draw->pipeline > g_pipelines.count) return -1;
    const VkPipeline pipeline = g_pipelines.items[draw->pipeline - 1].pipeline;
    if (pipeline != g.bound_pipeline) {
        vkCmdBindPipeline(g.commands, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
        g.bound_pipeline = pipeline;
        ++g.counts.pipeline_binds;
    }

    /* Constants: three blocks in this frame's room, named by dynamic offsets. */
    uint32_t offsets[3];
    if (!low_constants(draw->vertex_constants, (size_t)draw->vertex_constant_count * 16, LOW_CONSTANT_BLOCK,
                       &offsets[0]) ||
        !low_constants(draw->pixel_constants, (size_t)draw->pixel_constant_count * 16, LOW_CONSTANT_BLOCK,
                       &offsets[1]) ||
        !low_constants(draw->bool_loop_constants, draw->bool_loop_constants ? LOW_BOOL_LOOP_BLOCK : 0,
                       LOW_BOOL_LOOP_BLOCK, &offsets[2]))
        return -1;

    VkDescriptorSetAllocateInfo alloc = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    alloc.descriptorPool = g.descriptors[g.frame_slot];
    alloc.descriptorSetCount = 1;
    alloc.pSetLayouts = &g.set_layout;
    VkDescriptorSet set;
    if (vkAllocateDescriptorSets(g.dev.device, &alloc, &set) != VK_SUCCESS) return -1;

    VkDescriptorBufferInfo blocks[3] = {
        {g.room[g.frame_slot].buffer, 0, LOW_CONSTANT_BLOCK},
        {g.room[g.frame_slot].buffer, 0, LOW_CONSTANT_BLOCK},
        {g.room[g.frame_slot].buffer, 0, LOW_BOOL_LOOP_BLOCK},
    };
    VkDescriptorImageInfo images[NGL_VK_LOW_MAX_TEXTURES];
    VkWriteDescriptorSet writes[3 + NGL_VK_LOW_MAX_TEXTURES];
    memset(writes, 0, sizeof(writes));
    for (uint32_t i = 0; i < 3; ++i) {
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = set;
        writes[i].dstBinding = i;
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
        writes[i].pBufferInfo = &blocks[i];
    }
    for (uint32_t i = 0; i < NGL_VK_LOW_MAX_TEXTURES; ++i) {
        struct low_texture *t = low_texture_at(draw->texture[i]);
        if (!t || t->layout == VK_IMAGE_LAYOUT_UNDEFINED) t = low_texture_at(g.blank_texture);
        const uint32_t sampler = draw->sampler[i] && draw->sampler[i] <= g_samplers.count ? draw->sampler[i]
                                                                                           : g.blank_sampler;
        images[i].sampler = g_samplers.items[sampler - 1].sampler;
        images[i].imageView = t->view;
        images[i].imageLayout = t->layout;
        writes[3 + i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[3 + i].dstSet = set;
        writes[3 + i].dstBinding = LOW_TEXTURE_BINDING + i;
        writes[3 + i].descriptorCount = 1;
        writes[3 + i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[3 + i].pImageInfo = &images[i];
    }
    vkUpdateDescriptorSets(g.dev.device, 3 + NGL_VK_LOW_MAX_TEXTURES, writes, 0, NULL);
    vkCmdBindDescriptorSets(g.commands, VK_PIPELINE_BIND_POINT_GRAPHICS, g.pipeline_layout, 0, 1, &set, 3, offsets);

    VkBuffer streams[NGL_VK_LOW_MAX_STREAMS];
    VkDeviceSize stream_offsets[NGL_VK_LOW_MAX_STREAMS];
    uint32_t stream_count = 0;
    for (uint32_t i = 0; i < NGL_VK_LOW_MAX_STREAMS && draw->vertex_buffer[i]; ++i) {
        struct low_buffer *b = low_buffer_at(draw->vertex_buffer[i]);
        if (!b) return -1;
        streams[i] = b->buffer;
        stream_offsets[i] = draw->vertex_offset[i];
        stream_count = i + 1;
    }
    if (stream_count) vkCmdBindVertexBuffers(g.commands, 0, stream_count, streams, stream_offsets);

    const uint32_t instances = draw->instance_count ? draw->instance_count : 1;
    if (draw->index_buffer) {
        struct low_buffer *b = low_buffer_at(draw->index_buffer);
        if (!b) return -1;
        vkCmdBindIndexBuffer(g.commands, b->buffer, draw->index_offset,
                             draw->index_32bit ? VK_INDEX_TYPE_UINT32 : VK_INDEX_TYPE_UINT16);
        vkCmdDrawIndexed(g.commands, draw->index_count, instances, draw->first_index, draw->base_vertex, 0);
        g.counts.indices += draw->index_count;
    } else {
        vkCmdDraw(g.commands, draw->vertex_count, instances, 0, 0);
    }
    ++g.counts.draws;
    return 0;
}

/* ---------------------------------------------------------------------------------------------------------------
 * Labels and counts */

void ngl_vk_low_label_begin(const char *name) {
    if (!g.ready || g.commands == VK_NULL_HANDLE || !g.label_begin || !name) return;
    VkDebugUtilsLabelEXT label = {.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT};
    label.pLabelName = name;
    g.label_begin(g.commands, &label);
}

void ngl_vk_low_label_end(void) {
    if (!g.ready || g.commands == VK_NULL_HANDLE || !g.label_end) return;
    g.label_end(g.commands);
}

void ngl_vk_low_frame_counts(struct ngl_vk_low_counts *counts) {
    if (counts) *counts = g.counts;
}
