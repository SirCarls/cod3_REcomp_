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
static const uint32_t effect_depth_frag_spirv[] =  /* [rexglue-native] 2026-11-10: the effect layer (make_effect_layer) */
#include "effect_depth.frag.inc"
;
static const uint32_t effect_back_frag_spirv[] =   /* [rexglue-native] */
#include "effect_back.frag.inc"
;
static const uint32_t glow_first_frag_spirv[] =    /* [rexglue-native] 2026-11-10: the sky glow (make_glow) */
#include "glow_first.frag.inc"
;
static const uint32_t glow_blur_frag_spirv[] =     /* [rexglue-native] */
#include "glow_blur.frag.inc"
;
static const uint32_t glow_add_frag_spirv[] =      /* [rexglue-native] */
#include "glow_add.frag.inc"
;
static const uint32_t bloom_first_frag_spirv[] =   /* [cod2-native] 2026-11-24: the IW bloom (make_bloom) */
#include "bloom_first.frag.inc"
;
static const uint32_t bloom_blur_frag_spirv[] =    /* [cod2-native] */
#include "bloom_blur.frag.inc"
;
static const uint32_t sky_bleed_frag_spirv[] =     /* [cod2-native] 2026-11-24 g4: the IW sky bleed */
#include "sky_bleed.frag.inc"
;
static const uint32_t heat_warp_vert_spirv[] =     /* [rexglue-native-fx] 2026-11-11 fx1: the screen warp (make_heat) */
#include "heat_warp.vert.inc"
;
static const uint32_t heat_warp_frag_spirv[] =     /* [rexglue-native-fx] 2026-11-11 fx1 */
#include "heat_warp.frag.inc"
;
static const uint32_t output_frag_spirv[] =
#include "output.frag.inc"
;
static const uint32_t window_cas_frag_spirv[] =    /* [acct2-fh] 2026-11-14: the window's CAS and FSR 1 (window_fx) */
#include "window_cas.frag.inc"
;
static const uint32_t window_easu_frag_spirv[] =   /* [acct2-fh] */
#include "window_easu.frag.inc"
;
static const uint32_t window_rcas_frag_spirv[] =   /* [acct2-fh] */
#include "window_rcas.frag.inc"
;
static const uint32_t msaa_depth_frag_spirv[] =    /* [acct2-bro] 2026-11-22 rn113: MSAA's depth pass */
#include "msaa_depth.frag.inc"
;
static const uint32_t mesh_msaa_frag_spirv[] =     /* [acct2-bro] 2026-11-22 rn113b: the twins' mesh.frag */
#include "mesh_msaa.frag.inc"
;
static const uint32_t camo_vert_spirv[] =          /* [acct2-bro] 2026-11-22 Camo lane: the camo pass (make_camo) */
#include "camo.vert.inc"
;
static const uint32_t camo_frag_spirv[] =          /* [acct2-bro] 2026-11-22 Camo lane */
#include "camo.frag.inc"
;

#define TARGET_FORMAT   VK_FORMAT_R8G8B8A8_UNORM
#define DEPTH_FORMAT    VK_FORMAT_D32_SFLOAT
/* [acct2-bro] 2026-11-22 rn113: MSAA (ngl_vk_set_msaa): the pipelines that get multisampled twins (msaa_slot), and
 * the most samples the multisampled target may hold (a 3840x2160 picture with four) */
enum { MSAA_MESH, MSAA_CULL_CW, MSAA_CULL_CCW, MSAA_TWINS };
#define MSAA_MOST_SAMPLES (3840ull * 2160ull * 4ull)
#define PICTURE_FORMAT  VK_FORMAT_B8G8R8A8_UNORM     /* X8R8G8B8 in memory: blue, green, red, unused */
#define BYTES_PER_PIXEL 4u
#define WAIT_NS         5000000000ull               /* 5 s: a draw that takes longer has hung */
#define MAX_SWAP_IMAGES 8u                          /* a window's swapchain images */
#define MAX_VERTICES    (8192u * 6u)                /* 8192 quads a draw */
#define MAX_TEXTURES    2048u                       /* textures made with ngl_vk_texture */
#define DOF_SETS        2u                          /* the depth of field's two blurred pictures' sets */
#define POST_SETS       3u                          /* the tint's copy of the target, the last frame's (motion blur);
                                                     * [rexglue-native] 2026-11-11 rn47g: and the afterimage's kept
                                                     * picture (was 2) */
#define GLOW_SIZE       256u                        /* [rexglue-native] the sky glow's two targets, each way (make_glow) */
#define GLOW_MOST_BLURS 8u                          /* [rexglue-native] and the most blurs it draws (the game's levels: 4) */
#define BLOOM_MOST_BLURS 4u                         /* [cod2-native] 2026-11-24: the IW bloom's blurs (the game: 2 a set) */
#define OUTPUT_SETS     2u                          /* ngl_vk_output's: the target, the color table */
#define EMPTY_CUBE_SETS 1u                          /* [rexglue-native] rn34: the empty cube's (make_empty_cube) */
#define WINDOW_FX_SETS  4u                          /* [acct2-fh] the window's CAS / FSR 1: the target, the color
                                                     * table's image, its own 2 images */
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
    /* [acct2-bro] 2026-11-12: the same sampler with anisotropy, for the textures with smaller levels; none
     * (VK_NULL_HANDLE) unless it was asked for and the device has the feature on (ngl_vk_set_anisotropy) */
    VkSampler aniso_sampler;
    float anisotropy;                   /* its samples at most */
    int anisotropy_feature;             /* the device in use has samplerAnisotropy on */
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
    /* [rexglue-native-render] 2026-11-11 rn48l: the same, adding (NGL_VK_QUAD_ADD), and which of the two is bound in
     * the quads' loops (0 the plain one) */
    VkPipeline quad_add_pipeline;
    int quad_bound_added;
    VkBuffer vertices;
    VkDeviceMemory vertex_memory;
    void *vertex_data;
    /* 3D meshes: every mesh's corners and indices, and this frame's draws */
    VkPipelineLayout mesh_layout;
    int fabric_sets;                    /* [rexglue-native] 1: the layout has the fabric's sets 4 and 5 (mesh.frag) */
    VkPipeline mesh_pipeline;
    /* [acct2-bro] 2026-11-13 rn69a: the same with the faces that wind clockwise on the screen culled (NGL_VK_CULL_CW;
     * the mesh pipeline's front faces), and with the others culled (NGL_VK_CULL_CCW); VK_NULL_HANDLE: not made */
    VkPipeline mesh_cull_pipeline[2];
    VkPipeline mesh_blend_pipeline;     /* the same, blended by alpha, not writing depth */
    VkPipeline mesh_add_pipeline;       /* and added (NGL_VK_BLEND_ADD), taken away (NGL_VK_BLEND_SUBTRACT) */
    VkPipeline mesh_subtract_pipeline;
    /* [acct2-bro] 2026-11-22 defect E: premultiplied (NGL_VK_BLEND_PREMULTIPLIED); VK_NULL_HANDLE: not made */
    VkPipeline mesh_premultiplied_pipeline;
    VkPipeline mesh_multiply_pipeline;  /* [waw-decomp] 2026-11-10: multiplied (NGL_VK_BLEND_MULTIPLY: decals) */
    VkPipeline mesh_decal_pipeline;     /* [rexglue-native stlo 2026-11-16] the scene decal list's (NGL_VK_BLEND_DECAL) */
    VkPipeline mesh_decal_alpha_pipeline;   /* [rexglue-native stlo 2026-11-16] the decal list's blended ones (NGL_VK_BLEND_DECAL_ALPHA) */
    VkPipeline mesh_decal_depth_pipeline;   /* [rexglue-native stlo 2026-11-17] the depth-writing ones (NGL_VK_BLEND_DECAL_DEPTH) */
    float decal_depth_constant;             /* [rexglue-native stlo 2026-11-17] ngl_vk_set_decal_depth_bias */
    float decal_depth_slope;                /* [rexglue-native stlo 2026-11-17] */
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
    /* [acct2-bro] 2026-11-22 rn113: MSAA (ngl_vk_set_msaa; make_msaa_target): the samples in use (0 or 1: none),
     * the multisampled color and depth, the first half's pass and framebuffer, the pass after it (the depth into
     * the one-sample depth, the grass), the pipelines' twins (msaa_slot's order) and their coverage shader */
    uint32_t msaa_samples;
    int msaa_swapped;                   /* the twins are in the one-sample pipelines' places now */
    VkImage msaa_color, msaa_depth;
    VkDeviceMemory msaa_color_memory, msaa_depth_memory;
    VkImageView msaa_color_view, msaa_depth_view;
    VkRenderPass msaa_opaque_pass, msaa_depth_pass;
    VkFramebuffer msaa_framebuffer, msaa_depth_framebuffer;
    VkSampler msaa_sampler;
    VkDescriptorSetLayout msaa_set_layout;
    VkDescriptorPool msaa_pool;
    VkDescriptorSet msaa_set;
    VkPipelineLayout msaa_depth_layout;
    VkPipeline msaa_depth_pipeline;
    VkPipeline msaa_twin[MSAA_TWINS];
    VkShaderModule msaa_mesh_fragment;  /* rn113b: the twins' fragment shader while they are made */
    int msaa_coverage;                  /* rn113b: the twins were made with it */
    /* [acct2-bro] 2026-11-22 rn114: the effect layer's samples (ngl_vk_set_effect_count; make_effect_count): two
     * occlusion queries used in turn, the next one's index, the one this frame's layer counts into and the one
     * the last frame counted into (-1: none), the layer's pixels over the game's screen with it, and the last
     * count (the layer's pixels in the high 32 bits, the samples in the low 32; 0: none yet) */
    VkQueryPool effect_queries;
    VkQueryControlFlags effect_query_flags;
    uint32_t effect_query_next;
    int effect_query_frame, effect_query_last;
    uint64_t effect_screen_frame, effect_screen_last;
    uint64_t effect_count;
    /* [rexglue-native] 2026-11-10: the effect layer (ngl_vk.h effect_layer; make_effect_layer): a color and a depth
     * target of half the main target's width and height. Each frame that has layer draws: the layer's color cleared
     * and its depth filled from the opaque depth's copy (effect_fill_pass, effect_fill_pipeline), the layer's draws
     * with the mesh pipelines (effect_pass: the depth read only, tested and sampled, effect_shadow_set in set 3 with
     * the layer's depth in its binding 2), then the layer over the main target (effect_back_pipeline, in the overlay
     * pass). effect_ready 0: not made, the draws go into the main target. */
    int effect_ready;
    int effect_live;                /* drawing the layer's draws now (draw_meshes part 3) */
    uint32_t effect_width, effect_height;
    VkImage effect_color, effect_depth;
    VkDeviceMemory effect_color_memory, effect_depth_memory;
    VkImageView effect_color_view, effect_depth_view;
    VkRenderPass effect_fill_pass, effect_pass;
    VkFramebuffer effect_framebuffer;
    VkDescriptorPool effect_pool;
    VkDescriptorSet effect_color_set;   /* the layer's color, for the blend back (set 0) */
    VkDescriptorSet effect_shadow_set;  /* the shadow set with the layer's depth in binding 2 */
    VkPipeline effect_fill_pipeline, effect_back_pipeline;
    /* [rexglue-native-fx] 2026-11-11 fx1: the screen warp (ngl_vk_set_screen_warp; Call of Duty 3's heat haze) and its
     * mask. The draws whose blend is NGL_VK_BLEND_HEAT_MASK go into heat_mask (the effect layer's size, its depth, the
     * effect pass: draw_meshes part 4), cleared to 1 and multiplied by (1 - their alpha) each (mesh_heat_pipeline), so
     * it holds 1 - the mask; then, with the layer back over the picture, the target copied (post_copy) and drawn back
     * through the warp's strip (heat_pipeline, in the overlay pass), blended by the mask. heat_ready 0: not made: no
     * warp, the mask draws left out. */
    int heat_ready;                     /* [rexglue-native-fx] 2026-11-11 fx1 */
    int heat_live;                      /* this frame has mask draws (part 4 follows part 3) [rexglue-native-fx] */
    int heat_layout_ok;                 /* heat_mask has been drawn into (its layout known) [rexglue-native-fx] */
    uint32_t warp_count;                /* the next frame's strip's corners (0: none) [rexglue-native-fx] */
    float *warp_corners;                /* as ngl_vk_set_screen_warp took them, 4 floats each [rexglue-native-fx] */
    VkImage heat_mask;                  /* [rexglue-native-fx] 2026-11-11 fx1 */
    VkDeviceMemory heat_mask_memory;    /* [rexglue-native-fx] 2026-11-11 fx1 */
    VkImageView heat_mask_view;         /* [rexglue-native-fx] 2026-11-11 fx1 */
    VkFramebuffer heat_framebuffer;     /* heat_mask and the layer's depth, for effect_pass [rexglue-native-fx] */
    VkDescriptorPool heat_pool;         /* [rexglue-native-fx] 2026-11-11 fx1 */
    VkDescriptorSet heat_mask_set;      /* [rexglue-native-fx] 2026-11-11 fx1 */
    VkPipelineLayout heat_layout;       /* [rexglue-native-fx] 2026-11-11 fx1: the picture's copy, the mask */
    VkPipeline heat_pipeline;           /* [rexglue-native-fx] 2026-11-11 fx1: the strip over the picture */
    VkPipeline mesh_heat_pipeline;      /* the mask draws (made with the mesh pipelines) [rexglue-native-fx] */
    /* [acct2-bro] 2026-11-22 Camo lane: the camo pass (ngl_vk.h camo_texture; make_camo, draw_camo): by the draw's
     * culling (none, NGL_VK_CULL_CW, NGL_VK_CULL_CCW), and their MSAA twins for the multisampled first half */
    VkPipeline camo_pipeline[3];
    VkPipeline camo_msaa_pipeline[3];
    uint32_t camo_count;                /* [acct2-bro] 2026-11-22 Camo lane: camo passes drawn (ngl_vk_camo_count) */
    VkBuffer warp_buffer;               /* [rexglue-native-fx] 2026-11-11 fx1: the strip as drawn (host memory) */
    VkDeviceMemory warp_memory;         /* [rexglue-native-fx] 2026-11-11 fx1 */
    float *warp_data;                   /* [rexglue-native-fx] 2026-11-11 fx1: warp_buffer, mapped */
    /* [rexglue-native] 2026-11-10: the sky glow (ngl_vk_set_glow; Call of Duty 3's, drawn each frame after its tint).
     * Two targets of GLOW_SIZE x GLOW_SIZE (the game's are 256 x 256 whatever the picture's size). In a frame with a
     * glow, after the tint's draw: the target copied (post_copy), its sky alone drawn small into glow 0
     * (glow_first_pipeline: the opaque depth says where nothing is drawn), glow_passes blurs to and fro between the
     * two (glow_blur_pipeline, in the dof's blur pass), then the last one added over the game's screen in the
     * overlay pass (glow_add_pipeline), before the motion blur, post effect 7 and the quads. glow_ready 0: not made,
     * no glow. */
    int glow_ready;
    int glow_on;                        /* this frame has a glow (ngl_vk_set_glow) */
    float glow_tap, glow_gain, glow_strength;   /* ngl_vk_set_glow's, the tap in the small target's pixels */
    float glow_lean, glow_toward[2];
    uint32_t glow_passes;
    VkPipelineLayout glow_layout;       /* a sampled picture, the shadow set, eight floats */
    VkImage glow_image[2];
    VkDeviceMemory glow_memory[2];
    VkImageView glow_view[2];
    VkFramebuffer glow_framebuffer[2];
    VkDescriptorPool glow_pool;
    VkDescriptorSet glow_set[2];
    VkSampler glow_sampler;             /* filtered; black past the edges, as the game fetches its glow targets */
    VkPipeline glow_first_pipeline, glow_blur_pipeline, glow_add_pipeline;
    /* [cod2-native] 2026-11-24: the IW engine's bloom (ngl_vk_set_bloom): the sky glow's targets, sets and adding
     * with a first pass and a blur of its own (bloom_first.frag, bloom_blur.frag), and a layout of its own for their
     * 32 floats. bloom_on: this frame's glow is the bloom (glow_on is set with it). bloom_ready 0: none. */
    int bloom_ready;
    int bloom_on;
    int bloom_tried;    /* [cod2-native] g5: make_bloom asked once (on the first ngl_vk_set_bloom that wants it) */
    float bloom_setup[4];
    float bloom_taps[BLOOM_MOST_BLURS][32];
    uint32_t bloom_blurs;
    VkPipelineLayout bloom_layout;
    VkPipeline bloom_first_pipeline, bloom_blur_pipeline;
    /* [cod2-native] 2026-11-24 g4: the IW sky bleed (glow_apply_sky_bleed): the sky's share and color blurred by the
     * bloom's own passes, the picture drawn toward that color by saturate(2 x share) x bloom_sky_bleed */
    float bloom_sky_bleed;
    VkPipeline bloom_bleed_pipeline;
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
    /* its near depth, far depth, keep depth and blanket; [acct2-bro] 2026-11-13 rn64a: then the bands' scale (0: a
     * ramp between the two depths), the front depth and its amount, and one not used */
    float dof_focus[8];
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
    /* [rexglue-native] 2026-11-11 rn47g: post effect 4 as the game draws it (ngl_vk_set_afterimage): the picture it
     * keeps from frame to frame, sampled through after_set (in the tint's set 2); after_frame: the frame that last
     * wrote it (0: never) */
    VkImage after_image;
    VkDeviceMemory after_memory;
    VkImageView after_view;
    VkDescriptorSet after_set;
    int after_on;                       /* this frame has it */
    float after_share, after_inset;     /* its share of the kept picture; how far in the kept picture is read */
    uint64_t after_frame;
    /* [rexglue-native] 2026-11-11 rn48b: the game keeps that picture at a quarter of the screen's width and height (320 x 180 for
     * 1280 x 720), so its trails are soft; with after_small_on the picture is shrunk into after_small_image (a
     * quarter of the target's size, made with the rest) and stretched back into after_image, both filtered, in place
     * of the copy (ngl_vk_set_afterimage_small). [rexglue-native] 2026-11-11 rn48c: tint.frag reads the kept picture pixel by pixel in the
     * second pass, so a quarter-size one read directly filled only a corner (lvl bf); after_small_set is unused */
    VkImage after_small_image;
    VkDeviceMemory after_small_memory;
    VkImageView after_small_view;
    VkDescriptorSet after_small_set;
    uint32_t after_small_width, after_small_height;
    int after_small_on;
    uint32_t under_count;               /* [rexglue-native] 2026-11-11 rn47i: ngl_vk_set_quads_under_effects' count
                                         * this frame (0: none) */
    int keep_frame;                     /* [rexglue-native] 2026-11-11 rn47n: this frame's finished picture is
                                         * kept in post_history for the next (ngl_vk_keep_last_frame) */
    float tint[32];                     /* the weights times the exposure, the amount, the color, the keep depth; then
                                         * the brightness and the power (ngl_vk_set_brightness; 1, 1 for none), the
                                         * last frame's share (ngl_vk_set_motion_blur; 0 for none), the color look-up
                                         * (1 on); [rexglue-native] 2026-11-10 rn41b: then post effect 7's twelve
                                         * (ngl_vk_set_film; all 0 for none), as tint.frag's film, film_gray and
                                         * film_move; rn47g: then tint.frag's after (24 to 27; was 24 floats), 0 in
                                         * every draw but the afterimage's first; [waw-decomp] 2026-11-11: then
                                         * tint.frag's tweak (28 to 31; was 28 floats, now 128 bytes, the push
                                         * constants Vulkan always allows): the film tweak's delta and 1 for on
                                         * (ngl_vk_set_film_tweak, which puts its bias and base where the weights and
                                         * the color are), 0 in every draw but the tint's own */
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
    /* [rexglue-native thread2] 2026-11-24: a frame's commands are being recorded or submitted now (draw, or
     * ngl_vk_end_frame's submit with its overlay): ngl_vk_set_target_size must not run */
    int recording;
    int low_showing;                /* it has a window image (acquired at the begin) */
    uint32_t low_window_image;
    VkPhysicalDeviceFeatures enabled_features;  /* the device's, as its owner says (ngl_vk_set_device_features) */
    int block_textures;     /* [acct2-bro] 2026-11-23 c3camo v2: BC7 textures can be made (ngl_vk_texture_blocks) */
    /* [rexglue-native thread2] 2026-11-24: depth probes (ngl_vk_probe_depth; made at the first points): the next
     * draw's points and the frame they are for, the host buffer the texels go into (two slots of
     * NGL_VK_DEPTH_PROBES floats, taken in turn), each slot's frame, points, state and which points were on the
     * target, and the newest finished frame's depths (ngl_vk_probe_results) */
    float probe_points[NGL_VK_DEPTH_PROBES][2];
    uint32_t probe_count;
    uint64_t probe_frame;
    VkBuffer probe_buffer;
    VkDeviceMemory probe_memory;
    float *probe_mapped;
    uint32_t probe_next_slot;
    uint64_t probe_slot_frame[2];
    uint32_t probe_slot_count[2];
    int probe_slot_state[2];        /* PROBE_FREE, PROBE_RECORDED (in the commands being made), PROBE_SUBMITTED */
    uint8_t probe_slot_on_target[2][NGL_VK_DEPTH_PROBES];
    float probe_depths[NGL_VK_DEPTH_PROBES];
    uint32_t probe_depth_count;
    uint64_t probe_depth_frame;
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
/* [acct2-bro] 2026-11-12: what ngl_vk_set_anisotropy asked for (settings: they stay across ngl_vk_shutdown) */
static float anisotropy_asked = 1.0f;
static int anisotropy_on_borrowed_device;
/* [acct2-bro] 2026-11-12 rn61a: ngl_vk_set_shown_scale: the target is this many times the shown picture each way */
static uint32_t shown_scale = 1u;
/* [acct2-bro] 2026-11-22 rn113: ngl_vk_set_msaa: the samples asked for the 3D scene (1: none) */
static uint32_t msaa_wanted = 1u;
/* [acct2-bro] 2026-11-22 rn114: ngl_vk_set_effect_count: the effect layer's samples counted, and whether a
 * borrowed device has occlusionQueryPrecise on (as its owner made it) */
static int effect_count_wanted, effect_precise_on_borrowed_device;
/* [acct2-bro] 2026-11-23 c3camo v2: ngl_vk_set_block_textures: asked for, and whether a borrowed device has
 * textureCompressionBC on (as its owner made it) */
static int block_textures_asked, block_textures_on_borrowed_device;
/* [rexglue-native thread2] 2026-11-24: ngl_vk_set_target_size has been called (once is enough; it stays across
 * ngl_vk_shutdown, as the window's settings do). The port then asks for its picture at the size it is shown, so the
 * window copy is one to one when the target fits (record_window_copy); a port that never calls it keeps the copy
 * scaled to fit, exactly as before. */
static int target_size_asked;

/* [acct2-bro] 2026-11-23 c3camo v2: the GPU samples format with linear filtering in optimal tiling (and, on Vulkan 1.1
 * and later, can copy into it), as ngl_vk_texture_blocks' images need */
static int block_format_sampled(VkFormat format)
{
    VkFormatProperties properties;
    VkPhysicalDeviceProperties device;
    VkFormatFeatureFlags wanted = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;

    vkGetPhysicalDeviceProperties(vk.gpu, &device);
    if (device.apiVersion >= VK_MAKE_VERSION(1, 1, 0))
        wanted |= VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
    vkGetPhysicalDeviceFormatProperties(vk.gpu, format, &properties);
    return (properties.optimalTilingFeatures & wanted) == wanted;
}

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

/* [rexglue-native thread2] 2026-11-24: one combined image sampler of a set pointed at a view (nothing when the set
 * or the view isn't made). The pointing of each set at an image sized from the target goes through here, so init and
 * ngl_vk_set_target_size point them the same way. */
static void point_image(VkDescriptorSet set, uint32_t binding, VkSampler sampler, VkImageView view,
                        VkImageLayout layout)
{
    VkDescriptorImageInfo image_info = { 0 };
    VkWriteDescriptorSet write = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };

    if (!set || !view)
        return;
    image_info.sampler = sampler;
    image_info.imageView = view;
    image_info.imageLayout = layout;
    write.dstSet = set;
    write.dstBinding = binding;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.pImageInfo = &image_info;
    vkUpdateDescriptorSets(vk.device, 1, &write, 0, NULL);
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
static void probe_publish(void);    /* [rexglue-native thread2] 2026-11-24: depth probes (ngl_vk_probe_depth) */

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
    probe_publish();    /* [rexglue-native thread2] 2026-11-24: that frame's depth probes are in */
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
    VkPhysicalDeviceFeatures gpu_features, asked_features;     /* [acct2-bro] 2026-11-12: anisotropy */

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
    /* [acct2-bro] 2026-11-12: with anisotropy asked for (ngl_vk_set_anisotropy), the device's samplerAnisotropy
     * feature on when the GPU has it; no other feature, as before */
    if (anisotropy_asked > 1.0f) {
        vkGetPhysicalDeviceFeatures(vk.gpu, &gpu_features);
        if (gpu_features.samplerAnisotropy) {
            memset(&asked_features, 0, sizeof asked_features);
            asked_features.samplerAnisotropy = VK_TRUE;
            info.pEnabledFeatures = &asked_features;
            vk.anisotropy_feature = 1;
        }
    }
    /* [acct2-bro] 2026-11-22 rn114: with the effect count asked for, exact counts (occlusionQueryPrecise) when
     * the GPU has them; else the count may only say "some" */
    if (effect_count_wanted) {
        vkGetPhysicalDeviceFeatures(vk.gpu, &gpu_features);
        if (gpu_features.occlusionQueryPrecise) {
            if (!info.pEnabledFeatures)
                memset(&asked_features, 0, sizeof asked_features);
            asked_features.occlusionQueryPrecise = VK_TRUE;
            info.pEnabledFeatures = &asked_features;
            vk.effect_query_flags = VK_QUERY_CONTROL_PRECISE_BIT;
        }
    }
    /* [acct2-bro] 2026-11-23 c3camo v2: with block textures asked for (ngl_vk_set_block_textures), the device's
     * textureCompressionBC feature on when the GPU has it and samples BC7; never asked: nothing here, the device as
     * before */
    if (block_textures_asked) {
        vkGetPhysicalDeviceFeatures(vk.gpu, &gpu_features);
        if (gpu_features.textureCompressionBC && block_format_sampled(VK_FORMAT_BC7_UNORM_BLOCK)) {
            if (!info.pEnabledFeatures)
                memset(&asked_features, 0, sizeof asked_features);
            asked_features.textureCompressionBC = VK_TRUE;
            info.pEnabledFeatures = &asked_features;
            vk.block_textures = 1;
        }
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
    /* [acct2-fh] 2026-11-14 present_mode 2: each frame shown the moment it is done (IMMEDIATE, it may tear), else
     * MAILBOX; 0 and 1 as before (Big Red One and Call of Duty 3 never ask for 2) */
    for (i = 0; window.present_mode == 2 && i < mode_count; i++)
        if (modes[i] == VK_PRESENT_MODE_IMMEDIATE_KHR)
            info.presentMode = VK_PRESENT_MODE_IMMEDIATE_KHR;
    for (i = 0; window.present_mode == 2 && info.presentMode == VK_PRESENT_MODE_FIFO_KHR && i < mode_count; i++)
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

/* [acct2-fh] 2026-11-14 (ngl_vk.h): a new present mode, taken when the swapchain is made again at the next frame */
void ngl_vk_set_present_mode(int present_mode)
{
    if (present_mode < 0 || present_mode > 2)
        present_mode = 0;
    if (present_mode != window.present_mode)
        vk.swap_stale = 1;
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

/* ---------------------------------------------------------------------------------------------------------------
 * [acct2-bro] 2026-11-22 rn113: MSAA for the 3D scene's opaque half (ngl_vk_set_msaa; Call of Duty 3's owner, 11-22
 * 08:38: "The gun models need some anti-aliasing", 08:39: "The in-game msaa needs to be improved", 08:43-08:44: the
 * tree leaves, "density, lighting"). The first half's meshes (the world, the guns, the characters, every cut-out:
 * leaves, fences, wires) are drawn into multisampled color and depth (msaa_opaque_pass) and the color resolved into
 * the target. Then one pass over the target with one sample (msaa_resolve_depth): the multisampled depth goes into
 * the one-sample depth (msaa_depth.frag, each pixel's first sample) and the grass is drawn as before, with its
 * one-sample pipeline. From there the frame is the one-sample frame: copy_soft_depth, the second half (blended
 * draws, decals, effects), the effect layer, the post effects, the quads and the copies out are unchanged.
 * rn113c: the grass stays out of the samples: its multisampled twin hung the GPU (VK_ERROR_DEVICE_LOST on an RTX
 * 3080 at the first frame with grass, x243 to x246; the validation layer saw no error), and drawn with one sample
 * after the resolve it is drawn exactly as without MSAA. The opaque mesh pipelines get multisampled twins, made
 * right after each with the same state (msaa_twin), swapped into their places for the first half only
 * (msaa_swap_pipelines); with the fabric's sets the twins use mesh.frag's coverage build (rn113b: a cut-out's edge
 * takes its share of the pixel's samples). Nothing of this is made unless MSAA is asked for: with 1 (the default)
 * every pass, pipeline and frame is as before. */
static void depth_barrier(VkImage image, VkImageLayout from, VkImageLayout to, VkAccessFlags from_access,
                          VkAccessFlags to_access, VkPipelineStageFlags from_stage, VkPipelineStageFlags to_stage);
static int draw_grass(void);

/* the one-sample pipeline whose twin is vk.msaa_twin[slot]: the first half's (draw_meshes(0)) */
static VkPipeline *msaa_slot(int slot)
{
    switch (slot) {
    case MSAA_CULL_CW: return &vk.mesh_cull_pipeline[0];
    case MSAA_CULL_CCW: return &vk.mesh_cull_pipeline[1];
    default: return &vk.mesh_pipeline;
    }
}

/* The twins into the one-sample pipelines' places and back: the draw code binds whatever those places hold, so the
 * first half draws with the twins and every other pass (the grass, the second half, the effect layer, the shadow)
 * with the one-sample ones. */
static void msaa_swap_pipelines(void)
{
    for (int i = 0; i < MSAA_TWINS; i++) {
        VkPipeline *slot = msaa_slot(i), kept = *slot;

        *slot = vk.msaa_twin[i];
        vk.msaa_twin[i] = kept;
    }
    vk.msaa_swapped = !vk.msaa_swapped;
}

/* A one-sample pipeline's twin: the same create info with the samples and the multisampled pass, made right after it
 * (the info and multisample state as they were for it), with the coverage build of mesh.frag when it was made
 * (rn113b: told MSAA's count); not made when MSAA is off or the pipeline wasn't made. */
static void msaa_twin(VkGraphicsPipelineCreateInfo *info, VkPipelineMultisampleStateCreateInfo *multisample, int slot,
                      VkPipeline made)
{
    VkRenderPass kept_pass = info->renderPass;
    VkSampleCountFlagBits kept_samples = multisample->rasterizationSamples;
    VkGraphicsPipelineCreateInfo twin = *info;
    VkPipelineShaderStageCreateInfo stages[2];
    VkSpecializationMapEntry entry = { 0, 0, sizeof(int32_t) };
    int32_t samples = (int32_t)vk.msaa_samples;
    VkSpecializationInfo special = { 1, &entry, sizeof samples, &samples };

    if (vk.msaa_samples < 2u || made == VK_NULL_HANDLE || slot < 0 || slot >= MSAA_TWINS)
        return;
    multisample->rasterizationSamples = (VkSampleCountFlagBits)vk.msaa_samples;
    info->renderPass = vk.msaa_opaque_pass;
    twin = *info;
    if (vk.msaa_mesh_fragment && info->stageCount == 2u) {
        memcpy(stages, info->pStages, sizeof stages);
        stages[1].module = vk.msaa_mesh_fragment;
        stages[1].pSpecializationInfo = &special;
        twin.pStages = stages;
        vk.msaa_coverage = 1;
    }
    if (vkCreateGraphicsPipelines(vk.device, VK_NULL_HANDLE, 1, &twin, NULL, &vk.msaa_twin[slot]) != VK_SUCCESS)
        vk.msaa_twin[slot] = VK_NULL_HANDLE;
    multisample->rasterizationSamples = kept_samples;
    info->renderPass = kept_pass;
}

/* MSAA's parts let go (each may be missing), the twins back out of the pipelines' places first */
static void msaa_free(void)
{
    if (!vk.device)
        return;
    if (vk.msaa_swapped)
        msaa_swap_pipelines();
    for (int i = 0; i < MSAA_TWINS; i++) {
        vkDestroyPipeline(vk.device, vk.msaa_twin[i], NULL);
        vk.msaa_twin[i] = VK_NULL_HANDLE;
    }
    vkDestroyPipeline(vk.device, vk.msaa_depth_pipeline, NULL);
    vkDestroyPipelineLayout(vk.device, vk.msaa_depth_layout, NULL);
    vkDestroyDescriptorPool(vk.device, vk.msaa_pool, NULL);     /* and its set */
    vkDestroyDescriptorSetLayout(vk.device, vk.msaa_set_layout, NULL);
    vkDestroySampler(vk.device, vk.msaa_sampler, NULL);
    vkDestroyFramebuffer(vk.device, vk.msaa_framebuffer, NULL);
    vkDestroyFramebuffer(vk.device, vk.msaa_depth_framebuffer, NULL);
    vkDestroyRenderPass(vk.device, vk.msaa_opaque_pass, NULL);
    vkDestroyRenderPass(vk.device, vk.msaa_depth_pass, NULL);
    vkDestroyImageView(vk.device, vk.msaa_color_view, NULL);
    vkDestroyImageView(vk.device, vk.msaa_depth_view, NULL);
    vkDestroyImage(vk.device, vk.msaa_color, NULL);
    vkDestroyImage(vk.device, vk.msaa_depth, NULL);
    vkFreeMemory(vk.device, vk.msaa_color_memory, NULL);
    vkFreeMemory(vk.device, vk.msaa_depth_memory, NULL);
    vk.msaa_depth_pipeline = VK_NULL_HANDLE;
    vk.msaa_depth_layout = VK_NULL_HANDLE;
    vk.msaa_pool = VK_NULL_HANDLE;
    vk.msaa_set = VK_NULL_HANDLE;
    vk.msaa_set_layout = VK_NULL_HANDLE;
    vk.msaa_sampler = VK_NULL_HANDLE;
    vk.msaa_framebuffer = VK_NULL_HANDLE;
    vk.msaa_depth_framebuffer = VK_NULL_HANDLE;
    vk.msaa_opaque_pass = VK_NULL_HANDLE;
    vk.msaa_depth_pass = VK_NULL_HANDLE;
    vk.msaa_color_view = VK_NULL_HANDLE;
    vk.msaa_depth_view = VK_NULL_HANDLE;
    vk.msaa_color = VK_NULL_HANDLE;
    vk.msaa_depth = VK_NULL_HANDLE;
    vk.msaa_color_memory = VK_NULL_HANDLE;
    vk.msaa_depth_memory = VK_NULL_HANDLE;
    vk.msaa_coverage = 0;
    vk.msaa_samples = 1u;
}

/* A multisampled image of the target's size (vk.msaa_samples), in the GPU's own memory, and its view */
static int make_msaa_image(VkFormat format, VkImageUsageFlags usage, VkImage *image, VkDeviceMemory *memory,
                           VkImageView *view)
{
    VkImageCreateInfo info = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    VkImageViewCreateInfo view_info = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    VkMemoryAllocateInfo allocation = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    VkMemoryRequirements needs;

    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = format;
    info.extent.width = vk.width;
    info.extent.height = vk.height;
    info.extent.depth = 1;
    info.mipLevels = 1;
    info.arrayLayers = 1;
    info.samples = (VkSampleCountFlagBits)vk.msaa_samples;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = usage;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    CHECK(vkCreateImage(vk.device, &info, NULL, image));
    vkGetImageMemoryRequirements(vk.device, *image, &needs);
    allocation.allocationSize = needs.size;
    if (find_memory(needs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &allocation.memoryTypeIndex)) {
        fprintf(stderr, "[ngl_vk] rn113: no GPU memory for a %ux%u image of %u samples\n", vk.width, vk.height,
                vk.msaa_samples);
        return -1;
    }
    CHECK(vkAllocateMemory(vk.device, &allocation, NULL, memory));
    CHECK(vkBindImageMemory(vk.device, *image, *memory, 0));
    view_info.image = *image;
    view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view_info.format = format;
    view_info.subresourceRange.aspectMask = format == DEPTH_FORMAT ? VK_IMAGE_ASPECT_DEPTH_BIT
                                                                   : VK_IMAGE_ASPECT_COLOR_BIT;
    view_info.subresourceRange.levelCount = 1;
    view_info.subresourceRange.layerCount = 1;
    CHECK(vkCreateImageView(vk.device, &view_info, NULL, view));
    return 0;
}

/* The first half, multisampled: color (0) and depth (1) of vk.msaa_samples, both cleared, the target (2) the color's
 * resolve. The color isn't kept (the resolve is what is needed); the depth is, for msaa_resolve_depth. The target is
 * left as the one-sample first half leaves it (a color attachment, for the pass that follows). */
static int make_msaa_opaque_pass(void)
{
    VkAttachmentDescription attachments[3] = { { 0 }, { 0 }, { 0 } };
    VkAttachmentReference color = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
    VkAttachmentReference depth = { 1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL };
    VkAttachmentReference resolve = { 2, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
    VkSubpassDescription subpass = { 0 };
    VkSubpassDependency dependencies[2];
    VkRenderPassCreateInfo pass_info = { VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };

    attachments[0].format = TARGET_FORMAT;
    attachments[0].samples = (VkSampleCountFlagBits)vk.msaa_samples;
    attachments[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachments[0].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachments[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[0].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    attachments[0].finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    attachments[1].format = DEPTH_FORMAT;
    attachments[1].samples = (VkSampleCountFlagBits)vk.msaa_samples;
    attachments[1].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachments[1].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachments[1].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachments[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[1].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    attachments[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    attachments[2].format = TARGET_FORMAT;
    attachments[2].samples = VK_SAMPLE_COUNT_1_BIT;
    attachments[2].loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachments[2].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachments[2].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachments[2].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[2].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    attachments[2].finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &color;
    subpass.pResolveAttachments = &resolve;
    subpass.pDepthStencilAttachment = &depth;
    target_pass_dependencies(dependencies);     /* the one-sample main pass's own set */
    pass_info.attachmentCount = 3;
    pass_info.pAttachments = attachments;
    pass_info.subpassCount = 1;
    pass_info.pSubpasses = &subpass;
    pass_info.dependencyCount = 2;
    pass_info.pDependencies = dependencies;
    CHECK(vkCreateRenderPass(vk.device, &pass_info, NULL, &vk.msaa_opaque_pass));
    return 0;
}

/* The pass after the resolve, with one sample over the target and the one-sample depth (compatible with the main
 * pass, so the grass's one-sample pipeline draws in it): the target loaded and kept, the depth written whole by the
 * depth's pipeline (quad.vert's one triangle, msaa_depth.frag reading the samples, no color written) and left in the
 * transfer layout for copy_soft_depth, as the one-sample first half leaves it. */
static int make_msaa_depth_parts(void)
{
    VkAttachmentDescription attachments[2] = { { 0 }, { 0 } };
    VkAttachmentReference color = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
    VkAttachmentReference depth = { 1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL };
    VkSubpassDescription subpass = { 0 };
    VkSubpassDependency dependencies[2];
    VkRenderPassCreateInfo pass_info = { VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
    VkSamplerCreateInfo sampler_info = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    VkDescriptorSetLayoutBinding binding = { 0 };
    VkDescriptorSetLayoutCreateInfo set_layout_info = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    VkDescriptorPoolSize pool_size = { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1 };
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
    VkPipelineColorBlendAttachmentState blend_attachment = { 0 };   /* no color written (colorWriteMask 0) */
    VkPipelineColorBlendStateCreateInfo blend = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
    VkPipelineDepthStencilStateCreateInfo depth_state = {
        VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
    VkDynamicState dynamic_states[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dynamic = { VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
    VkGraphicsPipelineCreateInfo info = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
    VkResult result;

    attachments[0].format = TARGET_FORMAT;
    attachments[0].samples = VK_SAMPLE_COUNT_1_BIT;
    attachments[0].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    attachments[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachments[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachments[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[0].initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    attachments[0].finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    attachments[1].format = DEPTH_FORMAT;
    attachments[1].samples = VK_SAMPLE_COUNT_1_BIT;
    attachments[1].loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachments[1].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachments[1].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachments[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[1].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    attachments[1].finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &color;
    subpass.pDepthStencilAttachment = &depth;
    target_pass_dependencies(dependencies);     /* the main pass's own set: after the resolve, before the copy */
    pass_info.attachmentCount = 2;
    pass_info.pAttachments = attachments;
    pass_info.subpassCount = 1;
    pass_info.pSubpasses = &subpass;
    pass_info.dependencyCount = 2;
    pass_info.pDependencies = dependencies;
    CHECK(vkCreateRenderPass(vk.device, &pass_info, NULL, &vk.msaa_depth_pass));
    /* (its framebuffer is sized from the target: make_msaa_sized) */

    /* the samples read texel by texel (texelFetch: the sampler's filters aren't used) */
    sampler_info.magFilter = VK_FILTER_NEAREST;
    sampler_info.minFilter = VK_FILTER_NEAREST;
    sampler_info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sampler_info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler_info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler_info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler_info.maxLod = 0.0f;
    CHECK(vkCreateSampler(vk.device, &sampler_info, NULL, &vk.msaa_sampler));
    binding.binding = 0;
    binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    binding.descriptorCount = 1;
    binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    set_layout_info.bindingCount = 1;
    set_layout_info.pBindings = &binding;
    CHECK(vkCreateDescriptorSetLayout(vk.device, &set_layout_info, NULL, &vk.msaa_set_layout));
    pool_info.maxSets = 1;
    pool_info.poolSizeCount = 1;
    pool_info.pPoolSizes = &pool_size;
    CHECK(vkCreateDescriptorPool(vk.device, &pool_info, NULL, &vk.msaa_pool));
    set_info.descriptorPool = vk.msaa_pool;
    set_info.descriptorSetCount = 1;
    set_info.pSetLayouts = &vk.msaa_set_layout;
    CHECK(vkAllocateDescriptorSets(vk.device, &set_info, &vk.msaa_set));   /* (pointed by msaa_point) */
    layout_info.setLayoutCount = 1;
    layout_info.pSetLayouts = &vk.msaa_set_layout;
    CHECK(vkCreatePipelineLayout(vk.device, &layout_info, NULL, &vk.msaa_depth_layout));

    if (make_shader(quad_vert_spirv, sizeof quad_vert_spirv, &vertex_shader) ||
        make_shader(msaa_depth_frag_spirv, sizeof msaa_depth_frag_spirv, &fragment_shader)) {
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
    raster.frontFace = VK_FRONT_FACE_CLOCKWISE;
    raster.lineWidth = 1.0f;
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    blend.attachmentCount = 1;
    blend.pAttachments = &blend_attachment;
    depth_state.depthTestEnable = VK_TRUE;      /* (a depth is written only with the test on) */
    depth_state.depthWriteEnable = VK_TRUE;
    depth_state.depthCompareOp = VK_COMPARE_OP_ALWAYS;
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
    info.pDepthStencilState = &depth_state;
    info.pDynamicState = &dynamic;
    info.layout = vk.msaa_depth_layout;
    info.renderPass = vk.msaa_depth_pass;
    info.subpass = 0;
    result = vkCreateGraphicsPipelines(vk.device, VK_NULL_HANDLE, 1, &info, NULL, &vk.msaa_depth_pipeline);
    vkDestroyShaderModule(vk.device, vertex_shader, NULL);
    vkDestroyShaderModule(vk.device, fragment_shader, NULL);
    if (result != VK_SUCCESS) {
        vk.msaa_depth_pipeline = VK_NULL_HANDLE;
        fprintf(stderr, "[ngl_vk] rn113: vkCreateGraphicsPipelines (MSAA's depth) failed (%d)\n", (int)result);
        return -1;
    }
    return 0;
}

/* MSAA's images, passes, framebuffers and depth parts, at the end of make_target (the target and depth are made): the
 * samples asked for (msaa_wanted), fewer where the device can't draw or sample that many or the picture is very large
 * (MSAA_MOST_SAMPLES); none of it when 1 is asked or something can't be made (said on stderr: drawn with one sample).
 * The twins are made with the pipelines (msaa_twin) and checked when all are made (msaa_finish). */
/* [rexglue-native thread2] 2026-11-24: MSAA's parts sized from the target (at init and at ngl_vk_set_target_size):
 * the multisampled color and depth, the first half's framebuffer (their resolve into the target) and the depth
 * pass's (the target and the one-sample depth). The passes are made already; vk.target_view and vk.depth_view are
 * the current target's. */
static int make_msaa_sized(void)
{
    VkImageView views[3];
    VkFramebufferCreateInfo framebuffer_info = { VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };

    if (make_msaa_image(TARGET_FORMAT, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, &vk.msaa_color, &vk.msaa_color_memory,
                        &vk.msaa_color_view) ||
        make_msaa_image(DEPTH_FORMAT, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                        &vk.msaa_depth, &vk.msaa_depth_memory, &vk.msaa_depth_view))
        return -1;
    views[0] = vk.msaa_color_view;
    views[1] = vk.msaa_depth_view;
    views[2] = vk.target_view;
    framebuffer_info.renderPass = vk.msaa_opaque_pass;
    framebuffer_info.attachmentCount = 3;
    framebuffer_info.pAttachments = views;
    framebuffer_info.width = vk.width;
    framebuffer_info.height = vk.height;
    framebuffer_info.layers = 1;
    CHECK(vkCreateFramebuffer(vk.device, &framebuffer_info, NULL, &vk.msaa_framebuffer));
    views[0] = vk.target_view;
    views[1] = vk.depth_view;
    framebuffer_info.renderPass = vk.msaa_depth_pass;
    framebuffer_info.attachmentCount = 2;
    CHECK(vkCreateFramebuffer(vk.device, &framebuffer_info, NULL, &vk.msaa_depth_framebuffer));
    return 0;
}

/* [rexglue-native thread2] 2026-11-24: the depth pass's set pointed at the multisampled depth (read texel by texel
 * in its read-only layout), once it and the set are made */
static void msaa_point(void)
{
    point_image(vk.msaa_set, 0, vk.msaa_sampler, vk.msaa_depth_view, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL);
}

static int make_msaa_target(void)
{
    VkPhysicalDeviceProperties properties;
    VkSampleCountFlags can;
    uint32_t samples = msaa_wanted;

    vk.msaa_samples = 1u;
    if (samples < 2u)
        return 0;
    vkGetPhysicalDeviceProperties(vk.gpu, &properties);
    can = properties.limits.framebufferColorSampleCounts & properties.limits.framebufferDepthSampleCounts &
          properties.limits.sampledImageDepthSampleCounts;
    while (samples >= 2u && (!(can & samples) || (uint64_t)vk.width * vk.height * samples > MSAA_MOST_SAMPLES))
        samples /= 2u;
    if (samples < 2u) {
        fprintf(stderr, "[ngl_vk] rn113: MSAA x%u asked for, but the device or the picture's size (%ux%u) allows "
                "none: drawn with one sample\n", msaa_wanted, vk.width, vk.height);
        return -1;
    }
    vk.msaa_samples = samples;
    /* [rexglue-native thread2] 2026-11-24: the passes and the depth's parts first, then what is sized from the
     * target (make_msaa_sized, shared with ngl_vk_set_target_size), then the set pointed at it */
    if (make_msaa_opaque_pass() || make_msaa_depth_parts() || make_msaa_sized())
        goto failed;
    msaa_point();
    return 0;
failed:
    fprintf(stderr, "[ngl_vk] rn113: MSAA x%u's parts couldn't all be made: drawn with one sample\n", samples);
    msaa_free();
    return -1;
}

/* When make_drawing has made every pipeline: each one-sample pipeline of the first half must have its twin, and the
 * passes the multisampled frame goes on with must be there; else MSAA is let go (said on stderr) */
static void msaa_finish(void)
{
    int missing = 0;

    if (vk.msaa_samples < 2u)
        return;
    for (int i = 0; i < MSAA_TWINS; i++) {
        if (*msaa_slot(i) && !vk.msaa_twin[i])
            missing++;
    }
    if (missing || !vk.msaa_depth_pipeline || !vk.opaque_pass || !vk.blend_pass || !vk.soft_depth) {
        fprintf(stderr, "[ngl_vk] rn113: MSAA let go (%d pipeline twins not made, or the halves' passes missing): "
                "drawn with one sample\n", missing);
        msaa_free();
        return;
    }
    fprintf(stderr, "[ngl_vk] rn113c: MSAA, %u samples a pixel for the 3D scene's opaque half (%ux%u)%s\n",
            vk.msaa_samples, vk.width, vk.height, vk.msaa_coverage ? ", cut-outs by coverage" : "");
}

/* After the multisampled first half (its color resolved into the target): the pass with one sample. The samples'
 * depth into the one-sample depth (msaa_depth.frag: each pixel's first sample; read in the depth's read-only
 * layout), then this frame's grass with its one-sample pipeline over the game's screen (view), as the one-sample
 * first half ends with it. Leaves the target and the depth as the one-sample first half does, for copy_soft_depth
 * and the second half. */
static void msaa_resolve_depth(const VkViewport *view)
{
    VkRenderPassBeginInfo pass = { VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
    VkRect2D whole = { { 0, 0 }, { vk.width, vk.height } };
    VkViewport all = { 0.0f, 0.0f, (float)vk.width, (float)vk.height, 0.0f, 1.0f };

    depth_barrier(vk.msaa_depth, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                  VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL, VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                  VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                  VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    pass.renderPass = vk.msaa_depth_pass;
    pass.framebuffer = vk.msaa_depth_framebuffer;
    pass.renderArea = whole;
    vkCmdBeginRenderPass(vk.commands, &pass, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdSetViewport(vk.commands, 0, 1, &all);
    vkCmdSetScissor(vk.commands, 0, 1, &whole);
    vkCmdBindPipeline(vk.commands, VK_PIPELINE_BIND_POINT_GRAPHICS, vk.msaa_depth_pipeline);
    vkCmdBindDescriptorSets(vk.commands, VK_PIPELINE_BIND_POINT_GRAPHICS, vk.msaa_depth_layout, 0, 1, &vk.msaa_set, 0,
                            NULL);
    vkCmdDraw(vk.commands, 3, 1, 0, 0);
    vkCmdSetViewport(vk.commands, 0, 1, view);
    draw_grass();               /* (the whole target as its scissor, given back) */
    vkCmdEndRenderPass(vk.commands);
    depth_barrier(vk.msaa_depth, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL,
                  VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, VK_ACCESS_SHADER_READ_BIT,
                  VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                  VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                  VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT);
}

/* [rexglue-native thread2] 2026-11-24: what make_target sizes from the target (vk.width x vk.height), shared with
 * ngl_vk_set_target_size: the target and its depth, the main pass's framebuffer (its pass made already), the opaque
 * depth's copy for soft effects, and the host's read-back buffer. */
static int make_target_sized(void)
{
    VkImageView views[2];
    VkFramebufferCreateInfo framebuffer_info = { VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };

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
    if (make_image(vk.width, vk.height, 1, DEPTH_FORMAT, VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                   &vk.soft_depth, &vk.soft_depth_memory, &vk.soft_depth_view))
        return -1;
    return make_host_buffer_cached((VkDeviceSize)vk.width * vk.height * BYTES_PER_PIXEL,
                                   VK_BUFFER_USAGE_TRANSFER_DST_BIT, 1, &vk.readback, &vk.readback_memory,
                                   &vk.readback_pixels);
}

static int make_target(void)
{
    VkAttachmentDescription attachments[2] = { { 0 }, { 0 } };
    VkAttachmentReference color = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
    VkAttachmentReference depth = { 1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL };
    VkSubpassDescription subpass = { 0 };
    VkSubpassDependency dependencies[2];
    VkRenderPassCreateInfo pass_info = { VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };

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

    /* soft effects: the two halves and the depth's copy, read texel by texel */
    {
        VkSamplerCreateInfo sampler_info = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };

        if (make_half_pass(0, &vk.opaque_pass) || make_half_pass(1, &vk.blend_pass))
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

    /* [rexglue-native thread2] 2026-11-24: the images, framebuffer and read-back sized from the target, made the
     * way ngl_vk_set_target_size makes them again */
    if (make_target_sized())
        return -1;

    make_msaa_target();     /* [acct2-bro] 2026-11-22 rn113: MSAA's parts when asked (failing: one sample) */
    return 0;
}

static int make_pipeline(void)
{
    VkSamplerCreateInfo sampler_info = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    VkDescriptorSetLayoutBinding binding = { 0 };
    VkDescriptorSetLayoutCreateInfo set_layout_info = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    VkDescriptorPoolSize pool_size = { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                       1 + MAX_TEXTURES + DOF_SETS + POST_SETS + OUTPUT_SETS + EMPTY_CUBE_SETS + WINDOW_FX_SETS };
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
    /* [acct2-bro] 2026-11-12: the anisotropic one (ngl_vk_set_anisotropy), the same otherwise */
    if (vk.anisotropy_feature && anisotropy_asked > 1.0f) {
        VkPhysicalDeviceProperties gpu;

        vkGetPhysicalDeviceProperties(vk.gpu, &gpu);
        vk.anisotropy = anisotropy_asked < gpu.limits.maxSamplerAnisotropy ? anisotropy_asked
                                                                           : gpu.limits.maxSamplerAnisotropy;
        if (vk.anisotropy < 1.0f)
            vk.anisotropy = 1.0f;
        sampler_info.anisotropyEnable = VK_TRUE;
        sampler_info.maxAnisotropy = vk.anisotropy;
        CHECK(vkCreateSampler(vk.device, &sampler_info, NULL, &vk.aniso_sampler));
        sampler_info.anisotropyEnable = VK_FALSE;
        sampler_info.maxAnisotropy = 0.0f;
    }

    binding.binding = 0;
    binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    binding.descriptorCount = 1;
    binding.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;    /* the grass reads two in its
                                                                                      * vertex shader */
    set_layout_info.bindingCount = 1;
    set_layout_info.pBindings = &binding;
    CHECK(vkCreateDescriptorSetLayout(vk.device, &set_layout_info, NULL, &vk.set_layout));
    pool_info.maxSets = 1 + MAX_TEXTURES + DOF_SETS + POST_SETS + OUTPUT_SETS + EMPTY_CUBE_SETS + WINDOW_FX_SETS;
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

/* [rexglue-native-render] 2026-11-11 rn48l: in the quads' loops (the plain quad pipeline bound before the loop and
 * vk.quad_bound_added set to 0 there): the adding pipeline for a run of NGL_VK_QUAD_ADD corners, else the plain one,
 * bound only when it changes (both have quad_layout, so the bound sets and push constants stay) */
static void bind_quad_pipeline(int added)
{
    if (added && !vk.quad_add_pipeline)
        added = 0;
    if (added == vk.quad_bound_added)
        return;
    vkCmdBindPipeline(vk.commands, VK_PIPELINE_BIND_POINT_GRAPHICS, added ? vk.quad_add_pipeline : vk.quad_pipeline);
    vk.quad_bound_added = added;
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
    /* [rexglue-native-render] 2026-11-11 rn48l: the same pipeline adding (source x source alpha + what is there, the
     * target's alpha kept), for quads marked NGL_VK_QUAD_ADD; without it they are drawn alpha-blended, as before */
    if (result == VK_SUCCESS) {
        blend_attachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
        blend_attachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
        blend_attachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        if (vkCreateGraphicsPipelines(vk.device, VK_NULL_HANDLE, 1, &info, NULL, &vk.quad_add_pipeline) != VK_SUCCESS)
            vk.quad_add_pipeline = VK_NULL_HANDLE;
    }
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
/* [rexglue-native thread2] 2026-11-24: the shadow set's binding 2 pointed at the opaque depth's copy (soft effects;
 * sized from the target), at make_shadow and again at ngl_vk_set_target_size */
static void soft_depth_point(void)
{
    point_image(vk.shadow_set, 2, vk.soft_sampler, vk.soft_depth_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}

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
    VkDescriptorBufferInfo buffer_info = { VK_NULL_HANDLE, 0, VK_WHOLE_SIZE };
    VkDescriptorBufferInfo lights_info = { VK_NULL_HANDLE, 0, VK_WHOLE_SIZE };
    VkWriteDescriptorSet writes[3] = {
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
    lights_info.buffer = vk.draw_lights;
    writes[2].dstSet = vk.shadow_set;
    writes[2].dstBinding = 3;
    writes[2].descriptorCount = 1;
    writes[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[2].pBufferInfo = &lights_info;
    vkUpdateDescriptorSets(vk.device, 3, writes, 0, NULL);
    soft_depth_point();     /* [rexglue-native thread2] 2026-11-24: binding 2, as ngl_vk_set_target_size points it */
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

/* [acct2-bro] 2026-11-12: see ngl_vk.h */
void ngl_vk_set_anisotropy(float most, int borrowed_device_has_it)
{
    anisotropy_asked = most > 1.0f ? most : 1.0f;
    anisotropy_on_borrowed_device = borrowed_device_has_it != 0;
}

float ngl_vk_anisotropy(void)
{
    return vk.aniso_sampler ? vk.anisotropy : 1.0f;
}

/* [acct2-bro] 2026-11-12 rn61a: see ngl_vk.h */
void ngl_vk_set_shown_scale(uint32_t scale)
{
    shown_scale = scale < 1u ? 1u : scale > 4u ? 4u : scale;
}

/* [acct2-bro] 2026-11-22 rn113: see ngl_vk.h */
void ngl_vk_set_msaa(uint32_t samples)
{
    msaa_wanted = samples >= 8u ? 8u : samples >= 4u ? 4u : samples >= 2u ? 2u : 1u;
}

uint32_t ngl_vk_msaa(void)
{
    return vk.msaa_samples > 1u ? vk.msaa_samples : 1u;
}

/* [acct2-bro] 2026-11-22 rn114: see ngl_vk.h */
void ngl_vk_set_effect_count(int on, int precise_on_borrowed_device)
{
    effect_count_wanted = on != 0;
    effect_precise_on_borrowed_device = precise_on_borrowed_device != 0;
}

int ngl_vk_effect_count(uint64_t *samples, uint64_t *screen)
{
    const uint64_t count = __atomic_load_n(&vk.effect_count, __ATOMIC_ACQUIRE);

    if (!count || !samples || !screen)
        return 0;
    *samples = count & 0xFFFFFFFFull;
    *screen = count >> 32;
    return 1;
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

/* [rexglue-native thread2] 2026-11-24: MSAA's twins made again at another sample count (ngl_vk_set_target_size steps
 * MSAA down when a new size is too large for its samples). A twin is the opaque mesh pipeline's create info with the
 * samples and the multisampled pass (msaa_twin); that info lives in make_mesh_pipeline's locals, so the state it had
 * when the first twin was made is kept here (copied, its pointers pointed at the copies). The three twins differ only
 * in their culling (msaa_slot's order: none, the front faces, the back faces). The shader modules are made again from
 * the same SPIR-V and freed after, as make_mesh_pipeline does. */
static struct {
    int kept;
    VkVertexInputBindingDescription binding;
    VkVertexInputAttributeDescription attributes[8];
    VkPipelineVertexInputStateCreateInfo vertex_input;
    VkPipelineInputAssemblyStateCreateInfo assembly;
    VkPipelineViewportStateCreateInfo viewport;
    VkPipelineRasterizationStateCreateInfo raster;
    VkPipelineMultisampleStateCreateInfo multisample;
    VkPipelineColorBlendAttachmentState blend_attachment;
    VkPipelineColorBlendStateCreateInfo blend;
    VkPipelineDepthStencilStateCreateInfo depth;
    VkDynamicState dynamic_states[2];
    VkPipelineDynamicStateCreateInfo dynamic;
    VkPipelineLayout layout;
} msaa_recipe;

static void msaa_keep_recipe(const VkGraphicsPipelineCreateInfo *info)
{
    const VkPipelineVertexInputStateCreateInfo *input = info->pVertexInputState;

    msaa_recipe.kept = 0;
    if (input->vertexBindingDescriptionCount != 1u || input->vertexAttributeDescriptionCount > 8u ||
        info->pColorBlendState->attachmentCount != 1u || info->pDynamicState->dynamicStateCount != 2u)
        return;     /* (not the shape it has today: not kept, and MSAA can't be stepped down) */
    msaa_recipe.binding = input->pVertexBindingDescriptions[0];
    memcpy(msaa_recipe.attributes, input->pVertexAttributeDescriptions,
           input->vertexAttributeDescriptionCount * sizeof msaa_recipe.attributes[0]);
    msaa_recipe.vertex_input = *input;
    msaa_recipe.vertex_input.pVertexBindingDescriptions = &msaa_recipe.binding;
    msaa_recipe.vertex_input.pVertexAttributeDescriptions = msaa_recipe.attributes;
    msaa_recipe.assembly = *info->pInputAssemblyState;
    msaa_recipe.viewport = *info->pViewportState;
    msaa_recipe.raster = *info->pRasterizationState;
    msaa_recipe.multisample = *info->pMultisampleState;
    msaa_recipe.blend_attachment = info->pColorBlendState->pAttachments[0];
    msaa_recipe.blend = *info->pColorBlendState;
    msaa_recipe.blend.pAttachments = &msaa_recipe.blend_attachment;
    msaa_recipe.depth = *info->pDepthStencilState;
    memcpy(msaa_recipe.dynamic_states, info->pDynamicState->pDynamicStates, sizeof msaa_recipe.dynamic_states);
    msaa_recipe.dynamic = *info->pDynamicState;
    msaa_recipe.dynamic.pDynamicStates = msaa_recipe.dynamic_states;
    msaa_recipe.layout = info->layout;
    msaa_recipe.kept = 1;
}

/* The three twins at vk.msaa_samples into the multisampled pass vk.msaa_opaque_pass (both new; vk.msaa_twin empty);
 * 0 when every one-sample pipeline has its twin */
static int msaa_make_twins(void)
{
    static const VkCullModeFlags culls[MSAA_TWINS] = { VK_CULL_MODE_NONE, VK_CULL_MODE_FRONT_BIT,
                                                       VK_CULL_MODE_BACK_BIT };
    VkShaderModule vertex_shader = VK_NULL_HANDLE, fragment_shader = VK_NULL_HANDLE;
    VkPipelineShaderStageCreateInfo stages[2] = {
        { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO },
        { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO },
    };
    VkPipelineRasterizationStateCreateInfo raster = msaa_recipe.raster;
    VkPipelineMultisampleStateCreateInfo multisample = msaa_recipe.multisample;
    VkGraphicsPipelineCreateInfo info = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
    int missing = 0;

    if (!msaa_recipe.kept || vk.msaa_samples < 2u)
        return -1;
    if (make_shader(mesh_vert_spirv, sizeof mesh_vert_spirv, &vertex_shader) ||
        (vk.fabric_sets ? make_shader(mesh_frag_spirv, sizeof mesh_frag_spirv, &fragment_shader)
                        : make_shader(mesh_four_sets_frag_spirv, sizeof mesh_four_sets_frag_spirv, &fragment_shader))) {
        vkDestroyShaderModule(vk.device, vertex_shader, NULL);
        return -1;
    }
    /* the coverage build, as make_mesh_pipeline makes it (rn113b: told the new count by msaa_twin) */
    if (vk.fabric_sets && make_shader(mesh_msaa_frag_spirv, sizeof mesh_msaa_frag_spirv, &vk.msaa_mesh_fragment))
        vk.msaa_mesh_fragment = VK_NULL_HANDLE;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vertex_shader;
    stages[0].pName = "main";
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fragment_shader;
    stages[1].pName = "main";
    info.stageCount = 2;
    info.pStages = stages;
    info.pVertexInputState = &msaa_recipe.vertex_input;
    info.pInputAssemblyState = &msaa_recipe.assembly;
    info.pViewportState = &msaa_recipe.viewport;
    info.pRasterizationState = &raster;
    info.pMultisampleState = &multisample;
    info.pColorBlendState = &msaa_recipe.blend;
    info.pDepthStencilState = &msaa_recipe.depth;
    info.pDynamicState = &msaa_recipe.dynamic;
    info.layout = msaa_recipe.layout;
    info.renderPass = vk.render_pass;
    info.subpass = 0;
    vk.msaa_coverage = 0;
    for (int slot = 0; slot < MSAA_TWINS; slot++) {
        raster.cullMode = culls[slot];
        msaa_twin(&info, &multisample, slot, *msaa_slot(slot));
        missing += *msaa_slot(slot) && !vk.msaa_twin[slot];
    }
    vkDestroyShaderModule(vk.device, vertex_shader, NULL);
    vkDestroyShaderModule(vk.device, fragment_shader, NULL);
    vkDestroyShaderModule(vk.device, vk.msaa_mesh_fragment, NULL);
    vk.msaa_mesh_fragment = VK_NULL_HANDLE;
    return missing ? -1 : 0;
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
    /* [acct2-bro] 2026-11-22 rn113b: the mesh twins' fragment shader, mesh.frag with the cut-outs' coverage (made
     * only with MSAA and the fabric's sets; without it the twins share the one-sample shader) */
    if (vk.msaa_samples > 1u && vk.fabric_sets &&
        make_shader(mesh_msaa_frag_spirv, sizeof mesh_msaa_frag_spirv, &vk.msaa_mesh_fragment))
        vk.msaa_mesh_fragment = VK_NULL_HANDLE;
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
    msaa_keep_recipe(&info);    /* [rexglue-native thread2] 2026-11-24: for the twins made again (msaa_make_twins) */
    if (result == VK_SUCCESS)   /* [acct2-bro] 2026-11-22 rn113: its MSAA twin */
        msaa_twin(&info, &multisample, MSAA_MESH, vk.mesh_pipeline);
    /* [acct2-bro] 2026-11-13 rn69a: the two culling ones (front faces wind clockwise here, so CW is FRONT); without
     * them a draw that asks for culling is drawn from both sides, as before */
    if (result == VK_SUCCESS) {
        raster.cullMode = VK_CULL_MODE_FRONT_BIT;
        if (vkCreateGraphicsPipelines(vk.device, VK_NULL_HANDLE, 1, &info, NULL, &vk.mesh_cull_pipeline[0]) !=
            VK_SUCCESS)
            vk.mesh_cull_pipeline[0] = VK_NULL_HANDLE;
        msaa_twin(&info, &multisample, MSAA_CULL_CW, vk.mesh_cull_pipeline[0]);  /* [acct2-bro] rn113 */
        raster.cullMode = VK_CULL_MODE_BACK_BIT;
        if (vkCreateGraphicsPipelines(vk.device, VK_NULL_HANDLE, 1, &info, NULL, &vk.mesh_cull_pipeline[1]) !=
            VK_SUCCESS)
            vk.mesh_cull_pipeline[1] = VK_NULL_HANDLE;
        msaa_twin(&info, &multisample, MSAA_CULL_CCW, vk.mesh_cull_pipeline[1]);  /* [acct2-bro] rn113 */
        raster.cullMode = VK_CULL_MODE_NONE;    /* as before, for the pipelines that follow */
    }
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
    /* [acct2-bro] 2026-11-22 SP lane, defect E: premultiplied (NGL_VK_BLEND_PREMULTIPLIED): color + behind x
     * (1 - alpha), the game's ONE / INV_SRC_ALPHA (the scopes' glass); not made: drawn by the blended one instead */
    if (result == VK_SUCCESS) {
        blend_attachment.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
        if (vkCreateGraphicsPipelines(vk.device, VK_NULL_HANDLE, 1, &info, NULL, &vk.mesh_premultiplied_pipeline) !=
            VK_SUCCESS)
            vk.mesh_premultiplied_pipeline = VK_NULL_HANDLE;
        blend_attachment.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;    /* as the blended one left it */
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
    /* [rexglue-native stlo 2026-11-16] the scene decal list's surfaces (NGL_VK_BLEND_DECAL): not blended, no depth
     * written (as the blended pipelines), pulled toward the eye as the multiplied one, red, green and blue only; not made:
     * drawn by the blended pipeline instead */
    if (result == VK_SUCCESS) {
        VkPipelineColorBlendAttachmentState kept_decal = blend_attachment;

        blend_attachment.blendEnable = VK_FALSE;
        blend_attachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT;
        raster.depthBiasEnable = VK_TRUE;
        raster.depthBiasConstantFactor = -1.0f;
        raster.depthBiasSlopeFactor = 0.0f;
        /* [rexglue-native stlo 2026-11-17] their depth bias set as they are bound (ngl_vk_set_decal_depth_bias) */
        VkDynamicState decal_states[3] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR,
                                           VK_DYNAMIC_STATE_DEPTH_BIAS };
        VkPipelineDynamicStateCreateInfo decal_dynamic = dynamic;

        decal_dynamic.dynamicStateCount = 3;
        decal_dynamic.pDynamicStates = decal_states;
        info.pDynamicState = &decal_dynamic;
        if (vkCreateGraphicsPipelines(vk.device, VK_NULL_HANDLE, 1, &info, NULL, &vk.mesh_decal_pipeline) != VK_SUCCESS)
            vk.mesh_decal_pipeline = VK_NULL_HANDLE;
        /* [rexglue-native stlo 2026-11-17] NGL_VK_BLEND_DECAL_DEPTH: as NGL_VK_BLEND_DECAL, writing depth */
        depth.depthWriteEnable = VK_TRUE;
        if (vkCreateGraphicsPipelines(vk.device, VK_NULL_HANDLE, 1, &info, NULL, &vk.mesh_decal_depth_pipeline) !=
            VK_SUCCESS)
            vk.mesh_decal_depth_pipeline = VK_NULL_HANDLE;
        depth.depthWriteEnable = VK_FALSE;  /* [rexglue-native stlo 2026-11-17] as before, for what follows */
        /* [rexglue-native stlo 2026-11-16] NGL_VK_BLEND_DECAL_ALPHA: blended as the plain blended pipeline, the rest as above */
        blend_attachment.blendEnable = VK_TRUE;
        blend_attachment.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        blend_attachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        blend_attachment.colorBlendOp = VK_BLEND_OP_ADD;
        blend_attachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        blend_attachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        blend_attachment.alphaBlendOp = VK_BLEND_OP_ADD;
        if (vkCreateGraphicsPipelines(vk.device, VK_NULL_HANDLE, 1, &info, NULL, &vk.mesh_decal_alpha_pipeline) !=
            VK_SUCCESS)
            vk.mesh_decal_alpha_pipeline = VK_NULL_HANDLE;
        info.pDynamicState = &dynamic;    /* [rexglue-native stlo 2026-11-17] as before, for what follows */
        raster.depthBiasEnable = VK_FALSE;  /* as before, for what follows */
        raster.depthBiasConstantFactor = 0.0f;
        blend_attachment = kept_decal;
    }
    /* [rexglue-native-fx] 2026-11-11 fx1: the heat haze's mask (NGL_VK_BLEND_HEAT_MASK): what is there x (1 - alpha),
     * every channel; without it no heat haze */
    if (result == VK_SUCCESS) {         /* [rexglue-native-fx] */
        VkPipelineColorBlendAttachmentState kept = blend_attachment;                /* [rexglue-native-fx] */

        blend_attachment.srcColorBlendFactor = VK_BLEND_FACTOR_ZERO;               /* [rexglue-native-fx] */
        blend_attachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA; /* [rexglue-native-fx] */
        blend_attachment.colorBlendOp = VK_BLEND_OP_ADD;                            /* [rexglue-native-fx] */
        blend_attachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;               /* [rexglue-native-fx] */
        blend_attachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA; /* [rexglue-native-fx] */
        blend_attachment.alphaBlendOp = VK_BLEND_OP_ADD;                            /* [rexglue-native-fx] */
        if (vkCreateGraphicsPipelines(vk.device, VK_NULL_HANDLE, 1, &info, NULL, &vk.mesh_heat_pipeline) !=
            VK_SUCCESS)                                                             /* [rexglue-native-fx] */
            vk.mesh_heat_pipeline = VK_NULL_HANDLE;                                 /* [rexglue-native-fx] */
        blend_attachment = kept;    /* as the subtracting pipeline left it, for what follows [rexglue-native-fx] */
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
    vkDestroyShaderModule(vk.device, vk.msaa_mesh_fragment, NULL);   /* [acct2-bro] rn113b */
    vk.msaa_mesh_fragment = VK_NULL_HANDLE;
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

/* [rexglue-native] 2026-11-10 rn46m: whether a bumped vehicle part's draw has the props' overlay (ngl_vk.h
 * overlay_map): the map in set 4, its numbers at METAL_FLOATS_AT + 1, + 2 and the next vec4 */
static int overlay_draw(const struct ngl_vk_mesh_draw *d)
{
    return vehicle_draw(d) && d->overlay_map && d->overlay_map <= vk.texture_count &&
           !vk.textures[d->overlay_map].cube && d->overlay_scale > 0.0f;
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

/* [waw-decomp] 2026-11-10: whether a draw is drawn as Call of Duty 2's water (ngl_vk.h water_cube): a height map and a
 * cube that is one, no second texture, environment map, normal map or other cube, on a device that binds the fabric's
 * sets (set 6 holds the cube) */
static int water_draw(const struct ngl_vk_mesh_draw *d)
{
    return vk.fabric_sets && d->water_cube && d->water_cube <= vk.texture_count && vk.textures[d->water_cube].cube &&
           d->texture && d->texture <= vk.texture_count && !d->blend_texture && !d->env_texture && !d->normal_map &&
           !d->metal_cube && !d->lens_cube;
}

/* [waw-decomp] 2026-11-10: whether a draw adds a detail map to its color map (ngl_vk.h detail_add_map): a 2D texture,
 * on a device that binds the fabric's sets (set 5 holds the map) */
static int detail_add_draw(const struct ngl_vk_mesh_draw *d)
{
    return vk.fabric_sets && d->detail_add_map && d->detail_add_map <= vk.texture_count &&
           !vk.textures[d->detail_add_map].cube;
}

/* [waw-decomp] 2026-11-10: whether a draw's light is Call of Duty 2's light grid (ngl_vk.h grid_cube): a cube that
 * is one, no other cube, on a device that binds the fabric's sets (set 6 holds the cube) */
static int grid_draw(const struct ngl_vk_mesh_draw *d)
{
    return vk.fabric_sets && d->grid_cube && d->grid_cube <= vk.texture_count && vk.textures[d->grid_cube].cube &&
           !d->metal_cube && !d->lens_cube && !d->water_cube;
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

/* [waw-decomp] 2026-11-10: whether a draw is drawn as one of Call of Duty 2's models with a highlights' lookup
 * (ngl_vk.h specular_lookup): bumped and specular as draw_meshes tests them, its lookup a texture, and none of the
 * fabric's kinds, on a device that binds the fabric's sets (set 4 holds the lookup) */
static int lookup_draw(const struct ngl_vk_mesh_draw *d)
{
    return vk.fabric_sets && d->specular_lookup && d->specular_lookup <= vk.texture_count &&
           !vk.textures[d->specular_lookup].cube && d->texture && d->texture <= vk.texture_count &&
           !d->blend_texture && !d->env_texture && !d->blend_light && d->lit && d->shading == NGL_VK_SHADE_LIGHT &&
           d->normal_map && d->normal_map <= vk.texture_count && d->specular_map &&
           d->specular_map <= vk.texture_count && !fabric_draw(d);
}

/* [waw-decomp] 2026-11-10: whether a draw is one of Call of Duty 2's world surfaces with a highlights' lookup (ngl_vk.h
 * specular_lookup, its second half): lightmapped with a sun (lightmap_scale), its specular map and its lookup both
 * textures, no second texture, and none of the fabric's or the water's kinds, on a device that binds the fabric's
 * sets (set 4 holds the lookup; the specular map goes to set 2) */
static int world_lookup_draw(const struct ngl_vk_mesh_draw *d)
{
    return vk.fabric_sets && d->specular_lookup && d->specular_lookup <= vk.texture_count &&
           !vk.textures[d->specular_lookup].cube && d->texture && d->texture <= vk.texture_count &&
           d->lightmap && d->lightmap <= vk.texture_count && !d->blend_texture && !d->blend_light && d->lit &&
           (d->shading & 3u) == NGL_VK_SHADE_LIGHTMAP && d->lightmap_scale > 0.0f && d->specular_map &&
           d->specular_map <= vk.texture_count && !fabric_draw(d) && !water_draw(d);
}

/* [waw-decomp] 2026-11-11: whether a draw is one of Call of Duty 2's world surfaces drawn by its normal map (ngl_vk.h
 * world_normal_map): lightmapped with a sun as a world surface with a lookup is, its normal map a 2D texture (set 4),
 * and, when it has a highlights' lookup too, its tables a cube (set 6: the lookup can't stay in set 4) */
static int world_normal_draw(const struct ngl_vk_mesh_draw *d)
{
    int tables = d->world_tables && d->world_tables <= vk.texture_count && vk.textures[d->world_tables].cube;

    return vk.fabric_sets && d->world_normal_map && d->world_normal_map <= vk.texture_count &&
           !vk.textures[d->world_normal_map].cube && d->texture && d->texture <= vk.texture_count &&
           d->lightmap && d->lightmap <= vk.texture_count && !d->blend_texture && !d->blend_light && d->lit &&
           (d->shading & 3u) == NGL_VK_SHADE_LIGHTMAP && d->lightmap_scale > 0.0f && !fabric_draw(d) &&
           !lens_draw(d) && !water_draw(d) && (tables || !world_lookup_draw(d));
}

/* [acct2-bro] 2026-11-12: whether a world surface drawn by its normal map reads it Call of Duty 3's way (ngl_vk.h
 * world_bump_scale): a scale, and no highlights' lookup (that is Call of Duty 2's) */
static int world_bump_draw(const struct ngl_vk_mesh_draw *d)
{
    return world_normal_draw(d) && !world_lookup_draw(d) && d->world_bump_scale != 0.0f;
}

/* [acct2-bro] 2026-11-12: and whether such a draw has a detail normal map to mix in (a 2D texture and a repeat);
 * the map rides in set 5 */
static int world_bump_detail_draw(const struct ngl_vk_mesh_draw *d)
{
    return world_bump_draw(d) && d->world_detail_normal_map && d->world_detail_normal_map <= vk.texture_count &&
           !vk.textures[d->world_detail_normal_map].cube && d->world_detail_repeat > 0.0f;
}

/* [acct2-bro] 2026-11-22 rn111: a shiny draw with a strength and tint of its own (ngl_vk.h env_strength): an
 * environment map, no normal map, and none of the kinds that write the slot's later vec4s */
static int env_scaled_draw(const struct ngl_vk_mesh_draw *d)
{
    return d->env_strength > 0.0f && d->env_texture && d->env_texture <= vk.texture_count && !d->normal_map &&
           !d->blend_texture && !d->lightmap && !fabric_draw(d) && !lens_draw(d) && !water_draw(d) &&
           !lookup_draw(d) && !grid_draw(d) && !detail_add_draw(d) && !world_lookup_draw(d) && !world_normal_draw(d);
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
    int water = !fabric && !lens && water_draw(d);  /* [waw-decomp] */
    int lookup = !fabric && !lens && !water && lookup_draw(d);  /* [waw-decomp] */
    int grid = !fabric && !lens && !water && grid_draw(d);      /* [waw-decomp] */
    int detail_add = !fabric && !lens && !water && detail_add_draw(d);  /* [waw-decomp] */
    int world_lookup = !fabric && !lens && !water && world_lookup_draw(d);  /* [waw-decomp] */
    int world_normal = !fabric && !lens && !water && world_normal_draw(d);  /* [waw-decomp] 2026-11-11 */
    int env_scaled = env_scaled_draw(d);    /* [acct2-bro] 2026-11-22 rn111 */

    if (!vk.draw_light_data || vk.draw_light_slots >= MAX_DRAW_LIGHT_SLOTS)
        return 0;
    if (count && !inverted(d->world, inverse))
        count = 0;
    /* [waw-decomp] water, lookup, grid, detail_add, world_lookup */
    if (!count && !rows && !fabric && !lens && !water && !lookup && !grid && !detail_add && !world_lookup &&
        !world_normal && !env_scaled)   /* [waw-decomp] 2026-11-11; [acct2-bro] 2026-11-22 rn111 */
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
    /* [acct2-bro] 2026-11-21 rn101: the game's own normal map frame (ngl_vk.h game_bump_frame): the ninth row's
     * w 1 (mesh.frag reads it only with row 0's w 1; the light grid below fills these vec4s on its own draws,
     * with row 0's w 0) */
    if (rows && d->game_bump_frame)
        slot[16 + 4 * 8 + 3] = 1.0f;
    /* [waw-decomp] the rows' third-degree term (ngl_vk.h irradiance_cubic): row 1's w 1, and at METAL_FLOATS_AT its
     * three axes, each with one part of its color as its w. Not for the draws whose own numbers are there. */
    if (rows && !fabric && !lens && !water && !grid &&
        (d->irradiance_cubic_axes[0][0] != 0.0f || d->irradiance_cubic_axes[0][1] != 0.0f ||
         d->irradiance_cubic_axes[0][2] != 0.0f)) {
        slot[16 + 4 + 3] = 1.0f;
        for (int axis = 0; axis < 3; axis++) {
            slot[METAL_FLOATS_AT + 4 * axis] = d->irradiance_cubic_axes[axis][0];
            slot[METAL_FLOATS_AT + 4 * axis + 1] = d->irradiance_cubic_axes[axis][1];
            slot[METAL_FLOATS_AT + 4 * axis + 2] = d->irradiance_cubic_axes[axis][2];
            slot[METAL_FLOATS_AT + 4 * axis + 3] = d->irradiance_cubic[axis];
        }
    }
    if (detail_add) {   /* [waw-decomp] the detail map's numbers (ngl_vk.h detail_add_map): the second fabric vec4's
                         * y, z and w (its repeat and its offset; a lookup draw has its x), the third's w 2 (1 is
                         * the fabric's detail kind and the scratched metal) */
        slot[FABRIC_FLOATS_AT + 5] = d->detail_add_repeat[0];
        slot[FABRIC_FLOATS_AT + 6] = d->detail_add_repeat[1];
        slot[FABRIC_FLOATS_AT + 7] = d->detail_add_offset;
        slot[FABRIC_FLOATS_AT + 11] = 2.0f;
    }
    if (grid) {     /* [waw-decomp] the light grid (ngl_vk.h grid_cube) in the rows' place: vec4s 4 to 6 the turn's
                     * three rows, vec4 6's w 1 (vec4 4's w 0: no rows), vec4s 7 to 12 the six sets of four colors */
        memset(slot + 16, 0, 9 * 4 * sizeof(float));
        for (int row = 0; row < 3; row++)
            for (int part = 0; part < 3; part++)
                slot[16 + 4 * row + part] = d->grid_turn[row][part];
        slot[16 + 4 * 2 + 3] = 1.0f;
        memcpy(slot + 16 + 4 * 3, d->grid_colors, 6 * 4 * sizeof(float));
    }
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
        /* [rexglue-native-guns] 2026-11-11: the game's blur of the reflection (0: none) */
        slot[FABRIC_FLOATS_AT + 9] = d->metal_cube_blur;
        /* [acct2-bro] 2026-11-21 rn102: below 0, the game's own gradients (ngl_vk.h metal_cube_game_gradients) */
        if (d->metal_cube_game_gradients && d->metal_cube_blur > 0.0f)
            slot[FABRIC_FLOATS_AT + 9] = -d->metal_cube_blur;
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
        /* [acct2-bro] 2026-11-11: the mask's grey floor (ngl_vk.h metal_mask_floor; 0: none), in the w beside the
         * world matrix's first column */
        slot[METAL_FLOATS_AT + 7] = d->metal_mask_floor;
        /* [acct2-bro] 2026-11-12: the normal map's depth (ngl_vk.h metal_normal_depth; 0: none), in the w beside the
         * world matrix's second column */
        slot[METAL_FLOATS_AT + 11] = d->metal_normal_depth;
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
        if (overlay_draw(d)) {  /* [rexglue-native] 2026-11-10 rn46m: the props' overlay: its scale (0: none) and
                                 * angle beside the normal's scale, its key color in the next vec4 */
            slot[METAL_FLOATS_AT + 1] = d->overlay_scale;
            slot[METAL_FLOATS_AT + 2] = d->overlay_angle;
            slot[METAL_FLOATS_AT + 4] = d->overlay_key[0];
            slot[METAL_FLOATS_AT + 5] = d->overlay_key[1];
            slot[METAL_FLOATS_AT + 6] = d->overlay_key[2];
        }
        if (d->k98) {   /* [acct2-bro] 2026-11-15 rn99a: the K98 shader (ngl_vk.h k98): vec4 18 its tint and light
                         * scale, vec4 19 its specular color and 1 (the gun metal's world columns' place, which a
                         * vehicle part doesn't use) */
            slot[METAL_FLOATS_AT + 8] = d->k98_tint[0];
            slot[METAL_FLOATS_AT + 9] = d->k98_tint[1];
            slot[METAL_FLOATS_AT + 10] = d->k98_tint[2];
            slot[METAL_FLOATS_AT + 11] = d->k98_light_scale;
            slot[METAL_FLOATS_AT + 12] = d->k98_specular[0];
            slot[METAL_FLOATS_AT + 13] = d->k98_specular[1];
            slot[METAL_FLOATS_AT + 14] = d->k98_specular[2];
            slot[METAL_FLOATS_AT + 15] = 1.0f;
        }
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
            /* [acct2-bro] 2026-11-12 rn60a: its own tint (ngl_vk.h skin_tint; 0 0 0: none) in the next vec4, the
             * place of the gun metal's first world column, which a skin draw doesn't use */
            slot[METAL_FLOATS_AT + 4] = d->skin_tint[0];
            slot[METAL_FLOATS_AT + 5] = d->skin_tint[1];
            slot[METAL_FLOATS_AT + 6] = d->skin_tint[2];
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
        if (d->lens_opaque == 2) {  /* [acct2-bro] 2026-11-22 SP lane, defect E: the scopes' tinted glass (ngl_vk.h
                                     * lens_tint_map): at FABRIC_FLOATS_AT 2 (3: no tint map, taken as white) and
                                     * lens_color_scale in the w; no shadow's floor; lens_out_scale in the third
                                     * vec4's x, y and w (the fade's place, and the water's two, which a glass leaves
                                     * unread; its z stays 1: a cube) */
            int mapped = d->lens_tint_map && d->lens_tint_map <= vk.texture_count;

            slot[FABRIC_FLOATS_AT] = mapped ? 2.0f : 3.0f;
            slot[FABRIC_FLOATS_AT + 3] = d->lens_color_scale;
            slot[FABRIC_FLOATS_AT + 7] = 0.0f;
            slot[FABRIC_FLOATS_AT + 8] = d->lens_out_scale[0];
            slot[FABRIC_FLOATS_AT + 9] = d->lens_out_scale[1];
            slot[FABRIC_FLOATS_AT + 11] = d->lens_out_scale[2];
        }
    } else if (water && d->cod3_water) {    /* [acct2-bro] 2026-11-13 rn86a: Call of Duty 3's water (ngl_vk.h
                                             * cod3_water): at FABRIC_FLOATS_AT (its color, 0), (the reflection's
                                             * color, 0), (0, 1: water, 0: not a lens, -1: Call of Duty 3's); at
                                             * METAL_FLOATS_AT the two ripple layers (scale, offset u, offset v; the
                                             * bump's scale after the first, the cube's bias after the second), (the
                                             * Fresnel numbers, the opacity), (the highlights' power, 0, 0, 0) */
        for (int part = 0; part < 3; part++) {
            slot[FABRIC_FLOATS_AT + part] = d->cod3_water_color[part];
            slot[FABRIC_FLOATS_AT + 4 + part] = d->cod3_water_reflection[part];
            slot[METAL_FLOATS_AT + part] = d->cod3_water_ripple[0][part];
            slot[METAL_FLOATS_AT + 4 + part] = d->cod3_water_ripple[1][part];
            slot[METAL_FLOATS_AT + 8 + part] = d->cod3_water_fresnel[part];
        }
        slot[METAL_FLOATS_AT + 3] = d->cod3_water_bump;
        slot[METAL_FLOATS_AT + 7] = d->cod3_water_cube_bias;
        slot[METAL_FLOATS_AT + 11] = d->cod3_water_opacity;
        slot[METAL_FLOATS_AT + 12] = d->cod3_water_power;
        slot[FABRIC_FLOATS_AT + 9] = 1.0f;
        slot[FABRIC_FLOATS_AT + 11] = -1.0f;
    } else if (water) { /* [waw-decomp] the water (ngl_vk.h water_cube): at FABRIC_FLOATS_AT (its color, 0: not
                         * fabric), (the sun's color, 0), (0, 1: water, 0: not a lens, 0); at METAL_FLOATS_AT the
                         * way toward the sun */
        for (int part = 0; part < 3; part++) {
            slot[FABRIC_FLOATS_AT + part] = d->water_color[part];
            slot[FABRIC_FLOATS_AT + 4 + part] = d->water_sun_color[part];
            slot[METAL_FLOATS_AT + part] = d->water_sun_toward[part];
        }
        slot[FABRIC_FLOATS_AT + 9] = 1.0f;
    } else if (lookup) {    /* [waw-decomp] a model with a highlights' lookup (ngl_vk.h specular_lookup): at
                             * FABRIC_FLOATS_AT (the highlights' color, 0: not fabric), (a shadowed highlight's share,
                             * 0, 0, 0), (0, 1: a lookup, 0: not metal, 0) */
        for (int part = 0; part < 3; part++)
            slot[FABRIC_FLOATS_AT + part] = d->specular_lookup_color[part];
        slot[FABRIC_FLOATS_AT + 4] = d->specular_lookup_shadowed;
        slot[FABRIC_FLOATS_AT + 9] = 1.0f;
    } else if (world_lookup) {  /* [waw-decomp] a world surface with a highlights' lookup: the same places as a
                                 * model's (the highlights' color; a shadowed highlight's share), its mark the third
                                 * fabric vec4's x 1 (y stays 0: in a lightmapped draw's slot y 1 is the water's) */
        for (int part = 0; part < 3; part++)
            slot[FABRIC_FLOATS_AT + part] = d->specular_lookup_color[part];
        slot[FABRIC_FLOATS_AT + 4] = d->specular_lookup_shadowed;
        slot[FABRIC_FLOATS_AT + 8] = 1.0f;
    }
    if (world_normal)   /* [waw-decomp] 2026-11-11: a world surface drawn by its normal map (ngl_vk.h
                         * world_normal_map): that mark 2 with a highlight, -1 without one */
        slot[FABRIC_FLOATS_AT + 8] = world_lookup ? 2.0f : -1.0f;
    if (world_normal && world_bump_draw(d)) {   /* [acct2-bro] 2026-11-12: Call of Duty 3's way (ngl_vk.h
                                                 * world_bump_scale): that mark -2, and in the first fabric vec4
                                                 * the map's scale, the detail's scale, its repeat (0: no detail
                                                 * map) and the frame's test bits */
        slot[FABRIC_FLOATS_AT + 8] = -2.0f;
        slot[FABRIC_FLOATS_AT + 0] = d->world_bump_scale;
        slot[FABRIC_FLOATS_AT + 1] = d->world_detail_scale;
        slot[FABRIC_FLOATS_AT + 2] = world_bump_detail_draw(d) ? d->world_detail_repeat : 0.0f;
        slot[FABRIC_FLOATS_AT + 3] = (float)(d->world_bump_frame & 7u);
    }
    if (env_scaled) {   /* [acct2-bro] 2026-11-22 rn111: the slot's last vec4 (no other kind of this draw's writes
                         * there): the tint, and the strength in w (above 0 says the vec4 is in use) */
        slot[METAL_FLOATS_AT + 12] = d->env_tint[0];
        slot[METAL_FLOATS_AT + 13] = d->env_tint[1];
        slot[METAL_FLOATS_AT + 14] = d->env_tint[2];
        slot[METAL_FLOATS_AT + 15] = d->env_strength;
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
                       fabric_draw(&vk.draws[i]) || /* [rexglue-native] the fabric's numbers ride in the slot too */
                       water_draw(&vk.draws[i]) ||  /* [waw-decomp] and the water's */
                       lookup_draw(&vk.draws[i]) || /* [waw-decomp] and a highlights' lookup's */
                       grid_draw(&vk.draws[i]) ||   /* [waw-decomp] and the light grid's */
                       world_lookup_draw(&vk.draws[i]) ||   /* [waw-decomp] and a world surface's lookup */
                       world_normal_draw(&vk.draws[i]) ||   /* [waw-decomp] 2026-11-11: and its normal map's */
                       detail_add_draw(&vk.draws[i]);   /* [waw-decomp] and a detail map's */
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

/* [rexglue-native stlo 2026-11-17] */
void ngl_vk_set_decal_depth_bias(float constant_factor, float slope_factor)
{
    vk.decal_depth_constant = constant_factor;
    vk.decal_depth_slope = slope_factor;
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
/* [waw-decomp] 2026-11-24 (proposal): the slot takes bits 16-28 (MAX_DRAW_LIGHT_SLOTS 4096 + 1); bit 29 is free for
 * corner_alpha 2 and mesh.frag reads the slot through SLOT_MASK. */
#define PUSH_CORNER_TIMES_MAP   0x20000000u /* its alpha is its corners' times its base texture's (corner_alpha 2) */
#define PUSH_BILLBOARD          0x80000000u /* [rexglue-native-world] 2026-11-11 w1: a card turned to face the view
                                             * (ngl_vk.h billboard); above the slot's bits (a slot is at most
                                             * MAX_MESH_DRAWS, bits 16-30); mesh.vert clears it before the slot
                                             * is read */

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
    float matrix[16], inverse[9], determinant, turn;
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
    /* [acct2-bro] 2026-11-13 rn67b: the nine numbers above are the inverse times the determinant. Under a mirroring
     * world matrix the determinant is below 0, so a way taken through them came out turned around and the mesh was
     * lit from its far side (the shader's normalize takes the size out, not the sign). turn puts the sign back. */
    turn = invertible && determinant < 0 ? -1.0f : 1.0f;
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
        /* row vector x inverse; the inverse's 1/determinant scales (the shader normalizes) and, below 0, turns
         * the way around: turn (rn67b) */
        for (int column = 0; column < 3; column++)
            in_mesh[column] = invertible ? turn * (toward[0] * inverse[column] + toward[1] * inverse[3 + column] +
                                                   toward[2] * inverse[6 + column])
                                         : toward[column];
        push[24 + light] = packed_toward(in_mesh);
        push[28 + light] = packed_color(color);
    }
    if (mode & MESH_MODE_LIGHTMAP_SUN) {    /* the world's up (z) in the second light's place, turned as the lights are */
        float up[3] = { 0.0f, 0.0f, 1.0f };

        if (invertible)
            for (int column = 0; column < 3; column++)
                up[column] = turn * inverse[6 + column];    /* rn67b */
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
    if (d->corner_alpha == NGL_VK_CORNER_TIMES_MAP)    /* [waw-decomp] 2026-11-24 (proposal) */
        flags |= PUSH_CORNER_TIMES_MAP;
    if (d->billboard)                       /* [rexglue-native-world] 2026-11-11 w1 */
        flags |= PUSH_BILLBOARD;
    /* [acct2-bro] 2026-11-22 rn115a: the draw's own alpha test (alpha_cut) in the fourth light's color, which a draw
     * with fewer than four lights leaves 0 (set above) and no shader reads; mesh.frag and shadow.frag take it there */
    if (more < 3 && d->alpha_cut > 0.0f && d->alpha_cut <= 1.0f)
        memcpy(push + 31, &d->alpha_cut, sizeof d->alpha_cut);
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
 * pass). The frame's draws are let go after part 1 or 2 (after part 3 when the effect layer follows part 1). */
/* [rexglue-native] 2026-11-09: the scissor a mesh draw asks for (split screen), within the target; the whole target
 * when it asks for none */
static VkRect2D draw_scissor(const struct ngl_vk_mesh_draw *d)
{
    return scissor_within(d->scissor_x, d->scissor_y, d->scissor_width, d->scissor_height);
}

/* [acct2-bro] 2026-11-22 Camo lane: the camo pass over the opaque draw d (ngl_vk.h camo_texture), right after d was
 * drawn in draw_meshes' opaque loop: the pipeline for d's culling (its MSAA twin while the twins are in the
 * pipelines' places), the camo in set 0, its numbers in the push constants after d's matrix (push_mesh_draw has just
 * pushed d's; camo.vert reads the matrix and these), d's own indices and corners. Nothing is drawn for a camo that
 * isn't a 2D texture of this frame's, for numbers that aren't sane (scale above 0 and at most 64, strength above 0)
 * or without the pipeline. Then *pipeline is emptied and set 0 marked unbound, so the next draw binds its own again
 * (the other sets keep theirs: the same layout). */
static void draw_camo(const struct ngl_vk_mesh_draw *d, VkPipeline *pipeline, VkDescriptorSet *bound)
{
    const int k = d->cull == NGL_VK_CULL_CW ? 1 : d->cull == NGL_VK_CULL_CCW ? 2 : 0;
    const VkPipeline camo = vk.msaa_swapped ? vk.camo_msaa_pipeline[k] : vk.camo_pipeline[k];
    float params[8];

    if (d->alpha != NGL_VK_ALPHA_OPAQUE || !d->camo_texture || d->camo_texture > vk.texture_count ||
        vk.textures[d->camo_texture].cube || !camo || !d->mesh || d->mesh > vk.mesh_count)
        return;
    for (int i = 0; i < 8; i++) {   /* finite and in range, or not drawn (no math.h here: NaN fails x == x) */
        const float x = d->camo_params[i];

        if (!(x == x) || x < -1e4f || x > 1e4f)
            return;
        params[i] = x;
    }
    if (!(params[0] > 0.0f) || params[0] > 64.0f || !(params[3] > 0.0f))
        return;
    params[3] = params[3] > 1.0f ? 1.0f : params[3];
    for (int i = 4; i < 7; i++)
        params[i] = params[i] < 1.0f / 255.0f ? 1.0f / 255.0f : params[i] > 1.0f ? 1.0f : params[i];
    params[7] = 0.0f;
    vkCmdBindPipeline(vk.commands, VK_PIPELINE_BIND_POINT_GRAPHICS, camo);
    vkCmdBindDescriptorSets(vk.commands, VK_PIPELINE_BIND_POINT_GRAPHICS, vk.mesh_layout, 0, 1,
                            &vk.textures[d->camo_texture].set, 0, NULL);
    vkCmdPushConstants(vk.commands, vk.mesh_layout, VK_SHADER_STAGE_VERTEX_BIT, 16 * sizeof(float), sizeof params,
                       params);
    vkCmdDrawIndexed(vk.commands,
                     d->index_count && d->index_count < vk.meshes[d->mesh].index_count
                         ? d->index_count : vk.meshes[d->mesh].index_count,
                     1, vk.meshes[d->mesh].first_index,
                     (int32_t)(d->corners ? d->corners - 1u : vk.meshes[d->mesh].first_corner), 0);
    *pipeline = VK_NULL_HANDLE;
    bound[0] = VK_NULL_HANDLE;
    vk.camo_count++;
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

    /* [rexglue-native] 2026-11-10: part 3 is the effect layer's draws (ngl_vk.h effect_layer), into its half-size
     * target: the blended draws marked for it, with the layer's depth in set 3's binding 2; parts 1 leaves those out
     * while the layer is in use (effect_live says a part 3 follows this frame) */
    if (part == 3) {
        whole.extent.width = vk.effect_width;
        whole.extent.height = vk.effect_height;
        scissor = whole;
    }
    vkCmdBindVertexBuffers(vk.commands, 0, 1, &vk.mesh_corners, &no_offset);
    vkCmdBindIndexBuffer(vk.commands, vk.mesh_indices, 0, VK_INDEX_TYPE_UINT16);
    vkCmdBindDescriptorSets(vk.commands, VK_PIPELINE_BIND_POINT_GRAPHICS, vk.mesh_layout, 3, 1,
                            part == 3 ? &vk.effect_shadow_set : &vk.shadow_set, 0, NULL);
    /* [rexglue-native-fx] 2026-11-11 fx1: part 4 is the heat haze's mask draws (NGL_VK_BLEND_HEAT_MASK), into
     * heat_mask: the layer's size and depth, as part 3 */
    if (part == 4) {                                                                /* [rexglue-native-fx] */
        whole.extent.width = vk.effect_width;                                       /* [rexglue-native-fx] */
        whole.extent.height = vk.effect_height;                                     /* [rexglue-native-fx] */
        scissor = whole;                                                            /* [rexglue-native-fx] */
        vkCmdBindDescriptorSets(vk.commands, VK_PIPELINE_BIND_POINT_GRAPHICS, vk.mesh_layout, 3, 1,
                                &vk.effect_shadow_set, 0, NULL);                    /* [rexglue-native-fx] */
    }
    for (int blended = part == 1 || part == 3; blended < (part == 0 ? 1 : 2); blended++) {
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
                            /* [acct2-bro] 2026-11-22 rn111: a shiny draw with its own strength and tint
                             * (env_scaled_draw) carries the mix's by-alpha bit too: shiny is never mixed, so no draw
                             * had the pair, and mesh.frag reads the slot's last vec4 on that pair alone */
                            (env && env_scaled_draw(d) ? MESH_MODE_BY_ALPHA : 0u) |
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
                                   : specular ? vk.textures[d->specular_map].set
                                   /* [waw-decomp] a world surface with a highlights' lookup: its specular map */
                                   : world_lookup_draw(d) ? vk.textures[d->specular_map].set : untextured,
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
            int water = !fabric && !lens && water_draw(d);  /* [waw-decomp] the water: its cube in set 6 too */
            /* [waw-decomp] a model with a highlights' lookup: the lookup in set 4 */
            int lookup = !fabric && !lens && !water && lookup_draw(d);
            /* [waw-decomp] a model lit by Call of Duty 2's light grid: the weights' cube in set 6 */
            int grid = !fabric && !lens && !water && grid_draw(d);
            /* [waw-decomp] a draw that adds a detail map to its color map: the map in set 5 */
            int detail_add = !fabric && !lens && !water && detail_add_draw(d);
            /* [waw-decomp] a world surface with a highlights' lookup: the lookup in set 4 too */
            int world_lookup = !fabric && !lens && !water && world_lookup_draw(d);
            /* [waw-decomp] 2026-11-11: a world surface drawn by its normal map: the map in set 4 (in the lookup's
             * place), its tables' cube in set 6 when it has a highlight */
            int world_normal = !fabric && !lens && !water && world_normal_draw(d);
            /* [acct2-bro] 2026-11-12: Call of Duty 3's bumped world with a detail normal map: the map in set 5 */
            int world_bump_detail = world_normal && world_bump_detail_draw(d);
            VkDescriptorSet fabric_textures[3] = {
                world_normal ? vk.textures[d->world_normal_map].set              /* [waw-decomp] 2026-11-11 */
                : lookup || world_lookup ? vk.textures[d->specular_lookup].set   /* [waw-decomp] */
                : metal ? (metal_scratched(d) ? vk.textures[d->metal_scratch].set : untextured)
                      : vehicle && overlay_draw(d) ? vk.textures[d->overlay_map].set  /* [rexglue-native] rn46m */
                      : fabric && !skin && !detail && !vehicle ? vk.textures[d->fabric_mask].set
                      : skin && skin_woven(d)                  ? vk.textures[d->skin_swatch].set  /* rn40x */
                      /* [acct2-bro] 2026-11-22 defect E: the scopes' glass's tint map (ngl_vk.h lens_tint_map) */
                      : lens && d->lens_opaque == 2 && d->lens_tint_map && d->lens_tint_map <= vk.texture_count
                          ? vk.textures[d->lens_tint_map].set
                                                               : untextured,
                metal || vehicle ? untextured
                      : skin ? vk.textures[d->noise_swatch].set
                             : detail ? vk.textures[d->detail_map].set
                                      : fabric ? vk.textures[d->normal_swatch].set
                                      : detail_add ? vk.textures[d->detail_add_map].set     /* [waw-decomp] */
                                      /* [acct2-bro] 2026-11-12: or Call of Duty 3's world's detail normal map */
                                      : world_bump_detail ? vk.textures[d->world_detail_normal_map].set
                                                          : untextured,
                metal ? vk.textures[d->metal_cube].set : lens ? vk.textures[d->lens_cube].set
                      : water ? vk.textures[d->water_cube].set                       /* [waw-decomp] water */
                      : grid ? vk.textures[d->grid_cube].set                         /* [waw-decomp] the grid */
                      : world_normal && world_lookup ? vk.textures[d->world_tables].set /* [waw-decomp] 2026-11-11 */
                                                     : vk.empty_cube_set,
            };

            if (!d->mesh || d->mesh > vk.mesh_count || (d->alpha == NGL_VK_ALPHA_BLEND) != blended)
                continue;
            /* [rexglue-native] 2026-11-10: the effect layer's draws in part 3 only, the others never in it */
            if (vk.effect_live && blended && (part == 3) != (d->effect_layer != 0))
                continue;
            /* [rexglue-native-fx] 2026-11-11 fx1: the heat haze's mask draws in part 4 only, and nothing else there */
            if ((d->blend == NGL_VK_BLEND_HEAT_MASK) != (part == 4))                   /* [rexglue-native-fx] */
                continue;                                                           /* [rexglue-native-fx] */
            if (blended) {      /* in the order given: an effect's blending can change from one draw to the next */
                VkPipeline wanted = d->blend == NGL_VK_BLEND_ADD ? vk.mesh_add_pipeline
                                  : d->blend == NGL_VK_BLEND_SUBTRACT ? vk.mesh_subtract_pipeline
                                  : d->blend == NGL_VK_BLEND_MULTIPLY ? vk.mesh_multiply_pipeline  /* [waw-decomp] */
                                  /* [rexglue-native stlo 2026-11-16] */
                                  : d->blend == NGL_VK_BLEND_DECAL && vk.mesh_decal_pipeline ? vk.mesh_decal_pipeline
                                  /* [rexglue-native stlo 2026-11-17] */
                                  : d->blend == NGL_VK_BLEND_DECAL_DEPTH && vk.mesh_decal_depth_pipeline
                                      ? vk.mesh_decal_depth_pipeline
                                  : d->blend == NGL_VK_BLEND_DECAL_DEPTH && vk.mesh_decal_pipeline  /* [rexglue-native stlo 2026-11-17] */
                                      ? vk.mesh_decal_pipeline
                                  /* [rexglue-native stlo 2026-11-16] */
                                  : d->blend == NGL_VK_BLEND_DECAL_ALPHA && vk.mesh_decal_alpha_pipeline
                                      ? vk.mesh_decal_alpha_pipeline
                                  /* [acct2-bro] 2026-11-22 defect E */
                                  : d->blend == NGL_VK_BLEND_PREMULTIPLIED && vk.mesh_premultiplied_pipeline
                                      ? vk.mesh_premultiplied_pipeline
                                                                      : vk.mesh_blend_pipeline;
                if (d->blend == NGL_VK_BLEND_HEAT_MASK)     /* [rexglue-native-fx] 2026-11-11 fx1 */
                    wanted = vk.mesh_heat_pipeline;                                 /* [rexglue-native-fx] */

                if (wanted != pipeline) {
                    pipeline = wanted;
                    vkCmdBindPipeline(vk.commands, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
                    if (pipeline == vk.mesh_decal_pipeline || pipeline == vk.mesh_decal_alpha_pipeline ||  /* [rexglue-native stlo 2026-11-17] */
                        (pipeline == vk.mesh_decal_depth_pipeline && pipeline))  /* [rexglue-native stlo 2026-11-17] */
                        vkCmdSetDepthBias(vk.commands, vk.decal_depth_constant, 0.0f, vk.decal_depth_slope);
                    bound[0] = bound[1] = bound[2] = VK_NULL_HANDLE;
                    fabric_bound[0] = fabric_bound[1] = fabric_bound[2] = VK_NULL_HANDLE;
                }
            } else {    /* [acct2-bro] 2026-11-13 rn69a: an opaque draw drops the faces its game state drops */
                VkPipeline wanted = d->cull == NGL_VK_CULL_CW && vk.mesh_cull_pipeline[0] ? vk.mesh_cull_pipeline[0]
                                  : d->cull == NGL_VK_CULL_CCW && vk.mesh_cull_pipeline[1] ? vk.mesh_cull_pipeline[1]
                                                                                          : vk.mesh_pipeline;

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

                if (part == 3) {    /* [rexglue-native] 2026-11-10: the same part of the half-size layer */
                    uint32_t right = (uint32_t)wanted.offset.x + wanted.extent.width;
                    uint32_t bottom = (uint32_t)wanted.offset.y + wanted.extent.height;

                    wanted.offset.x /= 2;
                    wanted.offset.y /= 2;
                    wanted.extent.width = (right + 1u) / 2u - (uint32_t)wanted.offset.x;
                    wanted.extent.height = (bottom + 1u) / 2u - (uint32_t)wanted.offset.y;
                }
                if (part == 4) {    /* [rexglue-native-fx] 2026-11-11 fx1: the same for the heat mask */
                    uint32_t right = (uint32_t)wanted.offset.x + wanted.extent.width;      /* [rexglue-native-fx] */
                    uint32_t bottom = (uint32_t)wanted.offset.y + wanted.extent.height;    /* [rexglue-native-fx] */

                    wanted.offset.x /= 2;                                                   /* [rexglue-native-fx] */
                    wanted.offset.y /= 2;                                                   /* [rexglue-native-fx] */
                    wanted.extent.width = (right + 1u) / 2u - (uint32_t)wanted.offset.x;    /* [rexglue-native-fx] */
                    wanted.extent.height = (bottom + 1u) / 2u - (uint32_t)wanted.offset.y;  /* [rexglue-native-fx] */
                }
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
                             /* [rexglue-native stlo 2026-11-16] a decal list's cut-out surface */
                             (d->blend == NGL_VK_BLEND_DECAL && d->decal_alpha == NGL_VK_ALPHA_TEST ? MESH_ALPHA_TEST
                                                                                                    : 0u) |
                             /* [rexglue-native stlo 2026-11-17] and a depth-writing one's */
                             (d->blend == NGL_VK_BLEND_DECAL_DEPTH && d->decal_alpha == NGL_VK_ALPHA_TEST ? MESH_ALPHA_TEST
                                                                                                          : 0u) |
                             (d->light_once ? MESH_LIGHT_ONCE : 0u));
            /* [acct2-bro] 2026-11-22 Camo lane: its camo over it (draw_camo); the picture's opaque draws only */
            if (!blended && part != 3 && part != 4 && d->camo_texture)
                draw_camo(d, &pipeline, bound);
        }
    }
    if (memcmp(&scissor, &whole, sizeof whole))
        vkCmdSetScissor(vk.commands, 0, 1, &whole);    /* [rexglue-native] what comes after: the whole target */
    if (part == 0) {
        if (vk.msaa_swapped)    /* [acct2-bro] 2026-11-22 rn113c: with MSAA, after the resolve (msaa_resolve_depth) */
            return;
        draw_grass();       /* the pass ends after it: nothing to bind again */
        return;
    }
    /* [rexglue-native] 2026-11-10: with the effect layer in use, part 3 follows part 1 and needs the frame's draws
     * still (the first build let them go here: the layer stayed empty and the smoke, dust and fire were gone) */
    if (part == 1 && vk.effect_live)
        return;
    if (part == 3 && vk.heat_live)  /* [rexglue-native-fx] 2026-11-11 fx1: part 4 follows and needs them too */
        return;
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
        if (level_count > 1 && vk.aniso_sampler)    /* [acct2-bro] 2026-11-12: ngl_vk_set_anisotropy */
            image_info.sampler = vk.aniso_sampler;
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

/* [acct2-bro] 2026-11-23 c3camo v2: see ngl_vk.h */
void ngl_vk_set_block_textures(int on, int borrowed_device_has_it)
{
    block_textures_asked = on != 0;
    block_textures_on_borrowed_device = borrowed_device_has_it != 0;
}

int ngl_vk_block_textures(void)
{
    return vk.device && vk.block_textures;
}

/* [acct2-bro] 2026-11-23 c3camo v2: stage_texture for block levels: texture n's levels, sizes[i] bytes each, laid out
 * as copy[] says (each level's offset a multiple of 16, a block), into this frame's half of the staging buffer */
static int stage_blocks(uint32_t n, const void *const *levels, uint32_t level_count, const VkBufferImageCopy *copy,
                        const VkDeviceSize *sizes, VkDeviceSize bytes)
{
    VkDeviceSize at;
    uint32_t level;

    if ((vk.texture_staged + bytes > TEXTURE_STAGING_HALF || vk.pending_count == MAX_PENDING_TEXTURES) &&
        flush_textures())
        return -1;
    at = (VkDeviceSize)vk.texture_staging_half * TEXTURE_STAGING_HALF + vk.texture_staged;
    for (level = 0; level < level_count; level++) {
        memcpy(vk.texture_staging_data + at + copy[level].bufferOffset, levels[level], (size_t)sizes[level]);
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

/* [acct2-bro] 2026-11-23 c3camo v2: see ngl_vk.h. ngl_vk_texture_levels' steps with a block format: the levels'
 * sizes in blocks, a BC7 UNORM image, the same copies, barriers, descriptor set and sampler. */
uint32_t ngl_vk_texture_blocks(uint32_t format, const void *const *levels, uint32_t level_count, uint32_t width,
                               uint32_t height)
{
    VkCommandBufferBeginInfo begin = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    VkSubmitInfo submit = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    VkDescriptorSetAllocateInfo set_info = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
    VkWriteDescriptorSet write = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
    VkDescriptorImageInfo image_info;
    VkBufferImageCopy copy[MAX_TEXTURE_LEVELS];
    VkDeviceSize sizes[MAX_TEXTURE_LEVELS];
    VkBuffer staging = VK_NULL_HANDLE;
    VkDeviceMemory staging_memory = VK_NULL_HANDLE;
    void *staging_pixels = NULL;
    VkDeviceSize bytes = 0;
    uint32_t n, level, most = 1;
    int failed = 0, batched;

    if (format != NGL_VK_BLOCKS_BC7 || !vk.device || !vk.block_textures || !levels || !levels[0] || !level_count ||
        !width || !height || vk.texture_count == MAX_TEXTURES)
        return 0;
    while (most < MAX_TEXTURE_LEVELS && ((width >> most) || (height >> most)))
        most++;                                         /* down to 1 x 1 */
    if (level_count > most)
        level_count = most;
    for (level = 0; level < level_count; level++) {     /* level i: max(1, width >> i) x max(1, height >> i) texels */
        uint32_t w = width >> level ? width >> level : 1, h = height >> level ? height >> level : 1;

        if (!levels[level]) {                           /* the levels given stop here */
            level_count = level;
            break;
        }
        memset(&copy[level], 0, sizeof copy[level]);
        copy[level].bufferOffset = bytes;               /* a multiple of 16: whole blocks before it */
        copy[level].imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        copy[level].imageSubresource.mipLevel = level;
        copy[level].imageSubresource.layerCount = 1;
        copy[level].imageExtent.width = w;              /* the level's own size: the copy reaches its edges */
        copy[level].imageExtent.height = h;
        copy[level].imageExtent.depth = 1;
        sizes[level] = (VkDeviceSize)((w + 3u) / 4u) * ((h + 3u) / 4u) * 16u;
        bytes += sizes[level];
    }
    batched = vk.batch_textures && vk.texture_staging_data && bytes <= TEXTURE_STAGING_HALF && !vk.low_frame_open;
    if (!batched)
        finish_frame();     /* its upload reuses the draws' command buffer and fence */
    n = vk.texture_count + 1;
    if (make_image(width, height, level_count, VK_FORMAT_BC7_UNORM_BLOCK,
                   VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, &vk.textures[n].image,
                   &vk.textures[n].memory, &vk.textures[n].view) ||
        (!batched &&
         make_host_buffer(bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, &staging, &staging_memory, &staging_pixels)))
        failed = 1;
    if (!failed && !batched) {
        for (level = 0; level < level_count; level++)
            memcpy((uint8_t *)staging_pixels + copy[level].bufferOffset, levels[level], (size_t)sizes[level]);
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
    if (!failed && !vk.textures[n].set) {               /* a number given out again keeps its set */
        set_info.descriptorPool = vk.descriptor_pool;
        set_info.descriptorSetCount = 1;
        set_info.pSetLayouts = &vk.set_layout;
        failed = vkAllocateDescriptorSets(vk.device, &set_info, &vk.textures[n].set) != VK_SUCCESS;
    }
    if (!failed) {
        image_info.sampler = vk.sampler;
        if (level_count > 1 && vk.aniso_sampler)
            image_info.sampler = vk.aniso_sampler;
        image_info.imageView = vk.textures[n].view;
        image_info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        write.dstSet = vk.textures[n].set;
        write.dstBinding = 0;
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        write.pImageInfo = &image_info;
        vkUpdateDescriptorSets(vk.device, 1, &write, 0, NULL);
    }
    if (!failed && batched)                             /* last: a failure above leaves nothing waiting */
        failed = stage_blocks(n, levels, level_count, copy, sizes, bytes) != 0;
    if (staging_memory)
        vkUnmapMemory(vk.device, staging_memory);
    vkDestroyBuffer(vk.device, staging, NULL);
    vkFreeMemory(vk.device, staging_memory, NULL);
    if (failed) {
        VkDescriptorSet set = vk.textures[n].set;       /* kept for the next texture with this number */

        vkDestroyImageView(vk.device, vk.textures[n].view, NULL);
        vkDestroyImage(vk.device, vk.textures[n].image, NULL);
        vkFreeMemory(vk.device, vk.textures[n].memory, NULL);
        memset(&vk.textures[n], 0, sizeof vk.textures[n]);
        vk.textures[n].set = set;
        fprintf(stderr, "[ngl_vk] a %ux%u BC7 texture couldn't be made\n", width, height);
        return 0;
    }
    vk.textures[n].cube = 0;
    vk.texture_count = n;
    return n;
}

/* [waw-decomp] 2026-11-10: new pixels for a texture made earlier (ngl_vk.h ngl_vk_texture_rewrite): its one level
 * written whole. With batched textures it waits in the staging buffer like a new texture's pixels and is copied in at
 * the start of the next draw, in the order asked (so after the texture's own first copy, when that still waits);
 * else it is copied in and waited for now. The image's old pixels are given up first (its layout taken as undefined),
 * which is sound because the whole level is written. */
int ngl_vk_texture_rewrite(uint32_t texture, const void *pixels, uint32_t width, uint32_t height)
{
    VkCommandBufferBeginInfo begin = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    VkSubmitInfo submit = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    VkBufferImageCopy copy;
    VkBuffer staging = VK_NULL_HANDLE;
    VkDeviceMemory staging_memory = VK_NULL_HANDLE;
    void *staging_pixels = NULL;
    VkDeviceSize bytes = (VkDeviceSize)width * height * BYTES_PER_PIXEL;
    VkImage image;
    int failed;

    if (!vk.device || !pixels || !width || !height || !texture || texture > vk.texture_count ||
        vk.textures[texture].cube || !vk.textures[texture].image)
        return -1;
    image = vk.textures[texture].image;
    memset(&copy, 0, sizeof copy);
    copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    copy.imageSubresource.layerCount = 1;
    copy.imageExtent.width = width;
    copy.imageExtent.height = height;
    copy.imageExtent.depth = 1;
    if (vk.batch_textures && vk.texture_staging_data && bytes <= TEXTURE_STAGING_HALF && !vk.low_frame_open)
        return stage_texture(texture, &pixels, 1, &copy, bytes) ? -1 : 0;
    finish_frame();     /* the copy reuses the draws' command buffer and fence */
    failed = make_host_buffer(bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, &staging, &staging_memory, &staging_pixels) != 0;
    if (!failed) {
        memcpy(staging_pixels, pixels, (size_t)bytes);
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &vk.commands;
        failed = vkResetFences(vk.device, 1, &vk.done) != VK_SUCCESS ||
                 vkResetCommandBuffer(vk.commands, 0) != VK_SUCCESS ||
                 vkBeginCommandBuffer(vk.commands, &begin) != VK_SUCCESS;
    }
    if (!failed) {
        levels_barrier(image, 1, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
                       VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        vkCmdCopyBufferToImage(vk.commands, staging, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
        levels_barrier(image, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                       VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                       VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
        failed = vkEndCommandBuffer(vk.commands) != VK_SUCCESS || submit_to_queue(&submit, vk.done) != VK_SUCCESS ||
                 vkWaitForFences(vk.device, 1, &vk.done, VK_TRUE, WAIT_NS) != VK_SUCCESS;
    }
    if (staging_memory)
        vkUnmapMemory(vk.device, staging_memory);
    vkDestroyBuffer(vk.device, staging, NULL);
    vkFreeMemory(vk.device, staging_memory, NULL);
    return failed ? -1 : 0;
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

/* [cod2-native 2026-11-24] Every mesh forgotten, the textures kept (see ngl_vk.h): ngl_vk_forget without its textures,
 * so a game whose mesh corners run out (Call of Duty 2's world, every minute or two) doesn't decode and upload every
 * texture again. The last frame is waited for first, as there. */
void ngl_vk_forget_meshes(void)
{
    if (!vk.device)
        return;
    finish_frame();
    vk.mesh_count = 0;
    vk.corners_used = 0;
    vk.indices_used = 0;
    /* the copies still waiting were for the meshes just forgotten (the textures' own uploads wait apart, and stay) */
    vk.copy_count[0] = 0;
    vk.copy_count[1] = 0;
    vk.staged = 0;
    /* numbers the caller set for this frame: as ngl_vk_forget, set again each frame */
    vk.grass_patch_count = 0;
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
static int make_effect_layer(void);     /* [rexglue-native] 2026-11-10 */
static int make_glow(void);             /* [rexglue-native] 2026-11-10 */
static int make_heat(void);             /* [rexglue-native-fx] 2026-11-11 fx1 */

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
/* ---------------------------------------------------------------------------------------------------------------
 * [acct2-bro] 2026-11-22 rn114: the effect layer's samples (ngl_vk_set_effect_count, ngl_vk_effect_count), for Call of
 * Duty 3's effects governor: the game thins its new particles by the samples its effects drew the frame before (an
 * occlusion query around its effects pass, a half-size target on the Xbox 360 too); the port's GPU emulation
 * answers that query with a stand-in count, so nothing is ever thinned (the owner, 11-22 08:02: "ambient fog spawns
 * too quickly"). Here an occlusion query is wrapped around the effect layer's draws (draw_meshes(3)), every sample
 * that passes the depth test counted, overdraw included, as the game's query counts them. Two queries in turn: the
 * frame's is reset before the layer's passes, begun and ended around its draws, and read at the next draw, after
 * finish_frame has waited for that frame. Nothing is made unless the count is asked for. */
static void effect_count_free(void)
{
    if (vk.device && vk.effect_queries)
        vkDestroyQueryPool(vk.device, vk.effect_queries, NULL);
    vk.effect_queries = VK_NULL_HANDLE;
    vk.effect_query_frame = vk.effect_query_last = -1;
    __atomic_store_n(&vk.effect_count, 0, __ATOMIC_RELEASE);
}

/* the two queries, when asked for and the effect layer is there (its draws are what is counted) */
static void make_effect_count(void)
{
    VkQueryPoolCreateInfo info = { VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO };

    vk.effect_query_frame = vk.effect_query_last = -1;
    if (!effect_count_wanted || !vk.effect_ready || !vkCreateQueryPool)
        return;
    info.queryType = VK_QUERY_TYPE_OCCLUSION;
    info.queryCount = 2;
    if (vkCreateQueryPool(vk.device, &info, NULL, &vk.effect_queries) != VK_SUCCESS) {
        vk.effect_queries = VK_NULL_HANDLE;
        fprintf(stderr, "[ngl_vk] rn114: no query pool: the effect layer's samples aren't counted\n");
        return;
    }
    fprintf(stderr, "[ngl_vk] rn114: the effect layer's samples are counted (%s)\n",
            vk.effect_query_flags ? "exact" : "not exact: the device has no precise occlusion queries on");
}

/* At a draw, after finish_frame: the last frame's count, when it counted (none counted: no effect drawn, 0) */
static void effect_count_read(void)
{
    uint64_t samples = 0;

    if (!vk.effect_queries)
        return;
    if (vk.effect_query_last >= 0 &&
        vkGetQueryPoolResults(vk.device, vk.effect_queries, (uint32_t)vk.effect_query_last, 1, sizeof samples,
                              &samples, sizeof samples, VK_QUERY_RESULT_64_BIT) != VK_SUCCESS)
        return;     /* not there yet: the count before stays */
    if (vk.effect_query_last >= 0 && vk.effect_screen_last)
        __atomic_store_n(&vk.effect_count, vk.effect_screen_last << 32 | (samples > 0xFFFFFFFFull ? 0xFFFFFFFFull
                                                                                                   : samples),
                         __ATOMIC_RELEASE);
    else if (vk.effect_screen_last)
        __atomic_store_n(&vk.effect_count, vk.effect_screen_last << 32, __ATOMIC_RELEASE);
    vk.effect_query_last = -1;
}

/* Before the effect layer's passes (outside a pass): this frame's query reset */
static void effect_count_reset(void)
{
    if (!vk.effect_queries)
        return;
    vk.effect_query_frame = (int)vk.effect_query_next;
    vk.effect_query_next ^= 1u;
    vkCmdResetQueryPool(vk.commands, vk.effect_queries, (uint32_t)vk.effect_query_frame, 1);
}

/* Around the layer's draws: the count, and the layer's pixels over the game's screen (view, at the layer's half size) */
static void effect_count_begin(const VkViewport *view)
{
    if (!vk.effect_queries || vk.effect_query_frame < 0)
        return;
    vk.effect_screen_frame = (uint64_t)(view->width * 0.5f + 0.5f) * (uint64_t)(view->height * 0.5f + 0.5f);
    vkCmdBeginQuery(vk.commands, vk.effect_queries, (uint32_t)vk.effect_query_frame, vk.effect_query_flags);
}

static void effect_count_end(void)
{
    if (!vk.effect_queries || vk.effect_query_frame < 0)
        return;
    vkCmdEndQuery(vk.commands, vk.effect_queries, (uint32_t)vk.effect_query_frame);
    vk.effect_query_last = vk.effect_query_frame;
    vk.effect_screen_last = vk.effect_screen_frame;
    vk.effect_query_frame = -1;
}

/* [acct2-bro] 2026-11-22 Camo lane: the camo pass's pipelines (ngl_vk.h camo_texture; draw_camo draws with them).
 * The mesh pipelines' layout and corners, camo.vert and camo.frag; one for each culling the mesh draws use (none,
 * then the front and the back faces dropped, as mesh_cull_pipeline[0] and [1]), so the pass covers the faces its
 * draw drew and no others. Depth tested and not written, pulled toward the eye as the multiplied decals are (so it
 * lands on exactly the pixels its own draw just left, where something nearer hasn't covered them); blended as
 * DST_COLOR x out + SRC_COLOR x behind = the picture x 2 x out: camo.frag's factor (0 to 2) over 2 multiplies what
 * the draw left; red, green and blue only. With MSAA (msaa_finish kept it) each also gets a multisampled twin for the
 * first half. What isn't made is left out: no camo is drawn there (said once on stderr); nothing else uses them. */
/* [rexglue-native thread2] 2026-11-24: msaa_only 1: only the multisampled twins, for the one-sample ones made
 * already (ngl_vk_set_target_size, after MSAA stepped down to fewer samples and a new multisampled pass) */
static void make_camo_pipelines(int msaa_only)
{
    VkShaderModule vertex_shader = VK_NULL_HANDLE, fragment_shader = VK_NULL_HANDLE;
    VkPipelineShaderStageCreateInfo stages[2] = {
        { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO },
        { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO },
    };
    VkVertexInputBindingDescription binding = { 0, CORNER_FLOATS * sizeof(float), VK_VERTEX_INPUT_RATE_VERTEX };
    VkVertexInputAttributeDescription attributes[2] = {
        { 0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0 },                   /* position, as the mesh pipelines read it */
        { 1, 0, VK_FORMAT_R32G32_SFLOAT, 3 * sizeof(float) },      /* the base texture's coordinates */
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
    static const VkCullModeFlags culls[3] = { VK_CULL_MODE_NONE, VK_CULL_MODE_FRONT_BIT, VK_CULL_MODE_BACK_BIT };
    int made = 0;

    if (!vk.device || !vk.mesh_layout || !vk.render_pass || (!msaa_only && vk.camo_pipeline[0]))
        return;
    if (make_shader(camo_vert_spirv, sizeof camo_vert_spirv, &vertex_shader) ||
        make_shader(camo_frag_spirv, sizeof camo_frag_spirv, &fragment_shader)) {
        fprintf(stderr, "[ngl_vk] camo: its shaders couldn't be made: no camo is drawn\n");
        if (vertex_shader)
            vkDestroyShaderModule(vk.device, vertex_shader, NULL);
        return;
    }
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vertex_shader;
    stages[0].pName = "main";
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fragment_shader;
    stages[1].pName = "main";
    vertex_input.vertexBindingDescriptionCount = 1;
    vertex_input.pVertexBindingDescriptions = &binding;
    vertex_input.vertexAttributeDescriptionCount = 2;
    vertex_input.pVertexAttributeDescriptions = attributes;
    assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    viewport.viewportCount = 1;
    viewport.scissorCount = 1;
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.frontFace = VK_FRONT_FACE_CLOCKWISE;     /* as the mesh pipelines */
    raster.lineWidth = 1.0f;
    raster.depthBiasEnable = VK_TRUE;               /* as the multiplied decals: toward the eye, no slope part */
    raster.depthBiasConstantFactor = -1.0f;
    raster.depthBiasSlopeFactor = 0.0f;
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    blend_attachment.blendEnable = VK_TRUE;
    blend_attachment.srcColorBlendFactor = VK_BLEND_FACTOR_DST_COLOR;
    blend_attachment.dstColorBlendFactor = VK_BLEND_FACTOR_SRC_COLOR;
    blend_attachment.colorBlendOp = VK_BLEND_OP_ADD;
    blend_attachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
    blend_attachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    blend_attachment.alphaBlendOp = VK_BLEND_OP_ADD;
    blend_attachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT;
    blend.attachmentCount = 1;
    blend.pAttachments = &blend_attachment;
    depth.depthTestEnable = VK_TRUE;
    depth.depthWriteEnable = VK_FALSE;
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
    info.subpass = 0;
    for (int k = 0; k < 3; k++) {
        raster.cullMode = culls[k];
        multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        info.renderPass = vk.render_pass;
        if (!msaa_only &&
            vkCreateGraphicsPipelines(vk.device, VK_NULL_HANDLE, 1, &info, NULL, &vk.camo_pipeline[k]) != VK_SUCCESS)
            vk.camo_pipeline[k] = VK_NULL_HANDLE;
        made += vk.camo_pipeline[k] != VK_NULL_HANDLE;
        if (vk.camo_pipeline[k] && vk.msaa_samples >= 2u && vk.msaa_opaque_pass) {
            multisample.rasterizationSamples = (VkSampleCountFlagBits)vk.msaa_samples;
            info.renderPass = vk.msaa_opaque_pass;
            if (vkCreateGraphicsPipelines(vk.device, VK_NULL_HANDLE, 1, &info, NULL, &vk.camo_msaa_pipeline[k]) !=
                VK_SUCCESS)
                vk.camo_msaa_pipeline[k] = VK_NULL_HANDLE;
        }
    }
    vkDestroyShaderModule(vk.device, vertex_shader, NULL);
    vkDestroyShaderModule(vk.device, fragment_shader, NULL);
    if (!msaa_only && made < 3)
        fprintf(stderr, "[ngl_vk] camo: %d of its 3 pipelines made: the draws without one get no camo\n", made);
}

static void make_camo(void)
{
    make_camo_pipelines(0);
}

uint32_t ngl_vk_camo_count(void)    /* [acct2-bro] 2026-11-22 Camo lane (ngl_vk.h) */
{
    return vk.camo_count;
}

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
    if (make_effect_layer())    /* [rexglue-native] 2026-11-10 */
        fprintf(stderr, "[ngl_vk] no effect layer: its draws go straight into the picture\n");
    if (make_heat())            /* [rexglue-native-fx] 2026-11-11 fx1 */
        fprintf(stderr, "[ngl_vk] no screen warp: no heat haze\n");
    if (make_glow())            /* [rexglue-native] 2026-11-10 */
        fprintf(stderr, "[ngl_vk] no sky glow: drawn without it\n");
    msaa_finish();          /* [acct2-bro] 2026-11-22 rn113: every twin made, or MSAA let go */
    make_effect_count();    /* [acct2-bro] 2026-11-22 rn114: the effect layer's samples, when asked */
    make_camo();            /* [acct2-bro] 2026-11-22 Camo lane: after msaa_finish (its twins need MSAA kept) */
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
    vk.anisotropy_feature = anisotropy_on_borrowed_device;     /* [acct2-bro] 2026-11-12: as its owner made it */
    vk.effect_query_flags = effect_precise_on_borrowed_device ? VK_QUERY_CONTROL_PRECISE_BIT : 0;  /* rn114 */
    /* [acct2-bro] 2026-11-23 c3camo v2: block textures on a borrowed device only when asked for and its owner turned
     * textureCompressionBC on (ngl_vk_set_block_textures), and the GPU samples BC7 */
    vk.block_textures = block_textures_asked && block_textures_on_borrowed_device &&
                        block_format_sampled(VK_FORMAT_BC7_UNORM_BLOCK);
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

/* [acct2-ocr] 2026-11-13: the window through a color table (ngl_vk_set_window_table: Big Red One's gamma ramp, the
 * game's brightness as the Xbox's display showed it). The finished target is drawn through the table with
 * ngl_vk_output's pipeline into an image of the target's own size, and record_window_copy blits that instead. */
static struct {
    int on;                         /* a table is set */
    uint32_t table[256];            /* red in the low byte, then green and blue */
    VkImage image;                  /* the target through the table: vk.width x vk.height, TARGET_FORMAT */
    VkDeviceMemory memory;
    VkImageView view;
    VkFramebuffer framebuffer;      /* image, for vk.output_pass */
    VkRenderPass pass;              /* the vk.output_pass the framebuffer was made for */
    uint32_t width, height;         /* image's size */
    int failed;                     /* its parts couldn't be made: the plain blit from then on */
} window_ramp;                      /* [acct2-ocr] */

static int window_ramp_record(void);   /* [acct2-ocr] below ngl_vk_output: 0 when window_ramp.image holds the frame */

void ngl_vk_set_window_table(const uint32_t *table)    /* [acct2-ocr] 2026-11-13 */
{
    window_ramp.on = table != NULL;
    if (table)
        memcpy(window_ramp.table, table, sizeof window_ramp.table);
}

/* [acct2-fh] 2026-11-14 How the finished target is scaled into the window (ngl_vk.h ngl_vk_set_window_scaling): a
 * setting, kept across ngl_vk_shutdown. NGL_VK_SCALE_SMOOTH, the default, is the window copy as it always was. */
static int window_scaling = NGL_VK_SCALE_SMOOTH;
static struct {
    uint32_t from_width, from_height;
    int32_t x, y, width, height;
    int scaling, passes, ramp;
    uint32_t window_width, window_height;   /* [rexglue-native thread2] 2026-11-24: the swapchain's size */
} window_copy_said;
/* [rexglue-native thread2] 2026-11-24: the warning that a sized target is larger than the window has been given */
static int window_too_small_said;

void ngl_vk_set_window_scaling(int scaling)
{
    window_scaling = scaling == NGL_VK_SCALE_SHARP || scaling == NGL_VK_SCALE_INTEGER || scaling == NGL_VK_SCALE_CAS ||
                     scaling == NGL_VK_SCALE_FSR ? scaling : NGL_VK_SCALE_SMOOTH;
}

/* [acct2-fh] 2026-11-14 The window through AMD FidelityFX CAS or FSR 1 (ngl_vk.h NGL_VK_SCALE_CAS / NGL_VK_SCALE_FSR;
 * shaders/window_cas.frag, window_easu.frag, window_rcas.frag): drawn into one of two images the size of the
 * picture's place in the window, which record_window_copy then copies in one to one. The pass, its pipelines and sets
 * are made the first time they are used, the images again when that size changes; all freed by ngl_vk_shutdown (the
 * three settings stay). With a color table (ngl_vk_set_window_table) the table is drawn first, into window_ramp's
 * image, and CAS / FSR 1 read that (11-14 stage 5c). */
static struct {
    float cas_sharpness;            /* 0..1, CAS's sharpness (ngl_vk_set_window_sharpness) */
    float fsr_reduction;            /* 0..2, the stops FSR's RCAS is less sharp by */
    int fsr_passes;                 /* 1..8, FSR's EASU passes at most, each up to twice the size */
    VkRenderPass pass;              /* one TARGET_FORMAT image, in and out in COLOR_ATTACHMENT_OPTIMAL */
    VkPipelineLayout layout;        /* set 0 the picture read, 16 bytes of push constants */
    VkPipeline cas, easu, rcas;
    VkDescriptorSet target_set;     /* vk.target_view (target_view: the view written into it) */
    VkImageView target_view;
    VkDescriptorSet ramp_set;       /* window_ramp.view, the picture through the window's color table (ramp_view) */
    VkImageView ramp_view;
    VkDescriptorSet image_set[2];   /* image[i] read */
    VkImage image[2];
    VkDeviceMemory memory[2];
    VkImageView view[2];
    VkFramebuffer framebuffer[2];
    uint32_t width, height;         /* both images' size */
    int failed;                     /* couldn't be made: smooth from then on, until ngl_vk_shutdown */
} window_fx = { 0.0f, 0.2f, 1 };

void ngl_vk_set_window_sharpness(float cas_sharpness, float fsr_sharpness_reduction, int fsr_passes)
{
    window_fx.cas_sharpness = !(cas_sharpness > 0.0f) ? 0.0f : cas_sharpness > 1.0f ? 1.0f : cas_sharpness;
    window_fx.fsr_reduction = !(fsr_sharpness_reduction > 0.0f) ? 0.0f
                              : fsr_sharpness_reduction > 2.0f   ? 2.0f
                                                                 : fsr_sharpness_reduction;
    window_fx.fsr_passes = fsr_passes < 1 ? 1 : fsr_passes > 8 ? 8 : fsr_passes;   /* 8: 256 times, 64 to 16384 */
}

/* below window_ramp_record: the picture through CAS (passes 0) or FSR 1 (that many EASU passes, then RCAS) into one
 * of window_fx's images at width x height; returns which (left in TRANSFER_SRC_OPTIMAL), or -1 */
static int window_fx_record(uint32_t width, uint32_t height, int passes, int from_ramp);

/* The finished target (in TRANSFER_SRC_OPTIMAL) into the acquired image: black, then the target scaled to fit and
 * centered, then ready to be shown. [acct2-fh] With NGL_VK_SCALE_INTEGER the scale is the largest whole number that
 * fits (a target bigger than the window is still shrunk to fit); SHARP and INTEGER copy each pixel as it is; CAS and
 * FSR draw the picture at its place's size first (window_fx_record) and that is copied in as it is. FSR scales up
 * only: a picture not smaller than its place goes through CAS. */
static void record_window_copy(uint32_t index)
{
    VkImage image = vk.swap_images[index];
    VkClearColorValue black = { { 0.0f, 0.0f, 0.0f, 1.0f } };
    VkImageSubresourceRange range = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    VkImageBlit blit = { { 0 } };
    /* [rexglue-native thread2] 2026-11-24: the place worked out in whole pixels (no float rounding), so a target
     * whose fitted size is its own size is known exactly: then it takes the same-size path (a nearest blit of equal
     * extents: each pixel as it is) at a whole-pixel offset, e.g. 1440x1080 into 1920x1080 at x 240. Before, that
     * case was a linear blit at 1:1, which gives the same pixels; for every port the result is unchanged. */
    const uint32_t window_width = vk.swap_extent.width, window_height = vk.swap_extent.height;
    const int fits = vk.width <= window_width && vk.height <= window_height;
    int32_t width, height, x, y;
    int32_t src_x = 0, src_y = 0;   /* [rexglue-native thread2] 2026-11-24: the crop's corner in the target */
    int same_size, one_to_one, filtered, passes = 0, fx = -1, crop = 0;
    VkFilter filter;

    if (window_scaling == NGL_VK_SCALE_INTEGER && fits) {   /* [acct2-fh] the largest whole multiple that fits */
        uint32_t times_x = window_width / vk.width, times_y = window_height / vk.height;
        uint32_t times = times_x < times_y ? times_x : times_y;

        width = (int32_t)(vk.width * times);
        height = (int32_t)(vk.height * times);
    } else if ((uint64_t)window_width * vk.height <= (uint64_t)window_height * vk.width) {   /* the width limits */
        width = (int32_t)window_width;
        height = (int32_t)(((uint64_t)vk.height * window_width * 2u + vk.width) / (2u * (uint64_t)vk.width));
    } else {                                                                                 /* the height limits */
        height = (int32_t)window_height;
        width = (int32_t)(((uint64_t)vk.width * window_height * 2u + vk.height) / (2u * (uint64_t)vk.height));
    }
    same_size = width == (int32_t)vk.width && height == (int32_t)vk.height;
    /* (owner 07:48: no upscaling or downscaling) once the port sizes the target itself (ngl_vk_set_target_size), a
     * target that fits the window is also copied one to one, centered with black bars, rather than scaled up; it is
     * scaled only when it is larger than the window (said once). Ports that never call it keep the scale to fit. */
    one_to_one = same_size || (target_size_asked && fits);
    if (one_to_one) {
        width = (int32_t)vk.width;
        height = (int32_t)vk.height;
    }
    /* [rexglue-native thread2] 2026-11-24 (coordinator 18:14Z, from the BRO run): a port that sizes the target itself
     * can have a frame where the swapchain was just remade smaller and its next ngl_vk_set_target_size hasn't run
     * yet. That frame shows the target's centre at its own size (a crop, nearest), never a rescale; the next frame
     * has the new target. An acquired image must be presented, and black would flash, so a crop it is. */
    if (target_size_asked && !fits) {
        crop = 1;
        one_to_one = 1;
        width = (int32_t)(vk.width < window_width ? vk.width : window_width);
        height = (int32_t)(vk.height < window_height ? vk.height : window_height);
        src_x = ((int32_t)vk.width - width) / 2;
        src_y = ((int32_t)vk.height - height) / 2;
    }
    filter = one_to_one || window_scaling == NGL_VK_SCALE_SHARP || window_scaling == NGL_VK_SCALE_INTEGER
                 ? VK_FILTER_NEAREST
                 : VK_FILTER_LINEAR;
    x = ((int32_t)window_width - width) / 2;
    y = ((int32_t)window_height - height) / 2;
    /* [acct2-fh] CAS or FSR 1 (window_fx): FSR when the picture is scaled up, with an EASU pass for each doubling
     * (at most fsr_passes); passes 0 is CAS. With a color table on (window_ramp) the table comes first, at the
     * picture's size, and CAS / FSR 1 work from its image (11-14 stage 5c; before, a table meant smooth). */
    filtered = (window_scaling == NGL_VK_SCALE_CAS || window_scaling == NGL_VK_SCALE_FSR) && !window_fx.failed &&
               width > 0 && height > 0 && !crop;   /* the crop's one frame goes as it is */

    if (target_size_asked && !fits) {   /* [rexglue-native thread2] 2026-11-24: the last resort, said once */
        if (!window_too_small_said)
            fprintf(stderr, "[ngl_vk] the target (%u x %u) is larger than the window (%u x %u): a transitional "
                    "frame shows its centre at its own size (a crop, not scaled) until the port's next "
                    "ngl_vk_set_target_size\n", vk.width, vk.height, vk.swap_extent.width, vk.swap_extent.height);
        window_too_small_said = 1;
    } else {
        window_too_small_said = 0;  /* (said again if it happens again) */
    }
    if (filtered && window_scaling == NGL_VK_SCALE_FSR)
        while (passes < window_fx.fsr_passes &&
               ((vk.width << passes) < (uint32_t)width || (vk.height << passes) < (uint32_t)height))
            passes++;
    if (vk.width != window_copy_said.from_width || vk.height != window_copy_said.from_height ||
        x != window_copy_said.x || y != window_copy_said.y || width != window_copy_said.width ||
        height != window_copy_said.height || window_scaling != window_copy_said.scaling ||
        passes != window_copy_said.passes || window_ramp.on != window_copy_said.ramp ||
        window_width != window_copy_said.window_width || window_height != window_copy_said.window_height) {
        /* [acct2-fh] 2026-11-14 said once for each new placement, so a run's log shows where the picture went */
        char how[96];

        if (passes)
            snprintf(how, sizeof how, "AMD FSR 1 (%d EASU pass%s, then RCAS)%s", passes, passes > 1 ? "es" : "",
                     window_ramp.on ? ", after the color table" : "");
        else if (filtered && window_ramp.on)
            snprintf(how, sizeof how, "%s, after the color table",
                     window_scaling == NGL_VK_SCALE_CAS ? "AMD CAS" : "AMD FSR 1 asked, not scaled up: AMD CAS");
        else
            snprintf(how, sizeof how, "%s",
                     crop ? "its centre, cropped at its own size (transitional)"
                     : one_to_one && !filtered ? "same size, exact (nearest)"   /* [rexglue-native thread2] */
                     : window_scaling == NGL_VK_SCALE_SHARP   ? "sharp"
                     : window_scaling == NGL_VK_SCALE_INTEGER ? "whole multiples, sharp"
                     : window_scaling == NGL_VK_SCALE_CAS && filtered ? "AMD CAS"
                     : window_scaling == NGL_VK_SCALE_FSR && filtered ? "AMD FSR 1 asked, not scaled up: AMD CAS"
                                                                      : "smooth");
        fprintf(stderr, "[ngl_vk] the picture (%u x %u) shown at %d,%d as %d x %d in the window's %u x %u, %s\n",
                vk.width, vk.height, (int)x, (int)y, (int)width, (int)height, vk.swap_extent.width,
                vk.swap_extent.height, how);
        window_copy_said.from_width = vk.width;
        window_copy_said.from_height = vk.height;
        window_copy_said.x = x;
        window_copy_said.y = y;
        window_copy_said.width = width;
        window_copy_said.height = height;
        window_copy_said.scaling = window_scaling;
        window_copy_said.passes = passes;
        window_copy_said.ramp = window_ramp.on;
        window_copy_said.window_width = window_width;      /* [rexglue-native thread2]: and again after a */
        window_copy_said.window_height = window_height;    /* new swapchain */
    }
    image_barrier(image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
                  VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    vkCmdClearColorImage(vk.commands, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &range);
    image_barrier(image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                  VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                  VK_PIPELINE_STAGE_TRANSFER_BIT);
    if (filtered)                                                                  /* [acct2-fh] 2026-11-14 */
        fx = window_fx_record((uint32_t)width, (uint32_t)height, passes,
                              window_ramp.on && !window_ramp.failed && window_ramp_record() == 0);
    if (fx >= 0) {                                                                 /* [acct2-fh] as it is */
        blit.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        blit.srcSubresource.layerCount = 1;
        blit.srcOffsets[1].x = width;
        blit.srcOffsets[1].y = height;
        blit.srcOffsets[1].z = 1;
        blit.dstSubresource = blit.srcSubresource;
        blit.dstOffsets[0].x = x;
        blit.dstOffsets[0].y = y;
        blit.dstOffsets[1].x = x + width;
        blit.dstOffsets[1].y = y + height;
        blit.dstOffsets[1].z = 1;
        vkCmdBlitImage(vk.commands, window_fx.image[fx], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, image,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_NEAREST);
        image_barrier(image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                      VK_ACCESS_TRANSFER_WRITE_BIT, 0, VK_PIPELINE_STAGE_TRANSFER_BIT,
                      VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
        return;
    }
    if (window_ramp.on && !window_ramp.failed && window_ramp_record() == 0) {     /* [acct2-ocr] 2026-11-13 */
        blit.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;                /* [acct2-ocr] as below, from */
        blit.srcSubresource.layerCount = 1;                                        /* the target through the */
        blit.srcOffsets[0].x = src_x;                                              /* window's color table */
        blit.srcOffsets[0].y = src_y;                                              /* (a crop: its centre) */
        blit.srcOffsets[1].x = crop ? src_x + width : (int32_t)vk.width;
        blit.srcOffsets[1].y = crop ? src_y + height : (int32_t)vk.height;
        blit.srcOffsets[1].z = 1;
        blit.dstSubresource = blit.srcSubresource;
        blit.dstOffsets[0].x = x;
        blit.dstOffsets[0].y = y;
        blit.dstOffsets[1].x = x + width;
        blit.dstOffsets[1].y = y + height;
        blit.dstOffsets[1].z = 1;
        vkCmdBlitImage(vk.commands, window_ramp.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, image,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, filter);     /* [acct2-fh] was LINEAR */
        image_barrier(image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                      VK_ACCESS_TRANSFER_WRITE_BIT, 0, VK_PIPELINE_STAGE_TRANSFER_BIT,
                      VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
        return;                                                                    /* [acct2-ocr] */
    }
    blit.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    blit.srcSubresource.layerCount = 1;
    blit.srcOffsets[0].x = src_x;     /* [rexglue-native thread2] 2026-11-24: a crop's corner, else 0 */
    blit.srcOffsets[0].y = src_y;
    blit.srcOffsets[1].x = crop ? src_x + width : (int32_t)vk.width;
    blit.srcOffsets[1].y = crop ? src_y + height : (int32_t)vk.height;
    blit.srcOffsets[1].z = 1;
    blit.dstSubresource = blit.srcSubresource;
    blit.dstOffsets[0].x = x;
    blit.dstOffsets[0].y = y;
    blit.dstOffsets[1].x = x + width;
    blit.dstOffsets[1].y = y + height;
    blit.dstOffsets[1].z = 1;
    vkCmdBlitImage(vk.commands, vk.target, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, image,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, filter);             /* [acct2-fh] was LINEAR */
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

/* ---------------------------------------------------------------------------------------------------------------
 * [rexglue-native thread2] 2026-11-24: depth probes (ngl_vk.h ngl_vk_probe_depth). The texels are copied where
 * copy_soft_depth copies the whole depth (the image already in its transfer layout, after the opaque half, its
 * barriers covering the copy), into a host-cached buffer of two slots: the frame being recorded takes one, the
 * frame on the GPU may hold the other. A slot's depths are read once its frame's fence has signalled: in
 * finish_frame, which waits for it anyway before the next draw records, or in ngl_vk_probe_results when the fence
 * is already signalled (asked without waiting). The depth is D32_SFLOAT (DEPTH_FORMAT), so a texel's copy is the
 * float the depth test compared, 4 bytes, no unpacking. */
enum { PROBE_FREE, PROBE_RECORDED, PROBE_SUBMITTED };

static void probe_free(void)
{
    if (vk.device && vk.probe_memory && vk.probe_mapped)
        vkUnmapMemory(vk.device, vk.probe_memory);
    if (vk.device && vk.probe_buffer)
        vkDestroyBuffer(vk.device, vk.probe_buffer, NULL);
    if (vk.device && vk.probe_memory)
        vkFreeMemory(vk.device, vk.probe_memory, NULL);
    vk.probe_buffer = VK_NULL_HANDLE;
    vk.probe_memory = VK_NULL_HANDLE;
    vk.probe_mapped = NULL;
    vk.probe_slot_state[0] = vk.probe_slot_state[1] = PROBE_FREE;
    vk.probe_count = 0;
}

/* Once a frame's fence has signalled: its slot's depths become the newest results (the older slot first, so the
 * newer one wins if both are in) */
static void probe_publish(void)
{
    int order[2] = { 0, 1 };

    if (!vk.probe_mapped)
        return;
    if (vk.probe_slot_frame[1] < vk.probe_slot_frame[0]) {
        order[0] = 1;
        order[1] = 0;
    }
    for (int k = 0; k < 2; k++) {
        const int slot = order[k];
        const float *texels = vk.probe_mapped + (size_t)slot * NGL_VK_DEPTH_PROBES;

        if (vk.probe_slot_state[slot] != PROBE_SUBMITTED)
            continue;
        for (uint32_t i = 0; i < vk.probe_slot_count[slot]; i++)
            vk.probe_depths[i] = vk.probe_slot_on_target[slot][i] ? texels[i] : -1.0f;
        vk.probe_depth_count = vk.probe_slot_count[slot];
        vk.probe_depth_frame = vk.probe_slot_frame[slot];
        vk.probe_slot_state[slot] = PROBE_FREE;
    }
}

/* In copy_soft_depth (the depth in VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL): this draw's points, placed through the
 * meshes' viewport (view), each texel copied into a free slot. A point's pixel is the one its place falls in, as
 * the rasterizer samples a pixel at its center: (x, y) = the viewport's corner + (ndc x + 1) / 2 x its width and
 * (1 - ndc y) / 2 x its height (Direct3D's y up, the target's rows down, as mesh.vert turns y over). */
static void probe_record(const VkViewport *view)
{
    VkBufferImageCopy regions[NGL_VK_DEPTH_PROBES];
    VkBufferMemoryBarrier to_host = { VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER };
    uint32_t copies = 0, slot;
    void *mapped = NULL;

    if (!vk.probe_count || vk.probe_frame != vk.frames_done + 1)
        return;
    if (!vk.probe_buffer) {
        if (make_host_buffer_cached(2u * NGL_VK_DEPTH_PROBES * sizeof(float), VK_BUFFER_USAGE_TRANSFER_DST_BIT, 1,
                                    &vk.probe_buffer, &vk.probe_memory, &mapped)) {
            fprintf(stderr, "[ngl_vk] no buffer for the depth probes: none are read\n");
            probe_free();
            return;
        }
        vk.probe_mapped = (float *)mapped;
    }
    /* a slot left recorded by a frame that was never submitted is free again; one on the GPU is not */
    for (slot = 0; slot < 2; slot++) {
        if (vk.probe_slot_state[slot] == PROBE_RECORDED)
            vk.probe_slot_state[slot] = PROBE_FREE;
    }
    slot = vk.probe_next_slot;
    if (vk.probe_slot_state[slot] != PROBE_FREE)
        slot ^= 1u;
    if (vk.probe_slot_state[slot] != PROBE_FREE)
        return;     /* (cannot happen: finish_frame publishes before the next draw records) */
    memset(regions, 0, sizeof regions);
    for (uint32_t i = 0; i < vk.probe_count; i++) {
        const float x = view->x + (vk.probe_points[i][0] * 0.5f + 0.5f) * view->width;
        const float y = view->y + (0.5f - vk.probe_points[i][1] * 0.5f) * view->height;
        /* (written this way round, a NaN place is off the target too) */
        const int on = x >= 0.0f && y >= 0.0f && x < (float)vk.width && y < (float)vk.height;

        vk.probe_slot_on_target[slot][i] = (uint8_t)on;
        if (!on)
            continue;
        /* a depth aspect's buffer offset must be a multiple of 4: every float is */
        regions[copies].bufferOffset = ((VkDeviceSize)slot * NGL_VK_DEPTH_PROBES + i) * sizeof(float);
        regions[copies].imageSubresource.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
        regions[copies].imageSubresource.layerCount = 1;
        regions[copies].imageOffset.x = (int32_t)x;
        regions[copies].imageOffset.y = (int32_t)y;
        regions[copies].imageExtent.width = 1;
        regions[copies].imageExtent.height = 1;
        regions[copies].imageExtent.depth = 1;
        copies++;
    }
    if (copies) {
        vkCmdCopyImageToBuffer(vk.commands, vk.depth, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, vk.probe_buffer, copies,
                               regions);
        to_host.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        to_host.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        to_host.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        to_host.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        to_host.buffer = vk.probe_buffer;
        to_host.offset = (VkDeviceSize)slot * NGL_VK_DEPTH_PROBES * sizeof(float);
        to_host.size = NGL_VK_DEPTH_PROBES * sizeof(float);
        vkCmdPipelineBarrier(vk.commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 0, NULL, 1,
                             &to_host, 0, NULL);
    }
    vk.probe_slot_frame[slot] = vk.probe_frame;
    vk.probe_slot_count[slot] = vk.probe_count;
    vk.probe_slot_state[slot] = PROBE_RECORDED;
    vk.probe_next_slot = slot ^ 1u;
    vk.probe_count = 0;     /* this draw's alone */
}

/* After a frame's commands are submitted (submit_and_show): its recorded slot is on the GPU */
static void probe_submitted(void)
{
    for (int slot = 0; slot < 2; slot++) {
        if (vk.probe_slot_state[slot] == PROBE_RECORDED)
            vk.probe_slot_state[slot] = PROBE_SUBMITTED;
    }
    vk.probe_count = 0;     /* points given for a draw that took none (no meshes) are dropped with it */
}

uint64_t ngl_vk_probe_depth(const float (*points)[2], uint32_t count)
{
    if (!vk.device || !points || !count) {
        vk.probe_count = 0;
        return 0;
    }
    if (count > NGL_VK_DEPTH_PROBES)
        count = NGL_VK_DEPTH_PROBES;
    memcpy(vk.probe_points, points, (size_t)count * sizeof vk.probe_points[0]);
    vk.probe_count = count;
    vk.probe_frame = vk.frames_done + 1;    /* the next draw's (frames_done counts the frames finished) */
    return vk.probe_frame;
}

uint32_t ngl_vk_probe_results(float *depths, uint32_t max, uint64_t *frame)
{
    uint32_t count;

    /* the frame on the GPU, if it is done already: a zero timeout only asks */
    if (vk.device && vk.frame_pending && vk.probe_mapped &&
        (vk.probe_slot_state[0] == PROBE_SUBMITTED || vk.probe_slot_state[1] == PROBE_SUBMITTED) &&
        vkWaitForFences(vk.device, 1, &vk.done, VK_TRUE, 0) == VK_SUCCESS)
        probe_publish();
    count = vk.probe_depth_count < max ? vk.probe_depth_count : max;
    if (depths && count)
        memcpy(depths, vk.probe_depths, (size_t)count * sizeof depths[0]);
    if (frame)
        *frame = vk.probe_depth_frame;
    return depths ? count : 0;
}

/* Between the halves: the opaque meshes' depth (left in the transfer layout by the first half) copied for mesh.frag,
 * then back to a depth attachment for the second half's tests. [rexglue-native thread2] 2026-11-24: and the depth
 * probes' texels (probe_record), through the meshes' viewport (view). */
static void copy_soft_depth(const VkViewport *view)
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
    probe_record(view);     /* [rexglue-native thread2] 2026-11-24 (nothing unless points were given) */
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
    if (blended == 2)   /* [rexglue-native] 2026-11-10: the effect layer's blend back: its color as it is (already
                         * times its alpha) + what is there x (1 - its alpha); the target's own alpha kept */
        blend_attachment.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
    blend_attachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                      VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    if (blended == 4) { /* [rexglue-native] 2026-11-10: the sky glow: added to what is there; the target's alpha kept */
        blend_attachment.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
        blend_attachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
    }
    if (blended == 3) { /* [rexglue-native] 2026-11-10: the effect layer's depth fill: no color, the depth the shader
                         * writes, whatever is there */
        blend_attachment.blendEnable = VK_FALSE;
        blend_attachment.colorWriteMask = 0;
        no_depth.depthTestEnable = VK_TRUE;
        no_depth.depthWriteEnable = VK_TRUE;
        no_depth.depthCompareOp = VK_COMPARE_OP_ALWAYS;
    }
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

/* [rexglue-native thread2] 2026-11-24: depth of field's images, sized from the target (make_dof, and
 * ngl_vk_set_target_size): the half, the quarter and the blur's other quarter. [acct2-bro] 2026-11-12 rn61a: a
 * quarter of the shown picture (the target is shown_scale times it). */
static int make_dof_images(void)
{
    vk.dof_width = vk.width / (4u * shown_scale) ? vk.width / (4u * shown_scale) : 1u;
    vk.dof_height = vk.height / (4u * shown_scale) ? vk.height / (4u * shown_scale) : 1u;
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
                   &vk.dof_temp, &vk.dof_temp_memory, &vk.dof_temp_view))
        return -1;
    return 0;
}

/* [rexglue-native thread2] 2026-11-24: the blur pass's two framebuffers over the quarters (the pass made already) */
static int make_dof_framebuffers(void)
{
    VkFramebufferCreateInfo framebuffer_info = { VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };

    framebuffer_info.renderPass = vk.dof_blur_pass;
    framebuffer_info.attachmentCount = 1;
    framebuffer_info.width = vk.dof_width;
    framebuffer_info.height = vk.dof_height;
    framebuffer_info.layers = 1;
    framebuffer_info.pAttachments = &vk.dof_quarter_view;
    CHECK(vkCreateFramebuffer(vk.device, &framebuffer_info, NULL, &vk.dof_quarter_framebuffer));
    framebuffer_info.pAttachments = &vk.dof_temp_view;
    CHECK(vkCreateFramebuffer(vk.device, &framebuffer_info, NULL, &vk.dof_temp_framebuffer));
    return 0;
}

/* [rexglue-native thread2] 2026-11-24: the two blurred quarters' sets pointed at them */
static void dof_point(void)
{
    point_image(vk.dof_quarter_set, 0, vk.sampler, vk.dof_quarter_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    point_image(vk.dof_temp_set, 0, vk.sampler, vk.dof_temp_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}

/* Depth of field's images, passes, sets and pipelines (after the main pass, the textures' pool and the shadow's set,
 * whose binding 2, the opaque depth, the mix reads). */
static int make_dof(void)
{
    VkDescriptorSetLayout layouts[DOF_SETS] = { vk.set_layout, vk.set_layout };
    VkDescriptorSet sets[DOF_SETS] = { VK_NULL_HANDLE, VK_NULL_HANDLE };
    VkDescriptorSetAllocateInfo set_info = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
    /* [acct2-bro] 2026-11-13 rn64c: four floats (the step, the kind of blur, one not used), it was two */
    VkPushConstantRange blur_range = { VK_SHADER_STAGE_FRAGMENT_BIT, 0, 4 * sizeof(float) };
    /* [acct2-bro] 2026-11-13 rn64a: eight floats (dof_focus), it was four */
    VkPushConstantRange mix_range = { VK_SHADER_STAGE_FRAGMENT_BIT, 0, 8 * sizeof(float) };
    VkDescriptorSetLayout mix_layouts[2] = { vk.set_layout, vk.shadow_set_layout };
    VkPipelineLayoutCreateInfo layout_info = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };

    /* [rexglue-native thread2] 2026-11-24: the images, the passes, then the framebuffers, in the order they always
     * were made; the images and framebuffers are made the same way again by ngl_vk_set_target_size */
    if (make_dof_images() || make_dof_blur_pass() || make_overlay_pass() || make_dof_framebuffers())
        return -1;

    set_info.descriptorPool = vk.descriptor_pool;
    set_info.descriptorSetCount = DOF_SETS;
    set_info.pSetLayouts = layouts;
    CHECK(vkAllocateDescriptorSets(vk.device, &set_info, sets));
    vk.dof_quarter_set = sets[0];
    vk.dof_temp_set = sets[1];
    dof_point();

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
    vk.dof_focus[4] = 0.0f;     /* [acct2-bro] 2026-11-13 rn64a: no bands: the ramp */
    vk.dof_focus[5] = 0.0f;
    vk.dof_focus[6] = 0.0f;
    vk.dof_focus[7] = 0.0f;
}

/* [acct2-bro] 2026-11-13 rn64a: see ngl_vk.h */
void ngl_vk_set_depth_of_field_bands(float near_depth, float far_depth, float keep_depth, float blanket, float scale,
                                     float front_depth, float front_amount)
{
    ngl_vk_set_depth_of_field(near_depth, far_depth, keep_depth, blanket);
    vk.dof_focus[4] = scale > 0.0f ? scale : 0.0f;
    vk.dof_focus[5] = front_depth;
    vk.dof_focus[6] = front_amount < 0.0f ? 0.0f : front_amount > 1.0f ? 1.0f : front_amount;
}

/* [rexglue-native thread2] 2026-11-24: the tint's images, sized from the target (make_tint, and
 * ngl_vk_set_target_size): the last frame's picture and the target's copy (both needed: -1 without them), then the
 * afterimage's kept picture (want_after) and its quarter (want_small). At init (must 0) those two may be missing (no
 * afterimage, or the full-size one: as before); at a resize (must 1) the ones the init made must be made again, else
 * -1 and the old ones stay. *after_made: the kept picture was made. */
static int make_post_images(int want_after, int want_small, int must, int *after_made)
{
    *after_made = 0;
    if (make_image(vk.width, vk.height, 1, TARGET_FORMAT, VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                   &vk.post_history, &vk.post_history_memory, &vk.post_history_view))
        return -1;
    if (make_image(vk.width, vk.height, 1, TARGET_FORMAT, VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                   &vk.post_copy, &vk.post_copy_memory, &vk.post_copy_view))
        return -1;
    if (want_after) {
        if (!make_image(vk.width, vk.height, 1, TARGET_FORMAT,
                        VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, &vk.after_image,
                        &vk.after_memory, &vk.after_view))
            *after_made = 1;
        else if (must)
            return -1;
    }
    /* [rexglue-native] 2026-11-11 rn48b: the afterimage's kept picture at a quarter size; without it the full-size one is used */
    /* [acct2-bro] 2026-11-12 rn61a: of the shown picture, as the depth of field's quarter */
    vk.after_small_width = vk.width / (4u * shown_scale) ? vk.width / (4u * shown_scale) : 1u;
    vk.after_small_height = vk.height / (4u * shown_scale) ? vk.height / (4u * shown_scale) : 1u;
    /* [rexglue-native] 2026-11-11 rn48c: a blit source too (stretched back into after_image); no set of its own */
    if (want_small && make_image(vk.after_small_width, vk.after_small_height, 1, TARGET_FORMAT,
                                 VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                     VK_IMAGE_USAGE_SAMPLED_BIT,
                                 &vk.after_small_image, &vk.after_small_memory, &vk.after_small_view)) {
        if (must)
            return -1;
        vk.after_small_image = VK_NULL_HANDLE;
    }
    return 0;
}

/* [rexglue-native thread2] 2026-11-24: the tint's sets pointed at its images (the sets not made are left out) */
static void post_point(void)
{
    point_image(vk.post_copy_set, 0, vk.sampler, vk.post_copy_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    point_image(vk.post_history_set, 0, vk.sampler, vk.post_history_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    point_image(vk.after_set, 0, vk.sampler, vk.after_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}

/* The tint's copy of the target (sampled through set 0) and its pipeline, drawn in the overlay pass. */
static int make_tint(void)
{
    VkDescriptorSetAllocateInfo set_info = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
    VkPushConstantRange range = { VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof vk.tint };  /* rn41b: (was 12 floats) */
    /* the target's copy, the opaque depth, the last frame's picture, the color look-up's table */
    VkDescriptorSetLayout layouts[4] = { vk.set_layout, vk.shadow_set_layout, vk.set_layout, vk.set_layout };
    VkPipelineLayoutCreateInfo layout_info = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };

    int after_made = 0;

    if (!vk.overlay_pass || !vk.shadow_set_layout)
        return -1;
    /* [rexglue-native thread2] 2026-11-24: the images first (make_post_images, as ngl_vk_set_target_size makes them
     * again), then their sets, pointed by post_point */
    if (make_post_images(1, 1, 0, &after_made))
        return -1;
    set_info.descriptorPool = vk.descriptor_pool;
    set_info.descriptorSetCount = 1;
    set_info.pSetLayouts = &vk.set_layout;
    CHECK(vkAllocateDescriptorSets(vk.device, &set_info, &vk.post_copy_set));
    CHECK(vkAllocateDescriptorSets(vk.device, &set_info, &vk.post_history_set));
    /* [rexglue-native] 2026-11-11 rn47g: the afterimage's kept picture (ngl_vk_set_afterimage); without it (no
     * memory) the afterimage is left out (draw: after) */
    if (after_made && vkAllocateDescriptorSets(vk.device, &set_info, &vk.after_set) != VK_SUCCESS)
        vk.after_set = VK_NULL_HANDLE;
    post_point();

    layout_info.setLayoutCount = 4;
    layout_info.pSetLayouts = layouts;
    layout_info.pushConstantRangeCount = 1;
    layout_info.pPushConstantRanges = &range;
    CHECK(vkCreatePipelineLayout(vk.device, &layout_info, NULL, &vk.tint_layout));
    return make_fullscreen_pipeline(tint_frag_spirv, sizeof tint_frag_spirv, vk.tint_layout, vk.render_pass, 0,
                                    &vk.tint_pipeline);
}

/* [rexglue-native] 2026-11-10: one of the effect layer's two passes (make_effect_layer). fill 1: the color cleared, the
 * depth written whole by the fill draw and left to be read; fill 0: both loaded, the depth read only (tested and
 * sampled at once), the color left to be sampled by the blend back. Both are compatible with the main pass (the same
 * formats and dependencies), so the mesh pipelines serve the second. */
static int make_effect_pass(int fill, VkRenderPass *pass)
{
    VkAttachmentDescription attachments[2] = { { 0 }, { 0 } };
    VkAttachmentReference color = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
    VkAttachmentReference depth = { 1, fill ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL
                                           : VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL };
    VkSubpassDescription subpass = { 0 };
    VkSubpassDependency dependencies[2];
    VkRenderPassCreateInfo pass_info = { VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };

    attachments[0].format = TARGET_FORMAT;
    attachments[0].samples = VK_SAMPLE_COUNT_1_BIT;
    attachments[0].loadOp = fill ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
    attachments[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachments[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachments[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[0].initialLayout = fill ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    attachments[0].finalLayout = fill ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL
                                      : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    attachments[1].format = DEPTH_FORMAT;
    attachments[1].samples = VK_SAMPLE_COUNT_1_BIT;
    attachments[1].loadOp = fill ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
    attachments[1].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachments[1].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachments[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[1].initialLayout = fill ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
    attachments[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &color;
    subpass.pDepthStencilAttachment = &depth;
    target_pass_dependencies(dependencies);     /* the main pass's own set, so the passes stay compatible */
    pass_info.attachmentCount = 2;
    pass_info.pAttachments = attachments;
    pass_info.subpassCount = 1;
    pass_info.pSubpasses = &subpass;
    pass_info.dependencyCount = 2;
    pass_info.pDependencies = dependencies;
    CHECK(vkCreateRenderPass(vk.device, &pass_info, NULL, pass));
    return 0;
}

/* [rexglue-native thread2] 2026-11-24: the effect layer's images, half the target's size (make_effect_layer, and
 * ngl_vk_set_target_size) */
static int make_effect_images(void)
{
    vk.effect_width = (vk.width + 1u) / 2u;
    vk.effect_height = (vk.height + 1u) / 2u;
    if (make_image(vk.effect_width, vk.effect_height, 1, TARGET_FORMAT,
                   VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                   &vk.effect_color, &vk.effect_color_memory, &vk.effect_color_view) ||
        make_image(vk.effect_width, vk.effect_height, 1, DEPTH_FORMAT,
                   VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                   &vk.effect_depth, &vk.effect_depth_memory, &vk.effect_depth_view))
        return -1;
    return 0;
}

/* [rexglue-native thread2] 2026-11-24: the layer's framebuffer (its color and depth, for both of its passes) */
static int make_effect_framebuffer(void)
{
    VkImageView views[2];
    VkFramebufferCreateInfo framebuffer_info = { VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };

    views[0] = vk.effect_color_view;
    views[1] = vk.effect_depth_view;
    framebuffer_info.renderPass = vk.effect_fill_pass;
    framebuffer_info.attachmentCount = 2;
    framebuffer_info.pAttachments = views;
    framebuffer_info.width = vk.effect_width;
    framebuffer_info.height = vk.effect_height;
    framebuffer_info.layers = 1;
    CHECK(vkCreateFramebuffer(vk.device, &framebuffer_info, NULL, &vk.effect_framebuffer));
    return 0;
}

/* [rexglue-native thread2] 2026-11-24: the layer's images in its sets: its color for the blend back (unfiltered:
 * fetched texel by texel), its depth in the layer's shadow set's binding 2 (the opaque depth's place) */
static void effect_point(void)
{
    point_image(vk.effect_color_set, 0, vk.soft_sampler, vk.effect_color_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    point_image(vk.effect_shadow_set, 2, vk.soft_sampler, vk.effect_depth_view,
                VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL);
}

/* [rexglue-native] 2026-11-10: the effect layer (ngl_vk.h effect_layer): its two images, passes, framebuffer, sets and
 * pipelines. After make_dof (the overlay pass and the mix's layout, which the fill and the blend back use: set 0 a
 * texture, set 1 the shadow set's layout) and the shadow's set. Returns 0 when made (effect_ready 1); when something
 * can't be made the layer stays off and its draws go into the main target, as before. */
static int make_effect_layer(void)
{
    VkDescriptorPoolSize pool_sizes[3] = {
        { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 3 },   /* the layer's color; the shadow map and the layer's depth */
        { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1 },
        { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1 },
    };
    VkDescriptorPoolCreateInfo pool_info = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
    VkDescriptorSetLayout layouts[2] = { vk.set_layout, vk.shadow_set_layout };
    VkDescriptorSet sets[2] = { VK_NULL_HANDLE, VK_NULL_HANDLE };
    VkDescriptorSetAllocateInfo set_info = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
    VkDescriptorImageInfo shadow_info = { VK_NULL_HANDLE, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
    VkDescriptorBufferInfo buffer_info = { VK_NULL_HANDLE, 0, VK_WHOLE_SIZE };
    VkDescriptorBufferInfo lights_info = { VK_NULL_HANDLE, 0, VK_WHOLE_SIZE };
    VkWriteDescriptorSet writes[3] = {
        { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET }, { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET },
        { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET },
    };

    vk.effect_ready = 0;
    if (!vk.overlay_pass || !vk.dof_mix_layout || !vk.shadow_set_layout || !vk.shadow_view || !vk.soft_depth ||
        !vk.blend_pass || !vk.shadow_uniform || !vk.draw_lights)
        return -1;
    /* [rexglue-native thread2] 2026-11-24: the images, the passes, then the framebuffer, in the order they always
     * were made; the images and the framebuffer are made the same way again by ngl_vk_set_target_size */
    if (make_effect_images() || make_effect_pass(1, &vk.effect_fill_pass) || make_effect_pass(0, &vk.effect_pass) ||
        make_effect_framebuffer())
        return -1;

    pool_info.maxSets = 2;
    pool_info.poolSizeCount = 3;
    pool_info.pPoolSizes = pool_sizes;
    CHECK(vkCreateDescriptorPool(vk.device, &pool_info, NULL, &vk.effect_pool));
    set_info.descriptorPool = vk.effect_pool;
    set_info.descriptorSetCount = 2;
    set_info.pSetLayouts = layouts;
    CHECK(vkAllocateDescriptorSets(vk.device, &set_info, sets));
    vk.effect_color_set = sets[0];
    vk.effect_shadow_set = sets[1];
    /* the shadow set over again (make_shadow), but for binding 2: the layer's depth in the opaque depth's place
     * (effect_point, with the layer's color) */
    shadow_info.sampler = vk.shadow_sampler;
    shadow_info.imageView = vk.shadow_view;
    writes[0].dstSet = vk.effect_shadow_set;
    writes[0].dstBinding = 0;
    writes[0].descriptorCount = 1;
    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[0].pImageInfo = &shadow_info;
    buffer_info.buffer = vk.shadow_uniform;
    writes[1].dstSet = vk.effect_shadow_set;
    writes[1].dstBinding = 1;
    writes[1].descriptorCount = 1;
    writes[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    writes[1].pBufferInfo = &buffer_info;
    lights_info.buffer = vk.draw_lights;
    writes[2].dstSet = vk.effect_shadow_set;
    writes[2].dstBinding = 3;
    writes[2].descriptorCount = 1;
    writes[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[2].pBufferInfo = &lights_info;
    vkUpdateDescriptorSets(vk.device, 3, writes, 0, NULL);
    effect_point();

    if (make_fullscreen_pipeline(effect_depth_frag_spirv, sizeof effect_depth_frag_spirv, vk.dof_mix_layout,
                                 vk.effect_fill_pass, 3, &vk.effect_fill_pipeline) ||
        make_fullscreen_pipeline(effect_back_frag_spirv, sizeof effect_back_frag_spirv, vk.dof_mix_layout,
                                 vk.render_pass, 2, &vk.effect_back_pipeline))
        return -1;
    vk.effect_ready = 1;
    return 0;
}

/* [rexglue-native] 2026-11-10: the sky glow's two small targets, their sets and its three pipelines (the struct's note
 * has the whole of it). After make_dof (its blur pass serves: one color attachment left ready to sample) and make_tint
 * (the target's copy is what the first pass reads). A pool and a layout of its own (a sampled picture, the shadow set,
 * eight floats). */
static int make_glow(void)
{
    VkPushConstantRange range = { VK_SHADER_STAGE_FRAGMENT_BIT, 0, 8 * sizeof(float) };
    VkDescriptorSetLayout set_layouts[2] = { vk.set_layout, vk.shadow_set_layout };
    VkPipelineLayoutCreateInfo layout_info = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    VkFramebufferCreateInfo framebuffer_info = { VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
    VkDescriptorPoolSize pool_size = { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2 };
    VkDescriptorPoolCreateInfo pool_info = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
    VkDescriptorSetLayout layouts[2] = { vk.set_layout, vk.set_layout };
    VkDescriptorSetAllocateInfo set_info = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
    VkDescriptorImageInfo images[2] = { { 0 }, { 0 } };
    VkWriteDescriptorSet writes[2] = { { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET },
                                       { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET } };
    VkSamplerCreateInfo sampler_info = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };

    vk.glow_ready = 0;
    if (!vk.overlay_pass || !vk.dof_blur_pass || !vk.shadow_set_layout || !vk.tint_pipeline ||
        !vk.post_copy_set || !vk.shadow_set || !vk.soft_depth)
        return -1;
    layout_info.setLayoutCount = 2;
    layout_info.pSetLayouts = set_layouts;
    layout_info.pushConstantRangeCount = 1;
    layout_info.pPushConstantRanges = &range;
    CHECK(vkCreatePipelineLayout(vk.device, &layout_info, NULL, &vk.glow_layout));
    framebuffer_info.renderPass = vk.dof_blur_pass;
    framebuffer_info.attachmentCount = 1;
    framebuffer_info.width = GLOW_SIZE;
    framebuffer_info.height = GLOW_SIZE;
    framebuffer_info.layers = 1;
    for (uint32_t i = 0; i < 2; i++) {
        if (make_image(GLOW_SIZE, GLOW_SIZE, 1, TARGET_FORMAT,
                       VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, &vk.glow_image[i],
                       &vk.glow_memory[i], &vk.glow_view[i]))
            return -1;
        framebuffer_info.pAttachments = &vk.glow_view[i];
        CHECK(vkCreateFramebuffer(vk.device, &framebuffer_info, NULL, &vk.glow_framebuffer[i]));
    }
    pool_info.maxSets = 2;
    pool_info.poolSizeCount = 1;
    pool_info.pPoolSizes = &pool_size;
    CHECK(vkCreateDescriptorPool(vk.device, &pool_info, NULL, &vk.glow_pool));
    set_info.descriptorPool = vk.glow_pool;
    set_info.descriptorSetCount = 2;
    set_info.pSetLayouts = layouts;
    CHECK(vkAllocateDescriptorSets(vk.device, &set_info, vk.glow_set));
    /* The game sets its glow's four samplers to the border mode with a black border (0x820D1A10: address mode 6 for
     * both ways, border color 0), so a blur's tap past the target's edge fetches black: the glow fades toward the
     * picture's edges, and a lean toward a sun outside the picture takes less light, not the edge's sky again. */
    sampler_info.magFilter = VK_FILTER_LINEAR;
    sampler_info.minFilter = VK_FILTER_LINEAR;
    sampler_info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sampler_info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    sampler_info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    sampler_info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    sampler_info.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
    CHECK(vkCreateSampler(vk.device, &sampler_info, NULL, &vk.glow_sampler));
    for (uint32_t i = 0; i < 2; i++) {
        images[i].sampler = vk.glow_sampler;
        images[i].imageView = vk.glow_view[i];
        images[i].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        writes[i].dstSet = vk.glow_set[i];
        writes[i].dstBinding = 0;
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[i].pImageInfo = &images[i];
    }
    vkUpdateDescriptorSets(vk.device, 2, writes, 0, NULL);
    if (make_fullscreen_pipeline(glow_first_frag_spirv, sizeof glow_first_frag_spirv, vk.glow_layout,
                                 vk.dof_blur_pass, 0, &vk.glow_first_pipeline) ||
        make_fullscreen_pipeline(glow_blur_frag_spirv, sizeof glow_blur_frag_spirv, vk.glow_layout,
                                 vk.dof_blur_pass, 0, &vk.glow_blur_pipeline) ||
        make_fullscreen_pipeline(glow_add_frag_spirv, sizeof glow_add_frag_spirv, vk.glow_layout, vk.render_pass,
                                 4, &vk.glow_add_pipeline))
        return -1;
    vk.glow_ready = 1;
    return 0;
}

void ngl_vk_set_glow(float tap, float gain, uint32_t passes, float strength, float lean, const float toward[2])
{
    vk.glow_on = strength > 0.0f;
    vk.glow_tap = tap;
    vk.glow_gain = gain;
    vk.glow_passes = passes < GLOW_MOST_BLURS ? passes : GLOW_MOST_BLURS;
    vk.glow_strength = strength;
    vk.glow_lean = lean;
    vk.glow_toward[0] = toward ? toward[0] : 0.0f;
    vk.glow_toward[1] = toward ? toward[1] : 0.0f;
}

/* [cod2-native] 2026-11-24: the IW bloom's layout (the sky glow's two sets, 32 floats) and its two pipelines; the
 * targets, sets and adding are the sky glow's (make_glow, before). */
static int make_bloom(void)
{
    VkPushConstantRange range = { VK_SHADER_STAGE_FRAGMENT_BIT, 0, 32 * sizeof(float) };
    VkDescriptorSetLayout set_layouts[2] = { vk.set_layout, vk.shadow_set_layout };
    VkPipelineLayoutCreateInfo layout_info = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };

    vk.bloom_ready = 0;
    if (!vk.glow_ready)
        return -1;
    layout_info.setLayoutCount = 2;
    layout_info.pSetLayouts = set_layouts;
    layout_info.pushConstantRangeCount = 1;
    layout_info.pPushConstantRanges = &range;
    CHECK(vkCreatePipelineLayout(vk.device, &layout_info, NULL, &vk.bloom_layout));
    if (make_fullscreen_pipeline(bloom_first_frag_spirv, sizeof bloom_first_frag_spirv, vk.bloom_layout,
                                 vk.dof_blur_pass, 0, &vk.bloom_first_pipeline) ||
        make_fullscreen_pipeline(bloom_blur_frag_spirv, sizeof bloom_blur_frag_spirv, vk.bloom_layout,
                                 vk.dof_blur_pass, 0, &vk.bloom_blur_pipeline) ||
        make_fullscreen_pipeline(sky_bleed_frag_spirv, sizeof sky_bleed_frag_spirv, vk.glow_layout,   /* g4 */
                                 vk.render_pass, 1, &vk.bloom_bleed_pipeline))
        return -1;
    vk.bloom_ready = 1;
    return 0;
}

void ngl_vk_set_bloom(const float setup[4], const float *taps, uint32_t blurs, float intensity, float sky_bleed)
{
    /* [cod2-native] g5: made on the first call that wants a bloom, so a port that never asks makes nothing */
    if (!vk.bloom_ready && !vk.bloom_tried && vk.device && setup && taps && (intensity > 0.0f || sky_bleed > 0.0f)) {
        vk.bloom_tried = 1;
        if (make_bloom())
            fprintf(stderr, "[ngl_vk] no bloom: drawn without it\n");
        else
            fprintf(stderr, "[ngl_vk] bloom made (first ngl_vk_set_bloom)\n");
    }
    vk.bloom_on = vk.bloom_ready && setup && taps && (intensity > 0.0f || sky_bleed > 0.0f);   /* g4: or a bleed */
    vk.bloom_sky_bleed = vk.bloom_on && sky_bleed > 0.0f ? sky_bleed : 0.0f;
    if (!vk.bloom_on)
        return;
    vk.glow_on = 1;
    vk.glow_strength = intensity > 0.0f ? intensity : 0.0f;
    memcpy(vk.bloom_setup, setup, sizeof vk.bloom_setup);
    vk.bloom_blurs = blurs < BLOOM_MOST_BLURS ? blurs : BLOOM_MOST_BLURS;
    memset(vk.bloom_taps, 0, sizeof vk.bloom_taps);
    memcpy(vk.bloom_taps, taps, (size_t)vk.bloom_blurs * sizeof vk.bloom_taps[0]);
}

static void record_post_copy(void);     /* [rexglue-native-fx] 2026-11-11 fx1: (draw_warp copies the target first) */

/* [rexglue-native-fx] 2026-11-11 fx1: the screen warp's pipeline: heat_warp.vert / heat_warp.frag, one triangle strip
 * of corners (x, y, u, v) from warp_buffer, blended by the source alpha (the mask) over what is there, the target's
 * alpha kept, in the overlay pass. */
static int make_warp_pipeline(void)
{
    VkShaderModule vertex_shader = VK_NULL_HANDLE, fragment_shader = VK_NULL_HANDLE;
    VkPipelineShaderStageCreateInfo stages[2] = {
        { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO },
        { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO },
    };
    VkVertexInputBindingDescription binding = { 0, 4 * sizeof(float), VK_VERTEX_INPUT_RATE_VERTEX };
    VkVertexInputAttributeDescription attributes[2] = {
        { 0, 0, VK_FORMAT_R32G32_SFLOAT, 0 },                   /* where the corner goes */
        { 1, 0, VK_FORMAT_R32G32_SFLOAT, 2 * sizeof(float) },   /* where it reads the picture */
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

    if (make_shader(heat_warp_vert_spirv, sizeof heat_warp_vert_spirv, &vertex_shader) ||
        make_shader(heat_warp_frag_spirv, sizeof heat_warp_frag_spirv, &fragment_shader)) {
        vkDestroyShaderModule(vk.device, vertex_shader, NULL);
        return -1;
    }
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vertex_shader;
    stages[0].pName = "main";
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fragment_shader;
    stages[1].pName = "main";
    vertex_input.vertexBindingDescriptionCount = 1;
    vertex_input.pVertexBindingDescriptions = &binding;
    vertex_input.vertexAttributeDescriptionCount = 2;
    vertex_input.pVertexAttributeDescriptions = attributes;
    assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;   /* the game's own strip, its joins included */
    viewport.viewportCount = 1;
    viewport.scissorCount = 1;
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.cullMode = VK_CULL_MODE_NONE;
    raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    raster.lineWidth = 1.0f;
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    blend_attachment.blendEnable = VK_TRUE;     /* the game's: source alpha, one less source alpha */
    blend_attachment.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    blend_attachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blend_attachment.colorBlendOp = VK_BLEND_OP_ADD;
    blend_attachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;   /* the target's alpha kept */
    blend_attachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
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
    info.layout = vk.heat_layout;
    info.renderPass = vk.render_pass;       /* (the overlay pass is compatible) */
    info.subpass = 0;
    result = vkCreateGraphicsPipelines(vk.device, VK_NULL_HANDLE, 1, &info, NULL, &vk.heat_pipeline);
    vkDestroyShaderModule(vk.device, vertex_shader, NULL);
    vkDestroyShaderModule(vk.device, fragment_shader, NULL);
    if (result != VK_SUCCESS) {
        fprintf(stderr, "[ngl_vk] vkCreateGraphicsPipelines (screen warp) failed (%d)\n", (int)result);
        return -1;
    }
    return 0;
}

/* [rexglue-native thread2] 2026-11-24: the heat mask, the effect layer's size, and its framebuffer (the mask and the
 * layer's depth, for the effect pass), at make_heat and again at ngl_vk_set_target_size (after the layer's images) */
static int make_heat_sized(void)
{
    VkImageView views[2];
    VkFramebufferCreateInfo framebuffer_info = { VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };

    if (make_image(vk.effect_width, vk.effect_height, 1, TARGET_FORMAT,
                   VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                   &vk.heat_mask, &vk.heat_mask_memory, &vk.heat_mask_view))
        return -1;
    views[0] = vk.heat_mask_view;
    views[1] = vk.effect_depth_view;
    framebuffer_info.renderPass = vk.effect_pass;
    framebuffer_info.attachmentCount = 2;
    framebuffer_info.pAttachments = views;
    framebuffer_info.width = vk.effect_width;
    framebuffer_info.height = vk.effect_height;
    framebuffer_info.layers = 1;
    CHECK(vkCreateFramebuffer(vk.device, &framebuffer_info, NULL, &vk.heat_framebuffer));
    return 0;
}

/* [rexglue-native thread2] 2026-11-24: the mask's set pointed at it */
static void heat_point(void)
{
    point_image(vk.heat_mask_set, 0, vk.sampler, vk.heat_mask_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}

/* [rexglue-native-fx] 2026-11-11 fx1: the heat mask, its framebuffer (effect_pass: the mask and the layer's depth), its
 * set, the warp's layout, buffers and pipeline. After make_effect_layer (its size, depth and pass) and make_tint
 * (post_copy). */
static int make_heat(void)
{
    VkDescriptorPoolSize pool_size = { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1 };
    VkDescriptorPoolCreateInfo pool_info = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
    VkDescriptorSetAllocateInfo set_info = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
    VkDescriptorSetLayout layouts[2] = { vk.set_layout, vk.set_layout };   /* the picture's copy, the mask */
    VkPipelineLayoutCreateInfo layout_info = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };

    vk.heat_ready = 0;
    if (!vk.effect_ready || !vk.mesh_heat_pipeline || !vk.post_copy_set || !vk.overlay_pass)
        return -1;
    if (make_heat_sized())      /* [rexglue-native thread2] 2026-11-24: shared with ngl_vk_set_target_size */
        return -1;
    pool_info.maxSets = 1;
    pool_info.poolSizeCount = 1;
    pool_info.pPoolSizes = &pool_size;
    CHECK(vkCreateDescriptorPool(vk.device, &pool_info, NULL, &vk.heat_pool));
    set_info.descriptorPool = vk.heat_pool;
    set_info.descriptorSetCount = 1;
    set_info.pSetLayouts = &vk.set_layout;
    CHECK(vkAllocateDescriptorSets(vk.device, &set_info, &vk.heat_mask_set));
    heat_point();
    layout_info.setLayoutCount = 2;
    layout_info.pSetLayouts = layouts;
    CHECK(vkCreatePipelineLayout(vk.device, &layout_info, NULL, &vk.heat_layout));
    vk.warp_corners = calloc((size_t)NGL_VK_WARP_MOST_CORNERS * 4u, sizeof(float));
    if (!vk.warp_corners ||
        make_host_buffer((VkDeviceSize)NGL_VK_WARP_MOST_CORNERS * 4u * sizeof(float), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                         &vk.warp_buffer, &vk.warp_memory, (void **)&vk.warp_data) ||
        make_warp_pipeline())
        return -1;
    vk.heat_ready = 1;
    return 0;
}

void ngl_vk_set_screen_warp(const float *corners, uint32_t count)     /* [rexglue-native-fx] 2026-11-11 fx1 */
{
    if (!vk.heat_ready || !vk.warp_corners || !corners || count < 3u || count > NGL_VK_WARP_MOST_CORNERS) {
        vk.warp_count = 0;
        return;
    }
    memcpy(vk.warp_corners, corners, (size_t)count * 4u * sizeof(float));
    vk.warp_count = count;
}

/* [rexglue-native-fx] 2026-11-11 fx1: the screen warp, in the overlay pass the effect layer's blend back left open (the
 * pass ends, the target is copied into post_copy, the pass goes on): the strip drawn over the game's screen (`view`),
 * each corner reading the copy where the game's screen lies in the target, blended by the heat mask. The pass is left
 * open. */
static void draw_warp(VkRenderPassBeginInfo *pass, const VkViewport *view, const VkRect2D *whole)
{
    VkDescriptorSet sets[2] = { vk.post_copy_set, vk.heat_mask_set };
    VkDeviceSize no_offset = 0;
    float from_u = view->x / (float)vk.width, from_v = view->y / (float)vk.height;
    float to_u = view->width / (float)vk.width, to_v = view->height / (float)vk.height;

    vkCmdEndRenderPass(vk.commands);
    record_post_copy();
    for (uint32_t i = 0; i < vk.warp_count; i++) {     /* (the last frame is done with the buffer: finish_frame) */
        vk.warp_data[4 * i + 0] = vk.warp_corners[4 * i + 0];
        vk.warp_data[4 * i + 1] = vk.warp_corners[4 * i + 1];
        vk.warp_data[4 * i + 2] = from_u + vk.warp_corners[4 * i + 2] * to_u;
        vk.warp_data[4 * i + 3] = from_v + vk.warp_corners[4 * i + 3] * to_v;
    }
    pass->renderPass = vk.overlay_pass;
    vkCmdBeginRenderPass(vk.commands, pass, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdSetViewport(vk.commands, 0, 1, view);
    vkCmdSetScissor(vk.commands, 0, 1, whole);
    vkCmdBindPipeline(vk.commands, VK_PIPELINE_BIND_POINT_GRAPHICS, vk.heat_pipeline);
    vkCmdBindDescriptorSets(vk.commands, VK_PIPELINE_BIND_POINT_GRAPHICS, vk.heat_layout, 0, 2, sets, 0, NULL);
    vkCmdBindVertexBuffers(vk.commands, 0, 1, &vk.warp_buffer, &no_offset);
    vkCmdDraw(vk.commands, vk.warp_count, 1, 0, 0);
}

/* [rexglue-native] 2026-11-10: the sky glow's small passes, between two parts of the overlay pass (the target left
 * ready to be copied, post_copy holding the picture as it is now): the sky alone into glow 0, then glow_passes blurs
 * to and fro; returns which of the two holds the last one. screen: the game's screen within the target (0..1). */
/* [cod2-native] 2026-11-24: the IW bloom's small passes (ngl_vk_set_bloom), where record_glow's are and as they
 * are drawn: the bright part into glow 0, then each blur to and fro; returns which of the two holds the last. */
/* g4: sky 1: the first pass keeps the sky's own color times its share instead of the bright part (sky_bleed) */
static int record_bloom(const float screen[4], int sky)
{
    VkRenderPassBeginInfo pass = { VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
    VkViewport view = { 0.0f, 0.0f, (float)GLOW_SIZE, (float)GLOW_SIZE, 0.0f, 1.0f };
    VkRect2D area = { { 0, 0 }, { GLOW_SIZE, GLOW_SIZE } };
    VkDescriptorSet first_sets[2] = { vk.post_copy_set, vk.shadow_set };
    float first[8];
    int from = 0;

    memcpy(first, screen, 4 * sizeof(float));
    memcpy(first + 4, vk.bloom_setup, 4 * sizeof(float));
    first[6] = sky ? 1.0f : 0.0f;   /* [cod2-native] g4: the setup's z, unused by the game's glow_setup */
    pass.renderPass = vk.dof_blur_pass;
    pass.renderArea = area;
    pass.framebuffer = vk.glow_framebuffer[0];
    vkCmdBeginRenderPass(vk.commands, &pass, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdSetViewport(vk.commands, 0, 1, &view);
    vkCmdSetScissor(vk.commands, 0, 1, &area);
    vkCmdBindPipeline(vk.commands, VK_PIPELINE_BIND_POINT_GRAPHICS, vk.bloom_first_pipeline);
    vkCmdBindDescriptorSets(vk.commands, VK_PIPELINE_BIND_POINT_GRAPHICS, vk.bloom_layout, 0, 2, first_sets, 0, NULL);
    vkCmdPushConstants(vk.commands, vk.bloom_layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof first, first);
    vkCmdDraw(vk.commands, 3, 1, 0, 0);
    vkCmdEndRenderPass(vk.commands);
    for (uint32_t k = 0; k < vk.bloom_blurs; k++) {
        pass.framebuffer = vk.glow_framebuffer[from ^ 1];
        vkCmdBeginRenderPass(vk.commands, &pass, VK_SUBPASS_CONTENTS_INLINE);
        vkCmdSetViewport(vk.commands, 0, 1, &view);
        vkCmdSetScissor(vk.commands, 0, 1, &area);
        vkCmdBindPipeline(vk.commands, VK_PIPELINE_BIND_POINT_GRAPHICS, vk.bloom_blur_pipeline);
        vkCmdBindDescriptorSets(vk.commands, VK_PIPELINE_BIND_POINT_GRAPHICS, vk.bloom_layout, 0, 1,
                                &vk.glow_set[from], 0, NULL);
        vkCmdPushConstants(vk.commands, vk.bloom_layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof vk.bloom_taps[k],
                           vk.bloom_taps[k]);
        vkCmdDraw(vk.commands, 3, 1, 0, 0);
        vkCmdEndRenderPass(vk.commands);
        from ^= 1;
    }
    return from;
}

static int record_glow(const float screen[4])
{
    if (vk.bloom_on)    /* [cod2-native] 2026-11-24: this frame's glow is the IW bloom */
        return record_bloom(screen, 0);
    VkRenderPassBeginInfo pass = { VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
    VkViewport view = { 0.0f, 0.0f, (float)GLOW_SIZE, (float)GLOW_SIZE, 0.0f, 1.0f };
    VkRect2D area = { { 0, 0 }, { GLOW_SIZE, GLOW_SIZE } };
    VkDescriptorSet first_sets[2] = { vk.post_copy_set, vk.shadow_set };
    int from = 0;

    pass.renderPass = vk.dof_blur_pass;
    pass.renderArea = area;
    pass.framebuffer = vk.glow_framebuffer[0];
    vkCmdBeginRenderPass(vk.commands, &pass, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdSetViewport(vk.commands, 0, 1, &view);
    vkCmdSetScissor(vk.commands, 0, 1, &area);
    vkCmdBindPipeline(vk.commands, VK_PIPELINE_BIND_POINT_GRAPHICS, vk.glow_first_pipeline);
    vkCmdBindDescriptorSets(vk.commands, VK_PIPELINE_BIND_POINT_GRAPHICS, vk.glow_layout, 0, 2, first_sets, 0, NULL);
    vkCmdPushConstants(vk.commands, vk.glow_layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, 4 * sizeof(float), screen);
    vkCmdDraw(vk.commands, 3, 1, 0, 0);
    vkCmdEndRenderPass(vk.commands);
    for (uint32_t k = 1; k <= vk.glow_passes; k++) {
        /* the taps' distance in the small target's 0..1, the gain, the lean; the point the taps lean toward */
        float blur[8] = { (float)k * vk.glow_tap / (float)GLOW_SIZE, vk.glow_gain, vk.glow_lean, 0.0f,
                          vk.glow_toward[0], vk.glow_toward[1], 0.0f, 0.0f };

        pass.framebuffer = vk.glow_framebuffer[from ^ 1];
        vkCmdBeginRenderPass(vk.commands, &pass, VK_SUBPASS_CONTENTS_INLINE);
        vkCmdSetViewport(vk.commands, 0, 1, &view);
        vkCmdSetScissor(vk.commands, 0, 1, &area);
        vkCmdBindPipeline(vk.commands, VK_PIPELINE_BIND_POINT_GRAPHICS, vk.glow_blur_pipeline);
        vkCmdBindDescriptorSets(vk.commands, VK_PIPELINE_BIND_POINT_GRAPHICS, vk.glow_layout, 0, 1,
                                &vk.glow_set[from], 0, NULL);
        vkCmdPushConstants(vk.commands, vk.glow_layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof blur, blur);
        vkCmdDraw(vk.commands, 3, 1, 0, 0);
        vkCmdEndRenderPass(vk.commands);
        from ^= 1;
    }
    return from;
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

/* [rexglue-native] 2026-11-11 rn47n: the last finished frame for this frame's quads (NGL_VK_LAST_FRAME), and this
 * frame's kept for the next (draw: after the quads, record_history_copy, as the motion blur keeps it) */
int ngl_vk_keep_last_frame(void)
{
    if (!vk.device || !vk.post_history_set)
        return 0;
    vk.keep_frame = 1;
    return vk.history_frame && vk.history_frame == vk.frames_done;
}

/* [rexglue-native] 2026-11-11 rn47i: this frame's quads, or the first of them, under the afterimage and post effect
 * 7 (draw: under) */
void ngl_vk_set_quads_under_effects(uint32_t vertex_count)
{
    vk.under_count = vertex_count;
}

/* [rexglue-native] 2026-11-11 rn47g: post effect 4 as the game draws it (draw: after; tint.frag main) */
void ngl_vk_set_afterimage(float share, float inset, float keep_depth)
{
    if (!(share > 0.0f))
        return;
    if (!vk.tint_on)
        reset_tint();
    vk.tint_on = 1;
    vk.tint[7] = keep_depth;
    vk.after_on = 1;
    vk.after_share = share < 1.0f ? share : 1.0f;
    vk.after_inset = inset > 0.0f ? (inset < 0.45f ? inset : 0.45f) : 0.0f;
}

/* [rexglue-native] 2026-11-11 rn48b: the afterimage's kept picture at a quarter size (as the game's) or full size */
void ngl_vk_set_afterimage_small(int on)
{
    vk.after_small_on = on != 0;
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

/* [waw-decomp] 2026-11-11: Call of Duty 4's and World at War's film tweak, in the tint's own draw in place of its tint,
 * brightness and color look-up (tint.frag toned_at): the bias where the weights and the amount are, the base where the
 * color is, the delta and the "on" in tint.frag's tweak */
void ngl_vk_set_film_tweak(const float bias[4], const float base[3], const float delta[3], float keep_depth)
{
    if (!bias || !base || !delta)
        return;
    if (!vk.tint_on)
        reset_tint();
    vk.tint_on = 1;
    for (int i = 0; i < 4; i++)
        vk.tint[i] = bias[i];
    for (int i = 0; i < 3; i++) {
        vk.tint[4 + i] = base[i];
        vk.tint[28 + i] = delta[i];
    }
    vk.tint[7] = keep_depth;
    vk.tint[31] = 1.0f;
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

/* [rexglue-native] 2026-11-11 rn47g: after the afterimage's first draw (its pass ended, the target left ready to be
 * copied): the target copied whole into the afterimage's kept picture (as record_history_copy does it for the motion
 * blur's). Also once before that draw the first time ever, so the kept picture starts as this frame's. */
static void record_after_copy(void)
{
    VkImageCopy copy = { 0 };

    /* [rexglue-native] 2026-11-11 rn48b: into the quarter-size kept picture, filtered, when that one is used;
     * [rexglue-native] 2026-11-11 rn48c: then stretched back into after_image, filtered, which the draws read */
    if (vk.after_small_on && vk.after_small_image && vk.after_image) {
        VkImageBlit blit = { 0 };

        image_barrier(vk.target, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                      VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                      VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        image_barrier(vk.after_small_image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                      VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                      VK_PIPELINE_STAGE_TRANSFER_BIT);
        blit.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        blit.srcSubresource.layerCount = 1;
        blit.dstSubresource = blit.srcSubresource;
        blit.srcOffsets[1].x = (int32_t)vk.width;
        blit.srcOffsets[1].y = (int32_t)vk.height;
        blit.srcOffsets[1].z = 1;
        blit.dstOffsets[1].x = (int32_t)vk.after_small_width;
        blit.dstOffsets[1].y = (int32_t)vk.after_small_height;
        blit.dstOffsets[1].z = 1;
        vkCmdBlitImage(vk.commands, vk.target, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, vk.after_small_image,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);
        image_barrier(vk.after_small_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                      VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                      VK_PIPELINE_STAGE_TRANSFER_BIT);
        image_barrier(vk.after_image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                      VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                      VK_PIPELINE_STAGE_TRANSFER_BIT);
        blit.srcOffsets[1] = blit.dstOffsets[1];
        blit.dstOffsets[1].x = (int32_t)vk.width;
        blit.dstOffsets[1].y = (int32_t)vk.height;
        vkCmdBlitImage(vk.commands, vk.after_small_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, vk.after_image,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);
        image_barrier(vk.after_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                      VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                      VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
        image_barrier(vk.target, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                      VK_ACCESS_TRANSFER_READ_BIT,
                      VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                      VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
        vk.after_frame = vk.frames_done + 1;
        return;
    }

    image_barrier(vk.target, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                  VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                  VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    image_barrier(vk.after_image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                  VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                  VK_PIPELINE_STAGE_TRANSFER_BIT);
    copy.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    copy.srcSubresource.layerCount = 1;
    copy.dstSubresource = copy.srcSubresource;
    copy.extent.width = vk.width;
    copy.extent.height = vk.height;
    copy.extent.depth = 1;
    vkCmdCopyImage(vk.commands, vk.target, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, vk.after_image,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    image_barrier(vk.after_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                  VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                  VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    image_barrier(vk.target, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                  VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                  VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
    vk.after_frame = vk.frames_done + 1;  /* this frame's (frames_done counts the frames finished) */
}

/* [rexglue-native] 2026-11-11 rn47o: for a quad drawn with the picture so far (NGL_VK_THIS_FRAME), between two parts
 * of the quads' pass (the pass ended, the target left ready to be copied): the target copied whole into post_copy,
 * as record_film_copy does it, but whatever post_copy held or was last used for (its old picture isn't wanted). */
static void record_this_frame_copy(void)
{
    VkImageCopy copy = { 0 };

    image_barrier(vk.target, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                  VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                  VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    image_barrier(vk.post_copy, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
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

/* [rexglue-native] 2026-11-10: this frame's sky glow (ngl_vk_set_glow), called inside a part of the main pass, which
 * it ends: the target copied whole into post_copy (after_tint 1: as record_film_copy does it, post_copy having been
 * read by the tint's draw; 0: as record_post_copy, nothing having used it this frame), the glow's small passes, then
 * the overlay pass begun and the blurred sky added over the game's screen (view). Leaves the overlay pass open with
 * that viewport, as the quads want it. */
static void draw_glow(VkRenderPassBeginInfo *pass, const VkViewport *view, const VkRect2D *whole, int after_tint)
{
    float screen[4] = { view->x / (float)vk.width, view->y / (float)vk.height, view->width / (float)vk.width,
                        view->height / (float)vk.height };
    float strength[4] = { vk.glow_strength, 0.0f, 0.0f, 0.0f };
    int last;

    vkCmdEndRenderPass(vk.commands);
    if (after_tint)
        record_film_copy();
    else
        record_post_copy();
    if (vk.bloom_on && vk.bloom_sky_bleed > 0.0f) {
        /* [cod2-native] 2026-11-24 g4: the sky bleed first (the game's glow_apply_sky_bleed), its own blurred sky
         * color and share, drawn over the screen; the bloom after it reads the same copy, as the game's chain does */
        float bleed[4] = { vk.bloom_sky_bleed, 0.0f, 0.0f, 0.0f };

        last = record_bloom(screen, 1);
        pass->renderPass = vk.overlay_pass;
        vkCmdBeginRenderPass(vk.commands, pass, VK_SUBPASS_CONTENTS_INLINE);
        vkCmdSetViewport(vk.commands, 0, 1, view);
        vkCmdSetScissor(vk.commands, 0, 1, whole);
        vkCmdBindPipeline(vk.commands, VK_PIPELINE_BIND_POINT_GRAPHICS, vk.bloom_bleed_pipeline);
        vkCmdBindDescriptorSets(vk.commands, VK_PIPELINE_BIND_POINT_GRAPHICS, vk.glow_layout, 0, 1,
                                &vk.glow_set[last], 0, NULL);
        vkCmdPushConstants(vk.commands, vk.glow_layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof bleed, bleed);
        vkCmdDraw(vk.commands, 3, 1, 0, 0);
        if (vk.glow_strength <= 0.0f)
            return;
        vkCmdEndRenderPass(vk.commands);
    }
    last = record_glow(screen);
    pass->renderPass = vk.overlay_pass;
    vkCmdBeginRenderPass(vk.commands, pass, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdSetViewport(vk.commands, 0, 1, view);
    vkCmdSetScissor(vk.commands, 0, 1, whole);
    vkCmdBindPipeline(vk.commands, VK_PIPELINE_BIND_POINT_GRAPHICS, vk.glow_add_pipeline);
    vkCmdBindDescriptorSets(vk.commands, VK_PIPELINE_BIND_POINT_GRAPHICS, vk.glow_layout, 0, 1,
                            &vk.glow_set[last], 0, NULL);
    vkCmdPushConstants(vk.commands, vk.glow_layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof strength, strength);
    vkCmdDraw(vk.commands, 3, 1, 0, 0);
}

/* Between the second half and the overlay pass: the target (left ready to be copied) shrunk to a half and then a
 * quarter of its size, then blurred across into dof_temp and down back into dof_quarter, rounds times, the taps step
 * of its texels apart (1.5: about 12 of the target's at the Gaussian's sigma).
 * [acct2-bro] 2026-11-13 rn64c: game_taps: Call of Duty 3's own depth of field blur instead (its 0x82147458 with
 * the taps of 0x82146E90): one pass of 13 taps a texel apart from dof_quarter into dof_temp, then a plain copy back
 * into dof_quarter, which the mix reads; rounds and spacing are not used then. */
static void record_dof(int rounds, float spacing, int game_taps)
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
    for (int pass_number = 0; pass_number < 2 * (game_taps ? 1 : rounds); pass_number++) {
        int down = pass_number & 1;
        /* the step, then the kind (dof_blur.frag): 0 a line of nine taps, 1 the game's 13 taps, 2 a plain copy */
        float step[4] = { down ? 0.0f : spacing / (float)vk.dof_width,
                          down ? spacing / (float)vk.dof_height : 0.0f, 0.0f, 0.0f };

        if (game_taps) {    /* [acct2-bro] 2026-11-13 rn64c */
            step[0] = 1.0f / (float)vk.dof_width;
            step[1] = 1.0f / (float)vk.dof_height;
            step[2] = down ? 2.0f : 1.0f;
        }
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
    probe_submitted();      /* [rexglue-native thread2] 2026-11-24: this frame's depth probes are on the GPU */
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
    vk.after_on = 0;        /* [rexglue-native] 2026-11-11 rn47g: and an afterimage (its kept picture stays) */
    vk.under_count = 0;     /* [rexglue-native] 2026-11-11 rn47i: and quads under the effects */
    vk.keep_frame = 0;      /* [rexglue-native] 2026-11-11 rn47n: and a frame kept for the next */
    vk.glow_on = 0;         /* [rexglue-native] and a sky glow */
    vk.bloom_on = 0;        /* [cod2-native] 2026-11-24: or a bloom */
    vk.frames_done++;
    return 0;
}

static int draw_recorded(const void *pixels, uint32_t width, uint32_t height, uint32_t pitch,
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
    float film_constants[32];   /* [waw-decomp] 2026-11-11: 32 (was 24), pushed whole: the film tweak's four 0 */
    int motion = tint && vk.motion_on;
    float motion_constants[32]; /* [waw-decomp] 2026-11-11: as film_constants (was 24) */
    /* [rexglue-native] 2026-11-11 rn47g: and post effect 4 as the game draws it (ngl_vk_set_afterimage): two draws
     * of its own where the motion blur's is, before post effect 7's */
    int after = tint && vk.after_on && vk.after_image && vk.after_set;
    /* [rexglue-native] 2026-11-10: and the sky glow (ngl_vk_set_glow), right after the tint's draw */
    int glow = halves && vk.glow_on && vk.glow_ready && vk.overlay_pass;
    /* [acct2-bro] 2026-11-22 rn113c: and MSAA: the first half multisampled, the rest over the resolved target */
    int msaa = halves && vk.msaa_samples > 1u;

    if (!vk.device || !pixels || !width || !height || pitch < width * BYTES_PER_PIXEL)
        return -1;
    finish_frame();     /* the last draw's, when frames overlap */
    effect_count_read();    /* [acct2-bro] 2026-11-22 rn114: the last frame's effect samples, now it is done */
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
    if (msaa) {     /* [acct2-bro] 2026-11-22 rn113c: the first half into the samples, with the pipelines' twins */
        pass.renderPass = vk.msaa_opaque_pass;
        pass.framebuffer = vk.msaa_framebuffer;
        msaa_swap_pipelines();
    }
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
            if (msaa) {     /* [acct2-bro] 2026-11-22 rn113c: resolved; on with one sample (the depth, the grass) */
                msaa_swap_pipelines();
                msaa_resolve_depth(&view);
                pass.framebuffer = vk.framebuffer;
            }
            copy_soft_depth(&view);     /* [rexglue-native thread2] 2026-11-24: the probes' viewport */
            pass.renderPass = vk.blend_pass;
            vkCmdBeginRenderPass(vk.commands, &pass, VK_SUBPASS_CONTENTS_INLINE);
            vkCmdSetViewport(vk.commands, 0, 1, &view);
            vkCmdSetScissor(vk.commands, 0, 1, &whole);
            /* [rexglue-native] 2026-11-10: the effect layer (ngl_vk.h effect_layer), when this frame has draws for it */
            vk.effect_live = 0;
            if (vk.effect_ready && vk.overlay_pass) {
                for (uint32_t i = 0; i < vk.draw_count && !vk.effect_live; i++)
                    vk.effect_live = vk.draws[i].effect_layer && vk.draws[i].alpha == NGL_VK_ALPHA_BLEND;
            }
            vk.heat_live = 0;       /* [rexglue-native-fx] 2026-11-11 fx1: and the heat haze's mask draws among them */
            if (vk.effect_live && vk.heat_ready) {                                  /* [rexglue-native-fx] */
                for (uint32_t i = 0; i < vk.draw_count && !vk.heat_live; i++)       /* [rexglue-native-fx] */
                    vk.heat_live = vk.draws[i].blend == NGL_VK_BLEND_HEAT_MASK &&
                                   vk.draws[i].effect_layer && vk.draws[i].alpha == NGL_VK_ALPHA_BLEND;
            }
            vk.soft_live = 1;
            draw_meshes(1);
            vk.soft_live = 0;
            if (vk.effect_live) {   /* [rexglue-native] 2026-11-10 */
                VkRenderPassBeginInfo layer_pass = { VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
                VkClearValue layer_clear[2] = { { { { 0.0f, 0.0f, 0.0f, 0.0f } } }, { { { 1.0f, 0.0f, 0.0f, 0.0f } } } };
                VkRect2D layer_whole = { { 0, 0 }, { vk.effect_width, vk.effect_height } };
                VkViewport all = { 0.0f, 0.0f, (float)vk.width, (float)vk.height, 0.0f, 1.0f };
                VkViewport layer_all = { 0.0f, 0.0f, (float)vk.effect_width, (float)vk.effect_height, 0.0f, 1.0f };
                /* The layer's pixel (x, y) stands for the picture's pixel (2x, 2y), as the game's does: what the
                 * picture draws at the middle of that pixel, (2x + 0.5, 2y + 0.5), is at (x + 0.25, y + 0.25) of the
                 * layer at half the size, and has to be at the layer pixel's middle: a quarter pixel further on. */
                VkViewport layer_view = { view.x * 0.5f + 0.25f, view.y * 0.5f + 0.25f, view.width * 0.5f,
                                          view.height * 0.5f, 0.0f, 1.0f };
                VkDescriptorSet fill_sets[2] = { vk.dof_quarter_set, vk.shadow_set };   /* (set 0 isn't read) */
                VkDescriptorSet back_sets[2] = { vk.effect_color_set, vk.shadow_set };

                vkCmdEndRenderPass(vk.commands);
                effect_count_reset();   /* [acct2-bro] 2026-11-22 rn114 */
                /* its color cleared, its depth from the opaque depth's copy at every second pixel */
                layer_clear[1].depthStencil.depth = 1.0f;
                layer_clear[1].depthStencil.stencil = 0;
                layer_pass.renderPass = vk.effect_fill_pass;
                layer_pass.framebuffer = vk.effect_framebuffer;
                layer_pass.renderArea = layer_whole;
                layer_pass.clearValueCount = 2;
                layer_pass.pClearValues = layer_clear;
                vkCmdBeginRenderPass(vk.commands, &layer_pass, VK_SUBPASS_CONTENTS_INLINE);
                vkCmdSetViewport(vk.commands, 0, 1, &layer_all);
                vkCmdSetScissor(vk.commands, 0, 1, &layer_whole);
                vkCmdBindPipeline(vk.commands, VK_PIPELINE_BIND_POINT_GRAPHICS, vk.effect_fill_pipeline);
                vkCmdBindDescriptorSets(vk.commands, VK_PIPELINE_BIND_POINT_GRAPHICS, vk.dof_mix_layout, 0, 2,
                                        fill_sets, 0, NULL);
                vkCmdDraw(vk.commands, 3, 1, 0, 0);
                vkCmdEndRenderPass(vk.commands);
                /* its draws, tested against that depth and fading by it */
                layer_pass.renderPass = vk.effect_pass;
                vkCmdBeginRenderPass(vk.commands, &layer_pass, VK_SUBPASS_CONTENTS_INLINE);
                vkCmdSetViewport(vk.commands, 0, 1, &layer_view);
                vkCmdSetScissor(vk.commands, 0, 1, &layer_whole);
                effect_count_begin(&view);  /* [acct2-bro] 2026-11-22 rn114: the layer's samples, counted */
                vk.soft_live = 1;
                draw_meshes(3);
                vk.soft_live = 0;
                effect_count_end();     /* [acct2-bro] 2026-11-22 rn114 */
                vkCmdEndRenderPass(vk.commands);
                vk.effect_live = 0;
                if (vk.heat_live) {     /* [rexglue-native-fx] 2026-11-11 fx1: the heat haze's mask (part 4) */
                    /* cleared to 1 (no haze) with a transfer clear (the loader has vkCmdClearColorImage) */
                    VkClearColorValue one = { { 1.0f, 1.0f, 1.0f, 1.0f } };                /* [rexglue-native-fx] */
                    VkImageSubresourceRange mask_range = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };

                    image_barrier(vk.heat_mask, vk.heat_layout_ok ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                                                                  : VK_IMAGE_LAYOUT_UNDEFINED,
                                  VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_ACCESS_SHADER_READ_BIT,
                                  VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                                  VK_PIPELINE_STAGE_TRANSFER_BIT);
                    vkCmdClearColorImage(vk.commands, vk.heat_mask, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &one, 1,
                                         &mask_range);
                    image_barrier(vk.heat_mask, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                  VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
                                  VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                                  VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
                    vk.heat_layout_ok = 1;
                    layer_pass.renderPass = vk.effect_pass;
                    layer_pass.framebuffer = vk.heat_framebuffer;
                    vkCmdBeginRenderPass(vk.commands, &layer_pass, VK_SUBPASS_CONTENTS_INLINE);
                    vkCmdSetViewport(vk.commands, 0, 1, &layer_view);
                    vkCmdSetScissor(vk.commands, 0, 1, &layer_whole);
                    vk.soft_live = 1;
                    draw_meshes(4);
                    vk.soft_live = 0;
                    vkCmdEndRenderPass(vk.commands);
                    layer_pass.framebuffer = vk.effect_framebuffer;
                }
                /* and the layer over the picture (the overlay pass: the target as the second half left it) */
                pass.renderPass = vk.overlay_pass;
                vkCmdBeginRenderPass(vk.commands, &pass, VK_SUBPASS_CONTENTS_INLINE);
                vkCmdSetViewport(vk.commands, 0, 1, &all);
                vkCmdSetScissor(vk.commands, 0, 1, &whole);
                vkCmdBindPipeline(vk.commands, VK_PIPELINE_BIND_POINT_GRAPHICS, vk.effect_back_pipeline);
                vkCmdBindDescriptorSets(vk.commands, VK_PIPELINE_BIND_POINT_GRAPHICS, vk.dof_mix_layout, 0, 2,
                                        back_sets, 0, NULL);
                vkCmdDraw(vk.commands, 3, 1, 0, 0);
                vkCmdSetViewport(vk.commands, 0, 1, &view);
                /* [rexglue-native-fx] 2026-11-11 fx1: the screen warp over the picture as the layer left it, blended by
                 * the heat mask (the game draws its heat haze right after its effects' blend back) */
                if (vk.heat_live && vk.warp_count >= 3u)                                 /* [rexglue-native-fx] */
                    draw_warp(&pass, &view, &whole);                                    /* [rexglue-native-fx] */
                vk.heat_live = 0;                                                       /* [rexglue-native-fx] */
            }
            if (dof) {
                VkViewport all = { 0.0f, 0.0f, (float)vk.width, (float)vk.height, 0.0f, 1.0f };
                VkDescriptorSet sets[2] = { vk.dof_quarter_set, vk.shadow_set };

                vkCmdEndRenderPass(vk.commands);
                /* the blanket (the menus' blur) is wider than the depth of field: more rounds, the taps further apart.
                 * [rexglue-native-render] 2026-11-11 rn48m: three rounds 1.25 texels apart, sigma 3.72 quarter texels
                 * against the game's chain's 3.51 (post effect 10; RENDER-NOTES.md, 10:37 item 4); it was two rounds
                 * 2.0 apart: sigma 4.78, every odd texel skipped */
                /* [acct2-bro] 2026-11-13 rn64c: with the bands (dof_focus[4]), the game's own 13 taps */
                if (vk.dof_focus[3] > 0.0f)
                    record_dof(3, 1.25f, 0);    /* [rexglue-native-render] 2026-11-11 rn48m: was (2, 2.0f) */
                else
                    record_dof(1, 1.5f, vk.dof_focus[4] > 0.0f);
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
                if (glow)   /* [rexglue-native] 2026-11-10: the sky glow, of the picture as the tint left it */
                    draw_glow(&pass, &view, &whole, 1);
                /* rn41g: then the motion blur and post effect 7, each over the whole 3D, the scenes in front
                 * included (the game's concussion blurs its gun): the target copied again and drawn back. The
                 * quads come after them (the game's crosshair stays sharp), and the last frame's picture is kept
                 * after the quads (its compass trails: its motion blur's last frame holds the HUD) */
                /* [rexglue-native] 2026-11-11 rn47g: post effect 4 as the game draws it (ngl_vk_set_afterimage; the
                 * game: decomp 0x82149950), before post effect 7's draw: the picture so far copied (post_copy);
                 * first its shader drawn with the coordinates drawn in, which the game draws into its kept picture
                 * and ngl_vk draws over the target and copies into after_image; then the same over the target with
                 * plain coordinates, which is the tint's mix with "the last frame" with after_image as that frame.
                 * The tint pass's other numbers are neutral in both (film_constants', without post effect 7's). */
                /* [rexglue-native] 2026-11-11 rn47i: the quads the caller wants under the afterimage and post effect
                 * 7 (ngl_vk_set_quads_under_effects), in a frame with one of the two: drawn here, in the overlay
                 * pass the tint's draw (or the sky glow) left open, as the quads below are drawn; `first` then
                 * leaves only the rest to that loop */
                if (vk.under_count && (after || film) && vertex_count) {
                    uint32_t under = vk.under_count < vertex_count ? vk.under_count : vertex_count;

                    under -= under % 3u;    /* whole triangles */
                    vkCmdSetViewport(vk.commands, 0, 1, &view);
                    vkCmdSetScissor(vk.commands, 0, 1, &whole);
                    memcpy(vk.vertex_data, quad_vertices, (size_t)vertex_count * sizeof *quad_vertices);
                    vkCmdBindPipeline(vk.commands, VK_PIPELINE_BIND_POINT_GRAPHICS, vk.quad_pipeline);
                    vk.quad_bound_added = 0;    /* [rexglue-native-render] rn48l */
                    vkCmdPushConstants(vk.commands, vk.quad_layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof screen,
                                       screen);
                    vkCmdBindVertexBuffers(vk.commands, 0, 1, &vk.vertices, &no_offset);
                    while (first < under) {
                        uint32_t texture = quad_vertices[first].texture, end = first + 1;
                        VkDescriptorSet set = texture && texture <= vk.texture_count ? vk.textures[texture].set
                                              : texture == NGL_VK_LAST_FRAME && vk.history_frame &&
                                                vk.history_frame == vk.frames_done ? vk.post_history_set  /* rn47n */
                                                                                     : vk.set;
                        int added = quad_vertices[first].textured > 3.5f;   /* [rexglue-native-render] rn48l */

                        while (end < under && quad_vertices[end].texture == texture &&
                               (quad_vertices[end].textured > 3.5f) == added)
                            end++;
                        bind_quad_pipeline(added);
                        vkCmdBindDescriptorSets(vk.commands, VK_PIPELINE_BIND_POINT_GRAPHICS, vk.quad_layout, 0, 1,
                                                &set, 0, NULL);
                        vkCmdDraw(vk.commands, end - first, 1, first, 0);
                        first = end;
                    }
                }
                if (after) {
                    VkDescriptorSet after_sets[4] = { sets[0], sets[1], vk.after_set, sets[3] };
                    float after_constants[32];  /* [waw-decomp] 2026-11-11: 32 (was 28): the film tweak's four 0 */

                    memset(after_constants, 0, sizeof after_constants);
                    memcpy(after_constants, film_constants, 12 * sizeof(float));
                    after_constants[10] = vk.after_share;
                    after_constants[24] = vk.after_inset > 0.0f ? vk.after_inset : 1.0e-6f;
                    vkCmdEndRenderPass(vk.commands);
                    record_film_copy();
                    if (!vk.after_frame)    /* the first time ever: the kept picture starts as this frame's */
                        record_after_copy();
                    for (int half = 0; half < 2; half++) {
                        vkCmdBeginRenderPass(vk.commands, &pass, VK_SUBPASS_CONTENTS_INLINE);
                        vkCmdSetViewport(vk.commands, 0, 1, &all);
                        vkCmdSetScissor(vk.commands, 0, 1, &whole);
                        vkCmdBindPipeline(vk.commands, VK_PIPELINE_BIND_POINT_GRAPHICS, vk.tint_pipeline);
                        vkCmdBindDescriptorSets(vk.commands, VK_PIPELINE_BIND_POINT_GRAPHICS, vk.tint_layout, 0, 4,
                                                after_sets, 0, NULL);
                        vkCmdPushConstants(vk.commands, vk.tint_layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                                           sizeof after_constants, after_constants);
                        vkCmdDraw(vk.commands, 3, 1, 0, 0);
                        if (half == 0) {
                            vkCmdEndRenderPass(vk.commands);
                            record_after_copy();
                            after_constants[24] = 0.0f;     /* the second draw: plain coordinates */
                        }
                    }
                }
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
            if (glow && !tint)  /* [rexglue-native] 2026-11-10: a frame with a sky glow and no tint */
                draw_glow(&pass, &view, &whole, 0);
        } else if (vk.draw_count) {
            draw_meshes(2);
        }

        memcpy(vk.vertex_data, quad_vertices, (size_t)vertex_count * sizeof *quad_vertices);
        vkCmdBindPipeline(vk.commands, VK_PIPELINE_BIND_POINT_GRAPHICS, vk.quad_pipeline);
        vkCmdPushConstants(vk.commands, vk.quad_layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof screen, screen);
        vkCmdBindVertexBuffers(vk.commands, 0, 1, &vk.vertices, &no_offset);
        vk.quad_bound_added = 0;    /* [rexglue-native-render] rn48l: the plain pipeline is the one bound */
        /* each run of vertices with one texture, with that texture's set */
        while (first < vertex_count) {
            uint32_t texture = quad_vertices[first].texture, end = first + 1;
            VkDescriptorSet set = texture && texture <= vk.texture_count ? vk.textures[texture].set : vk.set;
            int added = quad_vertices[first].textured > 3.5f;       /* [rexglue-native-render] rn48l */

            /* [rexglue-native] 2026-11-11 rn47n: the last finished frame (ngl_vk_keep_last_frame), when it is held */
            if (texture == NGL_VK_LAST_FRAME && vk.history_frame && vk.history_frame == vk.frames_done)
                set = vk.post_history_set;
            /* [rexglue-native] 2026-11-11 rn47o: the picture so far (NGL_VK_THIS_FRAME), in a frame with meshes (the
             * target can be taken up again by the overlay pass): the pass ended, the target copied into post_copy
             * and the quads gone on with in the overlay pass */
            if (texture == NGL_VK_THIS_FRAME && halves && vk.post_copy && vk.post_copy_set && vk.overlay_pass &&
                pass.renderPass != vk.render_pass && pass.renderPass != vk.opaque_pass) {
                vkCmdEndRenderPass(vk.commands);
                record_this_frame_copy();
                pass.renderPass = vk.overlay_pass;      /* the target as it was left, whichever pass that was */
                vkCmdBeginRenderPass(vk.commands, &pass, VK_SUBPASS_CONTENTS_INLINE);
                vkCmdSetViewport(vk.commands, 0, 1, &view);
                vkCmdSetScissor(vk.commands, 0, 1, &whole);
                vkCmdBindPipeline(vk.commands, VK_PIPELINE_BIND_POINT_GRAPHICS, vk.quad_pipeline);
                vk.quad_bound_added = 0;    /* [rexglue-native-render] rn48l */
                vkCmdPushConstants(vk.commands, vk.quad_layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof screen, screen);
                vkCmdBindVertexBuffers(vk.commands, 0, 1, &vk.vertices, &no_offset);
                set = vk.post_copy_set;
            }
            while (end < vertex_count && quad_vertices[end].texture == texture &&
                   (quad_vertices[end].textured > 3.5f) == added)
                end++;
            bind_quad_pipeline(added);  /* [rexglue-native-render] rn48l */
            vkCmdBindDescriptorSets(vk.commands, VK_PIPELINE_BIND_POINT_GRAPHICS, vk.quad_layout, 0, 1, &set, 0,
                                    NULL);
            vkCmdDraw(vk.commands, end - first, 1, first, 0);
            first = end;
        }
    }
    vkCmdEndRenderPass(vk.commands);
    if (motion)  /* rn41g: the whole picture, the quads included, kept for the next frame's motion blur */
        record_history_copy();
    if (!motion && vk.keep_frame && vk.post_history)    /* [rexglue-native] 2026-11-11 rn47n: or for its quads */
        record_history_copy();
    vk.warp_count = 0;      /* [rexglue-native-fx] 2026-11-11 fx1: a warp is the frame's it was given for */
    return submit_and_show(showing, window_image);
}

/* [rexglue-native thread2] 2026-11-24: one frame (draw_recorded), marked as being recorded from its start to its
 * submit whichever way it returns, so ngl_vk_set_target_size called from inside it (an overlay's callback, say)
 * refuses instead of freeing what the frame's commands use */
static int draw(const void *pixels, uint32_t width, uint32_t height, uint32_t pitch,
                const struct ngl_vk_vertex *quad_vertices, uint32_t vertex_count,
                float screen_width, float screen_height)
{
    int result;

    vk.recording = 1;
    result = draw_recorded(pixels, width, height, pitch, quad_vertices, vertex_count, screen_width, screen_height);
    vk.recording = 0;
    return result;
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
    int result;

    if (!vk.low_frame_open)
        return -1;
    vk.low_frame_open = 0;
    vk.recording = 1;   /* [rexglue-native thread2] 2026-11-24: until it is submitted (draw's note) */
    result = submit_and_show(vk.low_showing, vk.low_window_image);
    vk.recording = 0;
    return result;
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

/* [acct2-ocr] 2026-11-13: window_ramp's image and framebuffer freed (after the GPU is done with them). */
static void window_ramp_free(void)
{
    if (!vk.device)
        return;
    vkDestroyFramebuffer(vk.device, window_ramp.framebuffer, NULL);
    vkDestroyImageView(vk.device, window_ramp.view, NULL);
    vkDestroyImage(vk.device, window_ramp.image, NULL);
    vkFreeMemory(vk.device, window_ramp.memory, NULL);
    window_ramp.framebuffer = VK_NULL_HANDLE;
    window_ramp.view = VK_NULL_HANDLE;
    window_ramp.image = VK_NULL_HANDLE;
    window_ramp.memory = VK_NULL_HANDLE;
    window_ramp.pass = VK_NULL_HANDLE;
    window_ramp.width = window_ramp.height = 0;
    window_fx.ramp_view = VK_NULL_HANDLE;   /* [acct2-fh] 11-14: window_fx's set points at the next one again */
}

/* [acct2-ocr] 2026-11-13: recorded into this frame's commands, after the read-back, from record_window_copy: the
 * finished target (TRANSFER_SRC_OPTIMAL) drawn through window_ramp.table into window_ramp.image, which is left in
 * TRANSFER_SRC_OPTIMAL for the blit; the target goes back to TRANSFER_SRC_OPTIMAL. The table, its image, the pass
 * and the pipeline are ngl_vk_output's (a program uses one or the other: Big Red One the window, Call of Duty 3
 * ngl_vk_output). draw() has finished the last frame (finish_frame) before this is recorded, so the image, its
 * framebuffer and the table's upload buffer are free. Returns 0 when the image holds the frame. */
static int window_ramp_record(void)
{
    VkFramebufferCreateInfo framebuffer_info = { VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
    VkRenderPassBeginInfo pass = { VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
    VkViewport viewport = { 0.0f, 0.0f, (float)vk.width, (float)vk.height, 0.0f, 1.0f };
    VkRect2D whole = { { 0, 0 }, { vk.width, vk.height } };
    VkDescriptorSet sets[OUTPUT_SETS];

    if (make_output_parts() || make_output_pass(TARGET_FORMAT)) {
        window_ramp.failed = 1;
        fprintf(stderr, "[ngl_vk] the window's color table can't be drawn: the window shows the target as drawn\n");
        return -1;
    }
    if (!window_ramp.image || window_ramp.width != vk.width || window_ramp.height != vk.height) {
        window_ramp_free();
        if (make_image(vk.width, vk.height, 1, TARGET_FORMAT,
                       VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                           VK_IMAGE_USAGE_SAMPLED_BIT,             /* [acct2-fh] 11-14: window_fx reads it too */
                       &window_ramp.image, &window_ramp.memory, &window_ramp.view)) {
            window_ramp.failed = 1;
            fprintf(stderr, "[ngl_vk] no image for the window's color table: the window shows the target as drawn\n");
            return -1;
        }
        window_ramp.width = vk.width;
        window_ramp.height = vk.height;
        fprintf(stderr, "[ngl_vk] the window shows the target through a color table (%ux%u)\n", vk.width, vk.height);
    }
    if (!window_ramp.framebuffer || window_ramp.pass != vk.output_pass) {
        vkDestroyFramebuffer(vk.device, window_ramp.framebuffer, NULL);
        window_ramp.framebuffer = VK_NULL_HANDLE;
        framebuffer_info.renderPass = vk.output_pass;
        framebuffer_info.attachmentCount = 1;
        framebuffer_info.pAttachments = &window_ramp.view;
        framebuffer_info.width = vk.width;
        framebuffer_info.height = vk.height;
        framebuffer_info.layers = 1;
        if (vkCreateFramebuffer(vk.device, &framebuffer_info, NULL, &window_ramp.framebuffer) != VK_SUCCESS) {
            window_ramp.failed = 1;
            fprintf(stderr, "[ngl_vk] no framebuffer for the window's color table: the target as drawn\n");
            return -1;
        }
        window_ramp.pass = vk.output_pass;
    }
    /* the table, when it changed (the game sets its ramp rarely) */
    if (!vk.table_ready || memcmp(vk.table_words, window_ramp.table, sizeof vk.table_words)) {
        VkBufferImageCopy copy = { 0 };

        memcpy(vk.table_words, window_ramp.table, sizeof vk.table_words);
        memcpy(vk.table_upload_data, window_ramp.table, sizeof vk.table_words);
        output_barrier(vk.commands, vk.table_image,
                       vk.table_ready ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
                       VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        copy.imageSubresource.layerCount = 1;
        copy.imageExtent.width = 256;
        copy.imageExtent.height = 1;
        copy.imageExtent.depth = 1;
        vkCmdCopyBufferToImage(vk.commands, vk.table_upload, vk.table_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                               &copy);
        output_barrier(vk.commands, vk.table_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                       VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
                       VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                       VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
        vk.table_ready = 1;
    }
    /* the target as the draw left it (drawn, perhaps copied out), to be sampled; the image, its old frame dropped */
    output_barrier(vk.commands, vk.target, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                   VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                   VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                   VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    output_barrier(vk.commands, window_ramp.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                   0, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                   VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
    pass.renderPass = vk.output_pass;
    pass.framebuffer = window_ramp.framebuffer;
    pass.renderArea = whole;
    vkCmdBeginRenderPass(vk.commands, &pass, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdSetViewport(vk.commands, 0, 1, &viewport);
    vkCmdSetScissor(vk.commands, 0, 1, &whole);
    vkCmdBindPipeline(vk.commands, VK_PIPELINE_BIND_POINT_GRAPHICS, vk.output_pipeline);
    sets[0] = vk.output_target_set;
    sets[1] = vk.output_table_set;
    vkCmdBindDescriptorSets(vk.commands, VK_PIPELINE_BIND_POINT_GRAPHICS, vk.output_layout, 0, OUTPUT_SETS, sets, 0,
                            NULL);
    vkCmdDraw(vk.commands, 3, 1, 0, 0);
    vkCmdEndRenderPass(vk.commands);
    /* the image ready to be blitted; the target back to where the draws leave it */
    output_barrier(vk.commands, window_ramp.image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                   VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                   VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                   VK_PIPELINE_STAGE_TRANSFER_BIT);
    output_barrier(vk.commands, vk.target, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   0, 0, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    return 0;
}

/* [acct2-fh] 2026-11-14: window_fx's two images and their framebuffers freed (the GPU done with them). */
static void window_fx_free_images(void)
{
    uint32_t i;

    for (i = 0; i < 2; i++) {
        vkDestroyFramebuffer(vk.device, window_fx.framebuffer[i], NULL);
        vkDestroyImageView(vk.device, window_fx.view[i], NULL);
        vkDestroyImage(vk.device, window_fx.image[i], NULL);
        vkFreeMemory(vk.device, window_fx.memory[i], NULL);
        window_fx.framebuffer[i] = VK_NULL_HANDLE;
        window_fx.view[i] = VK_NULL_HANDLE;
        window_fx.image[i] = VK_NULL_HANDLE;
        window_fx.memory[i] = VK_NULL_HANDLE;
    }
    window_fx.width = window_fx.height = 0;
}

/* [acct2-fh] 2026-11-14: all of window_fx's own Vulkan parts freed (its sets go with vk.descriptor_pool); the three
 * settings are kept. */
static void window_fx_free(void)
{
    if (!vk.device)
        return;
    window_fx_free_images();
    vkDestroyPipeline(vk.device, window_fx.cas, NULL);
    vkDestroyPipeline(vk.device, window_fx.easu, NULL);
    vkDestroyPipeline(vk.device, window_fx.rcas, NULL);
    vkDestroyPipelineLayout(vk.device, window_fx.layout, NULL);
    vkDestroyRenderPass(vk.device, window_fx.pass, NULL);
    window_fx.cas = window_fx.easu = window_fx.rcas = VK_NULL_HANDLE;
    window_fx.layout = VK_NULL_HANDLE;
    window_fx.pass = VK_NULL_HANDLE;
    window_fx.target_set = window_fx.image_set[0] = window_fx.image_set[1] = window_fx.ramp_set = VK_NULL_HANDLE;
    window_fx.target_view = window_fx.ramp_view = VK_NULL_HANDLE;
    window_fx.failed = 0;
}

/* [acct2-fh] 2026-11-14: one image of a set (binding 0) pointed at a view, read in SHADER_READ_ONLY_OPTIMAL. */
static void window_fx_point(VkDescriptorSet set, VkImageView view)
{
    VkDescriptorImageInfo image_info = { 0 };
    VkWriteDescriptorSet write = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };

    image_info.sampler = vk.sampler;
    image_info.imageView = view;
    image_info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    write.dstSet = set;
    write.dstBinding = 0;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.pImageInfo = &image_info;
    vkUpdateDescriptorSets(vk.device, 1, &write, 0, NULL);
}

/* [acct2-fh] 2026-11-14: window_fx's pass (the image's contents not kept: every pixel drawn is drawn), its layout,
 * its three pipelines (quad.vert's triangle over the viewport) and its three sets. Made once for a device. */
static int window_fx_make_parts(void)
{
    VkAttachmentDescription attachment = { 0 };
    VkAttachmentReference color = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
    VkSubpassDescription subpass = { 0 };
    VkRenderPassCreateInfo pass_info = { VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
    VkPushConstantRange push = { VK_SHADER_STAGE_FRAGMENT_BIT, 0, 4 * sizeof(float) };
    VkPipelineLayoutCreateInfo layout_info = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    VkDescriptorSetLayout layouts[WINDOW_FX_SETS] = { vk.set_layout, vk.set_layout, vk.set_layout, vk.set_layout };
    VkDescriptorSet sets[WINDOW_FX_SETS] = { VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE };
    VkDescriptorSetAllocateInfo set_info = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };

    if (window_fx.target_set)
        return 0;
    if (!window_fx.pass) {
        attachment.format = TARGET_FORMAT;
        attachment.samples = VK_SAMPLE_COUNT_1_BIT;
        attachment.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
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
        CHECK(vkCreateRenderPass(vk.device, &pass_info, NULL, &window_fx.pass));
    }
    if (!window_fx.layout) {
        layout_info.setLayoutCount = 1;
        layout_info.pSetLayouts = &vk.set_layout;
        layout_info.pushConstantRangeCount = 1;
        layout_info.pPushConstantRanges = &push;
        CHECK(vkCreatePipelineLayout(vk.device, &layout_info, NULL, &window_fx.layout));
    }
    if ((!window_fx.cas && make_fullscreen_pipeline(window_cas_frag_spirv, sizeof window_cas_frag_spirv,
                                                    window_fx.layout, window_fx.pass, 0, &window_fx.cas)) ||
        (!window_fx.easu && make_fullscreen_pipeline(window_easu_frag_spirv, sizeof window_easu_frag_spirv,
                                                     window_fx.layout, window_fx.pass, 0, &window_fx.easu)) ||
        (!window_fx.rcas && make_fullscreen_pipeline(window_rcas_frag_spirv, sizeof window_rcas_frag_spirv,
                                                     window_fx.layout, window_fx.pass, 0, &window_fx.rcas)))
        return -1;
    set_info.descriptorPool = vk.descriptor_pool;
    set_info.descriptorSetCount = WINDOW_FX_SETS;
    set_info.pSetLayouts = layouts;
    CHECK(vkAllocateDescriptorSets(vk.device, &set_info, sets));
    window_fx.target_set = sets[0];
    window_fx.image_set[0] = sets[1];
    window_fx.image_set[1] = sets[2];
    window_fx.ramp_set = sets[3];
    window_fx.target_view = window_fx.ramp_view = VK_NULL_HANDLE;     /* pointed when first read */
    return 0;
}

/* [acct2-fh] 2026-11-14: window_fx's two images at width x height (each drawn into and read, then copied out), their
 * framebuffers, and their sets pointed at them. */
static int window_fx_make_images(uint32_t width, uint32_t height)
{
    VkFramebufferCreateInfo framebuffer_info = { VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
    uint32_t i;

    window_fx_free_images();
    for (i = 0; i < 2; i++) {
        if (make_image(width, height, 1, TARGET_FORMAT,
                       VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                       &window_fx.image[i], &window_fx.memory[i], &window_fx.view[i]))
            return -1;
        framebuffer_info.renderPass = window_fx.pass;
        framebuffer_info.attachmentCount = 1;
        framebuffer_info.pAttachments = &window_fx.view[i];
        framebuffer_info.width = width;
        framebuffer_info.height = height;
        framebuffer_info.layers = 1;
        CHECK(vkCreateFramebuffer(vk.device, &framebuffer_info, NULL, &window_fx.framebuffer[i]));
        window_fx_point(window_fx.image_set[i], window_fx.view[i]);
    }
    window_fx.width = width;
    window_fx.height = height;
    return 0;
}

/* [acct2-fh] 2026-11-14: one of window_fx's passes: pipeline reads set (with the four push constants) and draws the
 * corner (0, 0) to (width, height) of image[into], left in COLOR_ATTACHMENT_OPTIMAL. The image's old contents are
 * dropped, after the reads of an earlier pass of this frame. */
static void window_fx_draw(int into, VkPipeline pipeline, VkDescriptorSet set, uint32_t width, uint32_t height,
                           const float push[4])
{
    VkRenderPassBeginInfo begin = { VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
    VkViewport viewport = { 0.0f, 0.0f, (float)width, (float)height, 0.0f, 1.0f };
    VkRect2D area = { { 0, 0 }, { width, height } };

    output_barrier(vk.commands, window_fx.image[into], VK_IMAGE_LAYOUT_UNDEFINED,
                   VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, 0, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                   VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                   VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
    begin.renderPass = window_fx.pass;
    begin.framebuffer = window_fx.framebuffer[into];
    begin.renderArea = area;
    vkCmdBeginRenderPass(vk.commands, &begin, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdSetViewport(vk.commands, 0, 1, &viewport);
    vkCmdSetScissor(vk.commands, 0, 1, &area);
    vkCmdBindPipeline(vk.commands, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    vkCmdBindDescriptorSets(vk.commands, VK_PIPELINE_BIND_POINT_GRAPHICS, window_fx.layout, 0, 1, &set, 0, NULL);
    vkCmdPushConstants(vk.commands, window_fx.layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, 4 * sizeof(float), push);
    vkCmdDraw(vk.commands, 3, 1, 0, 0);
    vkCmdEndRenderPass(vk.commands);
}

/* [acct2-fh] 2026-11-14: recorded into this frame's commands from record_window_copy, after the read-back: the
 * finished target (TRANSFER_SRC_OPTIMAL, and back so after) through CAS (passes 0: window_cas.frag, its 3 x 3 read
 * one shown pixel apart) or FSR 1 (passes EASU passes, each to at most twice the last size and the last to width x
 * height, then RCAS at width x height) into one of window_fx's images, ping-pong. draw() has finished the last frame
 * (finish_frame) before this is recorded, so the images and sets are free to be made again or pointed elsewhere.
 * Returns the image holding the picture (in TRANSFER_SRC_OPTIMAL), or -1 when the parts can't be made (said once:
 * smooth from then on). */
static int window_fx_record(uint32_t width, uint32_t height, int passes, int from_ramp)
{
    VkDescriptorSet from = VK_NULL_HANDLE, source_set;
    VkImage source;
    uint32_t from_width = vk.width, from_height = vk.height;
    float push[4];
    int into = 0, pass;

    if (window_fx_make_parts() ||
        ((width != window_fx.width || height != window_fx.height) && window_fx_make_images(width, height))) {
        window_fx.failed = 1;
        window_fx_free_images();
        fprintf(stderr, "[ngl_vk] the window's CAS / FSR 1 can't be made: the window shows the picture smooth\n");
        return -1;
    }
    /* [acct2-fh] 11-14 stage 5c: the source is the target, or the color table's image (window_ramp_record has just
     * drawn it, the picture's size, left in TRANSFER_SRC_OPTIMAL as the target is) */
    if (from_ramp) {
        if (window_fx.ramp_view != window_ramp.view) {
            window_fx_point(window_fx.ramp_set, window_ramp.view);
            window_fx.ramp_view = window_ramp.view;
        }
        source = window_ramp.image;
        source_set = window_fx.ramp_set;
    } else {
        if (window_fx.target_view != vk.target_view) {
            window_fx_point(window_fx.target_set, vk.target_view);
            window_fx.target_view = vk.target_view;
        }
        source = vk.target;
        source_set = window_fx.target_set;
    }
    output_barrier(vk.commands, source, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                   VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                   VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                   VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    if (passes > 0) {
        from = source_set;
        for (pass = 0; pass < passes; pass++) {
            uint32_t to_width = pass + 1 == passes || from_width * 2 > width ? width : from_width * 2;
            uint32_t to_height = pass + 1 == passes || from_height * 2 > height ? height : from_height * 2;

            push[0] = (float)from_width;
            push[1] = (float)from_height;
            push[2] = (float)to_width;
            push[3] = (float)to_height;
            window_fx_draw(into, window_fx.easu, from, to_width, to_height, push);
            output_barrier(vk.commands, window_fx.image[into], VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                           VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                           VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                           VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
            from = window_fx.image_set[into];
            from_width = to_width;
            from_height = to_height;
            into ^= 1;
        }
        push[0] = (float)width;
        push[1] = (float)height;
        push[2] = window_fx.fsr_reduction;              /* stops: the shader's sharpness is 2 to the minus that */
        push[3] = 0.0f;
        window_fx_draw(into, window_fx.rcas, from, width, height, push);
    } else {
        push[0] = 1.0f / (float)width;
        push[1] = 1.0f / (float)height;
        push[2] = -1.0f / (8.0f + (5.0f - 8.0f) * window_fx.cas_sharpness);   /* -1 / mix(8, 5, sharpness) */
        push[3] = 0.0f;
        window_fx_draw(into, window_fx.cas, source_set, width, height, push);
    }
    output_barrier(vk.commands, window_fx.image[into], VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                   VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                   VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                   VK_PIPELINE_STAGE_TRANSFER_BIT);
    output_barrier(vk.commands, source, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   0, 0, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    return into;
}

/* ---------------------------------------------------------------------------------------------------------------
 * [rexglue-native thread2] 2026-11-24: ngl_vk_set_target_size (ngl_vk.h; owner 07:48: "make sure we are getting all
 * these native renderers to actually render the required resolutions without upscaling then downscaling"; the design
 * is drafts/NATIVE-RES-AUDIT.md section 7). Everything sized from the target is listed in struct sized_set. A resize
 * moves the old set out of vk, makes the new one in its place with the same functions init uses (make_target_sized,
 * make_msaa_sized, make_dof_images and _framebuffers, make_post_images, make_effect_images and _framebuffer,
 * make_heat_sized), and only when all of it is made points the sets at it and frees the old set; if any of it
 * can't be made, what was made is freed and the old set goes back, untouched (no set was pointed elsewhere yet). */
#define SIZED_IMAGES(X)                                                                                              \
    X(target) X(depth) X(soft_depth) X(msaa_color) X(msaa_depth) X(dof_half) X(dof_quarter) X(dof_temp)              \
    X(post_copy) X(post_history) X(after_image) X(after_small_image) X(effect_color) X(effect_depth) X(heat_mask)
#define SIZED_MEMORY(X)                                                                                              \
    X(target_memory) X(depth_memory) X(soft_depth_memory) X(msaa_color_memory) X(msaa_depth_memory)                  \
    X(dof_half_memory) X(dof_quarter_memory) X(dof_temp_memory) X(post_copy_memory) X(post_history_memory)            \
    X(after_memory) X(after_small_memory) X(effect_color_memory) X(effect_depth_memory) X(heat_mask_memory)          \
    X(readback_memory)
#define SIZED_VIEWS(X)                                                                                               \
    X(target_view) X(depth_view) X(soft_depth_view) X(msaa_color_view) X(msaa_depth_view) X(dof_half_view)            \
    X(dof_quarter_view) X(dof_temp_view) X(post_copy_view) X(post_history_view) X(after_view) X(after_small_view)     \
    X(effect_color_view) X(effect_depth_view) X(heat_mask_view)
#define SIZED_FRAMEBUFFERS(X)                                                                                        \
    X(framebuffer) X(msaa_framebuffer) X(msaa_depth_framebuffer) X(dof_quarter_framebuffer) X(dof_temp_framebuffer)  \
    X(effect_framebuffer) X(heat_framebuffer)
#define SIZED_NUMBERS(X)                                                                                             \
    X(width) X(height) X(dof_width) X(dof_height) X(after_small_width) X(after_small_height) X(effect_width)           \
    X(effect_height)

struct sized_set {
#define SIZED_FIELD(type, name) type name;
#define SIZED_IMAGE_FIELD(name) SIZED_FIELD(VkImage, name)
#define SIZED_MEMORY_FIELD(name) SIZED_FIELD(VkDeviceMemory, name)
#define SIZED_VIEW_FIELD(name) SIZED_FIELD(VkImageView, name)
#define SIZED_FRAMEBUFFER_FIELD(name) SIZED_FIELD(VkFramebuffer, name)
#define SIZED_NUMBER_FIELD(name) SIZED_FIELD(uint32_t, name)
    SIZED_IMAGES(SIZED_IMAGE_FIELD)
    SIZED_MEMORY(SIZED_MEMORY_FIELD)
    SIZED_VIEWS(SIZED_VIEW_FIELD)
    SIZED_FRAMEBUFFERS(SIZED_FRAMEBUFFER_FIELD)
    SIZED_NUMBERS(SIZED_NUMBER_FIELD)
#undef SIZED_FIELD
#undef SIZED_IMAGE_FIELD
#undef SIZED_MEMORY_FIELD
#undef SIZED_VIEW_FIELD
#undef SIZED_FRAMEBUFFER_FIELD
#undef SIZED_NUMBER_FIELD
    VkBuffer readback;
    void *readback_pixels;
};

/* The set moved out of vk into *set (vk's handles left empty; its sizes kept, as the next make reads and sets them) */
static void sized_take(struct sized_set *set)
{
#define SIZED_TAKE(name) set->name = vk.name; vk.name = VK_NULL_HANDLE;
#define SIZED_COPY(name) set->name = vk.name;
    SIZED_IMAGES(SIZED_TAKE)
    SIZED_MEMORY(SIZED_TAKE)
    SIZED_VIEWS(SIZED_TAKE)
    SIZED_FRAMEBUFFERS(SIZED_TAKE)
    SIZED_NUMBERS(SIZED_COPY)
#undef SIZED_TAKE
#undef SIZED_COPY
    set->readback = vk.readback;
    set->readback_pixels = vk.readback_pixels;
    vk.readback = VK_NULL_HANDLE;
    vk.readback_pixels = NULL;
}

/* *set moved back into vk (whose handles of the set must be empty: freed or taken) */
static void sized_put(const struct sized_set *set)
{
#define SIZED_PUT(name) vk.name = set->name;
    SIZED_IMAGES(SIZED_PUT)
    SIZED_MEMORY(SIZED_PUT)
    SIZED_VIEWS(SIZED_PUT)
    SIZED_FRAMEBUFFERS(SIZED_PUT)
    SIZED_NUMBERS(SIZED_PUT)
#undef SIZED_PUT
    vk.readback = set->readback;
    vk.readback_pixels = set->readback_pixels;
}

/* Everything in *set freed (empty handles are skipped by Vulkan); the GPU must be done with it */
static void sized_free(struct sized_set *set)
{
#define SIZED_DESTROY_FRAMEBUFFER(name) vkDestroyFramebuffer(vk.device, set->name, NULL);
#define SIZED_DESTROY_VIEW(name) vkDestroyImageView(vk.device, set->name, NULL);
#define SIZED_DESTROY_IMAGE(name) vkDestroyImage(vk.device, set->name, NULL);
#define SIZED_FREE_MEMORY(name) vkFreeMemory(vk.device, set->name, NULL);
    SIZED_FRAMEBUFFERS(SIZED_DESTROY_FRAMEBUFFER)
    SIZED_VIEWS(SIZED_DESTROY_VIEW)
    SIZED_IMAGES(SIZED_DESTROY_IMAGE)
    if (set->readback_pixels)
        vkUnmapMemory(vk.device, set->readback_memory);
    vkDestroyBuffer(vk.device, set->readback, NULL);
    SIZED_MEMORY(SIZED_FREE_MEMORY)
#undef SIZED_DESTROY_FRAMEBUFFER
#undef SIZED_DESTROY_VIEW
#undef SIZED_DESTROY_IMAGE
#undef SIZED_FREE_MEMORY
}

/* Which groups of the set to make again: the ones the init made and the frame uses (taken before the old set is
 * moved out). A group that isn't in use (its init failed part way) isn't made again; its leftovers go with the old
 * set, and nothing reads them (each is checked where it is drawn). */
struct sized_groups {   /* (the target's own group, make_target_sized, is always made: the init needs all of it) */
    int msaa;   /* 1: made again; -1: let go (not even 2 samples fit the new size); 0: not in use */
    uint32_t samples;   /* with msaa 1: the samples it settles on (fewer than now: stepped down) */
    int dof_images, dof_framebuffers, post, after, after_small, effect, heat;
};

static void sized_groups_in_use(struct sized_groups *groups, uint32_t width, uint32_t height)
{
    memset(groups, 0, sizeof *groups);
    groups->msaa = vk.msaa_samples >= 2u && vk.msaa_framebuffer;
    groups->samples = vk.msaa_samples;
    if (groups->msaa) {
        /* as make_msaa_target chooses at init: halved until the device can draw and sample that many and the
         * picture's samples are within ngl_vk's budget (MSAA_MOST_SAMPLES); never more than now (the setting's) */
        VkPhysicalDeviceProperties properties;
        VkSampleCountFlags can;

        vkGetPhysicalDeviceProperties(vk.gpu, &properties);
        can = properties.limits.framebufferColorSampleCounts & properties.limits.framebufferDepthSampleCounts &
              properties.limits.sampledImageDepthSampleCounts;
        while (groups->samples >= 2u &&
               (!(can & groups->samples) || (uint64_t)width * height * groups->samples > MSAA_MOST_SAMPLES))
            groups->samples /= 2u;
        if (groups->samples < 2u)
            groups->msaa = -1;
    }
    groups->dof_images = vk.dof_half != VK_NULL_HANDLE;
    groups->dof_framebuffers = vk.dof_quarter_framebuffer && vk.dof_temp_framebuffer;
    groups->post = vk.post_history != VK_NULL_HANDLE;
    groups->after = vk.after_image != VK_NULL_HANDLE;
    groups->after_small = vk.after_small_image != VK_NULL_HANDLE;
    groups->effect = vk.effect_ready;
    groups->heat = vk.heat_ready && vk.effect_ready;
}

/* The new set at vk.width x vk.height (vk's handles of the set empty), group by group as the init made it; -1 when
 * something can't be made (what was made stays in vk, for sized_take and sized_free) */
static int make_sized(const struct sized_groups *groups)
{
    int after_made;

    if (make_target_sized())    /* the target, its depth and framebuffer, the soft depth, the read-back */
        return -1;
    if (groups->msaa > 0 && make_msaa_sized())
        return -1;
    if (groups->dof_images && make_dof_images())
        return -1;
    if (groups->dof_images && groups->dof_framebuffers && make_dof_framebuffers())
        return -1;
    if (groups->post && make_post_images(groups->after, groups->after_small, 1, &after_made))
        return -1;
    if (groups->effect && (make_effect_images() || make_effect_framebuffer()))
        return -1;
    if (groups->heat && make_heat_sized())
        return -1;
    return 0;
}

/* Every set that reads an image of the set pointed at the new one (the sets not made are left out) */
static void sized_point(void)
{
    soft_depth_point();
    if (vk.msaa_samples >= 2u && vk.msaa_depth_view)
        msaa_point();
    dof_point();
    post_point();
    effect_point();
    heat_point();
    point_image(vk.output_target_set, 0, vk.sampler, vk.target_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}

/* [rexglue-native thread2] 2026-11-24: MSAA's parts made for its sample count (the size-free ones: the multisampled
 * pass, the mesh twins, the camo twins), kept aside while a stepped-down set is made */
struct msaa_parts {
    uint32_t samples;
    int coverage;
    VkRenderPass opaque_pass;
    VkPipeline twins[MSAA_TWINS];
    VkPipeline camo[3];
};

static void msaa_parts_take(struct msaa_parts *parts)
{
    parts->samples = vk.msaa_samples;
    parts->coverage = vk.msaa_coverage;
    parts->opaque_pass = vk.msaa_opaque_pass;
    vk.msaa_opaque_pass = VK_NULL_HANDLE;
    for (int i = 0; i < MSAA_TWINS; i++) {
        parts->twins[i] = vk.msaa_twin[i];
        vk.msaa_twin[i] = VK_NULL_HANDLE;
    }
    for (int k = 0; k < 3; k++) {
        parts->camo[k] = vk.camo_msaa_pipeline[k];
        vk.camo_msaa_pipeline[k] = VK_NULL_HANDLE;
    }
}

static void msaa_parts_put(const struct msaa_parts *parts)
{
    vk.msaa_samples = parts->samples;
    vk.msaa_coverage = parts->coverage;
    vk.msaa_opaque_pass = parts->opaque_pass;
    for (int i = 0; i < MSAA_TWINS; i++)
        vk.msaa_twin[i] = parts->twins[i];
    for (int k = 0; k < 3; k++)
        vk.camo_msaa_pipeline[k] = parts->camo[k];
}

static void msaa_parts_free(struct msaa_parts *parts)
{
    vkDestroyRenderPass(vk.device, parts->opaque_pass, NULL);
    for (int i = 0; i < MSAA_TWINS; i++)
        vkDestroyPipeline(vk.device, parts->twins[i], NULL);
    for (int k = 0; k < 3; k++)
        vkDestroyPipeline(vk.device, parts->camo[k], NULL);
}

/* the stepped-down parts made so far freed (vk's fields emptied for msaa_parts_put) */
static void msaa_parts_free_new(void)
{
    struct msaa_parts made;

    msaa_parts_take(&made);
    msaa_parts_free(&made);
}

int ngl_vk_set_target_size(uint32_t width, uint32_t height)
{
    VkPhysicalDeviceProperties properties;
    struct sized_groups groups;
    struct sized_set old, failed;
    struct msaa_parts old_msaa;
    uint32_t most_width, most_height, old_samples;
    int stepped = 0;

    if (!vk.device) {
        fprintf(stderr, "[ngl_vk] ngl_vk_set_target_size(%u, %u) before ngl_vk_init: the init's size is the "
                "target's\n", width, height);
        return -1;
    }
    target_size_asked = 1;      /* (the port sizes the target: the window copy is one to one from now on) */
    if (width == vk.width && height == vk.height)
        return 0;
    if (vk.recording || vk.low_frame_open) {
        fprintf(stderr, "[ngl_vk] warning: ngl_vk_set_target_size(%u, %u) called inside a frame: the target stays "
                "%u x %u (call it between frames)\n", width, height, vk.width, vk.height);
        return -1;
    }
    vkGetPhysicalDeviceProperties(vk.gpu, &properties);
    most_width = properties.limits.maxImageDimension2D < properties.limits.maxFramebufferWidth
                     ? properties.limits.maxImageDimension2D
                     : properties.limits.maxFramebufferWidth;
    most_height = properties.limits.maxImageDimension2D < properties.limits.maxFramebufferHeight
                      ? properties.limits.maxImageDimension2D
                      : properties.limits.maxFramebufferHeight;
    if (!width || !height || width > most_width || height > most_height) {
        fprintf(stderr, "[ngl_vk] a %u x %u target can't be made (the device allows 1 x 1 to %u x %u): it stays "
                "%u x %u\n", width, height, most_width, most_height, vk.width, vk.height);
        return -1;
    }

    /* the frame in flight reads and writes the old set (and its window copy, in the same commands): its fence, and
     * the last ngl_vk_output's; nothing else of ngl_vk's is on the GPU (one frame in flight) */
    finish_frame();
    if (vk.output_pending) {
        if (vkWaitForFences(vk.device, 1, &vk.output_done, VK_TRUE, WAIT_NS) != VK_SUCCESS) {
            fprintf(stderr, "[ngl_vk] the last output didn't finish: the target stays %u x %u\n", vk.width,
                    vk.height);
            return -1;
        }
        vk.output_pending = 0;
    }

    sized_groups_in_use(&groups, width, height);
    old_samples = vk.msaa_samples;
    /* MSAA stepped down: a multisampled pass, twins and camo twins at the new count first (the old ones kept aside
     * until all of the new set is made) */
    if (groups.msaa > 0 && groups.samples != vk.msaa_samples) {
        msaa_parts_take(&old_msaa);
        stepped = 1;
        vk.msaa_samples = groups.samples;
        if (make_msaa_opaque_pass() || msaa_make_twins()) {
            msaa_parts_free_new();
            msaa_parts_put(&old_msaa);
            fprintf(stderr, "[ngl_vk] rn113: MSAA x%u's passes and pipelines couldn't be made for a %u x %u target: "
                    "it stays %u x %u\n", groups.samples, width, height, vk.width, vk.height);
            return -1;
        }
        make_camo_pipelines(1);     /* (one not made: that culling's camo is left out of the first half, as at init) */
    }
    sized_take(&old);
    vk.width = width;
    vk.height = height;
    if (make_sized(&groups)) {
        sized_take(&failed);
        sized_free(&failed);
        sized_put(&old);        /* (its sizes too: vk.width and vk.height are the old ones again) */
        if (stepped) {
            msaa_parts_free_new();
            msaa_parts_put(&old_msaa);
        }
        fprintf(stderr, "[ngl_vk] a %u x %u target couldn't all be made: it stays %u x %u\n", width, height,
                vk.width, vk.height);
        return -1;
    }
    sized_point();
    sized_free(&old);
    if (stepped)
        msaa_parts_free(&old_msaa);
    if (groups.msaa < 0)        /* (its images went with the old set; its passes, pipelines and set go now) */
        msaa_free();
    /* the samples MSAA settled on, once a resize (the owner chose MSAA for Call of Duty 3's anti-aliasing: a size
     * too large for it is said, never silent) */
    if (groups.msaa > 0)
        fprintf(stderr, "[ngl_vk] rn113: MSAA x%u at %u x %u%s\n", vk.msaa_samples, vk.width, vk.height,
                stepped ? " (stepped down: more samples are over the device's or ngl_vk's limit at this size)" : "");
    else if (groups.msaa < 0)
        fprintf(stderr, "[ngl_vk] rn113: MSAA x%u can't hold a %u x %u picture even at 2 samples: let go, drawn "
                "with one sample\n", old_samples, vk.width, vk.height);

    /* what pointed at the old set or held its pictures starts over */
    window_ramp_free();                         /* remade at the next window copy, at the new size */
    window_fx.target_view = VK_NULL_HANDLE;     /* its set pointed at the target again when next read */
    vk.drawn = 0;               /* nothing to output or read back until the next draw (the new target is undefined) */
    vk.read_back = 0;
    vk.soft_ready = 0;          /* the new images' layouts are undefined: put in their sampled layouts again */
    vk.history_layout_ok = 0;
    vk.heat_layout_ok = 0;
    vk.history_frame = 0;       /* no last frame (motion blur, NGL_VK_LAST_FRAME) */
    vk.after_frame = 0;         /* the afterimage's kept picture starts as the next frame's */
    /* the depth probes: results not yet read were read at the old size; the points given for the next draw stay */
    vk.probe_slot_state[0] = vk.probe_slot_state[1] = PROBE_FREE;
    vk.probe_slot_count[0] = vk.probe_slot_count[1] = 0;
    vk.probe_depth_count = 0;
    fprintf(stderr, "[ngl_vk] the target is now %u x %u (was %u x %u)\n", vk.width, vk.height, old.width,
            old.height);
    return 0;
}

void ngl_vk_shutdown(void)
{
    uint32_t row;

    if (vk.device) {
        wait_until_idle();
        free_swapchain();
        free_picture();
        window_ramp_free();     /* [acct2-ocr] 2026-11-13: the window's color table image (made only when used) */
        window_fx_free();       /* [acct2-fh] 2026-11-14: the window's CAS / FSR 1 parts (made only when used) */
        msaa_free();            /* [acct2-bro] 2026-11-22 rn113: MSAA's parts (made only when asked) */
        effect_count_free();    /* [acct2-bro] 2026-11-22 rn114 */
        probe_free();           /* [rexglue-native thread2] 2026-11-24: the depth probes' buffer (made only when used) */
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
        vkDestroyPipeline(vk.device, vk.mesh_cull_pipeline[0], NULL);  /* [acct2-bro] 2026-11-13 rn69a */
        vkDestroyPipeline(vk.device, vk.mesh_cull_pipeline[1], NULL);  /* [acct2-bro] */
        vkDestroyPipeline(vk.device, vk.mesh_blend_pipeline, NULL);
        vkDestroyPipeline(vk.device, vk.mesh_add_pipeline, NULL);
        vkDestroyPipeline(vk.device, vk.mesh_subtract_pipeline, NULL);
        vkDestroyPipeline(vk.device, vk.mesh_premultiplied_pipeline, NULL);  /* [acct2-bro] 2026-11-22 defect E */
        vk.mesh_premultiplied_pipeline = VK_NULL_HANDLE;
        vkDestroyPipeline(vk.device, vk.mesh_multiply_pipeline, NULL);  /* [waw-decomp] */
        vkDestroyPipeline(vk.device, vk.mesh_decal_pipeline, NULL);     /* [rexglue-native stlo 2026-11-16] */
        vkDestroyPipeline(vk.device, vk.mesh_decal_alpha_pipeline, NULL);   /* [rexglue-native stlo 2026-11-16] */
        vkDestroyPipeline(vk.device, vk.mesh_decal_depth_pipeline, NULL);   /* [rexglue-native stlo 2026-11-17] */
        vkDestroyPipeline(vk.device, vk.mesh_heat_pipeline, NULL);      /* [rexglue-native-fx] 2026-11-11 fx1 */
        for (int k = 0; k < 3; k++) {   /* [acct2-bro] 2026-11-22 Camo lane: the camo pass (make_camo) */
            vkDestroyPipeline(vk.device, vk.camo_pipeline[k], NULL);
            vkDestroyPipeline(vk.device, vk.camo_msaa_pipeline[k], NULL);
        }
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
        if (vk.quad_add_pipeline)   /* [rexglue-native-render] 2026-11-11 rn48l */
            vkDestroyPipeline(vk.device, vk.quad_add_pipeline, NULL);
        vkDestroyPipelineLayout(vk.device, vk.quad_layout, NULL);
        vkDestroyPipeline(vk.device, vk.pipeline, NULL);
        vkDestroyPipelineLayout(vk.device, vk.pipeline_layout, NULL);
        vkDestroyImageView(vk.device, vk.empty_cube_view, NULL);   /* [rexglue-native] rn34 (its set goes with the pool) */
        vkDestroyImage(vk.device, vk.empty_cube, NULL);
        vkFreeMemory(vk.device, vk.empty_cube_memory, NULL);
        vkDestroyDescriptorPool(vk.device, vk.descriptor_pool, NULL);
        vkDestroyDescriptorSetLayout(vk.device, vk.set_layout, NULL);
        vkDestroySampler(vk.device, vk.sampler, NULL);
        if (vk.aniso_sampler)   /* [acct2-bro] 2026-11-12 */
            vkDestroySampler(vk.device, vk.aniso_sampler, NULL);
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
        /* [rexglue-native] 2026-11-11 rn47g: the afterimage's kept picture; null handles where it wasn't made */
        vkDestroyImageView(vk.device, vk.after_view, NULL);
        vkDestroyImage(vk.device, vk.after_image, NULL);
        vkFreeMemory(vk.device, vk.after_memory, NULL);
        /* [rexglue-native] 2026-11-11 rn48b: and its quarter-size one */
        vkDestroyImageView(vk.device, vk.after_small_view, NULL);
        vkDestroyImage(vk.device, vk.after_small_image, NULL);
        vkFreeMemory(vk.device, vk.after_small_memory, NULL);
        /* [rexglue-native-fx] 2026-11-11 fx1: the screen warp (make_heat); null handles where it wasn't made */
        vkDestroyPipeline(vk.device, vk.heat_pipeline, NULL);
        vkDestroyPipelineLayout(vk.device, vk.heat_layout, NULL);
        vkDestroyDescriptorPool(vk.device, vk.heat_pool, NULL);
        vkDestroyFramebuffer(vk.device, vk.heat_framebuffer, NULL);
        vkDestroyImageView(vk.device, vk.heat_mask_view, NULL);
        vkDestroyImage(vk.device, vk.heat_mask, NULL);
        vkFreeMemory(vk.device, vk.heat_mask_memory, NULL);
        vkDestroyBuffer(vk.device, vk.warp_buffer, NULL);
        vkFreeMemory(vk.device, vk.warp_memory, NULL);
        free(vk.warp_corners);
        vk.warp_corners = NULL;
        vk.heat_ready = 0;
        /* [rexglue-native] 2026-11-10: the effect layer (make_effect_layer); null handles where it wasn't made */
        vkDestroyPipeline(vk.device, vk.effect_fill_pipeline, NULL);
        vkDestroyPipeline(vk.device, vk.effect_back_pipeline, NULL);
        vkDestroyDescriptorPool(vk.device, vk.effect_pool, NULL);
        vkDestroyFramebuffer(vk.device, vk.effect_framebuffer, NULL);
        vkDestroyRenderPass(vk.device, vk.effect_fill_pass, NULL);
        vkDestroyRenderPass(vk.device, vk.effect_pass, NULL);
        vkDestroyImageView(vk.device, vk.effect_color_view, NULL);
        vkDestroyImageView(vk.device, vk.effect_depth_view, NULL);
        vkDestroyImage(vk.device, vk.effect_color, NULL);
        vkDestroyImage(vk.device, vk.effect_depth, NULL);
        vkFreeMemory(vk.device, vk.effect_color_memory, NULL);
        vkFreeMemory(vk.device, vk.effect_depth_memory, NULL);
        /* [rexglue-native] 2026-11-10: the sky glow (make_glow); null handles where it wasn't made */
        vkDestroyPipeline(vk.device, vk.glow_first_pipeline, NULL);
        vkDestroyPipeline(vk.device, vk.glow_blur_pipeline, NULL);
        vkDestroyPipeline(vk.device, vk.glow_add_pipeline, NULL);
        /* [cod2-native] 2026-11-24: the IW bloom and sky bleed (make_bloom); null handles where not made */
        vkDestroyPipeline(vk.device, vk.bloom_first_pipeline, NULL);
        vkDestroyPipeline(vk.device, vk.bloom_blur_pipeline, NULL);
        vkDestroyPipeline(vk.device, vk.bloom_bleed_pipeline, NULL);
        vkDestroyPipelineLayout(vk.device, vk.bloom_layout, NULL);
        vkDestroyPipelineLayout(vk.device, vk.glow_layout, NULL);
        vkDestroyDescriptorPool(vk.device, vk.glow_pool, NULL);
        vkDestroySampler(vk.device, vk.glow_sampler, NULL);
        for (uint32_t i = 0; i < 2; i++) {
            vkDestroyFramebuffer(vk.device, vk.glow_framebuffer[i], NULL);
            vkDestroyImageView(vk.device, vk.glow_view[i], NULL);
            vkDestroyImage(vk.device, vk.glow_image[i], NULL);
            vkFreeMemory(vk.device, vk.glow_memory[i], NULL);
        }
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
    msaa_recipe.kept = 0;   /* [rexglue-native thread2] 2026-11-24: its layout went with the device's parts */
    memset(&vk, 0, sizeof vk);
    device_name[0] = '\0';
}
