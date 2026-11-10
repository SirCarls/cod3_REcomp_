/* [acct2-bro] Big Red One port: NGL's frame clock (vblank count, frame pacing and shader time), for the
 * nglBeginFrame, nglEndFrame and nglAdvanceShaderTime stand-ins. See ngl_frame_clock.c. */
#pragma once

#include <stdint.h>

/* [acct2-bro-game] 1 while frames come at the game's own pace (its frame interval's wait), 0 while the port paces
 * them (display.max_fps or BRO_FRAME_PACING=0). */
int ngl_frame_clock_paced(void);

/* What nglBeginFrame records about the time: the vblank count and the time stamp counter when the frame began. */
void ngl_frame_clock_begin_frame(void);

/* What nglEndFrame does about the time: waits for the frame interval nglSetVBlankMode chose, runs the game's vblank
 * callback once for each vblank that went by, then the game's own end-of-frame bookkeeping. */
void ngl_frame_clock_end_frame(void);

/* What nglAdvanceShaderTime does: NGL's shader time goes up by the frame's time (seconds, a float's bits), and the
 * four words at vector are kept for the shaders. */
void ngl_frame_clock_advance_shader_time(uint32_t vector, uint32_t seconds_bits);
