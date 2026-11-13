/* NGL on Vulkan: the presenter, the first piece of the renderer both ports share.
 *
 * It draws one 32-bit picture, scaled to fit and centered, into a picture of its own (the target), and keeps a copy
 * of the target where the host can read it. 2D quads and 3D meshes draw into the same target. With a window
 * (ngl_vk_set_window) each finished target is also shown in it. */
#pragma once
#include <stdint.h>

/* Starts Vulkan on the best GPU it finds, with a target of width x height. Returns 0 when it is ready. */
int ngl_vk_init(uint32_t width, uint32_t height);

/* [rexglue-native] 2026-11-09: ngl_vk doesn't link the Vulkan loader; it opens the system's when it starts
 * (libvulkan.so.1 on Linux, vulkan-1.dll on Windows). This opens it if it isn't open yet and returns its file name, or
 * NULL when there is none (ngl_vk_init and ngl_vk_init_on_device then fail; the reason is on stderr). */
const char *ngl_vk_loader(void);

/* Instead of ngl_vk_init: draws on a Vulkan device someone else made (Call of Duty 3's, the SDK presenter's), so the
 * target can reach their window on the GPU (ngl_vk_output) rather than through the host. instance, gpu and device are
 * the VkInstance, VkPhysicalDevice and VkDevice, made through the system's Vulkan loader, the one ngl_vk opens
 * ([rexglue-native] 2026-11-09: ngl_vk finds their functions through it); ngl_vk submits to
 * queue queue_index of queue_family. That queue is shared, so ngl_vk holds it around each use: lock(user) before,
 * which returns the VkQueue, and unlock(user) after, on the same thread. No window (ngl_vk_set_window is not used),
 * and ngl_vk_shutdown leaves the device and the instance to their owner. Returns 0 when it is ready. */
typedef void *(*ngl_vk_queue_lock)(void *user);
typedef void (*ngl_vk_queue_unlock)(void *user);
int ngl_vk_init_on_device(uint32_t width, uint32_t height, void *instance, void *gpu, void *device,
                          uint32_t queue_family, uint32_t queue_index, ngl_vk_queue_lock lock,
                          ngl_vk_queue_unlock unlock, void *user);

/* Whether each draw also copies the target out for the host (ngl_vk_target_pixels): 1, the default, or 0, when the
 * target only goes on through ngl_vk_output. */
void ngl_vk_set_readback(int on);

/* With ngl_vk_init_on_device: draws the last draw's target over the whole of another image of the device, through a
 * color table, and submits that on the shared queue after the draw (ngl_vk doesn't wait for it). image and view are a
 * VkImage and its VkImageView as 64-bit values: a 2D color image of one level and layer, usable as a color attachment,
 * of format format (a VkFormat), width x height. What it held is dropped: it is taken from VK_IMAGE_LAYOUT_UNDEFINED
 * once the stages after_stages (VkPipelineStageFlags; 0 when it has never been used) are done with it, and left in
 * final_layout (a VkImageLayout) for the stages ready_stages and the accesses ready_access. table is 256 words, red in
 * the low byte, then green and blue: a channel's value v becomes that channel of table[v] (NULL: kept as it is).
 * Returns 0 when it was submitted. */
int ngl_vk_output(uint64_t image, uint64_t view, uint32_t format, uint32_t width, uint32_t height,
                  uint32_t after_stages, uint32_t final_layout, uint32_t ready_stages, uint32_t ready_access,
                  const uint32_t *table);

/* A window to show the target in (Big Red One's own window). Called before ngl_vk_init: instance_extensions are the
 * instance extensions the window system needs (SDL_Vulkan_GetInstanceExtensions' list; the strings must outlive
 * ngl_vk_init), make_surface makes the window's surface once the instance exists (instance is a VkInstance and surface
 * points at a VkSurfaceKHR, as SDL_Vulkan_CreateSurface takes them; it returns 0 when it made one), and width x height
 * is the window's drawable size in pixels. ngl_vk_init then also makes a swapchain, and each draw shows the target in
 * the window, scaled to fit and centered on black, besides copying it out for the host as before. When the window
 * can't be used (no surface, no GPU that can show on it, no swapchain) ngl_vk says so and draws offscreen. present_mode
 * is how frames reach the screen: 0 in step with the display (FIFO, as the consoles did), 1 the newest frame at each
 * refresh without waiting (MAILBOX, when the GPU has it; side221: else IMMEDIATE, which may tear). */
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

/* [rexglue-native-render] 2026-11-11 rn48l: added to a corner's textured (0, 1 or 2 + this, the same on all six
 * corners of a quad): the same picture and color, blended additively (source x source alpha + what is there, the
 * target's alpha kept: Direct3D's SRCALPHA / ONE, as Call of Duty 3 draws its quads with +0x5C 01C00000). */
#define NGL_VK_QUAD_ADD 4.0f

/* A texture for quads to use: width x height pixels of 4 bytes, blue, green, red, alpha, rows packed. Returns its
 * number (1 and up), or 0 when it can't be made. */
uint32_t ngl_vk_texture(const void *pixels, uint32_t width, uint32_t height);

/* The same with the texture's own smaller levels: levels[i] is level i, max(1, width >> i) x max(1, height >> i)
 * pixels laid out the same way; level_count of them (stopping early at a NULL one), sampled with linear blending
 * between levels as the GPU does. ngl_vk_texture is this with one level. */
uint32_t ngl_vk_texture_levels(const void *const *levels, uint32_t level_count, uint32_t width, uint32_t height);

/* [waw-decomp] 2026-11-10: new pixels for a texture made by ngl_vk_texture (one level: not one made with smaller
 * levels, and not a cube). width x height must be the size it was made with, the pixels laid out the same way. For a
 * picture the game writes anew as it runs (Call of Duty 2 writes its water's height map each frame). Draws from the
 * next ngl_vk_draw_quads on see the new pixels. On the thread that draws. 0 when done, -1 when it can't be. */
int ngl_vk_texture_rewrite(uint32_t texture, const void *pixels, uint32_t width, uint32_t height);

/* [rexglue-native] 2026-11-09 rn34: a cube texture (for ngl_vk_mesh_draw's metal_cube only): six square faces of
 * size x size pixels laid out as ngl_vk_texture's, in Direct3D's order (+x, -x, +y, -y, +z, -z), each with its smaller
 * levels: faces[6 x level + face] is that face's level, max(1, size >> level) on a side; level_count levels (stopping
 * early at a NULL one). Sampled by a direction as Direct3D samples a cube. Returns its number (as ngl_vk_texture's; it
 * isn't a 2D texture, so only metal_cube takes it), or 0 when it can't be made. */
uint32_t ngl_vk_texture_cube(const void *const *faces, uint32_t level_count, uint32_t size);

/* Forgets every texture and mesh made so far and frees their memory (a new level, or before ngl_vk runs out of
 * room). Numbers are given out again from 1, so every number kept from before is no longer valid. It waits for the
 * last frame first. Call it between frames, on the thread that draws. [acct2-bro 2026-11-08] */
void ngl_vk_forget(void);

/* How many textures and mesh corners are in use, and how many there's room for. Any pointer may be NULL. */
void ngl_vk_room(uint32_t *textures, uint32_t *texture_room, uint32_t *corners, uint32_t *corner_room);

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

/* [rexglue-native] 2026-11-10 rn41q: a textured NGL_VK_SHADE_UNLIT draw's tint (its color is not used then) can carry
 * one of Call of Duty 3's unlit effects (the lanterns' glass, effect standard_7_1, and their flames, standard_10):
 * NGL_VK_TINT_EFFECT marks it; with NGL_VK_TINT_MAP_ALPHA its alpha is the texture's alone (not times the corners');
 * its low 4 bits are how many frames the texture holds side by side (0 or 1: it is one picture) and bits 4-7 the one
 * shown: read at u = (fract(u) + that frame) / frames, v = fract(v). */
enum {
    NGL_VK_TINT_EFFECT = 0x04000000,
    NGL_VK_TINT_MAP_ALPHA = 0x02000000,
    /* [rexglue-native] 2026-11-10 rn41x: a textured NGL_VK_SHADE_LIGHT draw with lit 1 (Call of Duty 3's SpeedTree
     * leaves, vertex shader 51732ACF1B6A87C5): its directional lights are times its corners' red (0 to 1, the leaf's
     * dimming) and not dimmed by the sun's shadow (the leaf pixel shader 6189945E43D7FBC8 reads none). */
    NGL_VK_TINT_LIGHT_DIMMED = 0x01000000,
    /* [rexglue-native] 2026-11-10 rn46c: a textured NGL_VK_SHADE_LIGHT draw with lit 1 and a lightmap texture set
     * (Call of Duty 3's SpeedTree branches and fronds, pixel shaders EA6B670C8ACDCED1 and F38DD169E44123DB): its
     * directional lights are times that texture's color at the corners' lightmap coordinates (the tree's
     * self-shadow map) and not dimmed by the sun's shadow; the ambient is not dimmed by it. Not with a normal map,
     * an environment map or two textures mixed (the lightmap's place is taken then).
     * [rexglue-native] 2026-11-10 21:40: moved from 0x00800000 to 0x02000000. Bit 23 is the top bit of the tint
     * color's red, so a textured lit draw that left a bright color in its tint was taken for a self-shadowed tree
     * (waw-decomp's 21:10 report: Call of Duty 2 handed 0x808080 with every draw). Now outside the color's 24 bits,
     * as the other flags are. The same number as NGL_VK_TINT_MAP_ALPHA: that one is read on textured
     * NGL_VK_SHADE_UNLIT draws with NGL_VK_TINT_EFFECT only, this one on NGL_VK_SHADE_LIGHT draws only. */
    NGL_VK_TINT_LIGHT_MAPPED = 0x02000000,
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
                                 * no fog. [waw-decomp] 2026-11-10: a start below 0 asks for exponential fog instead
                                 * (Call of Duty 2's): the amount at the end x (1 - 2^(start x the distance)). */
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
    /* 1 (a blended draw; Call of Duty 3's particles): faded where it nears the opaque meshes behind it, its alpha times
     * saturate((their depth - its depth) x scale), each corner's scale in its lightmap u's place (1 / the depth a
     * soft distance farther on, less its own: the game's soft particles). 0: as before. */
    uint32_t soft_depth;
    /* Call of Duty 3's per-pixel lit models (the gun and arms: pixel shader CDBAC6026B525050), NGL_VK_SHADE_LIGHT
     * with lit 1 only, not shiny: normal_map bends the normal the ambient and the lights see (a DXT5 normal map: x
     * and y in alpha and green, 2t - 1, at the base texture's coordinates; its frame from the screen slopes of the
     * position and those coordinates); specular_map adds a highlight from the first light (power 30, toward the
     * eye) times its color. Both from ngl_vk_texture; 0 for none. */
    uint32_t normal_map;
    uint32_t specular_map;
    /* NGL_VK_SHADE_LIGHTMAP with lit 1, Call of Duty 3's lightmapped world (its pixel shaders 141294CDC7C97127,
     * E72CD0D5E1523920 and their kin): when lightmap_scale is above 0, the light is
     *     lightmap_scale x (the lightmap's color x (1 + lightmap_up x the normal's up part, world z)
     *                       + the first light's color x the lightmap's alpha x the normal against that light)
     * instead of the lightmap doubled (or once): the first light is the sun (its color is the game's
     * COD_SHADOW_PROJCOLOR), and the lightmap's alpha is how much of it reaches there, times the sun's shadow map
     * when sun_shadowed is 1 (the shaders with COD_SHADOW_DEPTHMAP). lightmap_scale is the lightmap's fetch exponent
     * as a factor (1: none). The ambient and extra lights aren't used. 0: as before. */
    float lightmap_scale;
    float lightmap_up;
    /* With lightmap_scale above 0, or with lit 1: up to two nearby point lights (the game's LIGHT_POSITION1/2,
     * LIGHT_COLOR1/2 and LIGHT_INVFAR1/2), each adding its color x saturate(1 - its distance x inverse range) x the
     * normal against it to the light (before lightmap_scale): x, y, z its place in world space (as world maps the
     * mesh), its inverse range (1 / the distance it reaches), then red, green, blue (0 up: a fire is about 10) and an
     * unused float. point_light_count 0: none. */
    uint32_t point_light_count;
    float point_light[2][8];
    /* With lit 1: Call of Duty 3's per-pixel lit models' light, as the game hands each of their draws (its lighting,
     * 0x82142360; their pixel shaders' IrradianceCoeffs_0..8): nine rows of red, green and blue against xx, yy, zz,
     * xy, yz, zx, x, y and z of the normal, in the mesh's own space (the caller turns them), taken in place of the
     * ambient when irradiance_rows is 9 (0: none). The rows hold the set's ambient and its other lights, so such a
     * draw takes no extra lights. */
    uint32_t irradiance_rows;
    float irradiance[9][3];
    /* 1: its alpha is its corners' alpha alone, not its base texture's (Call of Duty 3's lightmapped world's second
     * layer, DiffuseMapB: drawn again over the first, blended by the weight each corner carries, lit the same way, so
     * the two come out as the game's shader mixes them). 0: as before. */
    uint32_t corner_alpha;
    /* [rexglue-native] 2026-11-09: split screen (Call of Duty 3's 2 to 4 local players). When scissor_width is above
     * 0, the draw only touches the target's pixels from scissor_x, scissor_y (its top-left corner, as the picture is
     * shown) to scissor_x + scissor_width, scissor_y + scissor_height: each player's view (its view_projection already
     * puts it in its own part of the screen) stays in its part. 0: the whole target, as before. */
    int32_t scissor_x, scissor_y;
    uint32_t scissor_width, scissor_height;
    /* [rexglue-native] 2026-11-09: Call of Duty 3's uniforms (the soldiers' bodies and some heads: the game's fabric
     * pixel shader 3BA30872B3955C23). With normal_map and specular_map set and lit 1, and both of these set (from
     * ngl_vk_texture; 0: none, drawn as before):
     *   - fabric_mask (the material's Fabric_Mask) and normal_swatch (its Normal_Swatch, a woven detail normal map, DXT5
     *     like the normal map). The swatch is read at (the coordinates - 0.5) x fabric_swatch_repeat + 0.5, combined
     *     with the normal map's normal (n.x s.z + s.x n.z, n.y s.z + s.y n.z, n.z s.z), and blended into it per
     *     channel by the mask's red, green and blue;
     *   - the base texture becomes fabric_diffuse_scale x a mix of it and pow(it, fabric_sheen) (red, green, blue), by
     *     how grazing the eye's view of the surface is (t = saturate(1 - normal . toward the eye), t^2 (3 - 2t));
     *   - the specular map takes highlights of power fabric_specular_power from the first light (times the sun's
     *     shadow) and from the nearby point lights (times their falloff), each toward the eye.
     * The numbers are the shader's own literals: 28.717, 0.51, (0.50694, 0.583088, 0.71) and 30. Needs a device with
     * six descriptor sets (every desktop driver has eight or more); with fewer, these draws stay bumped as before. */
    uint32_t fabric_mask;
    uint32_t normal_swatch;
    float fabric_swatch_repeat;
    float fabric_diffuse_scale;
    float fabric_sheen[3];
    float fabric_specular_power;
    /* [rexglue-native] 2026-11-09 rn30: the soldiers' own uniforms (third person) are drawn by another pixel shader,
     * 21168F53BCF9D156 (3BA30872B3955C23 draws only the view model's sleeves). With fabric_shadow_floor above 0, the
     * first light's color becomes max(mix(its color x the sun's shadow, 1, saturate(fabric_shadow_fade x the view
     * depth)), fabric_shadow_floor), for its light and its highlights: the game's COD_SHADOW_GREYLEVEL (0.4 in The Mace)
     * and COD_SHADOW_FARZ (0.001). 0: as 3BA30872B3955C23 (memset draws, Big Red One's, stay as before). */
    float fabric_shadow_floor;
    float fabric_shadow_fade;
    /* [rexglue-native] 2026-11-09 rn32: Call of Duty 3's soldiers' skin (the heads: the game's pixel shader
     * 3949D29E09A78AA1). With normal_map and specular_map set and lit 1, noise_swatch set (the material's Noise_Swatch,
     * from ngl_vk_texture) and fabric_mask 0:
     *   - the normal map bends the normal as for any bumped draw (no swatch is folded in);
     *   - the base texture becomes it x mix(skin_edge_lift, 1, s), s = t^2 (3 - 2t), t = saturate(normal . toward the
     *     eye): skin_edge_lift times it where the surface is seen edge-on, itself where it faces the eye;
     *   - the highlights (power fabric_specular_power, from the first light and the nearby point lights) are taken by
     *     mix(skin_specular_scale x the specular map, the noise swatch, skin_noise_mix), the swatch read at (the
     *     coordinates - 0.5) x fabric_swatch_repeat + 0.5;
     *   - the first light as fabric_shadow_floor and fabric_shadow_fade say (the same as the soldiers' uniforms).
     * The shader's own literals: 1.215, 0.4, 0.38, 11.018 and 68.528. 0 (memset draws): as before.
     * rn40t: the soldiers' helmets (the game's 1D4725CEF0551EBB, the same form without a noise swatch) are drawn this
     * way with 1.737, 2.38486 and 13.662, their specular map in noise_swatch and skin_noise_mix 0 (it adds nothing). */
    uint32_t noise_swatch;
    float skin_edge_lift;
    float skin_specular_scale;
    float skin_noise_mix;
    /* [rexglue-native] 2026-11-10 rn40x: and with skin_swatch set (a woven Normal_Swatch, DXT5 like the normal map, from
     * ngl_vk_texture) and skin_swatch_repeat above 0, the swatch read at (the coordinates - 0.5) x skin_swatch_repeat +
     * 0.5 is folded into the normal map whole (as the uniforms' fabric_mask would with a mask of 1): the game's
     * ADC02DCE4FDC9253 (helmets and bodies with a woven swatch; 1.737, 3.641, 0.345, 23.783, 28.717, 13.662). 0 (memset
     * draws): no swatch. */
    uint32_t skin_swatch;
    float skin_swatch_repeat;
    /* [rexglue-native] 2026-11-10 rn40y: and with skin_terminator_scale above 0, the first light and the nearby point
     * lights are each also taken by s = t^2 (3 - 2t), t = saturate(skin_terminator_scale x (the vertex normal, not the
     * normal map's, . toward the light + skin_terminator_bias)): they fade out where the surface itself turns from
     * the light. The game's 2340DF59702A9AD4 (the Polish infantry's heads drawn without a specular map; 0.2 and 5,
     * its c254.y and c254.w). 0 (memset draws): none. */
    float skin_terminator_bias;
    float skin_terminator_scale;
    /* [rexglue-native] 2026-11-09 rn34: Call of Duty 3's gun metal (the Springfield, the BREN: the game's pixel shader
     * CFC45995D1294CCE). With normal_map and specular_map set and lit 1, metal_cube set (an environment cube, from
     * ngl_vk_texture_cube), and fabric_mask and noise_swatch 0:
     *   - the normal map bends the normal as for any bumped draw;
     *   - the base texture becomes metal_hue (red, green, blue) x it, and the light (the irradiance rows, the first light
     *     x the sun's shadow x N.L, the nearby point lights x their falloff x N.L) times metal_brightness;
     *   - the highlights: metal_brightness x (the first light x the shadow, and the nearby point lights x their falloff)
     *     x saturate(N.H)^metal_power, toward the eye's own place, without N.L; plus metal_reflection x the cube at the
     *     eye's reflection about the normal in world space, read as the game reads it: at (x, z, y) of that reflection
     *     plus metal_cube_bias on each part;
     *   - those times a mask: the specular map + metal_scratch's color (0 for none) + mix(the base texture as it was,
     *     metal_rim, s), s = t^2 (3 - 2t), t = saturate(metal_rim_scale x (metal_rim_edge - N.V)).
     * Out: the base x the light + the highlights x the mask. The material's numbers are the game's texture_brightness,
     * Reflection_Amt, SpecularPower and Metal_Hue (its c29-c32); the rest the shader's literals (0.337, 3.546, (0.294,
     * 0.316, 0.412)). Needs a device that binds seven descriptor sets; with fewer, drawn bumped as before. 0 (memset
     * draws): as before. */
    uint32_t metal_cube;
    uint32_t metal_scratch;
    float metal_hue[3];
    float metal_brightness;
    float metal_reflection;
    float metal_power;
    float metal_rim[3];
    float metal_rim_edge;
    float metal_rim_scale;
    float metal_cube_bias;
    /* [rexglue-native] 2026-11-09 rn36: Call of Duty 3's detail-mapped vehicle metal (The Mace's tank body: the game's
     * pixel shaders C90DF51FF5AAF4F8 in the sun's shadow range and 8E7F713B9E3C1FAD beyond it). With normal_map and
     * specular_map set and lit 1, detail_map set (the material's Detail map, a DXT5 normal map like the normal map, from
     * ngl_vk_texture), and fabric_mask, noise_swatch and metal_cube 0:
     *   - the detail map is read at the coordinates x detail_repeat and folded into the normal map's normal everywhere:
     *     (n.x d' + d.x n', n.y d' + d.y n', n.z d.z), n' = n.z x detail_normal_scale, d' = d.z x detail_scale;
     *   - highlights of power detail_specular_power from the first light and the nearby point lights, toward the eye's
     *     own place, without N.L, times the specular map;
     *   - the first light as fabric_shadow_floor and fabric_shadow_fade say (0 for both: as it is).
     * The numbers are the material's (the shaders' c31-c34 / c27-c30). Needs a device that binds six descriptor sets;
     * with fewer, drawn bumped as before. 0 (memset draws): as before. */
    uint32_t detail_map;
    float detail_repeat;
    float detail_scale;
    float detail_normal_scale;
    float detail_specular_power;
    /* [rexglue-native] 2026-11-10 rn39: Call of Duty 3's bumped vehicle parts with a specular map (The Mace's tank
     * treads and wheels: the game's pixel shaders B2E0DA0062DFF88D and 9A3D3175A6CAFE56, the treads, and
     * 7D4E7CAD9503762D, the wheels; beyond the sun's shadow range BA59B7419D390DB8 / E470EE60FDF3D47D and the treads'
     * unshadowed pass). With normal_map and specular_map set and lit 1, vehicle_bumped 1, and fabric_mask,
     * noise_swatch, metal_cube and detail_map 0:
     *   - the normal map's x and y are scaled by vehicle_normal_scale (its z from the unscaled ones);
     *   - highlights of power vehicle_specular_power from the first light and the nearby point lights, toward the
     *     eye's own place, without N.L, times the specular map;
     *   - the first light as fabric_shadow_floor and fabric_shadow_fade say (the same as the soldiers' uniforms; 0 for
     *     both, as all but B2E0DA00 have: as it is).
     * The numbers are the material's (B2E0DA00's c31.x and c32.x, c29 and c30 the fade and the floor). Needs a device
     * that binds six descriptor sets; with fewer, drawn bumped as before. 0 (memset draws): as before. */
    uint32_t vehicle_bumped;
    float vehicle_normal_scale;
    float vehicle_specular_power;
    /* [rexglue-native] 2026-11-10 rn40: Call of Duty 3's glass with an environment cube (the soldiers' goggle lenses:
     * the game's per-pixel lit models' pixel shader FC767C09DBD34C2A with a cube in its tf1, drawn blended by alpha).
     * With lens_cube a cube texture, a color map, lit 1 and shading NGL_VK_SHADE_LIGHT, no normal map, environment map
     * or second texture, and the draw blended (NGL_VK_ALPHA_BLEND):
     *   color = the color map x its alpha x (the irradiance rows + lens_sun_color x N.L + the nearby point lights)
     *           + (highlights of power lens_power from that sun and the nearby point lights, toward the eye's own place,
     *              + the cube along the eye's reflection about the normal) x (1 - the alpha) x lens_reflection,
     *   alpha = the color map's alpha;
     * the sun at lens_sun_place (in the world, as the point lights), no shadow (that shader reads none), and
     * lens_cube_bias added to each part of the cube's direction. The numbers are the pass's (c31.x, c30, c29 and c16.x;
     * the power its literal c255.w, 300). Needs a device that binds six descriptor sets; with fewer, or lens_cube 0
     * (memset draws): as before.
     * rn40e: with lens_opaque 1, the soldiers' eyes (the game's pixel shader 5931940F174EA055; rn40e-rn40m took them
     * for the helmets, which are drawn as the skin, noise_swatch) the same way, but opaque and without the alpha's
     * parts:
     *   color = the color map x (the irradiance rows + sun x N.L + the nearby point lights)
     *           + (highlights of power lens_power + the cube x lens_reflection) x lens_tint,
     *   sun = max(mix(lens_sun_color x the sun's shadow, 1, saturate(lens_shadow_fade x the view depth)),
     *             lens_shadow_floor) when lens_shadow_floor > 0, else lens_sun_color (the glass reads no shadow).
     * The glass's lens_tint is 1 1 1. */
    uint32_t lens_cube;
    float lens_reflection;
    float lens_power;
    float lens_cube_bias;
    float lens_sun_place[3];
    float lens_sun_color[3];
    uint32_t lens_opaque;
    float lens_tint[3];
    float lens_shadow_floor;
    float lens_shadow_fade;
    /* [rexglue-native] 2026-11-10 rn40m: a check of the eyes' parts (ngl_side's cod3_debug_ngl_side_eyes_show):
     * 0 (memset draws) the whole as above; 1 only their light (the color map as white, no highlights or cube); 2 only
     * the color map (lit by 1, no highlights or cube); 3 magenta. */
    uint32_t lens_show;
    /* [waw-decomp] 2026-11-10: Call of Duty 2's water (the game's water.hlsl; on the Xbox 360 its pixel shader
     * DD12484223A211FC). With water_cube a cube texture (from ngl_vk_texture_cube), texture the water's height map (its
     * red, read repeating and blended between its four nearest texels), no second texture, environment map, normal
     * map, metal_cube or lens_cube, on a device that binds six descriptor sets, the draw is water. The mesh's own
     * space is taken for the world's, z up. Per pixel, with e the unit way from the eye to the pixel and uv its
     * texture coordinates:
     *   - uv moves by (0.5 - the height at uv / 2) x e.xy x 1.5 / 64;
     *   - the ripples' height there is three octaves of the map (the height at uv, + 0.6 x that at 3.7 uv, + 0.36 x
     *     that at 13.69 uv); from it at uv, at uv + (0.5 / 128, 0) and at uv + (0, 0.5 / 128) the normal
     *     n = normalize(across - here, along - here, 1);
     *   - m = e mirrored about n; the cube is read at (m.x, m.y, |m.z|);
     *   - color = mix(water_color x n.z, the cube's color, the cube's alpha)
     *             + max(m . water_sun_toward, 0)^64 x water_sun_color;
     *   - its alpha is its corners' alone; it is fogged as other draws (not with NGL_VK_SHADE_UNLIT).
     * water_sun_toward is the unit way toward the sun in the mesh's own space. water_cube 0 (memset draws): not
     * water, everything as before. */
    uint32_t water_cube;
    float water_color[3];
    float water_sun_toward[3];
    float water_sun_color[3];
    /* [rexglue-native] 2026-11-10: Call of Duty 3's effect layer. The game draws its smoke, dust and fire into a
     * target of half the picture's width and height, tested against the picture's depth at every second pixel, and
     * blends that target back over the picture with a filter of its own (its pixel shader 8E6F505DEE7CDF9E: an even
     * pixel takes its own layer pixel, an odd one the mean of those it lies between, or one of them whole at a depth
     * edge). effect_layer 1 on an NGL_VK_ALPHA_BLEND draw: drawn into that layer, in the order given among the
     * layer's draws, with the blending its `blend` asks for (NGL_VK_BLEND_ALPHA or NGL_VK_BLEND_ADD); the layer goes
     * over the picture after the other blended draws and before the depth of field and the tint. A soft draw
     * (soft_depth) fades by the layer's own depth. 0 (memset draws), or a device where the layer couldn't be made:
     * drawn straight into the picture, as before. */
    uint32_t effect_layer;
    /* [waw-decomp] 2026-11-10: Call of Duty 2's models with a specular map (its "_s" model shaders: the soldiers'
     * lprobe_s_pfog, on the Xbox 360 pixel shader 98A77B2DFA1049B4; the PC copy's lightprobe.hlsl). With normal_map
     * and specular_map set and lit 1 (the bumped path), specular_lookup a texture, and none of fabric_mask,
     * noise_swatch, metal_cube, detail_map, vehicle_bumped, blend_light, on a device that binds six descriptor sets:
     *   - the normal map's x and y (read as the bumped path reads them: alpha and green, each 2t - 1) bend the
     *     normal as N + x T + y B, made unit length, T and B the frame's unit ways along rising u and rising v (from
     *     the screen's slopes); the map gives no z;
     *   - the light is the bumped path's (the irradiance rows or the ambient, and the lights) at that normal;
     *   - the highlight, added to the lit color: the specular map's color x specular_lookup_color x the lookup
     *     texture read at (the specular map's alpha, N . H) x mix(specular_lookup_shadowed, 1, how much of the sun
     *     reaches the pixel), H halfway between the ways to the first light and to the eye's own place;
     *   - the color map is multiplied by the corners' color (18:23; the game's instruction 38).
     * The lookup texture is the game's own picture of a highlight's strength (sharpness across, N . H down); it is
     * read clamped, its top level only. specular_lookup 0 (memset draws): everything as before.
     * [waw-decomp] 2026-11-10 21:40, its second half: the same three fields and specular_map on a lightmapped draw
     * with a sun (NGL_VK_SHADE_LIGHTMAP, lit 1, lightmap_scale above 0; Call of Duty 2's world, its lmap_s shaders,
     * 6C7F0E21C61DF68F's instructions 14, 17, 29 to 31 and 39), no second texture or blend_light: added to the lit
     * color, the specular map's color x specular_lookup_color x the lookup read at (the specular map's alpha, N . H) x
     * mix(specular_lookup_shadowed, 1, the lightmap's alpha (the sun's share) x how much of the sun reaches the
     * pixel); N the corners' normal (the lightmap sits where a normal map would), H halfway between the ways to the
     * first light and to the eye. The specular map is read at the color map's coordinates. Before this such a draw
     * ignored the four fields. */
    uint32_t specular_lookup;
    float specular_lookup_color[3];
    float specular_lookup_shadowed;
    /* [waw-decomp] 2026-11-10: one third-degree term more for the irradiance rows (irradiance_rows 9), added to
     * their light: irradiance_cubic x (a . n)(b . n)(c . n), n the normal and a, b, c the three rows of
     * irradiance_cubic_axes, all in the mesh's own space. Call of Duty 2's light grid needs it: its eight weights
     * are (1 +- a)(1 +- b)(1 +- c) / 8 along three axes of its own, and the nine rows hold all of that but the
     * product of the three. Not for fabric, glass or water draws (their own numbers lie where it goes in the
     * draw's slot). The axes' first row all 0 (memset draws): none, everything as before. */
    float irradiance_cubic[3];
    float irradiance_cubic_axes[3][3];
    /* [waw-decomp] 2026-11-10: Call of Duty 2's light grid for a lit model (lit 1; not a fabric, glass or water
     * draw, no other cube), read as the game's lprobe pixel shaders read it. grid_cube is a cube texture
     * (ngl_vk_texture_cube) of four weights by direction. The light at a normal n of the mesh's own space, turned to
     * the world by grid_turn's three rows (the world's part k is n . grid_turn[k]), is for each color c (0 red,
     * 1 green, 2 blue)
     *     the cube at that direction . grid_colors[2c] + the cube at the opposite direction . grid_colors[2c + 1],
     * the cube's red, green, blue and alpha against the four numbers. It takes the place of the ambient and of the
     * irradiance rows, which stay the light where the device binds four descriptor sets only; the lights are added
     * as before. grid_cube 0 (memset draws): everything as before. */
    uint32_t grid_cube;
    float grid_colors[6][4];
    float grid_turn[3][3];
    /* [waw-decomp] 2026-11-10: Call of Duty 2's detail map (its pixel shaders with "_dtl" in their names; the PC
     * copy's shade.hlsl: sample.rgb += tex2D(detailMapSampler, texCoords * detailScale.xy).rgb - 0.5; on the Xbox
     * 360 lmap_s_dtl_pfog 6C7F0E21C61DF68F, instructions 19, 27 and 33). With detail_add_map a 2D texture, on a draw
     * that is not a fabric, glass or water draw and on a device that binds six descriptor sets, the color map's
     * color becomes itself + the detail map's at the texture coordinates x detail_add_repeat (wrapped) +
     * detail_add_offset (the game's: -0.5), not below 0, before the corners' color and the light multiply it.
     * detail_add_map 0 (memset draws): everything as before. */
    uint32_t detail_add_map;
    float detail_add_repeat[2];
    float detail_add_offset;
    /* [waw-decomp] 2026-11-11: Call of Duty 2's world by its normal map (its lmap shaders, such as 6C7F0E21C61DF68F:
     * instructions 4 and 9 to 13 make the normal N + (2x - 1) V + (2y - 1) W, of unit length, x and y the normal
     * map's, V and W the corners' two vectors; 35 to 37 and 14 use it for the sun and for the highlight). On a
     * lightmapped draw with a sun (NGL_VK_SHADE_LIGHTMAP, lit 1, lightmap_scale above 0) that is not of the fabric's,
     * the glass's or the water's kinds, on a device that binds the fabric's sets, with world_normal_map a 2D texture
     * laid out as the bumped path's normal_map (the bend along rising u in alpha, along rising v in green, each
     * 2t - 1; read at the texture coordinates, wrapped):
     *   - the normal the sun's part of the light and the highlight (specular_lookup's second half) see is the
     *     corners' normal + the two bends along the screen slopes' frame (each way of unit length), made unit
     *     length; the lightmap's own color is as before;
     *   - the map rides in set 4, where a world surface's highlights' lookup rode, so on such a draw the lookup is
     *     read from world_tables instead: a cube from ngl_vk_texture_cube whose +z face holds it (across: the
     *     specular map's alpha; down: N . H), read inside the face, its top level only. A draw with a
     *     specular_lookup and a world_normal_map but no world_tables is drawn as before, without the normal map; a
     *     draw without a highlight needs no world_tables.
     * world_normal_map 0 (memset draws): everything as before. NGL_VK_WORLD_NORMAL_MAP tells a caller that this
     * header has the two fields. */
#define NGL_VK_WORLD_NORMAL_MAP 1
    uint32_t world_normal_map;
    uint32_t world_tables;
    /* [rexglue-native] 2026-11-10 rn46m: Call of Duty 3's props with an overlay map (crates and boxes: effect
     * "standard_18", the game's pixel shader 9960D40B0F3CECA1, lit as the bumped vehicle parts are). On a
     * vehicle_bumped draw (see there) with overlay_map set (from ngl_vk_texture, not a cube) and overlay_scale above 0,
     * the base texture's color is darkened by the overlay before the light:
     *   - the overlay is read at (0.94 + a cos(angle) + b sin(angle), 0.5 - a sin(angle) + b cos(angle)), with
     *     a = (u - 0.5) x overlay_scale - 0.44 and b = (v - 0.5) x overlay_scale (angle 0: about the map's middle);
     *   - t = clamp((overlay - overlay_key) / (0.059 - overlay_key), 0, 1) for each of red, green and blue, and the
     *     color x t x t x (3 - 2 t): as it is where the overlay is dark, black where the overlay reaches the key.
     * overlay_angle is in radians (the game's constant is in turns: 2 pi x fract(turns + 0.5) - pi). Needs a device
     * that binds six descriptor sets (the map rides in set 4, which a vehicle_bumped draw leaves empty); with fewer,
     * no overlay. overlay_map 0 (memset draws): everything as before. */
    uint32_t overlay_map;
    float overlay_scale;
    float overlay_angle;
    float overlay_key[3];
    /* [rexglue-native-world] 2026-11-11 w1: Call of Duty 3's SpeedTree leaves (the game's leaf vertex shader
     * 51732ACF1B6A87C5 turns each leaf card to face the camera). billboard 1: each corner's lightmap u and v (such a
     * draw has no lightmap) are its offset from its position across and up the view, in the mesh's own units; the
     * corner is drawn at its position + u x the view's right + v x the view's up, both unit ways in the mesh's own
     * space (from the view-projection: in the sun's shadow map, the light's). 0 (memset draws): as before.
     * NGL_VK_BILLBOARD tells a caller that this header has the field. */
#define NGL_VK_BILLBOARD 1
    uint32_t billboard;
    /* [rexglue-native-guns] 2026-11-11: the gun metal's reflection blurred as Call of Duty 3's metal shaders blur
     * it (CFC45995D1294CCE's instructions 59-90 and its kin's: the cube read with register gradients,
     * setGradientH/V). On a metal_cube draw with metal_cube_blur above 0, the cube is read at the reflection R
     * (world space, z up; read at R.x, R.z, R.y as before) with the gradients metal_cube_blur x (0, -R.y / |R.z|,
     * sign R.z) and metal_cube_blur / |R.z| x (R.y^2 + R.z^2, -R.x R.z, -R.x R.y), in the cube's own axes.
     * metal_cube_blur 0 (memset draws): the cube read at the level the screen gives, as before.
     * NGL_VK_METAL_CUBE_BLUR tells a caller that this header has it. */
#define NGL_VK_METAL_CUBE_BLUR 1
    float metal_cube_blur;
    /* [acct2-bro] 2026-11-11: the gun metal's mask with a grey floor, as Call of Duty 3's 687855C9BA600FC3 (the
     * Thompson) has it (its instructions 76-90 and 132). On a metal_cube draw with metal_mask_floor above 0 the mask
     * is the specular map + metal_scratch's color + metal_mask_floor + s x metal_rim (the rim added to a grey), in
     * place of the specular map + the scratch + mix(the base texture, metal_rim, s); s as above.
     * metal_mask_floor 0 (memset draws): as before. NGL_VK_METAL_MASK_FLOOR tells a caller that this header has it. */
#define NGL_VK_METAL_MASK_FLOOR 1
    float metal_mask_floor;
    /* [acct2-bro] 2026-11-12: the gun metal's Normal_Map_Depth, as Call of Duty 3's 687855C9BA600FC3 (the Thompson,
     * instructions 21-26: c36.x) and CC31EB0C3D04DBEF / 35841B3D12A90CA4 (the M1 Garand: c29.x) have it. On a
     * metal_cube draw with metal_normal_depth above 0 the normal map's x and y are multiplied by it; its z stays the
     * one from the unscaled x and y. metal_normal_depth 0 (memset draws): the map as it is, as before.
     * NGL_VK_METAL_NORMAL_DEPTH tells a caller that this header has it. */
#define NGL_VK_METAL_NORMAL_DEPTH 1
    float metal_normal_depth;
    /* [acct2-bro] 2026-11-12: Call of Duty 3's bumped world, step 1: its normal (the game's pixel shaders
     * 141294CDC7C97127, 3CC8661DDBB318D9, 6D703BD4597C9D00 and kin, read whole: the first one's instructions 17-41;
     * dev/agents/rexglue-native/RENDER-NOTES.md has the formulas). On a draw world_normal_map applies to (above)
     * that has no specular_lookup, world_bump_scale not 0 says the map is read that game's way, not Call of Duty 2's:
     *   - x and y are the map's alpha and green, each 2t - 1, and z = sqrt(1 - x^2 - y^2);
     *   - with world_detail_normal_map a 2D texture packed the same way and world_detail_repeat above 0, the detail
     *     map is read at the texture coordinates x world_detail_repeat (wrapped) and the two are mixed as the
     *     shader mixes them: (x, y) = world_bump_scale x the map's (x, y) x the detail's z + world_detail_scale x
     *     the detail's (x, y) x the map's z, and z = the two z's multiplied; without one, (x, y) =
     *     world_bump_scale x the map's (x, y) and z stays the map's;
     *   - the normal is x along the frame's first way + y along its second + z along the corner's normal, made
     *     unit length; the frame is world_normal_map's (from the screen slopes of the position and the texture
     *     coordinates, each way of unit length). world_bump_frame turns it about, for tests: bit 0 x the other
     *     way, bit 1 y the other way, bit 2 x and y changed over.
     * The lightmap's up term, the sun's part and the nearby lights see that normal. world_bump_scale 0 (memset
     * draws): as before. NGL_VK_WORLD_BUMP tells a caller that this header has the fields. */
#define NGL_VK_WORLD_BUMP 1
    float world_bump_scale;
    uint32_t world_detail_normal_map;
    float world_detail_scale;
    float world_detail_repeat;
    uint32_t world_bump_frame;
    /* [acct2-bro] 2026-11-12 rn60a: a skin draw's own tint. With skin_tint not all 0, a draw taken as the soldiers'
     * skin (noise_swatch, above) has its base texture also multiplied by it (red, green, blue). The game's pixel
     * shader D2B5785887F9C326 (a soldier's simpler body: the model of three surfaces the game draws from some way
     * off) multiplies its color map by a color compiled into it (c255: 1.11383 1.115 1.04475) and has no sheen and
     * no highlights; it is drawn as the skin with every extra off and this. 0 0 0 (memset draws): as before.
     * NGL_VK_SKIN_TINT tells a caller that this header has the field. */
#define NGL_VK_SKIN_TINT 1
    float skin_tint[3];
};

/* How a blended mesh draw goes onto what's behind it. */
enum {
    NGL_VK_BLEND_ALPHA,         /* by its alpha: color x alpha + behind x (1 - alpha) */
    NGL_VK_BLEND_ADD,           /* added: behind + color x alpha */
    NGL_VK_BLEND_SUBTRACT,      /* taken away: behind - color x alpha */
    NGL_VK_BLEND_MULTIPLY,      /* [waw-decomp] 2026-11-10: multiplied: behind x color + behind x (1 - alpha), so
                                 * behind x lerp(1, color, alpha) when the color is already times its alpha (decals);
                                 * pulled a little toward the eye (depth bias -1, no slope part) to stay on its surface */
};

/* The meshes the next ngl_vk_draw_quads draws first, depth-tested, under the quads (the list is copied; at most 16384
 * a frame). */
void ngl_vk_set_meshes(const struct ngl_vk_mesh_draw *draws, uint32_t count);

/* The sun's shadow for the next ngl_vk_draw_quads (one frame): light_view_projection takes world space to the light's
 * clip space (Direct3D row vectors, orthographic: x and y -1 to 1 across the map, z 0 nearest the sun to 1), and
 * camera_view_projection is the view_projection of the draws marked sun_shadowed. The shadow casters are drawn into a
 * 2048 x 2048 depth map first. Null for no shadow this frame (sun_shadowed draws are then lit as before). */
void ngl_vk_set_sun_shadow(const float light_view_projection[16], const float camera_view_projection[16]);

/* Depth of field and blur for the next ngl_vk_draw_quads (one frame; Call of Duty 3's: aiming down the sights, and
 * the whole world blurred behind its menus): after the meshes and before the quads, the picture is mixed with a
 * blurred copy of itself (a quarter of its size, blurred) by the opaque meshes' depth (their clip-space z, 0 near to
 * 1 far): not at all nearer than near_depth, fully from far_depth on, in between by how far along it is, and at least
 * by blanket (0 to 1) everywhere; never where the depth is under keep_depth (a scene drawn in front of the world,
 * squeezed into its nearest depths, stays sharp). With a blanket the blur is wider (two rounds). Only in a frame with
 * meshes. near_depth and far_depth above 1: no depth of field, the blanket alone. */
void ngl_vk_set_depth_of_field(float near_depth, float far_depth, float keep_depth, float blanket);

/* A color tint for the next ngl_vk_draw_quads (one frame; Call of Duty 3's post effect 1): after the meshes and the depth
 * of field, before the quads, each pixel moves toward its brightness (its red, green and blue times weights, summed)
 * times color, by amount; not where the opaque depth is under keep_depth (as ngl_vk_set_depth_of_field). Only in a
 * frame with meshes. */
void ngl_vk_set_tint(float amount, const float weights[3], const float color[3], float keep_depth);

/* A color look-up for the next ngl_vk_draw_quads (one frame; Call of Duty 3's post effect 3, shader_8B289D78B550EBBD):
 * in the tint's pass, after the brightness, each pixel's red and green taken from the table texture at (red, green)
 * and its blue at (blue, red), alpha kept; not where the opaque depth is under keep_depth (as ngl_vk_set_tint).
 * table: from ngl_vk_texture. Only in a frame with meshes. */
void ngl_vk_set_color_lookup(uint32_t table, float keep_depth);

/* A brightness for the next ngl_vk_draw_quads (one frame; Call of Duty 3's post effect 2, its pixel shader
 * shader_377AC015A488B262: (color x brightness) to the power, alpha kept), in the tint's pass, after the tint; not where
 * the opaque depth is under keep_depth (as ngl_vk_set_tint). Only in a frame with meshes. */
void ngl_vk_set_brightness(float brightness, float power, float keep_depth);

/* [waw-decomp] 2026-11-11: a film tweak for the next ngl_vk_draw_quads (one frame; Call of Duty 4's and World at War's
 * film, their PC copies' postfx_color.hlsl): in the tint's pass, in place of the tint, the brightness and the color
 * look-up, each pixel's brightness lum (its red, green and blue times 0.299, 0.587 and 0.114, summed) gives
 * bias + (base + delta x lum) x (color x bias[3] + lum) for red, green and blue, alpha kept; not where the opaque
 * depth is under keep_depth (as ngl_vk_set_tint). bias, base and delta are the game's colorBias, colorTintBase and
 * colorTintDelta as they are. Only in a frame with meshes. */
void ngl_vk_set_film_tweak(const float bias[4], const float base[3], const float delta[3], float keep_depth);

/* The color the 3D picture starts from, red, green and blue from 0 to 1 (a scene's clear color), for the frames drawn
 * after this; black until set. */
void ngl_vk_set_clear_color(const float rgb[3]);

/* A motion blur for the next ngl_vk_draw_quads (one frame; Call of Duty 3's post effect 4, its pixel shader
 * shader_1A3598F9A2CE3A59: this frame x (1 - amount) + the last frame's picture x amount). [rexglue-native] 2026-11-10
 * rn41g: a draw of its own with the tint's pipeline after the tint's, over the whole 3D, the scenes in front included
 * (the game's concussion blurs its gun), before post effect 7; keep_depth: the tint pass's (as ngl_vk_set_tint's). The
 * last frame's picture is the whole one, quads included, kept after them (the game's compass trails in a concussion),
 * and only in frames with a motion blur, so the first frame of one blends nothing. Only in a frame with meshes. */
void ngl_vk_set_motion_blur(float amount, float keep_depth);

/* [rexglue-native] 2026-11-11 rn47g: Call of Duty 3's post effect 4 as the game draws it, for the next
 * ngl_vk_draw_quads (one frame; in place of ngl_vk_set_motion_blur, which blends the last finished frame instead): an
 * afterimage that streams outward. ngl_vk keeps a picture of its own for it, from one call's frame to the next's,
 * however many frames lie between (the game's kept picture is as old; the very first time it starts as this frame's).
 * Two draws with the tint's pipeline after the tint's and the sky glow, before post effect 7 and the quads, over the
 * whole 3D, the scenes in front included, as the game's pixel shader does it (ps_3_0, out = fac1 x Diffuse_Map + fac2
 * x Diffuse2_Map with fac1 = (1 - share) and fac2 = share):
 *   kept  = (1 - share) x the picture so far + share x kept, both read `inset` of the picture's width and height in
 *           from every edge (so the kept picture grows outward a little each frame);
 *   shown = (1 - share) x the picture so far + share x kept.
 * keep_depth: the tint pass's (as ngl_vk_set_tint's; the afterimage itself keeps nothing out). share 0 or less: none.
 * Only in a frame with meshes. A caller that never calls this gets what it got before. */
void ngl_vk_set_afterimage(float share, float inset, float keep_depth);

/* [rexglue-native] 2026-11-11 rn48b: the afterimage's kept picture (ngl_vk_set_afterimage) at a quarter of the picture's width and height
 * (on), as Call of Duty 3 keeps it (320 x 180 for its 1280 x 720): the first draw's picture shrunk to that and
 * stretched back, both filtered (rn48c), so its trails are as soft as the game's; or at full size (off, the default,
 * as before). Stays as set until changed. */
void ngl_vk_set_afterimage_small(int on);

/* [rexglue-native] 2026-11-11 rn47i: for the next ngl_vk_draw_quads (one frame): its first vertex_count vertices (whole
 * triangles; any number at or above the quads' count: all of them) are drawn before the afterimage's and post effect
 * 7's draws instead of after them, in a frame that has one of the two (in any other frame this changes nothing). Call
 * of Duty 3 asks for its post effects after it has drawn everything its list holds, the HUD included (rn47h: no draw
 * of the game's follows them), so a concussion smears and doubles its compass and crosshair, and its hurt screen's
 * dark red lies under the concussion's gray, not over it. The rest of the quads are drawn after, as before. */
void ngl_vk_set_quads_under_effects(uint32_t vertex_count);

/* [rexglue-native] 2026-11-11 rn47n: the last finished frame as a quad's texture. Call of Duty 3 draws its screen
 * back over itself in a concussion: a whole-screen quad whose texture is the screen buffer shown last (its two screen
 * textures take turns), a little to the side, blended by a share that fades: an echo of the last picture, HUD and
 * all, fed back frame after frame. ngl_vk_keep_last_frame() asks that this frame's finished picture (the quads
 * included) be kept for the next frame, and returns 1 when the last frame's is held now: then a quad of the next
 * ngl_vk_draw_quads may name NGL_VK_LAST_FRAME as its texture (the whole target, 0..1 across it; alpha taken as its
 * `textured` says). When it returns 0 (the frame before was not kept) such a quad would get the frame's picture
 * instead: leave it out. One frame; a caller that never calls this gets what it got before. */
#define NGL_VK_LAST_FRAME 0xFFFFFFFFu
int ngl_vk_keep_last_frame(void);

/* [rexglue-native] 2026-11-11 rn47o: this frame's picture so far as a quad's texture: a quad of ngl_vk_draw_quads that
 * names NGL_VK_THIS_FRAME is drawn with the target as it stands when the quad's turn comes (the 3D, the post effects
 * and the quads before it), the whole target, 0..1 across it. Call of Duty 3's concussion quad is this, not the last
 * frame (rn47n): the game copies its screen into a texture at the end of every scene and the quad, in a later scene
 * of the same frame, reads that copy. ngl_vk copies the target for it when it meets the quad (one copy a run of such
 * quads). Only in a frame with meshes; in any other frame the quad gets the frame's picture, as an unknown number
 * does. */
#define NGL_VK_THIS_FRAME 0xFFFFFFFEu

/* [rexglue-native] 2026-11-10 rn41b: the blur, grain and gray of a concussion for the next ngl_vk_draw_quads (one
 * frame; Call of Duty 3's post effect 7, drawn by the game as two passes over its screen copy after its other post
 * effects: pixel shaders 470878A4FB9D8A6F and D7B45AAE96CFCD55), a draw of its own with the tint's pipeline after the
 * motion blur's, over the whole 3D, the scenes in front included (the game's concussion blurs its gun; its crosshair
 * stays sharp, so the quads come after). First: five
 * taps of the picture at hashed places up to first[0] / 2 apart (in picture widths and heights), each mixed with a
 * hashed grain by first[1], averaged, then moved by first[2] toward the brightness of (that x scale) times gray;
 * first[3] is its time (the hashes' seed). Then that mixed with itself at a place moved by move (wrapped), by
 * moved_share. keep_depth: the tint pass's (as ngl_vk_set_tint's; post effect 7 itself keeps nothing out). scale 0:
 * none. Only in a frame with meshes and the tint pass. */
void ngl_vk_set_film(const float first[4], const float gray[3], float scale, const float move[2], float moved_share,
                     float keep_depth);

/* [rexglue-native] 2026-11-10: the sky's glow for the next ngl_vk_draw_quads (one frame; Call of Duty 3 draws it every
 * frame after its tint: its sky bleeds light over the roofs and trees before it). The picture where nothing is drawn
 * (the opaque depth at its far end) shrunk to 256 x 256 with everything else black, blurred `passes` times (at most
 * 8; pass k: the mean of four taps times gain, the taps in a plus k x tap of the small target's pixels from the
 * middle), then added over the game's screen times strength; after the tint's draw, before the motion blur, post
 * effect 7 and the quads. lean (0..1) moves each blur's taps from the plus onto the line from the pixel to `toward`
 * (a point in the screen's 0..1, x across, y down; it may lie outside): 0, 1, 4 and 8 times k x tap / 256 of the way
 * there (the game's shafts of light toward the sun). tap 0: no blur (the sky alone, softened by its small size).
 * strength 0 or less: none. Only in a frame with meshes. */
void ngl_vk_set_glow(float tap, float gain, uint32_t passes, float strength, float lean, const float toward[2]);

/* [rexglue-native-fx] 2026-11-11 fx1: Call of Duty 3's heat haze (its "cdheathaze": the picture shimmers over fires,
 * engines and gun barrels). A blended mesh draw (NGL_VK_ALPHA_BLEND, effect_layer 1) whose `blend` is
 * NGL_VK_BLEND_HEAT_MASK is drawn into the effect layer's heat mask instead of the picture (built up as "over" builds
 * alpha; left out where there is no effect layer); the screen warp then shows where that mask lies. Kept apart from the
 * NGL_VK_BLEND_* list's numbers. */
#define NGL_VK_BLEND_HEAT_MASK 16u                          /* [rexglue-native-fx] 2026-11-11 fx1 */
#define NGL_VK_WARP_MOST_CORNERS 4096u                      /* [rexglue-native-fx] 2026-11-11 fx1 */
/* [rexglue-native-fx] 2026-11-11 fx1: the screen warp for the next ngl_vk_draw_quads: one triangle strip of `count`
 * corners (at most NGL_VK_WARP_MOST_CORNERS; past that it is dropped), four floats each: where the corner goes on the
 * game's screen (x, y from -1 to 1, y down) and where it reads the picture (u, v from 0 to 1 across the game's screen).
 * After the effect layer is back over the picture, the picture is copied and drawn back through the strip, blended by
 * the heat mask. A frame with no NGL_VK_BLEND_HEAT_MASK draws draws no warp. */
void ngl_vk_set_screen_warp(const float *corners, uint32_t count);  /* [rexglue-native-fx] 2026-11-11 fx1 */

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
/* [rexglue-native] 2026-11-09: the part of the target the next draw's grass is kept to, in pixels from the top-left
 * corner (split screen: the part of the player whose camera the grass's constants hold; without it that player's
 * nearest blades spread into the next player's part). A width or height of 0: the whole target. Set it before
 * ngl_vk_set_grass each frame; it is used once, like the patches. */
void ngl_vk_set_grass_scissor(int32_t x, int32_t y, uint32_t width, uint32_t height);

/* The target as the last draw left it: width x height pixels of 4 bytes, red, green, blue, alpha. NULL before the
 * first draw, and when the last draw didn't copy it out (ngl_vk_set_readback(0)). */
const uint8_t *ngl_vk_target_pixels(uint32_t *width, uint32_t *height);

/* side220: the meshes' corners and indices in the GPU's own memory (on: the default), copied in through a staging
 * buffer at the start of each draw; off keeps them in memory the host sees, where the GPU reads them over the bus. Before
 * ngl_vk_init (or ngl_vk_init_on_device). */
void ngl_vk_set_geometry_on_gpu(int on);

/* side220: bytes staged for the GPU's corners and indices since the last call, and how many times a full staging half
 * was copied and waited for at once (flushes, unless NULL). On the thread that draws. */
uint64_t ngl_vk_take_staging(uint64_t *flushes);

/* [rexglue-native] 2026-11-09: batched textures. Off (the default), ngl_vk_texture and ngl_vk_texture_levels copy each
 * new texture to the GPU and wait for it there and then: a submit and a wait for every texture, a long frame when many
 * come at once (Call of Duty 3's native picture: about 10 ms a texture on a shared device). On, a new texture's levels
 * wait in a staging buffer of two halves and are copied in at the start of the next draw's commands, before any draw
 * reads them, with no wait; a full half is copied in and waited for at once (counted as a flush). A texture bigger than
 * a half, or one made between ngl_vk_begin_frame and ngl_vk_end_frame, still goes the old way. After ngl_vk_init (or
 * ngl_vk_init_on_device), on the thread that draws. Returns 0 when on (or turned off), -1 when the staging buffer
 * couldn't be made (it stays off). */
int ngl_vk_set_batched_textures(int on);

/* [rexglue-native] 2026-11-09: textures batched (see ngl_vk_set_batched_textures) since the last call, and how many
 * times a full half was copied in and waited for at once (flushes, unless NULL). On the thread that draws. */
uint64_t ngl_vk_take_texture_uploads(uint64_t *flushes);

/* side219: how long ngl_vk waited for the GPU to finish the last frame (in ns), and how many times (waits, unless
 * NULL), since the last call; the counts start again from 0. On the thread that draws. */
uint64_t ngl_vk_take_gpu_wait(uint32_t *waits);

/* [acct2-bro] 2026-11-12: anisotropic filtering. most: how many samples at most a texture with smaller levels
 * (ngl_vk_texture_levels with more than one level) is read with along the direction it is stretched in on the
 * screen: 2, 4, 8 or 16 (the GPU's own most if that is less); 1 or less: none (the default: every texture read as
 * before). A texture of one level, the cubes, the picture and ngl_vk's own targets are always read as before.
 * Vulkan wants the device's samplerAnisotropy feature on for it: ngl_vk's own device (ngl_vk_init) gets it when
 * anisotropy is asked for and the GPU has it; on a borrowed device (ngl_vk_init_on_device)
 * borrowed_device_has_it says whether its owner turned the feature on (0: no anisotropy there).
 * Before ngl_vk_init (or ngl_vk_init_on_device): the samplers are made at the start.
 * NGL_VK_ANISOTROPY tells a caller that this header has it. */
#define NGL_VK_ANISOTROPY 1
void ngl_vk_set_anisotropy(float most, int borrowed_device_has_it);

/* [acct2-bro] 2026-11-12: the samples in use (see ngl_vk_set_anisotropy) once ngl_vk has started; 1: none. */
float ngl_vk_anisotropy(void);

/* Frees everything ngl_vk_init (or ngl_vk_init_on_device) made. */
void ngl_vk_shutdown(void);
