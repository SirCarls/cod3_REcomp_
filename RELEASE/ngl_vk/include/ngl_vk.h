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

/* A texture for quads to use: width x height pixels of 4 bytes, blue, green, red, alpha, rows packed. Returns its
 * number (1 and up), or 0 when it can't be made. */
uint32_t ngl_vk_texture(const void *pixels, uint32_t width, uint32_t height);

/* The same with the texture's own smaller levels: levels[i] is level i, max(1, width >> i) x max(1, height >> i)
 * pixels laid out the same way; level_count of them (stopping early at a NULL one), sampled with linear blending
 * between levels as the GPU does. ngl_vk_texture is this with one level. */
uint32_t ngl_vk_texture_levels(const void *const *levels, uint32_t level_count, uint32_t width, uint32_t height);

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

/* Frees everything ngl_vk_init (or ngl_vk_init_on_device) made. */
void ngl_vk_shutdown(void);
