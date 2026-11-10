/* [acct2-bro] Big Red One port: NGL's meshes (ngl_mesh.c): the frame's list from nglListAddMesh, and each section's
 * triangles for drawing. */
#pragma once
#include <stdint.h>

/* One nglListAddMesh call kept for the frame. The matrices are Direct3D's (row-vector, 16 floats row by row): the
 * mesh's own, and the scene's view times its projection at the time of the call. */
struct ngl_mesh_draw {
    uint32_t mesh;                  /* guest address */
    uint32_t scene;                 /* the scene it was added to */
    float world[16];
    float view_projection[16];
    uint32_t skin_flags;            /* nglListAddMesh's params[0]: 0x04, 0x08 or 0x10 say how its bones are given */
    uint32_t bones;                 /* params[2]: the bone matrices (4x4, 64 bytes each), or 0 */
    uint32_t texture_frame;         /* the frame of an animated texture list it shows (ngl_mesh.c texture_frame) */
    float fog[3];                   /* the scene's fog when it was added: start, end (from the eye), amount at end */
    uint32_t fog_color;             /* its color, red, green, blue bytes, then the amount before the start as a byte */
    /* The light set's lights for it when it was added (read_lights): lit 1 when there was a light set; the ambient,
     * and the four strongest lights of the mesh's light group as directional lights (the way each travels, world
     * space; nglComputeObjectLights hands the vertex program four): the strongest in light_direction / light_color
     * (color 0 when none), the next up to three in extra_light_*; any more add their colors to the ambient at half. */
    uint32_t lit;
    float ambient[3];
    float light_direction[3];
    float light_color[3];
    uint32_t extra_light_count;     /* run 130 */
    float extra_light_direction[3][3];
    float extra_light_color[3][3];
    /* 1: added to R_RenderTestShadow's scene (0x00241290), whose render target is the 512 x 512 shadow texture
     * (0x0095F6C8): the soldiers drawn again from above for the world's shadow projector, never on screen (run 133) */
    uint32_t shadow_scene;
};

/* This frame's shadow projector (the light set's first, +0x30, as cdWorld_Draw 0x00053440 reads it), when there is
 * one: its matrix (world to the shadow texture's u, v and a depth) made into a light view-projection for
 * ngl_vk_set_sun_shadow (Direct3D row vectors: x and y -1 to 1 across the texture, z 0 nearest the light to 1).
 * Returns 0 when the frame has none. */
int ngl_mesh_projector(float light_view_projection[16]);

/* nglListAddMesh's arguments, from its stand-in: keeps the mesh for this frame (and reports it when
 * BRO_MESH_CENSUS=1). */
void ngl_mesh_list_add(uint32_t mesh, uint32_t matrix, uint32_t params, uint32_t out);

/* cdSimpleInstance_AddListToScene (0x00046560) natively (its stand-in, "native" in bro/config/stand_in_functions.tsv):
 * each instanced prop of the list in ecx, with its own light set and scale, through ngl_mesh_list_add. */
void ngl_mesh_add_instance_list(void);

/* A draw with no mesh in the current scene (the effects' quads, ngl_quad_batch.c): the scene, an identity world
 * matrix, the scene's view-projection (made first if the game marked it stale, as nglListAddMesh does) and its fog;
 * the rest 0. Returns the scene, or 0 when there is none (d then all 0). */
uint32_t ngl_mesh_current_scene(struct ngl_mesh_draw *d);

/* This frame's meshes, in the order they were added. */
unsigned ngl_mesh_frame(const struct ngl_mesh_draw **list);

/* The frame is over (after it is drawn): starts the next frame's list. */
void ngl_mesh_end_frame(void);

/* [acct2-bro] 2026-11-08 The field of view of the scene the world (cdWorld sections) was last drawn in, as the scene
 * keeps it (+0x3BC: aspect, field of view, near, far), for the mouse's zoom sensitivity (bro_pad.c); 0 before any. */
float ngl_mesh_world_fov(void);

/* [acct2-bro] 2026-11-08 How many of the world's meshes (cdWorld sections) have been put on a draw list so far: it
 * moves on every frame the world is drawn, so the key bindings (bro_binds.c) can tell play from the loading screens
 * and the front end. */
uint32_t ngl_mesh_world_lists(void);

/* A mesh's sections (guest addresses), a section's material, and where its vertex data is (0 if nowhere: a section
 * whose data moves is a different section). */
unsigned ngl_mesh_section_count(uint32_t mesh);
uint32_t ngl_mesh_section(uint32_t mesh, unsigned i);
uint32_t ngl_mesh_section_material(uint32_t section);
uint32_t ngl_mesh_section_data(uint32_t section);

/* A section's base texture (an nglTexture, as nglFindTexture hands them out), or 0 when it has none or where its
 * texture coordinates are in a vertex isn't known for its material class. A material whose texture wasn't loaded yet
 * when its mesh was has its name looked up again. */
uint32_t ngl_mesh_section_texture(uint32_t section);

/* Reports on stderr why a section has no base texture: its material, class, the texture at material +0x1C and the
 * name at +0x18. */
void ngl_mesh_report_untextured(uint32_t section);

/* How a section is lit, by its material class (the same numbers as ngl_vk's NGL_VK_SHADE_*). */
enum {
    NGL_SHADE_LIGHT,            /* by the scene's lights (ambient and one light): cdSimple, cdChar, cdGun, ... */
    NGL_SHADE_LIGHTMAP,         /* by its lightmap: cdWorld with a second texture */
    NGL_SHADE_VERTEX_COLOR,     /* by its vertices' colors: cdWorldVertexLit, cdSimplePrelit, cdPrelit, cdWorldBlend */
    NGL_SHADE_UNLIT,            /* its vertices' colors as they are, no light: cdSky, cdBackground */
};

/* How a section's pixels use its texture's alpha, as its class's draw sets the NV2A (the same values as
 * NGL_VK_ALPHA_*): kept as they are, kept only above one half (alpha test), or blended over what's behind. */
enum { NGL_ALPHA_OPAQUE, NGL_ALPHA_TEST, NGL_ALPHA_BLEND };
int ngl_mesh_section_alpha(uint32_t section);

/* Where a section goes among its mesh's sections, lowest first: a cdSky section's layer (its material's +0x30), 0 for
 * any other. */
unsigned ngl_mesh_section_layer(uint32_t section);

/* A section's shading and, for NGL_SHADE_LIGHTMAP, its lightmap (an nglTexture, material +0x24) in *lightmap. A
 * cdWorld section without a lightmap is NGL_SHADE_LIGHT. */
int ngl_mesh_section_shading(uint32_t section, uint32_t *lightmap);

/* Whether a section mixes two textures (cdWorldBlend: its base texture and the material's +0x24 mixed by the
 * vertices' alpha, then times +0x2C when the material has one; cdWorldBlend_Draw and its pixel shaders 0x0089F090,
 * 0x0089F270), with those two (nglTextures, 0 while not loaded) in *second and *third. */
int ngl_mesh_section_blend(uint32_t section, uint32_t *second, uint32_t *third);

/* A section's triangles as host copies: its corners and a triangle list of indices. A corner is 11 floats (ngl_vk's):
 * the position x, y, z; the base texture's u, v; the lightmap's u, v; the vertex color (a D3DCOLOR, bytes blue, green,
 * red, alpha) in the eighth float's place; the normal x, y, z. What a class doesn't have is 0, the color white. A
 * cdWorldBlend corner has its second texture's coordinates where the lightmap's go and its third's where the normal
 * goes (x, y; z 0), as ngl_vk takes a mixed draw's. */
#define NGL_CORNER_FLOATS 11
struct ngl_section_triangles {
    float *corners;
    uint32_t corner_count;
    uint16_t *indices;
    uint32_t index_count;
};

enum {
    NGL_SECTION_OK,
    NGL_SECTION_NO_VERTICES,        /* no vertex data, or it isn't in guest memory */
    NGL_SECTION_PUSH_BUFFER,        /* drawn by a prebuilt push buffer only (not read yet) */
    NGL_SECTION_NOT_TRIANGLES,      /* points or lines, or no whole triangle */
    NGL_SECTION_NOT_FLOATS,         /* the first 3 values of a vertex aren't a float position */
    NGL_SECTION_RESULTS
};

/* Reads a section's triangles into t (freed with ngl_mesh_free_triangles). Returns NGL_SECTION_OK, or why not. */
int ngl_mesh_section_triangles(uint32_t section, struct ngl_section_triangles *t);
void ngl_mesh_free_triangles(struct ngl_section_triangles *t);

/* Skinned sections (cdChar's): how many bones a section's vertices name (0: not skinned), and a draw's corners for
 * one (5 floats each, as ngl_mesh_section_triangles makes them, the positions moved by the draw's bones). Returns
 * the corner count written to out, or 0 when the draw has no bones or out is too small. */
unsigned ngl_mesh_section_bone_count(uint32_t section);
uint32_t ngl_mesh_skinned_corners(const struct ngl_mesh_draw *d, uint32_t section, float *out, uint32_t max);

/* A cdSky layer's corners this frame (NGL_CORNER_FLOATS each, as ngl_mesh_section_triangles makes them), its texture
 * coordinates moved by its drift. Returns the corner count written to out, or 0 when the section doesn't drift. */
uint32_t ngl_mesh_drifted_corners(uint32_t section, float *out, uint32_t max);

/* A cdOcean section's corners for this frame (into out, at most max): its waves' height and its texture coordinates
 * as its vertex program makes them, for a node whose matrix is world; and, when foam isn't NULL, the same corners for
 * its foam (the foam's coordinates, the crests' share of it as the colors' alpha, stage 3's coordinates kept where the
 * normal goes). -1 when the section isn't cdOcean's, 0 when its wave set can't be read (it isn't drawn then). */
int ngl_mesh_ocean_corners(uint32_t section, const float world[16], float *out, float *foam, uint32_t max);

/* Whether a section's draw picks an animated texture list's frame, and which (1 and *frame set): ShaderPCUV's the
 * draw's (struct ngl_mesh_draw texture_frame), cdOcean's and cdWater's the scene's. The others show the first frame. */
int ngl_mesh_section_frame(uint32_t section, const struct ngl_mesh_draw *d, uint32_t *frame);

/* A cdOcean section's foam texture (its material's stage 2), or 0 (none, or not cdOcean's). */
uint32_t ngl_mesh_ocean_foam_texture(uint32_t section);
