/* [acct2-bro] Big Red One port: what NGL's texture stand-ins must do for the game's own code. See ngl_texture.c. */
#pragma once
#include <stdint.h>

/* nglTexture_Lock's part the game depends on: fills the texture's locked rectangle (pitch and pixel pointer). */
void ngl_texture_lock(uint32_t texture);

/* The guest buffer, size and pitch the last lock handed out, for showing what the game wrote there. 0 before the
 * first lock. */
int ngl_texture_last_lock(uint32_t *buffer, uint32_t *width, uint32_t *height, uint32_t *pitch);

/* The nglTexture the last lock was for (the back buffer while a movie plays), 0 before the first lock. */
uint32_t ngl_texture_last_locked(void);

/* [rexglue-native] 2026-11-11: how many locks there have been (a movie locks the back buffer for every picture), so
 * ngl_present.c can tell whether the game has written a new picture since it kept the loading screen's map. */
unsigned long ngl_texture_lock_count(void);

/* nglGetBackBufferTexture's stand-in: the same block every call, with the 640 x 480 screen's size at +0x10 / +0x14
 * (the game's viewport code divides by it). */
uint32_t ngl_back_buffer_texture(void);

/* nglSetRenderTarget (0x00073A70) as the game has it: the texture becomes the current scene's render target (+0x2F8),
 * which the scene's viewport takes its size from. Returns 0 (the function returns nothing). */
uint32_t ngl_set_render_target(uint32_t texture);
