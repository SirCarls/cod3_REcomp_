/* [acct2-fh] 2026-11-13 Finest Hour port: the gamepad, at XAPI's level (Big Red One's route: bro/src/input/bro_pad.c).
 *
 * On the Xbox the game reaches its controllers through XAPI, statically linked in its XPP section, which drives the
 * USB host controllers directly. xboxrecomp's controller model can't trap those registers on an x86-64 Linux host,
 * so the game never saw a pad: the title screen's "Press START" could not be answered. These seven functions are
 * stand-ins here (fh/config/stand_in_functions.tsv, group "input"):
 *
 *   0x002D43C0 XGetDevices            0x002D44A5 XInputClose             0x002D46FC XInputSetState
 *   0x002D43E2 XGetDeviceChanges      0x002D44B1 XInputGetCapabilities
 *   0x002D444F XInputOpen             0x002D4689 XInputGetState
 *
 * Found by our own byte match against Big Red One's XAPI (the same XAPILIB 1.0.5849): five by acct2-bro's match of
 * 11-08 (COD3-rea-work/acct2-fh/v3/names/matches.tsv), XInputOpen and XInputClose by
 * dev/agents/acct2-fh/tools/fh_xpp_find.py (the same instructions at the same distance before XInputGetCapabilities
 * as in Big Red One). The game's pad code (0x0012D000 to 0x0012D800) passes the gamepad's device type, 0x002D23FC, to
 * XGetDevices, XGetDeviceChanges, XInputOpen and XInputGetCapabilities; it also asks XGetDevices about 0x002D2534
 * and 0x002D2540 (0x0012B709, 0x0012B715), other device types (the memory unit's among them), which get no device.
 *
 * One gamepad, in the first port. Its buttons come from a real controller (SDL3's gamepads, the first one plugged
 * in: A B X Y, the shoulders as white (left) and black (right), the triggers, the sticks, the d-pad, start, back and
 * the stick clicks), from the keyboard while the game's window has the focus (W A S D the left stick, the arrow keys
 * the d-pad, I J K L the right stick, Return start, Escape back, Space A, Left Ctrl B, R X, F Y, Q white, E black, Z
 * the left trigger, X the right trigger, Left Shift and V the stick clicks), and from FH_PAD_SCRIPT, all at once.
 * Test runs use the script, since their window never takes the focus:
 *
 *   FH_PAD_SCRIPT=6000:start,9000:a,12000:down,13000:a:400
 *
 * Each entry is <ms>:<button>[+<button>...][:<hold ms>], counted from the game's first XInputGetState, held 150 ms
 * unless a hold is given. Buttons: up down left right start back lthumb rthumb, and the analog a b x y black white
 * lt rt (pressed fully); the sticks pushed all the way: move_forward move_back move_left move_right (the left stick),
 * look_up look_down look_left look_right (the right stick). FH_PAD=0 unplugs the pad (as before this file).
 *
 * [input-port] 2026-11-24: the keyboard's keys are settings now (fh.toml [input] binds_kbm, FH_BINDS_KBM), read by
 * inputkit, the project's input code that Call of Duty 3 and Big Red One use too: the same key names and the same
 * rules (two keys an action, a key for up to three; "" the defaults above). Each pad button and stick direction is an
 * action named as in FH_PAD_SCRIPT: "a=SPACE;rt=X,MOUSE1;move_forward=W,UPARROW". Mouse buttons can be bound
 * (MOUSE1..MOUSE5); the wheel can't, since the pad is read and the wheel only ever sends events. Escape is always
 * back as well, whatever the binds say: inputkit keeps Escape (and ` / ~) for every game's menu and console, so it
 * can't be in the table, and the defaults keep it as before. Left and right Shift (and Ctrl) are one key to inputkit,
 * as to Call of Duty 3: the default lthumb is SHIFT, either side, where it was Left Shift only. fh.toml [input]
 * dead_zone (FH_DEAD_ZONE) gives the controller's sticks a radial dead zone (inputkit's, Big Red One's), 0 none (as
 * before). Both are read again when the F2 window or the console changes a setting.
 * Rumble (XInputSetState) is taken and reported done.
 *
 * [acct2-fh] 2026-11-22: with game.fixed_step_above_60 (fh_frame_clock.c), above 60 frames a second most frames hold
 * the game's update. On those the game is told the pad as it was at the last frame that updated, and what was
 * pressed meanwhile is kept: the next frame that updates sees every button and analog button pressed since (held
 * down, at its most), with the sticks as they are then. So the game sees the pad change only between updates, 60
 * times a second as on the console, and no press made between two updates is lost. */
#include "fh_pad.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "recomp_types.h"           /* MEM8 / MEM16 / MEM32: guest memory */
#include "f2/fh_f2.h"               /* [acct2-fh] 11-14: the F2 window has the keyboard and pad while open */
#include "fh_host.h"                /* [waw-decomp/fh2] 11-23: a steady clock */
#include "fh_frame_clock.h"         /* [acct2-fh] 11-22: whether this frame updates (the fixed step) */
#include "fh_settings.h"            /* [waw-decomp/fh14] 11-24: game.pad_keep_presses */

#if defined(FH_HAVE_SDL3)
#include <SDL3/SDL.h>

#include "inputkit/inputkit.h"      /* [input-port] 11-24: the key names, the binds and the dead zone */
#include "input/gamepad_db.h"       /* [input-port] 11-24: SDL_GameControllerDB, shared with BRO and the SDK */
#endif

#define GAMEPAD_TYPE        0x002D23FCu     /* the XPP device type the game's pad code passes */
#define PAD_HANDLE          0x50AD0001u     /* what XInputOpen hands back for the one pad */

#define ERROR_SUCCESS       0u
#define ERROR_NOT_CONNECTED 1167u           /* ERROR_DEVICE_NOT_CONNECTED */

/* XINPUT_STATE: dwPacketNumber, then XINPUT_GAMEPAD (wButtons, bAnalogButtons[8], four thumb shorts) */
#define STATE_PACKET        0x00u
#define STATE_BUTTONS       0x04u
#define STATE_ANALOG        0x06u
#define STATE_THUMBS        0x0Eu
/* XINPUT_CAPABILITIES: SubType, Reserved, In (an XINPUT_GAMEPAD), Out (rumble: two WORDs) */
#define CAPS_SUBTYPE        0x00u
#define CAPS_IN             0x02u
#define CAPS_OUT            0x18u
#define CAPS_BYTES          0x1Cu
#define GAMEPAD_BYTES       0x12u
#define SUBTYPE_GAMEPAD     0x01u

#define MAX_PRESSES         256
#define DEFAULT_HOLD_MS     150
#define PRESSED             0x20            /* an analog button counts as pressed from here (the packet's bits) */

/* The pad's inputs: the eight digital buttons (wButtons bits), then the eight analog ones (bAnalogButtons). */
static const struct { const char *name; uint16_t digital; int analog; } button_names[] = {
    { "up", 0x0001, -1 }, { "down", 0x0002, -1 }, { "left", 0x0004, -1 }, { "right", 0x0008, -1 },
    { "start", 0x0010, -1 }, { "back", 0x0020, -1 }, { "lthumb", 0x0040, -1 }, { "rthumb", 0x0080, -1 },
    { "a", 0, 0 }, { "b", 0, 1 }, { "x", 0, 2 }, { "y", 0, 3 }, { "black", 0, 4 }, { "white", 0, 5 },
    { "lt", 0, 6 }, { "rt", 0, 7 },
};
#define BUTTON_COUNT ((int)(sizeof button_names / sizeof button_names[0]))

/* The sticks: the thumb (0 left x, 1 left y, 2 right x, 3 right y) and the way it is pushed (y up-positive). */
static const struct { const char *name; int thumb, sign; } stick_names[] = {
    { "move_forward", 1, 1 }, { "move_back", 1, -1 }, { "move_left", 0, -1 }, { "move_right", 0, 1 },
    { "look_up", 3, 1 }, { "look_down", 3, -1 }, { "look_left", 2, -1 }, { "look_right", 2, 1 },
};
#define STICK_DIRECTIONS ((int)(sizeof stick_names / sizeof stick_names[0]))

static struct press {
    unsigned long at_ms, hold_ms;
    uint16_t digital;
    uint8_t analog;         /* bit n: analog button n */
    int8_t thumbs[4];       /* -1, 0 or 1: each stick axis pushed all the way, or not */
    int announced;
} presses[MAX_PRESSES];
static int press_count = -1;        /* -1: script not read yet */
static long long start_ms = -1;
static int reported_insert;
static uint32_t packet;
static uint16_t last_digital;
static uint8_t last_analog_values[8];
static int16_t last_thumbs[4];
static uint16_t held_digital;           /* [acct2-fh] 11-22: pressed during frames that held the update */
static uint8_t held_analog[8];
static int held_any;                    /* something was read on a held frame since the last update */

/* [waw-decomp/fh14] 2026-11-24: presses kept below 60 frames a second too (game.pad_keep_presses /
 * FH_PAD_KEEP_PRESSES, default on). The pad is read once a frame, and below 60 a frame can be longer than a quick
 * tap: a tap made and let go between two reads was never seen, and two presses in one frame were one (the game saw
 * the button stay down). Every press is now counted as it comes (SDL's button and key events, and the script's), and
 * each read that updates the game shows the next press not shown yet: one let go already is shown down for that one
 * update, and one made while the game still sees the button down is shown after one update with it up. So the game
 * sees every press, as many as were made, at any frame rate (CoD3's audit, S1: the same loss there). */
static int keep_presses = -1;
static int press_owed[16];              /* presses made and not shown yet, in button_names' order */
static unsigned long presses_kept;      /* shown that the frame's read alone would have lost */
static long long script_seen_t = -1;    /* the script's time at the last read */
static int script_downs[16];            /* the script's presses begun since the last update */
static int script_ups[16];              /* and let go since */
static int let_go[16];                  /* let go since the game was last shown it down */
static uint8_t watched_pause = 0xFF;    /* the game's pause bit (0x003B46D0 & 0x10), logged with a script */

/* [waw-decomp/fh2] 11-23: a steady clock on both systems (QueryPerformanceCounter on Windows, where this read the
 * wall clock, which a time sync can move; fh_host.c) */
static long long now_ms(void)
{
    return (long long)(fh_host_now_ns() / 1000000u);
}

static int pad_plugged(void)
{
    const char *setting = getenv("FH_PAD");

    return !(setting && setting[0] == '0');
}

/* ------------------------------------------------------------------------------------------------------------------
 * FH_PAD_SCRIPT */

static void read_script(void)
{
    const char *spec = getenv("FH_PAD_SCRIPT");
    char *copy, *entry, *save = NULL;

    press_count = 0;
    if (!spec || !*spec)
        return;
    copy = strdup(spec);
    for (entry = strtok_r(copy, ",", &save); entry && press_count < MAX_PRESSES; entry = strtok_r(NULL, ",", &save)) {
        struct press p = { 0, DEFAULT_HOLD_MS, 0, 0, { 0, 0, 0, 0 }, 0 };
        char *names, *hold, *name, *save_name = NULL;

        p.at_ms = strtoul(entry, &names, 10);
        if (*names != ':')
            continue;
        names++;
        hold = strchr(names, ':');
        if (hold) {
            *hold++ = '\0';
            p.hold_ms = strtoul(hold, NULL, 10);
        }
        for (name = strtok_r(names, "+", &save_name); name; name = strtok_r(NULL, "+", &save_name)) {
            int i, j;

            for (i = 0; i < BUTTON_COUNT && strcmp(name, button_names[i].name); i++)
                ;
            if (i < BUTTON_COUNT) {
                if (button_names[i].analog < 0)
                    p.digital |= button_names[i].digital;
                else
                    p.analog |= (uint8_t)(1u << button_names[i].analog);
                continue;
            }
            for (j = 0; j < STICK_DIRECTIONS && strcmp(name, stick_names[j].name); j++)
                ;
            if (j < STICK_DIRECTIONS)
                p.thumbs[stick_names[j].thumb] = (int8_t)stick_names[j].sign;
            else
                fprintf(stderr, "[pad] FH_PAD_SCRIPT: no button called \"%s\"\n", name);
        }
        presses[press_count++] = p;
    }
    free(copy);
    fprintf(stderr, "[pad] FH_PAD_SCRIPT: %d presses\n", press_count);
}

/* The buttons held and the sticks pushed now, from the script. */
static void script_now(uint16_t *digital, uint8_t analog_values[8], int16_t thumbs[4])
{
    long long t;
    int i, k;

    if (press_count < 0)
        read_script();
    if (start_ms < 0)
        start_ms = now_ms();
    t = now_ms() - start_ms;
    for (i = 0; i < press_count; i++) {
        struct press *p = &presses[i];

        if ((long long)p->at_ms > script_seen_t && (long long)p->at_ms <= t)   /* [waw-decomp/fh14] begun since */
            for (k = 0; k < 16; k++)
                if (k < 8 ? (p->digital & (1u << k)) != 0 : ((p->analog >> (k - 8)) & 1) != 0)
                    script_downs[k]++;
        if ((long long)(p->at_ms + p->hold_ms) > script_seen_t && (long long)(p->at_ms + p->hold_ms) <= t)
            for (k = 0; k < 16; k++)
                if (k < 8 ? (p->digital & (1u << k)) != 0 : ((p->analog >> (k - 8)) & 1) != 0)
                    script_ups[k]++;
        if (t < (long long)p->at_ms || t >= (long long)(p->at_ms + p->hold_ms))
            continue;
        *digital |= p->digital;
        for (k = 0; k < 8; k++)
            if ((p->analog >> k) & 1)
                analog_values[k] = 0xFF;
        for (k = 0; k < 4; k++)
            if (p->thumbs[k])
                thumbs[k] = p->thumbs[k] > 0 ? 32767 : -32767;
        if (!p->announced) {
            p->announced = 1;
            fprintf(stderr, "[pad] %lld ms: pressing digital 0x%04X analog 0x%02X sticks %d %d %d %d for %lu ms\n",
                    t, p->digital, p->analog, p->thumbs[0], p->thumbs[1], p->thumbs[2], p->thumbs[3], p->hold_ms);
        }
    }
    script_seen_t = t;
}

/* ------------------------------------------------------------------------------------------------------------------
 * A real controller and the keyboard (SDL3) */

#if defined(FH_HAVE_SDL3)
static int16_t clamp_thumb(int value)
{
    return (int16_t)(value > 32767 ? 32767 : value < -32767 ? -32767 : value);
}

/* The controller as an Xbox pad, in button_names' order (-1: an axis, below). */
static const int pad_buttons[16] = {
    SDL_GAMEPAD_BUTTON_DPAD_UP, SDL_GAMEPAD_BUTTON_DPAD_DOWN, SDL_GAMEPAD_BUTTON_DPAD_LEFT,
    SDL_GAMEPAD_BUTTON_DPAD_RIGHT, SDL_GAMEPAD_BUTTON_START, SDL_GAMEPAD_BUTTON_BACK, SDL_GAMEPAD_BUTTON_LEFT_STICK,
    SDL_GAMEPAD_BUTTON_RIGHT_STICK, SDL_GAMEPAD_BUTTON_SOUTH, SDL_GAMEPAD_BUTTON_EAST, SDL_GAMEPAD_BUTTON_WEST,
    SDL_GAMEPAD_BUTTON_NORTH, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, -1, -1,
};
static const int pad_axes[16] = {
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    SDL_GAMEPAD_AXIS_LEFT_TRIGGER, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER,
};
/* [input-port] 2026-11-24: the keyboard's actions, in button_names' order, then stick_names' order, with the keys
 * that were fixed here before as their defaults. "back" has none: Escape presses it always (see the top). */
#define KEY_ACTIONS (16 + 8)
static const char *const key_defaults[KEY_ACTIONS] = {
    "UPARROW", "DOWNARROW", "LEFTARROW", "RIGHTARROW", "ENTER", "", "SHIFT", "V",
    "SPACE", "CTRL", "R", "F", "E", "Q", "Z", "X",
    "W", "S", "A", "D", "I", "K", "J", "L",
};
#define ESCAPE_ACTION 5             /* button_names' "back" */
#define IK_ESCAPE 0x1B              /* inputkit's code for Escape (its codes are Windows' virtual keys) */

static struct {
    int ready;
    unsigned generation;            /* fh_settings_generation() when read */
    ik_binds *binds;
    uint16_t keys[KEY_ACTIONS][IK_SLOTS];
    float dead_zone;
} kb;

/* Reads input.binds_kbm and input.dead_zone, at the first poll and again after a setting changes in play. */
static void read_keys(void)
{
    static ik_action table[KEY_ACTIONS];
    int a, s, notes;

    if (kb.ready && kb.generation == fh_settings_generation())
        return;
    kb.ready = 1;
    kb.generation = fh_settings_generation();
    if (!kb.binds) {
        for (a = 0; a < KEY_ACTIONS; a++) {
            table[a].id = a < BUTTON_COUNT ? button_names[a].name : stick_names[a - BUTTON_COUNT].name;
            table[a].label = table[a].id;
            table[a].kind = 0;
            table[a].defaults = key_defaults[a];
        }
        kb.binds = ik_binds_new(table, KEY_ACTIONS, "input.binds_kbm");
    }
    notes = ik_binds_parse(kb.binds, fh_setting_text("input.binds_kbm", "FH_BINDS_KBM"));
    for (int i = 0; i < notes; i++)
        fprintf(stderr, "[pad] keys: %s\n", ik_binds_note(kb.binds, i));
    for (a = 0; a < KEY_ACTIONS; a++)
        for (s = 0; s < IK_SLOTS; s++)
            kb.keys[a][s] = ik_binds_key(kb.binds, a, s);
    kb.dead_zone = (float)fh_setting_number("input.dead_zone", "FH_DEAD_ZONE", 0.0);
    if (kb.dead_zone < 0.0f || kb.dead_zone > 0.9f) {
        fprintf(stderr, "[pad] input.dead_zone %.2f is outside 0 to 0.9; none\n", kb.dead_zone);
        kb.dead_zone = 0.0f;
    }
}

/* Which keys (inputkit's codes) are down now: the keyboard, then the mouse's buttons. */
static void keys_held(uint8_t held[IK_KEY_CODE_COUNT])
{
    int count = 0;
    const bool *keys = SDL_GetKeyboardState(&count);
    SDL_MouseButtonFlags buttons = SDL_GetMouseState(NULL, NULL);

    memset(held, 0, IK_KEY_CODE_COUNT);
    for (int sc = 0; keys && sc < count; sc++)
        if (keys[sc])
            held[ik_key_from_sdl_scancode(sc)] = 1;
    for (int button = 1; button <= 5; button++)
        if (buttons & SDL_BUTTON_MASK(button))
            held[ik_key_from_sdl_mouse_button(button)] = 1;
    held[IK_NO_KEY] = 0;            /* keys inputkit has no name for */
}

static int action_held(const uint8_t held[IK_KEY_CODE_COUNT], int a)
{
    for (int s = 0; s < IK_SLOTS; s++)
        if (kb.keys[a][s] != IK_NO_KEY && held[kb.keys[a][s]])
            return 1;
    return 0;
}

/* [waw-decomp-fh2] r17 11-24: a key event's key is one of button i's binds (the presses kept, fh14, with the
 * input-port binds): Escape is always back too */
static int key_is_action(int scancode, int i)
{
    uint16_t code = ik_key_from_sdl_scancode(scancode);

    if (code == IK_NO_KEY)
        return 0;
    if (i == ESCAPE_ACTION && code == IK_ESCAPE)
        return 1;
    if (!kb.ready)
        return 0;
    for (int s = 0; s < IK_SLOTS; s++)
        if (kb.keys[i][s] == code)
            return 1;
    return 0;
}

static int controllers_started;     /* 1 started, -1 failed */

/* [waw-decomp/fh14] 11-24: every press as it comes (SDL calls this as each event is queued, on any thread) */
static SDL_AtomicInt sdl_downs[16], sdl_ups[16];
static int trigger_down[2];

static bool SDLCALL count_presses(void *unused, SDL_Event *event)
{
    int i;

    (void)unused;
    switch (event->type) {
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
        for (i = 0; i < 16; i++)
            if (pad_buttons[i] == (int)event->gbutton.button)
                SDL_AddAtomicInt(&sdl_downs[i], 1);
        break;
    case SDL_EVENT_GAMEPAD_BUTTON_UP:
        for (i = 0; i < 16; i++)
            if (pad_buttons[i] == (int)event->gbutton.button)
                SDL_AddAtomicInt(&sdl_ups[i], 1);
        break;
    case SDL_EVENT_GAMEPAD_AXIS_MOTION:
        for (i = 14; i < 16; i++)
            if (pad_axes[i] == (int)event->gaxis.axis) {
                int down = event->gaxis.value / 128 >= PRESSED;

                if (down && !trigger_down[i - 14])
                    SDL_AddAtomicInt(&sdl_downs[i], 1);
                if (!down && trigger_down[i - 14])
                    SDL_AddAtomicInt(&sdl_ups[i], 1);
                trigger_down[i - 14] = down;
            }
        break;
    case SDL_EVENT_KEY_UP:
        for (i = 0; i < 16; i++)
            if (key_is_action((int)event->key.scancode, i))
                SDL_AddAtomicInt(&sdl_ups[i], 1);
        break;
    case SDL_EVENT_KEY_DOWN:
        if (!event->key.repeat)
            for (i = 0; i < 16; i++)
                if (key_is_action((int)event->key.scancode, i))
                    SDL_AddAtomicInt(&sdl_downs[i], 1);
        break;
    default:
        break;
    }
    return true;
}
static SDL_Gamepad *controller;
static long long last_scan_ms = -1000000;

/* [input-port] 2026-11-24 (owner: "make the controller module from our cod3 project work in the others", and the
 * SDL_GameControllerDB link): Finest Hour was the one game that never read the controller database, so a pad SDL
 * has no built-in mapping for (many Switch, GameCube and third-party pads) never became a gamepad here. As BRO does
 * (bro_pad.c, the same gamepad_db.h), the database is read once SDL's gamepads are up and before one is opened:
 * gamecontrollerdb.txt next to the program (the copy shipped with it), then gamecontrollerdb.txt in the folder
 * above it (your own lines, which win), then SDL's SDL_GAMECONTROLLERCONFIG_FILE and SDL_GAMECONTROLLERCONFIG.
 * A missing file is only noted. A joystick no mapping makes a gamepad is said once, by GUID, with how to map it. */
static char user_mappings[4200];
static char unmapped_noted[16][33];
static int unmapped_count;

static void mappings_log(void *ctx, int level, const char *text)
{
    (void)ctx;
    fprintf(stderr, "[pad] %s%s\n", level == GDB_WARN ? "warning: " : level == GDB_ERROR ? "error: " : "", text);
}

static void load_mappings(void)
{
    const char *base = SDL_GetBasePath();   /* the program's folder, UTF-8, ending in its separator */
    char shipped[4200] = "";

    if (base && base[0]) {
        snprintf(shipped, sizeof shipped, "%sgamecontrollerdb.txt", base);
        snprintf(user_mappings, sizeof user_mappings, "%s..%cgamecontrollerdb.txt", base,
                 base[strlen(base) - 1] == '\\' ? '\\' : '/');
    }
    gdb_load_all(shipped, user_mappings, mappings_log, NULL);
#if defined(__linux__)
    gdb_linux_access_hints(mappings_log, NULL);     /* a pad SDL can't open (no /dev/hidraw access) is said once */
#endif
}

/* A joystick SDL sees but can't treat as a gamepad: say so once per GUID (at most 16), instead of nothing. */
static void note_unmapped_joysticks(void)
{
    int count = 0, i, j;
    SDL_JoystickID *joysticks = SDL_GetJoysticks(&count);

    for (i = 0; joysticks && i < count; i++) {
        char guid[33];
        int seen = 0;

        if (SDL_IsGamepad(joysticks[i]))
            continue;
        SDL_GUIDToString(SDL_GetJoystickGUIDForID(joysticks[i]), guid, (int)sizeof guid);
        for (j = 0; j < unmapped_count; j++)
            seen |= strcmp(unmapped_noted[j], guid) == 0;
        if (seen || unmapped_count >= 16)
            continue;
        if (gdb_note_joystick(joysticks[i], user_mappings, mappings_log, NULL))
            memcpy(unmapped_noted[unmapped_count++], guid, sizeof guid);
    }
    SDL_free(joysticks);
}

static void real_pad(uint16_t *digital, uint8_t analog_values[8], int16_t thumbs[4])
{
    int i;

    read_keys();
    if (!controllers_started) {
        controllers_started = SDL_InitSubSystem(SDL_INIT_GAMEPAD) ? 1 : -1;
        SDL_AddEventWatch(count_presses, NULL);     /* [waw-decomp/fh14] 11-24 */
        if (controllers_started < 0)
            fprintf(stderr, "[pad] no game controllers (%s): the keyboard and FH_PAD_SCRIPT only\n", SDL_GetError());
        else
            load_mappings();    /* before any SDL_OpenGamepad, so the database's mappings apply to the first pad */
    }
    if (controllers_started > 0) {
        SDL_UpdateGamepads();
        if (controller && !SDL_GamepadConnected(controller)) {
            fprintf(stderr, "[pad] controller unplugged\n");
            SDL_CloseGamepad(controller);
            controller = NULL;
        }
        if (!controller && now_ms() - last_scan_ms >= 1000) {   /* look for one once a second */
            int count = 0;
            SDL_JoystickID *pads = SDL_GetGamepads(&count);

            last_scan_ms = now_ms();
            for (int n = 0; pads && n < count && !controller; n++)
                if ((controller = SDL_OpenGamepad(pads[n])) != NULL)
                    fprintf(stderr, "[pad] controller: %s\n", SDL_GetGamepadName(controller));
            SDL_free(pads);
            if (!controller)
                note_unmapped_joysticks();
        }
    }
    if (controller) {
        for (i = 0; i < BUTTON_COUNT; i++) {
            int value = 0;

            if (pad_buttons[i] >= 0)
                value = SDL_GetGamepadButton(controller, (SDL_GamepadButton)pad_buttons[i]) ? 0xFF : 0;
            else if (pad_axes[i] >= 0)
                value = SDL_GetGamepadAxis(controller, (SDL_GamepadAxis)pad_axes[i]) / 128;
            if (value <= 0)
                continue;
            if (button_names[i].analog < 0) {
                if (value >= PRESSED)
                    *digital |= button_names[i].digital;
            } else if (value > analog_values[button_names[i].analog]) {
                analog_values[button_names[i].analog] = (uint8_t)(value > 0xFF ? 0xFF : value);
            }
        }
        /* [input-port] 11-24: each stick's dead zone first (none unless input.dead_zone), on the controller alone */
        int lx = SDL_GetGamepadAxis(controller, SDL_GAMEPAD_AXIS_LEFTX);
        int ly = -SDL_GetGamepadAxis(controller, SDL_GAMEPAD_AXIS_LEFTY);
        int rx = SDL_GetGamepadAxis(controller, SDL_GAMEPAD_AXIS_RIGHTX);
        int ry = -SDL_GetGamepadAxis(controller, SDL_GAMEPAD_AXIS_RIGHTY);

        inputkit_radial_dead_zone(&lx, &ly, kb.dead_zone);
        inputkit_radial_dead_zone(&rx, &ry, kb.dead_zone);
        thumbs[0] = clamp_thumb(thumbs[0] + lx);
        thumbs[1] = clamp_thumb(thumbs[1] + ly);
        thumbs[2] = clamp_thumb(thumbs[2] + rx);
        thumbs[3] = clamp_thumb(thumbs[3] + ry);
    }
    if (SDL_GetKeyboardFocus()) {    /* only while the game's window has the focus */
        uint8_t held[IK_KEY_CODE_COUNT];
        int dir[8];

        keys_held(held);
        for (i = 0; i < BUTTON_COUNT; i++) {
            if (!action_held(held, i) && !(i == ESCAPE_ACTION && held[IK_ESCAPE]))
                continue;
            if (button_names[i].analog < 0)
                *digital |= button_names[i].digital;
            else
                analog_values[button_names[i].analog] = 0xFF;
        }
        for (i = 0; i < STICK_DIRECTIONS; i++)
            dir[i] = action_held(held, BUTTON_COUNT + i) ? 32767 : 0;
        thumbs[1] = clamp_thumb(thumbs[1] + dir[0] - dir[1]);   /* forward, back */
        thumbs[0] = clamp_thumb(thumbs[0] + dir[3] - dir[2]);   /* right, left */
        thumbs[3] = clamp_thumb(thumbs[3] + dir[4] - dir[5]);   /* up, down */
        thumbs[2] = clamp_thumb(thumbs[2] + dir[7] - dir[6]);   /* right, left */
    }
}
#endif

/* ------------------------------------------------------------------------------------------------------------------
 * XAPI */

static int is_pad(uint32_t handle)
{
    return handle == PAD_HANDLE && pad_plugged();
}

uint32_t fh_pad_get_devices(uint32_t device_type)
{
    return device_type == GAMEPAD_TYPE && pad_plugged() ? 1u : 0u;      /* bit 0: the first port */
}

uint32_t fh_pad_get_device_changes(uint32_t device_type, uint32_t insertions, uint32_t removals)
{
    uint32_t inserted = 0;

    if (device_type == GAMEPAD_TYPE && pad_plugged() && !reported_insert) {
        reported_insert = 1;
        inserted = 1;
        fprintf(stderr, "[pad] a gamepad in the first port\n");
    }
    if (insertions)
        MEM32(insertions) = inserted;
    if (removals)
        MEM32(removals) = 0;
    return inserted ? 1u : 0u;
}

uint32_t fh_pad_open(uint32_t device_type, uint32_t port, uint32_t slot, uint32_t polling)
{
    (void)slot;
    (void)polling;
    return device_type == GAMEPAD_TYPE && port == 0 && pad_plugged() ? PAD_HANDLE : 0u;
}

uint32_t fh_pad_close(uint32_t handle)
{
    (void)handle;
    return 0;
}

uint32_t fh_pad_get_capabilities(uint32_t handle, uint32_t capabilities)
{
    uint32_t i;

    if (!is_pad(handle) || !capabilities)
        return ERROR_NOT_CONNECTED;
    for (i = 0; i < CAPS_BYTES; i++)
        MEM8(capabilities + i) = 0;
    MEM8(capabilities + CAPS_SUBTYPE) = SUBTYPE_GAMEPAD;
    for (i = 0; i < GAMEPAD_BYTES; i++)     /* every button and axis there, as a full Xbox controller reports them */
        MEM8(capabilities + CAPS_IN + i) = 0xFF;
    MEM16(capabilities + CAPS_OUT) = 0xFFFF;
    MEM16(capabilities + CAPS_OUT + 2) = 0xFFFF;
    return ERROR_SUCCESS;
}

/* [waw-decomp/fh14] 11-24: presses kept (see the top). The presses begun since the last update, one button. */
static int take_downs(int i, int counting)
{
    int n = script_downs[i];

    script_downs[i] = 0;
#if defined(FH_HAVE_SDL3)
    n += SDL_SetAtomicInt(&sdl_downs[i], 0);
#endif
    return counting ? n : 0;
}

static int take_ups(int i, int counting)
{
    int n = script_ups[i];

    script_ups[i] = 0;
#if defined(FH_HAVE_SDL3)
    n += SDL_SetAtomicInt(&sdl_ups[i], 0);
#endif
    return counting ? n : 0;
}

static int is_down(int i, uint16_t digital, const uint8_t analog_values[8])
{
    int a = button_names[i].analog;

    return a < 0 ? (digital & button_names[i].digital) != 0 : analog_values[a] >= PRESSED;
}

static void show(int i, int down, uint16_t *digital, uint8_t analog_values[8])
{
    int a = button_names[i].analog;

    if (a < 0)
        *digital = (uint16_t)(down ? *digital | button_names[i].digital : *digital & ~button_names[i].digital);
    else if (down) {
        if (analog_values[a] < PRESSED)
            analog_values[a] = 0xFF;
    } else {
        analog_values[a] = 0;
    }
}

static void keep_the_presses(uint16_t *digital, uint8_t analog_values[8], int counting)
{
    int i;

    if (keep_presses < 0) {
        keep_presses = fh_setting_bool("game.pad_keep_presses", "FH_PAD_KEEP_PRESSES", 1);
        fprintf(stderr, "[pad] presses %s\n", keep_presses
                ? "kept: every press reaches the game, however short and however low the frame rate (game.pad_keep_presses)"
                : "as read once a frame (game.pad_keep_presses off)");
    }
    for (i = 0; i < BUTTON_COUNT && i < 16; i++) {
        int was = is_down(i, last_digital, last_analog_values);
        int now = is_down(i, *digital, analog_values);
        int downs = take_downs(i, counting);

        if (take_ups(i, counting))
            let_go[i] = 1;
        if (!keep_presses)
            continue;
        press_owed[i] += downs;
        if (press_owed[i] > 8)
            press_owed[i] = 8;
        if (press_owed[i] > 0) {
            if (!was) {
                if (!now)
                    presses_kept++;
                show(i, 1, digital, analog_values);
                press_owed[i]--;
            } else if (let_go[i]) {
                show(i, 0, digital, analog_values);         /* up for one update, so the next press is one */
            } else {
                press_owed[i]--;                            /* the press the game is shown already */
            }
        }
        if (!was && is_down(i, *digital, analog_values))
            let_go[i] = 0;
        else if (was && !is_down(i, *digital, analog_values))
            let_go[i] = 0;
    }
}

uint32_t fh_pad_get_state(uint32_t handle, uint32_t state)
{
    uint16_t digital = 0;
    uint8_t analog_values[8] = { 0 };
    int16_t thumbs[4] = { 0, 0, 0, 0 };
    int i;

    if (!is_pad(handle) || !state)
        return ERROR_NOT_CONNECTED;
    script_now(&digital, analog_values, thumbs);
#if defined(FH_HAVE_SDL3)
    if (fh_f2_game_gets_input()) {
        real_pad(&digital, analog_values, thumbs);
    } else {    /* [acct2-fh] the F2 window is open: a pad at rest (the controller still looked for) */
        uint16_t unused_digital = 0;
        uint8_t unused_analog[8] = { 0 };
        int16_t unused_thumbs[4] = { 0, 0, 0, 0 };

        real_pad(&unused_digital, unused_analog, unused_thumbs);
    }
#endif
    /* [acct2-fh] 11-22: the fixed step (see the top of the file) */
    if (!fh_frame_clock_updates()) {
        held_digital |= digital;
        held_any = 1;
        for (i = 0; i < 8; i++)
            if (analog_values[i] > held_analog[i])
                held_analog[i] = analog_values[i];
        digital = last_digital;
        memcpy(analog_values, last_analog_values, sizeof analog_values);
        memcpy(thumbs, last_thumbs, sizeof thumbs);
    } else if (held_any) {
        digital |= held_digital;
        for (i = 0; i < 8; i++)
            if (held_analog[i] > analog_values[i])
                analog_values[i] = held_analog[i];
        held_digital = 0;
        memset(held_analog, 0, sizeof held_analog);
        held_any = 0;
    }
    if (fh_frame_clock_updates()) {     /* [waw-decomp/fh14] 11-24: presses kept (see the top) */
#if defined(FH_HAVE_SDL3)
        keep_the_presses(&digital, analog_values, fh_f2_game_gets_input() || press_count > 0);
#else
        keep_the_presses(&digital, analog_values, 1);
#endif
        if (press_count > 0 && (MEM8(0x003B46D0u) & 0x10) != watched_pause) {
            watched_pause = MEM8(0x003B46D0u) & 0x10;
            fprintf(stderr, "[pad] %lld ms: the game is %s (presses kept so far %lu)\n",
                    now_ms() - start_ms, watched_pause ? "paused" : "running", presses_kept);
        }
    }
    if (digital != last_digital || memcmp(analog_values, last_analog_values, sizeof analog_values)
        || memcmp(thumbs, last_thumbs, sizeof thumbs)) {
        packet++;                           /* the packet number moves when anything changed, as XInput's does */
        last_digital = digital;
        memcpy(last_analog_values, analog_values, sizeof analog_values);
        memcpy(last_thumbs, thumbs, sizeof thumbs);
    }
    MEM32(state + STATE_PACKET) = packet;
    MEM16(state + STATE_BUTTONS) = digital;
    for (i = 0; i < 8; i++)
        MEM8(state + STATE_ANALOG + (uint32_t)i) = analog_values[i];
    for (i = 0; i < 4; i++)
        MEM16(state + STATE_THUMBS + (uint32_t)i * 2) = (uint16_t)thumbs[i];
    return ERROR_SUCCESS;
}

void fh_pad_last_state(uint16_t *digital, uint8_t analog[8], int16_t thumbs[4])
{
    *digital = last_digital;
    memcpy(analog, last_analog_values, sizeof last_analog_values);
    memcpy(thumbs, last_thumbs, sizeof last_thumbs);
}

uint32_t fh_pad_set_state(uint32_t handle, uint32_t feedback)
{
    if (!is_pad(handle))
        return ERROR_NOT_CONNECTED;
    if (feedback)
        MEM32(feedback) = ERROR_SUCCESS;    /* XINPUT_FEEDBACK_HEADER.dwStatus: done */
    return ERROR_SUCCESS;
}
