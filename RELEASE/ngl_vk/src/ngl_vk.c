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
 * the swapchain is out of date) makes the swapchain again at the next draw.
 *
 * On someone else's device (ngl_vk_init_on_device, Call of Duty 3's: the SDK presenter's) there is no window of ngl_vk's
 * own. ngl_vk_output then draws the target into the owner's image, through the game's gamma table, on the same queue
 * after the draw; each use of that queue is held through the owner's lock. With ngl_vk_set_readback(0) the target is
 * not copied out for the host at all.
 *
 * [rexglue-native] 2026-11-09: the Vulkan loader is opened when ngl_vk starts, not linked (ngl_vk_functions.h), so the
 * same code builds for Windows with llvm-mingw and runs where vulkan-1.dll is; without a loader ngl_vk_init fails. */
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
/* [rexglue-native] 2026-11-09: vulkan.h's types only; its functions are ngl_vk_functions.h's pointers of the same names */
#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif
#include <vulkan/vulkan.h>

#include "ngl_vk.h"
#include "ngl_vk_backend.h"  /* [acct2-bro] 2026-11-08: a lower layer records frames on ngl_vk's device */
#include "ngl_vk_functions.h"  /* [rexglue-native] 2026-11-09: the Vulkan loader, opened at run time */

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
/* [rexglue-native] 2026-11-09: mesh.frag without the fabric (NGL_VK_FOUR_SETS), for a device that binds fewer than six
 * descriptor sets */
static const uint32_t mesh_four_sets_frag_spirv[] =
#include "mesh_four_sets.frag.inc"
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
static const uint32_t dof_blur_frag_spirv[] =
#include "dof_blur.frag.inc"
;
static const uint32_t dof_mix_frag_spirv[] =
#include "dof_mix.frag.inc"
;
static const uint32_t tint_frag_spirv[] =
#include "tint.frag.inc"
;
static const uint32_t output_frag_spirv[] =
#include "output.frag.inc"
;

#define TARGET_FORMAT   VK_FORMAT_R8G8B8A8_UNORM
#define DEPTH_FORMAT    VK_FORMAT_D32_SFLOAT
#define PICTURE_FORMAT  VK_FORMAT_B8G8R8A8_UNORM     /* X8R8G8B8 in memory: blue, green, red, unused */
#define BYTES_PER_PIXEL 4u
#define WAIT_NS         5000000000ull               /* 5 s: a draw that takes longer has hung */
#define MAX_SWAP_IMAGES 8u                          /* a window's swapchain images */
#define MAX_VERTICES    (8192u * 6u)                /* 8192 quads a draw */
#define MAX_TEXTURES    2048u                       /* textures made with ngl_vk_texture */
#define DOF_SETS        2u                          /* the depth of field's two blurred pictures' sets */
#define POST_SETS       2u                          /* the tint's copy of the target, the last frame's (motion blur) */
#define OUTPUT_SETS     2u                          /* ngl_vk_output's: the target, the color table */
#define EMPTY_CUBE_SETS 1u                          /* [rexglue-native] rn34: the empty cube's (make_empty_cube) */
#define MAX_TEXTURE_LEVELS 14u                      /* a texture's levels: 8192 x 8192 down to 1 x 1 */
#define MAX_MESHES      65536u                      /* meshes made with ngl_vk_mesh */
#define FRAME_CORNERS   (512u << 10)                /* a frame's own corners (skinned meshes), at the buffer's end */
/* corners all meshes share: the meshes' own (3.5M, as before), then two frames' own (side214: the frame being
   built writes one half while the GPU may still draw the last frame from the other, so it needn't wait) */
#define MESH_CORNERS    ((4u << 20) + FRAME_CORNERS)
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
#define MESH_MODE_BUMPED    32u     /* a normal map (set 1) bends the normal (Call of Duty 3's per-pixel lit models) */
#define MESH_MODE_SPECULAR  64u     /* and a highlight times a specular map (set 2) */
#define MESH_MODE_LIGHTMAP_SUN 128u /* Call of Duty 3's lightmapped world: the lightmap x (1 + up x the normal's up
                                     * part) + the sun x the lightmap's alpha (ngl_vk.h lightmap_scale) */
#define MESH_INDICES    (16u << 20)                 /* 16-bit indices all meshes share: 32 MB */
#define STAGING_HALF    ((VkDeviceSize)32u << 20)   /* side220: a frame's corners and indices on their way to the GPU */
#define MAX_STAGED_COPIES 4096u                     /* copies waiting in one half, for each of the two buffers */
/* [rexglue-native] 2026-11-09: batched textures (ngl_vk_set_batched_textures): a frame's new textures on their way */
#define TEXTURE_STAGING_HALF ((VkDeviceSize)48u << 20)
#define MAX_PENDING_TEXTURES 1024u                  /* new textures waiting in one half */
#define MAX_MESH_DRAWS  16384u                      /* meshes a frame */
#define DRAW_LIGHT_FLOATS 80u                       /* a draw's own lights, 20 vec4s: 2 nearby lights (place in its
                                                     * mesh's space and inverse range, then color and whether it is
                                                     * there), the 9 irradiance rows (row 0's w 1 when there), the
                                                     * fabric's numbers (FABRIC_FLOATS_AT, 3 vec4s), (rn34) the gun
                                                     * metal's last ones (METAL_FLOATS_AT, 4 vec4s) */
/* [rexglue-native] 2026-11-09: where a slot holds the fabric's numbers (mesh.frag's FABRIC_AT, vec4 13): the swatch's
 * repeat, the diffuse scale, the highlights' power, 1 for a fabric draw; then the sheen's three powers and (rn30) the
 * shadow's floor; then (vec4 15) the shadow's fade with the view depth (ngl_vk.h fabric_shadow_floor) and (rn32) 1 for
 * the soldiers' skin, whose vec4 14 holds the edge lift, the specular map's scale and the noise swatch's share in the
 * sheen's place (ngl_vk.h noise_swatch) */
#define FABRIC_FLOATS_AT 52u
/* [rexglue-native] 2026-11-09 rn34: the gun metal (ngl_vk.h metal_cube) in the fabric's three vec4s: (texture_brightness,
 * Reflection_Amt, SpecularPower, 0: not fabric), (Metal_Hue's red, green, blue, the rim's edge), (the rim's scale, 0, 1
 * for metal, 1 when there is a scratch map); then from METAL_FLOATS_AT (vec4 16) the rim's color and the cube's bias,
 * and the world matrix's first three columns (vec4s 17 to 19: a direction in the mesh's space turned into world space,
 * one part each). rn36: the vehicles' detail-mapped metal (ngl_vk.h detail_map) as a fabric draw with no sheen
 * (powers 1) and vec4 15's w 1, the detail's repeat and the highlights' power in the swatch's and the fabric's places,
 * and at METAL_FLOATS_AT the normal map's and the detail's z scales. */
#define METAL_FLOATS_AT 64u
#define MESH_SETS_FABRIC 7u                         /* the mesh layout's sets with the fabric's two (4 and 5) and
                                                     * (rn34) the gun metal's cube (6) */
#define MAX_DRAW_LIGHT_SLOTS 4096u                  /* draws a frame with their own lights */
#define MAX_GRASS_PATCHES 1024u                     /* grass patches a frame */
#define SHADOW_SIZE     2048u                       /* the sun's shadow map, a side */
#define SHADOW_BIAS     0.0004f                     /* how much nearer the sun a caster must be (of its depth range) */
/* What a frame's main pass waits for before it clears and writes the target and the depth: the last frame's depth
 * tests, its copies of the target (out for the host, into the window) and ngl_vk_output's reads of it */
#define TARGET_LAST_USES (VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT | \
                          VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT)

#define CHECK(call)                                                                 \
    do {                                                                            \
        VkResult result_ = (call);                                                  \
        if (result_ != VK_SUCCESS) {                                                \
            fprintf(stderr, "[ngl_vk] %s failed (%d)\n", #call, (int)result_);      \
            return -1;                                                              \
        }                                                                           \
    } while (0)

/* A non-dispatchable Vulkan handle (VkImage, VkImageView) from the 64-bit value ngl_vk.h passes it as: a pointer on
 * 64-bit builds, a 64-bit integer on 32-bit ones */
#if UINTPTR_MAX > 0xFFFFFFFFu
#define HANDLE_FROM_BITS(type, bits) ((type)(uintptr_t)(bits))
#else
#define HANDLE_FROM_BITS(type, bits) ((type)(bits))
#endif

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
        int cube;                       /* [rexglue-native] rn34: a cube (ngl_vk_texture_cube), not a 2D texture */
    } textures[MAX_TEXTURES + 1];
    uint32_t texture_count;
    /* [rexglue-native] 2026-11-09 rn34: a 1 x 1 black cube, bound in set 6 for every draw that has no cube of its own
     * (mesh.frag declares the gun metal's cube there) */
    VkImage empty_cube;
    VkDeviceMemory empty_cube_memory;
    VkImageView empty_cube_view;
    VkDescriptorSet empty_cube_set;
    /* [rexglue-native] 2026-11-09: batched textures (ngl_vk_set_batched_textures). A new texture's levels wait in this
     * frame's half of a staging buffer; their copies are recorded at the start of the next draw's commands (that draw's
     * frame then owns the half until it is done, finish_frame) and the next textures go in the other half, as the
     * meshes' staging does (side220). */
    int batch_textures;
    VkBuffer texture_staging;
    VkDeviceMemory texture_staging_memory;
    uint8_t *texture_staging_data;
    VkDeviceSize texture_staged;        /* bytes used in this half */
    uint32_t texture_staging_half;
    uint32_t pending_count;             /* new textures waiting in this half */
    struct {
        uint32_t texture;               /* its number */
        uint32_t level_count;
        VkBufferImageCopy copies[MAX_TEXTURE_LEVELS];   /* each level's place in the staging buffer */
    } pending[MAX_PENDING_TEXTURES];
    uint64_t batched_textures, texture_flushes;          /* since ngl_vk_take_texture_uploads */

    /* 2D quads */
    VkPipelineLayout quad_layout;
    VkPipeline quad_pipeline;
    VkBuffer vertices;
    VkDeviceMemory vertex_memory;
    void *vertex_data;
    /* 3D meshes: every mesh's corners and indices, and this frame's draws */
    VkPipelineLayout mesh_layout;
    int fabric_sets;                    /* [rexglue-native] 1: the layout has the fabric's sets 4 and 5 (mesh.frag) */
    VkPipeline mesh_pipeline;
    VkPipeline mesh_blend_pipeline;     /* the same, blended by alpha, not writing depth */
    VkPipeline mesh_add_pipeline;       /* and added (NGL_VK_BLEND_ADD), taken away (NGL_VK_BLEND_SUBTRACT) */
    VkPipeline mesh_subtract_pipeline;
    VkPipeline mesh_multiply_pipeline;  /* [waw-decomp] 2026-11-10: multiplied (NGL_VK_BLEND_MULTIPLY: decals) */
    VkBuffer mesh_corners, mesh_indices;
    VkDeviceMemory mesh_corner_memory, mesh_index_memory;
    float *mesh_corner_data;        /* mapped, when the corners are in memory the host sees (side220: not by default) */
    uint16_t *mesh_index_data;
    /* side220: the corners and indices in the GPU's own memory (the GPU had read every corner over the bus every
     * frame), written through a staging buffer of two halves: the copies wait in this frame's half and are recorded at
     * the start of the next draw, then that draw's frame owns the half until it is done (finish_frame) */
    int geometry_on_gpu;
    int geometry_in_host_memory;    /* ngl_vk_set_geometry_on_gpu(0): the old way, for comparing */
    VkBuffer staging;
    VkDeviceMemory staging_memory;
    uint8_t *staging_data;
    VkDeviceSize staged;            /* bytes used in this half */
    uint32_t staging_half;
    uint32_t copy_count[2];         /* copies waiting, into the corners (0) and into the indices (1) */
    VkBufferCopy copies[2][MAX_STAGED_COPIES];
    uint64_t staged_bytes, staging_flushes;    /* since ngl_vk_take_staging */
    uint32_t corners_used, indices_used, frame_corners_used;
    uint32_t frame_corner_half;     /* side214: which half of the frames' corners this frame writes (0 or 1) */
    int frame_corners_written;      /* this frame wrote some, so the next one takes the other half */
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
    /* [rexglue-native] 2026-11-09: ngl_vk_set_grass_scissor's part (a width or height of 0: the whole target) */
    int32_t grass_scissor_x, grass_scissor_y;
    uint32_t grass_scissor_width, grass_scissor_height;
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
    VkBuffer draw_lights;           /* set 3 binding 3: each draw's own lights (DRAW_LIGHT_FLOATS) */
    VkDeviceMemory draw_light_memory;
    float *draw_light_data;
    uint32_t draw_light_slots;      /* this frame's draws with their own lights (ngl_vk_set_meshes) */
    VkPipeline mesh_shadow_pipeline;
    int shadow_cleared;             /* the map has been through its pass once (it is in its sampled layout) */
    int shadow_on;                  /* this frame has a sun's shadow */
    float shadow_light[16];         /* this frame's light view-projection */
    /* soft effects (a draw's soft_depth): the main pass in two halves, the opaque meshes' depth copied between them,
     * for blended draws to fade where they near what is behind them (Call of Duty 3's particles) */
    VkRenderPass opaque_pass;       /* the first half: both attachments cleared, the depth kept for the copy */
    VkRenderPass blend_pass;        /* the second: both loaded */
    VkImage soft_depth;             /* the copy, sampled by mesh.frag (set 3, binding 2) */
    VkDeviceMemory soft_depth_memory;
    VkImageView soft_depth_view;
    VkSampler soft_sampler;
    int soft_ready;                 /* the copy is in its sampled layout */
    int soft_live;                  /* drawing the second half: the copy holds this frame's depth */
    /* depth of field (ngl_vk_set_depth_of_field): after the second half, the target shrunk to a half and a quarter of
     * its size (two blits), blurred across into dof_temp and down back into dof_quarter, then mixed over the target
     * by the opaque depth in the overlay pass, the main pass's third part, which the quads are drawn in after it */
    VkImage dof_half, dof_quarter, dof_temp;
    VkDeviceMemory dof_half_memory, dof_quarter_memory, dof_temp_memory;
    VkImageView dof_half_view, dof_quarter_view, dof_temp_view;
    uint32_t dof_width, dof_height;     /* the quarter's size */
    VkRenderPass dof_blur_pass;         /* one color attachment at the quarter's size, left ready to sample */
    VkFramebuffer dof_quarter_framebuffer, dof_temp_framebuffer;
    VkDescriptorSet dof_quarter_set, dof_temp_set;
    VkPipelineLayout dof_blur_layout, dof_mix_layout;
    VkPipeline dof_blur_pipeline, dof_mix_pipeline;
    VkRenderPass overlay_pass;          /* the target loaded (and left to be copied out), the depth not used */
    int dof_on;                         /* this frame has depth of field */
    float dof_focus[4];                 /* its near depth, far depth, keep depth and blanket */
    /* a tint (ngl_vk_set_tint): after the depth of field, the target copied whole and drawn back over itself, toned */
    VkImage post_copy;
    VkDeviceMemory post_copy_memory;
    VkImageView post_copy_view;
    VkDescriptorSet post_copy_set;
    VkPipelineLayout tint_layout;
    VkPipeline tint_pipeline;
    VkImage post_history;               /* the last frame's 3D after the tint's pass (ngl_vk_set_motion_blur) */
    VkDeviceMemory post_history_memory;
    VkImageView post_history_view;
    VkDescriptorSet post_history_set;
    int history_layout_ok;              /* post_history has been put in its sampled layout once */
    int motion_on;                      /* this frame blends with the last one (and keeps itself for the next) */
    uint64_t frames_done, history_frame; /* frames drawn; the one post_history holds (0: none) */
    int tint_on;                        /* this frame has a tint (or a brightness, or a motion blur) */
    uint32_t lookup_table;              /* ngl_vk_set_color_lookup's table this frame (0: none) */
    float tint[24];                     /* the weights times the exposure, the amount, the color, the keep depth; then
                                         * the brightness and the power (ngl_vk_set_brightness; 1, 1 for none), the
                                         * last frame's share (ngl_vk_set_motion_blur; 0 for none), the color look-up
                                         * (1 on); [rexglue-native] 2026-11-10 rn41b: then post effect 7's twelve
                                         * (ngl_vk_set_film; all 0 for none), as tint.frag's film, film_gray and
                                         * film_move */
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
    /* someone else's device (ngl_vk_init_on_device): left to its owner at shutdown, its queue held through theirs */
    int borrowed;
    ngl_vk_queue_lock queue_lock;
    ngl_vk_queue_unlock queue_unlock;
    void *queue_user;
    int no_readback;                /* ngl_vk_set_readback(0) */
    int frame_pending;              /* a draw's work may still be on the GPU (frames overlap on a borrowed device) */
    uint64_t gpu_wait_ns;           /* side219: time finish_frame waited for the GPU, since ngl_vk_take_gpu_wait */
    uint32_t gpu_waits;             /* and how many times it waited */
    int read_back;                  /* the last draw copied the target out */
    /* ngl_vk_output: the target drawn into another image through a color table, in a pass made for that image's
     * format, with its own command buffer, fence and framebuffer (made each time, freed the next) */
    VkFormat output_format;
    VkRenderPass output_pass;
    VkPipelineLayout output_layout;
    VkPipeline output_pipeline;
    VkDescriptorSet output_target_set, output_table_set;
    VkImage table_image;            /* 256 x 1, the table's red, green and blue */
    VkDeviceMemory table_memory;
    VkImageView table_view;
    VkBuffer table_upload;
    VkDeviceMemory table_upload_memory;
    void *table_upload_data;
    uint32_t table_words[256];      /* what table_image holds */
    int table_ready;                /* table_image has been filled once (and is in its sampled layout) */
    VkCommandBuffer output_commands;
    VkFence output_done;
    int output_pending;             /* output_done waits on the last output */
    VkFramebuffer output_framebuffer;
    /* [acct2-bro] 2026-11-08: a frame recorded by a lower layer (ngl_vk_begin_frame .. ngl_vk_end_frame) */
    int low_frame_open;             /* begun and not yet ended */
    int low_showing;                /* it has a window image (acquired at the begin) */
    uint32_t low_window_image;
    VkPhysicalDeviceFeatures enabled_features;  /* the device's, as its owner says (ngl_vk_set_device_features) */
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

/* A buffer in memory the host can see, mapped; in memory the host also caches, when there is some and cached is
   set (for a buffer the host reads: uncached memory takes a bus trip for every read, a third of a second for a
   1280x720 picture read byte by byte, side122). */
static int make_host_buffer_cached(VkDeviceSize size, VkBufferUsageFlags usage, int cached, VkBuffer *buffer,
                                   VkDeviceMemory *memory, void **mapped)
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
    if ((!cached || find_memory(needs.memoryTypeBits,
                                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT |
                                    VK_MEMORY_PROPERTY_HOST_CACHED_BIT,
                                &allocation.memoryTypeIndex)) &&
        find_memory(needs.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                    &allocation.memoryTypeIndex)) {
        fprintf(stderr, "[ngl_vk] no host-visible memory for a buffer\n");
        return -1;
    }
    CHECK(vkAllocateMemory(vk.device, &allocation, NULL, memory));
    CHECK(vkBindBufferMemory(vk.device, *buffer, *memory, 0));
    CHECK(vkMapMemory(vk.device, *memory, 0, size, 0, mapped));
    return 0;
}

static int make_host_buffer(VkDeviceSize size, VkBufferUsageFlags usage, VkBuffer *buffer, VkDeviceMemory *memory,
                            void **mapped)
{
    return make_host_buffer_cached(size, usage, 0, buffer, memory, mapped);
}

/* side220: a buffer in the GPU's own memory, filled by copies (so a transfer destination too). */
static int make_device_buffer(VkDeviceSize size, VkBufferUsageFlags usage, VkBuffer *buffer, VkDeviceMemory *memory)
{
    VkBufferCreateInfo info = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    VkMemoryAllocateInfo allocation = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    VkMemoryRequirements needs;

    info.size = size;
    info.usage = usage | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    CHECK(vkCreateBuffer(vk.device, &info, NULL, buffer));
    vkGetBufferMemoryRequirements(vk.device, *buffer, &needs);
    allocation.allocationSize = needs.size;
    if (find_memory(needs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &allocation.memoryTypeIndex)) {
        vkDestroyBuffer(vk.device, *buffer, NULL);
        *buffer = VK_NULL_HANDLE;
        return -1;
    }
    if (vkAllocateMemory(vk.device, &allocation, NULL, memory) != VK_SUCCESS) {
        vkDestroyBuffer(vk.device, *buffer, NULL);
        *buffer = VK_NULL_HANDLE;
        *memory = VK_NULL_HANDLE;
        return -1;
    }
    CHECK(vkBindBufferMemory(vk.device, *buffer, *memory, 0));
    return 0;
}

/* A 2D image of `levels` levels (rn34: or, cube 1, a cube: six square layers, viewed as a cube), in the GPU's own memory
 * when it has some, and a view of it. */
static int make_image_or_cube(uint32_t width, uint32_t height, uint32_t levels, int cube, VkFormat format,
                              VkImageUsageFlags usage, VkImage *image, VkDeviceMemory *memory, VkImageView *view)
{
    VkImageCreateInfo info = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    VkImageViewCreateInfo view_info = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    VkMemoryAllocateInfo allocation = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    VkMemoryRequirements needs;

    info.flags = cube ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0;
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = format;
    info.extent.width = width;
    info.extent.height = height;
    info.extent.depth = 1;
    info.mipLevels = levels;
    info.arrayLayers = cube ? 6u : 1u;
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
    view_info.viewType = cube ? VK_IMAGE_VIEW_TYPE_CUBE : VK_IMAGE_VIEW_TYPE_2D;
    view_info.format = format;
    view_info.subresourceRange.aspectMask = format == DEPTH_FORMAT ? VK_IMAGE_ASPECT_DEPTH_BIT
                                                                   : VK_IMAGE_ASPECT_COLOR_BIT;
    view_info.subresourceRange.levelCount = levels;
    view_info.subresourceRange.layerCount = cube ? 6u : 1u;
    CHECK(vkCreateImageView(vk.device, &view_info, NULL, view));
    return 0;
}

/* A 2D image of `levels` levels, as make_image_or_cube makes it */
static int make_image(uint32_t width, uint32_t height, uint32_t levels, VkFormat format, VkImageUsageFlags usage,
                      VkImage *image, VkDeviceMemory *memory, VkImageView *view)
{
    return make_image_or_cube(width, height, levels, 0, format, usage, image, memory, view);
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

/* The command pool, the draws' command buffer and their fence (on ngl_vk's own device or a borrowed one). */
static int make_commands(void)
{
    VkCommandPoolCreateInfo pool_info = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
    VkCommandBufferAllocateInfo command_info = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    VkFenceCreateInfo fence_info = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };

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

/* side219: a clock for timing waits (timespec_get: C11, on Linux and on Windows alike). */
static uint64_t now_ns(void)
{
    struct timespec now;

    timespec_get(&now, TIME_UTC);
    return (uint64_t)now.tv_sec * 1000000000ull + (uint64_t)now.tv_nsec;
}

/* Submits one batch to the queue, held through the owner's lock on a borrowed device. */
static VkResult submit_to_queue(const VkSubmitInfo *submit, VkFence fence)
{
    VkQueue queue = vk.queue;
    VkResult result;

    if (vk.queue_lock)
        queue = (VkQueue)vk.queue_lock(vk.queue_user);
    result = vkQueueSubmit(queue, 1, submit, fence);
    if (vk.queue_unlock)
        vk.queue_unlock(vk.queue_user);
    return result;
}

/* Waits until the GPU has done all of ngl_vk's work: the whole device when it is ngl_vk's, the shared queue (where
 * all of ngl_vk's work goes) when it is borrowed, since waiting for a device needs every one of its queues held. */
static void wait_until_idle(void)
{
    if (!vk.borrowed) {
        vkDeviceWaitIdle(vk.device);
        return;
    }
    vkQueueWaitIdle((VkQueue)vk.queue_lock(vk.queue_user));
    vk.queue_unlock(vk.queue_user);
}

/* On a borrowed device a draw doesn't wait for the GPU: the CPU goes on with the game's next frame while the GPU
 * draws this one. The next call that writes memory the GPU may still be reading (the quads, a frame's corners and
 * lights, the shadow's and the grass's constants, the picture) or records into the draws' command buffer waits for
 * it first, here. */
static void finish_frame(void)
{
    uint64_t started;

    if (!vk.frame_pending)
        return;
    started = now_ns();
    if (vkWaitForFences(vk.device, 1, &vk.done, VK_TRUE, WAIT_NS) != VK_SUCCESS)
        fprintf(stderr, "[ngl_vk] the last frame took over %llu s on the GPU\n", WAIT_NS / 1000000000ull);
    vk.gpu_wait_ns += now_ns() - started;
    vk.gpu_waits++;
    vk.frame_pending = 0;
}

uint64_t ngl_vk_take_gpu_wait(uint32_t *waits)
{
    uint64_t waited = vk.gpu_wait_ns;

    if (waits)
        *waits = vk.gpu_waits;
    vk.gpu_wait_ns = 0;
    vk.gpu_waits = 0;
    return waited;
}

static int make_device(void)
{
    VkDeviceQueueCreateInfo queue_info = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
    VkDeviceCreateInfo info = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
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
    if (ngl_vk_load_device(vk.device)) {   /* [rexglue-native] 2026-11-09: its functions, from its driver */
        if (vkDestroyDevice)
            vkDestroyDevice(vk.device, NULL);
        vk.device = VK_NULL_HANDLE;
        return -1;
    }
    vkGetDeviceQueue(vk.device, vk.queue_family, 0, &vk.queue);
    return make_commands();
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
    /* side221 (acct2-bro-game's find): without MAILBOX (X11 on NVIDIA offers FIFO, IMMEDIATE and FIFO_RELAXED), the
     * picture is shown at once (it may tear) rather than held to the display's refresh, which capped every window */
    for (i = 0; window.present_mode == 1 && info.presentMode == VK_PRESENT_MODE_FIFO_KHR && i < mode_count; i++)
        if (modes[i] == VK_PRESENT_MODE_IMMEDIATE_KHR)
            info.presentMode = VK_PRESENT_MODE_IMMEDIATE_KHR;
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
            info.presentMode == VK_PRESENT_MODE_MAILBOX_KHR     ? "mailbox"
            : info.presentMode == VK_PRESENT_MODE_IMMEDIATE_KHR ? "immediate (may tear)"
                                                                : "in step with the display");
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
/* [rexglue-native] 2026-11-09: the subpass dependencies of the main pass, its two halves and the overlay pass. Render
 * passes are only compatible (and these share the main pass's pipelines and framebuffer) when they are identical apart
 * from load and store operations and layouts, their dependencies included: rn2's run under the validation layer had
 * 3.2 million "RenderPasses incompatible ... srcSubpass" errors from the halves' own dependencies, rn3's 325,000 from
 * the overlay pass's. So all four use this one set, the union of what each needs:
 *  in: the last pass's (or the first half's) depth and colors, the blits' reads and the blur's writes, and the last
 *      frame's readers of the target (its copies out, ngl_vk_output's pass), before this pass clears, loads or reads
 *      them (the overlay pass samples in its fragment shader);
 *  out: the depth and colors to the copies after the pass (the target's copy out, the first half's depth copy) and the
 *      first half's colors to the second. */
static void target_pass_dependencies(VkSubpassDependency dependencies[2])
{
    memset(dependencies, 0, 2 * sizeof *dependencies);
    dependencies[0].srcSubpass = VK_SUBPASS_EXTERNAL;
    dependencies[0].dstSubpass = 0;
    dependencies[0].srcStageMask = TARGET_LAST_USES | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependencies[0].dstStageMask = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                                   VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                                   VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;      /* the overlay pass's reads */
    dependencies[0].srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    dependencies[0].dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                                    VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
                                    VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
    dependencies[1].srcSubpass = 0;
    dependencies[1].dstSubpass = VK_SUBPASS_EXTERNAL;
    dependencies[1].srcStageMask = VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT |
                                   VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependencies[1].dstStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependencies[1].srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    dependencies[1].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
                                    VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
}

/* The main pass in two halves, for soft effects (draw): the first clears both attachments and leaves the depth in the
 * transfer layout for the copy; the second loads both and ends as the main pass does. Both are compatible with the
 * main pass (the same attachments and dependencies), so its pipelines and framebuffer serve them. */
static int make_half_pass(int second, VkRenderPass *pass)
{
    VkAttachmentDescription attachments[2] = { { 0 }, { 0 } };
    VkAttachmentReference color = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
    VkAttachmentReference depth = { 1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL };
    VkSubpassDescription subpass = { 0 };
    VkSubpassDependency dependencies[2];
    VkRenderPassCreateInfo pass_info = { VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };

    attachments[0].format = TARGET_FORMAT;
    attachments[0].samples = VK_SAMPLE_COUNT_1_BIT;
    attachments[0].loadOp = second ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachments[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachments[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachments[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[0].initialLayout = second ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED;
    attachments[0].finalLayout = second ? VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL
                                        : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    attachments[1].format = DEPTH_FORMAT;
    attachments[1].samples = VK_SAMPLE_COUNT_1_BIT;
    attachments[1].loadOp = second ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachments[1].storeOp = second ? VK_ATTACHMENT_STORE_OP_DONT_CARE : VK_ATTACHMENT_STORE_OP_STORE;
    attachments[1].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachments[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[1].initialLayout = second ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL
                                          : VK_IMAGE_LAYOUT_UNDEFINED;
    attachments[1].finalLayout = second ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL
                                        : VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &color;
    subpass.pDepthStencilAttachment = &depth;
    target_pass_dependencies(dependencies);     /* [rexglue-native] 2026-11-09: the main pass's own */
    pass_info.attachmentCount = 2;
    pass_info.pAttachments = attachments;
    pass_info.subpassCount = 1;
    pass_info.pSubpasses = &subpass;
    pass_info.dependencyCount = 2;
    pass_info.pDependencies = dependencies;
    CHECK(vkCreateRenderPass(vk.device, &pass_info, NULL, pass));
    return 0;
}

static int make_target(void)
{
    VkAttachmentDescription attachments[2] = { { 0 }, { 0 } };
    VkAttachmentReference color = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
    VkAttachmentReference depth = { 1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL };
    VkImageView views[2];
    VkSubpassDescription subpass = { 0 };
    VkSubpassDependency dependencies[2];
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
    /* [rexglue-native] 2026-11-09: the colors to the copy out after the pass; the last pass's depth tests and writes
     * done before this pass clears the depth buffer, and the last frame's readers of the target (its copies out,
     * ngl_vk_output's pass) before this one writes it: one set with the halves' (target_pass_dependencies) */
    target_pass_dependencies(dependencies);
    pass_info.attachmentCount = 2;
    pass_info.pAttachments = attachments;
    pass_info.subpassCount = 1;
    pass_info.pSubpasses = &subpass;
    pass_info.dependencyCount = 2;
    pass_info.pDependencies = dependencies;
    CHECK(vkCreateRenderPass(vk.device, &pass_info, NULL, &vk.render_pass));

    if (make_image(vk.width, vk.height, 1, TARGET_FORMAT,
                   VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                       VK_IMAGE_USAGE_SAMPLED_BIT,     /* ngl_vk_output samples it */
                   &vk.target, &vk.target_memory, &vk.target_view) ||
        make_image(vk.width, vk.height, 1, DEPTH_FORMAT,
                   VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
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

    /* soft effects: the two halves and the depth's copy, read texel by texel */
    {
        VkSamplerCreateInfo sampler_info = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };

        if (make_half_pass(0, &vk.opaque_pass) || make_half_pass(1, &vk.blend_pass) ||
            make_image(vk.width, vk.height, 1, DEPTH_FORMAT, VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                       &vk.soft_depth, &vk.soft_depth_memory, &vk.soft_depth_view))
            return -1;
        sampler_info.magFilter = VK_FILTER_NEAREST;
        sampler_info.minFilter = VK_FILTER_NEAREST;
        sampler_info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        sampler_info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sampler_info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sampler_info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sampler_info.maxLod = 0.0f;
        CHECK(vkCreateSampler(vk.device, &sampler_info, NULL, &vk.soft_sampler));
    }

    return make_host_buffer_cached((VkDeviceSize)vk.width * vk.height * BYTES_PER_PIXEL,
                                   VK_BUFFER_USAGE_TRANSFER_DST_BIT, 1, &vk.readback, &vk.readback_memory,
                                   &vk.readback_pixels);
}

static int make_pipeline(void)
{
    VkSamplerCreateInfo sampler_info = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    VkDescriptorSetLayoutBinding binding = { 0 };
    VkDescriptorSetLayoutCreateInfo set_layout_info = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    VkDescriptorPoolSize pool_size = { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                       1 + MAX_TEXTURES + DOF_SETS + POST_SETS + OUTPUT_SETS + EMPTY_CUBE_SETS };
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
    pool_info.maxSets = 1 + MAX_TEXTURES + DOF_SETS + POST_SETS + OUTPUT_SETS + EMPTY_CUBE_SETS;
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
    VkDescriptorSetLayoutBinding bindings[4] = { { 0 }, { 0 }, { 0 }, { 0 } };
    VkDescriptorSetLayoutCreateInfo set_layout_info = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    VkDescriptorPoolSize pool_sizes[3] = {
        { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2 },
        { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1 },
        { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1 },
    };
    VkDescriptorPoolCreateInfo pool_info = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
    VkDescriptorSetAllocateInfo set_info = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
    VkDescriptorImageInfo image_info = { VK_NULL_HANDLE, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
    VkDescriptorImageInfo soft_info = { VK_NULL_HANDLE, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
    VkDescriptorBufferInfo buffer_info = { VK_NULL_HANDLE, 0, VK_WHOLE_SIZE };
    VkDescriptorBufferInfo lights_info = { VK_NULL_HANDLE, 0, VK_WHOLE_SIZE };
    VkWriteDescriptorSet writes[4] = {
        { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET },
        { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET },
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
    bindings[2].binding = 2;        /* soft effects: the opaque meshes' depth (make_target) */
    bindings[2].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[2].descriptorCount = 1;
    bindings[2].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    bindings[3].binding = 3;        /* the draws' nearby lights (ngl_vk_set_meshes) */
    bindings[3].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bindings[3].descriptorCount = 1;
    bindings[3].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    set_layout_info.bindingCount = 4;
    set_layout_info.pBindings = bindings;
    CHECK(vkCreateDescriptorSetLayout(vk.device, &set_layout_info, NULL, &vk.shadow_set_layout));
    pool_info.maxSets = 1;
    pool_info.poolSizeCount = 3;
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
    if (make_host_buffer((VkDeviceSize)MAX_DRAW_LIGHT_SLOTS * DRAW_LIGHT_FLOATS * sizeof(float),
                         VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, &vk.draw_lights, &vk.draw_light_memory,
                         (void **)&vk.draw_light_data))
        return -1;
    memset(vk.draw_light_data, 0, (size_t)MAX_DRAW_LIGHT_SLOTS * DRAW_LIGHT_FLOATS * sizeof(float));
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
    soft_info.sampler = vk.soft_sampler;
    soft_info.imageView = vk.soft_depth_view;
    writes[2].dstSet = vk.shadow_set;
    writes[2].dstBinding = 2;
    writes[2].descriptorCount = 1;
    writes[2].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[2].pImageInfo = &soft_info;
    lights_info.buffer = vk.draw_lights;
    writes[3].dstSet = vk.shadow_set;
    writes[3].dstBinding = 3;
    writes[3].descriptorCount = 1;
    writes[3].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[3].pBufferInfo = &lights_info;
    vkUpdateDescriptorSets(vk.device, 4, writes, 0, NULL);
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
/* side220: the corners and indices in the GPU's own memory, and the staging buffer that fills them; 0 when made (else
 * nothing is left made, and they go in memory the host sees, as before). */
static int make_geometry_on_gpu(void)
{
    if (make_device_buffer((VkDeviceSize)MESH_CORNERS * CORNER_FLOATS * sizeof(float),
                           VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, &vk.mesh_corners, &vk.mesh_corner_memory) == 0 &&
        make_device_buffer((VkDeviceSize)MESH_INDICES * sizeof(uint16_t), VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                           &vk.mesh_indices, &vk.mesh_index_memory) == 0 &&
        make_host_buffer(2 * STAGING_HALF, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, &vk.staging, &vk.staging_memory,
                         (void **)&vk.staging_data) == 0) {
        vk.geometry_on_gpu = 1;
        return 0;
    }
    fprintf(stderr, "[ngl_vk] the meshes' corners don't fit in the GPU's own memory: kept where the host sees them\n");
    if (vk.staging_memory)
        vkUnmapMemory(vk.device, vk.staging_memory);
    vkDestroyBuffer(vk.device, vk.staging, NULL);
    vkFreeMemory(vk.device, vk.staging_memory, NULL);
    vkDestroyBuffer(vk.device, vk.mesh_corners, NULL);
    vkDestroyBuffer(vk.device, vk.mesh_indices, NULL);
    vkFreeMemory(vk.device, vk.mesh_corner_memory, NULL);
    vkFreeMemory(vk.device, vk.mesh_index_memory, NULL);
    vk.staging = vk.mesh_corners = vk.mesh_indices = VK_NULL_HANDLE;
    vk.staging_memory = vk.mesh_corner_memory = vk.mesh_index_memory = VK_NULL_HANDLE;
    vk.staging_data = NULL;
    return -1;
}

/* side220: records the copies waiting in this frame's half of the staging buffer, then a barrier so the draws after
 * them read what was copied; the next frame's copies go in the other half (free: its frame is done, finish_frame). */
static void record_staged_copies(VkCommandBuffer commands)
{
    VkMemoryBarrier copied = { VK_STRUCTURE_TYPE_MEMORY_BARRIER };

    if (!vk.copy_count[0] && !vk.copy_count[1])
        return;
    if (vk.copy_count[0])
        vkCmdCopyBuffer(commands, vk.staging, vk.mesh_corners, vk.copy_count[0], vk.copies[0]);
    if (vk.copy_count[1])
        vkCmdCopyBuffer(commands, vk.staging, vk.mesh_indices, vk.copy_count[1], vk.copies[1]);
    copied.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    copied.dstAccessMask = VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_INDEX_READ_BIT;
    vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_VERTEX_INPUT_BIT, 0, 1, &copied, 0,
                         NULL, 0, NULL);
    vk.copy_count[0] = vk.copy_count[1] = 0;
    vk.staged = 0;
    vk.staging_half ^= 1u;
}

static int submit_to_queue(const VkSubmitInfo *submit, VkFence fence);
static void finish_frame(void);

/* side220: the copies waiting, made now and waited for (this frame's half is full, or a copy list is). */
static int flush_staged(void)
{
    VkCommandBufferBeginInfo begin = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    VkSubmitInfo submit = { VK_STRUCTURE_TYPE_SUBMIT_INFO };

    finish_frame();     /* the draws' command buffer and fence are free */
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vkResetFences(vk.device, 1, &vk.done) != VK_SUCCESS || vkResetCommandBuffer(vk.commands, 0) != VK_SUCCESS ||
        vkBeginCommandBuffer(vk.commands, &begin) != VK_SUCCESS)
        return -1;
    record_staged_copies(vk.commands);
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &vk.commands;
    if (vkEndCommandBuffer(vk.commands) != VK_SUCCESS || submit_to_queue(&submit, vk.done) != VK_SUCCESS ||
        vkWaitForFences(vk.device, 1, &vk.done, VK_TRUE, WAIT_NS) != VK_SUCCESS)
        return -1;
    vk.staging_flushes++;
    return 0;
}

/* side220: size bytes for the GPU's corners (into 0) or indices (1) at offset, through this frame's staging half. */
static int stage(uint32_t into, VkDeviceSize offset, const void *data, VkDeviceSize size)
{
    const uint8_t *from = data;

    while (size) {
        VkDeviceSize room = STAGING_HALF - vk.staged, n, at;
        VkBufferCopy *last;

        if (!room || vk.copy_count[into] == MAX_STAGED_COPIES) {
            if (flush_staged())
                return -1;
            continue;
        }
        n = size < room ? size : room;
        at = (VkDeviceSize)vk.staging_half * STAGING_HALF + vk.staged;
        memcpy(vk.staging_data + at, from, (size_t)n);
        last = vk.copy_count[into] ? &vk.copies[into][vk.copy_count[into] - 1] : NULL;
        if (last && last->srcOffset + last->size == at && last->dstOffset + last->size == offset) {
            last->size += n;    /* runs on from the last copy in both buffers */
        } else {
            vk.copies[into][vk.copy_count[into]].srcOffset = at;
            vk.copies[into][vk.copy_count[into]].dstOffset = offset;
            vk.copies[into][vk.copy_count[into]].size = n;
            vk.copy_count[into]++;
        }
        vk.staged += n;
        vk.staged_bytes += n;
        from += n;
        offset += n;
        size -= n;
    }
    return 0;
}

void ngl_vk_set_geometry_on_gpu(int on)
{
    vk.geometry_in_host_memory = !on;
}

uint64_t ngl_vk_take_staging(uint64_t *flushes)
{
    uint64_t bytes = vk.staged_bytes;

    if (flushes)
        *flushes = vk.staging_flushes;
    vk.staged_bytes = 0;
    vk.staging_flushes = 0;
    return bytes;
}

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
    /* [rexglue-native] 2026-11-09: sets 4 and 5 hold the uniforms' fabric mask and swatch (mesh.frag); rn34: set 6 the
     * gun metal's cube (the same one-sampler layout: the view is a cube) */
    VkDescriptorSetLayout set_layouts[MESH_SETS_FABRIC] = { vk.set_layout, vk.set_layout, vk.set_layout,
                                                            vk.shadow_set_layout, vk.set_layout, vk.set_layout,
                                                            vk.set_layout };
    VkPhysicalDeviceProperties properties;
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

    /* [rexglue-native] 2026-11-09: the fabric's two sets (rn34: and the gun metal's cube) only where the device binds
     * seven (Vulkan promises four; every desktop driver has eight or more); else the shader built without them */
    vkGetPhysicalDeviceProperties(vk.gpu, &properties);
    vk.fabric_sets = properties.limits.maxBoundDescriptorSets >= MESH_SETS_FABRIC;
    if (!vk.fabric_sets)
        fprintf(stderr, "[ngl_vk] the device binds %u descriptor sets: the uniforms' fabric is drawn without its "
                "mask and swatch, and the gun metal without its cube\n", properties.limits.maxBoundDescriptorSets);
    layout_info.setLayoutCount = vk.fabric_sets ? MESH_SETS_FABRIC : 4u;
    layout_info.pSetLayouts = set_layouts;
    layout_info.pushConstantRangeCount = 1;
    layout_info.pPushConstantRanges = &matrices;
    CHECK(vkCreatePipelineLayout(vk.device, &layout_info, NULL, &vk.mesh_layout));

    if (make_shader(mesh_vert_spirv, sizeof mesh_vert_spirv, &vertex_shader) ||
        (vk.fabric_sets ? make_shader(mesh_frag_spirv, sizeof mesh_frag_spirv, &fragment_shader)
                        : make_shader(mesh_four_sets_frag_spirv, sizeof mesh_four_sets_frag_spirv, &fragment_shader)))
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
    if (result == VK_SUCCESS) {         /* [waw-decomp] 2026-11-10: multiplied: behind x color (decals) */
        /* behind x color + behind x (1 - alpha): with the color already times its alpha, behind x lerp(1, color,
         * alpha), as CoD2's decals (mul_pfog) are drawn */
        blend_attachment.srcColorBlendFactor = VK_BLEND_FACTOR_DST_COLOR;
        blend_attachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        blend_attachment.colorBlendOp = VK_BLEND_OP_ADD;
        /* toward the eye, so it isn't lost in the surface it lies on; no slope part: at a grazing view that would
         * lift it through what covers it (CoD2's decals under the snow on the ground, cod2side15) */
        raster.depthBiasEnable = VK_TRUE;
        raster.depthBiasConstantFactor = -1.0f;
        raster.depthBiasSlopeFactor = 0.0f;
        result = vkCreateGraphicsPipelines(vk.device, VK_NULL_HANDLE, 1, &info, NULL, &vk.mesh_multiply_pipeline);
        raster.depthBiasEnable = VK_FALSE;  /* as the subtracting pipeline left them, for what follows */
        raster.depthBiasConstantFactor = 0.0f;
        raster.depthBiasSlopeFactor = 0.0f;
        blend_attachment.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        blend_attachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
        blend_attachment.colorBlendOp = VK_BLEND_OP_REVERSE_SUBTRACT;
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
    if (!vk.geometry_in_host_memory && make_geometry_on_gpu() == 0)
        return 0;
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
    finish_frame();
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

void ngl_vk_set_grass_scissor(int32_t x, int32_t y, uint32_t width, uint32_t height)
{
    vk.grass_scissor_x = x;
    vk.grass_scissor_y = y;
    vk.grass_scissor_width = width;
    vk.grass_scissor_height = height;
}

/* [rexglue-native] 2026-11-09: a scissor (split screen) within the target; the whole target when it has no size */
static VkRect2D scissor_within(int32_t x, int32_t y, uint32_t width, uint32_t height)
{
    VkRect2D area = { { 0, 0 }, { vk.width, vk.height } };
    int64_t x0, y0, x1, y1;

    if (!width || !height)
        return area;
    x0 = x < 0 ? 0 : x;
    y0 = y < 0 ? 0 : y;
    x1 = (int64_t)x + width;
    y1 = (int64_t)y + height;
    if (x1 > (int64_t)vk.width)
        x1 = vk.width;
    if (y1 > (int64_t)vk.height)
        y1 = vk.height;
    if (x1 <= x0 || y1 <= y0)       /* nothing of it on the target: a scissor of no size */
        x1 = x0, y1 = y0;
    area.offset.x = (int32_t)x0;
    area.offset.y = (int32_t)y0;
    area.extent.width = (uint32_t)(x1 - x0);
    area.extent.height = (uint32_t)(y1 - y0);
    return area;
}

/* This frame's grass patches, after the opaque meshes; returns 1 when it bound its own buffers. The callers have the
 * whole target as the scissor, and get it back. */
static int draw_grass(void)
{
    VkDeviceSize no_offset = 0;
    VkDescriptorSet blade, untextured = vk.texture_count ? vk.textures[1].set : vk.set;
    uint32_t count = vk.grass_patch_count;
    /* [rexglue-native] 2026-11-09: split screen: kept to its player's part (ngl_vk_set_grass_scissor), once */
    VkRect2D whole = { { 0, 0 }, { vk.width, vk.height } };
    VkRect2D part = scissor_within(vk.grass_scissor_x, vk.grass_scissor_y, vk.grass_scissor_width,
                                   vk.grass_scissor_height);
    int kept = memcmp(&part, &whole, sizeof whole) != 0;

    vk.grass_patch_count = 0;   /* a frame's patches are drawn once */
    vk.grass_scissor_width = vk.grass_scissor_height = 0;
    if (!vk.grass_pipeline || !vk.grass_index_count || !count)
        return 0;
    if (kept)
        vkCmdSetScissor(vk.commands, 0, 1, &part);
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
    if (kept)
        vkCmdSetScissor(vk.commands, 0, 1, &whole);
    return 1;
}

uint32_t ngl_vk_mesh(const float *corners, uint32_t corner_count, const uint16_t *indices, uint32_t index_count)
{
    uint32_t n;

    if (!vk.device || !corners || !indices || !corner_count || !index_count || vk.mesh_count == MAX_MESHES ||
        corner_count > MESH_CORNERS - 2 * FRAME_CORNERS - vk.corners_used ||
        index_count > MESH_INDICES - vk.indices_used)
        return 0;
    n = ++vk.mesh_count;
    vk.meshes[n].first_corner = vk.corners_used;
    vk.meshes[n].first_index = vk.indices_used;
    vk.meshes[n].index_count = index_count;
    if (vk.geometry_on_gpu) {
        if (stage(0, (VkDeviceSize)vk.corners_used * CORNER_FLOATS * sizeof(float), corners,
                  (VkDeviceSize)corner_count * CORNER_FLOATS * sizeof(float)) ||
            stage(1, (VkDeviceSize)vk.indices_used * sizeof(uint16_t), indices,
                  (VkDeviceSize)index_count * sizeof(uint16_t))) {
            vk.mesh_count--;
            return 0;
        }
    } else {
        memcpy(vk.mesh_corner_data + (size_t)vk.corners_used * CORNER_FLOATS, corners,
               (size_t)corner_count * CORNER_FLOATS * sizeof(float));
        memcpy(vk.mesh_index_data + vk.indices_used, indices, (size_t)index_count * sizeof(uint16_t));
    }
    vk.corners_used += corner_count;
    vk.indices_used += index_count;
    return n;
}

uint32_t ngl_vk_frame_corners(const float *corners, uint32_t corner_count)
{
    /* side214: no wait for the GPU: the last frame, which it may still draw, read the other half */
    uint32_t first = MESH_CORNERS - (2u - vk.frame_corner_half) * FRAME_CORNERS + vk.frame_corners_used;

    if (!vk.mesh_corners || !corners || !corner_count || corner_count > FRAME_CORNERS - vk.frame_corners_used)
        return 0;
    if (vk.geometry_on_gpu) {   /* side220: copied in at the start of this frame's draw */
        if (stage(0, (VkDeviceSize)first * CORNER_FLOATS * sizeof(float), corners,
                  (VkDeviceSize)corner_count * CORNER_FLOATS * sizeof(float)))
            return 0;
    } else {
        memcpy(vk.mesh_corner_data + (size_t)first * CORNER_FLOATS, corners,
               (size_t)corner_count * CORNER_FLOATS * sizeof(float));
    }
    vk.frame_corners_used += corner_count;
    vk.frame_corners_written = 1;
    return first + 1;
}

static int inverted(const float m[16], double out[16]);

/* rn32: whether a draw is drawn as the soldiers' skin (ngl_vk.h noise_swatch): no fabric mask, a noise swatch that is
 * a texture */
static int skin_draw(const struct ngl_vk_mesh_draw *d)
{
    return !d->fabric_mask && d->noise_swatch && d->noise_swatch <= vk.texture_count && !vk.textures[d->noise_swatch].cube;
}

/* [rexglue-native] 2026-11-10 rn40x: whether a skin draw also folds a woven swatch into its normal (ngl_vk.h
 * skin_swatch: ADC02DCE4FDC9253's helmets and bodies), a texture that isn't a cube, with a repeat */
static int skin_woven(const struct ngl_vk_mesh_draw *d)
{
    return skin_draw(d) && d->skin_swatch && d->skin_swatch <= vk.texture_count &&
           !vk.textures[d->skin_swatch].cube && d->skin_swatch_repeat > 0.0f;
}

/* [rexglue-native] 2026-11-09 rn34: whether a draw is drawn as the gun metal (ngl_vk.h metal_cube): no fabric mask or
 * noise swatch, a cube that is one */
static int metal_draw(const struct ngl_vk_mesh_draw *d)
{
    return !d->fabric_mask && !d->noise_swatch && d->metal_cube && d->metal_cube <= vk.texture_count &&
           vk.textures[d->metal_cube].cube;
}

/* [rexglue-native] 2026-11-09 rn36: whether a draw is drawn as the vehicles' detail-mapped metal (ngl_vk.h detail_map):
 * no fabric mask, noise swatch or cube, a detail map that is a 2D texture */
static int detail_draw(const struct ngl_vk_mesh_draw *d)
{
    return !d->fabric_mask && !d->noise_swatch && !d->metal_cube && d->detail_map &&
           d->detail_map <= vk.texture_count && !vk.textures[d->detail_map].cube;
}

/* [rexglue-native] 2026-11-10 rn39: whether a draw is drawn as the bumped vehicle parts (ngl_vk.h vehicle_bumped): no
 * fabric mask, noise swatch, cube or detail map */
static int vehicle_draw(const struct ngl_vk_mesh_draw *d)
{
    return !d->fabric_mask && !d->noise_swatch && !d->metal_cube && !d->detail_map && d->vehicle_bumped;
}

/* [rexglue-native] 2026-11-10 rn40: whether a draw is drawn as glass with an environment cube (ngl_vk.h lens_cube): a
 * color map and a cube that is one, lit by the scene's lights, no normal map, environment map or second texture, on a
 * device that binds the fabric's sets (set 6 holds the cube) */
static int lens_draw(const struct ngl_vk_mesh_draw *d)
{
    return vk.fabric_sets && d->lens_cube && d->lens_cube <= vk.texture_count && vk.textures[d->lens_cube].cube &&
           d->texture && d->texture <= vk.texture_count && !d->blend_texture && !d->env_texture && !d->normal_map &&
           d->lit && d->shading == NGL_VK_SHADE_LIGHT;
}

/* rn34: whether a draw's metal_scratch is a 2D texture it can sample */
static int metal_scratched(const struct ngl_vk_mesh_draw *d)
{
    return d->metal_scratch && d->metal_scratch <= vk.texture_count && !vk.textures[d->metal_scratch].cube;
}

/* [rexglue-native] 2026-11-09: whether a draw is drawn as the uniforms' fabric (ngl_vk.h fabric_mask): a bumped draw
 * with a specular map (draw_meshes' bumped and specular, the same tests) whose mask and swatch are textures, on a
 * device with the fabric's sets. rn32: or as the soldiers' skin (skin_draw), which takes the same sets and slot; rn34:
 * or as the gun metal (metal_draw), the same and set 6; rn36: or as the vehicles' detail-mapped metal (detail_draw);
 * rn39: or as the bumped vehicle parts (vehicle_draw), the slot alone. */
static int fabric_draw(const struct ngl_vk_mesh_draw *d)
{
    int textured = d->texture && d->texture <= vk.texture_count;
    int lightmapped = d->lightmap && d->lightmap <= vk.texture_count;
    int mixed = textured && d->blend_texture && d->blend_texture <= vk.texture_count;
    uint32_t shading = d->shading == NGL_VK_SHADE_LIGHTMAP && !lightmapped ? NGL_VK_SHADE_LIGHT : d->shading & 3u;
    int env = textured && !mixed && shading != NGL_VK_SHADE_LIGHTMAP && d->env_texture &&
              d->env_texture <= vk.texture_count;
    int third = !env && d->blend_light && d->blend_light <= vk.texture_count;
    int bumped = textured && !mixed && !env && shading == NGL_VK_SHADE_LIGHT && d->lit && d->normal_map &&
                 d->normal_map <= vk.texture_count;
    int specular = bumped && !third && d->specular_map && d->specular_map <= vk.texture_count;

    return vk.fabric_sets && specular &&
           ((d->fabric_mask && d->fabric_mask <= vk.texture_count && d->normal_swatch &&
             d->normal_swatch <= vk.texture_count) || skin_draw(d) || metal_draw(d) || detail_draw(d) ||
            vehicle_draw(d));
}

/* A draw's own lights into this frame's slot (DRAW_LIGHT_FLOATS): its nearby lights, each place into its mesh's own
 * space (the world matrix's inverse; the world's meshes are rigid, so the range needs no scaling), then its irradiance
 * rows (already in the mesh's space), then (rexglue-native) the fabric's numbers, or (rn34) the gun metal's; the slot's
 * number (1 up) goes to the draw's push flags, 0: none. */
static uint32_t place_draw_lights(const struct ngl_vk_mesh_draw *d)
{
    double inverse[16];
    float *slot;
    uint32_t count = (d->point_light_count & 0xFFFFu) < 2 ? (d->point_light_count & 0xFFFFu) : 2;
    int rows = d->irradiance_rows == 9;
    int fabric = fabric_draw(d);
    int lens = !fabric && lens_draw(d);     /* rn40 */

    if (!vk.draw_light_data || vk.draw_light_slots >= MAX_DRAW_LIGHT_SLOTS)
        return 0;
    if (count && !inverted(d->world, inverse))
        count = 0;
    if (!count && !rows && !fabric && !lens)
        return 0;
    slot = vk.draw_light_data + (size_t)vk.draw_light_slots * DRAW_LIGHT_FLOATS;
    memset(slot, 0, DRAW_LIGHT_FLOATS * sizeof(float));
    for (uint32_t k = 0; k < count; k++) {
        const float *l = d->point_light[k];

        for (int column = 0; column < 3; column++)      /* row vector x inverse (Direct3D's row by row) */
            slot[8 * k + column] = (float)(l[0] * inverse[column] + l[1] * inverse[4 + column] +
                                           l[2] * inverse[8 + column] + inverse[12 + column]);
        slot[8 * k + 3] = l[3];
        slot[8 * k + 4] = l[4];
        slot[8 * k + 5] = l[5];
        slot[8 * k + 6] = l[6];
        slot[8 * k + 7] = 1.0f;
    }
    for (int row = 0; rows && row < 9; row++) {         /* vec4s 4 to 12 */
        slot[16 + 4 * row] = d->irradiance[row][0];
        slot[16 + 4 * row + 1] = d->irradiance[row][1];
        slot[16 + 4 * row + 2] = d->irradiance[row][2];
    }
    slot[16 + 3] = rows ? 1.0f : 0.0f;
    if (fabric && metal_draw(d)) {  /* rn34: the gun metal (METAL_FLOATS_AT's comment) */
        const float *w = d->world;

        slot[FABRIC_FLOATS_AT] = d->metal_brightness;
        slot[FABRIC_FLOATS_AT + 1] = d->metal_reflection;
        slot[FABRIC_FLOATS_AT + 2] = d->metal_power;
        slot[FABRIC_FLOATS_AT + 3] = 0.0f;                  /* not fabric */
        slot[FABRIC_FLOATS_AT + 4] = d->metal_hue[0];
        slot[FABRIC_FLOATS_AT + 5] = d->metal_hue[1];
        slot[FABRIC_FLOATS_AT + 6] = d->metal_hue[2];
        slot[FABRIC_FLOATS_AT + 7] = d->metal_rim_edge;
        slot[FABRIC_FLOATS_AT + 8] = d->metal_rim_scale;
        slot[FABRIC_FLOATS_AT + 10] = 1.0f;                 /* metal */
        slot[FABRIC_FLOATS_AT + 11] = metal_scratched(d) ? 1.0f : 0.0f;
        slot[METAL_FLOATS_AT] = d->metal_rim[0];
        slot[METAL_FLOATS_AT + 1] = d->metal_rim[1];
        slot[METAL_FLOATS_AT + 2] = d->metal_rim[2];
        slot[METAL_FLOATS_AT + 3] = d->metal_cube_bias;
        /* Direct3D's row vectors: a direction's world part is the sum of its parts x that column's, row by row */
        for (int part = 0; part < 3; part++) {
            slot[METAL_FLOATS_AT + 4 + 4 * part] = w[part];
            slot[METAL_FLOATS_AT + 5 + 4 * part] = w[4 + part];
            slot[METAL_FLOATS_AT + 6 + 4 * part] = w[8 + part];
        }
    } else if (fabric && detail_draw(d)) {  /* rn36: the vehicles' detail-mapped metal (METAL_FLOATS_AT's comment) */
        slot[FABRIC_FLOATS_AT] = d->detail_repeat;
        slot[FABRIC_FLOATS_AT + 1] = 1.0f;                  /* the base as it is */
        slot[FABRIC_FLOATS_AT + 2] = d->detail_specular_power;
        slot[FABRIC_FLOATS_AT + 3] = 1.0f;
        slot[FABRIC_FLOATS_AT + 4] = 1.0f;                  /* no sheen */
        slot[FABRIC_FLOATS_AT + 5] = 1.0f;
        slot[FABRIC_FLOATS_AT + 6] = 1.0f;
        slot[FABRIC_FLOATS_AT + 7] = d->fabric_shadow_floor;
        slot[FABRIC_FLOATS_AT + 8] = d->fabric_shadow_fade;
        slot[FABRIC_FLOATS_AT + 11] = 1.0f;                 /* detail */
        slot[METAL_FLOATS_AT] = d->detail_normal_scale;
        slot[METAL_FLOATS_AT + 1] = d->detail_scale;
    } else if (fabric && vehicle_draw(d)) { /* rn39: the bumped vehicle parts, as a fabric draw with no sheen (the base
                                             * as it is); at METAL_FLOATS_AT the normal map's scale, its w 1 */
        slot[FABRIC_FLOATS_AT] = 1.0f;
        slot[FABRIC_FLOATS_AT + 1] = 1.0f;                  /* the base as it is */
        slot[FABRIC_FLOATS_AT + 2] = d->vehicle_specular_power;
        slot[FABRIC_FLOATS_AT + 3] = 1.0f;
        slot[FABRIC_FLOATS_AT + 4] = 1.0f;                  /* no sheen */
        slot[FABRIC_FLOATS_AT + 5] = 1.0f;
        slot[FABRIC_FLOATS_AT + 6] = 1.0f;
        slot[FABRIC_FLOATS_AT + 7] = d->fabric_shadow_floor;
        slot[FABRIC_FLOATS_AT + 8] = d->fabric_shadow_fade;
        slot[METAL_FLOATS_AT] = d->vehicle_normal_scale;
        slot[METAL_FLOATS_AT + 3] = 1.0f;                   /* a vehicle part */
    } else if (fabric) {       /* [rexglue-native] vec4s 13 and 14 (mesh.frag's FABRIC_AT) */
        slot[FABRIC_FLOATS_AT] = d->fabric_swatch_repeat;
        slot[FABRIC_FLOATS_AT + 1] = d->fabric_diffuse_scale;
        slot[FABRIC_FLOATS_AT + 2] = d->fabric_specular_power;
        slot[FABRIC_FLOATS_AT + 3] = 1.0f;
        slot[FABRIC_FLOATS_AT + 4] = d->fabric_sheen[0];
        slot[FABRIC_FLOATS_AT + 5] = d->fabric_sheen[1];
        slot[FABRIC_FLOATS_AT + 6] = d->fabric_sheen[2];
        slot[FABRIC_FLOATS_AT + 7] = d->fabric_shadow_floor;   /* rn30: the third-person uniforms (21168F53BCF9D156) */
        slot[FABRIC_FLOATS_AT + 8] = d->fabric_shadow_fade;
        if (skin_draw(d)) {     /* rn32: the soldiers' skin (3949D29E09A78AA1), in the sheen's place */
            slot[FABRIC_FLOATS_AT + 4] = d->skin_edge_lift;
            slot[FABRIC_FLOATS_AT + 5] = d->skin_specular_scale;
            slot[FABRIC_FLOATS_AT + 6] = d->skin_noise_mix;
            slot[FABRIC_FLOATS_AT + 9] = 1.0f;
            /* rn40x: the woven swatch's repeat (vec4 15's z, which only the metal and the glass read otherwise, and
             * they aren't fabric draws), 0: none */
            slot[FABRIC_FLOATS_AT + 10] = skin_woven(d) ? d->skin_swatch_repeat : 0.0f;
            /* rn40y: the terminator's bias and scale at METAL_FLOATS_AT (the metal's, the detail's and the vehicles'
             * place; a skin draw is none of those), 0: none */
            slot[METAL_FLOATS_AT] = d->skin_terminator_bias;
            slot[METAL_FLOATS_AT + 1] = d->skin_terminator_scale;
        }
    } else if (lens) {  /* rn40: the glass (ngl_vk.h lens_cube): at FABRIC_FLOATS_AT (rn40e: 1 for the opaque eyes,
                         * the reflection's scale, the highlights' power, 0: not fabric), (the sun's color, rn40e: the
                         * shadow's floor), (rn40e: the shadow's fade, 0, 1: a cube, 0); at METAL_FLOATS_AT the sun's
                         * place in the mesh's own space and the cube's bias, then the world matrix's three columns as
                         * the gun metal's, rn40e: their w's lens_tint */
        double to_mesh[16];
        const float *w = d->world;
        const float *place = d->lens_sun_place;
        int placed = inverted(d->world, to_mesh);

        slot[FABRIC_FLOATS_AT] = d->lens_opaque ? 1.0f : 0.0f;
        slot[FABRIC_FLOATS_AT + 1] = d->lens_reflection;
        slot[FABRIC_FLOATS_AT + 2] = d->lens_power;
        slot[FABRIC_FLOATS_AT + 3] = d->lens_opaque ? (float)d->lens_show : 0.0f;    /* rn40m: a check */
        for (int part = 0; part < 3; part++)    /* no sun where its place can't be turned into the mesh's space */
            slot[FABRIC_FLOATS_AT + 4 + part] = placed ? d->lens_sun_color[part] : 0.0f;
        slot[FABRIC_FLOATS_AT + 7] = d->lens_shadow_floor;
        slot[FABRIC_FLOATS_AT + 8] = d->lens_shadow_fade;
        slot[FABRIC_FLOATS_AT + 10] = 1.0f;                 /* a cube */
        for (int column = 0; placed && column < 3; column++)    /* row vector x inverse, as the point lights' */
            slot[METAL_FLOATS_AT + column] = (float)(place[0] * to_mesh[column] + place[1] * to_mesh[4 + column] +
                                                     place[2] * to_mesh[8 + column] + to_mesh[12 + column]);
        slot[METAL_FLOATS_AT + 3] = d->lens_cube_bias;
        for (int part = 0; part < 3; part++) {
            slot[METAL_FLOATS_AT + 4 + 4 * part] = w[part];
            slot[METAL_FLOATS_AT + 5 + 4 * part] = w[4 + part];
            slot[METAL_FLOATS_AT + 6 + 4 * part] = w[8 + part];
            slot[METAL_FLOATS_AT + 7 + 4 * part] = d->lens_tint[part];
        }
    }
    return ++vk.draw_light_slots;
}

void ngl_vk_set_meshes(const struct ngl_vk_mesh_draw *draws, uint32_t count)
{
    if (!vk.draws)
        return;
    finish_frame();     /* the lights' slots */
    if (count > MAX_MESH_DRAWS)
        count = MAX_MESH_DRAWS;
    if (count)
        memcpy(vk.draws, draws, (size_t)count * sizeof *draws);
    vk.draw_count = count;
    vk.draw_light_slots = 0;
    for (uint32_t i = 0; i < count; i++) {
        /* the slot rides in point_light_count's high bits until push_mesh_draw takes it */
        uint32_t own = (vk.draws[i].point_light_count & 0xFFFFu) || vk.draws[i].irradiance_rows == 9 ||
                       fabric_draw(&vk.draws[i]);   /* [rexglue-native] the fabric's numbers ride in the slot too */
        uint32_t slot = own ? place_draw_lights(&vk.draws[i]) : 0;

        vk.draws[i].point_light_count = slot << 16 | (vk.draws[i].point_light_count & 0xFFFFu);
    }
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

    finish_frame();
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
#define PUSH_SOFT               64u     /* faded where it nears the opaque meshes behind it (soft_depth) */
#define PUSH_CORNER_ALPHA       128u    /* its alpha is its corners' alone (corner_alpha) */
#define PUSH_MODE_SHIFT         8u      /* the draw's MESH_MODE_* bits, 8 of them */
#define PUSH_DRAW_LIGHTS_SHIFT  16u     /* its own lights' slot in set 3 binding 3 (1 up), 0: none */

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

    flags |= (d->point_light_count >> 16) << PUSH_DRAW_LIGHTS_SHIFT;    /* ngl_vk_set_meshes' slot */
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
    if (mode & MESH_MODE_LIGHTMAP_SUN) {    /* a lightmapped draw has no use for the ambient: its scale and up go there */
        const float scale[3] = { d->lightmap_scale, d->lightmap_up, 0.0f };

        memcpy(push + 20, scale, sizeof scale);
    }

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
    if (mode & MESH_MODE_LIGHTMAP_SUN) {    /* the world's up (z) in the second light's place, turned as the lights are */
        float up[3] = { 0.0f, 0.0f, 1.0f };

        if (invertible)
            for (int column = 0; column < 3; column++)
                up[column] = inverse[6 + column];
        push[25] = packed_toward(up);
        push[29] = 0;
    }
    if (d->lit)
        flags |= PUSH_SCENE_LIGHT | more << PUSH_MORE_LIGHTS_SHIFT | (d->light_saturate ? PUSH_HELD : 0u);
    if (d->sun_shadowed && vk.shadow_on)
        flags |= PUSH_SHADOWED;
    if (d->soft_depth && vk.soft_live)
        flags |= PUSH_SOFT;
    if (d->corner_alpha)
        flags |= PUSH_CORNER_ALPHA;
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
/* The frame's meshes: part 0 the opaque ones and then the grass, part 1 the blended ones, part 2 all of them (one
 * pass). The frame's draws are let go after part 1 or 2. */
/* [rexglue-native] 2026-11-09: the scissor a mesh draw asks for (split screen), within the target; the whole target
 * when it asks for none */
static VkRect2D draw_scissor(const struct ngl_vk_mesh_draw *d)
{
    return scissor_within(d->scissor_x, d->scissor_y, d->scissor_width, d->scissor_height);
}

static void draw_meshes(int part)
{
    VkDeviceSize no_offset = 0;
    VkDescriptorSet bound[3] = { VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE };
    /* [rexglue-native] 2026-11-09: sets 4 and 5, the fabric's mask and swatch (any texture for the other draws); rn34:
     * set 6, the gun metal's cube (the empty cube for the other draws) */
    VkDescriptorSet fabric_bound[3] = { VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE };
    VkDescriptorSet untextured = vk.texture_count ? vk.textures[1].set : vk.set;
    /* [rexglue-native] 2026-11-09: the scissor last set here (split screen); the pass starts with the whole target */
    VkRect2D whole = { { 0, 0 }, { vk.width, vk.height } }, scissor = whole;

    vkCmdBindVertexBuffers(vk.commands, 0, 1, &vk.mesh_corners, &no_offset);
    vkCmdBindIndexBuffer(vk.commands, vk.mesh_indices, 0, VK_INDEX_TYPE_UINT16);
    vkCmdBindDescriptorSets(vk.commands, VK_PIPELINE_BIND_POINT_GRAPHICS, vk.mesh_layout, 3, 1, &vk.shadow_set, 0,
                            NULL);
    for (int blended = part == 1; blended < (part == 0 ? 1 : 2); blended++) {
        VkPipeline pipeline = blended ? vk.mesh_blend_pipeline : vk.mesh_pipeline;

        if (blended && part == 2 && memcmp(&scissor, &whole, sizeof whole)) {
            vkCmdSetScissor(vk.commands, 0, 1, &whole);     /* [rexglue-native] the grass on the whole target */
            scissor = whole;
        }
        if (blended && part == 2 && draw_grass()) {   /* the grass after the opaque meshes, own buffers and sets */
            vkCmdBindVertexBuffers(vk.commands, 0, 1, &vk.mesh_corners, &no_offset);
            vkCmdBindIndexBuffer(vk.commands, vk.mesh_indices, 0, VK_INDEX_TYPE_UINT16);
            vkCmdBindDescriptorSets(vk.commands, VK_PIPELINE_BIND_POINT_GRAPHICS, vk.mesh_layout, 3, 1,
                                    &vk.shadow_set, 0, NULL);
            bound[0] = bound[1] = bound[2] = VK_NULL_HANDLE;
            fabric_bound[0] = fabric_bound[1] = fabric_bound[2] = VK_NULL_HANDLE;
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
            /* a per-pixel lit model: its normal map in set 1, its specular map in set 2 */
            int bumped = textured && !mixed && !env && shading == NGL_VK_SHADE_LIGHT && d->lit && d->normal_map &&
                         d->normal_map <= vk.texture_count;
            int specular = bumped && !third && d->specular_map && d->specular_map <= vk.texture_count;
            uint32_t mode = (mixed ? MESH_MODE_MIX : 0u) | (third ? MESH_MODE_THIRD : 0u) |
                            (env ? MESH_MODE_SHINY : 0u) | (env_masked ? MESH_MODE_MASKED : 0u) |
                            (mixed && d->blend_by_alpha ? MESH_MODE_BY_ALPHA : 0u) |
                            (bumped ? MESH_MODE_BUMPED : 0u) | (specular ? MESH_MODE_SPECULAR : 0u) |
                            (shading == NGL_VK_SHADE_LIGHTMAP && !mixed && d->lit && d->lightmap_scale > 0.0f
                                 ? MESH_MODE_LIGHTMAP_SUN : 0u);
            VkDescriptorSet sets[3] = {
                textured ? vk.textures[d->texture].set : untextured,
                mixed ? vk.textures[d->blend_texture].set
                      : env ? vk.textures[d->env_texture].set
                            : bumped ? vk.textures[d->normal_map].set
                                     : lightmapped ? vk.textures[d->lightmap].set : untextured,
                third ? vk.textures[d->blend_light].set
                      : env_masked ? vk.textures[d->env_mask].set
                                   : specular ? vk.textures[d->specular_map].set : untextured,
            };
            int fabric = fabric_draw(d);    /* [rexglue-native] the uniforms: the mask in set 4, the swatch in set 5 */
            int skin = fabric && skin_draw(d);  /* rn32: the soldiers' skin: nothing in set 4 (rn40x: or the woven
                                                 * swatch, skin_woven), the noise swatch in 5 */
            /* rn34: the gun metal: its scratch map (when it has one) in set 4, nothing in 5, its cube in 6 */
            int metal = fabric && !skin && metal_draw(d);
            /* rn36: the vehicles' detail-mapped metal: nothing in set 4, the detail map in set 5 */
            int detail = fabric && !skin && !metal && detail_draw(d);
            /* rn39: the bumped vehicle parts: nothing in sets 4 and 5 */
            int vehicle = fabric && !skin && !metal && !detail && vehicle_draw(d);
            /* rn40: the glass: nothing in sets 4 and 5, its cube in 6 */
            int lens = !fabric && lens_draw(d);
            VkDescriptorSet fabric_textures[3] = {
                metal ? (metal_scratched(d) ? vk.textures[d->metal_scratch].set : untextured)
                      : fabric && !skin && !detail && !vehicle ? vk.textures[d->fabric_mask].set
                      : skin && skin_woven(d)                  ? vk.textures[d->skin_swatch].set  /* rn40x */
                                                               : untextured,
                metal || vehicle ? untextured
                      : skin ? vk.textures[d->noise_swatch].set
                             : detail ? vk.textures[d->detail_map].set
                                      : fabric ? vk.textures[d->normal_swatch].set : untextured,
                metal ? vk.textures[d->metal_cube].set : lens ? vk.textures[d->lens_cube].set : vk.empty_cube_set,
            };

            if (!d->mesh || d->mesh > vk.mesh_count || (d->alpha == NGL_VK_ALPHA_BLEND) != blended)
                continue;
            if (blended) {      /* in the order given: an effect's blending can change from one draw to the next */
                VkPipeline wanted = d->blend == NGL_VK_BLEND_ADD ? vk.mesh_add_pipeline
                                  : d->blend == NGL_VK_BLEND_SUBTRACT ? vk.mesh_subtract_pipeline
                                  : d->blend == NGL_VK_BLEND_MULTIPLY ? vk.mesh_multiply_pipeline  /* [waw-decomp] */
                                                                      : vk.mesh_blend_pipeline;

                if (wanted != pipeline) {
                    pipeline = wanted;
                    vkCmdBindPipeline(vk.commands, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
                    bound[0] = bound[1] = bound[2] = VK_NULL_HANDLE;
                    fabric_bound[0] = fabric_bound[1] = fabric_bound[2] = VK_NULL_HANDLE;
                }
            }
            if (sets[0] != bound[0] || sets[1] != bound[1] || sets[2] != bound[2]) {
                vkCmdBindDescriptorSets(vk.commands, VK_PIPELINE_BIND_POINT_GRAPHICS, vk.mesh_layout, 0, 3, sets, 0,
                                        NULL);
                memcpy(bound, sets, sizeof bound);
            }
            /* [rexglue-native] 2026-11-09: sets 4 and 5 are always bound where the layout has them (mesh.frag declares
             * them); they change only around the uniforms' draws */
            if (vk.fabric_sets && (fabric_textures[0] != fabric_bound[0] || fabric_textures[1] != fabric_bound[1] ||
                                   fabric_textures[2] != fabric_bound[2])) {
                vkCmdBindDescriptorSets(vk.commands, VK_PIPELINE_BIND_POINT_GRAPHICS, vk.mesh_layout, 4, 3,
                                        fabric_textures, 0, NULL);
                memcpy(fabric_bound, fabric_textures, sizeof fabric_bound);
            }
            {   /* [rexglue-native] 2026-11-09: split screen: this draw's part of the screen */
                VkRect2D wanted = draw_scissor(d);

                if (memcmp(&wanted, &scissor, sizeof wanted)) {
                    vkCmdSetScissor(vk.commands, 0, 1, &wanted);
                    scissor = wanted;
                }
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
    if (memcmp(&scissor, &whole, sizeof whole))
        vkCmdSetScissor(vk.commands, 0, 1, &whole);    /* [rexglue-native] what comes after: the whole target */
    if (part == 0) {
        draw_grass();       /* the pass ends after it: nothing to bind again */
        return;
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

    wait_until_idle();
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
static void layers_barrier(VkImage image, uint32_t levels, uint32_t layers, VkImageLayout from, VkImageLayout to,
                           VkAccessFlags from_access, VkAccessFlags to_access, VkPipelineStageFlags from_stage,
                           VkPipelineStageFlags to_stage);

uint32_t ngl_vk_texture(const void *pixels, uint32_t width, uint32_t height)
{
    return ngl_vk_texture_levels(&pixels, 1, width, height);
}

/* [rexglue-native] 2026-11-09: records the new textures waiting in this half (batched textures): each one's levels from
 * the staging buffer into its image, then into the layout the draws sample it in. Called at the start of a draw's
 * commands, before any draw. The next textures go in the other half (free: its frame is done, finish_frame). */
static void record_texture_uploads(void)
{
    uint32_t i;

    if (!vk.pending_count)
        return;
    for (i = 0; i < vk.pending_count; i++) {
        VkImage image = vk.textures[vk.pending[i].texture].image;
        uint32_t level_count = vk.pending[i].level_count;

        levels_barrier(image, level_count, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
                       VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        vkCmdCopyBufferToImage(vk.commands, vk.texture_staging, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                               level_count, vk.pending[i].copies);
        levels_barrier(image, level_count, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                       VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
                       VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                       VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    }
    vk.pending_count = 0;
    vk.texture_staged = 0;
    vk.texture_staging_half ^= 1u;
}

/* [rexglue-native] 2026-11-09: the new textures waiting, copied in now and waited for (this frame's half is full, or
 * its list is), as flush_staged does for the meshes. */
static int flush_textures(void)
{
    VkCommandBufferBeginInfo begin = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    VkSubmitInfo submit = { VK_STRUCTURE_TYPE_SUBMIT_INFO };

    finish_frame();     /* the draws' command buffer and fence are free */
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vkResetFences(vk.device, 1, &vk.done) != VK_SUCCESS || vkResetCommandBuffer(vk.commands, 0) != VK_SUCCESS ||
        vkBeginCommandBuffer(vk.commands, &begin) != VK_SUCCESS)
        return -1;
    record_texture_uploads();
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &vk.commands;
    if (vkEndCommandBuffer(vk.commands) != VK_SUCCESS || submit_to_queue(&submit, vk.done) != VK_SUCCESS ||
        vkWaitForFences(vk.device, 1, &vk.done, VK_TRUE, WAIT_NS) != VK_SUCCESS)
        return -1;
    vk.texture_flushes++;
    return 0;
}

/* [rexglue-native] 2026-11-09: texture n's levels (bytes in all, laid out as copy[] says) into this frame's half of
 * the textures' staging buffer, to be copied in at the start of the next draw (record_texture_uploads). */
static int stage_texture(uint32_t n, const void *const *levels, uint32_t level_count, const VkBufferImageCopy *copy,
                         VkDeviceSize bytes)
{
    VkDeviceSize at;
    uint32_t level;

    if ((vk.texture_staged + bytes > TEXTURE_STAGING_HALF || vk.pending_count == MAX_PENDING_TEXTURES) &&
        flush_textures())
        return -1;
    at = (VkDeviceSize)vk.texture_staging_half * TEXTURE_STAGING_HALF + vk.texture_staged;
    for (level = 0; level < level_count; level++) {
        memcpy(vk.texture_staging_data + at + copy[level].bufferOffset, levels[level],
               (size_t)copy[level].imageExtent.width * copy[level].imageExtent.height * BYTES_PER_PIXEL);
        vk.pending[vk.pending_count].copies[level] = copy[level];
        vk.pending[vk.pending_count].copies[level].bufferOffset += at;
    }
    vk.pending[vk.pending_count].texture = n;
    vk.pending[vk.pending_count].level_count = level_count;
    vk.pending_count++;
    vk.texture_staged += (bytes + 15u) & ~(VkDeviceSize)15u;   /* the next one's start 16-byte aligned */
    vk.batched_textures++;
    return 0;
}

int ngl_vk_set_batched_textures(int on)
{
    if (!on) {
        vk.batch_textures = 0;  /* any still waiting are recorded at the next draw all the same */
        return 0;
    }
    if (!vk.device)
        return -1;
    if (!vk.texture_staging_data &&
        make_host_buffer(2 * TEXTURE_STAGING_HALF, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, &vk.texture_staging,
                         &vk.texture_staging_memory, (void **)&vk.texture_staging_data)) {
        fprintf(stderr, "[ngl_vk] no room for the textures' staging buffer: each texture is copied in on its own\n");
        if (vk.texture_staging_memory)
            vkFreeMemory(vk.device, vk.texture_staging_memory, NULL);
        vkDestroyBuffer(vk.device, vk.texture_staging, NULL);
        vk.texture_staging = VK_NULL_HANDLE;
        vk.texture_staging_memory = VK_NULL_HANDLE;
        vk.texture_staging_data = NULL;
        return -1;
    }
    vk.batch_textures = 1;
    return 0;
}

uint64_t ngl_vk_take_texture_uploads(uint64_t *flushes)
{
    uint64_t batched = vk.batched_textures;

    if (flushes)
        *flushes = vk.texture_flushes;
    vk.batched_textures = 0;
    vk.texture_flushes = 0;
    return batched;
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
    int failed = 0, batched;

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
    /* [rexglue-native] 2026-11-09: batched (ngl_vk_set_batched_textures): staged for the next draw, below, with no
     * wait; else copied in and waited for now */
    batched = vk.batch_textures && vk.texture_staging_data && bytes <= TEXTURE_STAGING_HALF && !vk.low_frame_open;
    if (!batched)
        finish_frame();     /* its upload reuses the draws' command buffer and fence */
    n = vk.texture_count + 1;
    if (make_image(width, height, level_count, PICTURE_FORMAT,
                   VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, &vk.textures[n].image,
                   &vk.textures[n].memory, &vk.textures[n].view) ||
        (!batched &&
         make_host_buffer(bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, &staging, &staging_memory, &staging_pixels)))
        failed = 1;
    if (!failed && !batched) {
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
    if (!failed && !batched) {
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
                 submit_to_queue(&submit, vk.done) != VK_SUCCESS ||
                 vkWaitForFences(vk.device, 1, &vk.done, VK_TRUE, WAIT_NS) != VK_SUCCESS;
    }
    /* [acct2-bro 2026-11-08] a number given out again after ngl_vk_forget keeps its set (the pool can't free one) */
    if (!failed && !vk.textures[n].set) {
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
    /* [rexglue-native] 2026-11-09: last, so a texture that failed above leaves nothing waiting for its image */
    if (!failed && batched)
        failed = stage_texture(n, levels, level_count, copy, bytes) != 0;
    if (staging_memory)
        vkUnmapMemory(vk.device, staging_memory);
    vkDestroyBuffer(vk.device, staging, NULL);
    vkFreeMemory(vk.device, staging_memory, NULL);
    if (failed) {
        VkDescriptorSet set = vk.textures[n].set;   /* kept for the next texture with this number */

        vkDestroyImageView(vk.device, vk.textures[n].view, NULL);
        vkDestroyImage(vk.device, vk.textures[n].image, NULL);
        vkFreeMemory(vk.device, vk.textures[n].memory, NULL);
        memset(&vk.textures[n], 0, sizeof vk.textures[n]);
        vk.textures[n].set = set;
        fprintf(stderr, "[ngl_vk] a %ux%u texture couldn't be made\n", width, height);
        return 0;
    }
    vk.textures[n].cube = 0;    /* [rexglue-native] rn34: the number may have been a cube's before ngl_vk_forget */
    vk.texture_count = n;
    return n;
}

/* [rexglue-native] 2026-11-09 rn34: a cube texture (ngl_vk.h), copied in and waited for now (never batched: a level
 * has a few), with a descriptor set of the textures' one-sampler layout whose view is a cube. */
uint32_t ngl_vk_texture_cube(const void *const *faces, uint32_t level_count, uint32_t size)
{
    VkCommandBufferBeginInfo begin = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    VkSubmitInfo submit = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    VkDescriptorSetAllocateInfo set_info = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
    VkWriteDescriptorSet write = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
    VkDescriptorImageInfo image_info;
    VkBufferImageCopy copy[6 * MAX_TEXTURE_LEVELS];
    VkBuffer staging = VK_NULL_HANDLE;
    VkDeviceMemory staging_memory = VK_NULL_HANDLE;
    void *staging_pixels = NULL;
    VkDeviceSize bytes = 0;
    uint32_t n, level, face, copies = 0, most = 1;
    int failed = 0;

    if (!vk.device || !faces || !level_count || !size || vk.texture_count == MAX_TEXTURES)
        return 0;
    for (face = 0; face < 6; face++)
        if (!faces[face])
            return 0;
    while (most < MAX_TEXTURE_LEVELS && (size >> most))
        most++;                                         /* down to 1 x 1 */
    if (level_count > most)
        level_count = most;
    for (level = 0; level < level_count; level++) {     /* level i: max(1, size >> i) on a side, six faces */
        uint32_t side = size >> level ? size >> level : 1, given = 0;

        while (given < 6 && faces[6 * level + given])
            given++;
        if (given < 6) {                                /* the levels given stop here */
            level_count = level;
            break;
        }
        for (face = 0; face < 6; face++) {
            VkBufferImageCopy *c = &copy[copies++];

            memset(c, 0, sizeof *c);
            c->bufferOffset = bytes;
            c->imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            c->imageSubresource.mipLevel = level;
            c->imageSubresource.baseArrayLayer = face;
            c->imageSubresource.layerCount = 1;
            c->imageExtent.width = side;
            c->imageExtent.height = side;
            c->imageExtent.depth = 1;
            bytes += (VkDeviceSize)side * side * BYTES_PER_PIXEL;
        }
    }
    finish_frame();     /* its upload reuses the draws' command buffer and fence */
    n = vk.texture_count + 1;
    if (make_image_or_cube(size, size, level_count, 1, PICTURE_FORMAT,
                           VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, &vk.textures[n].image,
                           &vk.textures[n].memory, &vk.textures[n].view) ||
        make_host_buffer(bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, &staging, &staging_memory, &staging_pixels))
        failed = 1;
    if (!failed) {
        for (uint32_t i = 0; i < copies; i++)
            memcpy((uint8_t *)staging_pixels + copy[i].bufferOffset, faces[i],
                   (size_t)copy[i].imageExtent.width * copy[i].imageExtent.height * BYTES_PER_PIXEL);
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &vk.commands;
        failed = vkResetFences(vk.device, 1, &vk.done) != VK_SUCCESS ||
                 vkResetCommandBuffer(vk.commands, 0) != VK_SUCCESS ||
                 vkBeginCommandBuffer(vk.commands, &begin) != VK_SUCCESS;
    }
    if (!failed) {
        layers_barrier(vk.textures[n].image, level_count, 6, VK_IMAGE_LAYOUT_UNDEFINED,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
                       VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        vkCmdCopyBufferToImage(vk.commands, staging, vk.textures[n].image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                               copies, copy);
        layers_barrier(vk.textures[n].image, level_count, 6, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                       VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
                       VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                       VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
        failed = vkEndCommandBuffer(vk.commands) != VK_SUCCESS ||
                 submit_to_queue(&submit, vk.done) != VK_SUCCESS ||
                 vkWaitForFences(vk.device, 1, &vk.done, VK_TRUE, WAIT_NS) != VK_SUCCESS;
    }
    if (!failed && !vk.textures[n].set) {   /* a number given out again keeps its set, as ngl_vk_texture_levels' */
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
        VkDescriptorSet set = vk.textures[n].set;   /* kept for the next texture with this number */

        vkDestroyImageView(vk.device, vk.textures[n].view, NULL);
        vkDestroyImage(vk.device, vk.textures[n].image, NULL);
        vkFreeMemory(vk.device, vk.textures[n].memory, NULL);
        memset(&vk.textures[n], 0, sizeof vk.textures[n]);
        vk.textures[n].set = set;
        fprintf(stderr, "[ngl_vk] a %ux%u cube couldn't be made\n", size, size);
        return 0;
    }
    vk.textures[n].cube = 1;
    vk.texture_count = n;
    return n;
}

/* [acct2-bro 2026-11-08] Every texture and mesh forgotten (see ngl_vk.h). The last frame is waited for first, so
 * nothing the GPU may still read is freed. Each texture's descriptor set stays allocated for its number's next use. */
void ngl_vk_forget(void)
{
    uint32_t n;

    if (!vk.device)
        return;
    finish_frame();
    for (n = 1; n <= vk.texture_count; n++) {
        vkDestroyImageView(vk.device, vk.textures[n].view, NULL);
        vkDestroyImage(vk.device, vk.textures[n].image, NULL);
        vkFreeMemory(vk.device, vk.textures[n].memory, NULL);
        vk.textures[n].view = VK_NULL_HANDLE;
        vk.textures[n].image = VK_NULL_HANDLE;
        vk.textures[n].memory = VK_NULL_HANDLE;
        vk.textures[n].cube = 0;    /* [rexglue-native] rn34 */
    }
    vk.texture_count = 0;
    vk.mesh_count = 0;
    vk.corners_used = 0;
    vk.indices_used = 0;
    /* the copies still waiting were for the meshes just forgotten */
    vk.copy_count[0] = 0;
    vk.copy_count[1] = 0;
    vk.staged = 0;
    /* [rexglue-native] 2026-11-09: and the batched textures still waiting, for the images just freed */
    vk.pending_count = 0;
    vk.texture_staged = 0;
    /* numbers the caller set for this frame, no longer valid */
    vk.grass_blade = 0;
    vk.grass_patch_count = 0;
    vk.lookup_table = 0;
}

void ngl_vk_room(uint32_t *textures, uint32_t *texture_room, uint32_t *corners, uint32_t *corner_room)
{
    if (textures)
        *textures = vk.texture_count;
    if (texture_room)
        *texture_room = MAX_TEXTURES;
    if (corners)
        *corners = vk.corners_used;
    if (corner_room)
        *corner_room = MESH_CORNERS - 2 * FRAME_CORNERS;
}

static int make_dof(void);
static int make_tint(void);

/* [rexglue-native] 2026-11-09 rn34: the empty cube (set 6 for every mesh draw without a cube of its own): 1 x 1, black,
 * cleared once and left ready to sample, with its own set of the textures' one-sampler layout */
static int make_empty_cube(void)
{
    VkCommandBufferBeginInfo begin = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    VkSubmitInfo submit = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    VkDescriptorSetAllocateInfo set_info = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
    VkWriteDescriptorSet write = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
    VkDescriptorImageInfo image_info;
    VkClearColorValue black = { { 0.0f, 0.0f, 0.0f, 1.0f } };
    VkImageSubresourceRange all = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 6 };

    if (make_image_or_cube(1, 1, 1, 1, PICTURE_FORMAT, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                           &vk.empty_cube, &vk.empty_cube_memory, &vk.empty_cube_view))
        return -1;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &vk.commands;
    CHECK(vkResetFences(vk.device, 1, &vk.done));
    CHECK(vkResetCommandBuffer(vk.commands, 0));
    CHECK(vkBeginCommandBuffer(vk.commands, &begin));
    layers_barrier(vk.empty_cube, 1, 6, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
                   VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    vkCmdClearColorImage(vk.commands, vk.empty_cube, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &all);
    layers_barrier(vk.empty_cube, 1, 6, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                   VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                   VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    CHECK(vkEndCommandBuffer(vk.commands));
    CHECK(submit_to_queue(&submit, vk.done));
    CHECK(vkWaitForFences(vk.device, 1, &vk.done, VK_TRUE, WAIT_NS));
    set_info.descriptorPool = vk.descriptor_pool;
    set_info.descriptorSetCount = 1;
    set_info.pSetLayouts = &vk.set_layout;
    CHECK(vkAllocateDescriptorSets(vk.device, &set_info, &vk.empty_cube_set));
    image_info.sampler = vk.sampler;
    image_info.imageView = vk.empty_cube_view;
    image_info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    write.dstSet = vk.empty_cube_set;
    write.dstBinding = 0;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.pImageInfo = &image_info;
    vkUpdateDescriptorSets(vk.device, 1, &write, 0, NULL);
    return 0;
}

/* Everything a draw needs, once the device and its commands exist: the target and its passes, the pipelines, the
 * shadow; then the parts drawn without when they can't be made. */
static int make_drawing(void)
{
    if (make_target() || make_pipeline() || make_quad_pipeline() || make_shadow() || make_mesh_pipeline() ||
        (vk.fabric_sets && make_empty_cube()))     /* [rexglue-native] rn34 */
        return -1;
    if (make_grass_pipeline())
        fprintf(stderr, "[ngl_vk] no grass pipeline: the grass is left out\n");
    if (make_dof())
        fprintf(stderr, "[ngl_vk] no depth of field pipeline: drawn without it\n");
    if (make_tint())
        fprintf(stderr, "[ngl_vk] no tint pipeline: drawn without it\n");
    return 0;
}

int ngl_vk_init(uint32_t width, uint32_t height)
{
    VkApplicationInfo app = { VK_STRUCTURE_TYPE_APPLICATION_INFO };
    VkInstanceCreateInfo info = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };

    if (vk.device)
        return 0;
    if (!width || !height)
        return -1;
    if (ngl_vk_open_loader())   /* [rexglue-native] 2026-11-09: no Vulkan on this PC (said on stderr) */
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
    /* [rexglue-native] 2026-11-09: the instance's functions first */
    if (ngl_vk_load_instance(vk.instance) || choose_gpu() || make_device() || make_drawing()) {
        ngl_vk_shutdown();
        return -1;
    }
    /* [rexglue-native] 2026-11-09: (the swapchain's functions are there: the device was made with the extension) */
    if (vk.surface && (!vkCreateSwapchainKHR || make_swapchain())) {
        fprintf(stderr, "[ngl_vk] no swapchain for the window: drawing offscreen\n");
        drop_window();
    }
    return 0;
}

int ngl_vk_init_on_device(uint32_t width, uint32_t height, void *instance, void *gpu, void *device,
                          uint32_t queue_family, uint32_t queue_index, ngl_vk_queue_lock lock,
                          ngl_vk_queue_unlock unlock, void *user)
{
    VkPhysicalDeviceProperties properties;

    if (vk.device)
        return 0;
    if (!width || !height || !instance || !gpu || !device || !lock || !unlock)
        return -1;
    /* [rexglue-native] 2026-11-09: the owner's instance and device functions, from the loader the owner uses too */
    if (ngl_vk_open_loader() || ngl_vk_load_instance((VkInstance)instance) || ngl_vk_load_device((VkDevice)device))
        return -1;
    vk.width = width;
    vk.height = height;
    vk.borrowed = 1;
    vk.instance = (VkInstance)instance;
    vk.gpu = (VkPhysicalDevice)gpu;
    vk.device = (VkDevice)device;
    vk.queue_family = queue_family;
    vk.queue_lock = lock;
    vk.queue_unlock = unlock;
    vk.queue_user = user;
    vkGetDeviceQueue(vk.device, queue_family, queue_index, &vk.queue);
    vkGetPhysicalDeviceProperties(vk.gpu, &properties);
    snprintf(device_name, sizeof device_name, "%s", properties.deviceName);
    if (make_commands() || make_drawing()) {
        ngl_vk_shutdown();
        return -1;
    }
    return 0;
}

void ngl_vk_set_readback(int on)
{
    vk.no_readback = !on;
}

const char *ngl_vk_device_name(void)
{
    return device_name;
}

/* [rexglue-native] 2026-11-09 */
const char *ngl_vk_loader(void)
{
    return ngl_vk_open_loader() == 0 ? NGL_VK_LOADER_FILE : NULL;
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
    layers_barrier(image, levels, 1, from, to, from_access, to_access, from_stage, to_stage);
}

/* [rexglue-native] rn34: the same for the first `layers` layers too (a cube's six) */
static void layers_barrier(VkImage image, uint32_t levels, uint32_t layers, VkImageLayout from, VkImageLayout to,
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
    barrier.subresourceRange.layerCount = layers;
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

/* A depth image's layout change. */
static void depth_barrier(VkImage image, VkImageLayout from, VkImageLayout to, VkAccessFlags from_access,
                          VkAccessFlags to_access, VkPipelineStageFlags from_stage, VkPipelineStageFlags to_stage)
{
    VkImageMemoryBarrier barrier = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };

    barrier.srcAccessMask = from_access;
    barrier.dstAccessMask = to_access;
    barrier.oldLayout = from;
    barrier.newLayout = to;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.layerCount = 1;
    vkCmdPipelineBarrier(vk.commands, from_stage, to_stage, 0, 0, NULL, 0, NULL, 1, &barrier);
}

/* Between the halves: the opaque meshes' depth (left in the transfer layout by the first half) copied for mesh.frag,
 * then back to a depth attachment for the second half's tests. */
static void copy_soft_depth(void)
{
    VkImageCopy copy = { 0 };

    depth_barrier(vk.soft_depth, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                  VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                  VK_PIPELINE_STAGE_TRANSFER_BIT);
    copy.srcSubresource.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    copy.srcSubresource.layerCount = 1;
    copy.dstSubresource = copy.srcSubresource;
    copy.extent.width = vk.width;
    copy.extent.height = vk.height;
    copy.extent.depth = 1;
    vkCmdCopyImage(vk.commands, vk.depth, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, vk.soft_depth,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    depth_barrier(vk.soft_depth, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                  VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                  VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    depth_barrier(vk.depth, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                  VK_ACCESS_TRANSFER_READ_BIT,
                  VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                  VK_PIPELINE_STAGE_TRANSFER_BIT,
                  VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT);
}

/* Depth of field (ngl_vk_set_depth_of_field). The blur's render pass: one color attachment, every pixel drawn, left
 * ready to sample; after the blits that fill its source and the last pass's reads of the image it draws over, and
 * before the next pass reads it. */
static int make_dof_blur_pass(void)
{
    VkAttachmentDescription attachment = { 0 };
    VkAttachmentReference color = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
    VkSubpassDescription subpass = { 0 };
    VkSubpassDependency dependencies[2] = { { 0 }, { 0 } };
    VkRenderPassCreateInfo pass_info = { VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };

    attachment.format = TARGET_FORMAT;
    attachment.samples = VK_SAMPLE_COUNT_1_BIT;
    attachment.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    attachment.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &color;
    dependencies[0].srcSubpass = VK_SUBPASS_EXTERNAL;
    dependencies[0].dstSubpass = 0;
    dependencies[0].srcStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
                                   VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependencies[0].dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependencies[0].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    dependencies[0].dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    dependencies[1].srcSubpass = 0;
    dependencies[1].dstSubpass = VK_SUBPASS_EXTERNAL;
    dependencies[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependencies[1].dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    dependencies[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    dependencies[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    pass_info.attachmentCount = 1;
    pass_info.pAttachments = &attachment;
    pass_info.subpassCount = 1;
    pass_info.pSubpasses = &subpass;
    pass_info.dependencyCount = 2;
    pass_info.pDependencies = dependencies;
    CHECK(vkCreateRenderPass(vk.device, &pass_info, NULL, &vk.dof_blur_pass));
    return 0;
}

/* The main pass's third part (after the second half, when there is depth of field): the target loaded from where the
 * second half left it (ready to be copied, and read by the blits) and left the same way; the depth not used. Compatible
 * with the main pass, so its pipelines and framebuffer serve it. */
static int make_overlay_pass(void)
{
    VkAttachmentDescription attachments[2] = { { 0 }, { 0 } };
    VkAttachmentReference color = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
    VkAttachmentReference depth = { 1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL };
    VkSubpassDescription subpass = { 0 };
    VkSubpassDependency dependencies[2] = { { 0 }, { 0 } };
    VkRenderPassCreateInfo pass_info = { VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };

    attachments[0].format = TARGET_FORMAT;
    attachments[0].samples = VK_SAMPLE_COUNT_1_BIT;
    attachments[0].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    attachments[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachments[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachments[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[0].initialLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    attachments[0].finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    attachments[1].format = DEPTH_FORMAT;
    attachments[1].samples = VK_SAMPLE_COUNT_1_BIT;
    attachments[1].loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachments[1].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[1].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachments[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[1].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    attachments[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &color;
    subpass.pDepthStencilAttachment = &depth;
    /* in: after the second half's writes, the blits' reads and the blur's writes; out: to the copy.
     * [rexglue-native] 2026-11-09: the main pass's own set (target_pass_dependencies, which now holds this pass's
     * needs too): rn3's validation run had 325,000 "RenderPasses incompatible ... srcStageMask" from this pass's own */
    target_pass_dependencies(dependencies);
    pass_info.attachmentCount = 2;
    pass_info.pAttachments = attachments;
    pass_info.subpassCount = 1;
    pass_info.pSubpasses = &subpass;
    pass_info.dependencyCount = 2;
    pass_info.pDependencies = dependencies;
    CHECK(vkCreateRenderPass(vk.device, &pass_info, NULL, &vk.overlay_pass));
    return 0;
}

/* A pipeline that draws one triangle over the viewport (quad.vert) with the given fragment shader; blended 1: over
 * what is there by the alpha the shader writes (the target's own alpha kept). */
static int make_fullscreen_pipeline(const uint32_t *fragment_code, size_t fragment_bytes, VkPipelineLayout layout,
                                    VkRenderPass render_pass, int blended, VkPipeline *pipeline)
{
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
    VkPipelineDepthStencilStateCreateInfo no_depth = { VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
    VkDynamicState dynamic_states[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dynamic = { VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
    VkGraphicsPipelineCreateInfo info = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
    VkResult result;

    if (make_shader(quad_vert_spirv, sizeof quad_vert_spirv, &vertex_shader) ||
        make_shader(fragment_code, fragment_bytes, &fragment_shader)) {
        vkDestroyShaderModule(vk.device, vertex_shader, NULL);
        return -1;
    }
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
    if (blended) {
        blend_attachment.blendEnable = VK_TRUE;
        blend_attachment.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        blend_attachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        blend_attachment.colorBlendOp = VK_BLEND_OP_ADD;
        blend_attachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
        blend_attachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        blend_attachment.alphaBlendOp = VK_BLEND_OP_ADD;
    }
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
    info.layout = layout;
    info.renderPass = render_pass;
    info.subpass = 0;
    result = vkCreateGraphicsPipelines(vk.device, VK_NULL_HANDLE, 1, &info, NULL, pipeline);
    vkDestroyShaderModule(vk.device, vertex_shader, NULL);
    vkDestroyShaderModule(vk.device, fragment_shader, NULL);
    if (result != VK_SUCCESS) {
        fprintf(stderr, "[ngl_vk] vkCreateGraphicsPipelines (full screen) failed (%d)\n", (int)result);
        return -1;
    }
    return 0;
}

/* Depth of field's images, passes, sets and pipelines (after the main pass, the textures' pool and the shadow's set,
 * whose binding 2, the opaque depth, the mix reads). */
static int make_dof(void)
{
    VkDescriptorSetLayout layouts[DOF_SETS] = { vk.set_layout, vk.set_layout };
    VkDescriptorSet sets[DOF_SETS] = { VK_NULL_HANDLE, VK_NULL_HANDLE };
    VkDescriptorSetAllocateInfo set_info = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
    VkDescriptorImageInfo images[DOF_SETS] = { { 0 }, { 0 } };
    VkWriteDescriptorSet writes[DOF_SETS] = { { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET },
                                              { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET } };
    VkFramebufferCreateInfo framebuffer_info = { VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
    VkPushConstantRange blur_range = { VK_SHADER_STAGE_FRAGMENT_BIT, 0, 2 * sizeof(float) };
    VkPushConstantRange mix_range = { VK_SHADER_STAGE_FRAGMENT_BIT, 0, 4 * sizeof(float) };
    VkDescriptorSetLayout mix_layouts[2] = { vk.set_layout, vk.shadow_set_layout };
    VkPipelineLayoutCreateInfo layout_info = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };

    vk.dof_width = vk.width / 4u ? vk.width / 4u : 1u;
    vk.dof_height = vk.height / 4u ? vk.height / 4u : 1u;
    /* [rexglue-native] 2026-11-09: sampled too, only because make_image gives every image a view and a view needs a
     * usage that has one (the validation layer, VUID-VkImageViewCreateInfo-image-04441) */
    if (make_image(vk.dof_width * 2u, vk.dof_height * 2u, 1, TARGET_FORMAT,
                   VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                   &vk.dof_half, &vk.dof_half_memory, &vk.dof_half_view) ||
        make_image(vk.dof_width, vk.dof_height, 1, TARGET_FORMAT,
                   VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                       VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
                   &vk.dof_quarter, &vk.dof_quarter_memory, &vk.dof_quarter_view) ||
        make_image(vk.dof_width, vk.dof_height, 1, TARGET_FORMAT,
                   VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
                   &vk.dof_temp, &vk.dof_temp_memory, &vk.dof_temp_view) ||
        make_dof_blur_pass() || make_overlay_pass())
        return -1;

    framebuffer_info.renderPass = vk.dof_blur_pass;
    framebuffer_info.attachmentCount = 1;
    framebuffer_info.width = vk.dof_width;
    framebuffer_info.height = vk.dof_height;
    framebuffer_info.layers = 1;
    framebuffer_info.pAttachments = &vk.dof_quarter_view;
    CHECK(vkCreateFramebuffer(vk.device, &framebuffer_info, NULL, &vk.dof_quarter_framebuffer));
    framebuffer_info.pAttachments = &vk.dof_temp_view;
    CHECK(vkCreateFramebuffer(vk.device, &framebuffer_info, NULL, &vk.dof_temp_framebuffer));

    set_info.descriptorPool = vk.descriptor_pool;
    set_info.descriptorSetCount = DOF_SETS;
    set_info.pSetLayouts = layouts;
    CHECK(vkAllocateDescriptorSets(vk.device, &set_info, sets));
    vk.dof_quarter_set = sets[0];
    vk.dof_temp_set = sets[1];
    images[0].sampler = vk.sampler;
    images[0].imageView = vk.dof_quarter_view;
    images[0].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    images[1] = images[0];
    images[1].imageView = vk.dof_temp_view;
    for (uint32_t i = 0; i < DOF_SETS; i++) {
        writes[i].dstSet = sets[i];
        writes[i].dstBinding = 0;
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[i].pImageInfo = &images[i];
    }
    vkUpdateDescriptorSets(vk.device, DOF_SETS, writes, 0, NULL);

    layout_info.setLayoutCount = 1;
    layout_info.pSetLayouts = &vk.set_layout;
    layout_info.pushConstantRangeCount = 1;
    layout_info.pPushConstantRanges = &blur_range;
    CHECK(vkCreatePipelineLayout(vk.device, &layout_info, NULL, &vk.dof_blur_layout));
    layout_info.setLayoutCount = 2;
    layout_info.pSetLayouts = mix_layouts;
    layout_info.pPushConstantRanges = &mix_range;
    CHECK(vkCreatePipelineLayout(vk.device, &layout_info, NULL, &vk.dof_mix_layout));
    if (make_fullscreen_pipeline(dof_blur_frag_spirv, sizeof dof_blur_frag_spirv, vk.dof_blur_layout,
                                 vk.dof_blur_pass, 0, &vk.dof_blur_pipeline) ||
        make_fullscreen_pipeline(dof_mix_frag_spirv, sizeof dof_mix_frag_spirv, vk.dof_mix_layout, vk.render_pass, 1,
                                 &vk.dof_mix_pipeline))
        return -1;
    return 0;
}

void ngl_vk_set_depth_of_field(float near_depth, float far_depth, float keep_depth, float blanket)
{
    vk.dof_on = 1;
    vk.dof_focus[0] = near_depth;
    vk.dof_focus[1] = far_depth;
    vk.dof_focus[2] = keep_depth;
    vk.dof_focus[3] = blanket;
}

/* The tint's copy of the target (sampled through set 0) and its pipeline, drawn in the overlay pass. */
static int make_tint(void)
{
    VkDescriptorSetAllocateInfo set_info = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
    VkDescriptorImageInfo image = { 0 };
    VkWriteDescriptorSet write = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
    VkPushConstantRange range = { VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof vk.tint };  /* rn41b: (was 12 floats) */
    /* the target's copy, the opaque depth, the last frame's picture, the color look-up's table */
    VkDescriptorSetLayout layouts[4] = { vk.set_layout, vk.shadow_set_layout, vk.set_layout, vk.set_layout };
    VkPipelineLayoutCreateInfo layout_info = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };

    if (!vk.overlay_pass || !vk.shadow_set_layout)
        return -1;
    if (make_image(vk.width, vk.height, 1, TARGET_FORMAT, VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                   &vk.post_history, &vk.post_history_memory, &vk.post_history_view))
        return -1;
    if (make_image(vk.width, vk.height, 1, TARGET_FORMAT, VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                   &vk.post_copy, &vk.post_copy_memory, &vk.post_copy_view))
        return -1;
    set_info.descriptorPool = vk.descriptor_pool;
    set_info.descriptorSetCount = 1;
    set_info.pSetLayouts = &vk.set_layout;
    CHECK(vkAllocateDescriptorSets(vk.device, &set_info, &vk.post_copy_set));
    image.sampler = vk.sampler;
    image.imageView = vk.post_copy_view;
    image.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    write.dstSet = vk.post_copy_set;
    write.dstBinding = 0;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.pImageInfo = &image;
    vkUpdateDescriptorSets(vk.device, 1, &write, 0, NULL);
    CHECK(vkAllocateDescriptorSets(vk.device, &set_info, &vk.post_history_set));
    image.imageView = vk.post_history_view;
    write.dstSet = vk.post_history_set;
    vkUpdateDescriptorSets(vk.device, 1, &write, 0, NULL);

    layout_info.setLayoutCount = 4;
    layout_info.pSetLayouts = layouts;
    layout_info.pushConstantRangeCount = 1;
    layout_info.pPushConstantRanges = &range;
    CHECK(vkCreatePipelineLayout(vk.device, &layout_info, NULL, &vk.tint_layout));
    return make_fullscreen_pipeline(tint_frag_spirv, sizeof tint_frag_spirv, vk.tint_layout, vk.render_pass, 0,
                                    &vk.tint_pipeline);
}

/* The tint's push constants with nothing to do: no amount, brightness 1, power 1. */
static void reset_tint(void)
{
    memset(vk.tint, 0, sizeof vk.tint);
    vk.tint[8] = 1.0f;
    vk.tint[9] = 1.0f;
}

static float clear_color[3];    /* ngl_vk_set_clear_color's; black until set */

void ngl_vk_set_clear_color(const float rgb[3])
{
    for (int i = 0; i < 3; i++)
        clear_color[i] = rgb && rgb[i] > 0.0f ? (rgb[i] < 1.0f ? rgb[i] : 1.0f) : 0.0f;
}

void ngl_vk_set_brightness(float brightness, float power, float keep_depth)
{
    if (!vk.tint_on)
        reset_tint();
    vk.tint_on = 1;
    vk.tint[7] = keep_depth;
    vk.tint[8] = brightness;
    vk.tint[9] = power;
}

void ngl_vk_set_motion_blur(float amount, float keep_depth)
{
    if (!vk.tint_on)
        reset_tint();
    vk.tint_on = 1;
    vk.motion_on = 1;
    vk.tint[7] = keep_depth;
    vk.tint[10] = amount;
}

void ngl_vk_set_color_lookup(uint32_t table, float keep_depth)
{
    if (!vk.tint_on)
        reset_tint();
    vk.tint_on = 1;
    vk.tint[7] = keep_depth;
    vk.tint[11] = 1.0f;
    vk.lookup_table = table;
}

/* [rexglue-native] 2026-11-10 rn41b: post effect 7, drawn after the quads with the tint's pipeline (draw: film; tint.frag
 * film_at, main) */
void ngl_vk_set_film(const float first[4], const float gray[3], float scale, const float move[2], float moved_share,
                     float keep_depth)
{
    if (!first || !gray || !move || !(scale > 0.0f))
        return;
    if (!vk.tint_on)
        reset_tint();
    vk.tint_on = 1;
    vk.tint[7] = keep_depth;
    for (int i = 0; i < 4; i++)
        vk.tint[12 + i] = first[i];
    for (int i = 0; i < 3; i++)
        vk.tint[16 + i] = gray[i];
    vk.tint[19] = 1.0f;
    vk.tint[20] = scale;
    vk.tint[21] = move[0];
    vk.tint[22] = move[1];
    vk.tint[23] = moved_share;
}

void ngl_vk_set_tint(float amount, const float weights[3], const float color[3], float keep_depth)
{
    if (!vk.tint_on)
        reset_tint();
    vk.tint_on = 1;
    vk.tint[0] = weights[0];
    vk.tint[1] = weights[1];
    vk.tint[2] = weights[2];
    vk.tint[3] = amount;
    vk.tint[4] = color[0];
    vk.tint[5] = color[1];
    vk.tint[6] = color[2];
    vk.tint[7] = keep_depth;
}

/* After the tint's pass (its overlay pass ended, the target left ready to be copied): the target copied whole into
 * post_history, the last frame's 3D for the next frame's motion blur. */
static void record_history_copy(void)
{
    VkImageCopy copy = { 0 };

    image_barrier(vk.target, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                  VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                  VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    image_barrier(vk.post_history, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                  VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                  VK_PIPELINE_STAGE_TRANSFER_BIT);
    copy.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    copy.srcSubresource.layerCount = 1;
    copy.dstSubresource = copy.srcSubresource;
    copy.extent.width = vk.width;
    copy.extent.height = vk.height;
    copy.extent.depth = 1;
    vkCmdCopyImage(vk.commands, vk.target, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, vk.post_history,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    image_barrier(vk.post_history, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                  VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                  VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    image_barrier(vk.target, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                  VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                  VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
    vk.history_frame = vk.frames_done + 1;  /* this frame's (frames_done counts the frames finished) */
}

/* Between a part of the main pass and the overlay pass: the target (left ready to be copied) copied whole for the
 * tint to read. */
static void record_post_copy(void)
{
    VkImageCopy copy = { 0 };

    if (!vk.history_layout_ok) {  /* the last frame's, sampled before anything is copied in (its share 0 then) */
        image_barrier(vk.post_history, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0,
                      VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                      VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
        vk.history_layout_ok = 1;
    }

    image_barrier(vk.post_copy, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
                  VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    copy.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    copy.srcSubresource.layerCount = 1;
    copy.dstSubresource = copy.srcSubresource;
    copy.extent.width = vk.width;
    copy.extent.height = vk.height;
    copy.extent.depth = 1;
    vkCmdCopyImage(vk.commands, vk.target, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, vk.post_copy,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    image_barrier(vk.post_copy, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                  VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                  VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
}

/* [rexglue-native] 2026-11-10 rn41b: in a frame with a motion blur or post effect 7, after the tint's draw (its pass
 * ended, the target left ready to be copied): the target copied again into post_copy (which that draw has read) for
 * their own draws (rn41g: the motion blur's, then post effect 7's). */
static void record_film_copy(void)
{
    VkImageCopy copy = { 0 };

    image_barrier(vk.target, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                  VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                  VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    image_barrier(vk.post_copy, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                  VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                  VK_PIPELINE_STAGE_TRANSFER_BIT);
    copy.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    copy.srcSubresource.layerCount = 1;
    copy.dstSubresource = copy.srcSubresource;
    copy.extent.width = vk.width;
    copy.extent.height = vk.height;
    copy.extent.depth = 1;
    vkCmdCopyImage(vk.commands, vk.target, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, vk.post_copy,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    image_barrier(vk.post_copy, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                  VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                  VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    image_barrier(vk.target, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                  VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                  VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
}

/* Between the second half and the overlay pass: the target (left ready to be copied) shrunk to a half and then a
 * quarter of its size, then blurred across into dof_temp and down back into dof_quarter, rounds times, the taps step
 * of its texels apart (1.5: about 12 of the target's at the Gaussian's sigma). */
static void record_dof(int rounds, float spacing)
{
    VkImageBlit blit = { 0 };
    VkRenderPassBeginInfo pass = { VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
    VkViewport view = { 0 };
    VkRect2D area = { { 0, 0 }, { 0, 0 } };

    image_barrier(vk.dof_half, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
                  VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    blit.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    blit.srcSubresource.layerCount = 1;
    blit.dstSubresource = blit.srcSubresource;
    blit.srcOffsets[1].x = (int32_t)vk.width;
    blit.srcOffsets[1].y = (int32_t)vk.height;
    blit.srcOffsets[1].z = 1;
    blit.dstOffsets[1].x = (int32_t)(vk.dof_width * 2u);
    blit.dstOffsets[1].y = (int32_t)(vk.dof_height * 2u);
    blit.dstOffsets[1].z = 1;
    vkCmdBlitImage(vk.commands, vk.target, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, vk.dof_half,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);
    image_barrier(vk.dof_half, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                  VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                  VK_PIPELINE_STAGE_TRANSFER_BIT);
    image_barrier(vk.dof_quarter, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
                  VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    blit.srcOffsets[1] = blit.dstOffsets[1];
    blit.dstOffsets[1].x = (int32_t)vk.dof_width;
    blit.dstOffsets[1].y = (int32_t)vk.dof_height;
    vkCmdBlitImage(vk.commands, vk.dof_half, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, vk.dof_quarter,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);
    image_barrier(vk.dof_quarter, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                  VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                  VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);

    view.width = (float)vk.dof_width;
    view.height = (float)vk.dof_height;
    view.maxDepth = 1.0f;
    area.extent.width = vk.dof_width;
    area.extent.height = vk.dof_height;
    pass.renderPass = vk.dof_blur_pass;
    pass.renderArea = area;
    for (int pass_number = 0; pass_number < 2 * rounds; pass_number++) {
        int down = pass_number & 1;
        float step[2] = { down ? 0.0f : spacing / (float)vk.dof_width, down ? spacing / (float)vk.dof_height : 0.0f };

        pass.framebuffer = down ? vk.dof_quarter_framebuffer : vk.dof_temp_framebuffer;
        vkCmdBeginRenderPass(vk.commands, &pass, VK_SUBPASS_CONTENTS_INLINE);
        vkCmdSetViewport(vk.commands, 0, 1, &view);
        vkCmdSetScissor(vk.commands, 0, 1, &area);
        vkCmdBindPipeline(vk.commands, VK_PIPELINE_BIND_POINT_GRAPHICS, vk.dof_blur_pipeline);
        vkCmdBindDescriptorSets(vk.commands, VK_PIPELINE_BIND_POINT_GRAPHICS, vk.dof_blur_layout, 0, 1,
                                down ? &vk.dof_temp_set : &vk.dof_quarter_set, 0, NULL);
        vkCmdPushConstants(vk.commands, vk.dof_blur_layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof step, step);
        vkCmdDraw(vk.commands, 3, 1, 0, 0);
        vkCmdEndRenderPass(vk.commands);
    }
}

/* [f2-menus] Something drawn over the finished picture (Big Red One's F2 settings window): called at the end of
 * every frame, before the target is copied out and shown, with the frame's command buffer and the target in
 * TRANSFER_SRC_OPTIMAL, which it leaves so. ngl_vk_backend.h. */
static ngl_vk_overlay_fn overlay_fn;
static void *overlay_user;

void ngl_vk_set_overlay(ngl_vk_overlay_fn fn, void *user)
{
    overlay_fn = fn;
    overlay_user = user;
}

/* The end of a frame, after its last pass (the target in TRANSFER_SRC_OPTIMAL): the target copied out for the host
 * and into the window's image when there is one, the commands submitted, the image shown, and the frame's one-shot
 * settings cleared. draw()'s end, and ngl_vk_end_frame()'s (a lower layer's frame). */
static int submit_and_show(int showing, uint32_t window_image)
{
    VkBufferImageCopy download = { 0 };
    VkBufferMemoryBarrier to_host = { VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER };
    VkSubmitInfo submit = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkPresentInfoKHR present = { VK_STRUCTURE_TYPE_PRESENT_INFO_KHR };

    /* [f2-menus] Nothing unless ngl_vk_set_overlay() was called (Call of Duty 3 never calls it). Big Red One's F2
     * window is drawn here; its Dear ImGui backend may make a submit of its own on this queue (its font picture),
     * which is fine on a device ngl_vk owns, on this thread, before the frame's submit. It must NOT happen on
     * Call of Duty 3's shared device (cod3_native_shared_device: rexglue's presenter submits to that queue under
     * ngl_vk's queue lock): an overlay there may only record. [rexglue-native 2026-11-10 12:08] */
    if (overlay_fn)
        overlay_fn(vk.commands, overlay_user);

    /* the target out, for the host (unless ngl_vk_set_readback(0): then it only goes on through ngl_vk_output) */
    vk.read_back = !vk.no_readback;
    if (vk.read_back) {
        download.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        download.imageSubresource.layerCount = 1;
        download.imageExtent.width = vk.width;
        download.imageExtent.height = vk.height;
        download.imageExtent.depth = 1;
        vkCmdCopyImageToBuffer(vk.commands, vk.target, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, vk.readback, 1,
                               &download);
        to_host.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        to_host.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        to_host.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        to_host.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        to_host.buffer = vk.readback;
        to_host.size = VK_WHOLE_SIZE;
        vkCmdPipelineBarrier(vk.commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 0, NULL, 1,
                             &to_host, 0, NULL);
    }
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
    CHECK(submit_to_queue(&submit, vk.done));
    if (vk.frame_corners_written) {     /* side214: this draw reads these corners; the next frame writes the others */
        vk.frame_corner_half ^= 1u;
        vk.frame_corners_written = 0;
    }
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
    /* waited for when something needs it (finish_frame), on ngl_vk's own device too (2026-11-07, acct2-bro-game's
       proposal: draw finishes the last frame before it reuses the command buffer or the window's semaphore, and
       every writer of memory the GPU reads, and the read-back, finish it first) */
    vk.frame_pending = 1;
    vk.drawn = 1;
    vk.shadow_on = 0;       /* a sun's shadow is set for one frame */
    vk.dof_on = 0;          /* and depth of field */
    vk.tint_on = 0;         /* and a tint or a brightness */
    vk.lookup_table = 0;    /* and a color look-up */
    vk.motion_on = 0;       /* and a motion blur */
    vk.frames_done++;
    return 0;
}

static int draw(const void *pixels, uint32_t width, uint32_t height, uint32_t pitch,
                const struct ngl_vk_vertex *quad_vertices, uint32_t vertex_count,
                float screen_width, float screen_height)
{
    VkCommandBufferBeginInfo begin = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    VkRenderPassBeginInfo pass = { VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
    VkClearValue clear[2] = { { { { clear_color[0], clear_color[1], clear_color[2], 1.0f } } },
                              { { { 1.0f, 0.0f, 0.0f, 0.0f } } } };
    VkBufferImageCopy upload = { 0 };
    VkViewport view = { 0 };
    VkRect2D whole = { { 0, 0 }, { 0, 0 } };
    float scale_x, scale_y, scale, fit_width, fit_height;
    float screen[2] = { screen_width, screen_height };
    VkDeviceSize no_offset = 0;
    uint32_t row, window_image = 0;
    int showing;
    /* meshes this frame: the main pass in two halves, the depth copied between them (soft effects) */
    int halves = quad_vertices && vk.draw_count && vk.opaque_pass && vk.blend_pass && vk.soft_depth;
    /* and depth of field: a third part after them, the blur mixed in under the quads */
    int dof = halves && vk.dof_on && vk.dof_mix_pipeline && vk.overlay_pass;
    /* and a tint: a fourth part, the target copied and drawn back toned */
    int tint = halves && vk.tint_on && vk.tint_pipeline && vk.overlay_pass;
    /* [rexglue-native] 2026-11-10 rn41b, rn41g: and the motion blur and post effect 7: draws of their own after the
     * tint's, with its pipeline and nothing else of it (motion_constants, film_constants), over the whole 3D, the
     * scenes in front included; the last frame's picture kept after the quads */
    int film = tint && vk.tint[20] > 0.0f;
    float film_constants[24];
    int motion = tint && vk.motion_on;
    float motion_constants[24];

    if (!vk.device || !pixels || !width || !height || pitch < width * BYTES_PER_PIXEL)
        return -1;
    finish_frame();     /* the last draw's, when frames overlap */
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
    if (vk.geometry_on_gpu)
        record_staged_copies(vk.commands);  /* side220: this frame's new corners and indices, before any draw */
    record_texture_uploads();               /* [rexglue-native] 2026-11-09: and its new textures (batched) */

    /* the picture in (finish_frame above: the last draw has finished with it) */
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
    if (vk.soft_depth && !vk.soft_ready && quad_vertices && vk.draw_count) {   /* sampled layout before any draw */
        depth_barrier(vk.soft_depth, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0,
                      VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                      VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
        vk.soft_ready = 1;
    }
    if (halves)
        pass.renderPass = vk.opaque_pass;
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

        if (halves) {
            draw_meshes(0);
            vkCmdEndRenderPass(vk.commands);
            copy_soft_depth();
            pass.renderPass = vk.blend_pass;
            vkCmdBeginRenderPass(vk.commands, &pass, VK_SUBPASS_CONTENTS_INLINE);
            vkCmdSetViewport(vk.commands, 0, 1, &view);
            vkCmdSetScissor(vk.commands, 0, 1, &whole);
            vk.soft_live = 1;
            draw_meshes(1);
            vk.soft_live = 0;
            if (dof) {
                VkViewport all = { 0.0f, 0.0f, (float)vk.width, (float)vk.height, 0.0f, 1.0f };
                VkDescriptorSet sets[2] = { vk.dof_quarter_set, vk.shadow_set };

                vkCmdEndRenderPass(vk.commands);
                /* the blanket (the menus' blur) is wider than the depth of field: two rounds, the taps further apart */
                if (vk.dof_focus[3] > 0.0f)
                    record_dof(2, 2.0f);
                else
                    record_dof(1, 1.5f);
                pass.renderPass = vk.overlay_pass;
                vkCmdBeginRenderPass(vk.commands, &pass, VK_SUBPASS_CONTENTS_INLINE);
                vkCmdSetViewport(vk.commands, 0, 1, &all);     /* over the whole target, as the blits took it */
                vkCmdSetScissor(vk.commands, 0, 1, &whole);
                vkCmdBindPipeline(vk.commands, VK_PIPELINE_BIND_POINT_GRAPHICS, vk.dof_mix_pipeline);
                vkCmdBindDescriptorSets(vk.commands, VK_PIPELINE_BIND_POINT_GRAPHICS, vk.dof_mix_layout, 0, 2, sets, 0,
                                        NULL);
                vkCmdPushConstants(vk.commands, vk.dof_mix_layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                                   sizeof vk.dof_focus, vk.dof_focus);
                vkCmdDraw(vk.commands, 3, 1, 0, 0);
                vkCmdSetViewport(vk.commands, 0, 1, &view);
            }
            if (tint) {
                VkViewport all = { 0.0f, 0.0f, (float)vk.width, (float)vk.height, 0.0f, 1.0f };
                /* set 3, the color look-up's table: any set when there is none (the shader doesn't read it) */
                int lookup = vk.lookup_table && vk.lookup_table <= vk.texture_count;
                VkDescriptorSet sets[4] = { vk.post_copy_set, vk.shadow_set, vk.post_history_set,
                                            lookup ? vk.textures[vk.lookup_table].set : vk.post_copy_set };

                vkCmdEndRenderPass(vk.commands);
                record_post_copy();
                /* the last frame's share only when post_history holds the last frame (a motion blur that starts
                 * blends nothing in its first frame) */
                if (!vk.motion_on || !vk.history_frame || vk.history_frame != vk.frames_done)
                    vk.tint[10] = 0.0f;
                pass.renderPass = vk.overlay_pass;
                vkCmdBeginRenderPass(vk.commands, &pass, VK_SUBPASS_CONTENTS_INLINE);
                vkCmdSetViewport(vk.commands, 0, 1, &all);
                vkCmdSetScissor(vk.commands, 0, 1, &whole);
                vkCmdBindPipeline(vk.commands, VK_PIPELINE_BIND_POINT_GRAPHICS, vk.tint_pipeline);
                if (!lookup)
                    vk.tint[11] = 0.0f;
                vkCmdBindDescriptorSets(vk.commands, VK_PIPELINE_BIND_POINT_GRAPHICS, vk.tint_layout, 0, 4, sets, 0,
                                        NULL);
                /* rn41b: post effect 7 not in this draw: its own after it (film_constants: the tint pass's
                 * neutral numbers, nothing kept in front, and its own) */
                memset(film_constants, 0, sizeof film_constants);
                film_constants[7] = -1.0f;
                film_constants[8] = 1.0f;
                film_constants[9] = 1.0f;
                memcpy(film_constants + 12, vk.tint + 12, 12 * sizeof(float));
                vk.tint[20] = 0.0f;
                /* rn41g: nor the motion blur: its own, before post effect 7's */
                memcpy(motion_constants, film_constants, sizeof motion_constants);
                motion_constants[10] = vk.tint[10];
                motion_constants[20] = 0.0f;
                vk.tint[10] = 0.0f;
                vkCmdPushConstants(vk.commands, vk.tint_layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof vk.tint,
                                   vk.tint);
                vkCmdDraw(vk.commands, 3, 1, 0, 0);
                /* rn41g: then the motion blur and post effect 7, each over the whole 3D, the scenes in front
                 * included (the game's concussion blurs its gun): the target copied again and drawn back. The
                 * quads come after them (the game's crosshair stays sharp), and the last frame's picture is kept
                 * after the quads (its compass trails: its motion blur's last frame holds the HUD) */
                for (int step = 0; step < 2; step++) {
                    const float *constants = step == 0 ? (motion ? motion_constants : NULL)
                                                       : (film ? film_constants : NULL);

                    if (!constants)
                        continue;
                    vkCmdEndRenderPass(vk.commands);
                    record_film_copy();
                    vkCmdBeginRenderPass(vk.commands, &pass, VK_SUBPASS_CONTENTS_INLINE);
                    vkCmdSetViewport(vk.commands, 0, 1, &all);
                    vkCmdSetScissor(vk.commands, 0, 1, &whole);
                    vkCmdBindPipeline(vk.commands, VK_PIPELINE_BIND_POINT_GRAPHICS, vk.tint_pipeline);
                    vkCmdBindDescriptorSets(vk.commands, VK_PIPELINE_BIND_POINT_GRAPHICS, vk.tint_layout, 0, 4,
                                            sets, 0, NULL);
                    vkCmdPushConstants(vk.commands, vk.tint_layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                                       sizeof film_constants, constants);
                    vkCmdDraw(vk.commands, 3, 1, 0, 0);
                }
                vkCmdSetViewport(vk.commands, 0, 1, &view);
            }
        } else if (vk.draw_count) {
            draw_meshes(2);
        }

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
    if (motion)  /* rn41g: the whole picture, the quads included, kept for the next frame's motion blur */
        record_history_copy();
    return submit_and_show(showing, window_image);
}

/* ---------------------------------------------------------------------------------------------------------------
 * [acct2-bro] 2026-11-08: the backend (ngl_vk_backend.h), for a lower layer that records frames with its own
 * pipelines on ngl_vk's device and target (acct2-waw's ngl_vk_low: World at War, Call of Duty 4). */

int ngl_vk_backend(struct ngl_vk_backend *out)
{
    if (!out || !vk.device || !vk.target)
        return -1;
    memset(out, 0, sizeof *out);
    out->instance = vk.instance;
    out->gpu = vk.gpu;
    out->device = vk.device;
    out->queue = vk.queue;
    out->queue_family = vk.queue_family;
    out->width = vk.width;
    out->height = vk.height;
    out->target = vk.target;
    out->depth = vk.depth;
    out->target_view = vk.target_view;
    out->depth_view = vk.depth_view;
    out->target_format = TARGET_FORMAT;
    out->depth_format = DEPTH_FORMAT;
    out->target_layout_at_end = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;  /* what the copies out and the window take */
    out->frames_in_flight = 1;
    out->enabled_features = vk.enabled_features;
    return 0;
}

void ngl_vk_set_device_features(const VkPhysicalDeviceFeatures *features)
{
    if (features)
        vk.enabled_features = *features;
    else
        memset(&vk.enabled_features, 0, sizeof vk.enabled_features);
}

VkCommandBuffer ngl_vk_begin_frame(void)
{
    VkCommandBufferBeginInfo begin = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };

    if (!vk.device || !vk.target || vk.low_frame_open)
        return VK_NULL_HANDLE;
    finish_frame();     /* the last frame's commands and memory are free again */
    vk.low_showing = acquire_window_image(&vk.low_window_image);
    if (vkResetFences(vk.device, 1, &vk.done) != VK_SUCCESS || vkResetCommandBuffer(vk.commands, 0) != VK_SUCCESS)
        return VK_NULL_HANDLE;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vkBeginCommandBuffer(vk.commands, &begin) != VK_SUCCESS)
        return VK_NULL_HANDLE;
    if (vk.geometry_on_gpu)
        record_staged_copies(vk.commands);
    record_texture_uploads();   /* [rexglue-native] 2026-11-09: new textures waiting (batched), before any draw */
    /* the target and the depth start each frame in their attachment layouts, their old contents dropped (as
     * draw()'s passes start them); after every earlier use of the target, ngl_vk_output's reads included */
    image_barrier(vk.target, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, 0,
                  VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                  VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
    depth_barrier(vk.depth, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, 0,
                  VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                  VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT);
    vk.low_frame_open = 1;
    return vk.commands;
}

int ngl_vk_end_frame(void)
{
    if (!vk.low_frame_open)
        return -1;
    vk.low_frame_open = 0;
    return submit_and_show(vk.low_showing, vk.low_window_image);
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
    if (!vk.drawn || !vk.read_back)
        return NULL;
    finish_frame();     /* the copy out is done */
    *width = vk.width;
    *height = vk.height;
    return (const uint8_t *)vk.readback_pixels;
}

/* ngl_vk_output's color table (256 x 1), its upload buffer, its two sets, its command buffer and fence: made once. */
static int make_output_parts(void)
{
    VkDescriptorSetLayout layouts[OUTPUT_SETS] = { vk.set_layout, vk.set_layout };
    VkDescriptorSet sets[OUTPUT_SETS] = { VK_NULL_HANDLE, VK_NULL_HANDLE };
    VkDescriptorSetAllocateInfo set_info = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
    VkDescriptorImageInfo images[OUTPUT_SETS] = { { 0 }, { 0 } };
    VkWriteDescriptorSet writes[OUTPUT_SETS] = { { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET },
                                                 { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET } };
    VkPipelineLayoutCreateInfo layout_info = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    VkCommandBufferAllocateInfo command_info = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    VkFenceCreateInfo fence_info = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    uint32_t i;

    if (vk.output_layout)
        return 0;
    if (make_image(256, 1, 1, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                   &vk.table_image, &vk.table_memory, &vk.table_view) ||
        make_host_buffer(sizeof vk.table_words, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, &vk.table_upload,
                         &vk.table_upload_memory, &vk.table_upload_data))
        return -1;
    set_info.descriptorPool = vk.descriptor_pool;
    set_info.descriptorSetCount = OUTPUT_SETS;
    set_info.pSetLayouts = layouts;
    CHECK(vkAllocateDescriptorSets(vk.device, &set_info, sets));
    vk.output_target_set = sets[0];
    vk.output_table_set = sets[1];
    images[0].sampler = vk.sampler;
    images[0].imageView = vk.target_view;
    images[0].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    images[1] = images[0];
    images[1].imageView = vk.table_view;
    for (i = 0; i < OUTPUT_SETS; i++) {
        writes[i].dstSet = sets[i];
        writes[i].dstBinding = 0;
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[i].pImageInfo = &images[i];
    }
    vkUpdateDescriptorSets(vk.device, OUTPUT_SETS, writes, 0, NULL);
    command_info.commandPool = vk.command_pool;
    command_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    command_info.commandBufferCount = 1;
    CHECK(vkAllocateCommandBuffers(vk.device, &command_info, &vk.output_commands));
    CHECK(vkCreateFence(vk.device, &fence_info, NULL, &vk.output_done));
    layout_info.setLayoutCount = OUTPUT_SETS;
    layout_info.pSetLayouts = layouts;
    CHECK(vkCreatePipelineLayout(vk.device, &layout_info, NULL, &vk.output_layout));
    return 0;
}

/* ngl_vk_output's pass and pipeline for images of this format (made again if the format changes). The image is put
 * in the attachment layout before the pass and taken to its final layout after it, so the pass itself changes no
 * layout and waits for nothing (the barriers around it do). */
static int make_output_pass(VkFormat format)
{
    VkAttachmentDescription attachment = { 0 };
    VkAttachmentReference color = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
    VkSubpassDescription subpass = { 0 };
    VkRenderPassCreateInfo pass_info = { VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };

    if (vk.output_pass && vk.output_format == format)
        return 0;
    if (vk.output_pass) {
        if (vk.output_pending)
            vkWaitForFences(vk.device, 1, &vk.output_done, VK_TRUE, WAIT_NS);
        vk.output_pending = 0;
        vkDestroyFramebuffer(vk.device, vk.output_framebuffer, NULL);
        vkDestroyPipeline(vk.device, vk.output_pipeline, NULL);
        vkDestroyRenderPass(vk.device, vk.output_pass, NULL);
        vk.output_framebuffer = VK_NULL_HANDLE;
        vk.output_pipeline = VK_NULL_HANDLE;
        vk.output_pass = VK_NULL_HANDLE;
    }
    attachment.format = format;
    attachment.samples = VK_SAMPLE_COUNT_1_BIT;
    attachment.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;   /* every pixel is drawn */
    attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachment.initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    attachment.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &color;
    pass_info.attachmentCount = 1;
    pass_info.pAttachments = &attachment;
    pass_info.subpassCount = 1;
    pass_info.pSubpasses = &subpass;
    CHECK(vkCreateRenderPass(vk.device, &pass_info, NULL, &vk.output_pass));
    vk.output_format = format;
    if (make_fullscreen_pipeline(output_frag_spirv, sizeof output_frag_spirv, vk.output_layout, vk.output_pass, 0,
                                 &vk.output_pipeline)) {
        vkDestroyRenderPass(vk.device, vk.output_pass, NULL);
        vk.output_pass = VK_NULL_HANDLE;
        return -1;
    }
    return 0;
}

/* A layout change of one color image, recorded into commands. */
static void output_barrier(VkCommandBuffer commands, VkImage image, VkImageLayout from, VkImageLayout to,
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
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.layerCount = 1;
    vkCmdPipelineBarrier(commands, from_stage, to_stage, 0, 0, NULL, 0, NULL, 1, &barrier);
}

int ngl_vk_output(uint64_t image, uint64_t view, uint32_t format, uint32_t width, uint32_t height,
                  uint32_t after_stages, uint32_t final_layout, uint32_t ready_stages, uint32_t ready_access,
                  const uint32_t *table)
{
    static uint32_t identity[256];
    VkImage output = HANDLE_FROM_BITS(VkImage, image);
    VkImageView output_view = HANDLE_FROM_BITS(VkImageView, view);
    VkCommandBufferBeginInfo begin = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    VkFramebufferCreateInfo framebuffer_info = { VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
    VkRenderPassBeginInfo pass = { VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
    VkSubmitInfo submit = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    VkViewport viewport = { 0.0f, 0.0f, (float)width, (float)height, 0.0f, 1.0f };
    VkRect2D whole = { { 0, 0 }, { width, height } };
    VkDescriptorSet sets[OUTPUT_SETS];
    VkCommandBuffer commands;
    uint32_t i;

    if (!vk.borrowed || !vk.drawn || !output || !output_view || !width || !height)
        return -1;
    if (make_output_parts() || make_output_pass((VkFormat)format))
        return -1;
    if (!identity[255])
        for (i = 0; i < 256; i++)
            identity[i] = i | i << 8 | i << 16 | 0xFF000000u;
    if (!table)
        table = identity;
    /* the last output is done with its command buffer, its framebuffer and the table's upload buffer */
    if (vk.output_pending) {
        CHECK(vkWaitForFences(vk.device, 1, &vk.output_done, VK_TRUE, WAIT_NS));
        vk.output_pending = 0;
    }
    vkDestroyFramebuffer(vk.device, vk.output_framebuffer, NULL);
    vk.output_framebuffer = VK_NULL_HANDLE;
    framebuffer_info.renderPass = vk.output_pass;
    framebuffer_info.attachmentCount = 1;
    framebuffer_info.pAttachments = &output_view;
    framebuffer_info.width = width;
    framebuffer_info.height = height;
    framebuffer_info.layers = 1;
    CHECK(vkCreateFramebuffer(vk.device, &framebuffer_info, NULL, &vk.output_framebuffer));

    commands = vk.output_commands;
    CHECK(vkResetFences(vk.device, 1, &vk.output_done));
    CHECK(vkResetCommandBuffer(commands, 0));
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    CHECK(vkBeginCommandBuffer(commands, &begin));
    /* the table, when it changed (the game writes its gamma ramp rarely) */
    if (!vk.table_ready || memcmp(vk.table_words, table, sizeof vk.table_words)) {
        VkBufferImageCopy copy = { 0 };

        memcpy(vk.table_words, table, sizeof vk.table_words);
        memcpy(vk.table_upload_data, table, sizeof vk.table_words);
        output_barrier(commands, vk.table_image,
                       vk.table_ready ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
                       VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        copy.imageSubresource.layerCount = 1;
        copy.imageExtent.width = 256;
        copy.imageExtent.height = 1;
        copy.imageExtent.depth = 1;
        vkCmdCopyBufferToImage(commands, vk.table_upload, vk.table_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                               &copy);
        output_barrier(commands, vk.table_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                       VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
                       VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                       VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
        vk.table_ready = 1;
    }
    /* the target as the draw left it (ready to be copied), to be sampled; the owner's image, its old contents dropped,
     * once its last users are done with it */
    output_barrier(commands, vk.target, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                   VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                   VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                   VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    output_barrier(commands, output, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, 0,
                   VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                   after_stages ? (VkPipelineStageFlags)after_stages : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                   VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
    pass.renderPass = vk.output_pass;
    pass.framebuffer = vk.output_framebuffer;
    pass.renderArea = whole;
    vkCmdBeginRenderPass(commands, &pass, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdSetViewport(commands, 0, 1, &viewport);
    vkCmdSetScissor(commands, 0, 1, &whole);
    vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, vk.output_pipeline);
    sets[0] = vk.output_target_set;
    sets[1] = vk.output_table_set;
    vkCmdBindDescriptorSets(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, vk.output_layout, 0, OUTPUT_SETS, sets, 0,
                            NULL);
    vkCmdDraw(commands, 3, 1, 0, 0);
    vkCmdEndRenderPass(commands);
    /* the owner's image to where they read it; the target back to where the draws leave it */
    output_barrier(commands, output, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, (VkImageLayout)final_layout,
                   VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, (VkAccessFlags)ready_access,
                   VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                   ready_stages ? (VkPipelineStageFlags)ready_stages : VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
    output_barrier(commands, vk.target, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   0, 0, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    CHECK(vkEndCommandBuffer(commands));
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &commands;
    CHECK(submit_to_queue(&submit, vk.output_done));
    vk.output_pending = 1;
    return 0;
}

void ngl_vk_shutdown(void)
{
    uint32_t row;

    if (vk.device) {
        wait_until_idle();
        free_swapchain();
        free_picture();
        /* ngl_vk_output's (none of it made unless it was used) */
        vkDestroyFramebuffer(vk.device, vk.output_framebuffer, NULL);
        vkDestroyPipeline(vk.device, vk.output_pipeline, NULL);
        vkDestroyPipelineLayout(vk.device, vk.output_layout, NULL);
        vkDestroyRenderPass(vk.device, vk.output_pass, NULL);
        vkDestroyFence(vk.device, vk.output_done, NULL);
        if (vk.table_upload_memory)
            vkUnmapMemory(vk.device, vk.table_upload_memory);
        vkDestroyBuffer(vk.device, vk.table_upload, NULL);
        vkFreeMemory(vk.device, vk.table_upload_memory, NULL);
        vkDestroyImageView(vk.device, vk.table_view, NULL);
        vkDestroyImage(vk.device, vk.table_image, NULL);
        vkFreeMemory(vk.device, vk.table_memory, NULL);
        for (row = 1; row <= vk.texture_count; row++) {
            vkDestroyImageView(vk.device, vk.textures[row].view, NULL);
            vkDestroyImage(vk.device, vk.textures[row].image, NULL);
            vkFreeMemory(vk.device, vk.textures[row].memory, NULL);
        }
        if (vk.mesh_corner_data)
            vkUnmapMemory(vk.device, vk.mesh_corner_memory);
        if (vk.mesh_index_data)
            vkUnmapMemory(vk.device, vk.mesh_index_memory);
        if (vk.staging_data)        /* side220 */
            vkUnmapMemory(vk.device, vk.staging_memory);
        vkDestroyBuffer(vk.device, vk.staging, NULL);
        vkFreeMemory(vk.device, vk.staging_memory, NULL);
        if (vk.texture_staging_data)    /* [rexglue-native] 2026-11-09: batched textures */
            vkUnmapMemory(vk.device, vk.texture_staging_memory);
        vkDestroyBuffer(vk.device, vk.texture_staging, NULL);
        vkFreeMemory(vk.device, vk.texture_staging_memory, NULL);
        vkDestroyBuffer(vk.device, vk.mesh_corners, NULL);
        vkDestroyBuffer(vk.device, vk.mesh_indices, NULL);
        vkFreeMemory(vk.device, vk.mesh_corner_memory, NULL);
        vkFreeMemory(vk.device, vk.mesh_index_memory, NULL);
        vkDestroyPipeline(vk.device, vk.mesh_pipeline, NULL);
        vkDestroyPipeline(vk.device, vk.mesh_blend_pipeline, NULL);
        vkDestroyPipeline(vk.device, vk.mesh_add_pipeline, NULL);
        vkDestroyPipeline(vk.device, vk.mesh_subtract_pipeline, NULL);
        vkDestroyPipeline(vk.device, vk.mesh_multiply_pipeline, NULL);  /* [waw-decomp] */
        vkDestroyPipeline(vk.device, vk.mesh_shadow_pipeline, NULL);
        vkDestroyPipelineLayout(vk.device, vk.mesh_layout, NULL);
        if (vk.shadow_uniform_memory)
            vkUnmapMemory(vk.device, vk.shadow_uniform_memory);
        vkDestroyBuffer(vk.device, vk.shadow_uniform, NULL);
        vkFreeMemory(vk.device, vk.shadow_uniform_memory, NULL);
        if (vk.draw_light_memory)
            vkUnmapMemory(vk.device, vk.draw_light_memory);
        vkDestroyBuffer(vk.device, vk.draw_lights, NULL);
        vkFreeMemory(vk.device, vk.draw_light_memory, NULL);
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
        vkDestroyImageView(vk.device, vk.empty_cube_view, NULL);   /* [rexglue-native] rn34 (its set goes with the pool) */
        vkDestroyImage(vk.device, vk.empty_cube, NULL);
        vkFreeMemory(vk.device, vk.empty_cube_memory, NULL);
        vkDestroyDescriptorPool(vk.device, vk.descriptor_pool, NULL);
        vkDestroyDescriptorSetLayout(vk.device, vk.set_layout, NULL);
        vkDestroySampler(vk.device, vk.sampler, NULL);
        if (vk.readback_memory)
            vkUnmapMemory(vk.device, vk.readback_memory);
        vkDestroyBuffer(vk.device, vk.readback, NULL);
        vkFreeMemory(vk.device, vk.readback_memory, NULL);
        vkDestroyPipeline(vk.device, vk.tint_pipeline, NULL);
        vkDestroyPipelineLayout(vk.device, vk.tint_layout, NULL);
        vkDestroyImageView(vk.device, vk.post_copy_view, NULL);
        vkDestroyImage(vk.device, vk.post_copy, NULL);
        vkFreeMemory(vk.device, vk.post_copy_memory, NULL);
        vkDestroyImageView(vk.device, vk.post_history_view, NULL);
        vkDestroyImage(vk.device, vk.post_history, NULL);
        vkFreeMemory(vk.device, vk.post_history_memory, NULL);
        vkDestroyPipeline(vk.device, vk.dof_blur_pipeline, NULL);
        vkDestroyPipeline(vk.device, vk.dof_mix_pipeline, NULL);
        vkDestroyPipelineLayout(vk.device, vk.dof_blur_layout, NULL);
        vkDestroyPipelineLayout(vk.device, vk.dof_mix_layout, NULL);
        vkDestroyFramebuffer(vk.device, vk.dof_quarter_framebuffer, NULL);
        vkDestroyFramebuffer(vk.device, vk.dof_temp_framebuffer, NULL);
        vkDestroyRenderPass(vk.device, vk.dof_blur_pass, NULL);
        vkDestroyRenderPass(vk.device, vk.overlay_pass, NULL);
        vkDestroyImageView(vk.device, vk.dof_half_view, NULL);
        vkDestroyImageView(vk.device, vk.dof_quarter_view, NULL);
        vkDestroyImageView(vk.device, vk.dof_temp_view, NULL);
        vkDestroyImage(vk.device, vk.dof_half, NULL);
        vkDestroyImage(vk.device, vk.dof_quarter, NULL);
        vkDestroyImage(vk.device, vk.dof_temp, NULL);
        vkFreeMemory(vk.device, vk.dof_half_memory, NULL);
        vkFreeMemory(vk.device, vk.dof_quarter_memory, NULL);
        vkFreeMemory(vk.device, vk.dof_temp_memory, NULL);
        vkDestroySampler(vk.device, vk.soft_sampler, NULL);
        vkDestroyImageView(vk.device, vk.soft_depth_view, NULL);
        vkDestroyImage(vk.device, vk.soft_depth, NULL);
        vkFreeMemory(vk.device, vk.soft_depth_memory, NULL);
        vkDestroyRenderPass(vk.device, vk.opaque_pass, NULL);
        vkDestroyRenderPass(vk.device, vk.blend_pass, NULL);
        vkDestroyFramebuffer(vk.device, vk.framebuffer, NULL);
        vkDestroyRenderPass(vk.device, vk.render_pass, NULL);
        vkDestroyImageView(vk.device, vk.target_view, NULL);
        vkDestroyImage(vk.device, vk.target, NULL);
        vkFreeMemory(vk.device, vk.target_memory, NULL);
        vkDestroyImageView(vk.device, vk.depth_view, NULL);
        vkDestroyImage(vk.device, vk.depth, NULL);
        vkFreeMemory(vk.device, vk.depth_memory, NULL);
        vkDestroyFence(vk.device, vk.done, NULL);
        vkDestroyCommandPool(vk.device, vk.command_pool, NULL);   /* and the command buffers made from it */
        if (!vk.borrowed)
            vkDestroyDevice(vk.device, NULL);
    }
    /* [rexglue-native] 2026-11-09: (not found when the instance's functions couldn't be: the instance is then left) */
    if (vk.instance && vk.surface && vkDestroySurfaceKHR)
        vkDestroySurfaceKHR(vk.instance, vk.surface, NULL);
    if (vk.instance && !vk.borrowed && vkDestroyInstance)
        vkDestroyInstance(vk.instance, NULL);
    free(vk.draws);
    memset(&vk, 0, sizeof vk);
    device_name[0] = '\0';
}
