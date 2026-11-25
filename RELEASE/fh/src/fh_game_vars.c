/* [acct2-fh] 2026-11-14 The game's own debug values, by name (fh_game_vars.h).
 *
 * Read from the game's code (Europe v3 default.xbe; dev/agents/acct2-fh/CONSOLE.md has the reading):
 *
 *   The debug menu. 0x000A2390 (at 0x000A253A) runs 0x0011A790 on the menu object at 0x003E3CF8. That makes the menu's
 *   first page the item at 0x00322B4C, with nothing chosen and the menu shut (+0x14 = 0), then its pages with
 *   0x00119C40 (Renderer Switching, Game Settings, Cheats, Debug Switches) and 22 items on them with 0x0011A550(page,
 *   label, the value's address, kind). An item's kind is its first word: the menu's code (0x00119F00, run each frame by
 *   the main loop from 0x000A2FEA) jumps on it through the table at 0x0011A324. Kind 0 is a page (the pad's select,
 *   channel 0x27, opens it), 1 a whole number (the pad steps it, select sets 0), 2 a switch (select flips the word's
 *   lowest bit: xor dword [value], 1), 4 and 7 decimals (floats the pad steps; select sets a 7 to 0). Every value is a
 *   32-bit word.
 *
 *   The performance page. 0x0011A790 first runs 0x0011B680 on the object at 0x00323AE8. That adds a page
 *   "Performance Statistics" with "Performance Graphs" in it, a page there for each group of graphs, and a switch for
 *   each of 49 graphs: records 0x4C bytes apart from 0x00322C10 to the -1 at 0x00323A9C, each with its group (+0),
 *   its name (+4) and its switch (+0x40). Then four items of its own on Performance Statistics: Performance Text (a
 *   switch, 0x00323AE8), FPS: Graph Resolution (kind 4, 0x00323AEC), Reset All Graphs and Display Highest 4
 *   (switches, 0x00323AF0 and 0x00323AF4). 0x0011AA10, run while a frame is drawn (from 0x000DCEB8), draws them;
 *   0x0011AE80 prints a graph's numbers ("%3.1f"). The Cutting Room Floor's Finest Hour page lists the same 49 for the
 *   USA/Europe disc, 0x140 lower, and says a graph switched on draws a box on the left with the graph and the frame
 *   rate under it, and that the game clears them while a level loads. The graphs' names here come from the records.
 *
 *   Open and shut. 0x00119F00 does nothing in a game's first 60 frames (0x003B4798) or while +0x14 is 0, and nothing
 *   in the retail game sets +0x14 (before that test the code reads the pad's channel 0x0F and drops it: most likely
 *   what opened the menu once). +0x0C is the page shown and +0x10 the item chosen; a page's first item is at its +0x08
 *   (kind 0's code sets both so). Back on the first page (no page above it, its +0x10) clears +0x14 at 0x0011A314.
 *   0x0011A510 takes the menu down (from 0x000A2750, at 0x000A2934) and sets the first page's +0x08 to 0, so the menu
 *   is only opened while that is set. 0x000A2750 and 0x000A2390 are run as a pair (0x000A2C9F and 0x000A2C2F,
 *   0x000E2BCB and 0x000E2BD2): the menu is taken down and made again, shut, its values where they were.
 *
 *   The cheats. The player's profile is the object at [0x003EA958]: for cheat n (1-7) bit n-1 of its +0x30 says it is
 *   unlocked and the same bit of its +0x2C that it is on (0x000B0030 fills the Extras screen from them; the save code,
 *   0x000A4B20, keeps both). Play doesn't read the profile: 0x000A5D20 (from the main loop, 0x000A4940) points
 *   0x003B4848 at the profile's +0x2C (before there is one, at the word 0x003B484C), and 14 places read the word
 *   there. What each bit does there, as read:
 *     0x01  0x000EAB7E, 0x00114DD8 (two kinds of soldier): a part's scale is 2.0 where it is 1.0 (+0x7FC) otherwise
 *     0x02  0x00103DC4: when the shooter is the player, a bullet bounces (a count at +0x2C, at most 2)
 *     0x04  0x000E7A34, 0x00113286 (the damage of a hit): a hit on part 0 does 1000, any other 0 (0x002DB7D4)
 *     0x08  0x00103DCE: the same bounce for everyone's bullets
 *     0x10  0x000E7862, 0x001130C3: a soldier's size is 0.33 + 0.67 x his health left (+0x1D20 / +0x1D24)
 *     0x20  0x000E7A1B, 0x00113272: every hit does 1000 (0x002DBCF8), before the 0x04 test
 *     0x40  0x000EA157, 0x001144E0: a soldier's size is set from the game's random number (0x00202BF0)
 *   The cheat pages give Easy's four as Big Head, Head Shot, Ricochet and Ricochet All, Normal's two as Dead Eye and
 *   Shrink Shot, and Hard's as Big 'N' Small; 0x000A3E70 unlocks bits 0-3 on Easy, 4-5 on Normal and 6 on Hard, so
 *   the names below are those, matched to the bits by what the code does.
 *
 *   The level select. 0x000A3720, at a level's end, raises the profile's +0x54 to the level just reached (0x00129280),
 *   and the level select copies +0x54 when it opens (0x000B1157, its "enteringLevelSelect" event) and shows the
 *   levels up to it. Its own unlock (the "checkUnlockSPLvlsMBS" event, 0x000B12EE: the four presses at 0x002E5344
 *   while a second pad holds up) sets only that screen's copy, to 38 (0x26, 0x000B084F), for that visit. Here +0x54
 *   itself is set, so it lasts and is saved, as it would be with every level played. +0x30's bits 12-14 are the
 *   three campaigns done (0x000A42F0 sets 0x1000 and 0x2000, 0x000A3E70 0x4000); the Extras screen's "Making of"
 *   films wait for them (0x000AFF50, 0x000B0D50).
 *
 *   The player and god mode. The level scripts' commands are a table of name and function (from 0x003203E0 on:
 *   "DisablePlayerDamage" 0x000563F0, "SetPlayerHealthPercentage" 0x000563A0, "GetPlayerEntity" 0x000523A0, ...).
 *   Each gets the player with 0x0010F590(0): the handle at 0x003E266C + 4 x n, looked up by 0x000A0E80 in the handle
 *   table at 0x003B2B58 (slot = the handle's low 16 bits, signed; 8 bytes a slot; the object only when the slot's
 *   handle is the same handle). DisablePlayerDamage writes its argument to the player's +0x19FC; 0x000C3620 skips all
 *   its work while that is set, and 0x000CBAC0 answers 0 (the player's two functions that read it), and 0x000C4B90
 *   clears it (0x000C4F5E) when the player is made.
 *
 *   Unlimited ammo. The debug menu's Cheats > Unlimited Ammo (0x003BE334) still works: where a shot of the player's
 *   gun takes a round from the magazine (+0x414: 0x000D3499 in 0x000D30C0, 0x000D4AAF in 0x000D4040) and where the
 *   empty magazine starts a reload (+0x48 = 1: 0x000D3128, 0x000D4107), the code first checks that the gun is the
 *   player's ([[gun+0x448]+0xCC]) and that the word is 0. So with it set the magazine never runs down, and there is
 *   nothing to reload. 0x000C4B90 sets it to 0 (0x000C576A) when it makes the player, so it is set again after.
 *
 *   Moving the player. The level scripts' TeleportToNamedObject (0x00046630) moves a thing by its class's function
 *   at +0xBC with the new position (and turns it by +0xD4). The player's class (0x002E61B8) has 0x000C45A0 there:
 *   thiscall, one argument (the position's three floats), ret 4. It puts the position in the frame's own matrix
 *   (frame+0x40; the world matrix at +0x50 follows when 0x001BF690 marks the frame, which it does), moves the
 *   player's body (+0x284: +0xB0 and +0xC0+0x30) by the same distance, keeps the new and the old position (+0x434,
 *   +0x440), adds the height moved to +0x420 (so a teleport up isn't a fall) and stops the player (+0xCC..+0xD4 =
 *   0). That is all a teleport needs, so fh_game_player_teleport calls it as the script does. Its position is the
 *   frame's own (in its parent's space), so the port passes the frame's own position moved by the distance from the
 *   world position to the place asked for: the same when the frame has no parent, and right with a parent that only
 *   moves. Y is up (the height at +0x420 is the position's second float).
 *
 *   Weapons. The level scripts' SetPlayerWeapon, GivePlayerAmmo and RemovePlayerWeapon are one handler (0x000386F0),
 *   which calls 0x000C7190 on the player with the script's three numbers: the weapon, "now" and the slot. The player
 *   carries two (+0x1B2C, +0x1B30; +0x1AE8 says which is in hand, +0x1AE4 the weapon in hand). The slot number 0 is
 *   the one in hand, 1 the other. With "now" 1 the weapon goes into that slot and into the hand at once (the one in
 *   hand put away: 0x000D16D0, the new one taken up by its object's function +8 and 0x000D14D0); with 0 it only goes
 *   into the slot. Every weapon number has an object at +0x1A48 + 4 x the number, loaded or not (boot67), so the
 *   game's own "is it loaded" (0x000C2DE0, the scripts' IsPlayerWeaponStreamed) decides: given one the level hadn't
 *   loaded, 0x000C7190 called through a bad address and the game crashed (boot67, weapon 0).
 *
 *   Levels. The level scripts' QuitAndLoadNextLevel (0x0004C320) passes its number to 0x000A42F0, which does nothing
 *   while a level change is under way (0x003B46D0 bit 0x80), else keeps the level left (0x003B4730), marks the
 *   first or second campaign done on the profile when that level is 12 or 18, sets the next level (0x00129250),
 *   sets bit 0x80 and starts the fade whose end loads it (0x000DA9E0, 0x000DAB00 with 0x000A3F10). The levels'
 *   names are pointers at 0x00328668 (0 "L_NULL", 1 "L_1_1" to 38 "L_3_5_C"); 0x00129280 says which is played. */
#include "fh_game_vars.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "recomp_types.h"           /* MEM32: guest memory; g_esp, g_ecx for the take-a-hit watch */
#include "fh_frame_clock.h"         /* the game's time and frame count, for the take-a-hit watch */

#define MENU            0x003E3CF8u /* the debug menu object */
#define MENU_FIRST_PAGE 0x00322B4Cu /* its first page (an item in the game's data) */
#define MENU_PAGE       0x0Cu       /* the menu's: the page shown */
#define MENU_ITEM       0x10u       /* the item chosen */
#define MENU_SHOWN      0x14u       /* open while this is set */
#define PAGE_FIRST_ITEM 0x08u       /* a page's */
#define GRAPHS          0x00322C10u /* the performance graphs' records */
#define GRAPH_SIZE      0x4Cu
#define GRAPH_NAME      0x04u
#define GRAPH_SWITCH    0x40u
#define GRAPHS_MOST     64
#define CHEATS_ON       0x003B4848u /* the game's pointer to the cheat word play reads */
#define PROFILE         0x003EA958u /* the player's profile (0 before there is one) */
#define PROFILE_ON      0x2Cu       /* the profile's: the cheats that are on */
#define PROFILE_UNLOCKED 0x30u      /* the cheats unlocked (and, from bit 12, the campaigns done) */
#define CHEATS_ALL      0x7Fu
#define CAMPAIGNS_DONE  0x7000u     /* +0x30's bits 12-14: the three campaigns done */
#define PROFILE_FURTHEST 0x54u      /* the profile's: the furthest level reached */
#define LAST_LEVEL      38          /* 0x26: 0x000A3E70's last level, and the level select's "all" (0x000B084F) */
#define PAGE_ABOVE      0x10u       /* a page's: the page it is on (0 at the first) */
#define PLAYER_HANDLES  0x003E266Cu /* the players' handles, one word each (0x0010F590 reads them) */
#define HANDLE_TABLE    0x003B2B58u /* the game's handle table: per slot its handle (+0) and object (+4) */
#define PLAYER_NO_DAMAGE 0x19FCu    /* the player's: no damage while set (DisablePlayerDamage) */
#define PLAYER_HEALTH   0x19DCu     /* the player's health left, a float, 1.0 full (0x000C3620; class +0x238 sets it) */
#define PLAYER_HIT_POINTS 0x19E0u   /* the player's hit points, an int: a hit takes its damage / this from the health */
#define UNLIMITED_AMMO  0x003BE334u /* the debug menu's Unlimited Ammo: the player's magazine never runs down */
#define PLAYER_FRAME    0x340u      /* the player's RenderWare frame (0x0009B5C0, its position getter, reads it) */
#define FRAME_LTM_POS   0x80u       /* a frame's world matrix is at +0x50 (0x001BF6F0 returns it), its position +0x30 */
#define PLAYER_GUNS     0x1A48u     /* the player's weapon objects, one word a weapon number (0x000C7190) */
#define PLAYER_IN_HAND  0x1AE4u     /* the weapon number in hand */
#define PLAYER_GUN_MOST 39          /* the words from +0x1A48 up to +0x1AE4 */
#define GUN_MAGAZINE    0x414u      /* a gun's rounds in the magazine (0x000D3499 takes one a shot) */
#define FRAME_OWN_POS   0x40u       /* a frame's own matrix is at +0x10 (in its parent's space), its position +0x30 */
#define FRAME_LTM_RIGHT 0x50u       /* the world matrix's first axis (RwMatrix: right +0x00, up +0x10, at +0x20) */
#define FRAME_LTM_AT    0x70u       /* its third axis, the way the player faces */
#define CLASS_SET_POSITION 0xBCu    /* a thing's class: its "set the position" (0x000C45A0 for the player's class) */
#define CLASS_TAKE_HIT  0x60u       /* a thing's class: its "take a hit" (0x000C3620 for the player's), ret 4 */
#define TAKE_HIT_RETURN 0x00116546u /* where 0x001164E0, the game's own "hurt a group", comes back to after it */
#define HIT_SIZE        0x30u       /* the hit record 0x001164E0 makes on its stack, zeroed first */
#define HIT_KIND_WORD   0x00u       /* the word whose top half is +2, the hit's kind (0x001164E0's is 2) */
#define HIT_KIND_GROUP  2u
#define HIT_DAMAGE_TOO  0x28u       /* 0x001164E0 puts the damage here as well (0x000C3620 doesn't read it) */
#define HIT_DAMAGE      0x2Cu       /* the damage, a float: 0x000C3620 scales it by the difficulty's (0x000BF9B0) */
#define PLAYER_AFTER_HIT 0x19ECu    /* a float: above 0 just after a hit from an attacker, when 0x000C3620 takes none */
#define PLAYER_CAMERA   0x210Cu     /* a handle: the thing 0x000C3620 shakes after a hit (0x00110F40), the camera? */
#define CAMERA_NO_LOSS  0x414u      /* that thing's: 1 when 0x000C3620 takes no health (1 in L_1_2's opening?) */
#define NO_HEALTH_LOSS  0x003EA960u /* a byte: while set 0x000C3620 takes no health (its effects still play) */
#define DIFFICULTY      0x00328664u /* the difficulty, 0-2: 31 places read it; 0x00128E60 sets it at a level's start */
#define DIFFICULTY_MOST 3           /* from 0x001292B0 (the settings at [0x003EA958], their +0x24) */
#define DIFFICULTY_DAMAGE 0x003EA94Cu /* per difficulty, a float a hit's damage on the player is multiplied by */
#define DIFFICULTY_SET  0x001292D0u /* the game's own "set the difficulty" (cdecl, one argument): the settings' +0x24 */
#define DIFFICULTY_SET_RETURN 0x000B01B3u /* where the level select's call of it (0x000B01AE) comes back to */
#define SETTINGS        0x003EA958u /* the game's settings: +0x24 the difficulty a level starts with (0x001292B0) */
#define SETTINGS_DIFFICULTY 0x24u
#define SET_POSITION_RETURN 0x0004678Cu /* where TeleportToNamedObject (0x00046630) comes back to after calling it */
#define PLAYER_CARRIED  0x1B2Cu     /* the two weapons carried: their numbers, one word each */
#define PLAYER_CARRIED_IN_HAND 0x1AE8u /* which of the two is in hand (0 or 1) */
#define PLAYER_SCRIPT_WEAPON 0x2134u /* set to 1 by the scripts' weapon command after each change (0x00038740) */
#define SET_WEAPON      0x000C7190u /* the player's "carry this weapon": thiscall (weapon, now, other slot), ret 0xC */
#define SET_WEAPON_RETURN 0x00038740u /* where the scripts' SetPlayerWeapon (0x000386F0) comes back to after it */
#define WEAPON_NO_GUN   32          /* 0x20: the number 0x000C7190 treats apart (in hand at the first level's start) */
#define WEAPON_STREAMED 0x000C2DE0u /* the player's "is this weapon loaded": thiscall (weapon), ret 4; 1 or 0 */
#define WEAPON_STREAMED_RETURN 0x00038649u /* where the scripts' IsPlayerWeaponStreamed (0x00038610) comes back to */
#define LEVEL_GOTO      0x000A42F0u /* QuitAndLoadNextLevel's: end this level and load level n (cdecl, one argument) */
#define LEVEL_GOTO_RETURN 0x0004C37Fu /* where the scripts' QuitAndLoadNextLevel (0x0004C320) comes back to */
#define LEVEL_NOW       0x00129280u /* the level being played (no arguments) */
#define LEVEL_NOW_RETURN 0x000A42FEu /* where 0x000A42F0 comes back to after asking it */
#define WEAPON_NAMES    0x00321820u /* the game's 39 weapons' names, a pointer each by number (nothing refers to it,
                                     * so the numbering is ours; it fits: 32 "Mosin Ammo Clip", L_1_1's no-gun start) */
#define WEAPON_NAME_MOST 32u        /* the longest is 18 ("StickyBear Grenade") */
#define LEVEL_NAMES     0x00328668u /* the levels' names, a pointer each: 0 "L_NULL", then 1 to 38 */
#define LEVEL_CHANGING  0x003B46D0u /* the game's state bits: 0x80 while a level change is under way */
#define LEVEL_CHANGING_BIT 0x80u

struct game_var {
    char name[64];
    unsigned address;
    enum fh_game_var_kind kind;
    char help[256];
};

/* The debug menu's items, as 0x0011A790 and 0x0011B680 make them: the menu's page and label, the value's address and
 * its kind (the menu's 4 and 7 are both a decimal here). */
static const struct {
    const char *name, *page, *label;
    unsigned address;
    enum fh_game_var_kind kind;
} menu_items[] = {
    {"debug.disable_default_clump_renderer", "Renderer Switching", "Disable: DefaultClumpRenderer", 0x003B2B44u,
     FH_GAME_VAR_SWITCH},
    {"debug.disable_gimps", "Renderer Switching", "Disable: Gimps", 0x003E3874u, FH_GAME_VAR_SWITCH},
    {"debug.disable_shrimps", "Renderer Switching", "Disable: Shrimps", 0x003E3B90u, FH_GAME_VAR_SWITCH},
    {"debug.disable_clones", "Renderer Switching", "Disable: Clones", 0x003CBCACu, FH_GAME_VAR_SWITCH},
    {"debug.disable_particles", "Renderer Switching", "Disable: Particles", 0x003BC6D0u, FH_GAME_VAR_SWITCH},
    {"debug.disable_world_opaque", "Renderer Switching", "Disable: World Opaque", 0x003CBDC4u, FH_GAME_VAR_SWITCH},
    {"debug.disable_world_alpha", "Renderer Switching", "Disable: World Alpha", 0x003CBDC8u, FH_GAME_VAR_SWITCH},
    {"debug.disable_world_extra", "Renderer Switching", "Disable: World Extra", 0x003CBDCCu, FH_GAME_VAR_SWITCH},
    {"debug.disable_texture_anims", "Renderer Switching", "Disable: Texture Anims", 0x003CC774u, FH_GAME_VAR_SWITCH},
    {"debug.disable_sky_box", "Renderer Switching", "Disable: Sky Box", 0x003CBDD0u, FH_GAME_VAR_SWITCH},
    {"debug.light_scale_ambient", "Renderer Switching", "Lights: Scale Ambient", 0x003EAF68u, FH_GAME_VAR_FLOAT},
    {"debug.light_scale_directional", "Renderer Switching", "Lights: Scale Directional", 0x003EAF6Cu,
     FH_GAME_VAR_FLOAT},
    {"debug.disable_new_clones", "Renderer Switching", "Disable: New Clones", 0x003CBCC0u, FH_GAME_VAR_SWITCH},
    {"debug.disable_ram_particles", "Renderer Switching", "Disable: RAM Particles", 0x00329780u, FH_GAME_VAR_SWITCH},
    {"debug.disable_prelit_clones", "Renderer Switching", "Disable: Prelit Clones", 0x003CBCB4u, FH_GAME_VAR_SWITCH},
    {"debug.disable_adl_clones", "Renderer Switching", "Disable: ADL Clones", 0x003CBCB8u, FH_GAME_VAR_SWITCH},
    {"debug.game_speed", "Game Settings", "Game Speed (the frame timer's time scale, 1 the normal speed)",
     0x00321680u, FH_GAME_VAR_FLOAT},
    {"debug.unlimited_ammo", "Cheats", "Unlimited Ammo", UNLIMITED_AMMO, FH_GAME_VAR_SWITCH},
    {"debug.switch", "Debug Switches", "int g_debugSwitch", 0x00322B60u, FH_GAME_VAR_SWITCH},
    {"debug.float", "Debug Switches", "int g_debugFloat (a decimal, whatever its label says)", 0x00322B64u,
     FH_GAME_VAR_FLOAT},
    {"debug.int0", "Debug Switches", "int g_debugInt0", 0x003E3CECu, FH_GAME_VAR_INT},
    {"debug.int1", "Debug Switches", "int g_debugInt1", 0x003E3CF0u, FH_GAME_VAR_INT},
    /* 0x0011B680's own items, on its page */
    {"perf.text", "Performance Statistics", "Performance Text", 0x00323AE8u, FH_GAME_VAR_SWITCH},
    {"perf.fps_graph_resolution", "Performance Statistics", "FPS: Graph Resolution", 0x00323AECu, FH_GAME_VAR_FLOAT},
    {"perf.reset_all_graphs", "Performance Statistics", "Reset All Graphs", 0x00323AF0u, FH_GAME_VAR_SWITCH},
    {"perf.display_highest_4", "Performance Statistics", "Display Highest 4", 0x00323AF4u, FH_GAME_VAR_SWITCH},
};
#define MENU_ITEMS ((int)(sizeof menu_items / sizeof menu_items[0]))

static struct game_var vars[MENU_ITEMS + GRAPHS_MOST];
static int var_count = -1;

/* "TOTAL LOGIC" or "ENTITY UPD: CLASSAI" as a console name: "graph.total_logic", "graph.entity_upd_classai". */
static void graph_name(const char *label, char *out, size_t size)
{
    size_t n = (size_t)snprintf(out, size, "graph.");
    int gap = 0;

    for (; *label && n + 1 < size; label++) {
        if (isalnum((unsigned char)*label)) {
            if (gap && out[n - 1] != '.')
                out[n++] = '_';
            out[n++] = (char)tolower((unsigned char)*label);
            gap = 0;
        } else {
            gap = 1;
        }
    }
    out[n] = '\0';
}

static int name_taken(const char *name, int count)
{
    int i;

    for (i = 0; i < count; i++)
        if (strcmp(vars[i].name, name) == 0)
            return 1;
    return 0;
}

/* The table, made the first time it is asked for (the graphs' names are read from the game's memory, which is there
 * by then: the console runs once the game does). */
static void make_table(void)
{
    unsigned record;
    int i;

    if (var_count >= 0)
        return;
    var_count = 0;
    for (i = 0; i < MENU_ITEMS; i++) {
        struct game_var *v = &vars[var_count++];

        snprintf(v->name, sizeof v->name, "%s", menu_items[i].name);
        v->address = menu_items[i].address;
        v->kind = menu_items[i].kind;
        snprintf(v->help, sizeof v->help, "the game's debug menu, %s: \"%s\" (%s at 0x%08X)", menu_items[i].page,
                 menu_items[i].label,
                 v->kind == FH_GAME_VAR_SWITCH ? "a switch" : v->kind == FH_GAME_VAR_INT ? "a whole number"
                                                                                         : "a decimal",
                 v->address);
    }
    for (record = GRAPHS; var_count < MENU_ITEMS + GRAPHS_MOST; record += GRAPH_SIZE) {
        char label[61];
        struct game_var *v;

        if (MEM32(record) == 0xFFFFFFFFu)                       /* the table's end */
            break;
        for (i = 0; i < 60; i++) {
            label[i] = (char)MEM8(record + GRAPH_NAME + (unsigned)i);
            if (!label[i])
                break;
        }
        label[i] = '\0';
        while (i > 0 && label[i - 1] == ' ')                    /* any spaces at its end */
            label[--i] = '\0';
        if (!label[0])
            continue;                                           /* a record with no name: left out */
        v = &vars[var_count];
        graph_name(label, v->name, sizeof v->name);
        if (name_taken(v->name, var_count)) {                   /* ENTITY UPDATE is there twice */
            size_t n = strlen(v->name);

            snprintf(v->name + n, sizeof v->name - n, "_2");
        }
        v->address = record + GRAPH_SWITCH;
        v->kind = FH_GAME_VAR_SWITCH;
        snprintf(v->help, sizeof v->help, "a performance graph, \"%s\": on draws its box on the left with the frame "
                 "rate under it (a level's load turns it off; switch at 0x%08X)", label, v->address);
        var_count++;
    }
}

int fh_game_var_count(void)
{
    make_table();
    return var_count;
}

const char *fh_game_var_name(int index)
{
    make_table();
    return index >= 0 && index < var_count ? vars[index].name : NULL;
}

enum fh_game_var_kind fh_game_var_kind(int index)
{
    make_table();
    return index >= 0 && index < var_count ? vars[index].kind : FH_GAME_VAR_INT;
}

const char *fh_game_var_help(int index)
{
    make_table();
    return index >= 0 && index < var_count ? vars[index].help : NULL;
}

int fh_game_var_find(const char *name)
{
    int i;

    make_table();
    for (i = 0; i < var_count; i++) {
        const char *a = vars[i].name, *b = name;

        while (*a && *b && tolower((unsigned char)*a) == tolower((unsigned char)*b))
            a++, b++;
        if (!*a && !*b)
            return i;
    }
    return -1;
}

static float word_as_float(unsigned word)
{
    union {
        unsigned u;
        float f;
    } x;

    x.u = word;
    return x.f;
}

static unsigned float_as_word(float value)
{
    union {
        unsigned u;
        float f;
    } x;

    x.f = value;
    return x.u;
}

void fh_game_var_get(int index, char *out, int size)
{
    unsigned word;

    make_table();
    if (index < 0 || index >= var_count) {
        snprintf(out, (size_t)size, "?");
        return;
    }
    word = MEM32(vars[index].address);
    if (vars[index].kind == FH_GAME_VAR_FLOAT)
        snprintf(out, (size_t)size, "%g", (double)word_as_float(word));
    else if (vars[index].kind == FH_GAME_VAR_SWITCH)
        snprintf(out, (size_t)size, "%u", word & 1u);
    else
        snprintf(out, (size_t)size, "%d", (int)word);
}

int fh_game_var_set(int index, const char *text)
{
    char word[8], *end;
    size_t i;

    make_table();
    if (index < 0 || index >= var_count)
        return -1;
    if (vars[index].kind == FH_GAME_VAR_SWITCH) {
        for (i = 0; i + 1 < sizeof word && text[i]; i++)
            word[i] = (char)tolower((unsigned char)text[i]);
        word[i] = '\0';
        if (!strcmp(word, "1") || !strcmp(word, "true") || !strcmp(word, "on") || !strcmp(word, "yes"))
            MEM32(vars[index].address) = 1u;
        else if (!strcmp(word, "0") || !strcmp(word, "false") || !strcmp(word, "off") || !strcmp(word, "no"))
            MEM32(vars[index].address) = 0u;
        else
            return -1;
        return 0;
    }
    if (vars[index].kind == FH_GAME_VAR_FLOAT) {
        double value = strtod(text, &end);

        if (end == text || *end)
            return -1;
        MEM32(vars[index].address) = float_as_word((float)value);
        return 0;
    }
    {
        long value = strtol(text, &end, 0);

        if (end == text || *end)
            return -1;
        MEM32(vars[index].address) = (unsigned)value;
        return 0;
    }
}

int fh_game_debug_menu_open(void)
{
    return MEM32(MENU + MENU_SHOWN) != 0;
}

int fh_game_debug_menu_show(int open)
{
    if (!open) {
        MEM32(MENU + MENU_SHOWN) = 0u;
        return 0;
    }
    if (MEM32(MENU_FIRST_PAGE + PAGE_FIRST_ITEM) == 0u)
        return -1;                                              /* not made yet, or taken down */
    MEM32(MENU + MENU_PAGE) = MENU_FIRST_PAGE;                  /* the first page, its first item chosen */
    MEM32(MENU + MENU_ITEM) = MEM32(MENU_FIRST_PAGE + PAGE_FIRST_ITEM);
    MEM32(MENU + MENU_SHOWN) = 1u;
    return 0;
}

/* The seven cheats, in their bits' order (bit n-1 is cheat n). */
static const struct {
    const char *name, *help;
} cheats[] = {
    {"big_head", "Big Head (Easy): a soldier's part (by the name, the head) is drawn twice its size (bit 0x01)"},
    {"ricochet", "Ricochet (Easy): the player's bullets bounce, twice at most (bit 0x02)"},
    {"head_shot", "Head Shot (Easy): a hit on the head kills, a hit anywhere else does nothing (bit 0x04)"},
    {"ricochet_all", "Ricochet All (Easy): everyone's bullets bounce, twice at most (bit 0x08)"},
    {"shrink_shot", "Shrink Shot (Normal): a soldier shrinks as he is hurt, down to a third of his size (bit 0x10)"},
    {"dead_eye", "Dead Eye (Normal): every hit kills (bit 0x20)"},
    {"big_n_small", "Big 'N' Small (Hard): every soldier is a size of his own, by chance (bit 0x40)"},
};
#define CHEATS ((int)(sizeof cheats / sizeof cheats[0]))

int fh_game_cheat_count(void)
{
    return CHEATS;
}

const char *fh_game_cheat_name(int index)
{
    return index >= 0 && index < CHEATS ? cheats[index].name : NULL;
}

const char *fh_game_cheat_help(int index)
{
    return index >= 0 && index < CHEATS ? cheats[index].help : NULL;
}

/* "Big_Head", "bighead" and "big-head" are all big_head: letters and digits only, case aside. */
static int same_letters(const char *a, const char *b)
{
    for (;;) {
        while (*a && !isalnum((unsigned char)*a))
            a++;
        while (*b && !isalnum((unsigned char)*b))
            b++;
        if (!*a || !*b)
            return !*a && !*b;
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b))
            return 0;
        a++, b++;
    }
}

int fh_game_cheat_find(const char *text)
{
    int i;

    if (text[0] >= '1' && text[0] <= '0' + CHEATS && !text[1])
        return text[0] - '1';
    for (i = 0; i < CHEATS; i++)
        if (same_letters(cheats[i].name, text))
            return i;
    return -1;
}

int fh_game_cheat_state(int index)
{
    unsigned at = MEM32(CHEATS_ON), profile = MEM32(PROFILE);
    int state = 0;

    if (index < 0 || index >= CHEATS || at == 0u)
        return -1;
    if (MEM32(at) & (1u << index))
        state |= 1;
    if (profile != 0u && (MEM32(profile + PROFILE_UNLOCKED) & (1u << index)))
        state |= 2;
    return state;
}

int fh_game_cheat_set(int index, int on)
{
    unsigned at = MEM32(CHEATS_ON), profile = MEM32(PROFILE), bit;

    if (index < 0 || index >= CHEATS || at == 0u)
        return -1;
    bit = 1u << index;
    if (on) {
        MEM32(at) |= bit;
        if (profile != 0u)
            MEM32(profile + PROFILE_UNLOCKED) |= bit;
    } else {
        MEM32(at) &= ~bit;
    }
    return 0;
}

int fh_game_cheats_unlock(void)
{
    unsigned profile = MEM32(PROFILE);

    if (profile == 0u)
        return -1;
    MEM32(profile + PROFILE_UNLOCKED) |= CHEATS_ALL;
    return 0;
}

int fh_game_debug_menu_back(void)
{
    unsigned page, above;

    if (MEM32(MENU + MENU_SHOWN) == 0u)
        return -1;
    page = MEM32(MENU + MENU_PAGE);
    above = page ? MEM32(page + PAGE_ABOVE) : 0u;
    if (above == 0u) {                                          /* the first page: Back shuts the menu */
        MEM32(MENU + MENU_SHOWN) = 0u;
        return 0;
    }
    MEM32(MENU + MENU_PAGE) = above;                            /* the page above, its first item chosen */
    MEM32(MENU + MENU_ITEM) = MEM32(above + PAGE_FIRST_ITEM);
    return 1;
}

int fh_game_levels_reached(void)
{
    unsigned profile = MEM32(PROFILE);

    return profile != 0u ? (int)MEM32(profile + PROFILE_FURTHEST) : -1;
}

int fh_game_levels_unlock(void)
{
    unsigned profile = MEM32(PROFILE);

    if (profile == 0u)
        return -1;
    if ((int)MEM32(profile + PROFILE_FURTHEST) < LAST_LEVEL)
        MEM32(profile + PROFILE_FURTHEST) = (unsigned)LAST_LEVEL;
    return 0;
}

int fh_game_campaigns_unlock(void)
{
    unsigned profile = MEM32(PROFILE);

    if (profile == 0u)
        return -1;
    MEM32(profile + PROFILE_UNLOCKED) |= CAMPAIGNS_DONE;
    return 0;
}

unsigned fh_game_player(void)
{
    unsigned handle = MEM32(PLAYER_HANDLES), slot;

    if (handle == 0u)
        return 0u;
    slot = HANDLE_TABLE + (unsigned)((int)(short)(handle & 0xFFFFu) * 8);   /* movsx, shl 3 (0x000A0E80) */
    return MEM32(slot) == handle ? MEM32(slot + 4u) : 0u;
}

int fh_game_god_get(void)
{
    unsigned player = fh_game_player();

    return player != 0u ? MEM32(player + PLAYER_NO_DAMAGE) != 0u : -1;
}

int fh_game_god_set(int on)
{
    unsigned player = fh_game_player();

    if (player == 0u)
        return -1;
    MEM32(player + PLAYER_NO_DAMAGE) = on ? 1u : 0u;
    return 0;
}

int fh_game_ammo_get(void)
{
    return MEM32(UNLIMITED_AMMO) != 0u;
}

void fh_game_ammo_set(int on)
{
    MEM32(UNLIMITED_AMMO) = on ? 1u : 0u;
}

static float word_float(unsigned bits)
{
    float f;

    memcpy(&f, &bits, sizeof f);
    return f;
}

int fh_game_player_where(float pos[3], unsigned *class_table, int *weapon, int *magazine)
{
    unsigned player = fh_game_player(), frame, number, gun;

    *class_table = 0u;
    *weapon = -1;
    *magazine = -1;
    if (player == 0u)
        return -1;
    *class_table = MEM32(player);
    frame = MEM32(player + PLAYER_FRAME);
    if (frame == 0u)
        return -1;
    /* The world matrix as the game last worked it out (0x001BF6F0 brings it up to date first; between frames it
     * already is, the console reading it after the game's update). */
    pos[0] = word_float(MEM32(frame + FRAME_LTM_POS));
    pos[1] = word_float(MEM32(frame + FRAME_LTM_POS + 4u));
    pos[2] = word_float(MEM32(frame + FRAME_LTM_POS + 8u));
    number = MEM32(player + PLAYER_IN_HAND);
    if (number < PLAYER_GUN_MOST) {
        *weapon = (int)number;
        gun = MEM32(player + PLAYER_GUNS + 4u * number);
        if (gun != 0u)
            *magazine = (int)MEM32(gun + GUN_MAGAZINE);
    }
    return 0;
}

/* [acct2-fh] 11-15: the player's health. 0x000C3620, the player's own "take a hit" (it does nothing while
 * +0x19FC, god, is set), takes the hit's damage over the hit points (+0x19E0, an int) from the health left
 * (+0x19DC, a float, 1.0 full; below 0 the player is dead); the level scripts' SetPlayerHealthPercentage sets it
 * through the player's class +0x238 (0x000C8610, one store). */
int fh_game_player_health(float *health, int *hit_points)
{
    unsigned player = fh_game_player();

    *health = 0.0f;
    *hit_points = 0;
    if (player == 0u)
        return -1;
    *health = word_float(MEM32(player + PLAYER_HEALTH));
    *hit_points = (int)MEM32(player + PLAYER_HIT_POINTS);
    return 0;
}

int fh_game_player_axes(float at[3], float right[3])
{
    unsigned player = fh_game_player(), frame, i;

    if (player == 0u)
        return -1;
    frame = MEM32(player + PLAYER_FRAME);
    if (frame == 0u)
        return -1;
    for (i = 0; i < 3; i++) {
        at[i] = word_float(MEM32(frame + FRAME_LTM_AT + 4u * i));
        right[i] = word_float(MEM32(frame + FRAME_LTM_RIGHT + 4u * i));
    }
    return 0;
}

/* Calls one of the game's thiscall functions as its own code does: the arguments pushed last to first, then the
 * return address of a real call of it, ecx the object; its ret N takes them off. Every register is put back after
 * (the console runs inside the game's Present, on its thread). The function's eax, or ~0 when it isn't in the port. */
static uint32_t call_game(uint32_t function, uint32_t self, uint32_t return_address, int count, const uint32_t *args)
{
    uint32_t eax_was = g_eax, ebx_was = g_ebx, ecx_was = g_ecx, edx_was = g_edx;
    uint32_t esi_was = g_esi, edi_was = g_edi, ebp_was = g_ebp, esp_was = g_esp, result;
    recomp_func_t fn = recomp_lookup_manual(function);
    int i;

    if (!fn)
        fn = recomp_lookup(function);
    if (!fn)
        return ~0u;
    for (i = count - 1; i >= 0; i--)
        PUSH32(g_esp, args[i]);
    PUSH32(g_esp, return_address);
    g_ecx = self;
    fn();
    result = g_eax;
    g_eax = eax_was;
    g_ebx = ebx_was;
    g_ecx = ecx_was;
    g_edx = edx_was;
    g_esi = esi_was;
    g_edi = edi_was;
    g_ebp = ebp_was;
    g_esp = esp_was;
    return result;
}

/* 1 when the level has the weapon loaded (the scripts' IsPlayerWeaponStreamed: 0x000C2DE0 asks 0x0010C360 for the
 * weapon's model, 0x000D20D0(weapon)+0x24, and -1 is "not loaded"), 0 when not, -1 when the function is missing. */
static int weapon_streamed(unsigned player, int weapon)
{
    uint32_t arg = (uint32_t)weapon, answer = call_game(WEAPON_STREAMED, player, WEAPON_STREAMED_RETURN, 1, &arg);

    return answer == ~0u ? -1 : answer != 0u;
}

int fh_game_player_weapons(int carried[2], int *in_hand, unsigned loaded[2])
{
    unsigned player = fh_game_player(), i;

    loaded[0] = loaded[1] = 0u;
    carried[0] = carried[1] = -1;
    *in_hand = -1;
    if (player == 0u)
        return -1;
    /* Every number has an object at +0x1A48 (boot67: all 39), so that says nothing; the game's own "is it loaded"
     * does. */
    for (i = 0; i < PLAYER_GUN_MOST; i++)
        if (MEM32(player + PLAYER_GUNS + 4u * i) != 0u && weapon_streamed(player, (int)i) == 1)
            loaded[i / 32u] |= 1u << (i % 32u);
    carried[0] = (int)MEM32(player + PLAYER_CARRIED);
    carried[1] = (int)MEM32(player + PLAYER_CARRIED + 4u);
    *in_hand = (int)MEM32(player + PLAYER_IN_HAND);
    return 0;
}

int fh_game_player_give(int weapon)
{
    unsigned player = fh_game_player();
    uint32_t args[3];
    int streamed;

    if (player == 0u)
        return -1;
    if (weapon < 0 || weapon >= PLAYER_GUN_MOST || MEM32(player + PLAYER_GUNS + 4u * (unsigned)weapon) == 0u)
        return -2;
    /* Only a weapon the level loaded: with one it hadn't (weapon 0 in the first level, boot67) 0x000C7190 called
     * through a bad address and the game crashed. */
    streamed = weapon_streamed(player, weapon);
    if (streamed < 0)
        return -3;
    if (streamed == 0)
        return -2;
    /* As SetPlayerWeapon (0x000386F0) calls it: the weapon; now: 1, into the hand at once; the slot: 0, the one in
     * hand. Then the command's own +0x2134 = 1. */
    if (!recomp_lookup_manual(SET_WEAPON) && !recomp_lookup(SET_WEAPON))
        return -3;
    args[0] = (uint32_t)weapon;
    args[1] = 1u;
    args[2] = 0u;
    call_game(SET_WEAPON, player, SET_WEAPON_RETURN, 3, args);
    MEM32(player + PLAYER_SCRIPT_WEAPON) = 1u;
    return 0;
}

int fh_game_level_most(void)
{
    return LAST_LEVEL;
}

/* [acct2-fh] 11-15: a weapon's name from the game's own list ("Nagant Sniper", "T34 Cannon"...), for give and pos. */
int fh_game_weapon_name(int weapon, char *out, size_t size)
{
    unsigned text, i;

    if (size == 0)
        return -1;
    out[0] = '\0';
    if (weapon < 0 || weapon >= PLAYER_GUN_MOST)
        return -1;
    text = MEM32(WEAPON_NAMES + 4u * (unsigned)weapon);
    if (text == 0u)
        return -1;
    for (i = 0; i + 1 < size && i < WEAPON_NAME_MOST; i++) {
        unsigned char c = MEM8(text + i);

        if (c == 0u)
            break;
        out[i] = (char)(c >= 0x20u && c < 0x7Fu ? c : '?');
    }
    out[i] = '\0';
    return 0;
}

/* [acct2-fh] 11-15: a weapon's data, 0x550 bytes each by number from 0x003BE348 (0x000D20D0 gives a number's; the
 * level's tuning fills them as it loads), and the damage one of its bullets does: +0xDC, a float, before the
 * difficulty's scale (the guns' fire routine copies it into its hit at 0x000D39C2-0x000D39D8, 0 for the same side). */
#define WEAPON_DATA       0x003BE348u
#define WEAPON_DATA_SIZE  0x550u
#define WEAPON_DAMAGE     0xDCu

int fh_game_weapon_damage(int weapon, float *damage)
{
    *damage = 0.0f;
    if (weapon < 0 || weapon >= PLAYER_GUN_MOST)
        return -1;
    *damage = word_float(MEM32(WEAPON_DATA + WEAPON_DATA_SIZE * (unsigned)weapon + WEAPON_DAMAGE));
    return 0;
}

int fh_game_level_name(int level, char *out, size_t size)
{
    unsigned text, i;

    if (size == 0)
        return -1;
    out[0] = '\0';
    if (level < 0 || level > LAST_LEVEL)
        return -1;
    text = MEM32(LEVEL_NAMES + 4u * (unsigned)level);
    if (text == 0u)
        return -1;
    for (i = 0; i + 1 < size && i < 32u; i++) {     /* "L_1_1_B": short names in the game's data */
        unsigned char c = MEM8(text + i);

        if (c == 0u)
            break;
        out[i] = (char)(c >= 0x20u && c < 0x7Fu ? c : '?');
    }
    out[i] = '\0';
    return 0;
}

int fh_game_level_now(void)
{
    uint32_t level;

    if (fh_game_player() == 0u)
        return -1;
    level = call_game(LEVEL_NOW, 0u, LEVEL_NOW_RETURN, 0, NULL);
    return level <= (uint32_t)LAST_LEVEL ? (int)level : -1;
}

int fh_game_level_goto(int level)
{
    unsigned profile = MEM32(PROFILE), unlocked = 0u;
    uint32_t arg = (uint32_t)level;

    if (level < 1 || level > LAST_LEVEL)
        return -2;
    if (fh_game_player() == 0u)
        return -1;                          /* in a level only: the front end starts levels its own way */
    if (MEM32(LEVEL_CHANGING) & LEVEL_CHANGING_BIT)
        return -4;                          /* 0x000A42F0 itself would do nothing */
    if (!recomp_lookup_manual(LEVEL_GOTO) && !recomp_lookup(LEVEL_GOTO))
        return -3;
    /* 0x000A42F0 marks a campaign done on the profile (+0x30, 0x1000 / 0x2000) when the level left is the last of
     * one (12, 18); a jump from the console isn't finishing it, so the profile's word is put back after. */
    if (profile != 0u)
        unlocked = MEM32(profile + PROFILE_UNLOCKED);
    call_game(LEVEL_GOTO, 0u, LEVEL_GOTO_RETURN, 1, &arg);
    if (profile != 0u)
        MEM32(profile + PROFILE_UNLOCKED) = unlocked;
    return 0;
}

int fh_game_weapon_no_gun(void)
{
    return WEAPON_NO_GUN;
}

int fh_game_player_teleport(const float to[3])
{
    unsigned player = fh_game_player(), frame, target, block, i;
    uint32_t eax_was = g_eax, ebx_was = g_ebx, ecx_was = g_ecx, edx_was = g_edx;
    uint32_t esi_was = g_esi, edi_was = g_edi, ebp_was = g_ebp, esp_was = g_esp;
    recomp_func_t set_position;

    if (player == 0u)
        return -1;
    frame = MEM32(player + PLAYER_FRAME);
    if (frame == 0u)
        return -1;
    target = MEM32(MEM32(player) + CLASS_SET_POSITION);
    set_position = recomp_lookup_manual(target);    /* found as the game's own calls find it (RECOMP_ICALL_SAFE_AT) */
    if (!set_position)
        set_position = recomp_lookup(target);
    if (!set_position)
        return -2;
    /* The three floats on the game's stack, below what is in use there, then the call as 0x00046783 makes it:
     * push the position's address, the return address, ecx = the player. Its ret 4 takes both off again. */
    g_esp -= 16u;
    block = g_esp;
    for (i = 0; i < 3; i++)
        MEMF(block + 4u * i) = MEMF(frame + FRAME_OWN_POS + 4u * i) + (to[i] - MEMF(frame + FRAME_LTM_POS + 4u * i));
    PUSH32(g_esp, block);
    PUSH32(g_esp, SET_POSITION_RETURN);
    g_ecx = player;
    set_position();
    /* The game's registers as they were (the console runs inside the game's Present, between its own calls). */
    g_eax = eax_was;
    g_ebx = ebx_was;
    g_ecx = ecx_was;
    g_edx = edx_was;
    g_esi = esi_was;
    g_edi = edi_was;
    g_ebp = ebp_was;
    g_esp = esp_was;
    return 0;
}

/* [acct2-fh] 11-15: a hit on the player (phase 4, for hurt, mostly to show god works), made as the game's own "hurt a
 * group" (0x001164E0) makes one: a 0x30-byte record zeroed on the game's stack, its kind (+2) 2, the damage at +0x28
 * and +0x2C, no attacker (+0x0C 0, which 0x000C3620 and the two calls it makes with it check for), then the player's
 * class +0x60 with it, the return address 0x001164E0's call has. So the game decides all of it: nothing while god
 * (+0x19FC) is set, nothing just after a hit from an attacker (+0x19EC), the difficulty's scale, the hit's sound,
 * and a death when the health reaches 0. 0 done (with the health before and after), -1 with no player, -2 when the
 * function isn't in the port. */
int fh_game_player_hurt(float damage, float *before, float *after)
{
    unsigned player = fh_game_player(), i;
    uint32_t esp_was = g_esp, block, target, answer;

    *before = *after = 0.0f;
    if (player == 0u)
        return -1;
    target = MEM32(MEM32(player) + CLASS_TAKE_HIT);
    *before = word_float(MEM32(player + PLAYER_HEALTH));
    g_esp -= HIT_SIZE + 16u;            /* below what is in use on the game's stack, as for teleport */
    block = g_esp;
    for (i = 0; i < HIT_SIZE; i += 4u)
        MEM32(block + i) = 0u;
    MEM32(block + HIT_KIND_WORD) = HIT_KIND_GROUP << 16;
    MEM32(block + HIT_DAMAGE_TOO) = float_as_word(damage);
    MEM32(block + HIT_DAMAGE) = float_as_word(damage);
    answer = call_game(target, player, TAKE_HIT_RETURN, 1, &block);
    g_esp = esp_was;
    if (answer == ~0u)
        return -2;
    *after = word_float(MEM32(player + PLAYER_HEALTH));
    return 0;
}

/* [acct2-fh] 11-15: why the player's "take a hit" (0x000C3620) would take no health now, read in its own order: god
 * (+0x19FC), then the time after a hit from an attacker (+0x19EC above 0), then the byte at 0x003EA960, then +0x414 of
 * the thing whose handle is at +0x210C being 1 (the handle looked up as 0x000A0E80 does). 0 when none of them is
 * set (or there is no player). */
unsigned fh_game_player_hurt_gates(float *after_hit)
{
    unsigned player = fh_game_player(), gates = 0u, handle, slot, thing;

    *after_hit = 0.0f;
    if (player == 0u)
        return 0u;
    if (MEM32(player + PLAYER_NO_DAMAGE) != 0u)
        gates |= FH_HURT_GOD;
    *after_hit = word_float(MEM32(player + PLAYER_AFTER_HIT));
    if (*after_hit > 0.0f)
        gates |= FH_HURT_AFTER_HIT;
    if (MEM8(NO_HEALTH_LOSS) != 0u)
        gates |= FH_HURT_NO_LOSS;
    handle = MEM32(player + PLAYER_CAMERA);
    if (handle != 0u) {
        slot = HANDLE_TABLE + (unsigned)((int)(short)(handle & 0xFFFFu) * 8);
        thing = MEM32(slot) == handle ? MEM32(slot + 4u) : 0u;
        if (thing != 0u && MEM32(thing + CAMERA_NO_LOSS) == 1u)
            gates |= FH_HURT_CAMERA;
    }
    return gates;
}

/* [acct2-fh] 11-15: the difficulty, read only: 0x00328664 (0-2), and the scale 0x000BF9B0 gives a hit's damage on the
 * player for it (the game fills the three while it runs). 0 with them, -1 when it is outside 0-2 (then the game's
 * scale is its 1.0 at 0x002DBB48; *scale is 0). */
int fh_game_difficulty(int *level, float *scale)
{
    *level = (int)MEM32(DIFFICULTY);
    *scale = 0.0f;
    if (*level < 0 || *level >= DIFFICULTY_MOST)
        return -1;
    *scale = word_float(MEM32(DIFFICULTY_DAMAGE + 4u * (unsigned)*level));
    return 0;
}

/* [acct2-fh] 11-15: the difficulty a level starts with (the settings' +0x24, what the level select sets; 0x00128E60
 * copies it to 0x00328664 at a level's start), -1 with no settings yet; and the damage scale for each difficulty. */
int fh_game_difficulty_setting(float scales[3])
{
    unsigned settings = MEM32(SETTINGS), i;

    for (i = 0; i < (unsigned)DIFFICULTY_MOST; i++)
        scales[i] = word_float(MEM32(DIFFICULTY_DAMAGE + 4u * i));
    return settings != 0u ? (int)MEM32(settings + SETTINGS_DIFFICULTY) : -1;
}

/* [acct2-fh] 11-15: sets the difficulty as the level select does, through the game's own "set the difficulty"
 * (0x001292D0, the settings' +0x24), and for the level being played now (0x00328664, which all 31 places that ask
 * read), so it holds from here and for the levels after. Call it on the game's thread only. 0 done, -1 out of 0-2,
 * -2 when the function isn't in the port, -3 with no settings yet. */
int fh_game_difficulty_set(int level)
{
    uint32_t arg = (uint32_t)level;

    if (level < 0 || level >= DIFFICULTY_MOST)
        return -1;
    if (MEM32(SETTINGS) == 0u)
        return -3;
    if (call_game(DIFFICULTY_SET, 0u, DIFFICULTY_SET_RETURN, 1, &arg) == ~0u)
        return -2;
    MEM32(DIFFICULTY) = arg;
    return 0;
}

/* [acct2-fh] 11-15: the take-a-hit watch (the console's hitcalls, for the frame-rate audit of the enemy's fire on the
 * player in L_1_2: its 35-point hits come at 30 fps and hardly ever above, and no weapon's bullet does 35 or 60).
 *
 * The player's class's "take a hit" (0x000C3620, thiscall, one argument: the hit's block) is only ever called through
 * the class's table (call [reg+0x60]: 11 places, no direct call), and the generated code asks recomp_lookup_manual()
 * first on every call through a pointer. While the watch is on, fh_game_hit_watch_lookup() hands it hit_watched(),
 * which notes the call (where from, the block's kind and damage, the shooter, the health before and after) and runs
 * the game's own function, as generated, with nothing changed. Off, it is one comparison a call through a pointer.
 *
 * At the function's start [esp] is the return address (the call is 3 bytes, ff 5x 60) and [esp+4] the block (the
 * function reads it as [esp+0x1C] after its sub esp,0x10 and two pushes). The block (0x000D3948 builds a bullet's):
 * +2 a word, the kind; +0x08 0x201 / 0x202 for a gun's bullet; +0x18 the shooter; +0x28 and +0x2C the damage (a
 * bullet's is its weapon's +0xDC in both); 0x000C3684 takes +0x2C times the difficulty's scale / the hit points off
 * the health. The game's thread calls it, and the console reads the notes on the same thread. */
#define TAKE_A_HIT      0x000C3620u /* the player's class's "take a hit" (its table's +0x60) */
#define TAKE_A_HIT_CALL 3u          /* the bytes of call [reg+0x60] */
#define HIT_KIND        0x00u       /* the block's: its first dword (the words +0 and +2) */
#define HIT_SOURCE      0x04u       /* what dealt it: 0x00103850 (a projectile's step) puts the projectile here */
#define SHOT_POSITION   0x04u       /* a projectile's (0x00103850): where it is, three floats (where it started, at
                                     * its first step: the step doesn't move it the frame it starts) */
#define SHOT_VELOCITY   0x10u       /* a projectile's (0x00103850): its velocity, three floats (a step's worth) */
#define SHOT_BORN       0x1Cu       /* the game's time it started (0x003B4788, 60ths) */
#define SHOT_LIFE       0x20u       /* how long it may fly: 0x00103B83 ends it once the game's time - +0x1C passes it */
#define SHOT_RECORD     0x3Cu       /* its damage record (0x00102290 sets it from its third argument): +4 the damage */
#define SHOT_STEP       0x00103850u /* a projectile's step (its class's +0x14: tables 0x002E7E70 and 0x002E7EE0) */
#define HIT_WHAT        0x08u
#define HIT_BY          0x18u
#define HIT_DAMAGE_28   0x28u
#define HIT_DAMAGE      0x2Cu
#define GUEST_LOW       0x00010000u /* a shooter's class table is only read from an address in the guest's memory */
#define GUEST_HIGH      0x08000000u
#define HIT_NOTES       64u         /* notes kept until the console reads them */

void sub_000C3620(void);            /* generated (recomp_funcs.h): the game's own take-a-hit */
void sub_00103850(void);            /* generated: the game's own projectile step */

static struct fh_game_hit hit_notes[HIT_NOTES];
static unsigned hit_written, hit_read, hit_dropped;
static int hit_watching;

static void hit_watched(void)
{
    struct fh_game_hit *note;
    uint32_t back = MEM32(g_esp), block = MEM32(g_esp + 4u), taker = g_ecx, by;

    if (hit_written - hit_read >= HIT_NOTES) {      /* the console hasn't read the last 64: drop the oldest */
        hit_read++;
        hit_dropped++;
    }
    note = &hit_notes[hit_written % HIT_NOTES];
    hit_written++;                                  /* taken first, so a hit inside this one gets its own note */
    note->from = back - TAKE_A_HIT_CALL;
    note->taker = taker;
    note->player = taker == fh_game_player();
    note->kind = MEM32(block + HIT_KIND);
    note->what = MEM32(block + HIT_WHAT);
    by = MEM32(block + HIT_BY);
    note->by = by;
    note->by_class = by >= GUEST_LOW && by < GUEST_HIGH ? MEM32(by) : 0u;
    note->source = MEM32(block + HIT_SOURCE);
    note->source_born = note->source_life = note->source_speed = 0.0f;
    note->source_class = note->source_record = 0u;
    if (note->source >= GUEST_LOW && note->source < GUEST_HIGH) {
        uint32_t v = note->source + SHOT_VELOCITY;
        float vx = word_float(MEM32(v)), vy = word_float(MEM32(v + 4u)), vz = word_float(MEM32(v + 8u));

        note->source_class = MEM32(note->source);
        note->source_record = MEM32(note->source + SHOT_RECORD);
        note->source_born = word_float(MEM32(note->source + SHOT_BORN));
        note->source_life = word_float(MEM32(note->source + SHOT_LIFE));
        note->source_speed = (float)sqrt((double)(vx * vx + vy * vy + vz * vz));
    }
    note->damage = word_float(MEM32(block + HIT_DAMAGE));
    note->damage_28 = word_float(MEM32(block + HIT_DAMAGE_28));
    note->before = word_float(MEM32(taker + PLAYER_HEALTH));
    note->game_time = fh_frame_clock_game_time() / 60.0;
    note->frame = (unsigned)fh_frame_clock_frame_count();
    sub_000C3620();                                 /* the game's own, as it was */
    note->after = word_float(MEM32(taker + PLAYER_HEALTH));
}

/* [acct2-fh] 11-15: the rounds census, with the take-a-hit watch (boot87: every hit on the player is a projectile's,
 * with a damage record of 60 or 35; how many of each are fired, at each frame rate?). The step 0x00103850 is only
 * called through its class's table (no direct call), so the watch hands back shot_watched() for it too: it counts each
 * projectile once, the first time it steps (its address and start time not seen before), by its damage record and
 * class, then runs the game's own step unchanged. Since build64 (boot88: the 35-point rounds are made 1.1-1.7 a second
 * at 30 fps, 0.5 at 60 and 0.02 with no limit, while the 60-point ones are made 1.5-1.8 a second at every rate) it also
 * measures how far each round starts from the player, and counts the projectile's five launch functions by where they
 * return to: which code fires each kind, and from how far. */
#define SHOT_SEEN       1024u       /* projectiles remembered (by address) so each is counted once */
#define SHOT_KINDS      8u

static struct { uint32_t address; uint32_t born; } shot_seen[SHOT_SEEN];
static struct fh_game_shots shot_counts;

static void shot_count(uint32_t shot)
{
    uint32_t born = MEM32(shot + SHOT_BORN), record = MEM32(shot + SHOT_RECORD), slot = (shot >> 4) % SHOT_SEEN;
    uint32_t class_table = MEM32(shot), i;
    float damage = record >= GUEST_LOW && record < GUEST_HIGH ? word_float(MEM32(record + 4u)) : -1.0f;
    float at[3];
    unsigned player_class;
    int weapon, magazine;

    if (shot_seen[slot].address == shot && shot_seen[slot].born == born)
        return;                                     /* seen: not a new round */
    shot_seen[slot].address = shot;
    shot_seen[slot].born = born;
    for (i = 0; i < shot_counts.kinds; i++)
        if (shot_counts.kind[i].damage == damage && shot_counts.kind[i].class_table == class_table)
            break;
    if (i == shot_counts.kinds) {
        if (shot_counts.kinds == SHOT_KINDS) {
            shot_counts.other++;
            return;
        }
        shot_counts.kind[i].damage = damage;
        shot_counts.kind[i].class_table = class_table;
        shot_counts.kind[i].record = record;
        shot_counts.kind[i].made = 0u;
        shot_counts.kind[i].placed = 0u;
        shot_counts.kind[i].distance_sum = shot_counts.kind[i].distance_least = 0.0f;
        shot_counts.kinds++;
    }
    shot_counts.kind[i].made++;
    if (fh_game_player_where(at, &player_class, &weapon, &magazine) == 0) {
        float dx = word_float(MEM32(shot + SHOT_POSITION)) - at[0];
        float dy = word_float(MEM32(shot + SHOT_POSITION + 4u)) - at[1];
        float dz = word_float(MEM32(shot + SHOT_POSITION + 8u)) - at[2];
        float distance = (float)sqrt((double)(dx * dx + dy * dy + dz * dz));

        if (shot_counts.kind[i].placed == 0u || distance < shot_counts.kind[i].distance_least)
            shot_counts.kind[i].distance_least = distance;
        shot_counts.kind[i].distance_sum += distance;
        shot_counts.kind[i].placed++;
    }
}

static void shot_watched(void)
{
    shot_count(g_ecx);
    sub_00103850();                                 /* the game's own, as it was */
}

/* The launch functions: each calls 0x00102290 (the round's set-up) with its own first three arguments and a 0 (the
 * fifth, 0x00102EF0, with its fourth), this (ecx) being the round; 0x00102290 keeps the third as the damage record
 * (+0x3C). At the start [esp] is where the function returns to and [esp+0x0C] that third argument. None is called
 * directly (each sits in a class's table), so every call asks recomp_lookup_manual() first. */
#define SHOT_LAUNCH_1   0x00102970u
#define SHOT_LAUNCH_2   0x00102D30u
#define SHOT_LAUNCH_3   0x00102E50u
#define SHOT_LAUNCH_4   0x00102FF0u
#define SHOT_LAUNCH_5   0x00102EF0u
#define SHOT_SET_UP     0x00102290u /* the set-up itself: the first entry of the class table 0x002E7E70, so the
                                     * code that makes a round of that class calls it through the table (boot89:
                                     * every enemy round is of that class, and none came through the five above) */
#define LAUNCH_RECORD   0x0Cu       /* the third argument, from [esp] at the start */
/* The projectile factory 0x00101FA0 (thiscall, 7 arguments: by its object's kind it makes a round of one of nine
 * classes, then calls the class's first entry with its arguments 1, 2, 6 and 7) makes every round: its call returns
 * to 0x001021AE. Below that call its own frame is 0x34 bytes (3 pushes of its exception record, sub esp,8, 3 saved
 * registers, the 4 arguments and the return), so [esp+0x34] at the set-up's start is where the factory returns to.
 * Its only caller is 0x00102220 ("fire a round", cdecl, called from 16 places: the code that fires), from
 * 0x0010227C, returning to 0x00102281; that one's frame below its call is 0x28 bytes (2 saved registers, the 7
 * arguments, the return), so [esp+0x5C] is where it returns to: the code that fired. */
#define SHOT_FACTORY_BACK 0x001021AEu
#define FACTORY_CALLER  0x34u
#define FIRE_BACK       0x00102281u /* where the factory returns to in 0x00102220 */
#define FIRE_CALLER     0x5Cu

void sub_00102970(void);            /* generated: the game's own launch functions */
void sub_00102D30(void);
void sub_00102E50(void);
void sub_00102FF0(void);
void sub_00102EF0(void);
void sub_00102290(void);

static struct fh_game_launches launch_counts;

static void launch_count(uint32_t launch)
{
    uint32_t back = MEM32(g_esp), record = MEM32(g_esp + LAUNCH_RECORD), i;
    uint32_t from = back == SHOT_FACTORY_BACK ? MEM32(g_esp + FACTORY_CALLER) : 0u;

    if (from == FIRE_BACK)          /* through 0x00102220, as every round is: the code that called that */
        from = MEM32(g_esp + FIRE_CALLER);

    for (i = 0; i < launch_counts.sites; i++)
        if (launch_counts.site[i].launch == launch && launch_counts.site[i].back == back &&
            launch_counts.site[i].from == from && launch_counts.site[i].record == record)
            break;
    if (i == launch_counts.sites) {
        if (launch_counts.sites == FH_GAME_LAUNCH_SITES) {
            launch_counts.other++;
            return;
        }
        launch_counts.site[i].launch = launch;
        launch_counts.site[i].back = back;
        launch_counts.site[i].from = from;
        launch_counts.site[i].record = record;
        launch_counts.site[i].damage = record >= GUEST_LOW && record < GUEST_HIGH ?
                                       word_float(MEM32(record + 4u)) : -1.0f;
        launch_counts.site[i].made = 0u;
        launch_counts.sites++;
    }
    launch_counts.site[i].made++;
}

static void launch_1_watched(void)
{
    launch_count(SHOT_LAUNCH_1);
    sub_00102970();                                 /* the game's own, as it was */
}

static void launch_2_watched(void)
{
    launch_count(SHOT_LAUNCH_2);
    sub_00102D30();
}

static void launch_3_watched(void)
{
    launch_count(SHOT_LAUNCH_3);
    sub_00102E50();
}

static void launch_4_watched(void)
{
    launch_count(SHOT_LAUNCH_4);
    sub_00102FF0();
}

static void launch_5_watched(void)
{
    launch_count(SHOT_LAUNCH_5);
    sub_00102EF0();
}

/* Only the calls through the table come here: the five launch functions call it directly. Its arguments are theirs
 * (the third, [esp+0x0C], the damage record), so launch_count() reads it the same way. */
static void set_up_watched(void)
{
    launch_count(SHOT_SET_UP);
    sub_00102290();
}

fh_game_func_t fh_game_hit_watch_lookup(unsigned xbox_va)
{
    if (!hit_watching)
        return NULL;
    switch (xbox_va) {
    case TAKE_A_HIT:    return hit_watched;
    case SHOT_STEP:     return shot_watched;
    case SHOT_LAUNCH_1: return launch_1_watched;
    case SHOT_LAUNCH_2: return launch_2_watched;
    case SHOT_LAUNCH_3: return launch_3_watched;
    case SHOT_LAUNCH_4: return launch_4_watched;
    case SHOT_LAUNCH_5: return launch_5_watched;
    case SHOT_SET_UP:   return set_up_watched;
    default:            return NULL;
    }
}

void fh_game_shots_take(struct fh_game_shots *shots)
{
    *shots = shot_counts;
    memset(&shot_counts, 0, sizeof shot_counts);
}

void fh_game_launches_take(struct fh_game_launches *launches)
{
    *launches = launch_counts;
    memset(&launch_counts, 0, sizeof launch_counts);
}

void fh_game_hit_watch(int on)
{
    hit_watching = on != 0;
    hit_read = hit_written;
    hit_dropped = 0u;
    memset(&shot_counts, 0, sizeof shot_counts);
    memset(&launch_counts, 0, sizeof launch_counts);
}

int fh_game_hit_next(struct fh_game_hit *hit, unsigned *dropped)
{
    *dropped = hit_dropped;
    hit_dropped = 0u;
    if (hit_read == hit_written)
        return 0;
    *hit = hit_notes[hit_read % HIT_NOTES];
    hit_read++;
    return 1;
}
