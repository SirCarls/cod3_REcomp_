/* [waw-decomp] Finest Hour port, phase 2: the game's Direct3D draws on Vulkan. See fh_draw.c. */
#pragma once
#include <stdint.h>

/* Starts the drawing layer once ngl_vk is up (fh_present.c calls it after ngl_vk_init). Returns 0 when it is ready;
 * without it draws are left out. game_width x game_height is the game's screen (the back buffer). */
int fh_draw_start(uint32_t game_width, uint32_t game_height);

/* [acct2-fh] Before ngl_vk is closed and opened again at another size (a new render resolution, fh_present.c): shuts
 * the drawing layer down and forgets what it made on Vulkan; fh_draw_start starts it again afterwards. Between
 * frames. */
void fh_draw_stop(void);

/* One DrawIndexedVertices with the device's state now (fh_d3d_state.c calls it). Draws nothing until fh_draw_start
 * has worked. */
void fh_draw_indexed(uint32_t primitive_type, uint32_t index_count, uint32_t indices);

/* [acct2-fh] One DrawVertices: count vertices of the streams from vertex first, without indices. */
void fh_draw_vertices(uint32_t primitive_type, uint32_t first, uint32_t count);

/* [acct2-fh] One DrawVerticesUP: count vertices at guest address data, stride bytes apart (not from the streams). */
void fh_draw_vertices_up(uint32_t primitive_type, uint32_t count, uint32_t data, uint32_t stride);

/* At Swap: whether this frame has draws of its own (a Vulkan frame is open). */
int fh_draw_frame_open(void);

/* [acct2-fh] 11-22: 1 when this frame has drawn something already (a clear now comes in the middle of the frame). */
int fh_draw_frame_has_draws(void);

/* [acct2-fh] 11-22: a clear of the depth (to z, 0 to 1) and stencil in the middle of the frame: the frame's pass is
 * ended and started again on the same targets, keeping the colors (ngl_vk_low's pass without clear_color loads them)
 * and clearing the depth. 1 when done; 0 when no frame is open or the pass could not start again with the clear. */
int fh_draw_clear_depth(float z, uint32_t stencil);

/* At Swap, for a frame with draws: puts the CPU picture on top when the game wrote one this frame (pixels not NULL:
 * width x height, pitch bytes a row, 32-bit X8R8G8B8), closes the frame and shows it. Returns 0 when it was shown. */
int fh_draw_frame_end(const uint8_t *pixels, uint32_t width, uint32_t height, uint32_t pitch);

/* Draws this run so far, for the log: drawn, and left out (with the first reason). */
void fh_draw_report(void);

/* [acct2-fh] A guest resource was locked (D3DResource_Lock2, fh_d3d.c): when it is a texture, one of its surfaces or a
 * palette, the textures kept from its memory are let go of and made again from it at their next draw. Called with
 * the guest CPU held (RECOMP_GUEST_LOCK), as every draw is. */
void fh_draw_resource_locked(uint32_t resource);

/* [waw-decomp-fh2 r19] 2026-11-24 For the game's text at the picture's size (fh_text.c): the size of ngl_vk's picture, and
 * where the game's 640 x 480 screen lies on it (its 4:3 part: the films, the HUD, the text). Each returns 0 before
 * the drawing layer has started. fh_draw_begin_frame opens a frame for a Swap without draws of its own (a film
 * with text over it): 1 when one is open. */
int fh_draw_size(uint32_t *width, uint32_t *height);
int fh_draw_screen_area(float *x, float *y, float *width, float *height);
int fh_draw_begin_frame(void);
