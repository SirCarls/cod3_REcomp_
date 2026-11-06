/* NGL on Vulkan: the presenter, the first piece of the renderer both ports share.
 *
 * It draws one 32-bit picture, scaled to fit and centered, into a picture of its own (the target), and keeps a copy
 * of the target where the host can read it. 2D quads and 3D meshes draw into the same target. With a window
 * (ngl_vk_set_window) each finished target is also shown in it. */
#pragma once
#include <stdint.h>

/* Starts Vulkan on the best GPU it finds, with a target of width x height. Returns 0 when it is ready. */
int ngl_vk_init(uint32_t width, uint32_t height);

/* A window to show the target in (Big Red One's own window). Called before ngl_vk_init: instance_extensions are the
 * instance extensions the window system needs (SDL_Vulkan_GetInstanceExtensions' list; the strings must outlive
 * ngl_vk_init), make_surface makes the window's surface once the instance exists (instance is a VkInstance and surface
 * points at a VkSurfaceKHR, as SDL_Vulkan_CreateSurface takes them; it returns 0 when it made one), and width x height
 * is the window's drawable size in pixels. ngl_vk_init then also makes a swapchain, and each draw shows the target in
 * the window, scaled to fit and centered on black, besides copying it out for the host as before. When the window
 * can't be used (no surface, no GPU that can show on it, no swapchain) ngl_vk says so and draws offscreen. present_mode
 * is how frames reach the screen: 0 in step with the display (FIFO, as the consoles did), 1 the newest frame at each
 * refresh without waiting (MAILBOX, when the GPU has it). */
typedef int (*ngl_vk_surface_maker)(void *instance, void *surface, void *user);
void ngl_vk_set_window(const char *const *instance_extensions, uint32_t extension_count,
                       ngl_vk_surface_maker make_surface, void *user, uint32_t width, uint32_t height,
                       int present_mode);

/* The window's drawable size changed (0 x 0 while it is minimized: nothing is shown then). */
void ngl_vk_window_resized(uint32_t width, uint32_t height);

/* 1 when draws are being shown in a window, 0 when they are offscreen. */
int ngl_vk_showing(void);

/* The GPU ngl_vk_init chose ("" before it). */
const char *ngl_vk_device_name(void);

/* Draws a picture of width x height pixels into the target, scaled to fit and centered on black. The pixels are
 * 4 bytes each, in the order blue, green, red, unused (Direct3D's X8R8G8B8), and each row starts pitch bytes after
 * the one before. Returns 0 when the drawing is done and the target can be read. */
int ngl_vk_draw_picture(const void *pixels, uint32_t width, uint32_t height, uint32_t pitch);

/* One corner of a 2D triangle; a quad is two triangles, six of these. */
struct ngl_vk_vertex {
    float x, y;         /* the game's screen pixels, (0, 0) top left */
    float u, v;         /* picture coordinates, 0..1 */
    uint32_t color;     /* D3DCOLOR, 0xAARRGGBB; the picture is multiplied by it */
    float textured;     /* 0: the color alone; 1: the picture, opaque, times the color; 2: with its alpha */
    uint32_t texture;   /* the texture (from ngl_vk_texture), or 0 for the picture given to ngl_vk_draw_quads */
};

/* A texture for quads to use: width x height pixels of 4 bytes, blue, green, red, alpha, rows packed. Returns its
 * number (1 and up), or 0 when it can't be made. */
uint32_t ngl_vk_texture(const void *pixels, uint32_t width, uint32_t height);

/* The same with the texture's own smaller levels: levels[i] is level i, max(1, width >> i) x max(1, height >> i)
 * pixels laid out the same way; level_count of them (stopping early at a NULL one), sampled with linear blending
 * between levels as the GPU does. ngl_vk_texture is this with one level. */
uint32_t ngl_vk_texture_levels(const void *const *levels, uint32_t level_count, uint32_t width, uint32_t height);

/* Draws count vertices (count / 3 triangles, alpha-blended in order) on black, the game's screen_width x
 * screen_height screen scaled to fit the target. Textured ones sample the picture given (as ngl_vk_draw_picture
 * takes it; NULL for none). Returns 0 when the drawing is done and the target can be read. */
int ngl_vk_draw_quads(const struct ngl_vk_vertex *vertices, uint32_t count, float screen_width, float screen_height,
                      const void *pixels, uint32_t width, uint32_t height, uint32_t pitch);

/* 3D: a mesh's triangles, made once and kept: corner_count corners (NGL_VK_CORNER_FLOATS floats each: the position
 * x, y, z in the mesh's own space; the base texture's u, v; the lightmap's u, v; a color, 4 bytes blue, green, red,
 * alpha (a D3DCOLOR) in the eighth float's place; the normal x, y, z in the mesh's own space, or 0, 0, 0 for none, when
 * the face's is used) and index_count 16-bit corner numbers, three a triangle. Returns its number (1 and up), or 0
 * when there is no room left. */
#define NGL_VK_CORNER_FLOATS 11
uint32_t ngl_vk_mesh(const float *corners, uint32_t corner_count, const uint16_t *indices, uint32_t index_count);

/* Corners for one draw of this frame only (as ngl_vk_mesh's: a skinned mesh's, moved by its bones),
 * drawn with a mesh's triangles through a draw's corners field. Returns that number (1 and up), or 0 when the frame's
 * room is used up. The room is given back after the frame's meshes are drawn. */
uint32_t ngl_vk_frame_corners(const float *corners, uint32_t corner_count);

/* How a mesh is lit (its draw's shading). */
enum {
    NGL_VK_SHADE_LIGHT,         /* one fixed light (the scene's lights come later) */
    NGL_VK_SHADE_LIGHTMAP,      /* its lightmap at the corners' second coordinates, doubled */
    NGL_VK_SHADE_VERTEX_COLOR,  /* its corners' colors, doubled */
    NGL_VK_SHADE_UNLIT,         /* its corners' colors as they are */
};

/* How a mesh's pixels use its texture's alpha. */
enum {
    NGL_VK_ALPHA_OPAQUE,        /* not at all */
    NGL_VK_ALPHA_TEST,          /* a pixel is drawn only where alpha is above one half */
    NGL_VK_ALPHA_BLEND,         /* blended over what's behind it (drawn after the others, without writing depth) */
};

/* One mesh to draw. The matrices are Direct3D's (row-vector, 16 floats row by row): a corner at p lands at
 * p * world * view_projection, in Direct3D's clip space (z 0..1). Drawn with its texture, or in its tint's color,
 * lit as its shading says. */
struct ngl_vk_mesh_draw {
    uint32_t mesh;              /* from ngl_vk_mesh */
    uint32_t tint;              /* the color it is drawn in when it has no texture (28 bits are used) */
    uint32_t texture;           /* from ngl_vk_texture, or 0: no texture, the tint's color */
    uint32_t lightmap;          /* from ngl_vk_texture, for NGL_VK_SHADE_LIGHTMAP */
    uint32_t shading;           /* NGL_VK_SHADE_* */
    uint32_t alpha;             /* NGL_VK_ALPHA_* */
    uint32_t corners;           /* from ngl_vk_frame_corners, or 0: the mesh's own corners */
    float world[16];
    float view_projection[16];
    float fog[3];               /* start and end (distances from the eye) and the fog's amount at the end; end <= start:
                                 * no fog */
    uint32_t fog_color;         /* the fog's color, red, green, blue bytes, then the amount before the start (a byte) */
    /* NGL_VK_SHADE_LIGHT: when lit is 1, the scene's light for this mesh: the ambient color, plus one directional
     * light (light_direction is the way it travels, in world space; its color 0 for none, 0 to 4) and up to three
     * more (extra_light_count, below). When lit is 0, one fixed light from above. */
    uint32_t lit;
    float ambient[3];
    float light_direction[3];
    float light_color[3];
    /* Two textures mixed (Big Red One's cdWorldBlend): when blend_texture is set, the base texture and it are mixed by
     * the corners' alpha (it at the lightmap's coordinates), then multiplied by blend_light when that is set (at the
     * coordinates in the normal's place: its x and y), else not lit; shading and the normals aren't used then. Both
     * from ngl_vk_texture; 0 for none. blend_light without blend_texture (and not shiny): the draw is lit as its
     * shading says, then multiplied by it the same way (Big Red One's cdOcean foam, times the map over the sea). */
    uint32_t blend_texture;
    uint32_t blend_light;
    /* 1: NGL_VK_SHADE_LIGHTMAP's lightmap and NGL_VK_SHADE_VERTEX_COLOR's colors multiply once, as Big Red One's
     * pixel shaders do (cdWorld's 0x008A0280: t0 x t1); 0: doubled. */
    uint32_t light_once;
    /* For effects drawn as quads (Big Red One's particles): how many of the mesh's indices to draw, from its first
     * (0: all of them), and how an NGL_VK_ALPHA_BLEND draw goes onto what's behind it (NGL_VK_BLEND_*). */
    uint32_t index_count;
    uint32_t blend;
    /* A shiny surface (Call of Duty 3's guns): env_texture, an environment map, looked up as a sphere map by the
     * surface's normal turned toward the eye (0.5 + 0.5 x its x and y), times env_mask's color at the base texture's
     * coordinates (or as it is without one), and added to the lit color. Not with two textures mixed or a lightmap.
     * Both from ngl_vk_texture; 0 for none. */
    uint32_t env_texture;
    uint32_t env_mask;
    /* 1: two textures mixed by blend_texture's alpha times the corners' (Big Red One's cdOcean: its second layer by
     * its own alpha x the share the sea shows); 0: by the corners' alpha alone. */
    uint32_t blend_by_alpha;
    /* NGL_VK_SHADE_LIGHT with lit 1: up to three more directional lights beside the first (Call of Duty 3's vertex
     * shaders light by four: the normal against each one's direction, at least 0, times its color, plus the
     * ambient), each the way it travels in world space and its color (0 to 4); extra_light_count of them, 0 for none.
     * light_saturate 1: the ambient and the lights together are held to 1 before the texture, as those shaders'
     * saturated vertex color is; 0: not held. */
    uint32_t extra_light_count;
    float extra_light_direction[3][3];
    float extra_light_color[3][3];
    uint32_t light_saturate;
    /* The sun's shadow (ngl_vk_set_sun_shadow; Call of Duty 3's sun cascades). shadow_caster 1: drawn into the shadow
     * map too (depth only, alpha-tested as it is drawn; not when blended). sun_shadowed 1: its first light (or the
     * fixed light) is dimmed where the shadow map has something nearer the sun; its view_projection must be the
     * camera's given to ngl_vk_set_sun_shadow. Both 0: as before. */
    uint32_t shadow_caster;
    uint32_t sun_shadowed;
};

/* How a blended mesh draw goes onto what's behind it. */
enum {
    NGL_VK_BLEND_ALPHA,         /* by its alpha: color x alpha + behind x (1 - alpha) */
    NGL_VK_BLEND_ADD,           /* added: behind + color x alpha */
    NGL_VK_BLEND_SUBTRACT,      /* taken away: behind - color x alpha */
};

/* The meshes the next ngl_vk_draw_quads draws first, depth-tested, under the quads (the list is copied; at most 16384
 * a frame). */
void ngl_vk_set_meshes(const struct ngl_vk_mesh_draw *draws, uint32_t count);

/* The sun's shadow for the next ngl_vk_draw_quads (one frame): light_view_projection takes world space to the light's
 * clip space (Direct3D row vectors, orthographic: x and y -1 to 1 across the map, z 0 nearest the sun to 1), and
 * camera_view_projection is the view_projection of the draws marked sun_shadowed. The shadow casters are drawn into a
 * 2048 x 2048 depth map first. Null for no shadow this frame (sun_shadowed draws are then lit as before). */
void ngl_vk_set_sun_shadow(const float light_view_projection[16], const float camera_view_projection[16]);

/* The grass (Call of Duty 3's GrassRenderNode), drawn by the game's own grass shaders translated: one strip of blades
 * shared by every patch, made once: vertex_count vertices of NGL_VK_GRASS_VERTEX_BYTES (two 16-bit signed numbers,
 * the blade's root on its patch, then two sets of four 8-bit signed ones, its shape and its variety; little-endian)
 * and index_count 16-bit indices, 0xFFFF between blades. Returns 0 when it is ready (not when ngl_vk has no grass
 * pipeline, or it was made already). */
#define NGL_VK_GRASS_VERTEX_BYTES 12
int ngl_vk_grass_mesh(const void *vertices, uint32_t vertex_count, const uint16_t *indices, uint32_t index_count);

/* One patch of grass: its cell's two textures (from ngl_vk_texture: the color texture, its alpha the density, and
 * the cell texture, its alpha the height), how many of the strip's indices to draw (its level of detail; 0: all),
 * and its two vectors (the game's c84: the place in the cell's textures and in the world; c85: the cell's lowest
 * height and its height range). */
struct ngl_vk_grass_patch {
    uint32_t color_texture, cell_texture;
    uint32_t index_count;
    float place[8];
};

/* The next draw's grass, drawn after the opaque meshes: the game's vertex shader constants c0..c95
 * (NGL_VK_GRASS_CONSTANTS x 4 floats; c12..c15 the view-projection as Direct3D's columns, z 0..1), bend (the game's
 * b0: blades pushed away from c42), the blade texture (from ngl_vk_texture) and the patches (at most 1024; copied). */
#define NGL_VK_GRASS_CONSTANTS 96
void ngl_vk_set_grass(const float *constants, uint32_t bend, uint32_t blade_texture,
                      const struct ngl_vk_grass_patch *patches, uint32_t count);

/* The target as the last draw left it: width x height pixels of 4 bytes, red, green, blue, alpha. NULL before the
 * first draw. */
const uint8_t *ngl_vk_target_pixels(uint32_t *width, uint32_t *height);

/* Frees everything ngl_vk_init made. */
void ngl_vk_shutdown(void);
