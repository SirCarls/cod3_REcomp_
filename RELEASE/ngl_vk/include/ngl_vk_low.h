/* NGL on Vulkan, the lower layer: for renderers that bring their own shaders. [acct2-waw, 2026-11-07]
 *
 * ngl_vk.h draws NGL's own things (quads, meshes with NGL's shading, the sun's shadow, grass, post effects) with
 * ngl_vk's shaders. The IW3 games (World at War and Call of Duty 4, dev/agents/acct2-waw/NATIVE-RENDERER.md) draw
 * with the game's own shaders instead: each Xbox 360 shader is translated once, offline, to SPIR-V, and every draw
 * names its shaders, its render states, its buffers, its float4 constants and its textures. This layer is that much
 * Vulkan and no more. It shares ngl_vk's device, frame and target (ngl_vk_backend.h: ngl_vk_backend,
 * ngl_vk_begin_frame, ngl_vk_end_frame), so ngl_vk's window, present and the game's gamma are used as they are. It
 * never submits: ngl_vk_end_frame does, under ngl_vk's queue lock when the device is shared.
 *
 * Handles are numbers, 1 and up; 0 means none or failure. Formats are VkFormat values and most enums below are
 * Vulkan's own values (each says so), so no Vulkan header is needed to use this file.
 *
 * Threads: every call is made on the thread that draws (the one ngl_vk's frame runs on).
 *
 * The translated shaders' bindings (iw3_shader_cache writes them this way), descriptor set 0:
 *   binding 0  uniform buffer: the vertex shader's float4 constants, c0 and up (the 360 has 256)
 *   binding 1  uniform buffer: the pixel shader's float4 constants, c0 and up (256)
 *   binding 2  uniform buffer: the bool constants (8 words, 256 bits) then the loop constants (32 words)
 *   binding 8 + n, n = 0..15: combined image sampler for the shader's texture fetch slot n
 * Vertex inputs are at the locations the vertex layout below gives (the translator numbers them by fetch). */
#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------------------------------------------------
 * Start and stop */

/* Starts the layer on ngl_vk's device (ngl_vk_backend()). Called after ngl_vk_init or ngl_vk_init_on_device.
 * Returns 0 when it is ready. */
int ngl_vk_low_init(void);

/* Frees everything this layer made. Called once ngl_vk has waited for its last frame. */
void ngl_vk_low_shutdown(void);

/* A frame: the start takes ngl_vk's command buffer (ngl_vk_begin_frame) and resets this frame slot's memory; the end
 * closes any open pass, records the writes still waiting, leaves ngl_vk's target as ngl_vk wants it and hands the
 * frame to ngl_vk (ngl_vk_end_frame: submit and show). Draws, passes and copies happen between the two. The end
 * returns ngl_vk_end_frame's result. */
void ngl_vk_low_frame_begin(void);
int ngl_vk_low_frame_end(void);

/* ---------------------------------------------------------------------------------------------------------------
 * Shaders */

enum ngl_vk_low_stage {
    NGL_VK_LOW_VERTEX_SHADER = 0x01,   /* VK_SHADER_STAGE_VERTEX_BIT */
    NGL_VK_LOW_PIXEL_SHADER = 0x10,    /* VK_SHADER_STAGE_FRAGMENT_BIT */
};

/* A shader module from word_count SPIR-V words (made offline; nothing is compiled while a game runs). The entry
 * point is "main". Returns its handle. */
uint32_t ngl_vk_low_shader(enum ngl_vk_low_stage stage, const uint32_t *spirv, size_t word_count);

/* ---------------------------------------------------------------------------------------------------------------
 * Vertex layouts */

#define NGL_VK_LOW_MAX_ATTRIBUTES 16
#define NGL_VK_LOW_MAX_STREAMS 4

/* One vertex input: the shader's location, which stream it comes from, its byte offset in that stream's vertex, and
 * its format (a VkFormat: the 360's vertex formats map onto these, for example FLOAT3 to R32G32B32_SFLOAT, DEC3N to
 * A2B10G10R10_SNORM_PACK32 after a swap, UBYTE4N to R8G8B8A8_UNORM). */
struct ngl_vk_low_attribute {
    uint32_t location;
    uint32_t stream;
    uint32_t offset;
    uint32_t format;
};

/* The vertex layout of a pipeline: attribute_count attributes, and for each of stream_count streams its vertex size
 * in bytes and whether it steps per instance (1) or per vertex (0). */
struct ngl_vk_low_layout {
    struct ngl_vk_low_attribute attributes[NGL_VK_LOW_MAX_ATTRIBUTES];
    uint32_t attribute_count;
    uint32_t stride[NGL_VK_LOW_MAX_STREAMS];
    uint32_t per_instance[NGL_VK_LOW_MAX_STREAMS];
    uint32_t stream_count;
};

/* ---------------------------------------------------------------------------------------------------------------
 * Render states (what IW3's two state-bit words turn into; the game side does that translation) */

enum ngl_vk_low_blend_factor {      /* VkBlendFactor */
    NGL_VK_LOW_ZERO = 0, NGL_VK_LOW_ONE = 1,
    NGL_VK_LOW_SRC_COLOR = 2, NGL_VK_LOW_ONE_MINUS_SRC_COLOR = 3,
    NGL_VK_LOW_DST_COLOR = 4, NGL_VK_LOW_ONE_MINUS_DST_COLOR = 5,
    NGL_VK_LOW_SRC_ALPHA = 6, NGL_VK_LOW_ONE_MINUS_SRC_ALPHA = 7,
    NGL_VK_LOW_DST_ALPHA = 8, NGL_VK_LOW_ONE_MINUS_DST_ALPHA = 9,
};

enum ngl_vk_low_blend_op {          /* VkBlendOp */
    NGL_VK_LOW_ADD = 0, NGL_VK_LOW_SUBTRACT = 1, NGL_VK_LOW_REVERSE_SUBTRACT = 2, NGL_VK_LOW_MIN = 3, NGL_VK_LOW_MAX = 4,
};

enum ngl_vk_low_compare {           /* VkCompareOp */
    NGL_VK_LOW_NEVER = 0, NGL_VK_LOW_LESS = 1, NGL_VK_LOW_EQUAL = 2, NGL_VK_LOW_LESS_OR_EQUAL = 3,
    NGL_VK_LOW_GREATER = 4, NGL_VK_LOW_NOT_EQUAL = 5, NGL_VK_LOW_GREATER_OR_EQUAL = 6, NGL_VK_LOW_ALWAYS = 7,
};

enum ngl_vk_low_cull {              /* VkCullModeFlags; the front face is clockwise, as Direct3D's */
    NGL_VK_LOW_CULL_NONE = 0, NGL_VK_LOW_CULL_FRONT = 1, NGL_VK_LOW_CULL_BACK = 2,
};

enum ngl_vk_low_topology {          /* zero is triangle lists */
    NGL_VK_LOW_TRIANGLE_LIST = 0, NGL_VK_LOW_TRIANGLE_STRIP = 1, NGL_VK_LOW_LINE_LIST = 2, NGL_VK_LOW_POINT_LIST = 3,
};

/* Everything about a draw that a pipeline fixes. Zero-filled is: no blending, all colors written, no depth test or
 * write, no culling, triangle lists, no stencil, no depth bias. */
struct ngl_vk_low_state {
    uint32_t blend;                 /* 1: blend with the factors below */
    enum ngl_vk_low_blend_factor color_src, color_dst;
    enum ngl_vk_low_blend_op color_op;
    enum ngl_vk_low_blend_factor alpha_src, alpha_dst;
    enum ngl_vk_low_blend_op alpha_op;
    uint32_t color_mask;            /* bits 1 red, 2 green, 4 blue, 8 alpha; 0 here means all four */
    uint32_t no_color_write;        /* 1: write no color at all (depth prepass, shadow maps) */
    uint32_t depth_test;            /* 1: test against the depth target with depth_compare */
    uint32_t depth_write;
    enum ngl_vk_low_compare depth_compare;
    enum ngl_vk_low_cull cull;
    enum ngl_vk_low_topology topology;
    float depth_bias;               /* constant and slope-scaled depth bias (polygon offset, shadow maps, decals) */
    float depth_bias_slope;
    uint32_t stencil;               /* 1: the stencil test below is on (the same for both faces) */
    enum ngl_vk_low_compare stencil_compare;
    uint32_t stencil_fail, stencil_depth_fail, stencil_pass;   /* VkStencilOp */
    uint32_t stencil_reference, stencil_read_mask, stencil_write_mask;
    uint32_t alpha_to_coverage;
};

/* ---------------------------------------------------------------------------------------------------------------
 * Pipelines */

/* The color and depth formats a pipeline draws into (up to four colors; depth_format 0 for none) and their sample
 * count (1 for none). */
struct ngl_vk_low_formats {
    uint32_t color[4];
    uint32_t color_count;
    uint32_t depth_format;
    uint32_t samples;
};

/* A pipeline for these two shaders, this vertex layout, these states and these target formats. Pipelines are kept by
 * what they were made from: asking again for the same returns the same handle without making another. Returns its
 * handle. */
uint32_t ngl_vk_low_pipeline(uint32_t vertex_shader, uint32_t pixel_shader, const struct ngl_vk_low_layout *layout,
                             const struct ngl_vk_low_state *state, const struct ngl_vk_low_formats *formats);

/* ---------------------------------------------------------------------------------------------------------------
 * Buffers */

enum ngl_vk_low_buffer_use {
    NGL_VK_LOW_VERTICES = 1,
    NGL_VK_LOW_INDICES = 2,
};

/* A buffer of size bytes on the GPU for vertices or indices, kept until freed (a level's world, its models). Returns
 * its handle. */
uint32_t ngl_vk_low_buffer(enum ngl_vk_low_buffer_use use, size_t size);

/* Copies size bytes into the buffer at offset. The copy reaches the GPU before the next frame's draws. Returns 0 when
 * it is queued. */
int ngl_vk_low_buffer_write(uint32_t buffer, size_t offset, const void *data, size_t size);

void ngl_vk_low_buffer_free(uint32_t buffer);

/* Room for this frame only (the tess buffer's vertices and indices, skinned vertices): returns where to write size
 * bytes, and in *buffer and *offset what to name in this frame's draws. NULL when the frame's room is used up. The room
 * is reused once the GPU has finished the frame. */
void *ngl_vk_low_frame_room(enum ngl_vk_low_buffer_use use, size_t size, uint32_t *buffer, size_t *offset);

/* ---------------------------------------------------------------------------------------------------------------
 * Textures and samplers */

/* A texture: width x height (x depth for a volume, 6 layers for a cube), level_count levels, in format. Returns its
 * handle, or 0 for a BC format (DXT1-5) on a device made without textureCompressionBC: decompress first then. The
 * game side untiles the 360's data and swaps its byte order before writing. */
enum ngl_vk_low_texture_kind { NGL_VK_LOW_2D = 0, NGL_VK_LOW_3D = 1, NGL_VK_LOW_CUBE = 2 };
uint32_t ngl_vk_low_texture(enum ngl_vk_low_texture_kind kind, uint32_t format, uint32_t width, uint32_t height,
                            uint32_t depth, uint32_t level_count);

/* Writes one level of one layer (a cube's face 0-5; 0 otherwise): size bytes, rows row_pitch bytes apart (for block
 * formats, a row of 4x4 blocks). Reaches the GPU before the next frame's draws. Returns 0 when it is queued. */
int ngl_vk_low_texture_write(uint32_t texture, uint32_t level, uint32_t layer, const void *data, size_t size,
                             uint32_t row_pitch);

void ngl_vk_low_texture_free(uint32_t texture);

/* A sampler. filter: 0 nearest, 1 linear; mip: 0 nearest, 1 linear, 2 none; address (u, v, w): VkSamplerAddressMode
 * (0 repeat, 1 mirrored repeat, 2 clamp to edge, 3 clamp to border); anisotropy 1 for none (taken as 1 on a device
 * made without samplerAnisotropy); compare 1 for a shadow map's depth compare (less or equal). Samplers are kept by
 * what they were made from. Returns its handle. */
uint32_t ngl_vk_low_sampler(uint32_t filter, uint32_t mip, uint32_t address_u, uint32_t address_v, uint32_t address_w,
                            float anisotropy, uint32_t compare);

/* ---------------------------------------------------------------------------------------------------------------
 * Render targets and passes */

/* A picture to draw into that can later be sampled as a texture (shadow maps, the float Z picture, post effects):
 * width x height in format, samples 1 or more; depth 1 for a depth (or depth and stencil) format. Returns a texture
 * handle that passes and draws both take. */
uint32_t ngl_vk_low_target(uint32_t format, uint32_t width, uint32_t height, uint32_t samples, uint32_t depth);

/* ngl_vk's own main color and depth targets, as handles of this layer (what the game's frame ends up in). */
uint32_t ngl_vk_low_main_color(void);
uint32_t ngl_vk_low_main_depth(void);

/* A pass: the targets the draws go into, and what to clear first. */
struct ngl_vk_low_pass {
    uint32_t color[4];              /* target handles; color_count of them */
    uint32_t color_count;
    uint32_t depth;                 /* a depth target, or 0 */
    uint32_t clear_color;           /* 1: clear the colors to clear_rgba */
    float clear_rgba[4];
    uint32_t clear_depth;           /* 1: clear depth to clear_depth_value and stencil to clear_stencil_value */
    float clear_depth_value;
    uint32_t clear_stencil_value;
    const char *label;              /* a debug label for the pass (RenderDoc and the like), or NULL */
};

/* Starts a pass; every draw until ngl_vk_low_end_pass goes into its targets. The viewport and scissor start as the
 * whole first target. Returns 0 when it started. */
int ngl_vk_low_begin_pass(const struct ngl_vk_low_pass *pass);
void ngl_vk_low_end_pass(void);

/* The viewport (min_depth..max_depth, 0..1) and scissor rectangle, in the pass's target pixels. */
void ngl_vk_low_viewport(float x, float y, float width, float height, float min_depth, float max_depth);
void ngl_vk_low_scissor(int32_t x, int32_t y, uint32_t width, uint32_t height);

/* Copies a rectangle of one target into a texture (the 360's resolve), outside any pass: from from_x, from_y of
 * target, width x height, to to_x, to_y of level 0 of texture. Multisampled targets are resolved. Returns 0 when it
 * is recorded. */
int ngl_vk_low_copy(uint32_t target, int32_t from_x, int32_t from_y, uint32_t width, uint32_t height,
                    uint32_t texture, int32_t to_x, int32_t to_y);

/* ---------------------------------------------------------------------------------------------------------------
 * Draws */

#define NGL_VK_LOW_MAX_TEXTURES 16

/* One draw: the pipeline, the streams, the indices, the constants and the textures. Constants are copied when the
 * draw is recorded, so the arrays can change right after. */
struct ngl_vk_low_draw {
    uint32_t pipeline;
    uint32_t vertex_buffer[NGL_VK_LOW_MAX_STREAMS];   /* buffer handles (kept or this frame's), one per stream */
    size_t vertex_offset[NGL_VK_LOW_MAX_STREAMS];
    uint32_t index_buffer;          /* 0: draw vertex_count vertices without indices */
    size_t index_offset;
    uint32_t index_32bit;           /* 0: 16-bit indices, 1: 32-bit */
    uint32_t first_index, index_count;
    int32_t base_vertex;
    uint32_t vertex_count;          /* when there is no index buffer */
    uint32_t instance_count;        /* 0 means 1 */
    const float *vertex_constants;  /* vertex_constant_count float4s (c0 and up) */
    uint32_t vertex_constant_count;
    const float *pixel_constants;   /* pixel_constant_count float4s */
    uint32_t pixel_constant_count;
    const uint32_t *bool_loop_constants;   /* 8 bool words then 32 loop words, or NULL for zeros */
    uint32_t texture[NGL_VK_LOW_MAX_TEXTURES];    /* per fetch slot: a texture or target handle, or 0 */
    uint32_t sampler[NGL_VK_LOW_MAX_TEXTURES];
};

/* Records the draw into the current pass. Returns 0 when it is recorded. */
int ngl_vk_low_draw(const struct ngl_vk_low_draw *draw);

/* ---------------------------------------------------------------------------------------------------------------
 * Debug labels and counts */

/* Nested labels around groups of draws, seen in RenderDoc and in validation messages. */
void ngl_vk_low_label_begin(const char *name);
void ngl_vk_low_label_end(void);

/* This frame's counts so far (for the census and the side-by-side comparison). */
struct ngl_vk_low_counts {
    uint32_t passes, draws, pipelines_made, pipeline_binds;
    uint64_t indices;
    size_t frame_room_used;
};
void ngl_vk_low_frame_counts(struct ngl_vk_low_counts *counts);

#ifdef __cplusplus
}
#endif
