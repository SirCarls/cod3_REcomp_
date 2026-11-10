/* [acct2-bro] Big Red One port: NGL's quad batch, natively: the effects' particles (smoke, dust, sparks, muzzle
 * flashes, tracers).
 *
 * The effects draw while the game builds the frame (FX_DrawEffects 0x00246810, from R_RenderView): each effect op's
 * DrawQuads (FxOp53_DrawQuads 0x000A7930 and five more, called through their vtables) sets a material with
 * nglQuadBatch_SetMaterial (0x0009D5B0), asks nglQuadBatch_Reserve (0x0009D200) for room for n quads, writes them
 * there itself and calls nglQuadBatch_Draw (0x0009D240). In NGL the room is the locked quad vertex buffer and the draw
 * is Direct3D's DrawIndexedVertices (a triangle list, six indices a quad) under the effects' vertex shader. Until now
 * the stand-ins gave no room, which the effects take as the buffer being full, so no particle was drawn.
 *
 * A quad is four corners of 24 bytes, as FxOp53_DrawQuads writes them: the position in the world (three floats), a
 * D3DCOLOR and the texture's u, v. Corners 0 and 1 share one edge and 2 and 3 the opposite one (0 and 2 are across the
 * quad from 1 and 3), so the triangles are 0 1 2 and 2 1 3.
 *
 * Here the room is guest memory of this port's own, started over each frame as NGL's is; each draw keeps its quads'
 * place, the material, and the current scene's view, projection and fog (ngl_mesh_current_scene), and ngl_present.c
 * draws them after the meshes, blended in the order they came, without writing depth.
 *
 * The quads are made when NGL draws the scene, not when the game builds it: an effect op's AddToList (FxOp53_AddToList
 * 0x0009F0F0 and the rest) puts a node in the scene's lists (FxNode_AddToScene 0x0009E490: the translucent list at
 * scene +0x308, by the node's view depth at +0x08, or for an effect so marked the opaque list at +0x304), and NGL's
 * nglRenderSceneTree (0x00074330) later calls each node's Render, its vtable's first entry, with the node in ecx
 * (nglRenderOpaqueList 0x00075460: mov ecx, esi; call [edx]). The effect ops' Render functions are short thunks
 * (FUN_000a7920: the node's address passed on to FxOp53_DrawQuads) beside their DrawQuads. NGL's list rendering
 * isn't this port's, so ngl_quad_batch_render_effects does that part of it: before the frame is drawn it walks the
 * scenes as nglRenderSceneTree does (children first), and calls the Render of each node whose Render lies in the
 * effect ops' code, the opaque list's in its order and the translucent list's largest key first, with the node's
 * scene made current.
 *
 * nglQuadBatch_SetMaterial(texture, blend, z, alpha reference): the texture is an nglTexture (it sets it on stage 0);
 * blend 0 and 3 are alpha blending (source alpha, one minus source alpha), 1 adds (source alpha, one), 2 is the blend
 * equation 0x800B, reverse subtract (behind - color x alpha: darkening). It also turns alpha testing on (greater than
 * the reference); that isn't done here, since what it drops is nearly see-through. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "recomp_types.h"
#include "xbox_memory_layout.h"     /* xbox_HeapAlloc, XBOX_PTR: guest memory */
#include "ngl_mesh.h"               /* ngl_mesh_current_scene, NGL_CORNER_FLOATS */
#include "ngl_quad_batch.h"

#define NGL_CUR_SCENE       0x0090259Cu
#define SCENE_NEXT_SIBLING  0x2D4u
#define SCENE_FIRST_CHILD   0x2D8u
#define SCENE_OPAQUE        0x304u      /* node lists: +0x04 the next node, +0x08 the key */
#define SCENE_TRANSLUCENT   0x308u
#define NODE_NEXT           0x04u
#define NODE_KEY            0x08u
#define FX_RENDER_FIRST     0x000A66A0u /* the effect ops' Render thunks and draws: FxOp31_Draw 0x000A66B0 to */
#define FX_RENDER_END       0x000AC1B0u /* FxOp15b_DrawQuads's end */
#define MAX_SCENE_DEPTH     16
#define MAX_EFFECT_NODES    4096u

#define MAX_QUADS       8192u       /* a frame's quads (16-bit indices reach 16384) */
#define CORNER_BYTES    24u
#define QUAD_BYTES      (4u * CORNER_BYTES)
#define MAX_BATCHES     2048u
#define MATERIALS_SHOWN 12
#define REPORT_FRAMES   600u

static uint32_t room;               /* guest memory for MAX_QUADS quads */
static int no_room;                 /* the memory couldn't be had */
static uint32_t used;               /* quads drawn this frame */
static uint32_t reserved;           /* quads the last nglQuadBatch_Reserve gave room for */
static uint32_t texture, blend;     /* the material */
static struct ngl_quad_batch batches[MAX_BATCHES];
static unsigned batch_count;
static unsigned long frames, quads_drawn, quads_refused, draws_dropped, nodes_rendered, nodes_dropped, polys_drawn;
static struct { uint32_t node; float key; } effect_nodes[MAX_EFFECT_NODES];

void ngl_quad_batch_set_material(uint32_t new_texture, uint32_t new_blend, uint32_t z, uint32_t alpha_reference)
{
    static int shown;

    texture = new_texture;
    blend = new_blend == 1 ? NGL_QUAD_BLEND_ADD : new_blend == 2 ? NGL_QUAD_BLEND_SUBTRACT : NGL_QUAD_BLEND_ALPHA;
    if (shown < MATERIALS_SHOWN) {
        shown++;
        fprintf(stderr, "[fx] quad material: texture 0x%08X, blend %u, z %u, alpha reference %u\n", new_texture,
                new_blend, z & 0xFFu, alpha_reference);
    }
}

uint32_t ngl_quad_batch_reserve(uint32_t count)
{
    reserved = 0;
    if (!room && !no_room) {
        room = xbox_HeapAlloc(MAX_QUADS * QUAD_BYTES, 16);
        no_room = !room;
        if (no_room)
            fprintf(stderr, "[fx] no guest memory for the effects' quads: no particles\n");
    }
    if (!room || !count || count > MAX_QUADS - used) {
        quads_refused += count;
        return 0;
    }
    reserved = count;
    return room + used * QUAD_BYTES;
}

void ngl_quad_batch_draw(void)
{
    struct ngl_mesh_draw view;
    struct ngl_quad_batch *b;

    if (!reserved)
        return;
    if (batch_count == MAX_BATCHES || !ngl_mesh_current_scene(&view)) {
        draws_dropped++;
        reserved = 0;
        return;
    }
    b = &batches[batch_count++];
    b->texture = texture;
    b->blend = blend;
    b->first = used;
    b->count = reserved;
    memcpy(b->view_projection, view.view_projection, sizeof b->view_projection);
    memcpy(b->fog, view.fog, sizeof b->fog);
    b->fog_color = view.fog_color;
    used += reserved;
    quads_drawn += reserved;
    reserved = 0;
}

unsigned ngl_quad_batch_frame(const struct ngl_quad_batch **list)
{
    *list = batches;
    return batch_count;
}

unsigned ngl_quad_batch_corners(const struct ngl_quad_batch *b, float *out, unsigned max_corners)
{
    unsigned n = b->count * 4u;

    if (!room)
        return 0;
    if (n > max_corners)
        n = max_corners & ~3u;
    for (unsigned i = 0; i < n; i++) {
        const uint8_t *c = (const uint8_t *)XBOX_PTR(room + (b->first * 4u + i) * CORNER_BYTES);
        float *o = out + (size_t)i * NGL_CORNER_FLOATS;

        memcpy(o, c, 3 * sizeof(float));        /* x, y, z */
        memcpy(o + 3, c + 16, 2 * sizeof(float));   /* u, v */
        o[5] = o[6] = 0.0f;                     /* no lightmap */
        memcpy(o + 7, c + 12, sizeof(float));   /* the D3DCOLOR's bytes */
        o[8] = o[9] = o[10] = 0.0f;             /* no normal */
    }
    return n;
}

/* Guest memory this port maps: the low 64 MB and the contiguous window at 0x80000000 (as ngl_mesh.c reads). */
static int readable(uint32_t address)
{
    return (address >= 0x10000u && address < 0x04000000u) || (address >= 0x80000000u && address < 0x84000000u);
}

/* A node's Render if it is an effect op's, else 0. */
static uint32_t effect_render(uint32_t node)
{
    uint32_t vtable, render;

    if (!readable(node) || (node & 3u))
        return 0;
    vtable = MEM32(node);
    if (!readable(vtable))
        return 0;
    render = MEM32(vtable);
    return render >= FX_RENDER_FIRST && render < FX_RENDER_END ? render : 0;
}

/* Calls a node's Render as NGL does: the node in ecx, a return address pushed (its ret takes it off). Registers a
 * stand-in's caller may expect kept are put back, as ngl_frame_clock.c's calls do. */
static void render_node(uint32_t node, uint32_t render)
{
    static int reported;
    recomp_func_t fn = recomp_lookup(render);
    uint32_t eax = g_eax, ecx = g_ecx, edx = g_edx;

    if (!fn) {
        if (reported++ < 4)
            fprintf(stderr, "[fx] no recompiled code at 0x%08X (a node's Render)\n", render);
        return;
    }
    g_ecx = node;
    g_esp -= 4u;
    MEM32(g_esp) = render;
    fn();
    g_eax = eax;
    g_ecx = ecx;
    g_edx = edx;
    nodes_rendered++;
}

static int larger_key_first(const void *a, const void *b)
{
    float x = ((const float *)a)[1], y = ((const float *)b)[1];

    return x < y ? 1 : x > y ? -1 : 0;
}

/* One scene's effect nodes, its children's first. */
static void render_scene_effects(uint32_t scene, int depth)
{
    unsigned count = 0;

    if (!readable(scene) || depth > MAX_SCENE_DEPTH)
        return;
    for (uint32_t child = MEM32(scene + SCENE_FIRST_CHILD); readable(child); child = MEM32(child + SCENE_NEXT_SIBLING))
        render_scene_effects(child, depth + 1);
    MEM32(NGL_CUR_SCENE) = scene;
    for (uint32_t node = MEM32(scene + SCENE_OPAQUE), n = 0; node && n < MAX_EFFECT_NODES;
         node = MEM32(node + NODE_NEXT), n++) {
        uint32_t render = effect_render(node);

        if (!readable(node))
            break;
        if (render)
            render_node(node, render);
    }
    for (uint32_t node = MEM32(scene + SCENE_TRANSLUCENT), n = 0; node && n < MAX_EFFECT_NODES;
         node = MEM32(node + NODE_NEXT), n++) {
        uint32_t bits;

        if (!readable(node))
            break;
        if (!effect_render(node))
            continue;
        if (count == MAX_EFFECT_NODES) {
            nodes_dropped++;
            continue;
        }
        bits = MEM32(node + NODE_KEY);
        effect_nodes[count].node = node;
        memcpy(&effect_nodes[count++].key, &bits, sizeof bits);
    }
    qsort(effect_nodes, count, sizeof effect_nodes[0], larger_key_first);
    for (unsigned i = 0; i < count; i++)
        render_node(effect_nodes[i].node, effect_render(effect_nodes[i].node));
}

/* The renderer's polygons: RE_AddPolyToScene (0x0023EE60), the tracers' (CG_AddTracerLocalEntity). The game's makes
 * a mesh of one strip of the polygon's four corners with NGL's mesh builder (nglMeshBuilder_Begin and the rest) and
 * adds it with nglListAddMesh. The mesh builder is this port's stand-ins, so its allocator (the function pointer at
 * 0x00902928, which nglMeshBuilder_Begin sets) was never set: the call through it went nowhere and the corners were
 * written through whatever eax held, which corrupted the list of local entities (run 97: CG_AddLocalEntities read a
 * float as the next entity in Oran). Here a polygon is one quad of the frame's, drawn as the effects' are.
 *
 * A polygon corner (polyVert_t) is 32 bytes: the position (three floats), u and v at +0x0C, and the color's red,
 * green, blue and alpha bytes at +0x1C. The strip goes 0, 1, 3, 2, which is the quads' corner order. The game makes
 * the material with ShaderPCUV_InitMaterial (0x00047200: the shader at +0x1C, then 3 and 2); it is taken as the
 * effects' texture with alpha blending. Polygons of other sizes the game doesn't draw either. */
#define POLY_CORNER_BYTES   32u
#define POLYS_SHOWN         4

void ngl_quad_batch_add_poly(uint32_t shader, uint32_t corner_count, uint32_t corners)
{
    static const unsigned order[4] = { 0, 1, 3, 2 };
    static int shown;
    uint32_t effect_texture = texture, effect_blend = blend, at;

    if (corner_count != 4 || !readable(corners) || !readable(corners + 4u * POLY_CORNER_BYTES - 1u))
        return;
    if (shown < POLYS_SHOWN) {
        float first[3];

        shown++;
        memcpy(first, (const void *)XBOX_PTR(corners), sizeof first);
        fprintf(stderr, "[fx] polygon: shader 0x%08X, first corner %.1f %.1f %.1f\n", shader, first[0], first[1],
                first[2]);
    }
    texture = shader;
    blend = NGL_QUAD_BLEND_ALPHA;
    at = ngl_quad_batch_reserve(1);
    if (at) {
        for (unsigned i = 0; i < 4; i++) {
            const uint8_t *from = (const uint8_t *)XBOX_PTR(corners + order[i] * POLY_CORNER_BYTES);
            uint8_t *to = (uint8_t *)XBOX_PTR(at + i * CORNER_BYTES);

            memcpy(to, from, 3 * sizeof(float));        /* x, y, z */
            to[12] = from[0x1E];                        /* the D3DCOLOR: blue, green, red, alpha */
            to[13] = from[0x1D];
            to[14] = from[0x1C];
            to[15] = from[0x1F];
            memcpy(to + 16, from + 0x0C, 2 * sizeof(float));    /* u, v */
        }
        ngl_quad_batch_draw();
        polys_drawn++;
    }
    texture = effect_texture;
    blend = effect_blend;
}

void ngl_quad_batch_render_effects(uint32_t root)
{
    uint32_t current = MEM32(NGL_CUR_SCENE);

    if (!root)
        return;
    render_scene_effects(root, 0);
    MEM32(NGL_CUR_SCENE) = current;
}

void ngl_quad_batch_end_frame(void)
{
    if (++frames % REPORT_FRAMES == 0 && (quads_drawn || quads_refused || draws_dropped || nodes_rendered)) {
        fprintf(stderr, "[fx] last %u frames: %lu effect nodes rendered (%lu over the limit), %lu effect quads drawn "
                "(%lu of them polygons), %lu refused (room for %u a frame), %lu draws dropped\n", REPORT_FRAMES,
                nodes_rendered, nodes_dropped, quads_drawn, polys_drawn, quads_refused, MAX_QUADS, draws_dropped);
        quads_drawn = quads_refused = draws_dropped = nodes_rendered = nodes_dropped = polys_drawn = 0;
    }
    used = 0;
    reserved = 0;
    batch_count = 0;
}
