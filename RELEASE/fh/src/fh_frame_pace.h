/* [acct2-fh] Finest Hour port: each frame held to the presentation the game asked for. See fh_frame_pace.c. */
#pragma once

/* Called by Swap (fh_present.c) once its frame is shown: waits until the frame's time, as the Xbox's Swap waited for
 * its vertical blank, letting the game's other threads run meanwhile. */
void fh_frame_pace(void);
