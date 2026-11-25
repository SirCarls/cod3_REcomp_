/* [acct2-fh] 2026-11-14 Finest Hour port: the game's frame timer, as a readable stand-in. See fh_frame_clock.c. */
#pragma once

#include <stdint.h>

/* The game's frame timer, 0x000A44C0 (a stand-in: fh/config/stand_in_functions.tsv), called once a frame by the
 * game's loops: this frame's step in 60ths of a second, the game's time and its frame count, as the original does,
 * but with the time between frames measured to the microsecond instead of the whole millisecond. Returns what the
 * original leaves in eax: the GetTickCount it wrote as the frame's end. */
uint32_t fh_frame_clock_step(void);

/* What the frame timer did since the last call, for the frame-rate log (fh_frame_pace.c's FH_FPS_LOG line). */
struct fh_frame_clock_stats {
    unsigned frames;            /* calls since the last take */
    unsigned zero_steps;        /* frames that moved the game by nothing in a mode that moves it (0 and 6) */
    unsigned limited_steps;     /* frames whose step was held to the limit of 5 sixtieths */
    unsigned other_modes;       /* frames in a mode other than 0 and 6 (fixed, scaled, frozen) */
    double step_min, step_max;  /* the step in 60ths of a second, over the frames in mode 0 or 6 */
    double step_sum, step_sum2;
    unsigned held_frames;       /* [acct2-fh] 11-22: frames held at a step of 0 by game.fixed_step_above_60 */
    unsigned gathered_updates;  /* frames that got a step of 1 from gathered short frames */
    unsigned held_drawn;        /* [acct2-fh] 11-22: held frames the timer drew itself (the in-play loop's) */
};
void fh_frame_clock_take_stats(struct fh_frame_clock_stats *out);

/* The game's clock of frames as seconds, 0x001119B0 (a stand-in, as above): the original pushes the frame count x
 * 1/60 on the x87 stack, a clock that runs at the frame rate's pace (4 times too fast at 240 frames a second). Its only
 * user is the time of the game's noise wobble (0x00111BB0, through the object at 0x003E3870; see fh_frame_clock.c).
 * This pushes the time the frames really took instead (as many seconds as frames/60 at 60 frames a second). Returns
 * what the original leaves in eax: the frame count. */
uint32_t fh_frame_clock_frames_as_seconds(void);

/* [acct2-fh] 11-22: 1 when this frame's step lets the game update, 0 when game.fixed_step_above_60 held it at 0 (a
 * frame shorter than a 60th, the game's update waiting for a whole 60th). fh_pad.c holds the pad on held frames. */
int fh_frame_clock_updates(void);

/* [acct2-fh] 11-22: 1 while the timer is drawing a held frame itself (the in-play loop skips its drawing on a step of
 * 0; see fh_frame_clock.c). */
int fh_frame_clock_drawing_held(void);

/* The game's time (60ths of a second) and frame count, read from the game's own globals. */
double fh_frame_clock_game_time(void);
uint32_t fh_frame_clock_frame_count(void);
