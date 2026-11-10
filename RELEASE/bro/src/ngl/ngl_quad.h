/* [acct2-bro] Big Red One port: the quads (2D: menus, HUD, text) each frame queues. See ngl_quad.c. */
#pragma once
#include <stdint.h>

/* One quad as the game filled it in (nglInitQuad, nglSetQuad*), vertices in order 0-3. */
struct ngl_quad {
    float x[4], y[4];           /* screen position */
    float u[4], v[4];           /* texture coordinates */
    uint32_t color[4];          /* D3DCOLOR: 0xAARRGGBB */
    float z;                    /* depth (nglSetQuadZ) */
    uint32_t flags;             /* nglSetQuadFlags */
    uint32_t blend, blend_arg;  /* nglSetQuadBlend */
    uint32_t texture;           /* the nglTexture, a guest address; 0 for none */
};

/* What nglListAddQuad does with the quad at guest address quad: a copy for this frame's drawing. */
void ngl_quad_add(uint32_t quad);

/* A quad made natively (text: one a glyph), for this frame's drawing; on the translucent list, keyed by its z. */
void ngl_quad_add_host(const struct ngl_quad *quad);

/* This frame's quads, in the order NGL draws them (see ngl_quad.c). */
unsigned ngl_quad_frame(const struct ngl_quad **quads);

/* The frame is over (after it is drawn): starts the next frame's list. */
void ngl_quad_end_frame(void);
