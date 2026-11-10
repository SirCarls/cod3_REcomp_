/* [acct2-bro] Big Red One port: the picture each frame ends with, through ngl_vk. See ngl_present.c. */
#pragma once

/* What nglEndFrame's Present does: draws this frame's picture. Called once a frame, before the frame interval's wait. */
void ngl_present_frame(void);
