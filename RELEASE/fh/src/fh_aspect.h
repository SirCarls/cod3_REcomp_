/* [acct2-fh] 2026-11-13 Finest Hour port: the picture's shape (display.aspect_ratio), Hor+ as Call of Duty 3's port
 * does it (dev/agents/acct2-fh/RESOLUTION.md, stage 3). fh_aspect.c says how. */
#pragma once

#include <stdint.h>

/* The game's own shape: its 640 x 480 screen. */
#define FH_ASPECT_GAME (4.0f / 3.0f)

/* The stand-in for RenderWare's camera view window setter at 0x001C6A20 (camera, &window) -> camera
 * (fh/config/stand_in_functions.tsv): what the game's code does, with the window widened to the picture's shape. */
uint32_t fh_aspect_set_view_window(uint32_t camera, uint32_t window);

/* The shape (width / height) the game's 3D view is drawn at from now on; fh_present.c sets it with each render plan.
 * Cameras already set get the new shape at the next fh_aspect_follow. */
void fh_aspect_set_shape(float shape);

/* The shape set last (FH_ASPECT_GAME until one is). */
float fh_aspect_shape(void);

/* Once a frame, on the game's thread (Swap): after a change of shape, each camera this run has seen gets its view
 * window again for the new shape. */
void fh_aspect_follow(void);
