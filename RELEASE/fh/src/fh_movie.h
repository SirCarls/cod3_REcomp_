/* [acct2-fh] 2026-11-13: Finest Hour's films played by FFmpeg instead of the game's own Bink library.
 *
 * The game plays its films (the start-up logos, the intro, the front end's loop, the acts' briefings) through eleven
 * entry points of the Bink library linked into default.xbe. Each is a stand-in here (fh/config/stand_in_functions.tsv,
 * group "bink"), answered by fh_movie.c with FFmpeg's Bink decoders: the picture goes into the surface the game hands
 * to BinkCopyToBuffer, the sound to SDL3. See fh_movie.c for what each function does and how it was read from the
 * game's code. Arguments and results are guest values (addresses in the Xbox's memory), as the stand-ins pass them. */
#ifndef FH_MOVIE_H
#define FH_MOVIE_H

#include <stdint.h>

uint32_t fh_movie_set_sound_track(uint32_t total_tracks, uint32_t tracks);         /* 0x00251A80 */
uint32_t fh_movie_open(uint32_t name, uint32_t flags);                             /* 0x00251CC0 */
uint32_t fh_movie_get_error(void);                                                 /* 0x00251610 */
uint32_t fh_movie_wait(uint32_t bink);                                             /* 0x00253500 */
uint32_t fh_movie_pause(uint32_t bink, uint32_t pause);                            /* 0x00253700 */
uint32_t fh_movie_set_volume(uint32_t bink, uint32_t track_id, uint32_t volume);   /* 0x00253800 */
uint32_t fh_movie_get_track_id(uint32_t bink, uint32_t track_index);               /* 0x00253850 */
uint32_t fh_movie_copy_to_buffer(uint32_t bink, uint32_t dest, uint32_t pitch, uint32_t height, uint32_t x,
                                 uint32_t y, uint32_t flags);                      /* 0x002539B0 */
uint32_t fh_movie_do_frame(uint32_t bink);                                         /* 0x002539F0 */
uint32_t fh_movie_next_frame(uint32_t bink);                                       /* 0x00253FD0 */
uint32_t fh_movie_close(uint32_t bink);                                            /* 0x00254170 */

#endif
