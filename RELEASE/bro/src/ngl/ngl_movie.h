/* [acct2-bro-game] Big Red One port: movies at their own pace whatever the frame rate. See ngl_movie.c. */
#pragma once

#include <stdint.h>

#include "recomp_types.h"

/* For recomp_lookup_manual(): the gate in front of the movie player's decoder (it holds pictures back to their own
 * pace with display.movie_pacing, and only counts them without), or NULL for any other address. */
recomp_func_t ngl_movie_method_for(uint32_t xbox_va);

/* Once a frame, at its end (host time, ns): a movie that has stopped showing pictures goes to the log. */
void ngl_movie_frame_end(int64_t now);
