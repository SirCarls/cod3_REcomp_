/* [acct2-bro] Big Red One port: NGL's meshes, as the game hands them to nglListAddMesh (0x00068850).
 *
 * The nglListAddMesh stand-in hands each call here (ngl_log.c). Each mesh is kept for the frame with its world matrix
 * and the current scene's view and projection; at the end of the frame ngl_present.c draws them through ngl_vk, each
 * section's triangles made once (ngl_mesh_section_triangles). For now a section is drawn with its base texture and one
 * fixed light: its corners' positions (the first 3 floats of each vertex), the base texture's coordinates and its
 * index list; the rest of its material and its vertex shader come later.
 *
 * Layouts, from nglListAddMesh, nglParseMesh (0x00067950), nglCreateVertexBufferPhysical (0x000668D0),
 * nglDrawMeshSection (0x00078890) and nglRelocateMeshSection (0x0007A450):
 *
 *   mesh     +0x08 section count, +0x0C sections (8 bytes each, the section at +4), +0x18 LOD table (nonzero:
 *            nglSelectMeshLod picks the mesh), +0x20/+0x24/+0x28 bounding-sphere centre, +0x30 radius
 *   section  +0x04 material, +0x28 D3D primitive type, +0x2C index count (0: not indexed), +0x30 16-bit indices,
 *            +0x34/+0x38 a prebuilt push buffer (when +0x38 is set it is run instead), +0x40 vertex count,
 *            +0x44 the vertex data (nglParseMesh relocates it; +0x48 its size in bytes, +0x4C the Direct3D vertex
 *            buffer made over it), +0x50 the stride, +0x5C relocated (a pointer to a pointer)
 *   material +0x04 its type, whose first word is a vtable: Register, GetName, AddToList (called by nglListAddMesh as
 *            (record, section, material) to put the section in the frame's lists), LoadTextures, ReleaseTextures,
 *            Relocate. +0x18 the base texture's name and +0x1C the texture (every class's LoadTextures:
 *            nglMaterial_LoadTexture 0x0004E710, nglMaterial_LoadBaseAndOptional 0x0004C6E0 for cdWorld,
 *            cdWorldBlend_LoadTextures 0x00052730), then the class's other textures.
 *   vertex   per material class, from each class's vertex programs (out/ngl_spec/nv2a_vsh.tsv): the position is
 *            3 floats at 0, and the base texture's coordinates 2 floats at 12 (cdWorld, cdWorldBlend, cdSky,
 *            cdDecal, ...) or 16 (cdSimple, cdChar, cdGun, ...); cdWorld's lightmap coordinates at 20, and a
 *            D3DCOLOR in the vertex-lit classes. See classes below.
 *   scene    (ngl_curScene, 0x0090259C) +0x150 the view matrix (3x4, rows of 4 floats; nglGetMatrix kind 2),
 *            +0x110 the projection (4x4; kind 1), +0x190 kind 3, which nglListAddMesh multiplies the mesh's by.
 *            They are made lazily: a change to the camera or perspective sets +0x3B8, and nglListAddMesh and
 *            nglGetMatrix call nglUpdateScene(0) (0x0005FC90) first when it is set. So does the stand-in here
 *            (in run 59 the projection and +0x190 were still zero when the meshes came).
 *            +0x110 is the NV2A's projection, the viewport already in it (nglUpdateScene makes it from the matrices
 *            at +0x10, +0x50 and +0x90): x/w and y/w come out in screen pixels, y down, and z/w in 0..2^24, a
 *            24-bit depth buffer's (run 60: | 477.8 0 0 0 | 0 -454.4 0 0 | 384 128 1.679e7 1 | 0 0 -1.007e8 0 |
 *            with near 6 and far 8192). screen_to_clip takes it back to Direct3D's clip space for ngl_vk.
 *
 * Skinning (cdChar), from cdChar_Draw (0x0004BD00) and nglSetSkinningConstants (0x00076620): nglListAddMesh copies
 * its 48-byte params into the frame's record; params[0] is the flags and params[2] the bone matrices (4x4, 64 bytes
 * each). A section names the bones its vertices use: +0x08 the count, +0x0C a list of 16-bit bone numbers. Each one
 * becomes three vertex-shader constants (the matrix's columns): flags 0x04 bind (mesh +0x14, 64 bytes a bone) x bone x
 * the world's inverse; else 0x08 bind x bone; else 0x10 the bone alone; else none (identity). A cdChar vertex (32
 * bytes) holds four bone slots and four weights as bytes at +24 and +28 (the program scales the slot by 765 = 255 x 3,
 * one slot per byte value). Here that is done on the CPU for each draw (ngl_mesh_skinned_corners).
 * The bind matrices' last column is not (0, 0, 0, 1) but a copy of the third, so they are read as affine (the game's
 * multiply never looks at it); taken as a full 4x4 it adds the bone's translation into the rotation rows and the
 * characters explode (runs 62 and 63). Checked by hand in run 64: model vertex x bind lands within a unit of its
 * bone, and x bone puts it back near where it started, in the new pose.
 *
 * BRO_MESH_CENSUS=1 also reports the first meshes, their sections and matrices, and every material type behind them.
 * Each line can be read against the export: vtable entries are guest function addresses (functions.tsv names them). */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "recomp_types.h"
#include "xbox_memory_layout.h"     /* MEM32 */
#include "ngl_mesh.h"
#include "ngl_scene.h"         /* ngl_call_game */
#include "ngl_texture_load.h"  /* ngl_texture_lookup */

#define NGL_CUR_SCENE       0x0090259Cu
#define SCENE_VIEW          0x150u
#define SCENE_PROJECTION    0x110u
#define SCENE_DIRTY         0x3B8u      /* a byte: the matrices need making */
#define SCENE_FOG           0x388u      /* nglSetFog: start, end, amount before the start, amount at the end (floats) */
#define SCENE_FOG_COLOR     0x3A0u      /* nglSetFogColor: red, green, blue (floats, 0..1), then 1 */
#define NGL_UPDATE_SCENE    0x0005FC90u /* nglUpdateScene, the game's own */
#define NGL_LIGHT_SET       0x00902E44u /* the current light set (nglCreateLightSet makes one and makes it current) */
#define SCENE_LIGHT_SET     0x314u      /* the scene's own, for a mesh with none (nglSelectMeshLightSet) */
#define SCENE_ASPECT_FOV    0x3BCu      /* aspect, field of view, near, far (floats) */
#define CDWORLD_REGISTER    0x000531C0u /* cdWorld's Register function (classes below) */

static float world_fov;     /* [acct2-bro] 2026-11-08 ngl_mesh_world_fov: the world's scene's field of view */
static uint32_t world_lists;    /* [acct2-bro] 2026-11-08 ngl_mesh_world_lists */

float ngl_mesh_world_fov(void)
{
    return world_fov;
}

uint32_t ngl_mesh_world_lists(void)
{
    return world_lists;
}
#define TEXTURE_FRAME_PARAM 0x00902918u /* the texture-frame param's number among a draw's params */
#define LIGHT_SET_PARAM     0x00902914u /* the light-set param's number (nglSelectMeshLightSet 0x0006FE00) */
#define DEFAULT_PARAMS      0x009028F8u /* the param block nglListAddMesh keeps for a draw given none */
#define PARAMS_SCALE        0x02u       /* nglMeshParams flag: scale the matrix's rows by +0x10/+0x14/+0x18 */
#define PARAMS_SCALE_XYZ    0x10u       /* (nglScaleMeshMatrix 0x00068680) */
#define SCENE_TIME          0x3CCu      /* a float: the scene's own time (nglSetSceneTime 0x0005E550), 0 for none */
#define NGL_FRAME_VBLANK    0x009028E8u /* the vblank count when the frame began (nglBeginFrame; ngl_frame_clock.c) */
#define SCENE_TEXTURE_FRAME 0x3D4u      /* the frame nglBeginSceneRender (0x00073ED0) would set (scene_texture_frame) */
#define TEXTURE_FRAME_RATE  0x008A2978u
#define LIGHT_SET_AMBIENT   0x60u       /* nglSetAmbientColor: red, green, blue, 1 (floats) */
#define LIGHT_NEXT_SLOTS    8           /* a light set: 8 lists (light groups) at +0x00.., ended by the set itself */
#define LIGHT_KIND          0x28u       /* a light: +4*group the next in that group, +0x28 0 point / 1 directional, */
#define LIGHT_DATA          0x2Cu       /* +0x2C its data: direction or position (4 floats), color (4), near, far */
#define MESH_LIGHT_GROUP    7u          /* a mesh's byte: its light group + 1 (0: no lights) */
#define MESH_CENTER         0x20u
#define MESH_RADIUS         0x30u

#define MAX_DRAWS           4096        /* meshes kept a frame */
#define MAX_SECTION_CORNERS 65536u      /* 16-bit indices */
#define FAR_AWAY            1.0e6f      /* a corner further out than this isn't a float position */

#define MAX_MESHES_SHOWN    40
#define CDCHAR_REGISTER     0x0004BC40u /* cdChar_Register: the skinned class */
#define CDCHAR_STRIDE       32u
/* The water classes, whose vertices are 2 floats of position (their vertex programs make the height and the texture
 * coordinates): cdRiver (run 66) and, by its place just after it and the levels it shows on (Omaha, Gela; none on
 * inland Troina, run 107), cdOcean. Read as 3 floats they came out as big flat shapes across Gela's sky. Both are drawn
 * by cdOcean_Draw (0x000505C0) with the same constants (run 129: cdRiver_AddToList, 0x0004E810, pushes a node whose
 * vtable, 0x007B661C, starts with it): see ngl_mesh_ocean_corners. */
#define CDRIVER_REGISTER    0x0004E8A0u
#define CDSKY_REGISTER      0x00054A20u
#define TEXTURE_FORMAT      0x08u       /* an nglTexture's format (nglLoadDDS's numbers; ngl_texture_load.c) */
#define CDOCEAN_REGISTER    0x0004F2A0u
#define SHADER_PCUV_REGISTER 0x00047230u  /* its draw picks an animated texture's frame (nglSetTextureAnimFrame) */
#define CDWATER_REGISTER    0x00051100u /* its draw, like cdOcean's, shows the scene's frame */
#define SKIN_FLAGS          (0x04u | 0x08u | 0x10u)
#define MAX_BONES           64u
#define MAX_SKINS_SHOWN     4
#define MAX_TYPES           64

/* Direct3D's primitive types (the Xbox's numbering) */
enum { PRIM_TRIANGLE_LIST = 5, PRIM_TRIANGLE_STRIP = 6, PRIM_TRIANGLE_FAN = 7, PRIM_QUAD_LIST = 8 };

static struct ngl_mesh_draw draws[MAX_DRAWS];
static unsigned draw_count;
static unsigned long frames, dropped;
static unsigned long skin_draws, skin_odd, skin_world_odd, skin_world_odd_shadow;  /* BRO_SKIN_WATCH (watch_skin) */
static int skin_watch_on(void);

static int census = -1;
static uint32_t meshes_seen[MAX_MESHES_SHOWN];
static int meshes_shown;
static uint32_t types[MAX_TYPES];
static unsigned long type_sections[MAX_TYPES];
static int type_count;
static unsigned long calls;

static int census_on(void)
{
    if (census < 0) {
        const char *v = getenv("BRO_MESH_CENSUS");
        census = v && *v && *v != '0';
    }
    return census;
}

/* Guest memory is the low 64 MB (and the contiguous window at 0x80000000); anything else isn't read. */
static int readable(uint32_t address)
{
    return (address >= 0x10000u && address < 0x04000000u) || (address >= 0x80000000u && address < 0x84000000u);
}

static uint32_t word(uint32_t address)
{
    return readable(address) ? MEM32(address) : 0;
}

static float real(uint32_t address)
{
    uint32_t bits = word(address);
    float f;

    memcpy(&f, &bits, sizeof f);
    return f;
}

/* ---- the census ------------------------------------------------------------------------------------------- */

static void note_type(uint32_t type)
{
    int i;

    for (i = 0; i < type_count && types[i] != type; i++)
        ;
    if (i == type_count) {
        uint32_t vtable = word(type);

        if (type_count == MAX_TYPES)
            return;
        types[type_count++] = type;
        fprintf(stderr, "[mesh] material type 0x%08X vtable 0x%08X:", type, vtable);
        for (int e = 0; e < 8; e++)
            fprintf(stderr, " %08X", word(vtable + 4u * (uint32_t)e));
        fprintf(stderr, "\n");
    }
    type_sections[i]++;
}

static void show_mesh(uint32_t mesh)
{
    uint32_t sections = word(mesh + 0x08), table = word(mesh + 0x0C);

    fprintf(stderr, "[mesh] mesh 0x%08X: %u sections at 0x%08X, lod 0x%08X, sphere %g %g %g r %g\n", mesh,
            sections, table, word(mesh + 0x18), real(mesh + 0x20), real(mesh + 0x24), real(mesh + 0x28),
            real(mesh + 0x30));
    for (uint32_t i = 0; i < sections && i < 8; i++) {
        uint32_t section = word(table + 4 + i * 8), material = word(section + 0x04);
        uint32_t data = word(section + 0x44);

        fprintf(stderr, "[mesh]   section %u 0x%08X: material 0x%08X type 0x%08X; prim %u, %u indices at 0x%08X, "
                "push buffer 0x%08X/0x%08X, %u vertices at 0x%08X (%u bytes) stride %u\n",
                i, section, material, word(material + 0x04), word(section + 0x28), word(section + 0x2C),
                word(section + 0x30), word(section + 0x34), word(section + 0x38), word(section + 0x40), data,
                word(section + 0x48), word(section + 0x50));
        if (data) {
            fprintf(stderr, "[mesh]     first vertex:");
            for (uint32_t w = 0; w < 12 && w * 4 < word(section + 0x50); w++)
                fprintf(stderr, " %08X", word(data + w * 4));
            fprintf(stderr, "  (as floats %g %g %g)\n", real(data), real(data + 4), real(data + 8));
        }
    }
}

static void show_floats(const char *what, uint32_t address, int count)
{
    fprintf(stderr, "[mesh]   %s at 0x%08X:", what, address);
    for (int i = 0; i < count; i++)
        fprintf(stderr, "%s%g", i % 4 ? " " : " | ", real(address + 4u * (uint32_t)i));
    fprintf(stderr, "\n");
}

/* The matrices a mesh is drawn with: its own (nglListAddMesh's second argument) and the current scene's, as
 * nglGetMatrix hands them out, and the scene's aspect, field of view, near and far (+0x3BC). */
static void show_matrices(uint32_t matrix)
{
    uint32_t scene = word(NGL_CUR_SCENE);

    show_floats("world", matrix, 16);
    fprintf(stderr, "[mesh]   scene 0x%08X\n", scene);
    if (!scene)
        return;
    show_floats("view (+0x150)", scene + SCENE_VIEW, 16);
    show_floats("projection (+0x110)", scene + SCENE_PROJECTION, 16);
    show_floats("+0x190", scene + 0x190, 16);
    show_floats("+0x10", scene + 0x10, 16);
    show_floats("+0x50", scene + 0x50, 16);
    show_floats("+0x90", scene + 0x90, 16);
    show_floats("viewport? (+0x338)", scene + 0x338, 8);
    show_floats("aspect, fov, near, far (+0x3BC)", scene + 0x3BC, 4);
}

static uint32_t section_class(uint32_t section);

/* A skinned draw's bones and its cdChar sections' bone lists and first vertices' slot and weight bytes. */
static void show_skin(uint32_t mesh, uint32_t params)
{
    uint32_t sections = word(mesh + 0x08), table = word(mesh + 0x0C);

    fprintf(stderr, "[mesh] skin: mesh 0x%08X, params", mesh);
    for (int i = 0; i < 12; i++)
        fprintf(stderr, " %08X", word(params + 4u * (uint32_t)i));
    fprintf(stderr, ", bind matrices (mesh +0x14) 0x%08X\n", word(mesh + 0x14));
    show_floats("bone 0", word(params + 8), 16);
    for (uint32_t i = 0; i < sections && i < 16; i++) {
        uint32_t section = word(table + 4 + i * 8), count = word(section + 0x08), list = word(section + 0x0C);
        uint32_t data = word(section + 0x44);

        if (section_class(section) != CDCHAR_REGISTER)
            continue;
        fprintf(stderr, "[mesh]   section 0x%08X: %u bones (", section, count);
        for (uint32_t b = 0; b < count && b < 12; b++)
            fprintf(stderr, " %u", (word(list + (b & ~1u) * 2) >> ((b & 1u) * 16)) & 0xFFFFu);
        fprintf(stderr, " ), stride %u, first vertices' bytes +24 / +28:", word(section + 0x50));
        for (uint32_t v = 0; v < 6; v++)
            fprintf(stderr, " %08X/%08X", word(data + v * CDCHAR_STRIDE + 24), word(data + v * CDCHAR_STRIDE + 28));
        fprintf(stderr, "\n");
        /* vertex 0's position and, for each slot it uses, its bone's and bind matrix's rows */
        fprintf(stderr, "[mesh]   vertex 0 at (%g, %g, %g)\n", real(data), real(data + 4), real(data + 8));
        for (int k = 0; k < 4; k++) {
            uint32_t slot = (word(data + 24) >> (8 * k)) & 255u, weight = (word(data + 28) >> (8 * k)) & 255u;
            uint32_t number = slot < count ? (word(list + (slot & ~1u) * 2) >> ((slot & 1u) * 16)) & 0xFFFFu : 0;
            char what[64];

            if (!weight || slot >= count)
                continue;
            fprintf(stderr, "[mesh]   slot %u (weight %u): bone %u\n", slot, weight, number);
            snprintf(what, sizeof what, "bone %u", number);
            show_floats(what, word(params + 8) + number * 64u, 16);
            snprintf(what, sizeof what, "bind %u", number);
            show_floats(what, word(mesh + 0x14) + number * 64u, 16);
        }
    }
    for (uint32_t b = 0; b < 8; b++) {
        char what[64];

        snprintf(what, sizeof what, "bone %u's row 3, then bind %u's", b, b);
        show_floats(what, word(params + 8) + b * 64u + 48u, 4);
        show_floats(what, word(mesh + 0x14) + b * 64u + 48u, 4);
    }
}

/* The params of the first meshes drawn with an animated texture list (characters' uniform variants), and NGL's
 * index of the texture-frame param (nglSetTextureAnimFrame reads it), to see where a draw's frame comes from. */
static void show_variant(uint32_t mesh, uint32_t params)
{
    static uint32_t shown[8];
    static int shown_count;
    uint32_t sections = word(mesh + 0x08), table = word(mesh + 0x0C), frames = 0, texture = 0;

    if (shown_count == 8 || !params)
        return;
    for (uint32_t i = 0; i < sections && i < 64 && !frames; i++) {
        texture = ngl_mesh_section_texture(word(table + 4 + i * 8));
        frames = texture ? ngl_texture_frame_count(texture) : 0;
    }
    if (!frames)
        return;
    for (int i = 0; i < shown_count; i++)
        if (shown[i] == mesh)
            return;
    shown[shown_count++] = mesh;
    fprintf(stderr, "[mesh] variant: mesh 0x%08X, texture 0x%08X (%u frames), frame param index %u, scene default "
            "%u; params", mesh, texture, frames, word(0x00902918u), word(word(NGL_CUR_SCENE) + 0x3D4));
    for (int i = 0; i < 12; i++)
        fprintf(stderr, " %08X", word(params + 4u * (uint32_t)i));
    fprintf(stderr, "\n");
}

static void count_census(uint32_t mesh, uint32_t matrix, uint32_t params, uint32_t out)
{
    static int raw_shown, matrices_shown, skins_shown;

    if (raw_shown < 4) {
        raw_shown++;
        fprintf(stderr, "[mesh] nglListAddMesh(0x%08X, 0x%08X, 0x%08X, 0x%08X)%s\n", mesh, matrix, params, out,
                readable(mesh) ? "" : "  (mesh outside the ranges read)");
    }
    if (!readable(mesh))
        return;
    if (skins_shown < MAX_SKINS_SHOWN && (word(params) & SKIN_FLAGS) && readable(word(params + 8))) {
        skins_shown++;
        show_skin(mesh, params);
    }
    if (matrices_shown < 3 && readable(matrix)) {
        matrices_shown++;
        show_matrices(matrix);
    }
    show_variant(mesh, params);
    calls++;
    {
        uint32_t sections = word(mesh + 0x08), table = word(mesh + 0x0C);

        for (uint32_t i = 0; i < sections && i < 64; i++) {
            uint32_t material = word(word(table + 4 + i * 8) + 0x04);

            if (material)
                note_type(word(material + 0x04));
        }
    }
    if (meshes_shown < MAX_MESHES_SHOWN) {
        for (int i = 0; i < meshes_shown; i++)
            if (meshes_seen[i] == mesh)
                return;
        meshes_seen[meshes_shown++] = mesh;
        fprintf(stderr, "[mesh] call %lu, params 0x%08X (flags 0x%08X)\n", calls, params, word(params));
        show_mesh(mesh);
    }
    if (calls % 20000 == 0) {
        fprintf(stderr, "[mesh] %lu meshes so far; sections by material type:", calls);
        for (int i = 0; i < type_count; i++)
            fprintf(stderr, " %08X x%lu", types[i], type_sections[i]);
        fprintf(stderr, "\n");
    }
}

/* ---- the frame's meshes ----------------------------------------------------------------------------------- */

/* A 4x4 matrix from guest memory; affine ones (the view) get their last column made (0, 0, 0, 1), as nglGetMatrix
 * hands them out. */
static void read_matrix(uint32_t address, int affine, float m[16])
{
    for (int i = 0; i < 16; i++)
        m[i] = real(address + 4u * (uint32_t)i);
    if (affine) {
        m[3] = m[7] = m[11] = 0.0f;
        m[15] = 1.0f;
    }
}

/* From the NV2A's screen space (the game's 640 x 480 pixels, y down; depth 0..2^24 - 1) to Direct3D's clip space
 * (x and y -1..1, y up; z 0..1), as a row-vector matrix: x' = x / 320 - w, y' = w - y / 240, z' = z / (2^24 - 1). */
static const float screen_to_clip[16] = {
    1.0f / 320.0f, 0.0f,           0.0f,                0.0f,
    0.0f,          -1.0f / 240.0f, 0.0f,                0.0f,
    0.0f,          0.0f,           1.0f / 16777215.0f,  0.0f,
    -1.0f,         1.0f,           0.0f,                1.0f,
};

/* out = a * b, Direct3D's row-vector matrices (16 floats, row by row) */
static void multiply(const float a[16], const float b[16], float out[16])
{
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++)
            out[r * 4 + c] = a[r * 4 + 0] * b[0 * 4 + c] + a[r * 4 + 1] * b[1 * 4 + c] +
                             a[r * 4 + 2] * b[2 * 4 + c] + a[r * 4 + 3] * b[3 * 4 + c];
}

static float read_float(uint32_t address)
{
    uint32_t bits = word(address);
    float f;

    memcpy(&f, &bits, sizeof f);
    return f;
}

static uint32_t unit_byte(float f)
{
    return f <= 0.0f ? 0u : f >= 1.0f ? 255u : (uint32_t)(f * 255.0f + 0.5f);
}

/* The scene's fog (nglSetFog, nglSetFogColor) into a draw, the way the classes' vertex programs get it: the amount
 * goes from the first value to the second between start and end. Logged when it changes (the first 20 times). */
static void read_fog(uint32_t scene, struct ngl_mesh_draw *d)
{
    static float last[7];
    static int changes;
    float f[7];

    for (int i = 0; i < 4; i++)
        f[i] = read_float(scene + SCENE_FOG + 4u * (uint32_t)i);
    for (int i = 0; i < 3; i++)
        f[4 + i] = read_float(scene + SCENE_FOG_COLOR + 4u * (uint32_t)i);
    d->fog[0] = f[0];
    d->fog[1] = f[1];
    d->fog[2] = f[3];
    d->fog_color = unit_byte(f[4]) | unit_byte(f[5]) << 8 | unit_byte(f[6]) << 16 | unit_byte(f[2]) << 24;
    if (memcmp(f, last, sizeof f) && changes < 20) {
        changes++;
        memcpy(last, f, sizeof f);
        fprintf(stderr, "[mesh] fog: start %g, end %g, amount %g to %g, color %g %g %g\n", f[0], f[1], f[2], f[3],
                f[4], f[5], f[6]);
    }
}

/* A draw's param n from its param block (a 64-bit mask, low word first, then a word for each param: param n at
 * +4 (n + 2)), when its bit is set. nglListAddMesh keeps the block its fourth argument points at (the first word
 * there), or DEFAULT_PARAMS for none, as the record's +0x8C; nglSetTextureAnimFrame and nglSelectMeshLightSet read it
 * from there. Run 119 read the mesh params (the third argument) instead. */
static uint32_t param_block(uint32_t out)
{
    return out && readable(out) ? word(out) : DEFAULT_PARAMS;
}

static int block_param(uint32_t block, uint32_t n, uint32_t *value)
{
    uint64_t mask;

    if (n >= 64 || !readable(block) || !readable(block + 4 * (n + 2)))
        return 0;
    mask = (uint64_t)word(block + 4) << 32 | word(block);
    if (!((mask >> n) & 1))
        return 0;
    *value = word(block + 4 * (n + 2));
    return 1;
}

/* The light set's light for a mesh, as nglSelectMeshLightSet (0x0006FE00) picks them and nglComputeObjectLights
 * (0x00070200) hands up to four to the vertex program: the draw's light-set param (the set the game made for it:
 * R_AddDObjSurfaces, R_AddStaticModel, cdSimpleInstance_AddListToScene), else the scene's (the set current when the
 * mesh was added if the scene has none); the lights of the mesh's light group (mesh byte +7, minus 1), directional
 * ones always and point ones whose range reaches the mesh's bounding sphere; and the set's ambient. Here the strongest
 * becomes the draw's first directional light (a point light: from it to the mesh's centre, dimmed linearly from near
 * to far) and the next three its extra lights (run 130: ngl_vk lights by four, as the game's vertex programs do; a
 * muzzle flash or a fire near a soldier lights him beside the sun); any more are added to the ambient at half their
 * color, as all but the strongest were until run 129. Lights that add nothing are skipped (run 131). */
/* This frame's shadow projector (read_lights keeps it; ngl_mesh_end_frame forgets it): its 4x3 matrix's rows. */
static float projector_rows[4][3];
static int projector_live;

int ngl_mesh_projector(float light_view_projection[16])
{
    if (!projector_live)
        return 0;
    /* the record takes a world position (row vector) to the texture's u and v (0 to 1, 0.5 at its centre) and a depth
     * (0 at the light, falling 0.002 a unit below it); clip x = 2u - 1, y = 1 - 2v (the texture's v runs down), z as
     * it is */
    for (int row = 0; row < 4; row++) {
        light_view_projection[4 * row + 0] = 2.0f * projector_rows[row][0];
        light_view_projection[4 * row + 1] = -2.0f * projector_rows[row][1];
        light_view_projection[4 * row + 2] = projector_rows[row][2];
        light_view_projection[4 * row + 3] = 0.0f;
    }
    light_view_projection[12] -= 1.0f;
    light_view_projection[13] += 1.0f;
    light_view_projection[15] = 1.0f;
    return 1;
}

static void read_lights(uint32_t scene, uint32_t mesh, uint32_t block, struct ngl_mesh_draw *d)
{
    uint32_t set = 0;
    float center[3], radius;
    float top_luminance[4], top_direction[4][3], top_color[4][3];  /* the strongest four, strongest first */
    int group, count = 0, top = 0;

    d->lit = 0;
    memset(d->ambient, 0, sizeof d->ambient);
    memset(d->light_direction, 0, sizeof d->light_direction);
    memset(d->light_color, 0, sizeof d->light_color);
    d->extra_light_count = 0;
    memset(d->extra_light_direction, 0, sizeof d->extra_light_direction);
    memset(d->extra_light_color, 0, sizeof d->extra_light_color);
    if (block_param(block, word(LIGHT_SET_PARAM), &set) && readable(set)) {
        static int shown;

        if (shown < 8) {
            shown++;
            fprintf(stderr, "[mesh] light set 0x%08X from the draw's params (mesh 0x%08X, group %u, ambient %g %g %g)\n",
                    set, mesh, word(mesh + 4) >> 24 & 0xFFu, read_float(set + LIGHT_SET_AMBIENT),
                    read_float(set + LIGHT_SET_AMBIENT + 4), read_float(set + LIGHT_SET_AMBIENT + 8));
        }
    } else {
        set = word(scene + SCENE_LIGHT_SET);
    }
    if (!set || !readable(set))
        set = word(NGL_LIGHT_SET);
    if (!set || !readable(set))
        return;
    d->lit = 1;
    for (int i = 0; i < 3; i++)
        d->ambient[i] = read_float(set + LIGHT_SET_AMBIENT + 4u * (uint32_t)i);
    {
        /* Run 132: the light set's projectors (+0x30, nglLightSetAddProjector from nglAddShadowProjector 0x000704F0),
         * as cdWorld_Draw (0x00053440) reads the first: +0x2C its record, +0x88 the texture, +0xA0 / +0xA4 its centre's
         * x and y, the 4x3 matrix at +0x00 (rows +0x00, +0x10, +0x20, +0x30). */
        static int projector_shown;
        uint32_t node = word(set + 0x30);

        if (node && node != set + 0x30 && readable(node) && readable(word(node + 0x2C))) {
            uint32_t record = word(node + 0x2C);    /* run 133: kept for the frame (ngl_mesh_projector) */

            for (uint32_t row = 0; row < 4; row++)
                for (uint32_t column = 0; column < 3; column++)
                    projector_rows[row][column] = read_float(record + 16u * row + 4u * column);
            projector_live = 1;
        }
        if (projector_shown < 6 && node && node != set + 0x30 && readable(node)) {
            uint32_t record = word(node + 0x2C);

            projector_shown++;
            fprintf(stderr, "[mesh] light set 0x%08X has a projector: record 0x%08X", set, record);
            if (record && readable(record))
                fprintf(stderr, ", texture 0x%08X, centre %g %g, rows %g %g %g / %g %g %g / %g %g %g / %g %g %g",
                        word(record + 0x88), read_float(record + 0xA0), read_float(record + 0xA4),
                        read_float(record), read_float(record + 4), read_float(record + 8), read_float(record + 16),
                        read_float(record + 20), read_float(record + 24), read_float(record + 32),
                        read_float(record + 36), read_float(record + 40), read_float(record + 48),
                        read_float(record + 52), read_float(record + 56));
            fprintf(stderr, "\n");
        }
    }
    group = (int)(word(mesh + 4) >> 24 & 0xFFu) - 1;     /* the byte at +7 (the top of the little-endian word at +4) */
    if (group < 0 || group >= LIGHT_NEXT_SLOTS)
        return;
    for (int i = 0; i < 3; i++)                         /* the bounding sphere's centre, in the world (row vector) */
        center[i] = d->world[12 + i] + read_float(mesh + MESH_CENTER) * d->world[i] +
                    read_float(mesh + MESH_CENTER + 4) * d->world[4 + i] +
                    read_float(mesh + MESH_CENTER + 8) * d->world[8 + i];
    radius = read_float(mesh + MESH_RADIUS);
    for (uint32_t light = word(set + 4u * (uint32_t)group); light && light != set && readable(light) && count < 64;
         light = word(light + 4u * (uint32_t)group), count++) {
        uint32_t kind = word(light + LIGHT_KIND), data = word(light + LIGHT_DATA);
        float direction[3], color[3], strength = 1.0f, luminance;

        if (!data || !readable(data) || kind > 1)
            continue;
        for (int i = 0; i < 3; i++) {
            direction[i] = read_float(data + 4u * (uint32_t)i);
            color[i] = read_float(data + 16u + 4u * (uint32_t)i);
        }
        if (kind == 0) {                                /* a point light: data +0 its position, +32 near, +36 far */
            float near_range = read_float(data + 32), far_range = read_float(data + 36), length = 0.0f;

            for (int i = 0; i < 3; i++) {
                direction[i] = center[i] - direction[i];
                length += direction[i] * direction[i];
            }
            length = sqrtf(length);
            if (length > far_range + radius)
                continue;
            if (length > 0.0f)
                for (int i = 0; i < 3; i++)
                    direction[i] /= length;
            strength = far_range > near_range ? (far_range - length) / (far_range - near_range) : 1.0f;
            strength = strength < 0.0f ? 0.0f : strength > 1.0f ? 1.0f : strength;
        }
        /* A light that adds nothing here (black, or a point light at its far edge) takes no slot (run 131); a dark
         * light (negative color) still counts, since the vertex program adds it like any other. */
        if (strength == 0.0f || (color[0] == 0.0f && color[1] == 0.0f && color[2] == 0.0f))
            continue;
        luminance = strength * (0.3f * color[0] + 0.59f * color[1] + 0.11f * color[2]);
        {
            int slot = top;

            while (slot > 0 && top_luminance[slot - 1] < luminance)
                slot--;
            if (slot >= 4) {                            /* weaker than the four kept: to the ambient */
                for (int i = 0; i < 3; i++)
                    d->ambient[i] += 0.5f * strength * color[i];
                continue;
            }
            if (top == 4) {                             /* the fourth gives way and goes to the ambient */
                for (int i = 0; i < 3; i++)
                    d->ambient[i] += 0.5f * top_color[3][i];
            } else {
                top++;
            }
            for (int k = top - 1; k > slot; k--) {
                top_luminance[k] = top_luminance[k - 1];
                memcpy(top_direction[k], top_direction[k - 1], sizeof top_direction[k]);
                memcpy(top_color[k], top_color[k - 1], sizeof top_color[k]);
            }
            top_luminance[slot] = luminance;
            memcpy(top_direction[slot], direction, sizeof direction);
            for (int i = 0; i < 3; i++)
                top_color[slot][i] = strength * color[i];
        }
    }
    if (top > 0) {
        memcpy(d->light_direction, top_direction[0], sizeof d->light_direction);
        memcpy(d->light_color, top_color[0], sizeof d->light_color);
    }
    d->extra_light_count = top > 1 ? (uint32_t)(top - 1) : 0u;
    for (int k = 1; k < top; k++) {
        memcpy(d->extra_light_direction[k - 1], top_direction[k], sizeof d->extra_light_direction[k - 1]);
        memcpy(d->extra_light_color[k - 1], top_color[k], sizeof d->extra_light_color[k - 1]);
    }
    if (top > 1) {
        static int shown_lights;

        if (shown_lights < 8) {
            shown_lights++;
            fprintf(stderr, "[mesh] %d lights kept for mesh 0x%08X (group %d): luminance %g, %g, %g, %g\n", top, mesh,
                    group, top_luminance[0], top_luminance[1], top > 2 ? top_luminance[2] : 0.0f,
                    top > 3 ? top_luminance[3] : 0.0f);
        }
    }
}

/* A scene's animated-texture frame, made here the way nglBeginSceneRender (0x00073ED0) makes it at the start of the
 * scene's drawing, which the native drawing never runs (run 120: always 0): the scene's own time (+0x3CC,
 * nglSetSceneTime) when it has one, else the frame's vblank count x 16.667 ms (NTSC; nglIsPAL50 would say 20 ms),
 * times 0x008A2978 (30 frames a second), rounded. The vblank count comes from the host's clock
 * (ngl_frame_clock.c), so the textures move at the same speed at any frame rate. */
static uint32_t scene_texture_frame(uint32_t scene)
{
    float seconds = real(scene + SCENE_TIME);

    if (seconds == 0.0f)
        seconds = (float)word(NGL_FRAME_VBLANK) * 16.666666f * 0.001f;
    return (uint32_t)lrintf(seconds * real(TEXTURE_FRAME_RATE));
}

/* Which frame of an animated texture list a draw shows, as nglSetTextureAnimFrame (0x000476B0) picks it (Call of
 * Duty 3's 0x820D48F0 does the same): the draw's own texture-frame param when its bit is set, else the scene's
 * frame. Only ShaderPCUV's draw (0x000472E0) calls it (ngl_mesh_section_frame); the characters' uniform variants
 * (cdChar) keep the first frame (run 69). */
static uint32_t texture_frame(uint32_t block, uint32_t scene)
{
    uint32_t n = word(TEXTURE_FRAME_PARAM), frame;
    static uint32_t next_shown;
    static int shown;

    if (shown < 4 && word(NGL_FRAME_VBLANK) >= next_shown && readable(scene + SCENE_TEXTURE_FRAME)) {
        shown++;
        next_shown = word(NGL_FRAME_VBLANK) + 1200;     /* every 20 seconds */
        fprintf(stderr, "[mesh] texture frames: param %u, scene time %g, vblank %u, rate %g: scene frame %u\n", n,
                real(scene + SCENE_TIME), word(NGL_FRAME_VBLANK), real(TEXTURE_FRAME_RATE),
                scene_texture_frame(scene));
    }
    if (block_param(block, n, &frame))
        return frame;
    return scene_texture_frame(scene);
}

/* nglScaleMeshMatrix (0x00068680), for a draw whose params have the scale flag (cdSimpleInstance_AddListToScene's
 * props): the matrix's first three rows times the params' x, y and z scale; the translation as it was. */
static void scale_matrix(uint32_t params, float world[16])
{
    static int shown;

    if (!params || !readable(params + PARAMS_SCALE_XYZ + 8) || !(word(params) & PARAMS_SCALE))
        return;
    if (shown < 4) {
        shown++;
        fprintf(stderr, "[mesh] scaled draw: params 0x%08X flags 0x%X, scale %g %g %g, at %.1f %.1f %.1f\n", params,
                word(params), real(params + PARAMS_SCALE_XYZ), real(params + PARAMS_SCALE_XYZ + 4),
                real(params + PARAMS_SCALE_XYZ + 8), world[12], world[13], world[14]);
    }
    for (int r = 0; r < 3; r++) {
        float s = real(params + PARAMS_SCALE_XYZ + 4u * (uint32_t)r);

        for (int c = 0; c < 4; c++)
            world[4 * r + c] *= s;
    }
}

/* [acct2-bro] 2026-11-08 BRO_SKIN_WATCH=1, run t236: the bones were all rigid, but the matrix nglListAddMesh gets for a
 * skinned draw had a row as short as 0.64 in up to half of them (the soldier squashed along that axis). Here, for such
 * a matrix (a row below 0.9 or above 1.1), at most one a frame, every 15th frame, 80 in all: where it is, the draw's
 * mesh, params flags and scene, the three row lengths, how square the rows are to each other (the cosines between
 * them: 0 for a rotation) and the determinant (the volume: 1 for a rotation), and the matrix itself. A scale keeps the
 * rows square; a blend of two turns shortens them and leaves them square; a shear doesn't. */
static void watch_skin_matrix(const struct ngl_mesh_draw *d, uint32_t matrix)
{
    static int shown;
    static unsigned long last_frame = ~0ul;
    const float *m = d->world;
    float len[3], cosine[3];
    float det = m[0] * (m[5] * m[10] - m[6] * m[9]) - m[1] * (m[4] * m[10] - m[6] * m[8]) +
                m[2] * (m[4] * m[9] - m[5] * m[8]);
    int odd = 0;

    for (int r = 0; r < 3; r++) {
        len[r] = sqrtf(m[4 * r] * m[4 * r] + m[4 * r + 1] * m[4 * r + 1] + m[4 * r + 2] * m[4 * r + 2]);
        odd |= len[r] < 0.9f || len[r] > 1.1f;
    }
    if (!odd || shown >= 80 || frames == last_frame || frames % 15)
        return;
    for (int k = 0; k < 3; k++) {
        int a = k == 2 ? 1 : 0, b = k == 0 ? 1 : 2;
        float dot = m[4 * a] * m[4 * b] + m[4 * a + 1] * m[4 * b + 1] + m[4 * a + 2] * m[4 * b + 2];

        cosine[k] = len[a] > 0.0f && len[b] > 0.0f ? dot / (len[a] * len[b]) : 0.0f;
    }
    shown++;
    last_frame = frames;
    fprintf(stderr, "[skin] frame %lu: matrix 0x%08X mesh 0x%08X flags 0x%X scene 0x%08X%s: rows %.3f %.3f %.3f, "
            "cosines 01 %.3f 02 %.3f 12 %.3f, det %.3f; %.3f %.3f %.3f %.3f | %.3f %.3f %.3f %.3f | %.3f %.3f %.3f "
            "%.3f | %.1f %.1f %.1f %.3f\n", frames, matrix, d->mesh, d->skin_flags, d->scene,
            d->shadow_scene ? " (the shadow scene)" : "", len[0], len[1], len[2], cosine[0], cosine[1], cosine[2], det,
            real(matrix), real(matrix + 4), real(matrix + 8), real(matrix + 12), real(matrix + 16), real(matrix + 20),
            real(matrix + 24), real(matrix + 28), real(matrix + 32), real(matrix + 36), real(matrix + 40),
            real(matrix + 44), real(matrix + 48), real(matrix + 52), real(matrix + 56), real(matrix + 60));
}

void ngl_mesh_list_add(uint32_t mesh, uint32_t matrix, uint32_t params, uint32_t out)
{
    uint32_t scene = word(NGL_CUR_SCENE);
    struct ngl_mesh_draw *d;
    float view[16], projection[16], view_screen[16];

    if (scene && readable(scene) && (word(scene + SCENE_DIRTY) & 0xFFu))
        ngl_call_game(NGL_UPDATE_SCENE, 1, 0);      /* as nglListAddMesh does */
    if (census_on())
        count_census(mesh, matrix, params, out);
    if (!readable(mesh) || !readable(matrix) || !scene)
        return;
    if (draw_count == MAX_DRAWS) {
        dropped++;
        return;
    }
    d = &draws[draw_count++];
    d->mesh = mesh;
    d->scene = scene;
    if (word(mesh + 0x08) && readable(word(mesh + 0x0C)) &&
        section_class(word(word(mesh + 0x0C) + 4)) == CDWORLD_REGISTER) {
        world_fov = real(scene + SCENE_ASPECT_FOV + 4);     /* [acct2-bro] 2026-11-08 ngl_mesh_world_fov */
        world_lists++;
    }
    d->skin_flags = params ? word(params) : 0;
    d->bones = params ? word(params + 8) : 0;
    d->texture_frame = texture_frame(param_block(out), scene);
    read_matrix(matrix, 1, d->world);
    scale_matrix(params, d->world);
    read_matrix(scene + SCENE_VIEW, 1, view);
    read_matrix(scene + SCENE_PROJECTION, 0, projection);
    multiply(view, projection, view_screen);
    multiply(view_screen, screen_to_clip, d->view_projection);
    {
        /* Run 132: each scene its meshes come in, once, with its render target (+0x2F8: +0x10 width, +0x14 height, +0x28
         * flags) and whether its projection is orthographic (+0x110: row 3 col 3 is 1, col 2 is 0). R_RenderTestShadow
         * (0x00241290, every frame while r_testshadow is 1, its default) draws the soldiers again into a 512 x 512
         * texture (0x0095F6C8) from above for the world's shadow projector; the native side may be drawing them on
         * screen instead. */
        static uint32_t seen_scene[16], seen_target[16];
        static int seen;
        uint32_t target = word(scene + 0x2F8);
        int known = 0;

        d->shadow_scene = target && target == word(0x0095F6C8u);     /* run 133 */
        for (int i = 0; i < seen; i++)
            known |= seen_scene[i] == scene && seen_target[i] == target;
        if (!known && seen < 16) {
            seen_scene[seen] = scene;
            seen_target[seen++] = target;
            fprintf(stderr, "[mesh] scene 0x%08X: render target 0x%08X", scene, target);
            if (target && readable(target))
                fprintf(stderr, " (%u x %u, flags 0x%02X)", word(target + 0x10), word(target + 0x14),
                        word(target + 0x28) & 0xFFu);
            fprintf(stderr, ", projection row 3: %g %g %g %g%s; first mesh 0x%08X; the shadow texture 0x%08X\n",
                    projection[12], projection[13], projection[14], projection[15],
                    projection[15] == 1.0f && projection[11] == 0.0f ? " (orthographic)" : "", mesh,
                    word(0x0095F6C8u));
        }
    }
    {
        /* Since run 99 every perspective scene's view-projection is NaN (run 99p): the first few such scenes' view,
         * projection and camera numbers, to see which of them is NaN first (run 101). */
        static int nan_shown;
        int nan = 0;

        for (int i = 0; i < 16; i++)
            nan |= isnan(d->view_projection[i]);
        if (nan && nan_shown < 3) {
            nan_shown++;
            fprintf(stderr, "[mesh] NaN view-projection, mesh 0x%08X (view NaN %d, projection NaN %d):\n", mesh,
                    isnan(view[0]) || isnan(view[5]) || isnan(view[10]) || isnan(view[12]) || isnan(view[13]) ||
                        isnan(view[14]),
                    isnan(projection[0]) || isnan(projection[5]) || isnan(projection[10]) || isnan(projection[14]));
            show_matrices(matrix);
            /* run 102: nglUpdateScene's tables are sound; the NaN comes from the scene's viewport, which the viewport
             * code (0x00061B70) makes from its render target's size (+0x2F8: +0x10 width, +0x14 height, +0x28 flags,
             * 4 = the screen's size instead). Run 103 gave the back buffer stand-in a size and changed nothing: so the
             * scene's render target, and the screen rectangle (+0x328..+0x334) made from it (run 104). */
            {
                uint32_t target = word(scene + 0x2F8);

                fprintf(stderr, "[mesh]   render target (+0x2F8) 0x%08X", target);
                if (target && readable(target))
                    fprintf(stderr, ": size %u x %u, flags +0x28 0x%02X, first words %08X %08X %08X %08X",
                            word(target + 0x10), word(target + 0x14), word(target + 0x28) & 0xFFu, word(target),
                            word(target + 4), word(target + 8), word(target + 12));
                fprintf(stderr, "\n");
                show_floats("screen rectangle (+0x328)", scene + 0x328, 4);
            }
        }
    }
    read_fog(scene, d);
    read_lights(scene, mesh, param_block(out), d);
    if ((d->skin_flags & SKIN_FLAGS) && skin_watch_on())
        watch_skin_matrix(d, matrix);
}

/* cdSimpleInstance_AddListToScene (0x00046560, fastcall: ecx the list), done natively. The recompiler leaves every
 * stand-in's body unlifted, so the game's own can't simply come back (run 120's first build failed to link). The
 * lists are Scn_AddModelInstances' (0x002D90F0), drawn every frame from R_RenderView through
 * R_AddSimpleInstancesToScene (0x002A8D30): +0x00 the mesh, +0x0C the count, +0x10 the instances (0x80 bytes each,
 * cdSimpleInstance_AddInstance 0x000463C0): +0x00 the matrix (its third row made from the first two), +0x40 the
 * scale, +0x50 the light's direction, +0x60 its color, +0x70 the ambient. For each, as the game does: a param block
 * from the frame arena (8-aligned, 2 + 0x00902904 words, the mask cleared) holding a new light set (the game's
 * nglCreateLightSet) as the light-set param; the game's nglAddDirectionalLight (all groups) and nglSetAmbientColor
 * on it; and nglListAddMesh (here ngl_mesh_list_add) with params { flags PARAMS_SCALE, scale x 4 at +0x10 }. Nothing
 * when cdSimpleInstance_Toggle has turned them off (bit 2 of the byte at 0x00901E8E). */
#define NGL_FRAME_ARENA_PTR     0x009025A8u
#define PARAM_WORDS             0x00902904u /* how many params a param block holds */
#define SIMPLE_INSTANCE_FLAGS   0x00901E8Cu /* the word holding the byte at 0x00901E8E: bit 2 there, props off */
#define SIMPLE_INSTANCES_OFF    (0x02u << 16)
#define NGL_CREATE_LIGHT_SET    0x0006F7C0u
#define NGL_ADD_DIRECTIONAL_LIGHT 0x0006F960u
#define NGL_SET_AMBIENT_COLOR   0x0006F8D0u
#define INSTANCE_BYTES          0x80u
#define MESH_PARAMS_BYTES       0x30u       /* what nglListAddMesh copies of its params */
#define MAX_INSTANCES           65536u

static uint32_t arena_take(uint32_t bytes, uint32_t align)
{
    uint32_t at = (word(NGL_FRAME_ARENA_PTR) + align - 1u) & ~(align - 1u);

    MEM32(NGL_FRAME_ARENA_PTR) = at + bytes;
    return at;
}

void ngl_mesh_add_instance_list(void)
{
    uint32_t list = g_ecx, mesh = word(list), count = word(list + 0x0C), instances = word(list + 0x10);
    static unsigned long lists, added;

    if (readable(list) && !(word(SIMPLE_INSTANCE_FLAGS) & SIMPLE_INSTANCES_OFF) && readable(mesh) &&
        readable(instances) && word(NGL_FRAME_ARENA_PTR) && count <= MAX_INSTANCES) {
        for (uint32_t i = 0; i < count; i++) {
            uint32_t instance = instances + i * INSTANCE_BYTES, n = word(LIGHT_SET_PARAM), block, params;
            uint32_t light[3] = { 0xFFFFFFFFu, instance + 0x50, instance + 0x60 };
            uint32_t ambient[3] = { word(instance + 0x70), word(instance + 0x74), word(instance + 0x78) };

            block = arena_take(4u * (word(PARAM_WORDS) + 2u), 8);
            MEM32(block) = 0;
            MEM32(block + 4) = 0;
            ngl_call_game(NGL_CREATE_LIGHT_SET, 0, 0);
            MEM32(block) |= 1u << (n & 31u);
            MEM32(block + 4u * (n + 2u)) = g_eax;                  /* the new set */
            ngl_call_game_args(NGL_ADD_DIRECTIONAL_LIGHT, 3, light);
            ngl_call_game_args(NGL_SET_AMBIENT_COLOR, 3, ambient);
            params = arena_take(MESH_PARAMS_BYTES + 4u, 16);
            for (uint32_t k = 0; k < MESH_PARAMS_BYTES; k += 4)
                MEM32(params + k) = 0;
            MEM32(params) = PARAMS_SCALE;
            for (uint32_t k = 0; k < 4; k++)
                MEM32(params + PARAMS_SCALE_XYZ + 4u * k) = word(instance + 0x40);
            MEM32(params + MESH_PARAMS_BYTES) = block;               /* the fourth argument points at the block */
            ngl_mesh_list_add(mesh, instance, params, params + MESH_PARAMS_BYTES);
            added++;
        }
        if (++lists <= 4 || (lists & (lists - 1)) == 0)
            fprintf(stderr, "[mesh] instanced props: list %lu (0x%08X, mesh 0x%08X, %u), %lu added so far\n", lists,
                    list, mesh, count, added);
    }
    g_esp += 4u;    /* ret: fastcall, nothing on the stack */
}

uint32_t ngl_mesh_current_scene(struct ngl_mesh_draw *d)
{
    uint32_t scene = word(NGL_CUR_SCENE);
    float view[16], projection[16], view_screen[16];

    memset(d, 0, sizeof *d);
    if (!scene || !readable(scene))
        return 0;
    if (word(scene + SCENE_DIRTY) & 0xFFu)
        ngl_call_game(NGL_UPDATE_SCENE, 1, 0);      /* as nglListAddMesh does */
    d->scene = scene;
    for (int i = 0; i < 16; i += 5)
        d->world[i] = 1.0f;
    read_matrix(scene + SCENE_VIEW, 1, view);
    read_matrix(scene + SCENE_PROJECTION, 0, projection);
    multiply(view, projection, view_screen);
    multiply(view_screen, screen_to_clip, d->view_projection);
    read_fog(scene, d);
    return scene;
}

unsigned ngl_mesh_frame(const struct ngl_mesh_draw **list)
{
    *list = draws;
    return draw_count;
}

void ngl_mesh_end_frame(void)
{
    frames++;
    if (skin_watch_on() && frames % 600 == 0) {
        fprintf(stderr, "[skin] frames %lu-%lu: %lu skinned draws, %lu with a palette row off 1 by over 10 %%, %lu with "
                "a world row (%lu of them in the shadow scene)\n", frames - 599, frames, skin_draws, skin_odd,
                skin_world_odd, skin_world_odd_shadow);
        skin_draws = skin_odd = skin_world_odd = skin_world_odd_shadow = 0;
    }
    if (dropped && frames % 300 == 0) {
        fprintf(stderr, "[mesh] %lu meshes over the %d a frame kept, so far\n", dropped, MAX_DRAWS);
        dropped = 0;
    }
    draw_count = 0;
    projector_live = 0;
}

unsigned ngl_mesh_section_count(uint32_t mesh)
{
    uint32_t sections = word(mesh + 0x08);

    return sections < 256 ? sections : 0;
}

uint32_t ngl_mesh_section(uint32_t mesh, unsigned i)
{
    return word(word(mesh + 0x0C) + 4 + i * 8u);
}

uint32_t ngl_mesh_section_material(uint32_t section)
{
    return word(section + 0x04);
}

uint32_t ngl_mesh_section_data(uint32_t section)
{
    return word(section + 0x44);
}

/* A normal's offset in the class table for the classes whose vertex programs read v1 as packedx1 (NORMPACKED3, one word
 * at +12: x and y signed 11 bits, z signed 10 bits; out/ngl_spec/nv2a_vsh.tsv: cdChar, cdGun, cdGunSight, ...). A
 * plain offset is three floats (cdAirplaneMetal's, whose texture coordinates start at 24). */
#define NORMAL_PACKED 0x80000000u

/* What a vertex holds where, and how the class is lit, by material class (the class's Register function, entry 0 of
 * its type's vtable), from the inputs of the class's vertex programs (out/ngl_spec/nv2a_vsh.tsv): the base texture's
 * coordinates, the lightmap's (cdWorld's second set), and a vertex color (D3DCOLOR). Classes not listed get no
 * texture; cdOcean's and cdRiver's positions are 2 floats, and their height and texture coordinates are made each
 * frame (ngl_mesh_ocean_corners).
 * The alpha column is what each class's draw (cdX_Draw) sets on the NV2A: TEST is alpha test on, alpha greater than
 * 0x80, no blending (cdChar, cdGun, cdSimple, ...; cdBackground: greater or equal); BLEND is source-alpha blending
 * (cdGlass, cdDecal, cdWater, cdPropeller, cdSimpleAlpha); OPAQUE neither (cdWorld's extra blended passes aren't drawn
 * yet; cdSky's cloud layers blend, see ngl_mesh_section_alpha). */
static const struct material_class {
    uint32_t register_function;
    uint32_t uv, lightmap_uv, color;        /* offsets in a vertex; 0: none */
    int shading;
    int alpha;                              /* NGL_ALPHA_* */
    const char *name;
    uint32_t normal;                        /* the normal's offset: NORMAL_PACKED at 12 (NORMPACKED3), or floats */
    uint32_t third_uv;                      /* a third texture's coordinates (cdWorldBlend), in the normal's place */
} classes[] = {
    { 0x000531C0u, 12, 20, 0, NGL_SHADE_LIGHTMAP, NGL_ALPHA_OPAQUE, "cdWorld", 0 },
    { 0x00051890u, 12, 20, 36, NGL_SHADE_VERTEX_COLOR, NGL_ALPHA_OPAQUE, "cdWorldBlend", 0, 28 },
    { 0x00052B90u, 12, 0, 20, NGL_SHADE_VERTEX_COLOR, NGL_ALPHA_OPAQUE, "cdWorldVertexLit", 0 },
    { 0x00054A20u, 12, 0, 20, NGL_SHADE_UNLIT, NGL_ALPHA_OPAQUE, "cdSky", 0 },
    { 0x000543E0u, 12, 0, 20, NGL_SHADE_UNLIT, NGL_ALPHA_TEST, "cdBackground", 0 },
    { 0x0004B4A0u, 12, 0, 0, NGL_SHADE_LIGHT, NGL_ALPHA_BLEND, "cdDecal", 0 },
    { 0x0004DCA0u, 12, 0, 20, NGL_SHADE_VERTEX_COLOR, NGL_ALPHA_TEST, "cdSimplePrelit", 0 },
    { 0x00047230u, 12, 0, 0, NGL_SHADE_LIGHT, NGL_ALPHA_OPAQUE, "ShaderPCUV", 0 },
    { 0x00048920u, 12, 0, 0, NGL_SHADE_LIGHT, NGL_ALPHA_BLEND, "cdGlass", 0 },
    { 0x00051100u, 12, 0, 0, NGL_SHADE_LIGHT, NGL_ALPHA_BLEND, "cdWater", 0 },
    { 0x0004E390u, 16, 0, 0, NGL_SHADE_LIGHT, NGL_ALPHA_TEST, "cdSimple", NORMAL_PACKED },
    { 0x0004D400u, 16, 0, 0, NGL_SHADE_LIGHT, NGL_ALPHA_TEST, "cdSimpleUVAnim", NORMAL_PACKED },
    { 0x00049B90u, 16, 0, 0, NGL_SHADE_LIGHT, NGL_ALPHA_BLEND, "cdSimpleAlpha", NORMAL_PACKED },
    { 0x0004BC40u, 16, 0, 0, NGL_SHADE_LIGHT, NGL_ALPHA_TEST, "cdChar", NORMAL_PACKED },
    { 0x0004C380u, 16, 0, 12, NGL_SHADE_VERTEX_COLOR, NGL_ALPHA_OPAQUE, "cdPrelit", 0 },
    { 0x00048260u, 16, 0, 0, NGL_SHADE_LIGHT, NGL_ALPHA_TEST, "cdGun", NORMAL_PACKED },
    { 0x00047A50u, 16, 0, 0, NGL_SHADE_LIGHT, NGL_ALPHA_TEST, "cdGunSight", NORMAL_PACKED },
    { 0x0004ACA0u, 16, 0, 0, NGL_SHADE_LIGHT, NGL_ALPHA_BLEND, "cdPropeller", NORMAL_PACKED },
    { 0x0004CAF0u, 24, 0, 0, NGL_SHADE_LIGHT, NGL_ALPHA_OPAQUE, "cdAirplaneMetal", 12 },
    /* positions 2 floats at 0, stage 3's coordinates at 8 (the third texture's place); the rest is made each frame */
    { CDOCEAN_REGISTER, 0, 0, 0, NGL_SHADE_VERTEX_COLOR, NGL_ALPHA_OPAQUE, "cdOcean", 0, 8 },  /* fogged */
    /* positions 2 floats at 0, its own coordinates at 8 (v1, stages 0-2 less their drift), stage 3's at 16 (v2) */
    { CDRIVER_REGISTER, 8, 0, 0, NGL_SHADE_VERTEX_COLOR, NGL_ALPHA_OPAQUE, "cdRiver", 0, 16 },  /* fogged */
};

/* A section's material class: its type's Register function (vtable entry 0), or 0. */
static uint32_t section_class(uint32_t section)
{
    uint32_t material = word(section + 0x04), type = material ? word(material + 0x04) : 0;

    return type ? word(word(type)) : 0;
}

/* A section's class in the table above, or NULL when it isn't there or its vertices are too short for it. */
static const struct material_class *section_class_info(uint32_t section)
{
    uint32_t register_function = section_class(section), stride = word(section + 0x50);

    for (size_t i = 0; i < sizeof classes / sizeof classes[0]; i++) {
        const struct material_class *c = &classes[i];

        if (c->register_function == register_function)
            return c->uv + 8 <= stride && c->lightmap_uv + 8 <= stride && c->color + 4 <= stride &&
                   c->third_uv + 8 <= stride ? c : NULL;
    }
    return NULL;
}

/* The base texture's coordinates' offset in a section's vertices, or 0 if not known. */
static uint32_t section_uv_offset(uint32_t section)
{
    const struct material_class *c = section_class_info(section);

    return c ? c->uv : 0;
}

/* A material's texture at +offset, or, while that is 0, the one loaded under the name just before it:
 * nglMaterial_LoadTexture looks the name up once, as the mesh loads, and a texture loaded after its mesh is found
 * here instead. */
static uint32_t material_texture(uint32_t material, uint32_t offset)
{
    uint32_t texture = word(material + offset), name = word(material + offset - 4);

    if (!texture && readable(name) && readable(name + 31) && word(name))
        texture = ngl_texture_lookup(name);
    return texture;
}

/* The classes cdOcean_Draw draws: cdOcean, and cdRiver (its own vertex program and the same pixel shaders, run 129). */
static int waves_class(uint32_t type)
{
    return type == CDOCEAN_REGISTER || type == CDRIVER_REGISTER;
}

/* cdOcean's material (cdRiver's too: the class shares its texture loading, 0x0004EA90): four name / texture pairs,
 * its stages 0-3 (+0x1C/+0x20 the water, +0x24/+0x28 its second layer, +0x2C/+0x30 the foam, +0x34/+0x38 a map over
 * the whole grid), then +0x3C its wave set (run 113). */
#define OCEAN_WATER         0x20u
#define OCEAN_SECOND        0x28u
#define OCEAN_FOAM          0x30u
#define OCEAN_MAP           0x38u
#define OCEAN_WAVE_SET      0x3Cu

uint32_t ngl_mesh_section_texture(uint32_t section)
{
    uint32_t material = word(section + 0x04);

    if (material && waves_class(section_class(section)))
        return material_texture(material, OCEAN_WATER);
    return material && section_uv_offset(section) ? material_texture(material, 0x1C) : 0;
}

/* The draws that set the frame nglSetTexture (0x0006A760) picks an animated list's texture by (0x009026FC): ShaderPCUV
 * through nglSetTextureAnimFrame (the draw's param, else the scene's frame); cdOcean_Draw (0x000505C0) and
 * cdWater_Draw (0x000511C0) the scene's frame. The other classes leave whatever frame was set last. */
int ngl_mesh_section_frame(uint32_t section, const struct ngl_mesh_draw *d, uint32_t *frame)
{
    uint32_t type = section_class(section);

    if (type == SHADER_PCUV_REGISTER) {
        *frame = d->texture_frame;
        return 1;
    }
    if (waves_class(type) || type == CDWATER_REGISTER) {
        *frame = scene_texture_frame(d->scene);
        return 1;
    }
    return 0;
}

uint32_t ngl_mesh_ocean_foam_texture(uint32_t section)
{
    uint32_t material = word(section + 0x04);

    return material && waves_class(section_class(section)) ? material_texture(material, OCEAN_FOAM) : 0;
}

int ngl_mesh_section_shading(uint32_t section, uint32_t *lightmap)
{
    const struct material_class *c = section_class_info(section);
    uint32_t material = word(section + 0x04);

    *lightmap = 0;
    if (!c || !material)
        return NGL_SHADE_LIGHT;
    if (c->shading != NGL_SHADE_LIGHTMAP)
        return c->shading;
    *lightmap = material_texture(material, 0x24);      /* nglMaterial_LoadBaseAndOptional's second texture */
    return *lightmap ? NGL_SHADE_LIGHTMAP : NGL_SHADE_LIGHT;
}

/* cdWorldBlend's vertex program (0x0089EA20) passes its three coordinate sets (12, 20, 28) to texture stages 0-2 and
 * the color (36) as v0; its pixel shader mixes stage 0 and 1 by v0's alpha and multiplies by stage 2 when the
 * material has a third texture (cdWorldBlend_LoadTextures leaves +0x2C 0 when it would be the default one). */
int ngl_mesh_section_blend(uint32_t section, uint32_t *second, uint32_t *third)
{
    const struct material_class *c = section_class_info(section);
    uint32_t material = word(section + 0x04);

    *second = *third = 0;
    if (!c || !c->third_uv || !material)
        return 0;
    if (waves_class(c->register_function)) {     /* see ngl_mesh_ocean_corners */
        *second = material_texture(material, OCEAN_SECOND);
        *third = material_texture(material, OCEAN_MAP);
        return 1;
    }
    *second = material_texture(material, 0x24);
    *third = material_texture(material, 0x2C);
    return 1;
}

/* A cdSky material's fields (run 111: Gela's twelve sections, Omaha's five): +0x20 is 0 for the backdrop (drawn
 * opaque) and 1 for every layer over it, blended by its texture's alpha (clouds, glows, the moon, the city's
 * silhouette: DXT1 ones by their one-bit alpha); +0x30 is the layer's place, the backdrop 0, drawn lowest first (Gela's
 * city goes over its fires; its node's sort key is 1000 - layer, 0x55330); +0x24 and +0x28 are the layer's drift
 * (ngl_mesh_drifted_corners). The blended layers stay behind the world: ngl_present puts the sky at the far plane. */
#define SKY_BLENDED 0x20u
#define SKY_LAYER   0x30u

static uint32_t sky_material(uint32_t section)
{
    const struct material_class *c = section_class_info(section);
    uint32_t material = c && c->register_function == CDSKY_REGISTER ? word(section + 0x04) : 0;

    return material && readable(material + SKY_LAYER) ? material : 0;
}

int ngl_mesh_section_alpha(uint32_t section)
{
    const struct material_class *c = section_class_info(section);
    uint32_t sky = sky_material(section);

    if (sky)
        return word(sky + SKY_BLENDED) ? NGL_ALPHA_BLEND : NGL_ALPHA_OPAQUE;
    return c ? c->alpha : NGL_ALPHA_OPAQUE;
}

/* cdSky's drift (its node's render, 0x54AD0): each frame a layer's texture coordinates move by the fractions of
 * +0x24 x time x 0.1 and +0x28 x time x 0.1, time being NGL's clock in seconds (0x00901E90, which 0x45640 advances by
 * each frame's time step), so the clouds move at the game's speed whatever the frame rate. */
#define SKY_DRIFT_U     0x24u
#define SKY_DRIFT_V     0x28u
#define SKY_DRIFT_SCALE 0.1f
#define NGL_TIME        0x00901E90u

uint32_t ngl_mesh_drifted_corners(uint32_t section, float *out, uint32_t max)
{
    uint32_t sky = sky_material(section), count;
    struct ngl_section_triangles t;
    float time, du, dv;

    if (!sky)
        return 0;
    time = real(NGL_TIME);
    {
        static unsigned long calls;

        if (calls++ % 2000 == 0 && calls < 8000)
            fprintf(stderr, "[mesh] sky drift: NGL time %g s, layer drift %g %g\n", time, real(sky + SKY_DRIFT_U),
                    real(sky + SKY_DRIFT_V));
    }
    du = real(sky + SKY_DRIFT_U) * time * SKY_DRIFT_SCALE;
    dv = real(sky + SKY_DRIFT_V) * time * SKY_DRIFT_SCALE;
    if (!isfinite(du) || !isfinite(dv) || fabsf(du) > 1.0e6f || fabsf(dv) > 1.0e6f || (du == 0.0f && dv == 0.0f))
        return 0;
    du -= (float)(int32_t)du;       /* the fraction, as the game keeps it (truncated toward zero) */
    dv -= (float)(int32_t)dv;
    if (ngl_mesh_section_triangles(section, &t) != NGL_SECTION_OK)
        return 0;
    count = t.corner_count <= max ? t.corner_count : 0;
    for (uint32_t i = 0; i < count; i++) {
        memcpy(&out[i * NGL_CORNER_FLOATS], &t.corners[i * NGL_CORNER_FLOATS], NGL_CORNER_FLOATS * sizeof(float));
        out[i * NGL_CORNER_FLOATS + 3] += du;
        out[i * NGL_CORNER_FLOATS + 4] += dv;
    }
    ngl_mesh_free_triangles(&t);
    return count;
}

unsigned ngl_mesh_section_layer(uint32_t section)
{
    uint32_t sky = sky_material(section);
    uint32_t layer = sky ? word(sky + SKY_LAYER) : 0;

    return layer < 256 ? layer : 255;
}

/* cdOcean's waves. Its vertex program (0x0089DA40, 60 instructions; nv2a_vsh.py --show) takes a corner's x and y (v0),
 * turns them by the node's matrix (c12, c13: its first two rows, without the position), adds four waves to the water's
 * level (c20.x) for the height, turns that back into the mesh's z (c18: the third row of the node's scaled inverse,
 * without its position), and makes stages 0-2's texture coordinates from the turned x and y (c21-c23: a scale, minus a
 * drift); stage 3 gets v1 as it is, and oD0's alpha the foam. cdOcean_SetWaveConstants (0x0004F810) makes those
 * constants each frame from the material's wave set, a 0xD0-byte entry of the table at 0x00901B30:
 *   +0x00 the level; +0x04 how much of the second layer shows (pixel c0.a); +0x08 the foam's scale;
 *   +0x10, +0x14 stage 0's drift and +0x20, +0x24 stages 1 and 2's (texture repeats a second, times NGL's clock);
 *   +0x30..+0x44 the three stages' tile sizes (x, y each; the scale is 1 / max(1, size));
 *   per wave i (0-3): +0x50 + 8i its origin (x, y), +0x70 + 4i its heading in degrees (the direction is (sin, cos)),
 *   +0x80 + 4i the distance over which it dies away, +0x90 + 4i its wavelength, +0xA0 + 4i its height,
 *   +0xB0 + 4i its phase and +0xC0 + 4i its speed (in turns and turns a second).
 * A wave's height at a corner: its height x (1 - min(1, d / max(1, distance))) x cos(d x 2 pi / max(1, wavelength) +
 * (phase + speed x time) x 2 pi), d being how far along its direction the corner is from its origin, either way (the
 * game's own cosine is a polynomial; the vertex program's c2 and c3). Time is NGL's clock (0x00901E90), which follows
 * the game's time, so the waves move at the game's speed whatever the frame rate. The pixel shader (0x0089E498, all
 * four stages) is stage 0 mixed with stage 1 by stage 1's alpha x c0.a, plus the foam, times stage 3: ngl_vk draws it
 * as a two-texture mix (by the second's alpha x the corners', c0.a: blend_by_alpha) times the third, fogged as the
 * final combiner does. The foam (stage 2, material +0x30) is added: its texture x its alpha x the color's alpha,
 * which the vertex program makes the crests' share (oD0.w = c32 . (cos_i x (1 - fade_i)), c32_i = +0x08 / 4 x
 * clamp(round(height_i), 0, 1); cdOcean_SetWaveConstants, 0x0004F810), at coordinates x / max(1, +0x40),
 * y / max(1, +0x44) less stage 1's drift (c23). ngl_vk draws it as a second, added draw (ngl_mesh_ocean_foam_texture),
 * times stage 3 at its own coordinates (v1, kept in the foam's corners where the normal goes, as for the water).
 * The editor's live copy of a wave set (0x00902124 set) isn't followed.
 * cdRiver's vertex program (0x0089CDB0) is the same 60 instructions but for four: stages 0-2 take the corner's own
 * coordinates (v1, at 8) less each stage's drift, not its turned x and y times a scale, and stage 3 takes v2 (at 16).
 * Its eight pixel shaders (0x0089D8F8) are cdOcean's, word for word (run 129). */
#define OCEAN_WAVE_SETS      0x00901B30u
#define OCEAN_WAVE_SET_BYTES 0xD0u
#define OCEAN_WAVE_SET_COUNT 4u         /* the editor's index is held to 0-3 */
#define OCEAN_EDITING        0x00902124u
#define OCEAN_TWO_PI         6.2831855f

struct ocean_constants {
    float level, second, foam;
    float scale[3][2], drift[3][2];     /* stages 0, 1 and the foam's */
    float crest[4];                     /* each wave's share of the foam (c32) */
    float origin[4][2], direction[4][2], fade[4], wavenumber[4], height[4], phase[4];
};

static float at_least_one(float f)
{
    return isfinite(f) && f > 1.0f ? f : 1.0f;
}

/* The fraction of x (x - floor(x)); 0 for a value that isn't a number or is too big to have one. */
static float fraction(float x)
{
    return isfinite(x) && fabsf(x) < 1.0e7f ? x - floorf(x) : 0.0f;
}

static int ocean_constants(uint32_t set, float time, struct ocean_constants *o)
{
    if (!readable(set) || !readable(set + OCEAN_WAVE_SET_BYTES - 1))
        return 0;
    o->level = real(set + 0x00);
    o->second = real(set + 0x04);
    o->foam = real(set + 0x08);
    for (int s = 0; s < 3; s++)
        for (int k = 0; k < 2; k++) {
            o->scale[s][k] = 1.0f / at_least_one(real(set + 0x30 + 8u * (uint32_t)s + 4u * (uint32_t)k));
            o->drift[s][k] = fraction(real(set + (s ? 0x20u : 0x10u) + 4u * (uint32_t)k) * time);
        }
    for (uint32_t i = 0; i < 4; i++) {
        float heading = real(set + 0x70 + 4 * i) * 0.017453292f;

        o->origin[i][0] = real(set + 0x50 + 8 * i);
        o->origin[i][1] = real(set + 0x54 + 8 * i);
        o->direction[i][0] = sinf(heading);
        o->direction[i][1] = cosf(heading);
        o->fade[i] = 1.0f / at_least_one(real(set + 0x80 + 4 * i));
        o->wavenumber[i] = OCEAN_TWO_PI / at_least_one(real(set + 0x90 + 4 * i));
        o->height[i] = real(set + 0xA0 + 4 * i);
        o->phase[i] = fraction(real(set + 0xB0 + 4 * i) + real(set + 0xC0 + 4 * i) * time) * OCEAN_TWO_PI;
        o->crest[i] = o->foam * 0.25f * fminf(fmaxf(roundf(real(set + 0xA0 + 4 * i)), 0.0f), 1.0f);
    }
    for (size_t i = 0; i < sizeof *o / sizeof(float); i++)
        if (!isfinite(((const float *)o)[i]))
            return 0;
    return 1;
}

int ngl_mesh_ocean_corners(uint32_t section, const float world[16], float *out, float *foam, uint32_t max)
{
    uint32_t material = word(section + 0x04), set, count;
    struct ngl_section_triangles t;
    struct ocean_constants o;
    float inverse[3], det;
    uint32_t alpha;

    int river = section_class(section) == CDRIVER_REGISTER;

    if (!waves_class(section_class(section)))
        return -1;
    set = readable(material + OCEAN_WAVE_SET) ? word(material + OCEAN_WAVE_SET) : OCEAN_WAVE_SET_COUNT;
    if (set >= OCEAN_WAVE_SET_COUNT)
        return 0;
    if (!ocean_constants(OCEAN_WAVE_SETS + set * OCEAN_WAVE_SET_BYTES, real(NGL_TIME), &o))
        return 0;
    /* the third row of the inverse of the node's turn and scale (rows 0-2 of world): z's share of x, y and z */
    det = world[0] * (world[5] * world[10] - world[6] * world[9]) -
          world[1] * (world[4] * world[10] - world[6] * world[8]) +
          world[2] * (world[4] * world[9] - world[5] * world[8]);
    if (!isfinite(det) || fabsf(det) < 1.0e-12f)
        return 0;
    inverse[0] = (world[4] * world[9] - world[5] * world[8]) / det;
    inverse[1] = -(world[0] * world[9] - world[1] * world[8]) / det;
    inverse[2] = (world[0] * world[5] - world[1] * world[4]) / det;
    if (ngl_mesh_section_triangles(section, &t) != NGL_SECTION_OK)
        return 0;
    count = t.corner_count <= max ? t.corner_count : 0;
    alpha = (uint32_t)(fminf(fmaxf(isfinite(o.second) ? o.second : 0.0f, 0.0f), 1.0f) * 255.0f + 0.5f);
    for (uint32_t i = 0; i < count; i++) {
        float *c = &out[i * NGL_CORNER_FLOATS];
        float x, y, u, v, height = o.level, crests = 0.0f;
        uint32_t bgra = alpha << 24 | 0x00FFFFFFu;

        memcpy(c, &t.corners[i * NGL_CORNER_FLOATS], NGL_CORNER_FLOATS * sizeof(float));
        u = c[3];       /* cdRiver's own coordinates (v1) */
        v = c[4];
        x = c[0] * world[0] + c[1] * world[1];  /* as the program turns v0: dot with the matrix's first rows */
        y = c[0] * world[4] + c[1] * world[5];
        for (int w = 0; w < 4; w++) {
            float d = fabsf((x - o.origin[w][0]) * o.direction[w][0] + (y - o.origin[w][1]) * o.direction[w][1]);
            float fade = fminf(fmaxf(d * o.fade[w], 0.0f), 1.0f);

            float wave = (1.0f - fade) * cosf(d * o.wavenumber[w] + o.phase[w]);

            height += o.height[w] * wave;
            crests += o.crest[w] * wave;
        }
        c[2] = x * inverse[0] + y * inverse[1] + height * inverse[2];
        c[3] = (river ? u : x * o.scale[0][0]) - o.drift[0][0];
        c[4] = (river ? v : y * o.scale[0][1]) - o.drift[0][1];
        c[5] = (river ? u : x * o.scale[1][0]) - o.drift[1][0];
        c[6] = (river ? v : y * o.scale[1][1]) - o.drift[1][1];
        memcpy(&c[7], &bgra, sizeof bgra);
        if (foam) {     /* the same corner with the foam's coordinates, and its share of the foam as the alpha */
            float *f = &foam[i * NGL_CORNER_FLOATS];
            uint32_t share = (uint32_t)(fminf(fmaxf(crests, 0.0f), 1.0f) * 255.0f + 0.5f) << 24 | 0x00FFFFFFu;

            memcpy(f, c, NGL_CORNER_FLOATS * sizeof(float));
            f[3] = (river ? u : x * o.scale[2][0]) - o.drift[2][0];
            f[4] = (river ? v : y * o.scale[2][1]) - o.drift[2][1];
            f[5] = f[6] = 0.0f;
            memcpy(&f[7], &share, sizeof share);
            f[10] = 0.0f;   /* stage 3's coordinates stay at 8 and 9, as the water's */
        }
    }
    {
        static uint32_t shown[4];
        static unsigned shown_count;
        unsigned s = 0;

        while (s < shown_count && shown[s] != section)
            s++;
        if (s == shown_count && shown_count < 4 && count) {
            shown[shown_count++] = section;
            fprintf(stderr, "[mesh] %s section 0x%08X: wave set %u (editing %u): level %g, second layer %g, foam %g "
                    "(tiles %g %g), tiles "
                    "%g %g; waves (height, wavelength, fade, heading) %g %g %g %g | %g %g %g %g | %g %g %g %g | %g %g "
                    "%g %g; first corner (%g %g %g) uv (%g %g), %u corners\n", river ? "river" : "ocean", section, set,
                    word(OCEAN_EDITING),
                    o.level, o.second, o.foam, 1.0f / o.scale[2][0], 1.0f / o.scale[2][1],
                    1.0f / o.scale[0][0], 1.0f / o.scale[0][1],
                    o.height[0], OCEAN_TWO_PI / o.wavenumber[0], 1.0f / o.fade[0], real(OCEAN_WAVE_SETS + set *
                    OCEAN_WAVE_SET_BYTES + 0x70), o.height[1], OCEAN_TWO_PI / o.wavenumber[1], 1.0f / o.fade[1],
                    real(OCEAN_WAVE_SETS + set * OCEAN_WAVE_SET_BYTES + 0x74), o.height[2], OCEAN_TWO_PI /
                    o.wavenumber[2], 1.0f / o.fade[2], real(OCEAN_WAVE_SETS + set * OCEAN_WAVE_SET_BYTES + 0x78),
                    o.height[3], OCEAN_TWO_PI / o.wavenumber[3], 1.0f / o.fade[3], real(OCEAN_WAVE_SETS + set *
                    OCEAN_WAVE_SET_BYTES + 0x7C), out[0], out[1], out[2], out[3], out[4], count);
        }
    }
    ngl_mesh_free_triangles(&t);
    return (int)count;
}

void ngl_mesh_report_untextured(uint32_t section)
{
    uint32_t material = word(section + 0x04), register_function = section_class(section);
    uint32_t name = material ? word(material + 0x18) : 0, texture = material ? word(material + 0x1C) : 0;
    const char *class_name = "unknown";
    char text[33] = "";

    for (size_t i = 0; i < sizeof classes / sizeof classes[0]; i++)
        if (classes[i].register_function == register_function)
            class_name = classes[i].name;
    for (uint32_t i = 0; i < 32 && readable(name + i); i++) {
        char c = (char)((word((name + i) & ~3u) >> (8 * ((name + i) & 3u))) & 255u);

        text[i] = c >= 32 && c <= 126 ? c : '.';
        text[i + 1] = 0;
    }
    fprintf(stderr, "[mesh] section 0x%08X without a texture: material 0x%08X, class %s (0x%08X), uv offset %u, "
            "texture (+0x1C) 0x%08X, name (+0x18) 0x%08X \"%s\"\n", section, material, class_name, register_function,
            section_uv_offset(section), texture, name, text);
}

static void add_triangle(uint16_t *out, uint32_t *n, uint32_t a, uint32_t b, uint32_t c)
{
    if (a == b || b == c || a == c)
        return;     /* a strip's joins */
    out[(*n)++] = (uint16_t)a;
    out[(*n)++] = (uint16_t)b;
    out[(*n)++] = (uint16_t)c;
}

/* A vertex's normal (a class's normal column) into out[3]; 0, 0, 0 when it has none or it isn't a usable one. */
static void read_normal(uint32_t at, uint32_t normal, float out[3])
{
    out[0] = out[1] = out[2] = 0.0f;
    if (normal == NORMAL_PACKED) {
        uint32_t n = word(at + 12);

        out[0] = (float)((int32_t)(n << 21) >> 21) / 1023.0f;
        out[1] = (float)((int32_t)(n << 10) >> 21) / 1023.0f;
        out[2] = (float)((int32_t)n >> 22) / 511.0f;
    } else if (normal) {
        for (int k = 0; k < 3; k++)
            out[k] = real(at + normal + 4u * (uint32_t)k);
    }
    for (int k = 0; k < 3; k++)
        if (!isfinite(out[k]) || fabsf(out[k]) > 2.0f)
            out[0] = out[1] = out[2] = 0.0f;
}

int ngl_mesh_section_triangles(uint32_t section, struct ngl_section_triangles *t)
{
    uint32_t prim = word(section + 0x28), index_count = word(section + 0x2C), index_list = word(section + 0x30);
    uint32_t corners = word(section + 0x40), data = word(section + 0x44), stride = word(section + 0x50);
    const struct material_class *c = section_class_info(section);
    uint32_t uv = c ? c->uv : 0, lightmap_uv = c ? c->lightmap_uv : 0, color = c ? c->color : 0, count, i, n = 0;
    uint32_t normal = c && (c->normal == NORMAL_PACKED ? stride >= 16 : c->normal + 12 <= stride) ? c->normal : 0;
    uint32_t third_uv = c ? c->third_uv : 0;
    uint16_t *index;

    memset(t, 0, sizeof *t);
    int floats = waves_class(section_class(section)) ? 2 : 3;  /* their z: ngl_mesh_ocean_corners */

    if (!corners || corners > MAX_SECTION_CORNERS || !data || stride < 12 || stride > 256 ||
        !readable(data + corners * stride - 1))
        return NGL_SECTION_NO_VERTICES;
    if (index_count && !readable(index_list + index_count * 2 - 1))
        return NGL_SECTION_NO_VERTICES;
    if (!index_count && word(section + 0x38))
        return NGL_SECTION_PUSH_BUFFER;
    if (prim < PRIM_TRIANGLE_LIST || prim > PRIM_QUAD_LIST)
        return NGL_SECTION_NOT_TRIANGLES;

    t->corners = malloc((size_t)corners * NGL_CORNER_FLOATS * sizeof(float));
    count = index_count ? index_count : corners;
    t->indices = malloc((size_t)(count < 3 ? 3 : count) * 3 * sizeof(uint16_t) * 2);
    index = malloc((size_t)count * sizeof(uint16_t));
    if (!t->corners || !t->indices || !index) {
        free(index);
        ngl_mesh_free_triangles(t);
        return NGL_SECTION_NO_VERTICES;
    }
    for (i = 0; i < corners; i++) {
        for (int k = 0; k < 3; k++) {
            float f = k < floats ? real(data + i * stride + 4u * (uint32_t)k) : 0.0f;

            if (!isfinite(f) || fabsf(f) > FAR_AWAY) {
                free(index);
                ngl_mesh_free_triangles(t);
                return NGL_SECTION_NOT_FLOATS;
            }
            t->corners[i * NGL_CORNER_FLOATS + (uint32_t)k] = f;
        }
        for (int k = 0; k < 2; k++) {
            float f = uv ? real(data + i * stride + uv + 4u * (uint32_t)k) : 0.0f;
            float g = lightmap_uv ? real(data + i * stride + lightmap_uv + 4u * (uint32_t)k) : 0.0f;

            t->corners[i * NGL_CORNER_FLOATS + 3 + (uint32_t)k] = isfinite(f) ? f : 0.0f;
            t->corners[i * NGL_CORNER_FLOATS + 5 + (uint32_t)k] = isfinite(g) ? g : 0.0f;
        }
        {
            uint32_t bgra = color ? word(data + i * stride + color) : 0xFFFFFFFFu;

            memcpy(&t->corners[i * NGL_CORNER_FLOATS + 7], &bgra, sizeof bgra);
        }
        if (third_uv) {
            for (int k = 0; k < 2; k++) {
                float f = real(data + i * stride + third_uv + 4u * (uint32_t)k);

                t->corners[i * NGL_CORNER_FLOATS + 8 + (uint32_t)k] = isfinite(f) ? f : 0.0f;
            }
            t->corners[i * NGL_CORNER_FLOATS + 10] = 0.0f;
        } else {
            read_normal(data + i * stride, normal, &t->corners[i * NGL_CORNER_FLOATS + 8]);
        }
    }
    t->corner_count = corners;
    for (i = 0; i < count; i++) {
        uint32_t v = index_count ? (uint32_t)(MEM32(index_list + (i & ~1u) * 2) >> ((i & 1u) * 16)) & 0xFFFFu : i;

        index[i] = (uint16_t)(v < corners ? v : 0);
    }
    switch (prim) {
    case PRIM_TRIANGLE_LIST:
        for (i = 0; i + 2 < count; i += 3)
            add_triangle(t->indices, &n, index[i], index[i + 1], index[i + 2]);
        break;
    case PRIM_TRIANGLE_STRIP:
        for (i = 0; i + 2 < count; i++)
            add_triangle(t->indices, &n, index[i], index[i + 1 + (i & 1u)], index[i + 2 - (i & 1u)]);
        break;
    case PRIM_TRIANGLE_FAN:
        for (i = 1; i + 1 < count; i++)
            add_triangle(t->indices, &n, index[0], index[i], index[i + 1]);
        break;
    case PRIM_QUAD_LIST:
        for (i = 0; i + 3 < count; i += 4) {
            add_triangle(t->indices, &n, index[i], index[i + 1], index[i + 2]);
            add_triangle(t->indices, &n, index[i], index[i + 2], index[i + 3]);
        }
        break;
    }
    free(index);
    t->index_count = n;
    if (!n) {
        ngl_mesh_free_triangles(t);
        return NGL_SECTION_NOT_TRIANGLES;
    }
    return NGL_SECTION_OK;
}

void ngl_mesh_free_triangles(struct ngl_section_triangles *t)
{
    free(t->corners);
    free(t->indices);
    memset(t, 0, sizeof *t);
}

/* ---- skinning ------------------------------------------------------------------------------------------------ */

unsigned ngl_mesh_section_bone_count(uint32_t section)
{
    uint32_t count = word(section + 0x08);

    if (section_class(section) != CDCHAR_REGISTER || word(section + 0x50) != CDCHAR_STRIDE)
        return 0;
    return count && count <= MAX_BONES && readable(word(section + 0x0C)) ? count : 0;
}

/* The inverse of an affine row-vector matrix (rotation and scale in the top 3x3, translation in row 3). */
static void invert_affine(const float m[16], float out[16])
{
    float a = m[0], b = m[1], c = m[2], d = m[4], e = m[5], f = m[6], g = m[8], h = m[9], i = m[10];
    float det = a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g);
    float r = det != 0.0f ? 1.0f / det : 0.0f;

    memset(out, 0, 16 * sizeof(float));
    out[0] = (e * i - f * h) * r;
    out[1] = (c * h - b * i) * r;
    out[2] = (b * f - c * e) * r;
    out[4] = (f * g - d * i) * r;
    out[5] = (a * i - c * g) * r;
    out[6] = (c * d - a * f) * r;
    out[8] = (d * h - e * g) * r;
    out[9] = (b * g - a * h) * r;
    out[10] = (a * e - b * d) * r;
    for (int k = 0; k < 3; k++)
        out[12 + k] = -(m[12] * out[0 + k] + m[13] * out[4 + k] + m[14] * out[8 + k]);
    out[15] = 1.0f;
}

/* Whether the four bytes at +28 are the weights (they add up to about 255 in the first vertices) or the ones at +24
 * are: the decoded vertex layout names them only v3 and v4. */
static int weights_at_28(uint32_t data, uint32_t corners)
{
    int sum28 = 0, sum24 = 0;

    for (uint32_t v = 0; v < corners && v < 16; v++) {
        uint32_t s24 = word(data + v * CDCHAR_STRIDE + 24), s28 = word(data + v * CDCHAR_STRIDE + 28), a = 0, b = 0;

        for (int k = 0; k < 4; k++) {
            a += (s24 >> (8 * k)) & 255u;
            b += (s28 >> (8 * k)) & 255u;
        }
        sum24 += a >= 250 && a <= 260;
        sum28 += b >= 250 && b <= 260;
    }
    return sum28 >= sum24;
}

/* [acct2-bro] 2026-11-08 BRO_SKIN_WATCH=1 (owner 10:12: "in BRO I also noticed the NPC randomly get wide or narrow"):
 * each skinned draw's bone palette and world matrix measured. A rigid bone's three rows are 1 long, so a palette row
 * well away from 1 stretches its corners along that axis. Every 600 frames a line: how many skinned draws had a
 * palette row below 0.9 or above 1.1, and how many a world row; the first 40 such draws with their mesh, section,
 * flags, bone count, the extremes and the frame. Off by default; changes nothing that is drawn. */
static int skin_watch_on(void)
{
    static int on = -1;

    if (on < 0) {
        const char *v = getenv("BRO_SKIN_WATCH");

        on = v && *v && *v != '0';
    }
    return on;
}

static void row_lengths(const float m[16], float *lo, float *hi)
{
    for (int r = 0; r < 3; r++) {
        float l = sqrtf(m[4 * r] * m[4 * r] + m[4 * r + 1] * m[4 * r + 1] + m[4 * r + 2] * m[4 * r + 2]);

        if (l < *lo)
            *lo = l;
        if (l > *hi)
            *hi = l;
    }
}

static void watch_skin(const struct ngl_mesh_draw *d, uint32_t section, const float palette[][16], uint32_t count,
                       uint32_t flags)
{
    static int shown;
    float lo = 1e9f, hi = 0.0f, world_lo = 1e9f, world_hi = 0.0f;
    int odd, world_odd;

    for (uint32_t i = 0; i < count; i++)
        row_lengths(palette[i], &lo, &hi);
    row_lengths(d->world, &world_lo, &world_hi);
    odd = lo < 0.9f || hi > 1.1f;
    world_odd = world_lo < 0.9f || world_hi > 1.1f;
    skin_draws++;
    skin_odd += odd;
    skin_world_odd += world_odd;
    skin_world_odd_shadow += world_odd && d->shadow_scene;
    if ((odd || world_odd) && shown < 40) {
        shown++;
        fprintf(stderr, "[skin] frame %lu: mesh 0x%08X section 0x%08X flags 0x%X, %u bones: palette rows %.3f-%.3f, "
                "world rows %.3f-%.3f%s\n", frames, d->mesh, section, flags, count, lo, hi, world_lo, world_hi,
                d->shadow_scene ? " (the shadow scene)" : "");
    }
}

uint32_t ngl_mesh_skinned_corners(const struct ngl_mesh_draw *d, uint32_t section, float *out, uint32_t max)
{
    static float palette[MAX_BONES][16];
    uint32_t count = ngl_mesh_section_bone_count(section), corners = word(section + 0x40), data = word(section + 0x44);
    uint32_t list = word(section + 0x0C), bind = word(d->mesh + 0x14), flags = d->skin_flags, weights, slots;
    float world_inverse[16], bone[16], bind_matrix[16], step[16];

    if (!count || !(flags & SKIN_FLAGS) || !readable(d->bones) || !corners || corners > max ||
        !readable(data + corners * CDCHAR_STRIDE - 1) || ((flags & (0x04u | 0x08u)) && !readable(bind)))
        return 0;
    if (flags & 0x04u)
        invert_affine(d->world, world_inverse);
    for (uint32_t i = 0; i < count; i++) {
        uint32_t number = (word(list + (i & ~1u) * 2) >> ((i & 1u) * 16)) & 0xFFFFu;

        read_matrix(d->bones + number * 64u, 0, bone);
        if (flags & (0x04u | 0x08u)) {
            read_matrix(bind + number * 64u, 1, bind_matrix);
            multiply(bind_matrix, bone, step);
            if (flags & 0x04u)
                multiply(step, world_inverse, palette[i]);
            else
                memcpy(palette[i], step, sizeof step);
        } else {
            memcpy(palette[i], bone, sizeof bone);
        }
    }
    if (skin_watch_on())
        watch_skin(d, section, (const float (*)[16])palette, count, flags);
    weights = weights_at_28(data, corners) ? 28u : 24u;
    slots = weights == 28u ? 24u : 28u;
    uint32_t unweighted = 0, first_unweighted = 0;
    for (uint32_t v = 0; v < corners; v++) {
        uint32_t at = data + v * CDCHAR_STRIDE, w = word(at + weights), s = word(at + slots);
        float p[3] = { real(at), real(at + 4), real(at + 8) }, sum[3] = { 0.0f, 0.0f, 0.0f }, total = 0.0f;
        float normal[3], turned[3] = { 0.0f, 0.0f, 0.0f };
        float *o = out + v * NGL_CORNER_FLOATS;

        read_normal(at, NORMAL_PACKED, normal);     /* cdChar's (NORMPACKED3 at +12) */
        for (int k = 0; k < 4; k++) {
            float weight = (float)((w >> (8 * k)) & 255u);
            uint32_t slot = (s >> (8 * k)) & 255u;
            const float *m;

            if (weight == 0.0f || slot >= count)
                continue;
            m = palette[slot];
            for (int c = 0; c < 3; c++) {
                sum[c] += weight * (p[0] * m[0 + c] + p[1] * m[4 + c] + p[2] * m[8 + c] + m[12 + c]);
                turned[c] += weight * (normal[0] * m[0 + c] + normal[1] * m[4 + c] + normal[2] * m[8 + c]);
            }
            total += weight;
        }
        /* A corner none of whose slots names one of the section's bones (or with no weight at all) would stay where
         * the bind pose has it, far from the posed mesh (the first-person sleeve was pulled into a spike): it goes with
         * the section's first bone instead. */
        if (total <= 0.0f) {
            const float *m = palette[0];

            if (!unweighted++)
                first_unweighted = v;
            for (int c = 0; c < 3; c++) {
                sum[c] = p[0] * m[0 + c] + p[1] * m[4 + c] + p[2] * m[8 + c] + m[12 + c];
                turned[c] = normal[0] * m[0 + c] + normal[1] * m[4 + c] + normal[2] * m[8 + c];
            }
            total = 1.0f;
        }
        for (int c = 0; c < 3; c++)
            o[c] = isfinite(sum[c]) ? sum[c] / total : p[c];
        o[3] = real(at + 16);       /* cdChar's base texture coordinates */
        o[4] = real(at + 20);
        if (!isfinite(o[3]) || !isfinite(o[4]))
            o[3] = o[4] = 0.0f;
        o[5] = o[6] = 0.0f;         /* no lightmap; the color white */
        {
            uint32_t white = 0xFFFFFFFFu;

            memcpy(&o[7], &white, sizeof white);
        }
        for (int c = 0; c < 3; c++)                 /* the shader normalizes */
            o[8 + c] = isfinite(turned[c]) ? turned[c] / total : normal[c];
    }
    {
        static int sections_shown;

        if (unweighted && sections_shown < 8) {
            uint32_t at = data + first_unweighted * CDCHAR_STRIDE;

            sections_shown++;
            fprintf(stderr, "[mesh] skin: section 0x%08X: %u of %u corners name none of its %u bones (weights at +%u); "
                    "corner %u: slots %08X weights %08X\n", section, unweighted, corners, count, weights,
                    first_unweighted, word(at + slots), word(at + weights));
        }
    }
    return corners;
}
