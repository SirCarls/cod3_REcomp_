/* [acct2-bro-game] Big Red One port: the view's settings from bro.toml (display.fov). See bro_view.c. */
#pragma once

/* Once a frame, before the next frame's view is made: puts the player's field of view into the game's. Does nothing
 * when display.fov isn't given. */
void bro_view_apply(void);
