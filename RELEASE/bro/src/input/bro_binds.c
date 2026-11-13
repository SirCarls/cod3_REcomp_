/* [acct2-bro] Big Red One port: the keyboard's and the mouse's binds, as CoD3's (owner 2026-11-08 09:18, 09:21: "if
 * you can add binding similar to that of cod3 too that would be great").
 *
 * Each key calls one of the game's own actions, never a controller button the keyboard pretends to press, as CoD3's
 * key binds do (COD_3_WAW_Setup-tool/recomp/src/game/keybinds/model.cpp): the same action ids, the same default keys
 * (World at War's), the same key names (MOUSE1, MWHEELDOWN, SHIFT, CTRL, SPACE, F, ...), up to two keys an action and
 * up to three actions a key. Big Red One registers the same commands as CoD3 (its default.xbe has +attack, +speed,
 * +activate, +reload, +activatereload, +melee, +activatemelee, +grenadeattack, +sprint, +leanleft, +leanright,
 * +leanleftswitchnext, +leanrightswitchnext, +gostand, +moveup, +activatemoveup, +movedown, +prone, +scores,
 * togglecrouch, toggleprone, lowerstance, raisestance, weapnext, weapprev and weapalt). CoD3's smoke grenade,
 * binoculars, hold breath and weapon slots have no command of their own here, so they stay on the controller's
 * buttons ([keys] in bro.toml); CoD3's Shift (sprint / hold breath) is sprint.
 *
 * How the game is told: the command's text goes into the game's command buffer the way Cbuf_AddText (0x002AABB0) puts
 * it there (cmd_text: the data pointer at 0x00935328, its size at 0x0093532C, its length at 0x00935330), and the game
 * runs it at its next Cbuf_Execute. The pad is read from the game's own input code on the game's thread, so nothing
 * runs at the same time. A button action sends "+attack <id> <time>" on the press and "-attack <id> <time>" on the
 * release, each key with an id of its own (two keys are two holders, as two controller buttons are, and letting one go
 * doesn't let go of the other); a one-time action sends its command on the press (a wheel notch is a press). Text that
 * doesn't fit in the buffer waits for the next poll, in order, so no release is lost.
 *
 * The <time> is the game's com_frameTime (0x00947710) when the key changed, as Quake III's key events give it. The
 * game's IN_KeyUp (0x002ABF50) adds "the time since the press" to the button when it gets one; without one it adds half
 * a frame (frame_msec at 0x0092F130, halved), so a held look key kept turning for half a frame after it was let go:
 * 1.1 degrees at 60 fps, 3.5 at 20 (t238, t241). With the time, a hold counts the frames it was held and no more.
 *
 * Only in play: in the game's menus, on loading screens and in movies the keys are the controller's ([keys] in
 * bro.toml: Space A, Escape back, ...) so the menus work as before. In play a key that has an action does only the
 * action (its controller button isn't pressed; bro_binds_claims), and keys without one stay the controller's (WASD
 * move, the arrows look, Return start, Escape back). In play = the game makes commands from the controls (CL_MouseMove
 * flips cl.mouseIndex, 0x0096174C, as raw_mouse in bro_pad.c checks), the world is being drawn (ngl_mesh_world_lists
 * moves; the first test run, t232, found the commands made for a second at start-up, before any level), both within
 * the last 250 ms, and the byte CL_KeyEvent tests with 3 at 0x00934880 (Quake III's cls.keyCatchers: 1 the console, 2 a
 * menu) is clear of both; its changes are logged. BRO_BINDS_CATCHERS=0 leaves that byte out. Leaving play lets go of
 * every held action.
 *
 * Settings, as CoD3's (bro/config/bro.toml):
 *   [binds] fire = "MOUSE1", melee = "V, MOUSE3", usereload = "" (unbound), ...   one line an action, two keys at most
 *   input.binds_kbm = "fire=MOUSE1;aim=MOUSE2;..."   CoD3's own text (cod3.toml's cod3_binds_kbm_p1), so a CoD3 line
 *                                                    works here; [binds] wins over it; actions not given keep CoD3's
 *                                                    defaults unless their key went to another action
 *   input.binds = false                              no action binds: every key is the controller's, as before
 * Key names: CoD3's (SPACE TAB ENTER ESCAPE BACKSPACE SHIFT CTRL ALT CAPSLOCK UPARROW DOWNARROW LEFTARROW RIGHTARROW
 * INS DEL HOME END PGUP PGDN F1-F12 KP_0-KP_9 KP_ENTER SEMICOLON COMMA MOUSE1-MOUSE5 MWHEELUP MWHEELDOWN, letters,
 * digits, = - . / ` [ \ ] '), or SDL's ("Left Ctrl", "Return", ...).
 *
 * BRO_BINDS_TEST=<ms>:<command>[:<hold ms>],... sends commands as a key would, at times counted in play only (a load
 * in between doesn't count) ("+lookup" is held for <hold ms> from its press, 150 by default, then let go; others are
 * sent once) and logs the view's angles and the world's field of view at each, so a run without a keyboard checks the
 * whole path ("20000:+lookup:2000": the view pitches up for 2 s). */
#include <math.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include "recomp_types.h"
#include "xbox_memory_layout.h"     /* XBOX_PTR */
#include "bro_binds.h"
#include "bro_settings.h"
#include "ngl_mesh.h"          /* ngl_mesh_world_lists (in play?), ngl_mesh_world_fov (BRO_BINDS_TEST's log) */

#if defined(BRO_HAVE_SDL3)
#include <SDL3/SDL.h>
#endif

#define CMD_TEXT_DATA       0x00935328u     /* cmd_text: the buffer's guest address */
#define CMD_TEXT_SIZE       0x0093532Cu     /* its size */
#define CMD_TEXT_LENGTH     0x00935330u     /* the text in it now */
#define CL_MOUSE_INDEX      0x0096174Cu     /* cl.mouseIndex: CL_MouseMove flips it each command made from the controls */
#define KEY_CATCHERS        0x00934880u     /* cls.keyCatchers (a byte): 1 the console, 2 a menu */
#define CL_VIEW_PITCH       0x00961798u     /* cl.viewangles (for BRO_BINDS_TEST's log) */
#define CL_VIEW_YAW         0x0096179Cu
#define COM_FRAME_TIME      0x00947710u     /* com_frameTime: the frame's time in ms, the base of a key's <time> */
#define PLAY_IDLE_MS        250

/* The key codes here: 1..511 SDL scancodes; three that stand for either side's key; 1000 + a mouse button (bro_pad.c's
 * MOUSE_KEY_BASE); the wheel. */
#define KEY_SHIFT           600
#define KEY_CTRL            601
#define KEY_ALT             602
#define KEY_MOUSE_BASE      1000
#define KEY_WHEEL_UP        1101
#define KEY_WHEEL_DOWN      1102
#define KEY_ID_BASE         3000            /* + the key code: the key's id in "+attack <id>" */

#define SLOTS               2               /* keys an action (CoD3's kSlots) */
#define MAX_ACTIONS_A_KEY   3               /* CoD3's kMaxActionsPerKey */

enum delivery { BUTTON, ONCE };

static const struct action {
    const char *id;
    enum delivery delivery;
    const char *command;
    const char *defaults;           /* CoD3's: up to two key names, comma-separated */
} actions[] = {
    /* Combat */
    { "fire", BUTTON, "attack", "MOUSE1" },
    { "aim", BUTTON, "speed", "MOUSE2" },
    { "use", BUTTON, "activate", "F" },
    { "reload", BUTTON, "reload", "R" },
    { "usereload", BUTTON, "activatereload", "" },
    { "melee", BUTTON, "melee", "V,MOUSE3" },
    { "usemelee", BUTTON, "activatemelee", "" },
    { "frag", BUTTON, "grenadeattack", "G" },
    { "sprint", BUTTON, "sprint", "SHIFT" },
    { "leanleft", BUTTON, "leanleft", "Q" },
    { "leanright", BUTTON, "leanright", "E" },
    { "leanleftnext", BUTTON, "leanleftswitchnext", "" },
    { "leanrightnext", BUTTON, "leanrightswitchnext", "" },
    /* Stance */
    { "jump", BUTTON, "gostand", "SPACE" },
    { "moveup", BUTTON, "moveup", "" },
    { "usemoveup", BUTTON, "activatemoveup", "" },
    { "crouch", ONCE, "togglecrouch", "C" },
    { "crouchhold", BUTTON, "movedown", "" },
    { "prone", ONCE, "toggleprone", "CTRL" },
    { "pronehold", BUTTON, "prone", "" },
    { "lowerstance", ONCE, "lowerstance", "" },
    { "raisestance", ONCE, "raisestance", "" },
    /* Weapons */
    { "weapnext", ONCE, "weapnext", "MWHEELDOWN" },
    { "weapprev", ONCE, "weapprev", "MWHEELUP" },
    { "weapalt", ONCE, "weapalt", "" },
    /* General */
    { "scores", BUTTON, "scores", "" },
};
#define ACTION_COUNT ((int)(sizeof actions / sizeof actions[0]))

static const struct { const char *name; int code; } key_names[] = {
#if defined(BRO_HAVE_SDL3)
    { "SPACE", SDL_SCANCODE_SPACE }, { "TAB", SDL_SCANCODE_TAB }, { "ENTER", SDL_SCANCODE_RETURN },
    { "RETURN", SDL_SCANCODE_RETURN }, { "ESCAPE", SDL_SCANCODE_ESCAPE }, { "ESC", SDL_SCANCODE_ESCAPE },
    { "BACKSPACE", SDL_SCANCODE_BACKSPACE }, { "CAPSLOCK", SDL_SCANCODE_CAPSLOCK }, { "PAUSE", SDL_SCANCODE_PAUSE },
    { "UPARROW", SDL_SCANCODE_UP }, { "DOWNARROW", SDL_SCANCODE_DOWN }, { "LEFTARROW", SDL_SCANCODE_LEFT },
    { "RIGHTARROW", SDL_SCANCODE_RIGHT }, { "INS", SDL_SCANCODE_INSERT }, { "DEL", SDL_SCANCODE_DELETE },
    { "HOME", SDL_SCANCODE_HOME }, { "END", SDL_SCANCODE_END }, { "PGUP", SDL_SCANCODE_PAGEUP },
    { "PGDN", SDL_SCANCODE_PAGEDOWN }, { "KP_ENTER", SDL_SCANCODE_KP_ENTER }, { "KP_STAR", SDL_SCANCODE_KP_MULTIPLY },
    { "KP_PLUS", SDL_SCANCODE_KP_PLUS }, { "KP_MINUS", SDL_SCANCODE_KP_MINUS }, { "KP_DEL", SDL_SCANCODE_KP_PERIOD },
    { "KP_SLASH", SDL_SCANCODE_KP_DIVIDE }, { "SEMICOLON", SDL_SCANCODE_SEMICOLON }, { ";", SDL_SCANCODE_SEMICOLON },
    { "=", SDL_SCANCODE_EQUALS }, { "COMMA", SDL_SCANCODE_COMMA }, { "-", SDL_SCANCODE_MINUS },
    { ".", SDL_SCANCODE_PERIOD }, { "/", SDL_SCANCODE_SLASH }, { "`", SDL_SCANCODE_GRAVE }, { "~", SDL_SCANCODE_GRAVE },
    { "[", SDL_SCANCODE_LEFTBRACKET }, { "\\", SDL_SCANCODE_BACKSLASH }, { "]", SDL_SCANCODE_RIGHTBRACKET },
    { "'", SDL_SCANCODE_APOSTROPHE },
#endif
    { "SHIFT", KEY_SHIFT }, { "CTRL", KEY_CTRL }, { "CONTROL", KEY_CTRL }, { "ALT", KEY_ALT },
    { "MOUSE1", KEY_MOUSE_BASE + 1 }, { "MOUSE2", KEY_MOUSE_BASE + 3 }, { "MOUSE3", KEY_MOUSE_BASE + 2 },
    { "MOUSE4", KEY_MOUSE_BASE + 4 }, { "MOUSE5", KEY_MOUSE_BASE + 5 },
    { "LMB", KEY_MOUSE_BASE + 1 }, { "RMB", KEY_MOUSE_BASE + 3 }, { "MMB", KEY_MOUSE_BASE + 2 },
    { "MWHEELUP", KEY_WHEEL_UP }, { "MWHEELDOWN", KEY_WHEEL_DOWN },
};
/* MOUSE2 is the right button and MOUSE3 the middle, as in CoD3 and the PC games; SDL numbers them left 1, middle 2,
 * right 3, and the codes here are SDL's (so bro_pad.c's [keys] "mouse3", SDL's right button, is MOUSE2 here). */

static struct {
    int ready, on;
    int keys[64][SLOTS];            /* per action, its key codes (0: none) */
    int down[2048];                 /* per key code: held at the last poll */
    int check_catchers;
} b;

static atomic_int wheel_up_notches, wheel_down_notches;

/* Text for the command buffer that didn't fit yet, in order. */
static char pending[4096];
static size_t pending_length;

static long long now_ms(void)
{
    struct timespec t;

    clock_gettime(CLOCK_MONOTONIC, &t);
    return (long long)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

static float mem_float(uint32_t address)
{
    union { uint32_t bits; float value; } v;

    v.bits = MEM32(address);
    return v.value;
}

/* A key's code from its name (CoD3's or SDL's), or 0. */
static int key_code(const char *name)
{
    size_t i;

    for (i = 0; i < sizeof key_names / sizeof key_names[0]; i++)
        if (!strcasecmp(name, key_names[i].name))
            return key_names[i].code;
#if defined(BRO_HAVE_SDL3)
    if (name[0] && !name[1]) {
        char c = name[0];

        if (c >= 'a' && c <= 'z')
            c = (char)(c - 'a' + 'A');
        if (c >= 'A' && c <= 'Z')
            return SDL_SCANCODE_A + (c - 'A');
        if (c >= '1' && c <= '9')
            return SDL_SCANCODE_1 + (c - '1');
        if (c == '0')
            return SDL_SCANCODE_0;
    }
    if ((name[0] == 'F' || name[0] == 'f') && atoi(name + 1) >= 1 && atoi(name + 1) <= 12)
        return SDL_SCANCODE_F1 + atoi(name + 1) - 1;
    if (!strncasecmp(name, "KP_", 3) && name[3] >= '0' && name[3] <= '9' && !name[4])
        return name[3] == '0' ? SDL_SCANCODE_KP_0 : SDL_SCANCODE_KP_1 + (name[3] - '1');
    {
        SDL_Scancode code = SDL_GetScancodeFromName(name);

        if (code != SDL_SCANCODE_UNKNOWN)
            return (int)code;
    }
#endif
    return 0;
}

static int action_by_id(const char *id)
{
    for (int a = 0; a < ACTION_COUNT; a++)
        if (!strcasecmp(id, actions[a].id))
            return a;
    return -1;
}

/* The actions that have this key now. */
static int key_uses(int code, int except_action)
{
    int uses = 0;

    for (int a = 0; a < ACTION_COUNT; a++)
        for (int s = 0; s < SLOTS; s++)
            if (a != except_action && b.keys[a][s] == code)
                uses++;
    return uses;
}

/* "MOUSE1" / "V, MOUSE3" / "" into an action's two slots. A key that has three actions already is left out. */
static void set_keys(int a, const char *list, const char *where)
{
    char copy[96], *name, *save = NULL;
    int slot = 0;

    b.keys[a][0] = b.keys[a][1] = 0;
    snprintf(copy, sizeof copy, "%s", list);
    for (name = strtok_r(copy, ",", &save); name && slot < SLOTS; name = strtok_r(NULL, ",", &save)) {
        int code;
        char *end;

        while (*name == ' ')
            name++;
        end = name + strlen(name);
        while (end > name && end[-1] == ' ')
            *--end = '\0';
        if (!*name)
            continue;
        code = key_code(name);
        if (!code) {
            fprintf(stderr, "[binds] %s: %s: no key called \"%s\"\n", where, actions[a].id, name);
            continue;
        }
        if (key_uses(code, a) >= MAX_ACTIONS_A_KEY) {
            fprintf(stderr, "[binds] %s: %s: %s has %d actions already\n", where, actions[a].id, name,
                    MAX_ACTIONS_A_KEY);
            continue;
        }
        b.keys[a][slot++] = code;
    }
}

/* CoD3's text: "fire=MOUSE1;aim=MOUSE2;use=F;...;sprint=" (an empty list unbinds). */
static void read_cod3_text(const char *text, int given[])
{
    char *copy = strdup(text), *entry, *save = NULL;

    for (entry = strtok_r(copy, ";", &save); entry; entry = strtok_r(NULL, ";", &save)) {
        char *equals = strchr(entry, '=');
        int a;

        if (!equals)
            continue;
        *equals = '\0';
        while (*entry == ' ')
            entry++;
        a = action_by_id(entry);
        if (a < 0)
            continue;               /* a CoD3 action Big Red One hasn't (smoke, binoculars, the vehicles'...) */
        set_keys(a, equals + 1, "input.binds_kbm");
        given[a] = 1;
    }
    free(copy);
}

static void read_binds(void)
{
    int given[ACTION_COUNT] = { 0 };
    const char *text;
    char key[48];
    int a;

    b.ready = 1;
    b.on = bro_setting_bool("input.binds", "BRO_BINDS", 1);
    b.check_catchers = bro_setting_bool("input.binds_menu_check", "BRO_BINDS_CATCHERS", 1);
    if (!b.on) {
        fprintf(stderr, "[binds] off (input.binds): every key is the controller's\n");
        return;
    }
    text = bro_setting_text("input.binds_kbm", "BRO_BINDS_KBM");
    if (text && *text)
        read_cod3_text(text, given);
    for (a = 0; a < ACTION_COUNT; a++) {
        snprintf(key, sizeof key, "binds.%s", actions[a].id);
        text = bro_setting_text(key, NULL);
        if (text) {
            set_keys(a, text, key);
            given[a] = 1;
        }
    }
    /* the rest keep CoD3's defaults, unless their key went to another action */
    for (a = 0; a < ACTION_COUNT; a++) {
        int s;

        if (given[a])
            continue;
        set_keys(a, actions[a].defaults, "default");
        for (s = 0; s < SLOTS; s++)
            if (b.keys[a][s] && key_uses(b.keys[a][s], a) > 0) {
                int taken = 0;

                for (int other = 0; other < ACTION_COUNT && !taken; other++)
                    for (int t = 0; t < SLOTS; t++)
                        if (other != a && given[other] && b.keys[other][t] == b.keys[a][s])
                            taken = 1;
                if (taken)
                    b.keys[a][s] = 0;
            }
    }
    fprintf(stderr, "[binds] in play the keys do CoD3's actions:");
    for (a = 0; a < ACTION_COUNT; a++)
        if (b.keys[a][0])
            fprintf(stderr, " %s", actions[a].id);
    fprintf(stderr, " (bro.toml [binds]); in menus they are the controller's\n");
}

/* Puts text into the game's command buffer as Cbuf_AddText does, after what is still waiting. */
static void flush_pending(void)
{
    uint32_t data = MEM32(CMD_TEXT_DATA);
    int32_t size = (int32_t)MEM32(CMD_TEXT_SIZE), length = (int32_t)MEM32(CMD_TEXT_LENGTH);

    if (!pending_length || !data || size <= 0 || length < 0)
        return;
    if (length + (int32_t)pending_length >= size)
        return;                     /* no room this time: the next poll */
    /* [acct2-ocr] 2026-11-12 (written by acct2-bro): the cast, which the Windows build needs */
    memcpy((void *)(uintptr_t)XBOX_PTR(data + (uint32_t)length), pending, pending_length);
    MEM32(CMD_TEXT_LENGTH) = (uint32_t)(length + (int32_t)pending_length);
    pending_length = 0;
}

static void send_text(const char *text)
{
    static int logged;
    size_t n = strlen(text);

    if (pending_length + n >= sizeof pending) {
        fprintf(stderr, "[binds] the command buffer is full; \"%.*s\" dropped\n", (int)n - 1, text);
        return;
    }
    memcpy(pending + pending_length, text, n);
    pending_length += n;
    if (logged < 200) {
        logged++;
        fprintf(stderr, "[binds] %.*s\n", (int)n - 1, text);
    }
}

/* The time a key's press or release goes with: the game's com_frameTime now (see the top of this file). */
static uint32_t key_time(void)
{
    return MEM32(COM_FRAME_TIME);
}

static void send_action(int a, int code, int pressed)
{
    char text[64];

    if (actions[a].delivery == BUTTON)
        snprintf(text, sizeof text, "%c%s %d %u\n", pressed ? '+' : '-', actions[a].command, KEY_ID_BASE + code,
                 (unsigned)key_time());
    else if (pressed)
        snprintf(text, sizeof text, "%s\n", actions[a].command);
    else
        return;
    send_text(text);
}

/* A key went down or up: every action it has. */
static void key_changed(int code, int pressed)
{
    for (int a = 0; a < ACTION_COUNT; a++)
        for (int s = 0; s < SLOTS; s++)
            if (b.keys[a][s] == code)
                send_action(a, code, pressed);
}

static void let_go_of_everything(void)
{
    for (int code = 0; code < (int)(sizeof b.down / sizeof b.down[0]); code++)
        if (b.down[code]) {
            b.down[code] = 0;
            key_changed(code, 0);
        }
}

/* [f2-menus] 2026-11-11 input.binds, input.binds_kbm and binds.* read again at the next poll (bro_pad.c's
 * bro_pad_reload, on the game's thread, so nothing races the poll). */
void bro_binds_reload(void)
{
    b.ready = 0;
}

int bro_binds_in_play(void)
{
    static uint32_t last_slot = 2, last_world;
    static long long last_flip_ms, last_world_ms = -1000000;
    static int last_catchers = -1, last_in_play = -1;
    uint32_t slot = MEM32(CL_MOUSE_INDEX) & 1u, world = ngl_mesh_world_lists();
    int catchers = MEM8(KEY_CATCHERS), in_play;
    long long now = now_ms();

    if (!b.ready)
        read_binds();
    if (slot != last_slot) {
        last_slot = slot;
        last_flip_ms = now;
    }
    if (world != last_world) {
        last_world = world;
        last_world_ms = now;
    }
    if (catchers != last_catchers) {
        fprintf(stderr, "[binds] key catchers 0x%02X (1: the console, 2: a menu)\n", catchers);
        last_catchers = catchers;
    }
    in_play = b.on && now - last_flip_ms <= PLAY_IDLE_MS && now - last_world_ms <= PLAY_IDLE_MS &&
              (!b.check_catchers || !(catchers & 3));
    if (in_play != last_in_play) {
        fprintf(stderr, "[binds] %s\n", in_play ? "in play: the keys do their actions"
                                                : "not in play: the keys are the controller's");
        if (!in_play)
            let_go_of_everything();
        last_in_play = in_play;
    }
    return in_play;
}

int bro_binds_claims(int pad_code)
{
#if defined(BRO_HAVE_SDL3)
    int either = 0;

    if (pad_code == SDL_SCANCODE_LSHIFT || pad_code == SDL_SCANCODE_RSHIFT)
        either = KEY_SHIFT;
    else if (pad_code == SDL_SCANCODE_LCTRL || pad_code == SDL_SCANCODE_RCTRL)
        either = KEY_CTRL;
    else if (pad_code == SDL_SCANCODE_LALT || pad_code == SDL_SCANCODE_RALT)
        either = KEY_ALT;
    if (either && key_uses(either, -1))
        return 1;
#endif
    return pad_code > 0 && key_uses(pad_code, -1) > 0;
}

#if defined(BRO_HAVE_SDL3)
/* The wheel's notches, as they come (SDL calls this where the events are queued). A high-resolution wheel's small
 * steps add up to whole notches. */
static bool SDLCALL wheel_watch(void *userdata, SDL_Event *event)
{
    static float rest;

    (void)userdata;
    if (event->type == SDL_EVENT_MOUSE_WHEEL) {
        float y = event->wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -event->wheel.y : event->wheel.y;

        rest += y;
        while (rest >= 1.0f) {
            atomic_fetch_add(&wheel_up_notches, 1);
            rest -= 1.0f;
        }
        while (rest <= -1.0f) {
            atomic_fetch_add(&wheel_down_notches, 1);
            rest += 1.0f;
        }
    }
    return true;
}

static int key_held(int code, const bool *keys, uint32_t mouse, int focused)
{
    if (code == KEY_SHIFT)
        return keys[SDL_SCANCODE_LSHIFT] || keys[SDL_SCANCODE_RSHIFT];
    if (code == KEY_CTRL)
        return keys[SDL_SCANCODE_LCTRL] || keys[SDL_SCANCODE_RCTRL];
    if (code == KEY_ALT)
        return keys[SDL_SCANCODE_LALT] || keys[SDL_SCANCODE_RALT];
    if (code > KEY_MOUSE_BASE && code <= KEY_MOUSE_BASE + 5)
        return focused && (mouse & SDL_BUTTON_MASK(code - KEY_MOUSE_BASE));
    if (code > 0 && code < SDL_SCANCODE_COUNT)
        return keys[code];
    return 0;
}
#endif

/* BRO_BINDS_TEST: commands at set times, counted in play only. */
static void run_test(int in_play)
{
    static int parsed;
    static struct { long long at, hold, pressed; char command[40]; int state; } steps[32];
    static int count;
    static long long played, last_poll = -1;     /* ms in play so far; the last poll in play */
    long long t, now;

    if (!parsed) {
        const char *spec = getenv("BRO_BINDS_TEST");
        char *copy, *entry, *save = NULL;

        parsed = 1;
        if (!spec || !*spec)
            return;
        copy = strdup(spec);
        for (entry = strtok_r(copy, ",", &save); entry && count < 32; entry = strtok_r(NULL, ",", &save)) {
            char *command, *hold;

            steps[count].at = strtoll(entry, &command, 10);
            if (*command != ':')
                continue;
            command++;
            hold = strchr(command, ':');
            steps[count].hold = 150;
            if (hold) {
                *hold++ = '\0';
                steps[count].hold = strtoll(hold, NULL, 10);
            }
            snprintf(steps[count].command, sizeof steps[count].command, "%s", command);
            steps[count].state = 0;
            count++;
        }
        free(copy);
        fprintf(stderr, "[binds] BRO_BINDS_TEST: %d steps, counted in play only\n", count);
    }
    now = now_ms();
    if (!count || !in_play) {
        last_poll = -1;
        return;
    }
    if (last_poll >= 0)
        played += now - last_poll;
    last_poll = now;
    t = played;
    for (int i = 0; i < count; i++) {
        char text[64];
        int button = steps[i].command[0] == '+';

        if (steps[i].state == 0 && t >= steps[i].at) {
            snprintf(text, sizeof text, "%s %d %u\n", steps[i].command, KEY_ID_BASE + 999, (unsigned)key_time());
            if (!button)
                snprintf(text, sizeof text, "%s\n", steps[i].command);
            send_text(text);
            steps[i].state = 1;
            steps[i].pressed = t;
            fprintf(stderr, "[binds] test %lld ms: %s; view pitch %.2f yaw %.2f, world fov %.1f\n", t,
                    steps[i].command, mem_float(CL_VIEW_PITCH), mem_float(CL_VIEW_YAW), ngl_mesh_world_fov());
        } else if (steps[i].state == 1 && (!button || t >= steps[i].pressed + steps[i].hold)) {
            if (button) {
                snprintf(text, sizeof text, "-%s %d %u\n", steps[i].command + 1, KEY_ID_BASE + 999,
                         (unsigned)key_time());
                send_text(text);
            }
            steps[i].state = 2;
            fprintf(stderr, "[binds] test %lld ms: %s let go; view pitch %.2f yaw %.2f, world fov %.1f\n", t,
                    steps[i].command, mem_float(CL_VIEW_PITCH), mem_float(CL_VIEW_YAW), ngl_mesh_world_fov());
        }
    }
}

void bro_binds_poll(const void *keys_state, uint32_t mouse, int focused)
{
    int in_play = bro_binds_in_play();

    flush_pending();
    run_test(in_play);
#if defined(BRO_HAVE_SDL3)
    {
        static int watching;
        const bool *keys = (const bool *)keys_state;

        if (!watching) {
            watching = 1;
            SDL_AddEventWatch(wheel_watch, NULL);
        }
        if (b.on && in_play && keys) {
            for (int a = 0; a < ACTION_COUNT; a++)
                for (int s = 0; s < SLOTS; s++) {
                    int code = b.keys[a][s], held;

                    if (!code || code == KEY_WHEEL_UP || code == KEY_WHEEL_DOWN || code >= (int)(sizeof b.down /
                                                                                               sizeof b.down[0]))
                        continue;
                    held = key_held(code, keys, mouse, focused);
                    if (held != b.down[code]) {
                        b.down[code] = held;
                        key_changed(code, held);
                    }
                }
        }
        {
            int up = atomic_exchange(&wheel_up_notches, 0), down = atomic_exchange(&wheel_down_notches, 0);

            for (; b.on && in_play && focused && up > 0; up--) {
                key_changed(KEY_WHEEL_UP, 1);
                key_changed(KEY_WHEEL_UP, 0);
            }
            for (; b.on && in_play && focused && down > 0; down--) {
                key_changed(KEY_WHEEL_DOWN, 1);
                key_changed(KEY_WHEEL_DOWN, 0);
            }
        }
    }
#else
    (void)keys_state;
    (void)mouse;
    (void)focused;
#endif
    flush_pending();
}

int bro_binds_aim_held(void)
{
    int a = action_by_id("aim");

    for (int s = 0; a >= 0 && s < SLOTS; s++) {
        int code = b.keys[a][s];

        if (code > 0 && code < (int)(sizeof b.down / sizeof b.down[0]) && b.down[code])
            return 1;
    }
    return 0;
}
