/* [f2-menus] Big Red One's own cvars (its "dvars"), for the F2 window: read from the game's memory, changed through
 * the game's own Cvar_Set2, and the player's choices kept in bro.toml's [cvars] and put back when the game resets them.
 *
 * Every fact (the cvar array, its record, the flags, Cvar_Set2's and Cbuf_AddText's calls) is in
 * dev/agents/f2-menus/bro/DVARS.md, read from the campaign program (default.xbe) with dev/agents/f2-menus/tools/bro_cvars.py
 * and objdump. The multiplayer program's addresses are listed there too; this port runs the campaign.
 *
 * Threads: every call on the game's thread (the port draws on it too: ngl_present_frame, where the F2 window is drawn,
 * and ngl_frame_clock's frame end, where the changes go in). */
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The cvar flags, as the game's Cvar_Get and Cvar_Set2 test them (Quake III's values). */
#define BRO_CVAR_ARCHIVE      0x0001u   /* kept in the player's profile */
#define BRO_CVAR_USERINFO     0x0002u
#define BRO_CVAR_SERVERINFO   0x0004u
#define BRO_CVAR_SYSTEMINFO   0x0008u
#define BRO_CVAR_INIT         0x0010u   /* only at start: read-only here */
#define BRO_CVAR_LATCH        0x0020u   /* a change waits for the next level or start */
#define BRO_CVAR_ROM          0x0040u   /* the game's own: read-only here */
#define BRO_CVAR_USER_CREATED 0x0080u
#define BRO_CVAR_TEMP         0x0100u
#define BRO_CVAR_CHEAT        0x0200u   /* put back to its default when a level registers it (sv_cheats 0) */

struct bro_cvar {
    const char *name;
    const char *value;          /* its text */
    const char *reset;          /* its default */
    const char *latched;        /* a value waiting for the next restart, or NULL */
    uint32_t flags;
    int modification_count;
    float number;               /* the value as the game read it */
};

/* How many cvars the game has made so far (0 before it starts). */
int bro_game_cvars_count(void);
/* The index-th of them (0 .. count-1). 1 when *out was filled. The texts are the game's, valid until the game changes
 * that cvar (copy them to keep them). */
int bro_game_cvar_at(int index, struct bro_cvar *out);
/* By name, case-insensitive, as the game finds them. 1 when found. */
int bro_game_cvar_find(const char *name, struct bro_cvar *out);
/* 1 when the window must not change it (ROM or INIT). */
int bro_game_cvar_read_only(uint32_t flags);

/* A change, made at the end of this frame through the game's Cvar_Set2 (a CHEAT cvar with force, so it takes it
 * without sv_cheats; a LATCH cvar waits for the next level as in the game). The value is also kept: bro.toml's
 * [cvars] gets it at the next save, and it goes back in at every start and whenever the game resets it. */
void bro_game_cvar_set(const char *name, const char *value);
/* The kept value of a cvar (bro.toml's [cvars], or a change this run), or NULL. */
const char *bro_game_cvar_kept(const char *name);

/* A command line for the game's command buffer (Cbuf_AddText), run at its next frame: "set name value", "reset name",
 * "toggle name", "cvarlist", ... A newline is added. */
void bro_game_command(const char *text);

/* A 32-bit word of the game's memory, for test runs (the F2 window's BRO_F2_TEST watch / diff steps, which find where
 * the game keeps what a command changes); 0 outside the game's first 64 MB. */
uint32_t bro_game_peek32(uint32_t address);

/* 1 once after the game itself changed a kept cvar to another value of its own (its menu, the player's profile):
 * the kept value follows it, and the F2 window marks itself unsaved. */
int bro_game_cvars_take_adopted(void);

/* Once a frame at the frame's end (ngl_frame_clock.c, beside bro_view_apply): the queued commands and changes go
 * into the game, and the kept values are put back where the game reset them. */
void bro_game_cvars_frame(void);

#ifdef __cplusplus
}
#endif
