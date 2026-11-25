/* [acct2-fh] 2026-11-13 Finest Hour port: where the game's main thread spends its frames (fh_profile.c). */
#ifndef FH_PROFILE_H
#define FH_PROFILE_H

/* Called by Swap once a frame, on the game's main thread. With FH_PROFILE set, the first call starts sampling that
 * thread; later calls write the samples taken since the last write, a second's worth at a time. Without FH_PROFILE
 * it only counts the frame. */
void fh_profile_frame(void);

#endif
