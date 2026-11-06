/* NGL on Vulkan: the presenter (see ../include/ngl_vk.h).
 *
 * One render pass draws a picture into the target: the picture is copied into a sampled image, one triangle covering
 * a viewport scaled to the picture's shape samples it, and the target is copied into a buffer the host can read.
 * Every draw waits for the GPU to finish, which is simple and fast enough for one 640 x 480 picture a frame; the
 * renderer proper will keep frames in flight.
 *
 * 2D quads (ngl_vk_draw_quads) go through the same pass with a second pipeline: triangles in the game's screen pixels,
 * alpha-blended in the order given, each its texture (or the frame's picture) times its color, or its color alone.
 * Textures (ngl_vk_texture) are made once, each with its own descriptor set; the draw binds each run of triangles'
 * set in turn.
 *
 * 3D meshes (ngl_vk_mesh, ngl_vk_set_meshes) are drawn in the same pass before the quads, with a depth buffer: each
 * mesh's corners and triangles are copied once into two large host-visible buffers, and each draw pushes its world
 * and view-projection matrices (Direct3D's, row by row, which a GLSL mat4 reads as the transpose, so the shader's
 * matrix * vector is Direct3D's vector * matrix). A mesh is drawn with its base texture (its descriptor set as set 0;
 * the coordinates wrapped in the shader, since the sampler clamps for the quads) or in a color its tint picks, and
 * lit by its lightmap (set 1), its corners' colors, or one fixed light; the game's own shaders come later.
 *
 * The sun's shadow (ngl_vk_set_sun_shadow): before the main pass, a depth-only pass draws the frame's shadow casters
 * into a 2048 x 2048 map through the light's view-projection; mesh draws read it through set 3 (the map with a
 * comparing sampler, and a uniform buffer taking the camera's clip space to the light's), and the shadowed ones dim
 * their first light where a caster is nearer the sun.
 *
 * With a window (ngl_vk_set_window), the instance gets the window system's extensions, the GPU's queue must be able to
 * show on the window's surface, and the device gets VK_KHR_swapchain. Each draw then acquires a swapchain image before
 * recording, and after the target is copied out for the host it is blitted into that image, scaled to fit and centered
 * on black; the submit waits for the image and signals that image's own semaphore, which the present waits for. The
 * draw still waits for the GPU, so one acquire semaphore is enough. A window that changes size (or a present that says
 * the swapchain is out of date) makes the swapchain again at the next draw. */
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>

#include "ngl_vk.h"

/* SPIR-V from shaders/, compiled when the library is built */
static const uint32_t quad_vert_spirv[] =
#include "quad.vert.inc"
;
static const uint32_t quad_frag_spirv[] =
#include "quad.frag.inc"
;
static const uint32_t quad2d_vert_spirv[] =
#include "quad2d.vert.inc"
;
static const uint32_t quad2d_frag_spirv[] =
#include "quad2d.frag.inc"
;
static const uint32_t mesh_vert_spirv[] =
#include "mesh.vert.inc"
;
static const uint32_t mesh_frag_spirv[] =
#include "mesh.frag.inc"
;
static const uint32_t grass_vert_spirv[] =
#include "grass.vert.inc"
;
static const uint32_t grass_frag_spirv[] =
#include "grass.frag.inc"
;
static const uint32_t shadow_frag_spirv[] =
#include "shadow.frag.inc"
;

#define TARGET_FORMAT   VK_FORMAT_R8G8B8A8_UNORM
#define DEPTH_FORMAT    VK_FORMAT_D32_SFLOAT
#define PICTURE_FORMAT  VK_FORMAT_B8G8R8A8_UNORM     /* X8R8G8B8 in memory: blue, green, red, unused */
#define BYTES_PER_PIXEL 4u
#define WAIT_NS         5000000000ull               /* 5 s: a draw that takes longer has hung */
#define MAX_SWAP_IMAGES 8u                          /* a window's swapchain images */
#define MAX_VERTICES    (8192u * 6u)                /* 8192 quads a draw */
#define MAX_TEXTURES    2048u                       /* textures made with ngl_vk_texture */
#define MAX_TEXTURE_LEVELS 14u                      /* a texture's levels: 8192 x 8192 down to 1 x 1 */
#define MAX_MESHES      65536u                      /* meshes made with ngl_vk_mesh */
#define MESH_CORNERS    (4u << 20)                  /* corners all meshes share: 128 MB */
#define FRAME_CORNERS   (512u << 10)                /* of those, the last ones are this frame's (skinned meshes) */
#define CORNER_FLOATS   NGL_VK_CORNER_FLOATS        /* x, y, z, u, v, lightmap u, v, color, normal x, y, z */
#define MESH_TEXTURED   0x40000000u                 /* in a mesh draw's instance number: it samples its texture */
#define MESH_SHADING_SHIFT 28u                      /* and its shading, 2 bits */
#define MESH_ALPHA_TEST 0x80000000u                 /* and whether it is alpha-tested */
#define MESH_LIGHT_ONCE 0x08000000u                 /* and whether its lightmap or colors multiply once */
#define MESH_TINT       0x07FFFFFFu                 /* and its tint, the rest */
/* What a mesh draw's pixels add to its shading, bits 8-15 of its push constants' flags (mesh.frag's MODE_*) */
#define MESH_MODE_MIX       1u      /* the base texture and a second mixed (set 1) */
#define MESH_MODE_THIRD     2u      /* times a third texture (set 2) at the coordinates in the normal's place */
#define MESH_MODE_SHINY     4u      /* plus an environment map (set 1) */
#define MESH_MODE_MASKED    8u      /* the environment map times a mask (set 2) */
#define MESH_MODE_BY_ALPHA  16u     /* the mix also by the second texture's alpha */
#define MESH_INDICES    (16u << 20)                 /* 16-bit indices all meshes share: 32 MB */
#define MAX_MESH_DRAWS  16384u                      /* meshes a frame */
#define MAX_GRASS_PATCHES 1024u                     /* grass patches a frame */
#define SHADOW_SIZE     2048u                       /* the sun's shadow map, a side */
#define SHADOW_BIAS     0.0004f                     /* how much nearer the sun a caster must be (of its depth range) */

#define CHECK(call)                                                                 \
    do {                                                                            \
        VkResult result_ = (call);                                                  \
        if (result_ != VK_SUCCESS) {                                                \
            fprintf(stderr, "[ngl_vk] %s failed (%d)\n", #call, (int)result_);      \
            return -1;                                                              \
        }                                                                           \
    } while (0)

static struct {
    VkInstance instance;
    VkPhysicalDevice gpu;
    VkDevice device;
    VkQueue queue;
    uint32_t queue_family;
    VkCommandPool command_pool;
    VkCommandBuffer commands;
    VkFence done;

    /* the target, and the host's copy of it */
    uint32_t width, height;
    VkImage target;
    VkDeviceMemory target_memory;
    VkImageView target_view;
    VkImage depth;
    VkDeviceMemory depth_memory;
    VkImageView depth_view;
    VkRenderPass render_pass;
    VkFramebuffer framebuffer;
    VkBuffer readback;
    VkDeviceMemory readback_memory;
    void *readback_pixels;
    int drawn;

    /* the picture being drawn, and the buffer it is copied in through */
    uint32_t picture_width, picture_height;
    VkImage picture;
    VkDeviceMemory picture_memory;
    VkImageView picture_view;
    VkBuffer upload;
    VkDeviceMemory upload_memory;
    void *upload_pixels;

    VkSampler sampler;
    VkDescriptorSetLayout set_layout;
    VkDescriptorPool descriptor_pool;
    VkDescriptorSet set;
    VkPipelineLayout pipeline_layout;
    VkPipeline pipeline;

    /* textures, numbered from 1 (0 is the frame's picture) */
    struct {
        VkImage image;
        VkDeviceMemory memory;
        VkImageView view;
        VkDescriptorSet set;
    } textures[MAX_TEXTURES + 1];
    uint32_t texture_count;

    /* 2D quads */
    VkPipelineLayout quad_layout;
    VkPipeline quad_pipeline;
    VkBuffer vertices;
    VkDeviceMemory vertex_memory;
    void *vertex_data;
    /* 3D meshes: every mesh's corners and indices, and this frame's draws */
    VkPipelineLayout mesh_layout;
    VkPipeline mesh_pipeline;
    VkPipeline mesh_blend_pipeline;     /* the same, blended by alpha, not writing depth */
    VkPipeline mesh_add_pipeline;       /* and added (NGL_VK_BLEND_ADD), taken away (NGL_VK_BLEND_SUBTRACT) */
    VkPipeline mesh_subtract_pipeline;
    VkBuffer mesh_corners, mesh_indices;
    VkDeviceMemory mesh_corner_memory, mesh_index_memory;
    float *mesh_corner_data;
    uint16_t *mesh_index_data;
    uint32_t corners_used, indices_used, frame_corners_used;
    struct {
        uint32_t first_corner, first_index, index_count;
    } meshes[MAX_MESHES + 1];
    uint32_t mesh_count;
    struct ngl_vk_mesh_draw *draws;
    uint32_t draw_count;
    /* the grass (ngl_vk_set_grass): its pipeline, its constants' set, the shared blades, and this frame's patches */
    VkDescriptorSetLayout grass_set_layout;
    VkDescriptorPool grass_pool;
    VkDescriptorSet grass_set;
    VkPipelineLayout grass_layout;
    VkPipeline grass_pipeline;
    VkBuffer grass_constants, grass_vertices, grass_indices;
    VkDeviceMemory grass_constant_memory, grass_vertex_memory, grass_index_memory;
    void *grass_constant_data;
    uint32_t grass_index_count, grass_blade;
    struct ngl_vk_grass_patch grass_patches[MAX_GRASS_PATCHES];
    uint32_t grass_patch_count;
    /* the sun's shadow (ngl_vk_set_sun_shadow): its depth map and the depth-only pass that draws the casters into it;
     * set 3 of the mesh pipelines: the map with a comparing sampler, and the frame's lookup (a uniform buffer) */
    VkImage shadow_image;
    VkDeviceMemory shadow_memory;
    VkImageView shadow_view;
    VkRenderPass shadow_pass;
    VkFramebuffer shadow_framebuffer;
    VkSampler shadow_sampler;
    VkDescriptorSetLayout shadow_set_layout;
    VkDescriptorPool shadow_pool;
    VkDescriptorSet shadow_set;
    VkBuffer shadow_uniform;
    VkDeviceMemory shadow_uniform_memory;
    float *shadow_uniform_data;     /* the lookup (16 floats), then the bias, a texel, 0, 0 */
    VkPipeline mesh_shadow_pipeline;
    int shadow_cleared;             /* the map has been through its pass once (it is in its sampled layout) */
    int shadow_on;                  /* this frame has a sun's shadow */
    float shadow_light[16];         /* this frame's light view-projection */
    /* the window (ngl_vk_set_window): its surface and swapchain, while draws are shown in it */
    VkSurfaceKHR surface;
    VkSwapchainKHR swapchain;
    VkFormat swap_format;
    VkExtent2D swap_extent;
    uint32_t swap_count;
    VkImage swap_images[MAX_SWAP_IMAGES];
    VkSemaphore swap_ready;                         /* the acquired image is free to be written */
    VkSemaphore swap_shown[MAX_SWAP_IMAGES];        /* that image's copy is done: it can be shown */
    int swap_stale;                                 /* the window changed: make the swapchain again */
} vk;

/* What ngl_vk_set_window was given (kept across ngl_vk_shutdown, which clears vk) */
static struct {
    const char *const *extensions;
    uint32_t extension_count;
    ngl_vk_surface_maker make_surface;
    void *user;
    uint32_t width, height;
    int present_mode;
} window;

static char device_name[VK_MAX_PHYSICAL_DEVICE_NAME_SIZE];

/* A memory type in type_bits with all of the wanted properties. */
static int find_memory(uint32_t type_bits, VkMemoryPropertyFlags wanted, uint32_t *index)
{
    VkPhysicalDeviceMemoryProperties properties;
    uint32_t i;

    vkGetPhysicalDeviceMemoryProperties(vk.gpu, &properties);
    for (i = 0; i < properties.memoryTypeCount; i++) {
        if ((type_bits & (1u << i)) && (properties.memoryTypes[i].propertyFlags & wanted) == wanted) {
            *index = i;
            return 0;
        }
    }
    return -1;
}

/* A buffer in memory the host can see, mapped. */
static int make_host_buffer(VkDeviceSize size, VkBufferUsageFlags usage, VkBuffer *buffer, VkDeviceMemory *memory,
                            void **mapped)
{
    VkBufferCreateInfo info = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    VkMemoryAllocateInfo allocation = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    VkMemoryRequirements needs;

    info.size = size;
    info.usage = usage;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    CHECK(vkCreateBuffer(vk.device, &info, NULL, buffer));
    vkGetBufferMemoryRequirements(vk.device, *buffer, &needs);
    allocation.allocationSize = needs.size;
    if (find_memory(needs.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                    &allocation.memoryTypeIndex)) {
        fprintf(stderr, "[ngl_vk] no host-visible memory for a buffer\n");
        return -1;
    }
    CHECK(vkAllocateMemory(vk.device, &allocation, NULL, memory));
    CHECK(vkBindBufferMemory(vk.device, *buffer, *memory, 0));
    CHECK(vkMapMemory(vk.device, *memory, 0, size, 0, mapped));
    return 0;
}

/* A 2D image of one level, in the GPU's own memory when it has some, and a view of it. */
static int make_image(uint32_t width, uint32_t height, uint32_t levels, VkFormat format, VkImageUsageFlags usage,
                      VkImage *image, VkDeviceMemory *memory, VkImageView *view)
{
    VkImageCreateInfo info = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    VkImageViewCreateInfo view_info = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    VkMemoryAllocateInfo allocation = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    VkMemoryRequirements needs;

    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = format;
    info.extent.width = width;
    info.extent.height = height;
    info.extent.depth = 1;
    info.mipLevels = levels;
    info.arrayLayers = 1;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = usage;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    CHECK(vkCreateImage(vk.device, &info, NULL, image));

    vkGetImageMemoryRequirements(vk.device, *image, &needs);
    allocation.allocationSize = needs.size;
    if (find_memory(needs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &allocation.memoryTypeIndex) &&
        find_memory(needs.memoryTypeBits, 0, &allocation.memoryTypeIndex)) {
        fprintf(stderr, "[ngl_vk] no memory for a %ux%u image\n", width, height);
        return -1;
    }
    CHECK(vkAllocateMemory(vk.device, &allocation, NULL, memory));
    CHECK(vkBindImageMemory(vk.device, *image, *memory, 0));

    view_info.image = *image;
    view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view_info.format = format;
    view_info.subresourceRange.aspectMask = format == DEPTH_FORMAT ? VK_IMAGE_ASPECT_DEPTH_BIT
                                                                   : VK_IMAGE_ASPECT_COLOR_BIT;
    view_info.subresourceRange.levelCount = levels;
    view_info.subresourceRange.layerCount = 1;
    CHECK(vkCreateImageView(vk.device, &view_info, NULL, view));
    return 0;
}

static int make_shader(const uint32_t *code, size_t bytes, VkShaderModule *module)
{
    VkShaderModuleCreateInfo info = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };

    info.codeSize = bytes;
    info.pCode = code;
    CHECK(vkCreateShaderModule(vk.device, &info, NULL, module));
    return 0;
}

/* The GPU to draw on: a discrete one first, then an integrated one, then anything (a software renderer), with a
 * queue that can draw. */
/* Whether a GPU's device has VK_KHR_swapchain (to show in a window). */
static int has_swapchain_extension(VkPhysicalDevice gpu)
{
    VkExtensionProperties extensions[256];
    uint32_t count = 256, i;
    VkResult result = vkEnumerateDeviceExtensionProperties(gpu, NULL, &count, extensions);

    if (result != VK_SUCCESS && result != VK_INCOMPLETE)
        return 0;
    for (i = 0; i < count; i++)
        if (!strcmp(extensions[i].extensionName, VK_KHR_SWAPCHAIN_EXTENSION_NAME))
            return 1;
    return 0;
}

static int choose_gpu(void)
{
    VkPhysicalDevice gpus[16];
    uint32_t count = 16, i;
    int best_rank = -1;
    VkResult result = vkEnumeratePhysicalDevices(vk.instance, &count, gpus);

    if (result != VK_SUCCESS && result != VK_INCOMPLETE) {
        fprintf(stderr, "[ngl_vk] vkEnumeratePhysicalDevices failed (%d)\n", (int)result);
        return -1;
    }
    for (i = 0; i < count; i++) {
        VkPhysicalDeviceProperties properties;
        VkQueueFamilyProperties families[16];
        uint32_t family_count = 16, family;
        int rank;

        vkGetPhysicalDeviceProperties(gpus[i], &properties);
        vkGetPhysicalDeviceQueueFamilyProperties(gpus[i], &family_count, families);
        for (family = 0; family < family_count; family++) {
            VkBool32 shows = VK_TRUE;

            if (vk.surface && vkGetPhysicalDeviceSurfaceSupportKHR(gpus[i], family, vk.surface, &shows) != VK_SUCCESS)
                shows = VK_FALSE;
            if ((families[family].queueFlags & VK_QUEUE_GRAPHICS_BIT) && shows)
                break;
        }
        if (family == family_count || (vk.surface && !has_swapchain_extension(gpus[i])))
            continue;
        rank = properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 3
             : properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? 2 : 1;
        if (rank > best_rank) {
            best_rank = rank;
            vk.gpu = gpus[i];
            vk.queue_family = family;
            snprintf(device_name, sizeof device_name, "%s", properties.deviceName);
        }
    }
    if (best_rank < 0 && vk.surface) {
        fprintf(stderr, "[ngl_vk] no GPU can show on the window: drawing offscreen\n");
        vkDestroySurfaceKHR(vk.instance, vk.surface, NULL);
        vk.surface = VK_NULL_HANDLE;
        return choose_gpu();
    }
    if (best_rank < 0) {
        fprintf(stderr, "[ngl_vk] no GPU that can draw\n");
        return -1;
    }
    return 0;
}

static int make_device(void)
{
    VkDeviceQueueCreateInfo queue_info = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
    VkDeviceCreateInfo info = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
    VkCommandPoolCreateInfo pool_info = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
    VkCommandBufferAllocateInfo command_info = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    VkFenceCreateInfo fence_info = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    float priority = 1.0f;

    queue_info.queueFamilyIndex = vk.queue_family;
    queue_info.queueCount = 1;
    queue_info.pQueuePriorities = &priority;
    info.queueCreateInfoCount = 1;
    info.pQueueCreateInfos = &queue_info;
    if (vk.surface) {
        static const char *const swapchain_extension[] = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };

        info.enabledExtensionCount = 1;
        info.ppEnabledExtensionNames = swapchain_extension;
    }
    CHECK(vkCreateDevice(vk.gpu, &info, NULL, &vk.device));
    vkGetDeviceQueue(vk.device, vk.queue_family, 0, &vk.queue);

    pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pool_info.queueFamilyIndex = vk.queue_family;
    CHECK(vkCreateCommandPool(vk.device, &pool_info, NULL, &vk.command_pool));
    command_info.commandPool = vk.command_pool;
    command_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    command_info.commandBufferCount = 1;
    CHECK(vkAllocateCommandBuffers(vk.device, &command_info, &vk.commands));
    CHECK(vkCreateFence(vk.device, &fence_info, NULL, &vk.done));
    return 0;
}

/* The window's swapchain goes (the GPU is idle first). The surface stays. */
static void free_swapchain(void)
{
    uint32_t i;

    if (!vk.device)
        return;
    vkDeviceWaitIdle(vk.device);
    for (i = 0; i < MAX_SWAP_IMAGES; i++) {
        if (vk.swap_shown[i])
            vkDestroySemaphore(vk.device, vk.swap_shown[i], NULL);
        vk.swap_shown[i] = VK_NULL_HANDLE;
    }
    if (vk.swap_ready)
        vkDestroySemaphore(vk.device, vk.swap_ready, NULL);
    vk.swap_ready = VK_NULL_HANDLE;
    if (vk.swapchain)
        vkDestroySwapchainKHR(vk.device, vk.swapchain, NULL);
    vk.swapchain = VK_NULL_HANDLE;
    vk.swap_count = 0;
}

/* A swapchain for the window's surface at its current size: 8-bit color without sRGB conversion (the target's colors
 * go out as they are, as the host's copy has them), images the target can be blitted into. Returns 0 when there is one
 * (or the window is minimized, with none), -1 when the window can't be used. */
static int make_swapchain(void)
{
    VkSurfaceCapabilitiesKHR caps;
    VkSurfaceFormatKHR formats[64];
    VkPresentModeKHR modes[16];
    VkFormatProperties target_features, swap_features;
    VkSwapchainCreateInfoKHR info = { VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR };
    VkSemaphoreCreateInfo semaphore_info = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    VkSwapchainKHR old = vk.swapchain;
    uint32_t format_count = 64, mode_count = 16, i, image_count;
    VkResult result;

    vk.swap_stale = 0;
    CHECK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(vk.gpu, vk.surface, &caps));
    result = vkGetPhysicalDeviceSurfaceFormatsKHR(vk.gpu, vk.surface, &format_count, formats);
    if ((result != VK_SUCCESS && result != VK_INCOMPLETE) || !format_count) {
        fprintf(stderr, "[ngl_vk] the window's surface has no formats (%d)\n", (int)result);
        return -1;
    }
    result = vkGetPhysicalDeviceSurfacePresentModesKHR(vk.gpu, vk.surface, &mode_count, modes);
    if (result != VK_SUCCESS && result != VK_INCOMPLETE)
        mode_count = 0;
    if (!(caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT)) {
        fprintf(stderr, "[ngl_vk] the window's images can't be copied into\n");
        return -1;
    }

    info.imageFormat = formats[0].format;
    info.imageColorSpace = formats[0].colorSpace;
    for (i = 0; i < format_count; i++) {
        if ((formats[i].format == VK_FORMAT_B8G8R8A8_UNORM || formats[i].format == VK_FORMAT_R8G8B8A8_UNORM) &&
            formats[i].colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            info.imageFormat = formats[i].format;
            info.imageColorSpace = formats[i].colorSpace;
            break;
        }
    }
    if (info.imageFormat == VK_FORMAT_UNDEFINED)    /* any format: the surface takes what is asked */
        info.imageFormat = VK_FORMAT_B8G8R8A8_UNORM;
    vkGetPhysicalDeviceFormatProperties(vk.gpu, TARGET_FORMAT, &target_features);
    vkGetPhysicalDeviceFormatProperties(vk.gpu, info.imageFormat, &swap_features);
    if (!(target_features.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_SRC_BIT) ||
        !(swap_features.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_DST_BIT)) {
        fprintf(stderr, "[ngl_vk] the target can't be blitted into the window's format %d\n", (int)info.imageFormat);
        return -1;
    }

    info.imageExtent = caps.currentExtent;
    if (caps.currentExtent.width == 0xFFFFFFFFu) {  /* the surface takes its size from the swapchain */
        info.imageExtent.width = window.width;
        info.imageExtent.height = window.height;
        if (info.imageExtent.width < caps.minImageExtent.width)
            info.imageExtent.width = caps.minImageExtent.width;
        if (info.imageExtent.width > caps.maxImageExtent.width)
            info.imageExtent.width = caps.maxImageExtent.width;
        if (info.imageExtent.height < caps.minImageExtent.height)
            info.imageExtent.height = caps.minImageExtent.height;
        if (info.imageExtent.height > caps.maxImageExtent.height)
            info.imageExtent.height = caps.maxImageExtent.height;
    }
    if (!info.imageExtent.width || !info.imageExtent.height || !window.width || !window.height) {
        free_swapchain();                           /* minimized: nothing to show until it is back */
        return 0;
    }

    image_count = caps.minImageCount + 1;
    if (caps.maxImageCount && image_count > caps.maxImageCount)
        image_count = caps.maxImageCount;
    if (image_count > MAX_SWAP_IMAGES)
        image_count = MAX_SWAP_IMAGES;
    info.presentMode = VK_PRESENT_MODE_FIFO_KHR;    /* every surface has it */
    for (i = 0; window.present_mode == 1 && i < mode_count; i++)
        if (modes[i] == VK_PRESENT_MODE_MAILBOX_KHR)
            info.presentMode = VK_PRESENT_MODE_MAILBOX_KHR;
    info.surface = vk.surface;
    info.minImageCount = image_count;
    info.imageArrayLayers = 1;
    info.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.preTransform = (caps.supportedTransforms & VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR)
                        ? VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR : caps.currentTransform;
    info.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    if (!(caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR)) {
        uint32_t bit;

        for (bit = 1; bit && !(caps.supportedCompositeAlpha & bit); bit <<= 1)
            ;
        info.compositeAlpha = (VkCompositeAlphaFlagBitsKHR)bit;
    }
    info.clipped = VK_TRUE;
    info.oldSwapchain = old;
    vkDeviceWaitIdle(vk.device);
    CHECK(vkCreateSwapchainKHR(vk.device, &info, NULL, &vk.swapchain));
    if (old)
        vkDestroySwapchainKHR(vk.device, old, NULL);
    vk.swap_format = info.imageFormat;
    vk.swap_extent = info.imageExtent;
    vk.swap_count = MAX_SWAP_IMAGES;
    result = vkGetSwapchainImagesKHR(vk.device, vk.swapchain, &vk.swap_count, vk.swap_images);
    if (result != VK_SUCCESS && result != VK_INCOMPLETE) {     /* more than MAX_SWAP_IMAGES: the first ones serve */
        fprintf(stderr, "[ngl_vk] vkGetSwapchainImagesKHR failed (%d)\n", (int)result);
        return -1;
    }
    if (!vk.swap_ready)
        CHECK(vkCreateSemaphore(vk.device, &semaphore_info, NULL, &vk.swap_ready));
    for (i = 0; i < vk.swap_count; i++)
        if (!vk.swap_shown[i])
            CHECK(vkCreateSemaphore(vk.device, &semaphore_info, NULL, &vk.swap_shown[i]));
    fprintf(stderr, "[ngl_vk] showing in the window: %u x %u, %u images, format %d, %s\n", vk.swap_extent.width,
            vk.swap_extent.height, vk.swap_count, (int)vk.swap_format,
            info.presentMode == VK_PRESENT_MODE_MAILBOX_KHR ? "mailbox" : "in step with the display");
    return 0;
}

/* The window can't be shown in: draws go on offscreen. */
static void drop_window(void)
{
    free_swapchain();
    if (vk.surface)
        vkDestroySurfaceKHR(vk.instance, vk.surface, NULL);
    vk.surface = VK_NULL_HANDLE;
}

void ngl_vk_set_window(const char *const *instance_extensions, uint32_t extension_count,
                       ngl_vk_surface_maker make_surface, void *user, uint32_t width, uint32_t height,
                       int present_mode)
{
    window.extensions = instance_extensions;
    window.extension_count = instance_extensions ? extension_count : 0;
    window.make_surface = make_surface;
    window.user = user;
    window.width = width;
    window.height = height;
    window.present_mode = present_mode;
}

void ngl_vk_window_resized(uint32_t width, uint32_t height)
{
    if (width != window.width || height != window.height)
        vk.swap_stale = 1;
    window.width = width;
    window.height = height;
}

int ngl_vk_showing(void)
{
    return vk.swapchain != VK_NULL_HANDLE;
}

/* The render pass leaves the target ready to be copied out. */
static int make_target(void)
{
    VkAttachmentDescription attachments[2] = { { 0 }, { 0 } };
    VkAttachmentReference color = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
    VkAttachmentReference depth = { 1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL };
    VkImageView views[2];
    VkSubpassDescription subpass = { 0 };
    VkSubpassDependency dependencies[2] = { { 0 }, { 0 } };
    VkSubpassDependency *to_copy = &dependencies[0], *depth_reuse = &dependencies[1];
    VkRenderPassCreateInfo pass_info = { VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
    VkFramebufferCreateInfo framebuffer_info = { VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };

    attachments[0].format = TARGET_FORMAT;
    attachments[0].samples = VK_SAMPLE_COUNT_1_BIT;
    attachments[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachments[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachments[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachments[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[0].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    attachments[0].finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    /* the depth buffer: cleared each pass, not kept */
    attachments[1].format = DEPTH_FORMAT;
    attachments[1].samples = VK_SAMPLE_COUNT_1_BIT;
    attachments[1].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachments[1].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[1].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachments[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[1].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    attachments[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &color;
    subpass.pDepthStencilAttachment = &depth;
    to_copy->srcSubpass = 0;
    to_copy->dstSubpass = VK_SUBPASS_EXTERNAL;
    to_copy->srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    to_copy->dstStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT;
    to_copy->srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    to_copy->dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    /* the last pass's depth tests and writes are done before this pass clears the depth buffer */
    depth_reuse->srcSubpass = VK_SUBPASS_EXTERNAL;
    depth_reuse->dstSubpass = 0;
    depth_reuse->srcStageMask = VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    depth_reuse->dstStageMask = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    depth_reuse->srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    depth_reuse->dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    pass_info.attachmentCount = 2;
    pass_info.pAttachments = attachments;
    pass_info.subpassCount = 1;
    pass_info.pSubpasses = &subpass;
    pass_info.dependencyCount = 2;
    pass_info.pDependencies = dependencies;
    CHECK(vkCreateRenderPass(vk.device, &pass_info, NULL, &vk.render_pass));

    if (make_image(vk.width, vk.height, 1, TARGET_FORMAT,
                   VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                   &vk.target, &vk.target_memory, &vk.target_view) ||
        make_image(vk.width, vk.height, 1, DEPTH_FORMAT, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
                   &vk.depth, &vk.depth_memory, &vk.depth_view))
        return -1;
    views[0] = vk.target_view;
    views[1] = vk.depth_view;
    framebuffer_info.renderPass = vk.render_pass;
    framebuffer_info.attachmentCount = 2;
    framebuffer_info.pAttachments = views;
    framebuffer_info.width = vk.width;
    framebuffer_info.height = vk.height;
    framebuffer_info.layers = 1;
    CHECK(vkCreateFramebuffer(vk.device, &framebuffer_info, NULL, &vk.framebuffer));

    return make_host_buffer((VkDeviceSize)vk.width * vk.height * BYTES_PER_PIXEL, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                            &vk.readback, &vk.readback_memory, &vk.readback_pixels);
}

static int make_pipeline(void)
{
    VkSamplerCreateInfo sampler_info = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    VkDescriptorSetLayoutBinding binding = { 0 };
    VkDescriptorSetLayoutCreateInfo set_layout_info = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    VkDescriptorPoolSize pool_size = { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1 + MAX_TEXTURES };
    VkDescriptorPoolCreateInfo pool_info = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
    VkDescriptorSetAllocateInfo set_info = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
    VkPipelineLayoutCreateInfo layout_info = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    VkShaderModule vertex_shader = VK_NULL_HANDLE, fragment_shader = VK_NULL_HANDLE;
    VkPipelineShaderStageCreateInfo stages[2] = {
        { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO },
        { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO },
    };
    VkPipelineVertexInputStateCreateInfo vertex_input = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    VkPipelineInputAssemblyStateCreateInfo assembly = {
        VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
    VkPipelineViewportStateCreateInfo viewport = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
    VkPipelineRasterizationStateCreateInfo raster = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
    VkPipelineMultisampleStateCreateInfo multisample = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
    VkPipelineColorBlendAttachmentState blend_attachment = { 0 };
    VkPipelineColorBlendStateCreateInfo blend = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
    VkPipelineDepthStencilStateCreateInfo no_depth = {
        VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };     /* drawn over everything */
    VkDynamicState dynamic_states[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dynamic = { VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
    VkGraphicsPipelineCreateInfo info = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
    VkResult result;

    sampler_info.magFilter = VK_FILTER_LINEAR;
    sampler_info.minFilter = VK_FILTER_LINEAR;
    sampler_info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;   /* a texture's own levels blended, as the GPU does */
    sampler_info.maxLod = VK_LOD_CLAMP_NONE;
    sampler_info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler_info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler_info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    CHECK(vkCreateSampler(vk.device, &sampler_info, NULL, &vk.sampler));

    binding.binding = 0;
    binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    binding.descriptorCount = 1;
    binding.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;    /* the grass reads two in its
                                                                                      * vertex shader */
    set_layout_info.bindingCount = 1;
    set_layout_info.pBindings = &binding;
    CHECK(vkCreateDescriptorSetLayout(vk.device, &set_layout_info, NULL, &vk.set_layout));
    pool_info.maxSets = 1 + MAX_TEXTURES;
    pool_info.poolSizeCount = 1;
    pool_info.pPoolSizes = &pool_size;
    CHECK(vkCreateDescriptorPool(vk.device, &pool_info, NULL, &vk.descriptor_pool));
    set_info.descriptorPool = vk.descriptor_pool;
    set_info.descriptorSetCount = 1;
    set_info.pSetLayouts = &vk.set_layout;
    CHECK(vkAllocateDescriptorSets(vk.device, &set_info, &vk.set));
    layout_info.setLayoutCount = 1;
    layout_info.pSetLayouts = &vk.set_layout;
    CHECK(vkCreatePipelineLayout(vk.device, &layout_info, NULL, &vk.pipeline_layout));

    if (make_shader(quad_vert_spirv, sizeof quad_vert_spirv, &vertex_shader) ||
        make_shader(quad_frag_spirv, sizeof quad_frag_spirv, &fragment_shader))
        return -1;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vertex_shader;
    stages[0].pName = "main";
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fragment_shader;
    stages[1].pName = "main";
    assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    viewport.viewportCount = 1;
    viewport.scissorCount = 1;
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.cullMode = VK_CULL_MODE_NONE;
    raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    raster.lineWidth = 1.0f;
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    blend_attachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                      VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    blend.attachmentCount = 1;
    blend.pAttachments = &blend_attachment;
    dynamic.dynamicStateCount = 2;
    dynamic.pDynamicStates = dynamic_states;

    info.stageCount = 2;
    info.pStages = stages;
    info.pVertexInputState = &vertex_input;
    info.pInputAssemblyState = &assembly;
    info.pViewportState = &viewport;
    info.pRasterizationState = &raster;
    info.pMultisampleState = &multisample;
    info.pColorBlendState = &blend;
    info.pDepthStencilState = &no_depth;
    info.pDynamicState = &dynamic;
    info.layout = vk.pipeline_layout;
    info.renderPass = vk.render_pass;
    info.subpass = 0;
    result = vkCreateGraphicsPipelines(vk.device, VK_NULL_HANDLE, 1, &info, NULL, &vk.pipeline);
    vkDestroyShaderModule(vk.device, vertex_shader, NULL);
    vkDestroyShaderModule(vk.device, fragment_shader, NULL);
    if (result != VK_SUCCESS) {
        fprintf(stderr, "[ngl_vk] vkCreateGraphicsPipelines failed (%d)\n", (int)result);
        return -1;
    }
    return 0;
}

static int make_quad_pipeline(void)
{
    VkPushConstantRange screen_size = { VK_SHADER_STAGE_VERTEX_BIT, 0, 2 * sizeof(float) };
    VkPipelineLayoutCreateInfo layout_info = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    VkShaderModule vertex_shader = VK_NULL_HANDLE, fragment_shader = VK_NULL_HANDLE;
    VkPipelineShaderStageCreateInfo stages[2] = {
        { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO },
        { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO },
    };
    VkVertexInputBindingDescription binding = { 0, sizeof(struct ngl_vk_vertex), VK_VERTEX_INPUT_RATE_VERTEX };
    VkVertexInputAttributeDescription attributes[4] = {
        { 0, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(struct ngl_vk_vertex, x) },
        { 1, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(struct ngl_vk_vertex, u) },
        { 2, 0, VK_FORMAT_B8G8R8A8_UNORM, offsetof(struct ngl_vk_vertex, color) },
        { 3, 0, VK_FORMAT_R32_SFLOAT, offsetof(struct ngl_vk_vertex, textured) },
    };
    VkPipelineVertexInputStateCreateInfo vertex_input = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    VkPipelineInputAssemblyStateCreateInfo assembly = {
        VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
    VkPipelineViewportStateCreateInfo viewport = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
    VkPipelineRasterizationStateCreateInfo raster = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
    VkPipelineMultisampleStateCreateInfo multisample = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
    VkPipelineColorBlendAttachmentState blend_attachment = { 0 };
    VkPipelineColorBlendStateCreateInfo blend = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
    VkPipelineDepthStencilStateCreateInfo no_depth = {
        VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };     /* drawn over everything */
    VkDynamicState dynamic_states[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dynamic = { VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
    VkGraphicsPipelineCreateInfo info = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
    VkResult result;

    layout_info.setLayoutCount = 1;
    layout_info.pSetLayouts = &vk.set_layout;
    layout_info.pushConstantRangeCount = 1;
    layout_info.pPushConstantRanges = &screen_size;
    CHECK(vkCreatePipelineLayout(vk.device, &layout_info, NULL, &vk.quad_layout));

    if (make_shader(quad2d_vert_spirv, sizeof quad2d_vert_spirv, &vertex_shader) ||
        make_shader(quad2d_frag_spirv, sizeof quad2d_frag_spirv, &fragment_shader))
        return -1;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vertex_shader;
    stages[0].pName = "main";
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fragment_shader;
    stages[1].pName = "main";
    vertex_input.vertexBindingDescriptionCount = 1;
    vertex_input.pVertexBindingDescriptions = &binding;
    vertex_input.vertexAttributeDescriptionCount = (uint32_t)(sizeof attributes / sizeof attributes[0]);
    vertex_input.pVertexAttributeDescriptions = attributes;
    assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    viewport.viewportCount = 1;
    viewport.scissorCount = 1;
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.cullMode = VK_CULL_MODE_NONE;
    raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    raster.lineWidth = 1.0f;
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    /* ordinary alpha blending, in the order the quads come */
    blend_attachment.blendEnable = VK_TRUE;
    blend_attachment.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    blend_attachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blend_attachment.colorBlendOp = VK_BLEND_OP_ADD;
    blend_attachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    blend_attachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blend_attachment.alphaBlendOp = VK_BLEND_OP_ADD;
    blend_attachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                      VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    blend.attachmentCount = 1;
    blend.pAttachments = &blend_attachment;
    dynamic.dynamicStateCount = 2;
    dynamic.pDynamicStates = dynamic_states;

    info.stageCount = 2;
    info.pStages = stages;
    info.pVertexInputState = &vertex_input;
    info.pInputAssemblyState = &assembly;
    info.pViewportState = &viewport;
    info.pRasterizationState = &raster;
    info.pMultisampleState = &multisample;
    info.pColorBlendState = &blend;
    info.pDepthStencilState = &no_depth;
    info.pDynamicState = &dynamic;
    info.layout = vk.quad_layout;
    info.renderPass = vk.render_pass;
    info.subpass = 0;
    result = vkCreateGraphicsPipelines(vk.device, VK_NULL_HANDLE, 1, &info, NULL, &vk.quad_pipeline);
    vkDestroyShaderModule(vk.device, vertex_shader, NULL);
    vkDestroyShaderModule(vk.device, fragment_shader, NULL);
    if (result != VK_SUCCESS) {
        fprintf(stderr, "[ngl_vk] vkCreateGraphicsPipelines (quads) failed (%d)\n", (int)result);
        return -1;
    }
    return make_host_buffer((VkDeviceSize)MAX_VERTICES * sizeof(struct ngl_vk_vertex),
                            VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, &vk.vertices, &vk.vertex_memory, &vk.vertex_data);
}

/* The sun's shadow map: a SHADOW_SIZE x SHADOW_SIZE depth image; the depth-only pass that clears it to the far end and
 * draws the casters into it, leaving it ready to sample; a sampler that compares (filtered where the GPU filters the
 * format, so four texels' answers are blended); and set 3 of the mesh pipelines: binding 0 the map, binding 1 the
 * frame's lookup (a uniform buffer: the camera's clip space to the light's, then the bias and a texel). */
static int make_shadow(void)
{
    VkAttachmentDescription attachment = { 0 };
    VkAttachmentReference depth = { 0, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL };
    VkSubpassDescription subpass = { 0 };
    VkSubpassDependency dependencies[2] = { { 0 }, { 0 } };
    VkRenderPassCreateInfo pass_info = { VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
    VkFramebufferCreateInfo framebuffer_info = { VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
    VkSamplerCreateInfo sampler_info = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    VkFormatProperties format_properties;
    VkDescriptorSetLayoutBinding bindings[2] = { { 0 }, { 0 } };
    VkDescriptorSetLayoutCreateInfo set_layout_info = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    VkDescriptorPoolSize pool_sizes[2] = {
        { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1 },
        { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1 },
    };
    VkDescriptorPoolCreateInfo pool_info = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
    VkDescriptorSetAllocateInfo set_info = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
    VkDescriptorImageInfo image_info = { VK_NULL_HANDLE, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
    VkDescriptorBufferInfo buffer_info = { VK_NULL_HANDLE, 0, VK_WHOLE_SIZE };
    VkWriteDescriptorSet writes[2] = {
        { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET },
        { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET },
    };

    attachment.format = DEPTH_FORMAT;
    attachment.samples = VK_SAMPLE_COUNT_1_BIT;
    attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    attachment.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.pDepthStencilAttachment = &depth;
    /* the last frame's reads are done before the clear, and this pass's writes before the meshes read them */
    dependencies[0].srcSubpass = VK_SUBPASS_EXTERNAL;
    dependencies[0].dstSubpass = 0;
    dependencies[0].srcStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    dependencies[0].dstStageMask = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    dependencies[0].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    dependencies[0].dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    dependencies[1].srcSubpass = 0;
    dependencies[1].dstSubpass = VK_SUBPASS_EXTERNAL;
    dependencies[1].srcStageMask = VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    dependencies[1].dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    dependencies[1].srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    dependencies[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    pass_info.attachmentCount = 1;
    pass_info.pAttachments = &attachment;
    pass_info.subpassCount = 1;
    pass_info.pSubpasses = &subpass;
    pass_info.dependencyCount = 2;
    pass_info.pDependencies = dependencies;
    CHECK(vkCreateRenderPass(vk.device, &pass_info, NULL, &vk.shadow_pass));
    if (make_image(SHADOW_SIZE, SHADOW_SIZE, 1, DEPTH_FORMAT,
                   VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                   &vk.shadow_image, &vk.shadow_memory, &vk.shadow_view))
        return -1;
    framebuffer_info.renderPass = vk.shadow_pass;
    framebuffer_info.attachmentCount = 1;
    framebuffer_info.pAttachments = &vk.shadow_view;
    framebuffer_info.width = SHADOW_SIZE;
    framebuffer_info.height = SHADOW_SIZE;
    framebuffer_info.layers = 1;
    CHECK(vkCreateFramebuffer(vk.device, &framebuffer_info, NULL, &vk.shadow_framebuffer));

    vkGetPhysicalDeviceFormatProperties(vk.gpu, DEPTH_FORMAT, &format_properties);
    sampler_info.magFilter = (format_properties.optimalTilingFeatures &
                              VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT) ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    sampler_info.minFilter = sampler_info.magFilter;
    sampler_info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sampler_info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler_info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler_info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler_info.compareEnable = VK_TRUE;
    sampler_info.compareOp = VK_COMPARE_OP_LESS_OR_EQUAL;     /* lit: the pixel no deeper than the nearest caster */
    sampler_info.maxLod = 0.0f;
    sampler_info.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
    CHECK(vkCreateSampler(vk.device, &sampler_info, NULL, &vk.shadow_sampler));

    bindings[0].binding = 0;
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    bindings[1].binding = 1;
    bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    bindings[1].descriptorCount = 1;
    bindings[1].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    set_layout_info.bindingCount = 2;
    set_layout_info.pBindings = bindings;
    CHECK(vkCreateDescriptorSetLayout(vk.device, &set_layout_info, NULL, &vk.shadow_set_layout));
    pool_info.maxSets = 1;
    pool_info.poolSizeCount = 2;
    pool_info.pPoolSizes = pool_sizes;
    CHECK(vkCreateDescriptorPool(vk.device, &pool_info, NULL, &vk.shadow_pool));
    set_info.descriptorPool = vk.shadow_pool;
    set_info.descriptorSetCount = 1;
    set_info.pSetLayouts = &vk.shadow_set_layout;
    CHECK(vkAllocateDescriptorSets(vk.device, &set_info, &vk.shadow_set));
    if (make_host_buffer(32 * sizeof(float), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, &vk.shadow_uniform,
                         &vk.shadow_uniform_memory, (void **)&vk.shadow_uniform_data))
        return -1;
    memset(vk.shadow_uniform_data, 0, 32 * sizeof(float));
    image_info.sampler = vk.shadow_sampler;
    image_info.imageView = vk.shadow_view;
    buffer_info.buffer = vk.shadow_uniform;
    writes[0].dstSet = vk.shadow_set;
    writes[0].dstBinding = 0;
    writes[0].descriptorCount = 1;
    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[0].pImageInfo = &image_info;
    writes[1].dstSet = vk.shadow_set;
    writes[1].dstBinding = 1;
    writes[1].descriptorCount = 1;
    writes[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    writes[1].pBufferInfo = &buffer_info;
    vkUpdateDescriptorSets(vk.device, 2, writes, 0, NULL);
    return 0;
}

/* 3D meshes: corners as 8 floats (position, texture and lightmap coordinates, color), triangle lists of 16-bit
 * indices, depth-tested and depth-written, both faces drawn (the game's culling isn't known yet), not blended; and a
 * second pipeline the same but blended by the pixel's alpha and not writing depth, for NGL_VK_ALPHA_BLEND, with two
 * more like it for effects: added (NGL_VK_BLEND_ADD) and taken away (NGL_VK_BLEND_SUBTRACT). Sets 0,
 * 1 and 2 are textures' (the quads' layout): the base texture, the lightmap (or a mixed draw's second texture) and a
 * mixed draw's third; set 3 the sun's shadow (make_shadow). Push constants: the world-view-projection, the fog and
 * the light (push_mesh_draw). One more pipeline draws shadow casters into the shadow map: the same vertex shader,
 * shaders/shadow.frag (depth only, alpha-tested), the shadow pass, a slope-scaled depth bias. */
static int make_mesh_pipeline(void)
{
    VkPushConstantRange matrices = { VK_SHADER_STAGE_VERTEX_BIT, 0, 32 * sizeof(float) };
    VkPipelineLayoutCreateInfo layout_info = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    VkShaderModule vertex_shader = VK_NULL_HANDLE, fragment_shader = VK_NULL_HANDLE;
    VkPipelineShaderStageCreateInfo stages[2] = {
        { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO },
        { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO },
    };
    VkVertexInputBindingDescription binding = { 0, CORNER_FLOATS * sizeof(float), VK_VERTEX_INPUT_RATE_VERTEX };
    VkVertexInputAttributeDescription attributes[5] = {
        { 0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0 },                   /* position */
        { 1, 0, VK_FORMAT_R32G32_SFLOAT, 3 * sizeof(float) },      /* texture coordinates */
        { 2, 0, VK_FORMAT_R32G32_SFLOAT, 5 * sizeof(float) },      /* lightmap coordinates */
        { 3, 0, VK_FORMAT_B8G8R8A8_UNORM, 7 * sizeof(float) },     /* color: a D3DCOLOR's bytes */
        { 4, 0, VK_FORMAT_R32G32B32_SFLOAT, 8 * sizeof(float) },   /* normal (0, 0, 0: none) */
    };
    VkDescriptorSetLayout set_layouts[4] = { vk.set_layout, vk.set_layout, vk.set_layout, vk.shadow_set_layout };
    VkPipelineVertexInputStateCreateInfo vertex_input = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    VkPipelineInputAssemblyStateCreateInfo assembly = {
        VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
    VkPipelineViewportStateCreateInfo viewport = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
    VkPipelineRasterizationStateCreateInfo raster = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
    VkPipelineMultisampleStateCreateInfo multisample = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
    VkPipelineColorBlendAttachmentState blend_attachment = { 0 };
    VkPipelineColorBlendStateCreateInfo blend = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
    VkPipelineDepthStencilStateCreateInfo depth = { VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
    VkDynamicState dynamic_states[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dynamic = { VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
    VkGraphicsPipelineCreateInfo info = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
    VkResult result;

    layout_info.setLayoutCount = 4;
    layout_info.pSetLayouts = set_layouts;
    layout_info.pushConstantRangeCount = 1;
    layout_info.pPushConstantRanges = &matrices;
    CHECK(vkCreatePipelineLayout(vk.device, &layout_info, NULL, &vk.mesh_layout));

    if (make_shader(mesh_vert_spirv, sizeof mesh_vert_spirv, &vertex_shader) ||
        make_shader(mesh_frag_spirv, sizeof mesh_frag_spirv, &fragment_shader))
        return -1;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vertex_shader;
    stages[0].pName = "main";
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fragment_shader;
    stages[1].pName = "main";
    vertex_input.vertexBindingDescriptionCount = 1;
    vertex_input.pVertexBindingDescriptions = &binding;
    vertex_input.vertexAttributeDescriptionCount = (uint32_t)(sizeof attributes / sizeof attributes[0]);
    vertex_input.pVertexAttributeDescriptions = attributes;
    assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    viewport.viewportCount = 1;
    viewport.scissorCount = 1;
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.cullMode = VK_CULL_MODE_NONE;
    raster.frontFace = VK_FRONT_FACE_CLOCKWISE;
    raster.lineWidth = 1.0f;
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    blend_attachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                      VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    blend.attachmentCount = 1;
    blend.pAttachments = &blend_attachment;
    depth.depthTestEnable = VK_TRUE;
    depth.depthWriteEnable = VK_TRUE;
    depth.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
    dynamic.dynamicStateCount = 2;
    dynamic.pDynamicStates = dynamic_states;

    info.stageCount = 2;
    info.pStages = stages;
    info.pVertexInputState = &vertex_input;
    info.pInputAssemblyState = &assembly;
    info.pViewportState = &viewport;
    info.pRasterizationState = &raster;
    info.pMultisampleState = &multisample;
    info.pColorBlendState = &blend;
    info.pDepthStencilState = &depth;
    info.pDynamicState = &dynamic;
    info.layout = vk.mesh_layout;
    info.renderPass = vk.render_pass;
    info.subpass = 0;
    result = vkCreateGraphicsPipelines(vk.device, VK_NULL_HANDLE, 1, &info, NULL, &vk.mesh_pipeline);
    if (result == VK_SUCCESS) {
        blend_attachment.blendEnable = VK_TRUE;
        blend_attachment.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        blend_attachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        blend_attachment.colorBlendOp = VK_BLEND_OP_ADD;
        blend_attachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        blend_attachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        blend_attachment.alphaBlendOp = VK_BLEND_OP_ADD;
        depth.depthWriteEnable = VK_FALSE;
        result = vkCreateGraphicsPipelines(vk.device, VK_NULL_HANDLE, 1, &info, NULL, &vk.mesh_blend_pipeline);
    }
    if (result == VK_SUCCESS) {         /* added: behind + color x alpha */
        blend_attachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
        blend_attachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
        blend_attachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        result = vkCreateGraphicsPipelines(vk.device, VK_NULL_HANDLE, 1, &info, NULL, &vk.mesh_add_pipeline);
    }
    if (result == VK_SUCCESS) {         /* taken away: behind - color x alpha */
        blend_attachment.colorBlendOp = VK_BLEND_OP_REVERSE_SUBTRACT;
        result = vkCreateGraphicsPipelines(vk.device, VK_NULL_HANDLE, 1, &info, NULL, &vk.mesh_subtract_pipeline);
    }
    if (result == VK_SUCCESS) {         /* the sun's shadow casters: depth only, into the shadow map */
        VkShaderModule shadow_shader = VK_NULL_HANDLE;

        if (make_shader(shadow_frag_spirv, sizeof shadow_frag_spirv, &shadow_shader)) {
            result = VK_ERROR_INITIALIZATION_FAILED;
        } else {
            stages[1].module = shadow_shader;
            blend.attachmentCount = 0;
            blend.pAttachments = NULL;
            depth.depthWriteEnable = VK_TRUE;
            raster.depthBiasEnable = VK_TRUE;
            raster.depthBiasConstantFactor = 1.0f;
            raster.depthBiasSlopeFactor = 1.5f;
            info.renderPass = vk.shadow_pass;
            result = vkCreateGraphicsPipelines(vk.device, VK_NULL_HANDLE, 1, &info, NULL, &vk.mesh_shadow_pipeline);
            vkDestroyShaderModule(vk.device, shadow_shader, NULL);
        }
    }
    vkDestroyShaderModule(vk.device, vertex_shader, NULL);
    vkDestroyShaderModule(vk.device, fragment_shader, NULL);
    if (result != VK_SUCCESS) {
        fprintf(stderr, "[ngl_vk] vkCreateGraphicsPipelines (meshes) failed (%d)\n", (int)result);
        return -1;
    }
    vk.draws = calloc(MAX_MESH_DRAWS, sizeof *vk.draws);
    if (!vk.draws)
        return -1;
    if (make_host_buffer((VkDeviceSize)MESH_CORNERS * CORNER_FLOATS * sizeof(float),
                         VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, &vk.mesh_corners, &vk.mesh_corner_memory,
                         (void **)&vk.mesh_corner_data))
        return -1;
    return make_host_buffer((VkDeviceSize)MESH_INDICES * sizeof(uint16_t), VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                            &vk.mesh_indices, &vk.mesh_index_memory, (void **)&vk.mesh_index_data);
}

/* The grass (Call of Duty 3's GrassRenderNode): the game's own vertex and pixel shaders translated (shaders/grass.vert,
 * grass.frag), one indexed strip of blades (16-bit indices, 0xFFFF between blades) drawn once per patch. Sets 0 and 1
 * the patch's color and cell textures (read by the vertex shader), 2 the blade texture, 3 the constants; the patch's
 * two vectors are pushed. Made when ngl_vk_init runs; when it can't be, the grass is left out (and said so). */
static int make_grass_pipeline(void)
{
    VkPushConstantRange place = { VK_SHADER_STAGE_VERTEX_BIT, 0, 8 * sizeof(float) };
    VkDescriptorSetLayoutBinding binding = { 0 };
    VkDescriptorSetLayoutCreateInfo set_layout_info = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    VkDescriptorPoolSize pool_size = { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1 };
    VkDescriptorPoolCreateInfo pool_info = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
    VkDescriptorSetAllocateInfo set_info = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
    VkDescriptorBufferInfo buffer_info = { VK_NULL_HANDLE, 0, VK_WHOLE_SIZE };
    VkWriteDescriptorSet write = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
    VkDescriptorSetLayout set_layouts[4];
    VkPipelineLayoutCreateInfo layout_info = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    VkShaderModule vertex_shader = VK_NULL_HANDLE, fragment_shader = VK_NULL_HANDLE;
    VkPipelineShaderStageCreateInfo stages[2] = {
        { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO },
        { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO },
    };
    VkVertexInputBindingDescription vertex_binding = { 0, NGL_VK_GRASS_VERTEX_BYTES, VK_VERTEX_INPUT_RATE_VERTEX };
    VkVertexInputAttributeDescription attributes[3] = {
        { 0, 0, VK_FORMAT_R16G16_SNORM, 0 },        /* the root */
        { 1, 0, VK_FORMAT_R8G8B8A8_SNORM, 4 },      /* the shape */
        { 2, 0, VK_FORMAT_R8G8B8A8_SNORM, 8 },      /* the variety */
    };
    VkPipelineVertexInputStateCreateInfo vertex_input = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    VkPipelineInputAssemblyStateCreateInfo assembly = {
        VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
    VkPipelineViewportStateCreateInfo viewport = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
    VkPipelineRasterizationStateCreateInfo raster = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
    VkPipelineMultisampleStateCreateInfo multisample = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
    VkPipelineColorBlendAttachmentState blend_attachment = { 0 };
    VkPipelineColorBlendStateCreateInfo blend = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
    VkPipelineDepthStencilStateCreateInfo depth = { VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
    VkDynamicState dynamic_states[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dynamic = { VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
    VkGraphicsPipelineCreateInfo info = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
    VkResult result;

    binding.binding = 0;
    binding.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    binding.descriptorCount = 1;
    binding.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    set_layout_info.bindingCount = 1;
    set_layout_info.pBindings = &binding;
    CHECK(vkCreateDescriptorSetLayout(vk.device, &set_layout_info, NULL, &vk.grass_set_layout));
    pool_info.maxSets = 1;
    pool_info.poolSizeCount = 1;
    pool_info.pPoolSizes = &pool_size;
    CHECK(vkCreateDescriptorPool(vk.device, &pool_info, NULL, &vk.grass_pool));
    set_info.descriptorPool = vk.grass_pool;
    set_info.descriptorSetCount = 1;
    set_info.pSetLayouts = &vk.grass_set_layout;
    CHECK(vkAllocateDescriptorSets(vk.device, &set_info, &vk.grass_set));
    if (make_host_buffer((NGL_VK_GRASS_CONSTANTS + 1) * 4 * sizeof(float), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                         &vk.grass_constants, &vk.grass_constant_memory, &vk.grass_constant_data))
        return -1;
    memset(vk.grass_constant_data, 0, (NGL_VK_GRASS_CONSTANTS + 1) * 4 * sizeof(float));
    buffer_info.buffer = vk.grass_constants;
    write.dstSet = vk.grass_set;
    write.dstBinding = 0;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    write.pBufferInfo = &buffer_info;
    vkUpdateDescriptorSets(vk.device, 1, &write, 0, NULL);

    set_layouts[0] = set_layouts[1] = set_layouts[2] = vk.set_layout;
    set_layouts[3] = vk.grass_set_layout;
    layout_info.setLayoutCount = 4;
    layout_info.pSetLayouts = set_layouts;
    layout_info.pushConstantRangeCount = 1;
    layout_info.pPushConstantRanges = &place;
    CHECK(vkCreatePipelineLayout(vk.device, &layout_info, NULL, &vk.grass_layout));

    if (make_shader(grass_vert_spirv, sizeof grass_vert_spirv, &vertex_shader) ||
        make_shader(grass_frag_spirv, sizeof grass_frag_spirv, &fragment_shader))
        return -1;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vertex_shader;
    stages[0].pName = "main";
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fragment_shader;
    stages[1].pName = "main";
    vertex_input.vertexBindingDescriptionCount = 1;
    vertex_input.pVertexBindingDescriptions = &vertex_binding;
    vertex_input.vertexAttributeDescriptionCount = 3;
    vertex_input.pVertexAttributeDescriptions = attributes;
    assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
    assembly.primitiveRestartEnable = VK_TRUE;      /* 0xFFFF between blades */
    viewport.viewportCount = 1;
    viewport.scissorCount = 1;
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.cullMode = VK_CULL_MODE_NONE;            /* a blade is seen from both sides */
    raster.frontFace = VK_FRONT_FACE_CLOCKWISE;
    raster.lineWidth = 1.0f;
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    blend_attachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                      VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    blend.attachmentCount = 1;
    blend.pAttachments = &blend_attachment;
    depth.depthTestEnable = VK_TRUE;
    depth.depthWriteEnable = VK_TRUE;
    depth.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
    dynamic.dynamicStateCount = 2;
    dynamic.pDynamicStates = dynamic_states;

    info.stageCount = 2;
    info.pStages = stages;
    info.pVertexInputState = &vertex_input;
    info.pInputAssemblyState = &assembly;
    info.pViewportState = &viewport;
    info.pRasterizationState = &raster;
    info.pMultisampleState = &multisample;
    info.pColorBlendState = &blend;
    info.pDepthStencilState = &depth;
    info.pDynamicState = &dynamic;
    info.layout = vk.grass_layout;
    info.renderPass = vk.render_pass;
    info.subpass = 0;
    result = vkCreateGraphicsPipelines(vk.device, VK_NULL_HANDLE, 1, &info, NULL, &vk.grass_pipeline);
    vkDestroyShaderModule(vk.device, vertex_shader, NULL);
    vkDestroyShaderModule(vk.device, fragment_shader, NULL);
    if (result != VK_SUCCESS) {
        fprintf(stderr, "[ngl_vk] vkCreateGraphicsPipelines (grass) failed (%d)\n", (int)result);
        vk.grass_pipeline = VK_NULL_HANDLE;
        return -1;
    }
    return 0;
}

int ngl_vk_grass_mesh(const void *vertices, uint32_t vertex_count, const uint16_t *indices, uint32_t index_count)
{
    void *vertex_data = NULL, *index_data = NULL;

    if (!vk.grass_pipeline || !vertices || !indices || !vertex_count || !index_count || vk.grass_index_count)
        return -1;
    if (make_host_buffer((VkDeviceSize)vertex_count * NGL_VK_GRASS_VERTEX_BYTES, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                         &vk.grass_vertices, &vk.grass_vertex_memory, &vertex_data) ||
        make_host_buffer((VkDeviceSize)index_count * sizeof(uint16_t), VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                         &vk.grass_indices, &vk.grass_index_memory, &index_data))
        return -1;
    memcpy(vertex_data, vertices, (size_t)vertex_count * NGL_VK_GRASS_VERTEX_BYTES);
    memcpy(index_data, indices, (size_t)index_count * sizeof(uint16_t));
    vk.grass_index_count = index_count;
    return 0;
}

void ngl_vk_set_grass(const float *constants, uint32_t bend, uint32_t blade_texture,
                      const struct ngl_vk_grass_patch *patches, uint32_t count)
{
    uint32_t *flags;

    if (!vk.grass_constant_data || !constants)
        return;
    if (count > MAX_GRASS_PATCHES)
        count = MAX_GRASS_PATCHES;
    memcpy(vk.grass_constant_data, constants, NGL_VK_GRASS_CONSTANTS * 4 * sizeof(float));
    flags = (uint32_t *)((float *)vk.grass_constant_data + NGL_VK_GRASS_CONSTANTS * 4);
    flags[0] = bend ? 1u : 0u;
    vk.grass_blade = blade_texture;
    if (count)
        memcpy(vk.grass_patches, patches, (size_t)count * sizeof *patches);
    vk.grass_patch_count = count;
}

/* This frame's grass patches, after the opaque meshes; returns 1 when it bound its own buffers. */
static int draw_grass(void)
{
    VkDeviceSize no_offset = 0;
    VkDescriptorSet blade, untextured = vk.texture_count ? vk.textures[1].set : vk.set;
    uint32_t count = vk.grass_patch_count;

    vk.grass_patch_count = 0;   /* a frame's patches are drawn once */
    if (!vk.grass_pipeline || !vk.grass_index_count || !count)
        return 0;
    blade = vk.grass_blade && vk.grass_blade <= vk.texture_count ? vk.textures[vk.grass_blade].set : untextured;
    vkCmdBindPipeline(vk.commands, VK_PIPELINE_BIND_POINT_GRAPHICS, vk.grass_pipeline);
    vkCmdBindVertexBuffers(vk.commands, 0, 1, &vk.grass_vertices, &no_offset);
    vkCmdBindIndexBuffer(vk.commands, vk.grass_indices, 0, VK_INDEX_TYPE_UINT16);
    vkCmdBindDescriptorSets(vk.commands, VK_PIPELINE_BIND_POINT_GRAPHICS, vk.grass_layout, 2, 1, &blade, 0, NULL);
    vkCmdBindDescriptorSets(vk.commands, VK_PIPELINE_BIND_POINT_GRAPHICS, vk.grass_layout, 3, 1, &vk.grass_set, 0,
                            NULL);
    for (uint32_t i = 0; i < count; i++) {
        const struct ngl_vk_grass_patch *p = &vk.grass_patches[i];
        VkDescriptorSet sets[2];

        if (!p->color_texture || p->color_texture > vk.texture_count || !p->cell_texture ||
            p->cell_texture > vk.texture_count)
            continue;
        sets[0] = vk.textures[p->color_texture].set;
        sets[1] = vk.textures[p->cell_texture].set;
        vkCmdBindDescriptorSets(vk.commands, VK_PIPELINE_BIND_POINT_GRAPHICS, vk.grass_layout, 0, 2, sets, 0, NULL);
        vkCmdPushConstants(vk.commands, vk.grass_layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof p->place, p->place);
        vkCmdDrawIndexed(vk.commands,
                         p->index_count && p->index_count < vk.grass_index_count ? p->index_count
                                                                                 : vk.grass_index_count,
                         1, 0, 0, 0);
    }
    return 1;
}

uint32_t ngl_vk_mesh(const float *corners, uint32_t corner_count, const uint16_t *indices, uint32_t index_count)
{
    uint32_t n;

    if (!vk.device || !corners || !indices || !corner_count || !index_count || vk.mesh_count == MAX_MESHES ||
        corner_count > MESH_CORNERS - FRAME_CORNERS - vk.corners_used || index_count > MESH_INDICES - vk.indices_used)
        return 0;
    n = ++vk.mesh_count;
    vk.meshes[n].first_corner = vk.corners_used;
    vk.meshes[n].first_index = vk.indices_used;
    vk.meshes[n].index_count = index_count;
    memcpy(vk.mesh_corner_data + (size_t)vk.corners_used * CORNER_FLOATS, corners,
           (size_t)corner_count * CORNER_FLOATS * sizeof(float));
    memcpy(vk.mesh_index_data + vk.indices_used, indices, (size_t)index_count * sizeof(uint16_t));
    vk.corners_used += corner_count;
    vk.indices_used += index_count;
    return n;
}

uint32_t ngl_vk_frame_corners(const float *corners, uint32_t corner_count)
{
    uint32_t first = MESH_CORNERS - FRAME_CORNERS + vk.frame_corners_used;

    if (!vk.mesh_corner_data || !corners || !corner_count || corner_count > FRAME_CORNERS - vk.frame_corners_used)
        return 0;
    memcpy(vk.mesh_corner_data + (size_t)first * CORNER_FLOATS, corners,
           (size_t)corner_count * CORNER_FLOATS * sizeof(float));
    vk.frame_corners_used += corner_count;
    return first + 1;
}

void ngl_vk_set_meshes(const struct ngl_vk_mesh_draw *draws, uint32_t count)
{
    if (!vk.draws)
        return;
    if (count > MAX_MESH_DRAWS)
        count = MAX_MESH_DRAWS;
    if (count)
        memcpy(vk.draws, draws, (size_t)count * sizeof *draws);
    vk.draw_count = count;
}

/* The inverse of a 4 x 4 matrix (row by row), in double, by cofactors; 0 when it has none. */
static int inverted(const float m[16], double out[16])
{
    double a[16], cofactor[16], determinant = 0.0;

    for (int i = 0; i < 16; i++)
        a[i] = m[i];
    for (int row = 0; row < 4; row++) {
        for (int column = 0; column < 4; column++) {
            double minor[9];
            int n = 0;

            for (int r = 0; r < 4; r++) {
                for (int c = 0; c < 4; c++) {
                    if (r != row && c != column)
                        minor[n++] = a[r * 4 + c];
                }
            }
            cofactor[row * 4 + column] = ((row + column) & 1 ? -1.0 : 1.0) *
                                         (minor[0] * (minor[4] * minor[8] - minor[5] * minor[7]) -
                                          minor[1] * (minor[3] * minor[8] - minor[5] * minor[6]) +
                                          minor[2] * (minor[3] * minor[7] - minor[4] * minor[6]));
        }
    }
    for (int column = 0; column < 4; column++)
        determinant += a[column] * cofactor[column];
    if (!(determinant > 1e-30 || determinant < -1e-30))
        return 0;
    for (int row = 0; row < 4; row++) {         /* the adjugate (the cofactors transposed) over the determinant */
        for (int column = 0; column < 4; column++)
            out[row * 4 + column] = cofactor[column * 4 + row] / determinant;
    }
    return 1;
}

void ngl_vk_set_sun_shadow(const float light_view_projection[16], const float camera_view_projection[16])
{
    double inverse[16];

    vk.shadow_on = 0;
    if (!vk.shadow_uniform_data || !light_view_projection || !camera_view_projection ||
        !inverted(camera_view_projection, inverse))
        return;
    /* row vectors: the light's clip position = the camera's clip position x inverse(camera) x light; the shader's
     * mat4 reads it row by row as its transpose, as it does the world-view-projection */
    for (int row = 0; row < 4; row++) {
        for (int column = 0; column < 4; column++) {
            double sum = 0.0;

            for (int k = 0; k < 4; k++)
                sum += inverse[row * 4 + k] * light_view_projection[k * 4 + column];
            vk.shadow_uniform_data[row * 4 + column] = (float)sum;
        }
    }
    vk.shadow_uniform_data[16] = SHADOW_BIAS;
    vk.shadow_uniform_data[17] = 1.0f / (float)SHADOW_SIZE;
    vk.shadow_uniform_data[18] = 0.0f;
    vk.shadow_uniform_data[19] = 0.0f;
    memcpy(vk.shadow_light, light_view_projection, sizeof vk.shadow_light);
    vk.shadow_on = 1;
}

/* The flags word of a mesh draw's push constants (mesh.frag's SCENE_LIGHT and the rest) */
#define PUSH_SCENE_LIGHT        1u      /* lit by the scene's lights, not the fixed one */
#define PUSH_MIRRORED           2u      /* the world matrix's determinant is negative */
#define PUSH_MORE_LIGHTS_SHIFT  2u      /* how many lights after the first (0 to 3), 2 bits */
#define PUSH_HELD               16u     /* the light held to 1 before the texture */
#define PUSH_SHADOWED           32u     /* the first light dimmed by the sun's shadow map */
#define PUSH_MODE_SHIFT         8u      /* the draw's MESH_MODE_* bits, 8 of them */

/* A way toward a light as three 10-bit signed numbers (x in the low bits), scaled so its longest part is 511 (the
 * shader normalizes); 0 for none */
static uint32_t packed_toward(const float toward[3])
{
    float longest = 0.0f;
    uint32_t packed = 0;

    for (int i = 0; i < 3; i++) {
        float a = toward[i] < 0.0f ? -toward[i] : toward[i];

        if (a > longest)
            longest = a;
    }
    if (!(longest > 1e-20f) || !(longest < 1e30f))      /* none, or not a number */
        return 0;
    for (int i = 0; i < 3; i++) {
        float v = toward[i] / longest * 511.0f;
        int rounded = (int)(v < 0.0f ? v - 0.5f : v + 0.5f);

        packed |= ((uint32_t)rounded & 1023u) << (10 * i);
    }
    return packed;
}

/* A light's color, 0 to 4, as three 10-bit numbers (red in the low bits) */
static uint32_t packed_color(const float color[3])
{
    uint32_t packed = 0;

    for (int i = 0; i < 3; i++) {
        float v = color[i] * (1023.0f / 4.0f);

        packed |= (v > 0.0f ? (v < 1023.0f ? (uint32_t)(v + 0.5f) : 1023u) : 0u) << (10 * i);
    }
    return packed;
}

/* A mesh draw's push constants, 128 bytes (the least every device has): the world matrix (taken as affine: the
 * fourth column of a Direct3D world matrix is 0, 0, 0, 1) times the view-projection, row by row; the fog; the ambient
 * and the flags (PUSH_*: the scene's lights or the fixed one, mirrored, how many more lights, held to 1, and the
 * draw's MESH_MODE_* bits, mode, from draw_meshes); then for up to four lights the way toward each in the mesh's own
 * space, and their colors, packed. The shader lights by the corners' normals, or without them by the face's, taken
 * from the screen-space slopes of the mesh's own positions: that one faces the eye and turns over under a mirroring
 * world matrix, so the shader turns it back. As the games do (nglComputeObjectLights), a light goes into the mesh's
 * space as (-direction) x the world's inverse. */
static void push_mesh_draw(const struct ngl_vk_mesh_draw *d, uint32_t mode)
{
    static const float fixed_light[3] = { 0.35f, 0.25f, 0.9f };    /* toward it, from above (z is up) */
    const float *w = d->world;
    float matrix[16], inverse[9], determinant;
    uint32_t push[32], flags = (mode & 255u) << PUSH_MODE_SHIFT;
    uint32_t more = d->lit ? (d->extra_light_count < 3 ? d->extra_light_count : 3) : 0;
    int invertible;

    for (int row = 0; row < 4; row++) {
        for (int column = 0; column < 4; column++) {
            float sum = row == 3 ? d->view_projection[12 + column] : 0.0f;

            for (int k = 0; k < 3; k++)
                sum += w[row * 4 + k] * d->view_projection[k * 4 + column];
            matrix[row * 4 + column] = sum;
        }
    }
    memcpy(push, matrix, sizeof matrix);
    memcpy(push + 16, d->fog, sizeof d->fog);
    memcpy(push + 19, &d->fog_color, sizeof d->fog_color);
    memcpy(push + 20, d->ambient, sizeof d->ambient);

    /* the 3x3's inverse by cofactors (rows of w) */
    inverse[0] = w[5] * w[10] - w[6] * w[9];
    inverse[1] = w[2] * w[9] - w[1] * w[10];
    inverse[2] = w[1] * w[6] - w[2] * w[5];
    inverse[3] = w[6] * w[8] - w[4] * w[10];
    inverse[4] = w[0] * w[10] - w[2] * w[8];
    inverse[5] = w[2] * w[4] - w[0] * w[6];
    inverse[6] = w[4] * w[9] - w[5] * w[8];
    inverse[7] = w[1] * w[8] - w[0] * w[9];
    inverse[8] = w[0] * w[5] - w[1] * w[4];
    determinant = w[0] * inverse[0] + w[1] * inverse[3] + w[2] * inverse[6];
    invertible = determinant > 1e-12f || determinant < -1e-12f;
    if (invertible && determinant < 0)
        flags |= PUSH_MIRRORED;
    for (uint32_t light = 0; light < 4; light++) {
        const float *direction = light == 0 ? d->light_direction : d->extra_light_direction[light - 1];
        const float *color = light == 0 ? d->light_color : d->extra_light_color[light - 1];
        float toward[3], in_mesh[3];

        if (light > more) {
            push[24 + light] = push[28 + light] = 0;
            continue;
        }
        for (int i = 0; i < 3; i++)
            toward[i] = d->lit ? -direction[i] : fixed_light[i];
        /* row vector x inverse; the inverse's 1/determinant only scales, and the shader normalizes */
        for (int column = 0; column < 3; column++)
            in_mesh[column] = invertible ? toward[0] * inverse[column] + toward[1] * inverse[3 + column] +
                                           toward[2] * inverse[6 + column]
                                         : toward[column];
        push[24 + light] = packed_toward(in_mesh);
        push[28 + light] = packed_color(color);
    }
    if (d->lit)
        flags |= PUSH_SCENE_LIGHT | more << PUSH_MORE_LIGHTS_SHIFT | (d->light_saturate ? PUSH_HELD : 0u);
    if (d->sun_shadowed && vk.shadow_on)
        flags |= PUSH_SHADOWED;
    push[23] = flags;
    vkCmdPushConstants(vk.commands, vk.mesh_layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof push, push);
}

/* The sun's shadow map, before the main pass: cleared to the far end, then (with a sun's shadow this frame) every
 * shadow caster that isn't blended drawn into it through the light's view-projection, depth only, its alpha test kept.
 * Run once with no shadow too, so the map is in its sampled layout before any mesh draw binds it. */
static void draw_shadow(void)
{
    VkRenderPassBeginInfo pass = { VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
    VkClearValue clear;
    VkViewport view = { 0.0f, 0.0f, (float)SHADOW_SIZE, (float)SHADOW_SIZE, 0.0f, 1.0f };
    VkRect2D whole = { { 0, 0 }, { SHADOW_SIZE, SHADOW_SIZE } };
    VkDeviceSize no_offset = 0;
    VkDescriptorSet bound = VK_NULL_HANDLE;
    VkDescriptorSet untextured = vk.texture_count ? vk.textures[1].set : vk.set;

    clear.depthStencil.depth = 1.0f;
    clear.depthStencil.stencil = 0;
    pass.renderPass = vk.shadow_pass;
    pass.framebuffer = vk.shadow_framebuffer;
    pass.renderArea = whole;
    pass.clearValueCount = 1;
    pass.pClearValues = &clear;
    vkCmdBeginRenderPass(vk.commands, &pass, VK_SUBPASS_CONTENTS_INLINE);
    if (vk.shadow_on && vk.draw_count) {
        vkCmdSetViewport(vk.commands, 0, 1, &view);
        vkCmdSetScissor(vk.commands, 0, 1, &whole);
        vkCmdBindPipeline(vk.commands, VK_PIPELINE_BIND_POINT_GRAPHICS, vk.mesh_shadow_pipeline);
        vkCmdBindVertexBuffers(vk.commands, 0, 1, &vk.mesh_corners, &no_offset);
        vkCmdBindIndexBuffer(vk.commands, vk.mesh_indices, 0, VK_INDEX_TYPE_UINT16);
        vkCmdBindDescriptorSets(vk.commands, VK_PIPELINE_BIND_POINT_GRAPHICS, vk.mesh_layout, 3, 1, &vk.shadow_set,
                                0, NULL);
        for (uint32_t i = 0; i < vk.draw_count; i++) {
            const struct ngl_vk_mesh_draw *d = &vk.draws[i];
            int textured = d->texture && d->texture <= vk.texture_count;
            VkDescriptorSet set = textured ? vk.textures[d->texture].set : untextured;
            struct ngl_vk_mesh_draw caster;

            if (!d->shadow_caster || !d->mesh || d->mesh > vk.mesh_count || d->alpha == NGL_VK_ALPHA_BLEND)
                continue;
            if (set != bound) {
                vkCmdBindDescriptorSets(vk.commands, VK_PIPELINE_BIND_POINT_GRAPHICS, vk.mesh_layout, 0, 1, &set, 0,
                                        NULL);
                bound = set;
            }
            caster = *d;
            memcpy(caster.view_projection, vk.shadow_light, sizeof caster.view_projection);
            caster.sun_shadowed = 0;
            push_mesh_draw(&caster, 0);
            vkCmdDrawIndexed(vk.commands,
                             d->index_count && d->index_count < vk.meshes[d->mesh].index_count
                                 ? d->index_count : vk.meshes[d->mesh].index_count,
                             1, vk.meshes[d->mesh].first_index,
                             (int32_t)(d->corners ? d->corners - 1u : vk.meshes[d->mesh].first_corner),
                             (textured ? MESH_TEXTURED : 0u) | (d->alpha == NGL_VK_ALPHA_TEST ? MESH_ALPHA_TEST : 0u));
        }
    }
    vkCmdEndRenderPass(vk.commands);
    vk.shadow_cleared = 1;
}

/* This frame's meshes, in the viewport the quads use (the game's screen fitted to the target): first the opaque and
 * alpha-tested ones, then the blended ones over them. A draw without a texture or lightmap still binds a set there (the
 * shader declares the samplers): the first texture's, or the picture's. A draw with two textures mixed has its
 * second where the lightmap goes (set 1) and its third in set 2; a shiny one its environment map and its mask; one
 * multiplied by a third texture alone, that one in set 2. */
static void draw_meshes(void)
{
    VkDeviceSize no_offset = 0;
    VkDescriptorSet bound[3] = { VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE };
    VkDescriptorSet untextured = vk.texture_count ? vk.textures[1].set : vk.set;

    vkCmdBindVertexBuffers(vk.commands, 0, 1, &vk.mesh_corners, &no_offset);
    vkCmdBindIndexBuffer(vk.commands, vk.mesh_indices, 0, VK_INDEX_TYPE_UINT16);
    vkCmdBindDescriptorSets(vk.commands, VK_PIPELINE_BIND_POINT_GRAPHICS, vk.mesh_layout, 3, 1, &vk.shadow_set, 0,
                            NULL);
    for (int blended = 0; blended < 2; blended++) {
        VkPipeline pipeline = blended ? vk.mesh_blend_pipeline : vk.mesh_pipeline;

        if (blended && draw_grass()) {      /* the grass after the opaque meshes, with its own buffers and sets */
            vkCmdBindVertexBuffers(vk.commands, 0, 1, &vk.mesh_corners, &no_offset);
            vkCmdBindIndexBuffer(vk.commands, vk.mesh_indices, 0, VK_INDEX_TYPE_UINT16);
            vkCmdBindDescriptorSets(vk.commands, VK_PIPELINE_BIND_POINT_GRAPHICS, vk.mesh_layout, 3, 1,
                                    &vk.shadow_set, 0, NULL);
            bound[0] = bound[1] = bound[2] = VK_NULL_HANDLE;
        }
        vkCmdBindPipeline(vk.commands, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
        for (uint32_t i = 0; i < vk.draw_count; i++) {
            const struct ngl_vk_mesh_draw *d = &vk.draws[i];
            int textured = d->texture && d->texture <= vk.texture_count;
            int lightmapped = d->lightmap && d->lightmap <= vk.texture_count;
            int mixed = textured && d->blend_texture && d->blend_texture <= vk.texture_count;
            uint32_t shading = d->shading == NGL_VK_SHADE_LIGHTMAP && !lightmapped ? NGL_VK_SHADE_LIGHT
                                                                                    : d->shading & 3u;
            /* a shiny surface: its environment map where the lightmap goes (set 1), its mask in set 2 */
            int env = textured && !mixed && shading != NGL_VK_SHADE_LIGHTMAP && d->env_texture &&
                      d->env_texture <= vk.texture_count;
            int env_masked = env && d->env_mask && d->env_mask <= vk.texture_count;
            int third = !env && d->blend_light && d->blend_light <= vk.texture_count;
            uint32_t mode = (mixed ? MESH_MODE_MIX : 0u) | (third ? MESH_MODE_THIRD : 0u) |
                            (env ? MESH_MODE_SHINY : 0u) | (env_masked ? MESH_MODE_MASKED : 0u) |
                            (mixed && d->blend_by_alpha ? MESH_MODE_BY_ALPHA : 0u);
            VkDescriptorSet sets[3] = {
                textured ? vk.textures[d->texture].set : untextured,
                mixed ? vk.textures[d->blend_texture].set
                      : env ? vk.textures[d->env_texture].set
                            : lightmapped ? vk.textures[d->lightmap].set : untextured,
                third ? vk.textures[d->blend_light].set
                      : env_masked ? vk.textures[d->env_mask].set : untextured,
            };

            if (!d->mesh || d->mesh > vk.mesh_count || (d->alpha == NGL_VK_ALPHA_BLEND) != blended)
                continue;
            if (blended) {      /* in the order given: an effect's blending can change from one draw to the next */
                VkPipeline wanted = d->blend == NGL_VK_BLEND_ADD ? vk.mesh_add_pipeline
                                  : d->blend == NGL_VK_BLEND_SUBTRACT ? vk.mesh_subtract_pipeline
                                                                      : vk.mesh_blend_pipeline;

                if (wanted != pipeline) {
                    pipeline = wanted;
                    vkCmdBindPipeline(vk.commands, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
                    bound[0] = bound[1] = bound[2] = VK_NULL_HANDLE;
                }
            }
            if (sets[0] != bound[0] || sets[1] != bound[1] || sets[2] != bound[2]) {
                vkCmdBindDescriptorSets(vk.commands, VK_PIPELINE_BIND_POINT_GRAPHICS, vk.mesh_layout, 0, 3, sets, 0,
                                        NULL);
                memcpy(bound, sets, sizeof bound);
            }
            push_mesh_draw(d, mode);
            /* the tint, the shading, whether to sample the texture and the alpha test ride in as the instance number */
            vkCmdDrawIndexed(vk.commands,
                             d->index_count && d->index_count < vk.meshes[d->mesh].index_count
                                 ? d->index_count : vk.meshes[d->mesh].index_count,
                             1, vk.meshes[d->mesh].first_index,
                             (int32_t)(d->corners ? d->corners - 1u : vk.meshes[d->mesh].first_corner),
                             (d->tint & MESH_TINT) | shading << MESH_SHADING_SHIFT | (textured ? MESH_TEXTURED : 0u) |
                             (d->alpha == NGL_VK_ALPHA_TEST ? MESH_ALPHA_TEST : 0u) |
                             (d->light_once ? MESH_LIGHT_ONCE : 0u));
        }
    }
    vk.draw_count = 0;      /* a frame's meshes are drawn once, with that frame's shadow */
    vk.frame_corners_used = 0;
    vk.shadow_on = 0;
}

static void free_picture(void)
{
    if (vk.upload_memory)
        vkUnmapMemory(vk.device, vk.upload_memory);
    vkDestroyBuffer(vk.device, vk.upload, NULL);
    vkFreeMemory(vk.device, vk.upload_memory, NULL);
    vkDestroyImageView(vk.device, vk.picture_view, NULL);
    vkDestroyImage(vk.device, vk.picture, NULL);
    vkFreeMemory(vk.device, vk.picture_memory, NULL);
    vk.upload = VK_NULL_HANDLE;
    vk.upload_memory = VK_NULL_HANDLE;
    vk.upload_pixels = NULL;
    vk.picture_view = VK_NULL_HANDLE;
    vk.picture = VK_NULL_HANDLE;
    vk.picture_memory = VK_NULL_HANDLE;
    vk.picture_width = vk.picture_height = 0;
}

/* The sampled image for pictures of this size (made again when the size changes). */
static int make_picture(uint32_t width, uint32_t height)
{
    VkDescriptorImageInfo image_info;
    VkWriteDescriptorSet write = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };

    vkDeviceWaitIdle(vk.device);
    free_picture();
    if (make_image(width, height, 1, PICTURE_FORMAT, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                   &vk.picture, &vk.picture_memory, &vk.picture_view) ||
        make_host_buffer((VkDeviceSize)width * height * BYTES_PER_PIXEL, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                         &vk.upload, &vk.upload_memory, &vk.upload_pixels))
        return -1;
    vk.picture_width = width;
    vk.picture_height = height;

    image_info.sampler = vk.sampler;
    image_info.imageView = vk.picture_view;
    image_info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    write.dstSet = vk.set;
    write.dstBinding = 0;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.pImageInfo = &image_info;
    vkUpdateDescriptorSets(vk.device, 1, &write, 0, NULL);
    return 0;
}

static void image_barrier(VkImage image, VkImageLayout from, VkImageLayout to, VkAccessFlags from_access,
                          VkAccessFlags to_access, VkPipelineStageFlags from_stage, VkPipelineStageFlags to_stage);
static void levels_barrier(VkImage image, uint32_t levels, VkImageLayout from, VkImageLayout to,
                           VkAccessFlags from_access, VkAccessFlags to_access, VkPipelineStageFlags from_stage,
                           VkPipelineStageFlags to_stage);

uint32_t ngl_vk_texture(const void *pixels, uint32_t width, uint32_t height)
{
    return ngl_vk_texture_levels(&pixels, 1, width, height);
}

uint32_t ngl_vk_texture_levels(const void *const *levels, uint32_t level_count, uint32_t width, uint32_t height)
{
    VkCommandBufferBeginInfo begin = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    VkSubmitInfo submit = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    VkDescriptorSetAllocateInfo set_info = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
    VkWriteDescriptorSet write = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
    VkDescriptorImageInfo image_info;
    VkBufferImageCopy copy[MAX_TEXTURE_LEVELS];
    VkBuffer staging = VK_NULL_HANDLE;
    VkDeviceMemory staging_memory = VK_NULL_HANDLE;
    void *staging_pixels = NULL;
    VkDeviceSize bytes = 0;
    uint32_t n, level, most = 1;
    int failed = 0;

    if (!vk.device || !levels || !levels[0] || !level_count || !width || !height || vk.texture_count == MAX_TEXTURES)
        return 0;
    while (most < MAX_TEXTURE_LEVELS && ((width >> most) || (height >> most)))
        most++;                                         /* down to 1 x 1 */
    if (level_count > most)
        level_count = most;
    for (level = 0; level < level_count; level++) {     /* level i: max(1, width >> i) x max(1, height >> i) */
        uint32_t w = width >> level ? width >> level : 1, h = height >> level ? height >> level : 1;

        if (!levels[level]) {                           /* the levels given stop here */
            level_count = level;
            break;
        }
        memset(&copy[level], 0, sizeof copy[level]);
        copy[level].bufferOffset = bytes;
        copy[level].imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        copy[level].imageSubresource.mipLevel = level;
        copy[level].imageSubresource.layerCount = 1;
        copy[level].imageExtent.width = w;
        copy[level].imageExtent.height = h;
        copy[level].imageExtent.depth = 1;
        bytes += (VkDeviceSize)w * h * BYTES_PER_PIXEL;
    }
    n = vk.texture_count + 1;
    if (make_image(width, height, level_count, PICTURE_FORMAT,
                   VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, &vk.textures[n].image,
                   &vk.textures[n].memory, &vk.textures[n].view) ||
        make_host_buffer(bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, &staging, &staging_memory, &staging_pixels))
        failed = 1;
    if (!failed) {
        for (level = 0; level < level_count; level++)
            memcpy((uint8_t *)staging_pixels + copy[level].bufferOffset, levels[level],
                   (size_t)copy[level].imageExtent.width * copy[level].imageExtent.height * BYTES_PER_PIXEL);
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &vk.commands;
        failed = vkResetFences(vk.device, 1, &vk.done) != VK_SUCCESS ||
                 vkResetCommandBuffer(vk.commands, 0) != VK_SUCCESS ||
                 vkBeginCommandBuffer(vk.commands, &begin) != VK_SUCCESS;
    }
    if (!failed) {
        levels_barrier(vk.textures[n].image, level_count, VK_IMAGE_LAYOUT_UNDEFINED,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
                       VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        vkCmdCopyBufferToImage(vk.commands, staging, vk.textures[n].image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                               level_count, copy);
        levels_barrier(vk.textures[n].image, level_count, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                       VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
                       VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                       VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
        failed = vkEndCommandBuffer(vk.commands) != VK_SUCCESS ||
                 vkQueueSubmit(vk.queue, 1, &submit, vk.done) != VK_SUCCESS ||
                 vkWaitForFences(vk.device, 1, &vk.done, VK_TRUE, WAIT_NS) != VK_SUCCESS;
    }
    if (!failed) {
        set_info.descriptorPool = vk.descriptor_pool;
        set_info.descriptorSetCount = 1;
        set_info.pSetLayouts = &vk.set_layout;
        failed = vkAllocateDescriptorSets(vk.device, &set_info, &vk.textures[n].set) != VK_SUCCESS;
    }
    if (!failed) {
        image_info.sampler = vk.sampler;
        image_info.imageView = vk.textures[n].view;
        image_info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        write.dstSet = vk.textures[n].set;
        write.dstBinding = 0;
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        write.pImageInfo = &image_info;
        vkUpdateDescriptorSets(vk.device, 1, &write, 0, NULL);
    }
    if (staging_memory)
        vkUnmapMemory(vk.device, staging_memory);
    vkDestroyBuffer(vk.device, staging, NULL);
    vkFreeMemory(vk.device, staging_memory, NULL);
    if (failed) {
        vkDestroyImageView(vk.device, vk.textures[n].view, NULL);
        vkDestroyImage(vk.device, vk.textures[n].image, NULL);
        vkFreeMemory(vk.device, vk.textures[n].memory, NULL);
        memset(&vk.textures[n], 0, sizeof vk.textures[n]);
        fprintf(stderr, "[ngl_vk] a %ux%u texture couldn't be made\n", width, height);
        return 0;
    }
    vk.texture_count = n;
    return n;
}

int ngl_vk_init(uint32_t width, uint32_t height)
{
    VkApplicationInfo app = { VK_STRUCTURE_TYPE_APPLICATION_INFO };
    VkInstanceCreateInfo info = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };

    if (vk.device)
        return 0;
    if (!width || !height)
        return -1;
    vk.width = width;
    vk.height = height;
    app.pApplicationName = "ngl_vk";
    app.pEngineName = "ngl_vk";
    app.apiVersion = VK_API_VERSION_1_0;
    info.pApplicationInfo = &app;
    if (window.make_surface) {      /* with the window system's extensions, then without them if that fails */
        info.enabledExtensionCount = window.extension_count;
        info.ppEnabledExtensionNames = window.extensions;
        if (vkCreateInstance(&info, NULL, &vk.instance) != VK_SUCCESS) {
            fprintf(stderr, "[ngl_vk] no instance with the window's extensions: drawing offscreen\n");
            vk.instance = VK_NULL_HANDLE;
            info.enabledExtensionCount = 0;
            info.ppEnabledExtensionNames = NULL;
        } else if (window.make_surface(vk.instance, &vk.surface, window.user) || !vk.surface) {
            fprintf(stderr, "[ngl_vk] the window gave no surface: drawing offscreen\n");
            vk.surface = VK_NULL_HANDLE;
        }
    }
    if (!vk.instance)
        CHECK(vkCreateInstance(&info, NULL, &vk.instance));
    if (choose_gpu() || make_device() || make_target() || make_pipeline() || make_quad_pipeline() || make_shadow() ||
        make_mesh_pipeline()) {
        ngl_vk_shutdown();
        return -1;
    }
    if (make_grass_pipeline())
        fprintf(stderr, "[ngl_vk] no grass pipeline: the grass is left out\n");
    if (vk.surface && make_swapchain()) {
        fprintf(stderr, "[ngl_vk] no swapchain for the window: drawing offscreen\n");
        drop_window();
    }
    return 0;
}

const char *ngl_vk_device_name(void)
{
    return device_name;
}

static void image_barrier(VkImage image, VkImageLayout from, VkImageLayout to, VkAccessFlags from_access,
                          VkAccessFlags to_access, VkPipelineStageFlags from_stage, VkPipelineStageFlags to_stage)
{
    levels_barrier(image, 1, from, to, from_access, to_access, from_stage, to_stage);
}

/* The same for an image's first `levels` levels. */
static void levels_barrier(VkImage image, uint32_t levels, VkImageLayout from, VkImageLayout to,
                           VkAccessFlags from_access, VkAccessFlags to_access, VkPipelineStageFlags from_stage,
                           VkPipelineStageFlags to_stage)
{
    VkImageMemoryBarrier barrier = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };

    barrier.srcAccessMask = from_access;
    barrier.dstAccessMask = to_access;
    barrier.oldLayout = from;
    barrier.newLayout = to;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.levelCount = levels;
    barrier.subresourceRange.layerCount = 1;
    vkCmdPipelineBarrier(vk.commands, from_stage, to_stage, 0, 0, NULL, 0, NULL, 1, &barrier);
}

/* One pass: the picture in, then either the picture scaled to fit (vertices NULL) or the quads, in a viewport that
 * fits the game's screen (screen_width x screen_height) to the target. */
/* The swapchain image this draw shows in, acquired before recording (making the swapchain again first when the
 * window changed). Returns 1 with *index set, 0 when nothing is shown this time (offscreen, minimized, or the window
 * just changed). */
static int acquire_window_image(uint32_t *index)
{
    VkResult result;

    if (!vk.surface)
        return 0;
    if ((vk.swap_stale || !vk.swapchain) && make_swapchain()) {
        fprintf(stderr, "[ngl_vk] the window's swapchain couldn't be made again: drawing offscreen\n");
        drop_window();
        return 0;
    }
    if (!vk.swapchain)
        return 0;
    result = vkAcquireNextImageKHR(vk.device, vk.swapchain, WAIT_NS, vk.swap_ready, VK_NULL_HANDLE, index);
    if (result == VK_SUBOPTIMAL_KHR)
        vk.swap_stale = 1;                          /* shown this time, made again for the next */
    else if (result == VK_ERROR_OUT_OF_DATE_KHR) {
        vk.swap_stale = 1;
        return 0;
    } else if (result != VK_SUCCESS) {
        fprintf(stderr, "[ngl_vk] vkAcquireNextImageKHR failed (%d): drawing offscreen\n", (int)result);
        drop_window();
        return 0;
    }
    return *index < vk.swap_count;
}

/* The finished target (in TRANSFER_SRC_OPTIMAL) into the acquired image: black, then the target scaled to fit and
 * centered, then ready to be shown. */
static void record_window_copy(uint32_t index)
{
    VkImage image = vk.swap_images[index];
    VkClearColorValue black = { { 0.0f, 0.0f, 0.0f, 1.0f } };
    VkImageSubresourceRange range = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    VkImageBlit blit = { { 0 } };
    float scale_x = (float)vk.swap_extent.width / (float)vk.width;
    float scale_y = (float)vk.swap_extent.height / (float)vk.height;
    float scale = scale_x < scale_y ? scale_x : scale_y;
    int32_t width = (int32_t)((float)vk.width * scale + 0.5f), height = (int32_t)((float)vk.height * scale + 0.5f);
    int32_t x = ((int32_t)vk.swap_extent.width - width) / 2, y = ((int32_t)vk.swap_extent.height - height) / 2;

    image_barrier(image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
                  VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    vkCmdClearColorImage(vk.commands, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &range);
    image_barrier(image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                  VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                  VK_PIPELINE_STAGE_TRANSFER_BIT);
    blit.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    blit.srcSubresource.layerCount = 1;
    blit.srcOffsets[1].x = (int32_t)vk.width;
    blit.srcOffsets[1].y = (int32_t)vk.height;
    blit.srcOffsets[1].z = 1;
    blit.dstSubresource = blit.srcSubresource;
    blit.dstOffsets[0].x = x;
    blit.dstOffsets[0].y = y;
    blit.dstOffsets[1].x = x + width;
    blit.dstOffsets[1].y = y + height;
    blit.dstOffsets[1].z = 1;
    vkCmdBlitImage(vk.commands, vk.target, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, image,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);
    image_barrier(image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                  VK_ACCESS_TRANSFER_WRITE_BIT, 0, VK_PIPELINE_STAGE_TRANSFER_BIT,
                  VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
}

static int draw(const void *pixels, uint32_t width, uint32_t height, uint32_t pitch,
                const struct ngl_vk_vertex *quad_vertices, uint32_t vertex_count,
                float screen_width, float screen_height)
{
    VkCommandBufferBeginInfo begin = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    VkRenderPassBeginInfo pass = { VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
    VkClearValue clear[2] = { { { { 0.0f, 0.0f, 0.0f, 1.0f } } }, { { { 1.0f, 0.0f, 0.0f, 0.0f } } } };
    VkBufferImageCopy upload = { 0 }, download = { 0 };
    VkBufferMemoryBarrier to_host = { VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER };
    VkSubmitInfo submit = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    VkViewport view = { 0 };
    VkRect2D whole = { { 0, 0 }, { 0, 0 } };
    float scale_x, scale_y, scale, fit_width, fit_height;
    float screen[2] = { screen_width, screen_height };
    VkDeviceSize no_offset = 0;
    VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkPresentInfoKHR present = { VK_STRUCTURE_TYPE_PRESENT_INFO_KHR };
    uint32_t row, window_image = 0;
    int showing;

    if (!vk.device || !pixels || !width || !height || pitch < width * BYTES_PER_PIXEL)
        return -1;
    if ((width != vk.picture_width || height != vk.picture_height) && make_picture(width, height))
        return -1;
    for (row = 0; row < height; row++)
        memcpy((uint8_t *)vk.upload_pixels + (size_t)row * width * BYTES_PER_PIXEL,
               (const uint8_t *)pixels + (size_t)row * pitch, (size_t)width * BYTES_PER_PIXEL);

    showing = acquire_window_image(&window_image);
    CHECK(vkResetFences(vk.device, 1, &vk.done));
    CHECK(vkResetCommandBuffer(vk.commands, 0));
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    CHECK(vkBeginCommandBuffer(vk.commands, &begin));

    /* the picture in (the last draw has finished with the image: every draw waits) */
    image_barrier(vk.picture, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
                  VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    upload.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    upload.imageSubresource.layerCount = 1;
    upload.imageExtent.width = width;
    upload.imageExtent.height = height;
    upload.imageExtent.depth = 1;
    vkCmdCopyBufferToImage(vk.commands, vk.upload, vk.picture, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &upload);
    image_barrier(vk.picture, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                  VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                  VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);

    /* drawn scaled to fit, centered: the picture, or the game's screen the quads are placed on */
    fit_width = quad_vertices ? screen_width : (float)width;
    fit_height = quad_vertices ? screen_height : (float)height;
    scale_x = (float)vk.width / fit_width;
    scale_y = (float)vk.height / fit_height;
    scale = scale_x < scale_y ? scale_x : scale_y;
    view.width = fit_width * scale;
    view.height = fit_height * scale;
    view.x = ((float)vk.width - view.width) * 0.5f;
    view.y = ((float)vk.height - view.height) * 0.5f;
    view.maxDepth = 1.0f;
    whole.extent.width = vk.width;
    whole.extent.height = vk.height;
    pass.renderPass = vk.render_pass;
    pass.framebuffer = vk.framebuffer;
    pass.renderArea = whole;
    clear[1].depthStencil.depth = 1.0f;
    clear[1].depthStencil.stencil = 0;
    pass.clearValueCount = 2;
    pass.pClearValues = clear;
    /* the sun's shadow map first, outside the main pass (and once before any mesh draw binds it) */
    if (quad_vertices && vk.draw_count && (vk.shadow_on || !vk.shadow_cleared))
        draw_shadow();
    vkCmdBeginRenderPass(vk.commands, &pass, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdSetViewport(vk.commands, 0, 1, &view);
    vkCmdSetScissor(vk.commands, 0, 1, &whole);
    if (!quad_vertices) {
        vkCmdBindPipeline(vk.commands, VK_PIPELINE_BIND_POINT_GRAPHICS, vk.pipeline);
        vkCmdBindDescriptorSets(vk.commands, VK_PIPELINE_BIND_POINT_GRAPHICS, vk.pipeline_layout, 0, 1, &vk.set, 0,
                                NULL);
        vkCmdDraw(vk.commands, 3, 1, 0, 0);
    } else if (vertex_count || vk.draw_count) {
        uint32_t first = 0;

        if (vk.draw_count)
            draw_meshes();

        memcpy(vk.vertex_data, quad_vertices, (size_t)vertex_count * sizeof *quad_vertices);
        vkCmdBindPipeline(vk.commands, VK_PIPELINE_BIND_POINT_GRAPHICS, vk.quad_pipeline);
        vkCmdPushConstants(vk.commands, vk.quad_layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof screen, screen);
        vkCmdBindVertexBuffers(vk.commands, 0, 1, &vk.vertices, &no_offset);
        /* each run of vertices with one texture, with that texture's set */
        while (first < vertex_count) {
            uint32_t texture = quad_vertices[first].texture, end = first + 1;
            VkDescriptorSet set = texture && texture <= vk.texture_count ? vk.textures[texture].set : vk.set;

            while (end < vertex_count && quad_vertices[end].texture == texture)
                end++;
            vkCmdBindDescriptorSets(vk.commands, VK_PIPELINE_BIND_POINT_GRAPHICS, vk.quad_layout, 0, 1, &set, 0,
                                    NULL);
            vkCmdDraw(vk.commands, end - first, 1, first, 0);
            first = end;
        }
    }
    vkCmdEndRenderPass(vk.commands);

    /* the target out, for the host */
    download.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    download.imageSubresource.layerCount = 1;
    download.imageExtent.width = vk.width;
    download.imageExtent.height = vk.height;
    download.imageExtent.depth = 1;
    vkCmdCopyImageToBuffer(vk.commands, vk.target, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, vk.readback, 1, &download);
    to_host.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    to_host.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    to_host.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_host.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_host.buffer = vk.readback;
    to_host.size = VK_WHOLE_SIZE;
    vkCmdPipelineBarrier(vk.commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 0, NULL, 1,
                         &to_host, 0, NULL);
    if (showing)
        record_window_copy(window_image);
    CHECK(vkEndCommandBuffer(vk.commands));

    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &vk.commands;
    if (showing) {
        submit.waitSemaphoreCount = 1;
        submit.pWaitSemaphores = &vk.swap_ready;
        submit.pWaitDstStageMask = &wait_stage;
        submit.signalSemaphoreCount = 1;
        submit.pSignalSemaphores = &vk.swap_shown[window_image];
    }
    CHECK(vkQueueSubmit(vk.queue, 1, &submit, vk.done));
    if (showing) {
        VkResult shown;

        present.waitSemaphoreCount = 1;
        present.pWaitSemaphores = &vk.swap_shown[window_image];
        present.swapchainCount = 1;
        present.pSwapchains = &vk.swapchain;
        present.pImageIndices = &window_image;
        shown = vkQueuePresentKHR(vk.queue, &present);
        if (shown == VK_ERROR_OUT_OF_DATE_KHR || shown == VK_SUBOPTIMAL_KHR)
            vk.swap_stale = 1;
        else if (shown != VK_SUCCESS)
            fprintf(stderr, "[ngl_vk] vkQueuePresentKHR failed (%d)\n", (int)shown);
    }
    CHECK(vkWaitForFences(vk.device, 1, &vk.done, VK_TRUE, WAIT_NS));
    vk.drawn = 1;
    vk.shadow_on = 0;       /* a sun's shadow is set for one frame */
    return 0;
}

int ngl_vk_draw_picture(const void *pixels, uint32_t width, uint32_t height, uint32_t pitch)
{
    return draw(pixels, width, height, pitch, NULL, 0, 0.0f, 0.0f);
}

int ngl_vk_draw_quads(const struct ngl_vk_vertex *vertices, uint32_t count, float screen_width, float screen_height,
                      const void *pixels, uint32_t width, uint32_t height, uint32_t pitch)
{
    static const uint32_t white = 0xFFFFFFFFu;

    if (!vertices || count > MAX_VERTICES || screen_width <= 0.0f || screen_height <= 0.0f)
        return -1;
    if (!pixels) {
        pixels = &white;
        width = height = 1;
        pitch = BYTES_PER_PIXEL;
    }
    return draw(pixels, width, height, pitch, vertices, count, screen_width, screen_height);
}

const uint8_t *ngl_vk_target_pixels(uint32_t *width, uint32_t *height)
{
    if (!vk.drawn)
        return NULL;
    *width = vk.width;
    *height = vk.height;
    return (const uint8_t *)vk.readback_pixels;
}

void ngl_vk_shutdown(void)
{
    uint32_t row;

    if (vk.device) {
        vkDeviceWaitIdle(vk.device);
        free_swapchain();
        free_picture();
        for (row = 1; row <= vk.texture_count; row++) {
            vkDestroyImageView(vk.device, vk.textures[row].view, NULL);
            vkDestroyImage(vk.device, vk.textures[row].image, NULL);
            vkFreeMemory(vk.device, vk.textures[row].memory, NULL);
        }
        if (vk.mesh_corner_memory)
            vkUnmapMemory(vk.device, vk.mesh_corner_memory);
        if (vk.mesh_index_memory)
            vkUnmapMemory(vk.device, vk.mesh_index_memory);
        vkDestroyBuffer(vk.device, vk.mesh_corners, NULL);
        vkDestroyBuffer(vk.device, vk.mesh_indices, NULL);
        vkFreeMemory(vk.device, vk.mesh_corner_memory, NULL);
        vkFreeMemory(vk.device, vk.mesh_index_memory, NULL);
        vkDestroyPipeline(vk.device, vk.mesh_pipeline, NULL);
        vkDestroyPipeline(vk.device, vk.mesh_blend_pipeline, NULL);
        vkDestroyPipeline(vk.device, vk.mesh_add_pipeline, NULL);
        vkDestroyPipeline(vk.device, vk.mesh_subtract_pipeline, NULL);
        vkDestroyPipeline(vk.device, vk.mesh_shadow_pipeline, NULL);
        vkDestroyPipelineLayout(vk.device, vk.mesh_layout, NULL);
        if (vk.shadow_uniform_memory)
            vkUnmapMemory(vk.device, vk.shadow_uniform_memory);
        vkDestroyBuffer(vk.device, vk.shadow_uniform, NULL);
        vkFreeMemory(vk.device, vk.shadow_uniform_memory, NULL);
        vkDestroyDescriptorPool(vk.device, vk.shadow_pool, NULL);
        vkDestroyDescriptorSetLayout(vk.device, vk.shadow_set_layout, NULL);
        vkDestroySampler(vk.device, vk.shadow_sampler, NULL);
        vkDestroyFramebuffer(vk.device, vk.shadow_framebuffer, NULL);
        vkDestroyRenderPass(vk.device, vk.shadow_pass, NULL);
        vkDestroyImageView(vk.device, vk.shadow_view, NULL);
        vkDestroyImage(vk.device, vk.shadow_image, NULL);
        vkFreeMemory(vk.device, vk.shadow_memory, NULL);
        if (vk.grass_constant_memory)
            vkUnmapMemory(vk.device, vk.grass_constant_memory);
        if (vk.grass_vertex_memory)
            vkUnmapMemory(vk.device, vk.grass_vertex_memory);
        if (vk.grass_index_memory)
            vkUnmapMemory(vk.device, vk.grass_index_memory);
        vkDestroyBuffer(vk.device, vk.grass_constants, NULL);
        vkDestroyBuffer(vk.device, vk.grass_vertices, NULL);
        vkDestroyBuffer(vk.device, vk.grass_indices, NULL);
        vkFreeMemory(vk.device, vk.grass_constant_memory, NULL);
        vkFreeMemory(vk.device, vk.grass_vertex_memory, NULL);
        vkFreeMemory(vk.device, vk.grass_index_memory, NULL);
        vkDestroyPipeline(vk.device, vk.grass_pipeline, NULL);
        vkDestroyPipelineLayout(vk.device, vk.grass_layout, NULL);
        vkDestroyDescriptorPool(vk.device, vk.grass_pool, NULL);
        vkDestroyDescriptorSetLayout(vk.device, vk.grass_set_layout, NULL);
        if (vk.vertex_memory)
            vkUnmapMemory(vk.device, vk.vertex_memory);
        vkDestroyBuffer(vk.device, vk.vertices, NULL);
        vkFreeMemory(vk.device, vk.vertex_memory, NULL);
        vkDestroyPipeline(vk.device, vk.quad_pipeline, NULL);
        vkDestroyPipelineLayout(vk.device, vk.quad_layout, NULL);
        vkDestroyPipeline(vk.device, vk.pipeline, NULL);
        vkDestroyPipelineLayout(vk.device, vk.pipeline_layout, NULL);
        vkDestroyDescriptorPool(vk.device, vk.descriptor_pool, NULL);
        vkDestroyDescriptorSetLayout(vk.device, vk.set_layout, NULL);
        vkDestroySampler(vk.device, vk.sampler, NULL);
        if (vk.readback_memory)
            vkUnmapMemory(vk.device, vk.readback_memory);
        vkDestroyBuffer(vk.device, vk.readback, NULL);
        vkFreeMemory(vk.device, vk.readback_memory, NULL);
        vkDestroyFramebuffer(vk.device, vk.framebuffer, NULL);
        vkDestroyRenderPass(vk.device, vk.render_pass, NULL);
        vkDestroyImageView(vk.device, vk.target_view, NULL);
        vkDestroyImage(vk.device, vk.target, NULL);
        vkFreeMemory(vk.device, vk.target_memory, NULL);
        vkDestroyImageView(vk.device, vk.depth_view, NULL);
        vkDestroyImage(vk.device, vk.depth, NULL);
        vkFreeMemory(vk.device, vk.depth_memory, NULL);
        vkDestroyFence(vk.device, vk.done, NULL);
        vkDestroyCommandPool(vk.device, vk.command_pool, NULL);
        vkDestroyDevice(vk.device, NULL);
    }
    if (vk.instance && vk.surface)
        vkDestroySurfaceKHR(vk.instance, vk.surface, NULL);
    if (vk.instance)
        vkDestroyInstance(vk.instance, NULL);
    free(vk.draws);
    memset(&vk, 0, sizeof vk);
    device_name[0] = '\0';
}
