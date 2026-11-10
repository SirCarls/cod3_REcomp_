/* [acct2-bro] Big Red One port: the quads each frame queues.
 *
 * NGL draws all 2D (menus, HUD, text, the movie) as quads. The game fills a quad in with nglInitQuad and the
 * nglSetQuad* functions, which are the game's own code here (they only write the quad's fields), then hands it to
 * nglListAddQuad (0x0005E270), which copies it onto the current scene's list for drawing. nglListAddQuad is a
 * stand-in: this file keeps the copy instead, so the frame's end can draw the quads through ngl_vk.
 *
 * The order they are drawn in is NGL's, not the order they came: nglListAddQuad puts a quad with blend mode 0 or 1
 * on the scene's opaque list and any other on its translucent list, keyed by its depth (+0x50); text
 * (nglListAddText, 0x000661C0) always goes on the translucent list, keyed by its z. nglRenderScene draws the opaque
 * list, then the translucent one sorted (std_InsertionSort_TranslucentNodes 0x000748B0 shows the order) by key from
 * largest to smallest, the nodes with one key in the order they came. So a full-screen movie quad queued after the
 * menu still goes under it when its depth is larger. Scenes (one list each, child scenes drawn first) aren't kept
 * apart yet: everything is in one list.
 *
 * The quad's layout (dev/bro-decomp/NGL-SPEC.md, the same in CoD3): four vertices of 0x14 bytes (x, y, u, v,
 * color), then depth +0x50, flags +0x54, blend +0x58 / +0x5C, texture +0x60. 0x64 bytes in all. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "recomp_types.h"
#include "ngl_quad.h"

#define QUAD_VERTEX_BYTES   0x14u
#define QUAD_Z              0x50u
#define QUAD_FLAGS          0x54u
#define QUAD_BLEND          0x58u
#define QUAD_BLEND_ARG      0x5Cu
#define QUAD_TEXTURE        0x60u

#define MAX_QUADS           8192u       /* a frame that queues more draws the first this many */
#define QUADS_DESCRIBED     4           /* quads written out in full to the log, the first ones queued */
#define FRAMES_PER_SUMMARY  300ul

static struct ngl_quad quads[MAX_QUADS];        /* in the order they came */
static unsigned char translucent[MAX_QUADS];    /* on the translucent list (else the opaque one) */
static unsigned quad_count;
static struct ngl_quad sorted[MAX_QUADS];       /* in the order they are drawn */
static unsigned order[MAX_QUADS];
static int sorted_count = -1;                   /* -1: not sorted since the last quad came */
static unsigned long frames;

static float guest_float(uint32_t address)
{
    uint32_t bits = MEM32(address);
    float value;

    memcpy(&value, &bits, sizeof value);
    return value;
}

static void describe(const struct ngl_quad *q)
{
    fprintf(stderr, "[ngl] quad: (%.1f,%.1f) (%.1f,%.1f) (%.1f,%.1f) (%.1f,%.1f) uv (%.3f,%.3f)-(%.3f,%.3f) "
            "color %08X z %.3f flags 0x%X blend %u/%u texture 0x%08X\n",
            q->x[0], q->y[0], q->x[1], q->y[1], q->x[2], q->y[2], q->x[3], q->y[3], q->u[0], q->v[0], q->u[3], q->v[3],
            q->color[0], q->z, q->flags, q->blend, q->blend_arg, q->texture);
}

void ngl_quad_add(uint32_t quad)
{
    static int described;
    static int overflow_reported;
    struct ngl_quad *q;
    int i;

    if (!quad)
        return;
    if (quad_count == MAX_QUADS) {
        if (!overflow_reported++)
            fprintf(stderr, "[ngl] more than %u quads in a frame; the rest are not drawn\n", MAX_QUADS);
        return;
    }
    q = &quads[quad_count++];
    for (i = 0; i < 4; i++) {
        uint32_t vertex = quad + (uint32_t)i * QUAD_VERTEX_BYTES;

        q->x[i] = guest_float(vertex);
        q->y[i] = guest_float(vertex + 4);
        q->u[i] = guest_float(vertex + 8);
        q->v[i] = guest_float(vertex + 12);
        q->color[i] = MEM32(vertex + 16);
    }
    q->z = guest_float(quad + QUAD_Z);
    q->flags = MEM32(quad + QUAD_FLAGS);
    q->blend = MEM32(quad + QUAD_BLEND);
    q->blend_arg = MEM32(quad + QUAD_BLEND_ARG);
    q->texture = MEM32(quad + QUAD_TEXTURE);
    translucent[quad_count - 1] = ((1u << (q->blend & 31)) & 3u) == 0;
    sorted_count = -1;
    if (described < QUADS_DESCRIBED) {
        described++;
        describe(q);
    }
}

void ngl_quad_add_host(const struct ngl_quad *quad)
{
    static int overflow_reported;

    if (quad_count == MAX_QUADS) {
        if (!overflow_reported++)
            fprintf(stderr, "[ngl] more than %u quads in a frame; the rest are not drawn\n", MAX_QUADS);
        return;
    }
    translucent[quad_count] = 1;
    quads[quad_count++] = *quad;
    sorted_count = -1;
}

/* Opaque quads before translucent ones; translucent ones by depth, largest first; otherwise in the order they came. */
static int draw_order(const void *a, const void *b)
{
    unsigned i = *(const unsigned *)a, j = *(const unsigned *)b;

    if (translucent[i] != translucent[j])
        return translucent[i] ? 1 : -1;
    if (translucent[i] && quads[i].z != quads[j].z)
        return quads[i].z > quads[j].z ? -1 : 1;
    return i < j ? -1 : i > j ? 1 : 0;
}

unsigned ngl_quad_frame(const struct ngl_quad **list)
{
    unsigned i;

    if (sorted_count != (int)quad_count) {
        for (i = 0; i < quad_count; i++)
            order[i] = i;
        qsort(order, quad_count, sizeof *order, draw_order);
        for (i = 0; i < quad_count; i++)
            sorted[i] = quads[order[i]];
        sorted_count = (int)quad_count;
    }
    *list = sorted;
    return quad_count;
}

void ngl_quad_end_frame(void)
{
    if (++frames % FRAMES_PER_SUMMARY == 0)
        fprintf(stderr, "[ngl] frame %lu: %u quads\n", frames, quad_count);
    quad_count = 0;
    sorted_count = -1;
}
