/* [acct2-fh] 2026-11-14 The game's own debug values, by name, for the console (fh_console.c): the items of the
 * developers' debug menu still in Finest Hour's program (22 on its own pages, 4 on its performance page) and its 49
 * performance graphs. See fh_game_vars.c and dev/agents/acct2-fh/CONSOLE.md (phase 2).
 *
 * Every value is a 32-bit word in the game's memory: a switch (0 or 1), a whole number or a decimal, as the menu's
 * own code treats it. They are read and written on the game's thread (the console's), between frames. */
#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

enum fh_game_var_kind { FH_GAME_VAR_SWITCH, FH_GAME_VAR_INT, FH_GAME_VAR_FLOAT };

/* How many there are, and each one's name ("debug.disable_sky_box", "graph.total_logic"), kind and a line about it. */
int fh_game_var_count(void);
const char *fh_game_var_name(int index);
enum fh_game_var_kind fh_game_var_kind(int index);
const char *fh_game_var_help(int index);

/* The one called name (case aside), or -1. */
int fh_game_var_find(const char *name);

/* Its value as text ("1", "37", "0.5"), into out. */
void fh_game_var_get(int index, char *out, int size);

/* Sets it from text: a switch takes 1/0, true/false, on/off, yes/no. 0, or -1 when the text isn't one of its kind. */
int fh_game_var_set(int index, const char *text);

/* The game's debug menu itself: 1 while it is open. Opening it shows its first page (0, or -1 while the game hasn't
 * made the menu yet); closing it is what the menu's own Back does at its first page. */
int fh_game_debug_menu_open(void);
int fh_game_debug_menu_show(int open);

/* The menu's own Back (0x0011A205): to the page above, or at the first page shut. 1 when it went up a page, 0 when it
 * shut, -1 while the menu isn't open. */
int fh_game_debug_menu_back(void);

/* The game's seven cheats, its Extras screen's (phase 3). Each is one bit of the word the game's play code reads
 * through its pointer at 0x003B4848, which is the profile's +0x2C once the game has a profile; the profile's +0x30
 * has the same bits for "unlocked" (the Extras screen shows only those). The cheats' order and number (1-7) are the
 * bits' (the Extras screen's mcCheat1-mcCheat7); the names are the ones the cheat pages give, matched by what each
 * bit does in the game's code. */
int fh_game_cheat_count(void);
const char *fh_game_cheat_name(int index);       /* "big_head" */
const char *fh_game_cheat_help(int index);

/* A cheat by its name (case and '_' aside: "bighead", "Big_Head") or its number "1"-"7", or -1. */
int fh_game_cheat_find(const char *text);

/* Bit 0: on. Bit 1: unlocked on the profile. -1 while the game hasn't set its cheat pointer yet. */
int fh_game_cheat_state(int index);

/* Turns one on or off, at once. Turning one on also unlocks it on the profile, so the Extras screen shows it and can
 * turn it off again (the game saves both words with the profile). 0, or -1 while the game hasn't set its pointer. */
int fh_game_cheat_set(int index, int on);

/* Unlocks all seven on the profile, the same as the game's own pad code on its menus (or dword [+0x30], 0x7F). 0, or
 * -1 while the game has no profile. */
int fh_game_cheats_unlock(void);

/* The level select's levels: the furthest level the profile has reached (its +0x54, 0 to 38; the level select shows
 * the levels up to it). -1 while the game has no profile. */
int fh_game_levels_reached(void);

/* Every level open at the level select: the profile's furthest level set to the last (38), as playing them all does
 * (the game saves it with the profile). 0, or -1 while the game has no profile. */
int fh_game_levels_unlock(void);

/* The three campaigns marked done on the profile (+0x30 bits 12-14), which the Extras screen's three "Making of"
 * films wait for. 0, or -1 while the game has no profile. */
int fh_game_campaigns_unlock(void);

/* The player, and the game's own "the player takes no damage" switch (the level scripts' DisablePlayerDamage; phase
 * 4). fh_game_player: the player's object (0 while there is none: the front end, a level's load). god_get: 1 or 0,
 * -1 with no player. god_set: 0, -1 with no player. The game clears the switch when it makes the player at a level's
 * start, so the console keeps it on through loads (fh_console_frame). */
unsigned fh_game_player(void);
int fh_game_god_get(void);
int fh_game_god_set(int on);

/* The debug menu's Unlimited Ammo (phase 4): while it is set the player's magazine never runs down, so there is no
 * reload. The game clears it when it makes the player, so the console keeps it on through loads too. */
int fh_game_ammo_get(void);
void fh_game_ammo_set(int on);

/* Where the player is, read only (phase 4, for pos): its position from its RenderWare frame's world matrix, the
 * address of its class's function table, the weapon number in hand and that gun's rounds in the magazine (-1 when
 * there is none). 0 with them filled, -1 with no player (or no frame yet). */
int fh_game_player_where(float pos[3], unsigned *class_table, int *weapon, int *magazine);

/* [acct2-fh] 11-15: the player's health left (1.0 full, the game's own float) and hit points (what a hit's damage is
 * divided by), read only, for pos. 0 with them filled, -1 with no player. */
int fh_game_player_health(float *health, int *hit_points);

/* [acct2-fh] 11-15: a hit of this much damage on the player through the player's own "take a hit" (0x000C3620), made
 * as the game's own "hurt a group" (0x001164E0) makes one, so god, the difficulty and a death are the game's. Call it
 * on the game's thread only (the console runs there). 0 done (the health, 1.0 full, before and after), -1 with no
 * player, -2 when the function isn't in the port. */
int fh_game_player_hurt(float damage, float *before, float *after);

/* [acct2-fh] 11-15: why the player's "take a hit" would take no health now, as bits, in the order it checks them:
 * god; just after a hit from an attacker (*after_hit, the time left, above 0); the game's byte that stops the loss
 * (0x003EA960); the thing at the player's +0x210C (the camera it shakes?) with +0x414 1. 0: none, or no player. */
#define FH_HURT_GOD       1u
#define FH_HURT_AFTER_HIT 2u
#define FH_HURT_NO_LOSS   4u
#define FH_HURT_CAMERA    8u
unsigned fh_game_player_hurt_gates(float *after_hit);

/* [acct2-fh] 11-15: the difficulty (0x00328664, 0-2) and the scale a hit's damage on the player gets for it, read
 * only. 0 with them, -1 when the word is outside 0-2. */
int fh_game_difficulty(int *level, float *scale);

/* [acct2-fh] 11-15: the difficulty a level starts with (the game's settings; -1 with none yet), and the damage scale
 * the game gives a hit on the player at each of the three difficulties (scales[0-2]). */
int fh_game_difficulty_setting(float scales[3]);

/* [acct2-fh] 11-15: sets the difficulty (0-2) as the level select does and for the level being played. Call it on the
 * game's thread only. 0 done, -1 out of 0-2, -2 when the game's function isn't in the port, -3 with no settings yet. */
int fh_game_difficulty_set(int level);

/* Moves the player to a world position (phase 4, for teleport), by the player's class's own "set the position"
 * (0x000C45A0), as the level scripts' TeleportToNamedObject does: the body goes with it and the player is stopped.
 * Call it on the game's thread only (the console runs there). The world position (what pos shows) catches up at the
 * game's next update. 0 done, -1 with no player (or no frame yet), -2 when the function isn't in the port. */
int fh_game_player_teleport(const float to[3]);

/* The player's frame's world matrix's "at" and "right" axes (phase 4, for noclip): the way the player faces and its
 * right hand, as the game last worked them out. 0 with them filled, -1 with no player (or no frame yet). */
int fh_game_player_axes(float at[3], float right[3]);

/* The player's weapons (phase 4, for give): the two carried (weapon numbers), the one in hand, and which numbers the
 * level loaded (bit n of loaded[n / 32], by the game's own IsPlayerWeaponStreamed). On the game's thread only (it
 * calls the game's code). 0 with them filled, -1 with no player. */
int fh_game_player_weapons(int carried[2], int *in_hand, unsigned loaded[2]);

/* Puts a loaded weapon in the player's hand at once, by the player's own "carry this weapon" (0x000C7190), as the
 * level scripts' SetPlayerWeapon does (the weapon in hand gives its slot); a weapon the game doesn't call loaded is
 * refused. On the game's thread only. 0 done, -1 with no player, -2 when the level didn't load that weapon (or the
 * number is out of range), -3 when a function isn't in the port. */
int fh_game_player_give(int weapon);

/* The weapon number the game treats as no gun (32): give next passes over it. */
int fh_game_weapon_no_gun(void);

/* [acct2-fh] 11-15: a weapon's name from the game's own list of the 39 (0x00321820: "Nagant Sniper", "T34 Cannon",
 * "Mosin Ammo Clip"...). 0 done, -1 out of range or none. */
int fh_game_weapon_name(int weapon, char *out, size_t size);

/* [acct2-fh] 11-15: the damage one bullet of a weapon does, from its data (0x003BE348 + 0x550 a number, +0xDC), before
 * the difficulty's scale; 0 for a weapon this level's tuning didn't fill or that fires no bullets. 0 done, -1 out of
 * range. */
int fh_game_weapon_damage(int weapon, float *damage);

/* [acct2-fh] 11-15: the take-a-hit watch (the console's hitcalls; the frame-rate audit). While it is on, every call
 * of the player's class's "take a hit" (0x000C3620) is noted, then run as the game's own, unchanged: one note a call,
 * read in order with fh_game_hit_next (the game's thread does both). recomp_lookup_manual() asks
 * fh_game_hit_watch_lookup first on every call through a pointer: the watcher for 0x000C3620 while on (and for the
 * projectile step 0x00103850, to count the rounds made), else none. */
struct fh_game_hit {
    unsigned from;          /* the call's address in the game's code (one of its 11 calls through the table's +0x60) */
    unsigned taker;         /* the thing hit (the player, or another thing of its class) */
    int player;             /* 1 when the thing hit is the player */
    unsigned kind;          /* the block's first dword: the word +0 (low half) and the word +2 (high half) */
    unsigned what;          /* the block's +0x08 (0x201 or 0x202 from a gun's bullet) */
    unsigned by;            /* the block's +0x18: the shooter (0 for none) */
    unsigned by_class;      /* the shooter's class table (its first word; 0 for none) */
    float damage;           /* the block's +0x2C: the damage, before the difficulty's scale */
    float damage_28;        /* the block's +0x28 (a bullet's has the damage in both) */
    float before, after;    /* the health of the thing hit, before and after (1.0 full) */
    unsigned source;        /* the block's +0x04: what dealt it (from 0x00103D44, the projectile that hit) */
    float source_born;      /* the source's +0x1C, +0x20 and the length of +0x10..+0x18: for a projectile its */
    float source_life;      /* start (the game's time, 60ths), how long it may fly (60ths) and its speed (units a */
    float source_speed;     /* step of 1.0); all 0 when +0x04 isn't an address in the guest's memory */
    unsigned source_class;  /* the source's class table (its first word) and its +0x3C (a projectile's damage */
    unsigned source_record; /* record: +4 the damage) */
    double game_time;       /* the game's time, seconds (0x003B4788 / 60) */
    unsigned frame;         /* the game's frame count (0x003B4798) */
};
typedef void (*fh_game_func_t)(void);   /* a function of the game's (the generated code's recomp_func_t) */
fh_game_func_t fh_game_hit_watch_lookup(unsigned xbox_va);
void fh_game_hit_watch(int on);         /* on or off; either way the notes not read yet are dropped */
/* [acct2-fh] 11-15: the rounds the watch saw made since the last call (and the count starts again): each kind (a
 * damage, from the round's record, and a class table) with how many; -1 damage when the record isn't readable. Since
 * build64 also how far each kind started from the player (the round's position when it first steps, to the player's
 * world position then): the sum and the nearest, over placed of them (0 when there was no player). */
struct fh_game_shots {
    unsigned kinds;
    struct {
        float damage;
        unsigned class_table;
        unsigned record;
        unsigned made;
        unsigned placed;    /* of made, those with a player to measure from */
        float distance_sum, distance_least;
    } kind[8];
    unsigned other;         /* rounds of a ninth kind or more */
};
void fh_game_shots_take(struct fh_game_shots *shots);

/* [acct2-fh] 11-15 (build64): the launches the watch saw since the last call (and the count starts again): a
 * projectile's five "launch" functions (0x00102970, 0x00102D30, 0x00102E50, 0x00102EF0, 0x00102FF0, each only called
 * through a pointer, each passing its third argument to 0x00102290 as the round's damage record), by the function,
 * where it returns to (the code that fired) and the record; -1 damage when the record isn't readable. Since build65
 * also the set-up 0x00102290 itself when it is called through its class's table (0x002E7E70's first entry), and
 * for a launch from the projectile factory, the code that fired (where "fire a round" 0x00102220 returns to). */
#define FH_GAME_LAUNCH_SITES 12
struct fh_game_launches {
    unsigned sites;
    struct {
        unsigned launch;
        unsigned back;      /* where the launch function returns to */
        unsigned from;      /* when that is the projectile factory 0x00101FA0 (0x001021AE): the code that fired
                             * (where 0x00102220, the factory's one caller, returns to); else 0 */
        unsigned record;
        float damage;
        unsigned made;
    } site[FH_GAME_LAUNCH_SITES];
    unsigned other;         /* launches from a thirteenth site or more */
};
void fh_game_launches_take(struct fh_game_launches *launches);

/* The oldest note not read yet: 1 with it, 0 with none. *dropped: the notes overwritten before they were read since
 * the last call (64 are kept). */
int fh_game_hit_next(struct fh_game_hit *hit, unsigned *dropped);

/* Levels (phase 4, for map): the last level's number (38); a level's name as the game's data has it ("L_1_1",
 * "L_2_3"...: 0 done, -1 out of range); the level being played (-1 with no player); and a jump to another, as the
 * level scripts' QuitAndLoadNextLevel makes it (the level fades out and the new one loads; the profile's campaigns
 * are left as they were). goto: 0 started, -1 with no player (in a level only), -2 not a level number, -3 the
 * function isn't in the port, -4 a level change is already under way. On the game's thread only. */
int fh_game_level_most(void);
int fh_game_level_name(int level, char *out, size_t size);
int fh_game_level_now(void);
int fh_game_level_goto(int level);

#ifdef __cplusplus
}
#endif
