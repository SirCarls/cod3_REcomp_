/* [acct2-bro] Big Red One port: nglLoadTexture, natively. See ngl_texture_load.c. */
#pragma once
#include <stdint.h>

#define NGL_MAX_LEVELS 13u      /* 4096 down to 1 */

/* A loaded texture decoded for drawing: 4 bytes a pixel, blue, green, red, alpha, rows packed. levels[0] is pixels
 * (level 0, width x height); levels[1..level_count - 1] are the file's own smaller levels, each half the one above
 * (at least 1). */
struct ngl_loaded_texture {
    uint32_t texture;           /* the nglTexture (guest address) the game was given */
    uint32_t width, height;
    uint8_t *pixels;
    uint32_t level_count;       /* 1 when the file has no smaller levels */
    uint8_t *levels[NGL_MAX_LEVELS];
};

/* nglLoadTexture(name, type, data, size): the texture called name, made from the file in guest memory at data
 * the first time. Returns the nglTexture's guest address, or the game's default texture when it can't. */
uint32_t ngl_texture_load(uint32_t name, uint32_t type, uint32_t data, uint32_t size);

/* The decoded pixels of a texture ngl_texture_load made, or NULL; for an animated texture list, its first frame's. */
const struct ngl_loaded_texture *ngl_texture_loaded(uint32_t texture);

/* How many frames an animated texture list has, or 0 for any other texture. */
unsigned ngl_texture_frame_count(uint32_t texture);

/* An animated texture list's frame (frame mod its count) as the nglTexture of its own, once a texture of that name
 * is loaded; 0 when it isn't yet, or texture isn't a list. */
uint32_t ngl_texture_list_frame(uint32_t texture, unsigned frame);

/* RE_RegisterShader(path) (0x0023EC30): the texture loaded under the path's last name ("gfx/reticle/hud@center_ads.tga"
 * is "center_ads"), or one that becomes it when a texture of that name loads. */
uint32_t ngl_texture_register_shader(uint32_t path);

/* nglFindTextureAddRef(name): the texture already loaded under name (a 32-byte NGL name), counted up, or 0. */
uint32_t ngl_texture_find(uint32_t name);

/* nglFindTexture(name) (0x00063530, the texture manager's find): the same texture, not counted up. The menus look
 * their backgrounds up by name with it. */
uint32_t ngl_texture_lookup(uint32_t name);

/* [rexglue-native] 2026-11-11: 1 if texture is one ngl_texture_load made under the NGL name `name` (lower case,
 * without its 4-byte hash), else 0. The loading screen's map is every mission's "texload". */
int ngl_texture_named(uint32_t texture, const char *name);
