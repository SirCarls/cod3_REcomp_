/* [acct2-bro] Big Red One port: the picture each frame ends with, through ngl_vk (../../../ngl_vk).
 *
 * Until NGL's draws are rewritten on Vulkan, a frame is the last texture the game locked and wrote, then the frame's
 * quads (ngl_quad.c) on top. While a movie plays the locked texture is the back buffer, which the movie player decodes
 * each frame straight into (see ngl_texture.c), so it is the frame's background, as it was on the Xbox.
 *
 * Quads with a texture the game loaded (nglLoadTexture, ngl_texture_load.c) draw it, with its alpha; each texture is
 * handed to ngl_vk the first time a quad uses it. A quad whose texture has no pixels (one this port doesn't make yet)
 * is left out; BRO_VK_PLACEHOLDERS=1 draws those as faint boxes of their color, to see where they go.
 *
 * The frame's 3D meshes (nglListAddMesh, ngl_mesh.c) are drawn first, under the quads: each mesh section is handed to
 * ngl_vk once (its corners, base-texture coordinates and triangles), and drawn with the mesh's world matrix and its
 * scene's view and projection, in its material's base texture (or, without one, a color its material picks) under
 * one fixed light. A skinned section (a character's) gets this frame's corners for each draw, moved by the draw's
 * bones on the CPU (ngl_mesh_skinned_corners). Sections ngl_mesh.c can't read yet are counted at the dump frames.
 *
 * ngl_vk draws into a picture of its own (the target: 640 x 480 offscreen, 1280 x 960 with a window), which can be
 * saved to check it. When SDL3 has a display, the target is also shown in a window of its own, scaled to fit
 * (open_window: an SDL window made for Vulkan, its surface made by SDL_Vulkan_CreateSurface, on Windows from the
 * window's handle; the swapchain by ngl_vk); closing the window ends the game, as SIGINT and SIGTERM do with or without a window (check_quit). Without
 * a display (the test runs' sandbox) frames are drawn offscreen.
 *
 *   BRO_VK=1                    draw each frame through Vulkan (off by default while it is only this)
 *   BRO_WINDOW=0                no window even when there is a display (bro.toml: display.window)
 *   BRO_FULLSCREEN=1            the window fills the screen (display.fullscreen)
 *   BRO_VSYNC=0                 frames shown as they come (mailbox) instead of in step with the display
 *                               (display.vsync)
 *   BRO_MOUSE_LOOK=0            the mouse isn't captured for looking around (bro_pad.c)
 *   BRO_VK_SIZE=<w>x<h>         the target's size (the window shows it scaled to fit) (display.size)
 *   BRO_SHOW_FPS=1              the frame-rate counter over the picture and in the title (display.show_fps,
 *                               ngl_fps_counter.c)
 *   BRO_VK_PLACEHOLDERS=1       draw quads whose texture has no pixels yet as faint boxes
 *   BRO_VK_DUMP=<frame>[,<frame>...]  save what Vulkan drew at those frames (counting from 1) ...
 *   BRO_VK_DUMP_PATH=<file.ppm>       ... to this file; a %lu in the name becomes the frame's number */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "recomp_types.h"
#include "xbox_memory_layout.h"     /* XBOX_PTR: guest memory */
#include "ngl_texture.h"
#include "ngl_quad.h"
#include "ngl_texture_load.h"
#include "ngl_mesh.h"
#include "ngl_quad_batch.h"
#include "ngl_scene.h"
#include "ngl_fps_counter.h"
#include "bro_settings.h"
#include "ngl_present.h"

#if defined(BRO_HAVE_SDL3)
#include <SDL3/SDL.h>
#endif

#if defined(BRO_HAVE_SDL3)
static int window_clicked;      /* a mouse button went down since the last pump_window (check_quit notes it) */
#endif

/* SDL catches SIGINT and SIGTERM once any part of it is started (the window, the sound or the controllers) and turns
 * them into SDL_EVENT_QUIT, as it does closing the window. Only pump_window read SDL's queue, and only with a window,
 * so a game without one kept running when asked to quit. Checked every frame, with or without a window or Vulkan:
 * SDL_EVENT_QUIT ends the game. The queue's other events are dropped (a mouse click is noted first, for
 * pump_window), the window's own left for pump_window. */
static void check_quit(void)
{
#if defined(BRO_HAVE_SDL3)
    SDL_Event event;

    if (!SDL_WasInit(SDL_INIT_EVENTS))
        return;
    SDL_PumpEvents();
    if (SDL_PeepEvents(&event, 1, SDL_GETEVENT, SDL_EVENT_QUIT, SDL_EVENT_QUIT) > 0) {
        fprintf(stderr, "[ngl] asked to quit (the window was closed, or SIGINT or SIGTERM): ending\n");
        fflush(NULL);
        exit(0);
    }
    while (SDL_PeepEvents(&event, 1, SDL_GETEVENT, SDL_EVENT_MOUSE_BUTTON_DOWN, SDL_EVENT_MOUSE_BUTTON_DOWN) > 0)
        window_clicked = 1;
    SDL_FlushEvents(SDL_EVENT_FIRST, SDL_EVENT_WINDOW_FIRST - 1);
    SDL_FlushEvents(SDL_EVENT_WINDOW_LAST + 1, SDL_EVENT_LAST);
#endif
}

#ifdef BRO_HAVE_VULKAN
#include "ngl_vk.h"
#if defined(BRO_HAVE_SDL3)
#if defined(_WIN32)
#define VK_USE_PLATFORM_WIN32_KHR   /* vkCreateWin32SurfaceKHR, for open_window's surface */
#include <vulkan/vulkan.h>
#endif
#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#endif

#define SCREEN_WIDTH   640u
#define SCREEN_HEIGHT  480u
#define MAX_VERTICES   ((8192u + 1u) * 6u)     /* the background and every quad ngl_quad.c keeps */
#define PLACEHOLDER_ALPHA_SHIFT 2              /* placeholder boxes at a quarter of their color's alpha */

#define MAX_VK_TEXTURES 2048u
#define QUADS_A_TEXTURE_DESCRIBED 3u           /* at the dump frame, each texture's first quads are written out */
#define TEXTURES_DESCRIBED        64u
#define UNTEXTURED_QUADS_DESCRIBED 12u         /* and the first quads without a texture */

#define SECTION_SLOTS   65536u                 /* sections handed to ngl_vk; a power of two */
#define MAX_MESH_DRAWS  16384u                 /* sections drawn a frame (ngl_vk's limit) */
#define MAX_CASTERS     128u                   /* meshes in R_RenderTestShadow's scene a frame (run 133) */
#define MAX_ORDERED     64u                    /* sections a mesh may have to be drawn in layer order */
#define EFFECT_QUADS    8192u                  /* an effect draw's quads at most (ngl_quad_batch.c's frame) */

static struct ngl_vk_vertex vertices[MAX_VERTICES];
static struct {
    uint32_t section, data, vk, texture;    /* section -> ngl_vk's mesh, and its base texture's ngl_vk number */
    uint32_t lightmap;                      /* its lightmap's ngl_vk number, or 0 */
    uint32_t blend_texture, blend_light;    /* cdWorldBlend's second and third textures' ngl_vk numbers, or 0 */
    int shading, wants_lightmap;            /* NGL_SHADE_*; whether its class has a lightmap (or a cdWorldBlend
                                             * texture) not loaded yet */
    int alpha;                              /* NGL_ALPHA_* */
    int result;
} sections[SECTION_SLOTS];
static unsigned section_count, textured_sections;
static unsigned long skinned_draws, unskinned_draws;    /* draws of skinned sections, with and without their bones */
static float skin_corners[65536 * NGL_CORNER_FLOATS];   /* one skinned draw's corners */
static float foam_corners[65536 * NGL_CORNER_FLOATS];   /* an ocean draw's foam's corners */
static unsigned long section_results[NGL_SECTION_RESULTS];
static struct ngl_vk_mesh_draw mesh_draws[MAX_MESH_DRAWS];
static float effect_corners[EFFECT_QUADS * 4 * NGL_CORNER_FLOATS];   /* one effect draw's corners */
static unsigned long effect_draws, untextured_effect_draws;

/* At a dump frame, the effects' quads by texture and blend: how much of the screen they cover (each quad's projected
 * box as a share of the screen, overlaps counted again), how see-through their corners are, and how many reach
 * behind the eye (those aren't in the cover). */
#define EFFECT_GROUPS 32u
static struct {
    uint32_t texture, blend;
    unsigned long quads, behind_eye;
    double cover, alpha;
} effect_groups[EFFECT_GROUPS];
static unsigned effect_group_count;
static int describing_effects;
static unsigned long described_frame;     /* the frame being described */
static struct { uint32_t texture, vk; } vk_textures[MAX_VK_TEXTURES];    /* nglTexture -> ngl_vk's number */
static unsigned vk_texture_count;

/* ngl_vk's number for a texture the game loaded, made the first time; 0 if it has no pixels. */
static uint32_t vk_texture_for(uint32_t texture)
{
    const struct ngl_loaded_texture *loaded;
    unsigned i;

    for (i = 0; i < vk_texture_count; i++)
        if (vk_textures[i].texture == texture)
            return vk_textures[i].vk;
    loaded = ngl_texture_loaded(texture);
    if (!loaded || vk_texture_count == MAX_VK_TEXTURES)
        return 0;
    vk_textures[vk_texture_count].texture = texture;
    vk_textures[vk_texture_count].vk = loaded->level_count > 1
        ? ngl_vk_texture_levels((const void *const *)loaded->levels, loaded->level_count, loaded->width, loaded->height)
        : ngl_vk_texture(loaded->pixels, loaded->width, loaded->height);
    return vk_textures[vk_texture_count++].vk;
}

/* A cached section's base texture as ngl_vk's number, looked up again while it has none: a material's texture can
 * load after its mesh is first drawn. */
static void section_texture(uint32_t slot)
{
    uint32_t base = ngl_mesh_section_texture(sections[slot].section), lightmap = 0;

    if (!sections[slot].texture) {
        sections[slot].texture = base ? vk_texture_for(base) : 0;
        textured_sections += sections[slot].texture != 0;
    }
    sections[slot].shading = ngl_mesh_section_shading(sections[slot].section, &lightmap);
    sections[slot].alpha = ngl_mesh_section_alpha(sections[slot].section);
    sections[slot].lightmap = lightmap ? vk_texture_for(lightmap) : 0;
    sections[slot].wants_lightmap = lightmap && !sections[slot].lightmap;
    if (sections[slot].shading == NGL_SHADE_LIGHTMAP && !sections[slot].lightmap)
        sections[slot].shading = NGL_SHADE_LIGHT;
    {
        uint32_t second, third;
        static int shown;

        if (ngl_mesh_section_blend(sections[slot].section, &second, &third)) {
            sections[slot].blend_texture = second ? vk_texture_for(second) : 0;
            sections[slot].blend_light = third ? vk_texture_for(third) : 0;
            sections[slot].wants_lightmap |= (second && !sections[slot].blend_texture) ||
                                             (third && !sections[slot].blend_light);
            if (shown < 6) {
                shown++;
                fprintf(stderr, "[present] two-texture section 0x%08X: base %u, second 0x%08X (%u), third 0x%08X "
                        "(%u)\n", sections[slot].section, sections[slot].texture, second, sections[slot].blend_texture,
                        third, sections[slot].blend_light);
            }
        }
    }
}

/* The cache slot of a section's ngl_vk mesh, made the first time it is drawn (with the same vertex data); -1 if it
 * can't be drawn. */
static int vk_mesh_for(uint32_t section)
{
    uint32_t data = ngl_mesh_section_data(section), slot = (section >> 4) * 2654435761u & (SECTION_SLOTS - 1);
    struct ngl_section_triangles t;

    while (sections[slot].section && (sections[slot].section != section || sections[slot].data != data))
        slot = (slot + 1) & (SECTION_SLOTS - 1);
    if (sections[slot].section) {
        if (sections[slot].vk && (!sections[slot].texture || sections[slot].wants_lightmap))
            section_texture(slot);  /* its textures may have loaded since */
        return sections[slot].vk ? (int)slot : -1;
    }
    if (section_count >= SECTION_SLOTS * 3 / 4)
        return -1;                  /* full: draw no new sections */
    section_count++;
    sections[slot].section = section;
    sections[slot].data = data;
    sections[slot].result = ngl_mesh_section_triangles(section, &t);
    section_results[sections[slot].result]++;
    if (sections[slot].result == NGL_SECTION_OK) {
        sections[slot].vk = ngl_vk_mesh(t.corners, t.corner_count, t.indices, t.index_count);
        section_texture(slot);
        if (!sections[slot].texture)
            ngl_mesh_report_untextured(section);
        ngl_mesh_free_triangles(&t);
    }
    return sections[slot].vk ? (int)slot : -1;
}

/* The frame's effect quads (ngl_quad_batch.c) after its meshes, from draw n: each draw's quads as this frame's
 * corners, drawn with one mesh's triangles (made the first time: 0 1 2 and 2 1 3 a quad) as far as it has quads;
 * unlit (the texture times the corners' colors, as the effects' quads are), blended as its material says, in the
 * order they came, in its scene's view. A draw whose texture has no pixels (yet) is left out. Returns the draws' count
 * after them. */
static void note_effect_quads(const struct ngl_quad_batch *b, const float *corners, unsigned corner_count)
{
    const float *m = b->view_projection;    /* Direct3D's row vectors: clip = (x, y, z, 1) x m */
    unsigned g = 0;

    while (g < effect_group_count && (effect_groups[g].texture != b->texture || effect_groups[g].blend != b->blend))
        g++;
    if (g == effect_group_count) {
        if (g == EFFECT_GROUPS)
            return;
        memset(&effect_groups[g], 0, sizeof effect_groups[g]);
        effect_groups[g].texture = b->texture;
        effect_groups[g].blend = b->blend;
        effect_group_count++;
    }
    for (unsigned q = 0; q + 4u <= corner_count; q += 4u) {
        float left = 1.0f, right = -1.0f, bottom = 1.0f, top = -1.0f;
        int behind = 0;

        for (unsigned c = q; c < q + 4u; c++) {
            const float *p = corners + (size_t)c * NGL_CORNER_FLOATS;
            float x = p[0] * m[0] + p[1] * m[4] + p[2] * m[8] + m[12];
            float y = p[0] * m[1] + p[1] * m[5] + p[2] * m[9] + m[13];
            float w = p[0] * m[3] + p[1] * m[7] + p[2] * m[11] + m[15];
            uint8_t bytes[4];

            memcpy(bytes, p + 7, sizeof bytes);     /* the D3DCOLOR's bytes: blue, green, red, alpha */
            effect_groups[g].alpha += bytes[3] / 255.0;
            if (w <= 0.01f) {
                behind = 1;
                continue;
            }
            x /= w;
            y /= w;
            left = x < left ? x : left;
            right = x > right ? x : right;
            bottom = y < bottom ? y : bottom;
            top = y > top ? y : top;
        }
        effect_groups[g].quads++;
        if (behind) {
            effect_groups[g].behind_eye++;
            continue;
        }
        left = left < -1.0f ? -1.0f : left;
        right = right > 1.0f ? 1.0f : right;
        bottom = bottom < -1.0f ? -1.0f : bottom;
        top = top > 1.0f ? 1.0f : top;
        if (right > left && top > bottom)
            effect_groups[g].cover += (double)(right - left) * (top - bottom) / 4.0;
    }
}

static void report_effect_groups(unsigned long frame)
{
    for (unsigned g = 0; g < effect_group_count; g++)
        fprintf(stderr, "[fx] frame %lu: texture 0x%08X blend %u: %lu quads covering %.2f screens, mean corner "
                "alpha %.2f, %lu reaching behind the eye\n", frame, effect_groups[g].texture,
                effect_groups[g].blend, effect_groups[g].quads, effect_groups[g].cover,
                effect_groups[g].quads ? effect_groups[g].alpha / (4.0 * effect_groups[g].quads) : 0.0,
                effect_groups[g].behind_eye);
    effect_group_count = 0;
}

static unsigned frame_effects(unsigned n)
{
    static uint32_t quad_mesh;
    static int no_mesh;
    const struct ngl_quad_batch *list;
    unsigned count = ngl_quad_batch_frame(&list);

    if (!count || no_mesh)
        return n;
    if (!quad_mesh) {
        static uint16_t indices[EFFECT_QUADS * 6];
        static const float four[4 * NGL_CORNER_FLOATS];    /* never drawn: the draws bring their own corners */

        for (uint32_t q = 0; q < EFFECT_QUADS; q++) {
            uint16_t *i = indices + q * 6, c = (uint16_t)(q * 4);

            i[0] = c;
            i[1] = (uint16_t)(c + 1);
            i[2] = (uint16_t)(c + 2);
            i[3] = (uint16_t)(c + 2);
            i[4] = (uint16_t)(c + 1);
            i[5] = (uint16_t)(c + 3);
        }
        quad_mesh = ngl_vk_mesh(four, 4, indices, EFFECT_QUADS * 6);
        no_mesh = !quad_mesh;
        if (no_mesh) {
            fprintf(stderr, "[present] no room in ngl_vk for the effects' quads: no particles\n");
            return n;
        }
    }
    for (unsigned i = 0; i < count && n < MAX_MESH_DRAWS; i++) {
        const struct ngl_quad_batch *b = &list[i];
        uint32_t texture = b->texture ? vk_texture_for(b->texture) : 0, corners = 0;
        unsigned corner_count;

        if (!texture) {
            untextured_effect_draws++;
            continue;
        }
        corner_count = ngl_quad_batch_corners(b, effect_corners, EFFECT_QUADS * 4);
        if (describing_effects)
            note_effect_quads(b, effect_corners, corner_count);
        if (corner_count)
            corners = ngl_vk_frame_corners(effect_corners, corner_count);
        if (!corners)
            continue;       /* never with the mesh's own corners: it has four */
        memset(&mesh_draws[n], 0, sizeof mesh_draws[n]);
        mesh_draws[n].mesh = quad_mesh;
        mesh_draws[n].texture = texture;
        /* The corners' colors as they are, and fogged: the effects' pixel shaders (FxOp1d5 to FxOp53, all but
         * FxOp31) end in a final combiner that mixes toward the fog color by the vertex program's fog
         * (0x008BD1C0: oFog from the depth by FxShader_SetFadeConstant's slope and offset, made from the scene's
         * nglSetFog start, end and amounts). The unlit shading isn't fogged (it's the sky's), so these are the
         * corners' colors multiplied once, which is the same color. */
        mesh_draws[n].shading = NGL_VK_SHADE_VERTEX_COLOR;
        mesh_draws[n].light_once = 1;
        mesh_draws[n].alpha = NGL_VK_ALPHA_BLEND;
        mesh_draws[n].corners = corners;
        mesh_draws[n].index_count = corner_count / 4 * 6;
        mesh_draws[n].blend = b->blend;    /* NGL_QUAD_BLEND_* is NGL_VK_BLEND_* */
        for (int k = 0; k < 16; k += 5)
            mesh_draws[n].world[k] = 1.0f;
        memcpy(mesh_draws[n].view_projection, b->view_projection, sizeof mesh_draws[n].view_projection);
        memcpy(mesh_draws[n].fog, b->fog, sizeof mesh_draws[n].fog);
        mesh_draws[n].fog_color = b->fog_color;
        effect_draws++;
        n++;
    }
    return n;
}

/* At a dumped frame: one draw as ngl_vk gets it, with where its mesh's origin lands on the screen (x and y from -1 to
 * 1, z from 0 to 1 is in view) and the light it gets. For a level whose 3D comes out black. */
static void describe_draw(unsigned long frame, const struct ngl_mesh_draw *from, const struct ngl_vk_mesh_draw *d,
                          uint32_t section)
{
    static const char *shades[] = { "light", "lightmap", "vertex color", "unlit" };
    const float *w = from->world, *m = d->view_projection;
    float clip[4];

    for (int c = 0; c < 4; c++)
        clip[c] = w[12] * m[c] + w[13] * m[4 + c] + w[14] * m[8 + c] + m[12 + c];
    fprintf(stderr, "[present] frame %lu scene 0x%08X mesh 0x%08X section 0x%08X: %s, alpha %u, texture %u, lightmap "
            "%u, tint 0x%X; origin %.1f %.1f %.1f at %.2f %.2f depth %.3f (w %.1f); lit %u ambient %.2f %.2f %.2f "
            "light %.2f %.2f %.2f; fog %.0f to %.0f\n", frame, from->scene, from->mesh, section,
            d->shading < 4 ? shades[d->shading] : "?", d->alpha, d->texture, d->lightmap, d->tint, w[12], w[13], w[14],
            clip[3] ? clip[0] / clip[3] : 0.0f, clip[3] ? clip[1] / clip[3] : 0.0f, clip[3] ? clip[2] / clip[3] : 0.0f,
            clip[3], d->lit, d->ambient[0], d->ambient[1], d->ambient[2], d->light_color[0], d->light_color[1],
            d->light_color[2], d->fog[0], d->fog[1]);
}

/* A mesh's sections in the order they are drawn, into order[]: a sky's by layer (ngl_mesh_section_layer; Gela's city
 * silhouettes go over its fires and clouds), in the order they came where layers are equal. Returns 0 (take them as
 * they come) for a mesh of more than MAX_ORDERED sections. */
static int section_order(uint32_t mesh, unsigned count, unsigned *order)
{
    unsigned layer[MAX_ORDERED];

    if (count > MAX_ORDERED)
        return 0;
    for (unsigned s = 0; s < count; s++) {
        uint32_t section = ngl_mesh_section(mesh, s);
        unsigned key = section ? ngl_mesh_section_layer(section) : 0, at = s;

        while (at > 0 && layer[at - 1] > key) {
            layer[at] = layer[at - 1];
            order[at] = order[at - 1];
            at--;
        }
        layer[at] = key;
        order[at] = s;
    }
    return 1;
}

/* This frame's meshes for ngl_vk, a draw for each section it can draw, then the effects'. Returns how many. */
static unsigned frame_meshes(void)
{
    const struct ngl_mesh_draw *list;
    unsigned count = ngl_mesh_frame(&list), n = 0;
    /* The scene with the most meshes is the world. A scene whose meshes come after the world's (the first-person
     * arms and gun: the game clears depth before drawing it) goes in front of the world: ngl_vk has one depth buffer
     * for everything, so its depth is squeezed into the nearest 2% instead. A scene before the world (the sky) goes
     * at the far plane, every pixel at depth 1: the world covers it, the sky's layers cover each other in the order
     * they came (depth test less or equal), and its blended layers, which ngl_vk draws after the world, stay behind
     * the world (run 110 drew Gela's glow layer over the soldiers). */
    uint32_t scenes[8], world_scene = 0;
    unsigned counts[8], first[8], kinds = 0, best = 0, world_first = 0;
    unsigned described[8] = { 0 }, shaded[5] = { 0 }, untextured = 0, unlit_light = 0;
    static int scenes_shown;
    /* Run 133: the world's shadow projector. R_RenderTestShadow (0x00241290, every frame: r_testshadow defaults to 1)
     * draws the soldiers again, from above, into a 512 x 512 texture, and cdWorld_Draw / cdWorldBlend_Draw darken the
     * world where it falls (the projector's matrix from the light set). Its scene is never drawn on screen (until run
     * 132 it was taken for the first-person one). Its meshes mark the same meshes in the world (the same mesh at the
     * same place) as casters of ngl_vk's sun shadow, cast with the projector's matrix; the world's lightmapped and
     * mixed draws take it. */
    uint32_t caster_mesh[MAX_CASTERS];
    float caster_at[MAX_CASTERS][3], light_view_projection[16];
    unsigned casters = 0, cast = 0, shadowed = 0;
    const float *camera = NULL;
    static int shadow_shown;

    for (unsigned i = 0; i < count; i++) {
        unsigned j = 0;

        if (list[i].shadow_scene) {
            if (casters < MAX_CASTERS) {
                caster_mesh[casters] = list[i].mesh;
                memcpy(caster_at[casters++], list[i].world + 12, sizeof caster_at[0]);
            }
            continue;
        }
        while (j < kinds && scenes[j] != list[i].scene)
            j++;
        if (j == kinds) {
            if (kinds == 8)
                continue;
            scenes[kinds] = list[i].scene;
            counts[kinds] = 0;
            first[kinds++] = i;
        }
        if (++counts[j] > best) {
            best = counts[j];
            world_scene = scenes[j];
            world_first = first[j];
        }
    }
    if (kinds > 1 && scenes_shown < 3) {
        scenes_shown++;
        for (unsigned j = 0; j < kinds; j++)
            fprintf(stderr, "[present] scene 0x%08X: %u meshes from #%u%s\n", scenes[j], counts[j], first[j],
                    scenes[j] == world_scene ? " (the world)" : first[j] > world_first ? " (in front)" : " (far)");
    }

    int projected = casters > 0 && ngl_mesh_projector(light_view_projection);

    for (unsigned i = 0; i < count && n < MAX_MESH_DRAWS; i++) {
        unsigned section_total, order[MAX_ORDERED];
        int ordered, caster = 0;

        if (list[i].shadow_scene)
            continue;
        section_total = ngl_mesh_section_count(list[i].mesh);
        ordered = section_order(list[i].mesh, section_total, order);
        if (projected && list[i].scene == world_scene) {
            if (!camera)
                camera = list[i].view_projection;
            for (unsigned c = 0; c < casters && !caster; c++)
                caster = caster_mesh[c] == list[i].mesh && fabsf(caster_at[c][0] - list[i].world[12]) < 0.5f &&
                         fabsf(caster_at[c][1] - list[i].world[13]) < 0.5f &&
                         fabsf(caster_at[c][2] - list[i].world[14]) < 0.5f;
        }

        for (unsigned k = 0; k < section_total && n < MAX_MESH_DRAWS; k++) {
            uint32_t section = ngl_mesh_section(list[i].mesh, ordered ? order[k] : k);
            int slot = section ? vk_mesh_for(section) : -1;
            uint32_t foam = 0, foam_texture = 0;     /* an ocean's foam: its frame corners and texture */

            if (slot < 0)
                continue;
            mesh_draws[n].mesh = sections[slot].vk;
            mesh_draws[n].tint = ngl_mesh_section_material(section) >> 4;
            mesh_draws[n].texture = sections[slot].texture;
            {   /* an animated texture list shows the frame its class's draw picks (ngl_mesh_section_frame) */
                uint32_t list_texture = ngl_mesh_section_texture(section), at;
                unsigned frames = list_texture ? ngl_texture_frame_count(list_texture) : 0;

                if (frames > 1 && ngl_mesh_section_frame(section, &list[i], &at)) {
                    uint32_t frame = ngl_texture_list_frame(list_texture, at % frames);
                    uint32_t vk = frame ? vk_texture_for(frame) : 0;
                    static uint32_t shown[16];
                    static unsigned shown_count;
                    unsigned s = 0;

                    if (vk)
                        mesh_draws[n].texture = vk;
                    while (s < shown_count && shown[s] != (list_texture ^ (at % frames)))
                        s++;
                    if (s == shown_count && shown_count < 16) {
                        shown[shown_count++] = list_texture ^ (at % frames);
                        fprintf(stderr, "[present] animated texture 0x%08X (%u frames): frame %u (of %u) is 0x%08X "
                                "(%u)\n", list_texture, frames, at % frames, at, frame, vk);
                    }
                }
            }
            mesh_draws[n].lightmap = sections[slot].lightmap;
            mesh_draws[n].blend_texture = sections[slot].blend_texture;
            mesh_draws[n].blend_light = sections[slot].blend_light;
            /* Big Red One multiplies the lightmap and the vertex colors once: cdWorld's pixel shader (0x008A0280) is
             * t0 x t1, and the prelit classes' vertex programs pass the color straight to oD0 (cdWorldVertexLit
             * 0x0089F770, cdSimplePrelit 0x0089C850, cdPrelit 0x0089BCD0) for pixel shaders doing t0 x v0
             * (tools/bro_decomp/nv2a_vsh.py and nv2a_psh.py --show) */
            mesh_draws[n].light_once = 1;
            mesh_draws[n].index_count = 0;         /* all of it (an effect draw may have had this slot) */
            mesh_draws[n].blend_by_alpha = 0;
            mesh_draws[n].blend = NGL_VK_BLEND_ALPHA;
            mesh_draws[n].shading = (uint32_t)sections[slot].shading;      /* NGL_SHADE_* is NGL_VK_SHADE_* */
            mesh_draws[n].alpha = (uint32_t)sections[slot].alpha;          /* NGL_ALPHA_* is NGL_VK_ALPHA_* */
            mesh_draws[n].corners = 0;
            if (ngl_mesh_section_bone_count(section)) {
                uint32_t corners = ngl_mesh_skinned_corners(&list[i], section, skin_corners, 65536);

                mesh_draws[n].corners = corners ? ngl_vk_frame_corners(skin_corners, corners) : 0;
                if (mesh_draws[n].corners)
                    skinned_draws++;
                else
                    unskinned_draws++;
            } else {
                uint32_t foam_source = ngl_mesh_ocean_foam_texture(section);
                int ocean;
                uint32_t corners;

                foam_texture = foam_source ? vk_texture_for(foam_source) : 0;
                ocean = ngl_mesh_ocean_corners(section, list[i].world, skin_corners, foam_texture ? foam_corners : NULL,
                                               65536);
                corners = ocean >= 0 ? (uint32_t)ocean
                                     : ngl_mesh_drifted_corners(section, skin_corners, 65536);   /* a sky's */
                mesh_draws[n].corners = corners ? ngl_vk_frame_corners(skin_corners, corners) : 0;
                if (ocean >= 0 && !mesh_draws[n].corners)
                    continue;       /* the ocean without its waves would be drawn at the node's own height */
                mesh_draws[n].blend_by_alpha = ocean >= 0;     /* its second layer by its own alpha too */
                if (ocean > 0 && foam_texture)
                    foam = ngl_vk_frame_corners(foam_corners, corners);
            }
            memcpy(mesh_draws[n].world, list[i].world, sizeof mesh_draws[n].world);
            memcpy(mesh_draws[n].view_projection, list[i].view_projection, sizeof mesh_draws[n].view_projection);
            if (list[i].scene != world_scene) {
                unsigned j = 0;

                while (j < kinds && scenes[j] != list[i].scene)
                    j++;
                if (j < kinds && first[j] > world_first)
                    for (int r = 0; r < 4; r++)
                        mesh_draws[n].view_projection[4 * r + 2] *= 0.02f;
                else if (j < kinds)
                    for (int r = 0; r < 4; r++)     /* z = w */
                        mesh_draws[n].view_projection[4 * r + 2] = mesh_draws[n].view_projection[4 * r + 3];
            }
            memcpy(mesh_draws[n].fog, list[i].fog, sizeof mesh_draws[n].fog);
            mesh_draws[n].fog_color = list[i].fog_color;
            mesh_draws[n].lit = list[i].lit;
            memcpy(mesh_draws[n].ambient, list[i].ambient, sizeof mesh_draws[n].ambient);
            memcpy(mesh_draws[n].light_direction, list[i].light_direction, sizeof mesh_draws[n].light_direction);
            memcpy(mesh_draws[n].light_color, list[i].light_color, sizeof mesh_draws[n].light_color);
            mesh_draws[n].extra_light_count = list[i].extra_light_count;     /* run 130: four lights */
            memcpy(mesh_draws[n].extra_light_direction, list[i].extra_light_direction,
                   sizeof mesh_draws[n].extra_light_direction);
            memcpy(mesh_draws[n].extra_light_color, list[i].extra_light_color, sizeof mesh_draws[n].extra_light_color);
            /* run 133: a caster's opaque sections cast; the world's lightmapped and mixed sections take the shadow */
            mesh_draws[n].shadow_caster = caster && mesh_draws[n].alpha != NGL_VK_ALPHA_BLEND;
            mesh_draws[n].sun_shadowed = projected && list[i].scene == world_scene &&
                                         (mesh_draws[n].shading == NGL_SHADE_LIGHTMAP || mesh_draws[n].blend_texture);
            cast += mesh_draws[n].shadow_caster;
            shadowed += mesh_draws[n].sun_shadowed;
            if (describing_effects) {
                unsigned j = 0;

                while (j < kinds && scenes[j] != list[i].scene)
                    j++;
                shaded[mesh_draws[n].shading < 4 ? mesh_draws[n].shading : 4]++;
                untextured += !mesh_draws[n].texture;
                unlit_light += mesh_draws[n].shading == NGL_SHADE_LIGHT && !list[i].ambient[0] && !list[i].ambient[1] &&
                               !list[i].ambient[2] && !list[i].light_color[0] && !list[i].light_color[1] &&
                               !list[i].light_color[2];
                if (j < kinds && described[j] < 3) {
                    described[j]++;
                    describe_draw(described_frame, &list[i], &mesh_draws[n], section);
                }
            }
            n++;
            /* cdOcean's foam, added over its water (see ngl_mesh_ocean_corners): its texture times its alpha times the
             * corners' (the crests' share), times the water's map over the sea (blend_light, kept), faded by the fog
             * toward black as an added color must be */
            if (foam && n < MAX_MESH_DRAWS) {
                mesh_draws[n] = mesh_draws[n - 1];
                mesh_draws[n].texture = foam_texture;
                mesh_draws[n].lightmap = mesh_draws[n].blend_texture = 0;
                mesh_draws[n].blend_by_alpha = 0;
                mesh_draws[n].shading = NGL_VK_SHADE_VERTEX_COLOR;
                mesh_draws[n].light_once = 1;
                mesh_draws[n].alpha = NGL_VK_ALPHA_BLEND;
                mesh_draws[n].blend = NGL_VK_BLEND_ADD;
                mesh_draws[n].corners = foam;
                mesh_draws[n].fog_color &= 0xFF000000u;
                mesh_draws[n].shadow_caster = mesh_draws[n].sun_shadowed = 0;
                n++;
            }
        }
    }
    if (describing_effects)
        fprintf(stderr, "[present] frame %lu: %u draws: %u by light (%u of them with no light at all), %u by lightmap, "
                "%u by vertex color, %u unlit, %u other; %u without a texture\n", described_frame, n, shaded[0],
                unlit_light, shaded[1], shaded[2], shaded[3], shaded[4], untextured);
    ngl_vk_set_sun_shadow(projected && cast && camera ? light_view_projection : NULL, camera);
    if (projected && shadow_shown < 6) {
        shadow_shown++;
        fprintf(stderr, "[present] the shadow projector: %u meshes in its scene, %u sections cast, %u take it; its "
                "matrix x %g %g %g %g, y %g %g %g %g, z %g %g %g %g\n", casters, cast, shadowed,
                light_view_projection[0], light_view_projection[4], light_view_projection[8],
                light_view_projection[12], light_view_projection[1], light_view_projection[5],
                light_view_projection[9], light_view_projection[13], light_view_projection[2],
                light_view_projection[6], light_view_projection[10], light_view_projection[14]);
    }
    n = frame_effects(n);
    ngl_vk_set_meshes(mesh_draws, n);
    return n;
}

/* Two triangles for the corners in NGL's order: 0 top left, 1 top right, 2 bottom left, 3 bottom right. */
static unsigned add_quad(unsigned at, const float x[4], const float y[4], const float u[4], const float v[4],
                         const uint32_t color[4], float textured, uint32_t texture)
{
    static const int corners[6] = { 0, 1, 2, 2, 1, 3 };
    int i;

    for (i = 0; i < 6; i++) {
        struct ngl_vk_vertex *out = &vertices[at + i];
        int c = corners[i];

        out->x = x[c];
        out->y = y[c];
        out->u = u[c];
        out->v = v[c];
        out->color = color[c];
        out->textured = textured;
        out->texture = texture;
    }
    return at + 6;
}

/* This frame's vertices: the locked picture over the whole screen (when there is one: picture_texture not 0), then
 * each quad. */
static unsigned frame_vertices(uint32_t picture_texture, int placeholders, unsigned *drawn, unsigned *placeheld)
{
    static const float screen_x[4] = { 0.0f, SCREEN_WIDTH, 0.0f, SCREEN_WIDTH };
    static const float screen_y[4] = { 0.0f, 0.0f, SCREEN_HEIGHT, SCREEN_HEIGHT };
    static const float corner_u[4] = { 0.0f, 1.0f, 0.0f, 1.0f };
    static const float corner_v[4] = { 0.0f, 0.0f, 1.0f, 1.0f };
    static const uint32_t opaque[4] = { 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu };
    const struct ngl_quad *quads;
    unsigned count = ngl_quad_frame(&quads), at, i;

    *drawn = *placeheld = 0;
    at = picture_texture ? add_quad(0, screen_x, screen_y, corner_u, corner_v, opaque, 1.0f, 0) : 0;
    for (i = 0; i < count && at + 6 <= MAX_VERTICES; i++) {
        const struct ngl_quad *q = &quads[i];

        uint32_t vk_texture = q->texture && q->texture != picture_texture ? vk_texture_for(q->texture) : 0;
        /* a picture RE_RegisterShader named that no texture has (yet): the original drew ngl's white texture */
        uint32_t texture = vk_texture || !ngl_texture_frame_count(q->texture) ? q->texture : 0;

        if (q->texture && q->texture == picture_texture) {
            at = add_quad(at, q->x, q->y, q->u, q->v, q->color, 1.0f, 0);
            (*drawn)++;
        } else if (vk_texture) {
            at = add_quad(at, q->x, q->y, q->u, q->v, q->color, 2.0f, vk_texture);
            (*drawn)++;
        } else if (placeholders || !texture) {
            uint32_t faint[4];
            int c;

            for (c = 0; c < 4; c++) {
                uint32_t alpha = q->color[c] >> 24;

                faint[c] = (q->color[c] & 0x00FFFFFFu) |
                           ((texture ? alpha >> PLACEHOLDER_ALPHA_SHIFT : alpha) << 24);
            }
            at = add_quad(at, q->x, q->y, q->u, q->v, faint, 0.0f, 0);
            if (texture)
                (*placeheld)++;
            else
                (*drawn)++;
        }
    }
    return at;
}

/* How much of a loaded texture a quad covers isn't transparent: the alpha at the middle of its texture coordinates,
 * and how many of the texels inside them have alpha above 0. */
static void texture_under_quad(const struct ngl_loaded_texture *t, const struct ngl_quad *q, int *middle_alpha,
                               unsigned long *covered, unsigned long *visible)
{
    float u0 = q->u[0] < q->u[3] ? q->u[0] : q->u[3], u1 = q->u[0] < q->u[3] ? q->u[3] : q->u[0];
    float v0 = q->v[0] < q->v[3] ? q->v[0] : q->v[3], v1 = q->v[0] < q->v[3] ? q->v[3] : q->v[0];
    long left = (long)(u0 * (float)t->width), right = (long)(u1 * (float)t->width);
    long top = (long)(v0 * (float)t->height), bottom = (long)(v1 * (float)t->height);
    long x, y, mx, my;

    left = left < 0 ? 0 : left > (long)t->width - 1 ? (long)t->width - 1 : left;
    right = right < left ? left : right > (long)t->width - 1 ? (long)t->width - 1 : right;
    top = top < 0 ? 0 : top > (long)t->height - 1 ? (long)t->height - 1 : top;
    bottom = bottom < top ? top : bottom > (long)t->height - 1 ? (long)t->height - 1 : bottom;
    *covered = *visible = 0;
    for (y = top; y <= bottom; y++)
        for (x = left; x <= right; x++) {
            (*covered)++;
            *visible += t->pixels[((size_t)y * t->width + (size_t)x) * 4 + 3] != 0;
        }
    mx = (left + right) / 2;
    my = (top + bottom) / 2;
    *middle_alpha = t->pixels[((size_t)my * t->width + (size_t)mx) * 4 + 3];
}

/* At the dump frame: each texture's first few quads as they go to Vulkan, with what the texture holds under them. */
static void describe_quads(unsigned long frame, uint32_t picture_texture)
{
    struct { uint32_t texture; unsigned described; } seen[TEXTURES_DESCRIBED];      /* this frame's */
    unsigned seen_count = 0;
    const struct ngl_quad *quads;
    unsigned count = ngl_quad_frame(&quads), i, j;

    for (i = 0; i < count; i++) {
        const struct ngl_quad *q = &quads[i];
        const struct ngl_loaded_texture *t = q->texture ? ngl_texture_loaded(q->texture) : NULL;
        unsigned long covered = 0, visible = 0;
        int middle_alpha = -1;

        for (j = 0; j < seen_count && seen[j].texture != q->texture; j++)
            ;
        if (j == seen_count) {
            if (seen_count == TEXTURES_DESCRIBED)
                continue;
            seen[seen_count].texture = q->texture;
            seen[seen_count++].described = 0;
        }
        if (seen[j].described++ >= (q->texture ? QUADS_A_TEXTURE_DESCRIBED : UNTEXTURED_QUADS_DESCRIBED))
            continue;
        if (t && t->pixels && t->width && t->height)
            texture_under_quad(t, q, &middle_alpha, &covered, &visible);
        fprintf(stderr, "[ngl] Vulkan: frame %lu quad %u (%.1f,%.1f)-(%.1f,%.1f) uv (%.3f,%.3f)-(%.3f,%.3f) color %08X "
                "z %.3f blend %u texture %08X%s vk %u %ux%u: middle alpha %d, %lu of %lu texels not transparent\n",
                frame, i, q->x[0], q->y[0], q->x[3], q->y[3], q->u[0], q->v[0], q->u[3], q->v[3], q->color[0], q->z,
                q->blend, q->texture,
                q->texture && q->texture == picture_texture ? " (the picture)" : "",
                q->texture && q->texture != picture_texture ? vk_texture_for(q->texture) : 0, t ? t->width : 0,
                t ? t->height : 0, middle_alpha, visible, covered);
    }
}

/* The game's gamma ramp (owner 2026-11-05 22:10: "consider the colorspaces"). Direct3D's SetGammaRamp (0x003E5770)
 * keeps two ramps in its device (the pointer at 0x003F8318), each BYTE red[256], green[256], blue[256], at +0x1E04 and
 * +0x2104, and writes the index of the newest at +0x240C; the Xbox put it on the screen at the next vertical blank (at
 * once with D3DSGR_IMMEDIATE). It is the game's brightness. There is no window yet, so for now it goes into the saved
 * pictures (the drawn colors are what the difference to the game's picture is measured on). A device without a ramp yet
 * (all zero) leaves the colors as drawn. When the window comes, the same table goes before the output color space. */
#define D3D_DEVICE_POINTER 0x003F8318u
#define D3D_DEVICE_GAMMA_RAMPS 0x1E04u
#define D3D_DEVICE_GAMMA_RAMP_NEWEST 0x240Cu
#define D3D_GAMMA_RAMP_BYTES 0x300u

/* The ramp the game set last (red, green, blue: 256 each); 0 when there is none or it changes nothing. */
static int game_gamma_ramp(uint8_t ramp[3][256])
{
    static uint8_t logged[3][256];
    static int logged_once;
    uint32_t device = *(const uint32_t *)XBOX_PTR(D3D_DEVICE_POINTER);
    uint32_t newest, c, i;
    int identity = 1, empty = 1;

    if (!device)
        return 0;
    newest = *(const uint32_t *)XBOX_PTR(device + D3D_DEVICE_GAMMA_RAMP_NEWEST) & 1;
    memcpy(ramp, (const void *)XBOX_PTR(device + D3D_DEVICE_GAMMA_RAMPS + newest * D3D_GAMMA_RAMP_BYTES),
           D3D_GAMMA_RAMP_BYTES);
    for (c = 0; c < 3; c++) {
        for (i = 0; i < 256; i++) {
            identity &= ramp[c][i] == i;
            empty &= ramp[c][i] == 0;
        }
    }
    if (!logged_once || memcmp(logged, ramp, sizeof logged)) {
        logged_once = 1;
        memcpy(logged, ramp, sizeof logged);
        fprintf(stderr, "[ngl] the game's gamma ramp (slot %u): red 0/32/64/128/192/255 -> %u/%u/%u/%u/%u/%u, "
                "green 128 -> %u, blue 128 -> %u%s\n", newest, ramp[0][0], ramp[0][32], ramp[0][64], ramp[0][128],
                ramp[0][192], ramp[0][255], ramp[1][128], ramp[2][128],
                empty ? " (none set yet: colors as drawn)" : identity ? " (changes nothing)" : "");
    }
    return !empty && !identity;
}

/* Save the target as a picture (RGBA in, RGB out), and say how far it is from the game's picture it was drawn from
 * when the two are the same size (then each target pixel is one picture pixel). */
static void save_target(unsigned long frame, const char *path, const uint8_t *picture, uint32_t picture_width,
                        uint32_t picture_height, uint32_t pitch)
{
    uint32_t width, height, i;
    unsigned long difference = 0, largest = 0;
    uint32_t left = UINT32_MAX, top = UINT32_MAX, right = 0, bottom = 0;   /* where they differ */
    const uint8_t *pixels = ngl_vk_target_pixels(&width, &height);
    unsigned long lit = 0;
    FILE *out;
    uint8_t ramp[3][256];
    const int ramped = game_gamma_ramp(ramp);

    if (!pixels)
        return;
    out = fopen(path, "wb");
    if (!out) {
        fprintf(stderr, "[ngl] Vulkan: can't write %s\n", path);
        return;
    }
    fprintf(out, "P6\n%u %u\n255\n", width, height);
    for (i = 0; i < width * height; i++) {
        lit += (pixels[i * 4] | pixels[i * 4 + 1] | pixels[i * 4 + 2]) != 0;
        if (picture_width == width && picture_height == height) {
            const uint8_t *source = picture + (size_t)(i / width) * pitch + (size_t)(i % width) * 4;
            int c;

            for (c = 0; c < 3; c++) {      /* target red, green, blue; picture blue, green, red */
                int d = (int)pixels[i * 4 + c] - (int)source[2 - c];
                unsigned long size = (unsigned long)(d < 0 ? -d : d);

                difference += size;
                if (size > largest)
                    largest = size;
                if (size) {
                    uint32_t x = i % width, y = i / width;

                    left = x < left ? x : left;
                    right = x > right ? x : right;
                    top = y < top ? y : top;
                    bottom = y > bottom ? y : bottom;
                }
            }
        }
        if (ramped) {               /* as the game's ramp shows it */
            const uint8_t shown[3] = {ramp[0][pixels[i * 4]], ramp[1][pixels[i * 4 + 1]], ramp[2][pixels[i * 4 + 2]]};

            fwrite(shown, 1, 3, out);
        } else {
            fwrite(pixels + (size_t)i * 4, 1, 3, out);
        }
    }
    fclose(out);
    fprintf(stderr, "[ngl] Vulkan: frame %lu saved to %s, %ux%u (%lu pixels not black)\n", frame, path, width, height,
            lit);
    if (picture_width == width && picture_height == height)
        fprintf(stderr, "[ngl] Vulkan: frame %lu differs from the game's picture by %.3f on average, %lu at most "
                "(of 255)\n", frame, (double)difference / ((double)width * height * 3), largest);
    if (largest)
        fprintf(stderr, "[ngl] Vulkan: frame %lu differs within (%u,%u)-(%u,%u)\n", frame, left, top, right, bottom);
}

/* Whether frame is one of the comma-separated numbers in list. */
static int in_list(const char *list, unsigned long frame)
{
    char *end;

    while (*list) {
        unsigned long n = strtoul(list, &end, 0);

        if (end == list)
            return 0;
        if (n == frame)
            return 1;
        if (*end != ',')
            return 0;
        list = end + 1;
    }
    return 0;
}

#if defined(BRO_HAVE_SDL3)
static SDL_Window *window;

#if defined(_WIN32)
/* The SDK builds SDL3 for Windows without its Vulkan part (rexglue-sdk/thirdparty/CMakeLists.txt turns SDL_VULKAN off
 * for MinGW), so on Windows the window is a plain one: its surface is made here from the window's handle, with the two
 * instance extensions a Windows surface needs (the Vulkan header is included above, for Windows). */
static const char *const window_extensions[] = {VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_WIN32_SURFACE_EXTENSION_NAME};
#define WINDOW_FLAGS (SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY)

static int make_window_surface(void *instance, void *surface, void *user)
{
    VkWin32SurfaceCreateInfoKHR info = {VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR};

    info.hinstance = GetModuleHandleW(NULL);
    info.hwnd = (HWND)SDL_GetPointerProperty(SDL_GetWindowProperties((SDL_Window *)user),
                                             SDL_PROP_WINDOW_WIN32_HWND_POINTER, NULL);
    if (!info.hwnd)
        return -1;
    return vkCreateWin32SurfaceKHR((VkInstance)instance, &info, NULL, (VkSurfaceKHR *)surface) == VK_SUCCESS ? 0 : -1;
}

static const char *const *window_instance_extensions(Uint32 *count)
{
    *count = (Uint32)(sizeof window_extensions / sizeof window_extensions[0]);
    return window_extensions;
}
#else
#define WINDOW_FLAGS (SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY)

static int make_window_surface(void *instance, void *surface, void *user)
{
    return SDL_Vulkan_CreateSurface((SDL_Window *)user, (VkInstance)instance, NULL, (VkSurfaceKHR *)surface) ? 0 : -1;
}

static const char *const *window_instance_extensions(Uint32 *count)
{
    return SDL_Vulkan_GetInstanceExtensions(count);
}
#endif

#if defined(_WIN32)
/* display.start_in_background on Windows: the window, still hidden, goes to the bottom of the stacking order, so SDL
 * shows it there without activating it. */
static void send_behind_other_windows(SDL_Window *w)
{
    HWND hwnd = (HWND)SDL_GetPointerProperty(SDL_GetWindowProperties(w), SDL_PROP_WINDOW_WIN32_HWND_POINTER, NULL);

    if (hwnd)
        SetWindowPos(hwnd, HWND_BOTTOM, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
}
#else
/* display.start_in_background on X11 uses EWMH hints SDL has no call for, through Xlib loaded at run time (BRO
 * doesn't link it). Each does nothing when the window isn't an X11 one:
 * - a _NET_WM_USER_TIME of 0 on the window before it's mapped asks the window manager not to focus it when it's
 *   mapped; the window managers that honor it (Muffin/Mutter, KWin, Xfwm, Openbox, as the SDK's patch 0063 notes)
 *   also stack it below the focused window (ask_x11_not_to_focus_on_map);
 * - _NET_WM_STATE_BELOW keeps it under the normal windows whatever opens or closes (keep_x11_window_below);
 * - the root's _NET_ACTIVE_WINDOW is read for the window event log (x11_active_window). */
typedef unsigned long x11_atom;
typedef unsigned long (*x11_intern_atom_fn)(void *display, const char *name, int only_if_exists);
typedef int (*x11_change_property_fn)(void *display, unsigned long window, x11_atom property, x11_atom type,
                                      int format, int mode, const unsigned char *data, int count);
typedef int (*x11_flush_fn)(void *display);
typedef int (*x11_send_event_fn)(void *display, unsigned long window, int propagate, long mask, void *event);
typedef unsigned long (*x11_default_root_window_fn)(void *display);
typedef int (*x11_get_window_property_fn)(void *display, unsigned long window, x11_atom property, long offset,
                                          long length, int remove, x11_atom type, x11_atom *actual_type,
                                          int *actual_format, unsigned long *count, unsigned long *bytes_after,
                                          unsigned char **data);
typedef int (*x11_free_fn)(void *data);
#define X11_XA_CARDINAL         6ul                         /* Xatom.h */
#define X11_ANY_PROPERTY_TYPE   0ul                         /* X.h */
#define X11_PROP_MODE_REPLACE   0                           /* Xlib.h */
#define X11_CLIENT_MESSAGE      33                          /* X.h */
#define X11_SUBSTRUCTURE_MASKS  ((1l << 19) | (1l << 20))   /* SubstructureNotifyMask | SubstructureRedirectMask */

static struct {
    SDL_SharedObject *library;
    void *display;
    unsigned long window, root;
    x11_intern_atom_fn intern_atom;
    x11_change_property_fn change_property;
    x11_flush_fn flush;
    x11_send_event_fn send_event;
    x11_get_window_property_fn get_window_property;
    x11_free_fn free;
} xlib;

/* Loads Xlib and finds the window's X11 display, window and root, once. Returns 1 when all of that worked. */
static int load_xlib(SDL_Window *w)
{
    SDL_PropertiesID props = SDL_GetWindowProperties(w);
    x11_default_root_window_fn default_root_window;

    if (xlib.library)
        return 1;
    xlib.display = SDL_GetPointerProperty(props, SDL_PROP_WINDOW_X11_DISPLAY_POINTER, NULL);
    xlib.window = (unsigned long)SDL_GetNumberProperty(props, SDL_PROP_WINDOW_X11_WINDOW_NUMBER, 0);
    if (!xlib.display || !xlib.window || !(xlib.library = SDL_LoadObject("libX11.so.6")))
        return 0;
    xlib.intern_atom = (x11_intern_atom_fn)SDL_LoadFunction(xlib.library, "XInternAtom");
    xlib.change_property = (x11_change_property_fn)SDL_LoadFunction(xlib.library, "XChangeProperty");
    xlib.flush = (x11_flush_fn)SDL_LoadFunction(xlib.library, "XFlush");
    xlib.send_event = (x11_send_event_fn)SDL_LoadFunction(xlib.library, "XSendEvent");
    xlib.get_window_property = (x11_get_window_property_fn)SDL_LoadFunction(xlib.library, "XGetWindowProperty");
    xlib.free = (x11_free_fn)SDL_LoadFunction(xlib.library, "XFree");
    default_root_window = (x11_default_root_window_fn)SDL_LoadFunction(xlib.library, "XDefaultRootWindow");
    if (!xlib.intern_atom || !xlib.change_property || !xlib.flush || !xlib.send_event || !xlib.get_window_property ||
        !xlib.free || !default_root_window) {
        SDL_UnloadObject(xlib.library);
        xlib.library = NULL;
        return 0;
    }
    xlib.root = default_root_window(xlib.display);
    return 1;
}

static int ask_x11_not_to_focus_on_map(SDL_Window *w)
{
    x11_atom user_time;
    long zero = 0;

    if (!load_xlib(w) || !(user_time = xlib.intern_atom(xlib.display, "_NET_WM_USER_TIME", 0)))
        return 0;
    xlib.change_property(xlib.display, xlib.window, user_time, X11_XA_CARDINAL, 32, X11_PROP_MODE_REPLACE,
                         (const unsigned char *)&zero, 1);
    xlib.flush(xlib.display);
    return 1;
}

/* Adds (below = 1) or takes off (0) _NET_WM_STATE_BELOW on the mapped window. A client asks for it with a message to
 * the root window (EWMH), laid out as Xlib's XClientMessageEvent inside an XEvent's 24 longs. */
static void keep_x11_window_below(int below)
{
    union {
        struct {
            int type;
            unsigned long serial;
            int send_event;
            void *display;
            unsigned long window;
            x11_atom message_type;
            int format;
            long data[5];
        } message;
        long size[24];
    } event;
    x11_atom state, state_below;

    if (!xlib.library || !(state = xlib.intern_atom(xlib.display, "_NET_WM_STATE", 0)) ||
        !(state_below = xlib.intern_atom(xlib.display, "_NET_WM_STATE_BELOW", 0)))
        return;
    memset(&event, 0, sizeof event);
    event.message.type = X11_CLIENT_MESSAGE;
    event.message.window = xlib.window;
    event.message.message_type = state;
    event.message.format = 32;
    event.message.data[0] = below ? 1 : 0;      /* _NET_WM_STATE_ADD or _NET_WM_STATE_REMOVE */
    event.message.data[1] = (long)state_below;
    event.message.data[3] = 1;                  /* asked by an application */
    xlib.send_event(xlib.display, xlib.root, 0, X11_SUBSTRUCTURE_MASKS, &event);
    xlib.flush(xlib.display);
}

/* The root's _NET_ACTIVE_WINDOW, the window the window manager calls active (0 when it can't be read). */
static unsigned long x11_active_window(void)
{
    x11_atom active, type = 0;
    int format = 0;
    unsigned long count = 0, bytes_after = 0, id = 0;
    unsigned char *data = NULL;

    if (!xlib.library || !(active = xlib.intern_atom(xlib.display, "_NET_ACTIVE_WINDOW", 0)))
        return 0;
    if (xlib.get_window_property(xlib.display, xlib.root, active, 0, 1, 0, X11_ANY_PROPERTY_TYPE, &type, &format,
                                 &count, &bytes_after, &data) == 0 && data && format == 32 && count == 1)
        id = *(unsigned long *)data;            /* Xlib hands 32-bit items over as longs */
    if (data)
        xlib.free(data);
    return id;
}
#endif

static int fullscreen_waits_for_focus;  /* display.start_in_background with display.fullscreen: until the first focus */
static int background_window;           /* display.start_in_background: the window event log is on */
/* display.start_in_background on X11, from the window's first SHOWN to its first click: it can't take the focus
 * (WM_HINTS input False, the ICCCM "no input" model: SDL lists no WM_TAKE_FOCUS, so the window manager never hands
 * it the focus) and stays under the other windows (_NET_WM_STATE_BELOW). Not from its creation: a window made
 * unfocusable (SDL_WINDOW_NOT_FOCUSABLE) stayed on top of everything (build g4), likely because one that can't take
 * the focus is never denied it and so isn't stacked under the focused window. */
static int kept_in_background;
static int background_done;             /* the first click has brought it forward: not again */

/* A window for ngl_vk to show the frames in, when SDL3 has a display (display.window off, BRO_WINDOW=0: none).
 * display.start_in_background (BRO_START_IN_BACKGROUND: test runs on a PC someone is using) opens it the way CoD3's
 * start_in_background does (the project's SDK patch 0063): created hidden, shown without activating it
 * (SDL_HINT_WINDOW_ACTIVATE_WHEN_SHOWN "0", no XDG_ACTIVATION_TOKEN) and below the window in use (X11:
 * _NET_WM_USER_TIME 0 before it's mapped; Windows: the bottom of the stacking order). On X11, once it's shown, it
 * also can't take the focus and stays under the other windows until it's clicked (kept_in_background, pump_window).
 * Clicking it gives it the focus, and only then does it go full screen if display.fullscreen asks for that
 * (pump_window). Without the focus the mouse isn't captured either. Returns 1 when there is one. */
static int open_window(void)
{
    const char *const *extensions;
    int windowed = bro_setting_bool("display.window", "BRO_WINDOW", 1);
    int fullscreen = bro_setting_bool("display.fullscreen", "BRO_FULLSCREEN", 0);
    int vsync = bro_setting_bool("display.vsync", "BRO_VSYNC", 1);
    int background = bro_setting_bool("display.start_in_background", "BRO_START_IN_BACKGROUND", 0);
    Uint32 count = 0;
    int width = 0, height = 0;
    SDL_WindowFlags flags = WINDOW_FLAGS;

    if (!windowed)
        return 0;
    if (!SDL_InitSubSystem(SDL_INIT_VIDEO)) {
        fprintf(stderr, "[ngl] no window (%s): drawing offscreen\n", SDL_GetError());
        return 0;
    }
    if (background) {
        flags |= SDL_WINDOW_HIDDEN;         /* shown below, once it's been sent behind */
        fullscreen_waits_for_focus = fullscreen;
        background_window = 1;
    } else if (fullscreen) {
        flags |= SDL_WINDOW_FULLSCREEN;     /* the desktop's own size, with no mode change */
    }
    window = SDL_CreateWindow("Call of Duty 2: Big Red One", 2 * SCREEN_WIDTH, 2 * SCREEN_HEIGHT, flags);
    if (!window || !(extensions = window_instance_extensions(&count))) {
        fprintf(stderr, "[ngl] no window for Vulkan (%s): drawing offscreen\n", SDL_GetError());
        if (window)
            SDL_DestroyWindow(window);
        window = NULL;
        return 0;
    }
    if (background) {
        SDL_SetHint(SDL_HINT_WINDOW_ACTIVATE_WHEN_SHOWN, "0");
        if (SDL_getenv("XDG_ACTIVATION_TOKEN"))
            SDL_unsetenv_unsafe("XDG_ACTIVATION_TOKEN");    /* Wayland: it would let the compositor focus it */
#if defined(_WIN32)
        send_behind_other_windows(window);
#else
        ask_x11_not_to_focus_on_map(window);
#endif
        SDL_ShowWindow(window);
        SDL_ResetHint(SDL_HINT_WINDOW_ACTIVATE_WHEN_SHOWN);
        fprintf(stderr, "[ngl] window opened in the background (display.start_in_background, X11 window 0x%lx)%s\n",
                (unsigned long)SDL_GetNumberProperty(SDL_GetWindowProperties(window),
                                                     SDL_PROP_WINDOW_X11_WINDOW_NUMBER, 0),
                fullscreen ? "; full screen when it's first clicked" : "");
    }
    SDL_GetWindowSizeInPixels(window, &width, &height);
    ngl_vk_set_window(extensions, count, make_window_surface, window, (uint32_t)width, (uint32_t)height,
                      vsync ? 0 : 1);
    return 1;
}

/* The name of a window event, for the log. */
static const char *window_event_name(Uint32 type)
{
    switch (type) {
    case SDL_EVENT_WINDOW_SHOWN: return "shown";
    case SDL_EVENT_WINDOW_HIDDEN: return "hidden";
    case SDL_EVENT_WINDOW_EXPOSED: return "exposed";
    case SDL_EVENT_WINDOW_MOVED: return "moved";
    case SDL_EVENT_WINDOW_RESIZED: return "resized";
    case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED: return "pixel size changed";
    case SDL_EVENT_WINDOW_MINIMIZED: return "minimized";
    case SDL_EVENT_WINDOW_MAXIMIZED: return "maximized";
    case SDL_EVENT_WINDOW_RESTORED: return "restored";
    case SDL_EVENT_WINDOW_MOUSE_ENTER: return "mouse enter";
    case SDL_EVENT_WINDOW_MOUSE_LEAVE: return "mouse leave";
    case SDL_EVENT_WINDOW_FOCUS_GAINED: return "focus gained";
    case SDL_EVENT_WINDOW_FOCUS_LOST: return "focus lost";
    case SDL_EVENT_WINDOW_CLOSE_REQUESTED: return "close requested";
    case SDL_EVENT_WINDOW_OCCLUDED: return "occluded";
    case SDL_EVENT_WINDOW_ENTER_FULLSCREEN: return "enter full screen";
    case SDL_EVENT_WINDOW_LEAVE_FULLSCREEN: return "leave full screen";
    case SDL_EVENT_WINDOW_DISPLAY_CHANGED: return "display changed";
    case SDL_EVENT_WINDOW_DESTROYED: return "destroyed";
    default: return NULL;
    }
}

/* display.start_in_background: each window event in the log, with SDL's clock, so a run shows how the window got
 * the focus if it ever does (with, on X11, the window the window manager calls active then). At most 200 lines. */
static void log_window_event(Uint32 type)
{
    static unsigned lines;
    const char *name = window_event_name(type);
    double at = (double)SDL_GetTicks() / 1000.0;

    if (!background_window || lines >= 200)
        return;
    if (++lines == 200) {
        fprintf(stderr, "[ngl] window event log: 200 lines, no more\n");
        return;
    }
    if (name)
        fprintf(stderr, "[ngl] window event: %s at %.3f s", name, at);
    else
        fprintf(stderr, "[ngl] window event: 0x%x at %.3f s", (unsigned)type, at);
#if !defined(_WIN32)
    if (type == SDL_EVENT_WINDOW_FOCUS_GAINED)
        fprintf(stderr, " (the active window: 0x%lx; this one: 0x%lx%s)", x11_active_window(), xlib.window,
                kept_in_background ? "; it was kept in the background" : "");
#endif
    fputc('\n', stderr);
}

/* display.start_in_background on X11: once the window is shown (placed under the active window), it can't take the
 * focus and stays under the other windows; its first click undoes both and asks for the focus. */
static void keep_in_background(int keep)
{
#if !defined(_WIN32)
    if (keep == kept_in_background || !xlib.library)
        return;
    SDL_SetWindowFocusable(window, !keep);
    keep_x11_window_below(keep);
    kept_in_background = keep;
    if (keep) {
        fprintf(stderr, "[ngl] the window takes no focus and stays under the others until it's clicked\n");
    } else {
        background_done = 1;
        SDL_RaiseWindow(window);
        fprintf(stderr, "[ngl] the window was clicked: it can take the focus now\n");
    }
#else
    (void)keep;
#endif
}

/* The window's events, which check_quit left in SDL's queue this frame: a new size goes to ngl_vk; the focus turns
 * mouse look on and off; a background window's first SHOWN and first click change how it takes the focus. The rest
 * are dropped. */
static void pump_window(void)
{
    SDL_Event event;

    if (!window)
        return;
    if (window_clicked && kept_in_background)
        keep_in_background(0);
    window_clicked = 0;
    while (SDL_PeepEvents(&event, 1, SDL_GETEVENT, SDL_EVENT_WINDOW_FIRST, SDL_EVENT_WINDOW_LAST) > 0) {
        log_window_event(event.type);
        if (event.type == SDL_EVENT_WINDOW_SHOWN && background_window && !background_done)
            keep_in_background(1);
        if (event.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED || event.type == SDL_EVENT_WINDOW_RESIZED ||
            event.type == SDL_EVENT_WINDOW_MINIMIZED || event.type == SDL_EVENT_WINDOW_RESTORED ||
            event.type == SDL_EVENT_WINDOW_MAXIMIZED) {
            int width = 0, height = 0;

            if (SDL_GetWindowFlags(window) & SDL_WINDOW_MINIMIZED)
                width = height = 0;
            else
                SDL_GetWindowSizeInPixels(window, &width, &height);
            ngl_vk_window_resized((uint32_t)width, (uint32_t)height);
        } else if (event.type == SDL_EVENT_WINDOW_FOCUS_GAINED || event.type == SDL_EVENT_WINDOW_FOCUS_LOST) {
            /* the mouse looks around while the window has the focus (bro_pad.c reads its motion) */
            const char *look = getenv("BRO_MOUSE_LOOK");

            if (event.type == SDL_EVENT_WINDOW_FOCUS_GAINED && fullscreen_waits_for_focus) {
                fullscreen_waits_for_focus = 0;     /* display.start_in_background: full screen once clicked */
                SDL_SetWindowFullscreen(window, true);
            }

            if (!(look && look[0] == '0'))
                SDL_SetWindowRelativeMouseMode(window, event.type == SDL_EVENT_WINDOW_FOCUS_GAINED);
        }
    }
    SDL_FlushEvents(SDL_EVENT_FIRST, SDL_EVENT_LAST);
}
#else
static int open_window(void)
{
    return 0;
}

static void pump_window(void)
{
}
#endif

/* The target's size: display.size = "<w>x<h>" (BRO_VK_SIZE), else 640 x 480 offscreen and twice that with a
 * window. */
static void target_size(int windowed, uint32_t *width, uint32_t *height)
{
    const char *setting = bro_setting_text("display.size", "BRO_VK_SIZE");
    unsigned w = 0, h = 0;

    if (setting && sscanf(setting, "%ux%u", &w, &h) == 2 && w >= 64 && h >= 48 && w <= 7680 && h <= 4320) {
        *width = w;
        *height = h;
    } else {
        *width = windowed ? 2 * SCREEN_WIDTH : SCREEN_WIDTH;
        *height = windowed ? 2 * SCREEN_HEIGHT : SCREEN_HEIGHT;
    }
}

void ngl_present_frame(void)
{
    static int state;               /* 0: not started, 1: drawing, -1: off */
    static unsigned long frames;
    static int placeholders;
    const char *dump_frame, *dump_path;
    uint32_t buffer, width, height, pitch;
    unsigned count, drawn, placeheld, meshes;
    const uint8_t *picture;
    uint32_t picture_texture;

    check_quit();
    if (state == 0) {
        const char *setting = getenv("BRO_VK");

        uint32_t target_width, target_height;

        if (!setting || setting[0] == '0') {
            state = -1;
            return;
        }
        target_size(open_window(), &target_width, &target_height);
        /* BRO_VK_GEOMETRY_ON_GPU=0: the meshes' corners and indices stay in memory the host sees, as before ngl_vk's
         * side220, to compare the two ways (a test switch, not a setting) */
        setting = getenv("BRO_VK_GEOMETRY_ON_GPU");
        if (setting && setting[0] == '0') {
            ngl_vk_set_geometry_on_gpu(0);
            fprintf(stderr, "[ngl] Vulkan: the meshes' corners and indices stay in host memory "
                    "(BRO_VK_GEOMETRY_ON_GPU=0)\n");
        }
        if (ngl_vk_init(target_width, target_height)) {
            fprintf(stderr, "[ngl] Vulkan didn't start; frames are not drawn\n");
            state = -1;
            return;
        }
        fprintf(stderr, "[ngl] Vulkan: drawing on %s, %ux%u, %s\n", ngl_vk_device_name(), target_width, target_height,
                ngl_vk_showing() ? "shown in the window" : "offscreen");
        setting = getenv("BRO_VK_PLACEHOLDERS");
        placeholders = setting && setting[0] == '1';
        state = 1;
    }
    if (state < 0)
        return;
    pump_window();
    ngl_quad_batch_render_effects(ngl_scene_root());   /* the effects' quads, as NGL's scene rendering makes them */

    frames++;
    dump_frame = getenv("BRO_VK_DUMP");
    dump_path = getenv("BRO_VK_DUMP_PATH");
    describing_effects = dump_frame && dump_path && in_list(dump_frame, frames);
    described_frame = frames;
    if (ngl_texture_last_lock(&buffer, &width, &height, &pitch)) {
        picture = (const uint8_t *)XBOX_PTR(buffer);
        picture_texture = ngl_texture_last_locked();
    } else {
        picture = NULL;             /* nothing locked yet: the quads on black */
        picture_texture = 0;
        width = height = pitch = 0;
    }
    /* a frame with 3D meshes is the game's world: the last picture the game wrote (a loading screen) isn't its
     * background any more */
    meshes = frame_meshes();
    count = frame_vertices(meshes ? 0 : picture_texture, placeholders, &drawn, &placeheld);
    count += ngl_fps_counter_vertices(vertices + count, MAX_VERTICES - count);
#if defined(BRO_HAVE_SDL3)
    if (window && ngl_fps_counter_on()) {
        char title[128];

        if (ngl_fps_counter_title(title, sizeof title))
            SDL_SetWindowTitle(window, title);
    }
#endif
    /* [acct2-bro-game] In a frame with 3D meshes no quad samples the picture (frame_vertices was given none), so it
     * isn't copied up to the GPU (ngl_vk then takes one white pixel): 1.2 MB a frame less. And the target is copied
     * back to the host only for a frame saved for checks (BRO_VK_DUMP, save_target): the window gets its picture on
     * the GPU, so the copy back was 4.9 MB a frame (at 1280 x 960) for nothing. */
    ngl_vk_set_readback(describing_effects);
    if (ngl_vk_draw_quads(vertices, count, (float)SCREEN_WIDTH, (float)SCREEN_HEIGHT, meshes ? NULL : picture,
                          width, height, pitch)) {
        fprintf(stderr, "[ngl] Vulkan: drawing frame %lu failed; frames are not drawn from here on\n", frames);
        state = -1;
        return;
    }
    if (describing_effects) {
        char path[1024];

        if (strstr(dump_path, "%lu"))
            snprintf(path, sizeof path, dump_path, frames);
        else
            snprintf(path, sizeof path, "%s", dump_path);
        fprintf(stderr, "[ngl] Vulkan: frame %lu: %u quads drawn, %u as placeholders; %u mesh sections\n", frames,
                drawn, placeheld, meshes);
        fprintf(stderr, "[ngl] Vulkan: sections so far: %lu drawable (%u with a texture), %lu without vertices, "
                "%lu push buffer only, %lu not triangles, %lu not float positions; skinned draws %lu, %lu without "
                "bones\n", section_results[NGL_SECTION_OK], textured_sections,
                section_results[NGL_SECTION_NO_VERTICES], section_results[NGL_SECTION_PUSH_BUFFER],
                section_results[NGL_SECTION_NOT_TRIANGLES], section_results[NGL_SECTION_NOT_FLOATS], skinned_draws,
                unskinned_draws);
        fprintf(stderr, "[ngl] Vulkan: effect draws so far: %lu drawn, %lu left out (texture without pixels)\n",
                effect_draws, untextured_effect_draws);
        report_effect_groups(frames);
        describe_quads(frames, picture_texture);
        save_target(frames, path, picture, width, height, pitch);
    }
}

#else

void ngl_present_frame(void)
{
    check_quit();
}

#endif
