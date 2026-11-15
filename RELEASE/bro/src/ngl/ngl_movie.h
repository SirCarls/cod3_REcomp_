/* [acct2-bro-game] Big Red One port: movies at their own pace whatever the frame rate. See ngl_movie.c. */
#pragma once

#include <stdint.h>

#include "recomp_types.h"

/* For recomp_lookup_manual(): the gate in front of the movie player's decoder (it holds pictures back to their own
 * pace with display.movie_pacing, and only counts them without), or NULL for any other address. */
recomp_func_t ngl_movie_method_for(uint32_t xbox_va);

/* Once a frame, at its end (host time, ns): a movie that has stopped showing pictures goes to the log. */
void ngl_movie_frame_end(int64_t now);

/* [acct2-bro] 2026-11-14: the host time (ns, CLOCK_MONOTONIC) the decoded film picture is to go on the screen
 * (display.movie_lead_ms), 0 when none is waiting. */
int64_t ngl_movie_show_at(void);

/* [acct2-bro] 2026-11-14: the waiting picture goes on the screen with this frame. */
void ngl_movie_show_now(void);

/* [acct2-bro] 2026-11-14: while a decoded picture waits for its time, the picture before it (a host copy, its size
 * and pitch) for the frame to show instead of the locked one; 0 otherwise. */
int ngl_movie_held_picture(const uint8_t **pixels, uint32_t *width, uint32_t *height, uint32_t *pitch);

/* [acct2-bro] 2026-11-14: once a frame, right after its picture is drawn (host time, ns): for the movie's log line
 * on how evenly its pictures reached the screen. */
void ngl_movie_frame_drawn(int64_t drawn);
