/* [acct2-fh] 2026-11-13 Finest Hour port: guest memory words watched from frame to frame (FH_WATCH). */
#ifndef FH_WATCH_H
#define FH_WATCH_H

/* Called at each Swap with the frame number (Swaps so far): prints each watched word that changed. */
void fh_watch_frame(unsigned long frame);

#endif
