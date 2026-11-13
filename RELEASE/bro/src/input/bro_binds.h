/* [acct2-bro] Big Red One port: the keyboard's and the mouse's binds, as CoD3's (bro_binds.c). */
#pragma once

#include <stdint.h>

/* Whether the player is in play: the game makes commands from the controls (cl.mouseIndex flips) and no menu or the
 * console has the keys. Called once a poll before the others. */
int bro_binds_in_play(void);

/* With bro_binds_in_play: whether a key of the controller layer (an SDL scancode, or 1000 + a mouse button: bro_pad.c's
 * codes) has an action, so the controller's button behind it isn't pressed as well. */
int bro_binds_claims(int pad_code);

/* Once a poll, from the game's own input code (bro_pad.c, XInputGetState): the keys' presses and releases become the
 * game's commands. keys: SDL's keyboard state (NULL: none); mouse: SDL's mouse buttons; focused: the window has the
 * keyboard. Also BRO_BINDS_TEST. */
void bro_binds_poll(const void *keys, uint32_t mouse, int focused);

/* Whether the aim action's keys are held now (the mouse's zoom sensitivity). */
int bro_binds_aim_held(void);

/* [f2-menus] 2026-11-11 Read the binds' settings again at the next poll (bro_pad.c, on the game's thread). */
void bro_binds_reload(void);
