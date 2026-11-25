/* [waw-decomp] Finest Hour port: the picture each frame ends with. See fh_present.c. */
#pragma once
#include <stddef.h>
#include <stdint.h>

/* D3DDevice_Swap (0x0022D440, Flags): the end of the game's frame. Shows the render target (through ngl_vk, in a
 * window when there is a display, else offscreen), counts the frame rate, and ends the frame's draw census
 * (fh_d3d_state.c). Returns 0, as the phase 1 stand-in did (the game doesn't read it). */
uint32_t fh_present_swap(uint32_t flags);

/* [acct2-fh] 2026-11-13: D3DDevice_SetGammaRamp (0x0021EFC0, Flags, pRamp): the ramp the Xbox's video output showed
 * every frame through (D3DGAMMARAMP: 256 red, then 256 green, then 256 blue bytes). Finest Hour sets one, once, at
 * start-up (0x0012C810). Kept, described in the log, and shown in the window through ngl_vk_set_window_table (FH_GAMMA=0:
 * not); saved pictures stay what the game drew. Returns 0 (the game's is void). */
uint32_t fh_present_gamma_ramp(uint32_t flags, uint32_t ramp);

/* The ramp the game last set, as 3 x 256 bytes (red, green, blue), or NULL before it set one or with FH_GAMMA=0. */
const uint8_t *fh_present_gamma(void);

/* [acct2-fh] 2026-11-14 What the game is drawn at now, in plain words, for the F2 window's VIDEO page (the log's
 * "resolution" line: "auto -> 1600x900 (100%, shape 1.778:1), shown in 1600x900"); "" before the first frame. */
void fh_present_resolution_note(char *out, size_t size);
