/* [acct2-bro] Big Red One port: NGL's quad batch, natively: the effects' particles. See ngl_quad_batch.c. */
#pragma once
#include <stdint.h>

/* How an effect's quads go onto what's behind them (the same numbers as ngl_vk's NGL_VK_BLEND_*). */
enum {
    NGL_QUAD_BLEND_ALPHA,       /* by their alpha */
    NGL_QUAD_BLEND_ADD,         /* added */
    NGL_QUAD_BLEND_SUBTRACT,    /* taken away (darkening) */
};

/* One nglQuadBatch_Draw: count of this frame's quads from first, with the material and the view it was drawn with. */
struct ngl_quad_batch {
    uint32_t texture;           /* the nglTexture, or 0 */
    uint32_t blend;             /* NGL_QUAD_BLEND_* */
    uint32_t first, count;
    float view_projection[16];  /* the scene's, into Direct3D's clip space (as a mesh draw's) */
    float fog[3];               /* the scene's fog, as a mesh draw's */
    uint32_t fog_color;
};

/* The stand-ins (bro/config/stand_in_functions.tsv): nglQuadBatch_SetMaterial(texture, blend, z, alpha reference)
 * (0x0009D5B0); nglQuadBatch_Reserve(quads) (0x0009D200), the guest address the effect writes that many quads to, or
 * 0 when there is no room; nglQuadBatch_Draw() (0x0009D240), the reserved quads drawn. */
void ngl_quad_batch_set_material(uint32_t texture, uint32_t blend, uint32_t z, uint32_t alpha_reference);
uint32_t ngl_quad_batch_reserve(uint32_t count);
void ngl_quad_batch_draw(void);

/* RE_AddPolyToScene(shader, corner count, corners) (0x0023EE60), its stand-in: a four-corner polygon (a tracer) as one
 * quad of the frame's, in the current scene's view. */
void ngl_quad_batch_add_poly(uint32_t shader, uint32_t corner_count, uint32_t corners);

/* NGL's rendering of the effects' nodes in the frame's scenes (root and its children): each effect op node's Render
 * called as nglRenderSceneTree would, which makes its quads through the stand-ins above. Before the frame is drawn. */
void ngl_quad_batch_render_effects(uint32_t root);

/* This frame's draws, in the order they came. */
unsigned ngl_quad_batch_frame(const struct ngl_quad_batch **list);

/* A draw's corners for ngl_vk (NGL_CORNER_FLOATS floats each, as a mesh section's: position, texture coordinates, no
 * lightmap coordinates, the color, no normal), four a quad, at most max_corners. Returns how many. */
unsigned ngl_quad_batch_corners(const struct ngl_quad_batch *b, float *out, unsigned max_corners);

/* The frame is over (after it is drawn): the room and the list start over. */
void ngl_quad_batch_end_frame(void);
