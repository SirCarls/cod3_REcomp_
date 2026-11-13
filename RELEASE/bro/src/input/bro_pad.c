/* [acct2-bro] Big Red One port: the gamepad, at XAPI's level.
 *
 * On the Xbox the game reaches its controllers through XAPI (statically linked: XGetDevices 0x007AC250,
 * XGetDeviceChanges 0x007AC272, XInputOpen 0x007ABF0D, XInputClose 0x007ABF63, XInputGetCapabilities 0x007ABF6F,
 * XInputGetState 0x007AC147, XInputSetState 0x007AC1BA), which drives the USB host controllers directly.
 * xboxrecomp's controller model can't trap those registers on an x86-64 Linux host ("no trapped-MMIO decoder"), so
 * these functions are stand-ins here that answer for one gamepad in the first port (port 0), and nothing else.
 *
 * The game asks with two device types: 0x007A9F78 from its input code (0x00055BC0, 0x00055D10), the gamepad, and
 * 0x007AA028 from its save code, the memory unit. Only the gamepad is reported.
 *
 * The pad's buttons come from a real controller (SDL3's gamepads, the first one plugged in: its A B X Y,
 * shoulders as white (left) and black (right), triggers, sticks, d-pad, start, back and stick clicks), the keyboard
 * while the game's window has the focus (W A S D the left stick, the arrow keys the right stick, space A, left control
 * B, R X, F Y, Q white, E black, the mouse buttons the triggers... see default_key_binds), and BRO_PAD_SCRIPT, all
 * at once. [acct2-bro-game] Which control drives which of the pad's inputs is the player's: bro.toml's [pad] and
 * [keys] sections (bro/config/bro.toml), with the defaults above. The sticks get input.dead_zone. The mouse's motion,
 * while the window holds it, turns the view by how far it moves, as on the PC (input.raw_mouse, on by default;
 * input.mouse_sensitivity, input.mouse_scale_x / _y, input.invert_y: see raw_mouse), or with raw_mouse off moves the
 * right stick as before (input.mouse_speed, BRO_MOUSE_SPEED, stick per pixel a poll, 1600 by default). The script:
 *
 *   BRO_PAD_SCRIPT=6000:start,9000:a,12000:down,13000:a:400
 *
 * Each entry is <ms>:<button>[+<button>...][:<hold ms>], counted from the game's first XInputGetState, held 150 ms
 * unless a hold is given. Buttons: up down left right start back lthumb rthumb, and the analog a b x y black white
 * lt rt (pressed fully). [acct2-bro-game] The sticks, pushed all the way, by the names bro.toml's [keys] gives their
 * directions: move_forward move_back move_left move_right (the left stick), look_up look_down look_left look_right
 * (the right stick), so a test can walk ("30000:move_forward:5000"). And "<ms>:wait=<text>" stops the script's clock
 * there until the game draws a string with that text in it (no commas or colons in it), so the presses after it
 * count from when that screen came up, however long loading took: "27000:start,28000:wait=Collector's Edition,
 * 30000:down" presses down 2 s after the main menu shows. BRO_PAD=0 unplugs the pad.
 *
 * [acct2-bro] 2026-11-08 In play the keyboard and the mouse also do CoD3's actions (bro_binds.c: fire, aim, use,
 * reload, melee, frag, sprint, lean, jump, crouch, prone, next and previous weapon on the wheel, ... with CoD3's keys and
 * names, bro.toml's [binds]); a key that has one doesn't press its controller button then. While aiming the mouse turns
 * slower by the zoom, as in CoD3 (input.zoom_sensitivity).
 *
 * Also here, for test runs: BRO_DEVMAP=<map> starts the game in a level (bro_launch_info, at the end). */
#include <math.h>
#include <stdatomic.h>              /* [f2-menus] 2026-11-11 bro_pad_reload */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "recomp_types.h"
#include "bro_binds.h"
#include "bro_pad.h"
#include "bro_settings.h"
#include "ngl_mesh.h"          /* ngl_mesh_world_fov */

#if defined(BRO_HAVE_SDL3)
#include <SDL3/SDL.h>
#endif

#define GAMEPAD_TYPE        0x007A9F78u     /* the XPP device type the game's input code passes */
#define PAD_HANDLE          0x50AD0001u     /* what XInputOpen hands back for the one pad */

#define ERROR_SUCCESS       0u
#define ERROR_NOT_CONNECTED 1167u           /* ERROR_DEVICE_NOT_CONNECTED */

/* XINPUT_STATE: dwPacketNumber, then XINPUT_GAMEPAD (wButtons, bAnalogButtons[8], four thumb shorts) */
#define STATE_PACKET        0x00u
#define STATE_BUTTONS       0x04u
#define STATE_ANALOG        0x06u
#define STATE_THUMBS        0x0Eu
#define STATE_BYTES         0x16u
/* XINPUT_CAPABILITIES: SubType, Reserved, In (an XINPUT_GAMEPAD), Out (rumble: two WORDs) */
#define CAPS_SUBTYPE        0x00u
#define CAPS_IN             0x02u
#define CAPS_OUT            0x18u
#define CAPS_BYTES          0x1Cu
#define GAMEPAD_BYTES       0x12u
#define SUBTYPE_GAMEPAD     0x01u

#define MAX_PRESSES         256
#define DEFAULT_HOLD_MS     150

static const struct { const char *name; uint16_t digital; int analog; } button_names[] = {
    { "up", 0x0001, -1 }, { "down", 0x0002, -1 }, { "left", 0x0004, -1 }, { "right", 0x0008, -1 },
    { "start", 0x0010, -1 }, { "back", 0x0020, -1 }, { "lthumb", 0x0040, -1 }, { "rthumb", 0x0080, -1 },
    { "a", 0, 0 }, { "b", 0, 1 }, { "x", 0, 2 }, { "y", 0, 3 }, { "black", 0, 4 }, { "white", 0, 5 },
    { "lt", 0, 6 }, { "rt", 0, 7 },
};

/* The sticks in the script: the thumb (0 left x, 1 left y, 2 right x, 3 right y) and the way it is pushed. */
static const struct { const char *name; int thumb, sign; } stick_names[] = {
    { "move_forward", 1, 1 }, { "move_back", 1, -1 }, { "move_left", 0, -1 }, { "move_right", 0, 1 },
    { "look_up", 3, 1 }, { "look_down", 3, -1 }, { "look_left", 2, -1 }, { "look_right", 2, 1 },
};

static struct press {
    unsigned long at_ms, hold_ms;
    uint16_t digital;
    uint8_t analog;         /* bit n: analog button n */
    int8_t thumbs[4];       /* -1, 0 or 1: each stick axis pushed all the way, or not */
    char *wait;             /* "wait=<text>": the text this entry waits for, or NULL */
    int announced;
} presses[MAX_PRESSES];
static int waiting = -1;            /* the wait entry the script is stopped at, or -1 */
static volatile int wait_seen;      /* its text has been drawn */
static long long waited_ms;         /* time spent waiting, taken off the script's clock */
static int press_count = -1;    /* -1: script not read yet */
static int reported_insert;
static long long start_ms = -1;
static uint32_t packet;
static uint16_t last_digital;
static uint8_t last_analog;

static long long now_ms(void)
{
    struct timespec t;

    clock_gettime(CLOCK_MONOTONIC, &t);
    return (long long)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

static int pad_plugged(void)
{
    const char *setting = getenv("BRO_PAD");

    return !(setting && setting[0] == '0');
}

static void read_script(void)
{
    const char *spec = getenv("BRO_PAD_SCRIPT");
    char *copy, *entry, *save = NULL;

    press_count = 0;
    if (!spec || !*spec)
        return;
    copy = strdup(spec);
    for (entry = strtok_r(copy, ",", &save); entry && press_count < MAX_PRESSES; entry = strtok_r(NULL, ",", &save)) {
        struct press p = { 0, DEFAULT_HOLD_MS, 0, 0, { 0, 0, 0, 0 }, NULL, 0 };
        char *names, *hold, *name, *save_name = NULL;

        p.at_ms = strtoul(entry, &names, 10);
        if (*names != ':')
            continue;
        names++;
        if (!strncmp(names, "wait=", 5)) {
            p.wait = strdup(names + 5);
            presses[press_count++] = p;
            continue;
        }
        hold = strchr(names, ':');
        if (hold) {
            *hold++ = '\0';
            p.hold_ms = strtoul(hold, NULL, 10);
        }
        for (name = strtok_r(names, "+", &save_name); name; name = strtok_r(NULL, "+", &save_name)) {
            size_t i;

            for (i = 0; i < sizeof button_names / sizeof button_names[0]; i++) {
                if (!strcmp(name, button_names[i].name)) {
                    if (button_names[i].analog < 0)
                        p.digital |= button_names[i].digital;
                    else
                        p.analog |= (uint8_t)(1u << button_names[i].analog);
                    break;
                }
            }
            if (i == sizeof button_names / sizeof button_names[0]) {
                size_t j;

                for (j = 0; j < sizeof stick_names / sizeof stick_names[0]; j++) {
                    if (!strcmp(name, stick_names[j].name)) {
                        p.thumbs[stick_names[j].thumb] = (int8_t)stick_names[j].sign;
                        break;
                    }
                }
                if (j == sizeof stick_names / sizeof stick_names[0])
                    fprintf(stderr, "[pad] BRO_PAD_SCRIPT: no button called \"%s\"\n", name);
            }
        }
        presses[press_count++] = p;
    }
    free(copy);
    fprintf(stderr, "[pad] BRO_PAD_SCRIPT: %d presses\n", press_count);
}

/* The buttons held and the sticks pushed now, from the script. */
static void held_now(uint16_t *digital, uint8_t *analog, int16_t thumbs[4])
{
    long long t;
    int i;

    if (press_count < 0)
        read_script();
    if (start_ms < 0)
        start_ms = now_ms();
    *digital = 0;
    *analog = 0;
    if (waiting >= 0) {
        if (!wait_seen) {               /* the clock stays where the wait entry is */
            waited_ms = now_ms() - start_ms - (long long)presses[waiting].at_ms;
            return;
        }
        fprintf(stderr, "[pad] %lu ms: \"%s\" is on the screen (after %lld ms), going on\n", presses[waiting].at_ms,
                presses[waiting].wait, waited_ms);
        waiting = -1;
    }
    t = now_ms() - start_ms - waited_ms;
    for (i = 0; i < press_count; i++) {
        struct press *p = &presses[i];

        if (p->wait) {
            if (!p->announced && t >= (long long)p->at_ms) {
                p->announced = 1;
                fprintf(stderr, "[pad] %lu ms: waiting for \"%s\"\n", p->at_ms, p->wait);
                wait_seen = 0;
                waiting = i;
                return;
            }
            continue;
        }
        if (t >= (long long)p->at_ms && t < (long long)(p->at_ms + p->hold_ms)) {
            int k;

            *digital |= p->digital;
            *analog |= p->analog;
            for (k = 0; k < 4; k++)
                if (p->thumbs[k])
                    thumbs[k] = p->thumbs[k] > 0 ? 32767 : -32767;
            if (!p->announced) {
                p->announced = 1;
                fprintf(stderr, "[pad] %lld ms: pressing digital 0x%04X analog 0x%02X sticks %d %d %d %d for %lu ms\n",
                        t, p->digital, p->analog, p->thumbs[0], p->thumbs[1], p->thumbs[2], p->thumbs[3], p->hold_ms);
            }
        }
    }
}

#if defined(BRO_HAVE_SDL3)
/* [acct2-bro-game] What drives each of the pad's inputs: the controller's buttons ([pad] in bro.toml) and the
 * keyboard's keys and the mouse's buttons ([keys]). Each input is named as in BRO_PAD_SCRIPT (button_names above);
 * the sticks' directions from keys are move_forward, move_back, move_left, move_right (the left stick) and look_up,
 * look_down, look_left, look_right (the right stick). A [pad] value is an SDL gamepad button or trigger name ("a",
 * "b", "x", "y", "leftshoulder", "rightshoulder", "lefttrigger", "righttrigger", "start", "back", "dpup", "dpdown",
 * "dpleft", "dpright", "leftstick", "rightstick", or "none"); a [keys] value is a list of key names as SDL writes
 * them ("W", "Space", "Left Ctrl", "Return", "Escape", "Up") and mouse1 to mouse5, comma-separated ("" for none).
 * Anything not in the file keeps the default below, so every older bro.toml goes on working. */
#define MAX_KEYS_A_BIND 4
#define STICK_DIRECTIONS 8
#define MOUSE_KEY_BASE   1000   /* key codes from here are mouse buttons (1000 + SDL's button number) */

static const char *const stick_direction_names[STICK_DIRECTIONS] = {
    "move_forward", "move_back", "move_left", "move_right", "look_up", "look_down", "look_left", "look_right",
};

/* The defaults: the controller as an Xbox pad (the same order as button_names). */
static const char *const default_pad_binds[] = {
    "dpup", "dpdown", "dpleft", "dpright", "start", "back", "leftstick", "rightstick",
    "a", "b", "x", "y", "rightshoulder", "leftshoulder", "lefttrigger", "righttrigger",
};
/* The keyboard and the mouse (the same order as button_names, then stick_direction_names). */
static const char *const default_key_binds[] = {
    "1", "2", "3", "4", "Return", "Escape", "Left Shift", "V",
    "Space", "Left Ctrl", "R", "F", "E", "Q", "Z,mouse3", "X,mouse1",
    "W", "S", "A", "D", "Up", "Down", "Left", "Right",
};
#define BUTTON_COUNT ((int)(sizeof button_names / sizeof button_names[0]))

static struct {
    int ready;
    int pad_button[16];             /* SDL_GamepadButton, or -1 */
    int pad_axis[16];               /* SDL_GamepadAxis (the triggers), or -1 */
    int keys[16 + STICK_DIRECTIONS][MAX_KEYS_A_BIND];   /* SDL_Scancode or MOUSE_KEY_BASE + button; 0: none */
    float dead_zone;                /* input.dead_zone: the sticks' own, 0 = the game's alone */
    int raw_mouse;                  /* input.raw_mouse: the mouse turns the view itself */
    float sensitivity;              /* input.mouse_sensitivity: as the PC games' "sensitivity" */
    float scale_x, scale_y;         /* input.mouse_scale_x / _y, the y one negative with input.invert_y */
    int stick_speed;                /* input.mouse_speed: stick per pixel when raw_mouse is off */
    float zoom_sensitivity;         /* input.zoom_sensitivity: on top of the zoom's own slowdown (CoD3's) */
} binds;

static void read_key_list(const char *list, int out[MAX_KEYS_A_BIND], const char *what)
{
    char copy[128], *name, *save = NULL;
    int n = 0;

    memset(out, 0, sizeof(int) * MAX_KEYS_A_BIND);
    snprintf(copy, sizeof copy, "%s", list);
    for (name = strtok_r(copy, ",", &save); name && n < MAX_KEYS_A_BIND; name = strtok_r(NULL, ",", &save)) {
        SDL_Scancode code;

        while (*name == ' ')
            name++;
        if (!*name)
            continue;
        if (!strncmp(name, "mouse", 5) && name[5] >= '1' && name[5] <= '5' && !name[6]) {
            out[n++] = MOUSE_KEY_BASE + (name[5] - '0');
            continue;
        }
        code = SDL_GetScancodeFromName(name);
        if (code == SDL_SCANCODE_UNKNOWN)
            fprintf(stderr, "[pad] keys.%s: no key called \"%s\"\n", what, name);
        else
            out[n++] = (int)code;
    }
}

static void read_binds(void)
{
    char key[48];
    int i;

    binds.ready = 1;
    for (i = 0; i < BUTTON_COUNT; i++) {
        const char *name;

        snprintf(key, sizeof key, "pad.%s", button_names[i].name);
        name = bro_setting_text(key, NULL);
        if (!name)
            name = default_pad_binds[i];
        binds.pad_button[i] = binds.pad_axis[i] = -1;
        if (!strcmp(name, "lefttrigger") || !strcmp(name, "righttrigger"))
            binds.pad_axis[i] = (int)SDL_GetGamepadAxisFromString(name);
        else if (strcmp(name, "none") && (binds.pad_button[i] = (int)SDL_GetGamepadButtonFromString(name)) < 0)
            fprintf(stderr, "[pad] %s: no controller button called \"%s\"\n", key, name);
    }
    for (i = 0; i < BUTTON_COUNT + STICK_DIRECTIONS; i++) {
        const char *what = i < BUTTON_COUNT ? button_names[i].name : stick_direction_names[i - BUTTON_COUNT];
        const char *list;

        snprintf(key, sizeof key, "keys.%s", what);
        list = bro_setting_text(key, NULL);
        read_key_list(list ? list : default_key_binds[i], binds.keys[i], what);
    }
    binds.dead_zone = (float)bro_setting_number("input.dead_zone", "BRO_DEAD_ZONE", 0.0);
    if (binds.dead_zone < 0.0f || binds.dead_zone > 0.9f)
        binds.dead_zone = 0.0f;
    binds.raw_mouse = bro_setting_bool("input.raw_mouse", "BRO_RAW_MOUSE", 1);
    binds.sensitivity = (float)bro_setting_number("input.mouse_sensitivity", "BRO_MOUSE_SENSITIVITY", 5.0);
    if (binds.sensitivity <= 0.0f || binds.sensitivity > 100.0f)
        binds.sensitivity = 5.0f;
    binds.scale_x = (float)bro_setting_number("input.mouse_scale_x", "BRO_MOUSE_SCALE_X", 1.0);
    binds.scale_y = (float)bro_setting_number("input.mouse_scale_y", "BRO_MOUSE_SCALE_Y", 1.0);
    if (bro_setting_bool("input.invert_y", "BRO_INVERT_Y", 0))
        binds.scale_y = -binds.scale_y;
    binds.stick_speed = bro_setting_int("input.mouse_speed", "BRO_MOUSE_SPEED", 1600);
    if (binds.stick_speed <= 0 || binds.stick_speed > 32767)
        binds.stick_speed = 1600;
    binds.zoom_sensitivity = (float)bro_setting_number("input.zoom_sensitivity", "BRO_ZOOM_SENSITIVITY", 1.0);
    if (!(binds.zoom_sensitivity >= 0.05f && binds.zoom_sensitivity <= 4.0f))
        binds.zoom_sensitivity = 1.0f;
}

/* Whether any of a bind's keys is held. [acct2-bro] In play a key that has one of CoD3's actions (bro_binds.c) does
 * only that, so it doesn't count here then. */
static int bind_held(const int keys_of_bind[MAX_KEYS_A_BIND], const bool *keys, SDL_MouseButtonFlags mouse,
                     int focused, int in_play)
{
    int k;

    for (k = 0; k < MAX_KEYS_A_BIND && keys_of_bind[k]; k++) {
        int code = keys_of_bind[k];

        if (in_play && bro_binds_claims(code))
            continue;
        if (code >= MOUSE_KEY_BASE) {
            if (focused && (mouse & SDL_BUTTON_MASK(code - MOUSE_KEY_BASE)))
                return 1;
        } else if (keys[code]) {
            return 1;
        }
    }
    return 0;
}

static SDL_Gamepad *controller;
static int controllers_started;     /* 0: not tried, 1: SDL's gamepads are up, -1: they aren't */
static long long last_scan_ms;

static int16_t clamp_thumb(int value)
{
    return (int16_t)(value > 32767 ? 32767 : value < -32767 ? -32767 : value);
}

/* input.dead_zone on one stick: inside it the stick is at rest, outside it the rest of its travel is stretched back
 * over the whole range (radial, so diagonals keep their direction). */
static void apply_dead_zone(int *x, int *y)
{
    float fx = (float)*x / 32767.0f, fy = (float)*y / 32767.0f;
    float length = sqrtf(fx * fx + fy * fy), travel, scale;

    if (binds.dead_zone <= 0.0f)
        return;
    if (length <= binds.dead_zone) {
        *x = *y = 0;
        return;
    }
    travel = length > 1.0f ? 1.0f : length;
    scale = (travel - binds.dead_zone) / (1.0f - binds.dead_zone) / length;
    *x = (int)(fx * scale * 32767.0f);
    *y = (int)(fy * scale * 32767.0f);
}

/* [acct2-bro-game] Raw mouse: the mouse's motion turns the view itself, by how far the mouse moves and never by the
 * frame rate, as on the PC: each count is 0.022 degrees times input.mouse_sensitivity (5 by default, the PC games'
 * own default: 0.11 degrees a count), times input.mouse_scale_x / _y, up and down flipped with input.invert_y.
 *
 * The game's own mouse look (Quake III's cl.mouseDx / cl.mouseDy, read by CL_MouseMove 0x002B39E0) can't do this on
 * the console build: it multiplies the motion by the frame's milliseconds, as it does a stick's (measured headless in
 * Gela: the same 5 counts a frame turned the view as far a second at 60 fps as uncapped). So the motion goes into the
 * view angles that CL_CreateCmd sends, cl.viewangles (pitch 0x00961798, yaw 0x0096179C), measured headless: 0.110
 * degrees a count at 60 fps and uncapped alike. Also times the float CL_MouseMove scales by at 0x00961774 (Quake III's
 * cl.cgameSensitivity, the game's own scale for the sights, it seems; 1 in every run so far, also with LT held).
 *
 * CL_MouseMove flips cl.mouseIndex (0x0096174C) every time the game makes a command from the controls; when it hasn't
 * for a while (no level, a loading screen, a movie) the motion is dropped, so nothing piles up to turn the view later.
 * BRO_MOUSE_TEST=<dx>,<dy> adds that motion at every poll (test runs without a mouse) and logs the view angles. */
#define MOUSE_LOOK_IDLE_MS      250
#define MOUSE_DEGREES_A_COUNT   0.022f          /* m_yaw / m_pitch, as the PC games have them */
#define CL_MOUSE_INDEX          0x0096174Cu
#define CL_GAME_SENSITIVITY     0x00961774u     /* float */
#define CL_VIEW_PITCH           0x00961798u     /* cl.viewangles, floats: pitch, yaw */
#define CL_VIEW_YAW             0x0096179Cu

static float mem_float(uint32_t address)
{
    union { uint32_t bits; float value; } v;

    v.bits = MEM32(address);
    return v.value;
}

static void set_mem_float(uint32_t address, float value)
{
    union { uint32_t bits; float value; } v;

    v.value = value;
    MEM32(address) = v.bits;
}

/* [acct2-bro] 2026-11-08 CoD3's zoom sensitivity (game/mouse_look.cpp): while aiming, the mouse turns slower by the
 * world's field of view against the one before aiming (ngl_mesh_world_fov), times input.zoom_sensitivity (1 by
 * default, as CoD3's cod3_zoom_sensitivity). The game's own scale for the sights (cl.cgameSensitivity) stays 1 on this
 * build. */
static float zoom_ratio(int aiming)
{
    static float base;
    float fov = ngl_mesh_world_fov(), ratio;

    if (!(fov > 0.0f && isfinite(fov)))
        return 1.0f;
    if (!aiming) {
        base = fov;
        return 1.0f;
    }
    if (base <= 0.0f)
        return 1.0f;
    ratio = fov / base;
    if (ratio >= 0.999f)
        return 1.0f;
    return (ratio < 0.05f ? 0.05f : ratio) * binds.zoom_sensitivity;
}

#if defined(BRO_HAVE_F2)
void bro_f2_mouse(float *dx, float *dy);    /* [f2-menus] bro_f2.h */
#endif

static void raw_mouse(float dx, float dy, int aiming)
{
    static int test = -1;
    static float test_dx, test_dy;
    static unsigned polls;
    static uint32_t last_slot = 2;
    static long long last_flip_ms;
    uint32_t slot = MEM32(CL_MOUSE_INDEX) & 1u;
    float zoom = mem_float(CL_GAME_SENSITIVITY), degrees;

    if (slot != last_slot) {
        last_slot = slot;
        last_flip_ms = now_ms();
    }
    if (test < 0) {
        const char *setting = getenv("BRO_MOUSE_TEST");

        test = setting && sscanf(setting, "%f,%f", &test_dx, &test_dy) == 2;
    }
    if (test) {
        dx += test_dx;
        dy += test_dy;
        if (polls++ % 120 == 0)
            fprintf(stderr, "[mouse] poll %u: view pitch %.2f yaw %.2f, game sensitivity %.3f\n", polls - 1,
                    mem_float(CL_VIEW_PITCH), mem_float(CL_VIEW_YAW), zoom);
    }
    if (now_ms() - last_flip_ms > MOUSE_LOOK_IDLE_MS || (dx == 0.0f && dy == 0.0f))
        return;
    if (!(zoom > 0.0f && zoom <= 4.0f))     /* not set (or not a number): as it is outside the sights */
        zoom = 1.0f;
    zoom *= zoom_ratio(aiming);
    degrees = MOUSE_DEGREES_A_COUNT * binds.sensitivity * zoom;
    set_mem_float(CL_VIEW_YAW, mem_float(CL_VIEW_YAW) - dx * binds.scale_x * degrees);
    set_mem_float(CL_VIEW_PITCH, mem_float(CL_VIEW_PITCH) + dy * binds.scale_y * degrees);
}

/* [f2-menus] 2026-11-11 A control setting changed in the F2 window (bro/src/f2/bro_f2_store.cpp calls this; weak
 * there, so without it those rows say "takes effect at the next start"): any thread may ask; the game's next poll
 * (real_pad, on its own thread) reads input.*, [pad], [keys] and the binds (bro_binds.c) again. */
static atomic_int reload_wanted;

void bro_pad_reload(void)
{
    atomic_store(&reload_wanted, 1);
}

/* What the real controller and the keyboard hold now, added to the script's buttons. Stick y is up-positive, as XInput
 * has it (SDL's is down-positive). */
static void real_pad(uint16_t *digital, uint8_t *analog, uint8_t analog_values[8], int16_t thumbs[4])
{
    const bool *keys;
    SDL_Window *focus;
    SDL_MouseButtonFlags mouse = 0;
    int i, in_play;

    if (atomic_exchange(&reload_wanted, 0)) {  /* [f2-menus] 2026-11-11 a change in F2 (bro_pad_reload) */
        binds.ready = 0;
        bro_binds_reload();
    }
    if (!binds.ready)
        read_binds();
    in_play = bro_binds_in_play();
    if (!controllers_started) {
        controllers_started = SDL_InitSubSystem(SDL_INIT_GAMEPAD) ? 1 : -1;
        if (controllers_started < 0)
            fprintf(stderr, "[pad] no game controllers (%s): the keyboard and BRO_PAD_SCRIPT only\n", SDL_GetError());
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
            for (int n = 0; pads && n < count && !controller; n++) {
                if ((controller = SDL_OpenGamepad(pads[n])) != NULL)
                    fprintf(stderr, "[pad] controller: %s\n", SDL_GetGamepadName(controller));
            }
            SDL_free(pads);
        }
    }
    if (controller) {
        int lx = SDL_GetGamepadAxis(controller, SDL_GAMEPAD_AXIS_LEFTX);
        int ly = -SDL_GetGamepadAxis(controller, SDL_GAMEPAD_AXIS_LEFTY);
        int rx = SDL_GetGamepadAxis(controller, SDL_GAMEPAD_AXIS_RIGHTX);
        int ry = -SDL_GetGamepadAxis(controller, SDL_GAMEPAD_AXIS_RIGHTY);

        for (i = 0; i < BUTTON_COUNT; i++) {
            int value = 0;

            if (binds.pad_button[i] >= 0)
                value = SDL_GetGamepadButton(controller, (SDL_GamepadButton)binds.pad_button[i]) ? 0xFF : 0;
            else if (binds.pad_axis[i] >= 0)
                value = SDL_GetGamepadAxis(controller, (SDL_GamepadAxis)binds.pad_axis[i]) / 128;
            if (!value)
                continue;
            if (button_names[i].analog < 0) {
                if (value >= 0x20)
                    *digital |= button_names[i].digital;
            } else if (value > analog_values[button_names[i].analog]) {
                analog_values[button_names[i].analog] = (uint8_t)(value > 0xFF ? 0xFF : value);
            }
        }
        apply_dead_zone(&lx, &ly);
        apply_dead_zone(&rx, &ry);
        thumbs[0] = clamp_thumb(thumbs[0] + lx);
        thumbs[1] = clamp_thumb(thumbs[1] + ly);
        thumbs[2] = clamp_thumb(thumbs[2] + rx);
        thumbs[3] = clamp_thumb(thumbs[3] + ry);
    }
    keys = SDL_GetKeyboardState(NULL);     /* all up without a focused window */
    focus = SDL_GetKeyboardFocus();
    if (keys) {
        int dir[STICK_DIRECTIONS];

        mouse = SDL_GetMouseState(NULL, NULL);
        for (i = 0; i < BUTTON_COUNT; i++) {
            if (!bind_held(binds.keys[i], keys, mouse, focus != NULL, in_play))
                continue;
            if (button_names[i].analog < 0)
                *digital |= button_names[i].digital;
            else
                analog_values[button_names[i].analog] = 0xFF;
        }
        for (i = 0; i < STICK_DIRECTIONS; i++)
            dir[i] = bind_held(binds.keys[BUTTON_COUNT + i], keys, mouse, focus != NULL, in_play) ? 32767 : 0;
        thumbs[1] = clamp_thumb(thumbs[1] + dir[0] - dir[1]);   /* forward, back */
        thumbs[0] = clamp_thumb(thumbs[0] + dir[3] - dir[2]);   /* right, left */
        thumbs[3] = clamp_thumb(thumbs[3] + dir[4] - dir[5]);   /* up, down */
        thumbs[2] = clamp_thumb(thumbs[2] + dir[7] - dir[6]);   /* right, left */
    }
    bro_binds_poll(keys, (uint32_t)mouse, focus != NULL);   /* [acct2-bro] CoD3's actions, in play */
    {
        float dx = 0.0f, dy = 0.0f;

        if (focus && SDL_GetWindowRelativeMouseMode(focus))  /* captured (ngl_present.c): its motion since the last poll */
            SDL_GetRelativeMouseState(&dx, &dy);
        if (binds.raw_mouse) {
#if defined(BRO_HAVE_F2)
            bro_f2_mouse(&dx, &dy);     /* [f2-menus] 2026-11-11 the in-game acceleration and sensitivity randomizer */
#endif
            raw_mouse(dx, dy, analog_values[6] >= 0x20 || bro_binds_aim_held());  /* aiming: LT or the aim keys */
        } else {
            thumbs[2] = clamp_thumb(thumbs[2] + (int)(dx * binds.scale_x * (float)binds.stick_speed));
            thumbs[3] = clamp_thumb(thumbs[3] - (int)(dy * binds.scale_y * (float)binds.stick_speed));
        }
    }
    for (i = 0; i < 8; i++)
        if (analog_values[i] >= 0x20)          /* the bits the packet number follows */
            *analog |= (uint8_t)(1u << i);
}
#endif

static int is_pad(uint32_t handle)
{
    return handle == PAD_HANDLE && pad_plugged();
}

uint32_t bro_pad_get_devices(uint32_t device_type)
{
    return device_type == GAMEPAD_TYPE && pad_plugged() ? 1u : 0u;      /* bit 0: the first port */
}

uint32_t bro_pad_get_device_changes(uint32_t device_type, uint32_t insertions, uint32_t removals)
{
    uint32_t inserted = 0;

    if (device_type == GAMEPAD_TYPE && pad_plugged() && !reported_insert) {
        reported_insert = 1;
        inserted = 1;
    }
    if (insertions)
        MEM32(insertions) = inserted;
    if (removals)
        MEM32(removals) = 0;
    return inserted ? 1u : 0u;
}

uint32_t bro_pad_open(uint32_t device_type, uint32_t port, uint32_t slot, uint32_t polling)
{
    (void)slot;
    (void)polling;
    return device_type == GAMEPAD_TYPE && port == 0 && pad_plugged() ? PAD_HANDLE : 0u;
}

uint32_t bro_pad_close(uint32_t handle)
{
    (void)handle;
    return 0;
}

uint32_t bro_pad_get_capabilities(uint32_t handle, uint32_t capabilities)
{
    uint32_t i;

    if (!is_pad(handle) || !capabilities)
        return ERROR_NOT_CONNECTED;
    for (i = 0; i < CAPS_BYTES; i++)
        MEM8(capabilities + i) = 0;
    MEM8(capabilities + CAPS_SUBTYPE) = SUBTYPE_GAMEPAD;
    /* every button and axis there, as a full Xbox controller reports them */
    for (i = 0; i < GAMEPAD_BYTES; i++)
        MEM8(capabilities + CAPS_IN + i) = 0xFF;
    MEM16(capabilities + CAPS_OUT) = 0xFFFF;
    MEM16(capabilities + CAPS_OUT + 2) = 0xFFFF;
    return ERROR_SUCCESS;
}

#if defined(BRO_HAVE_F2)
int bro_f2_game_gets_input(void);    /* [f2-menus] bro_f2.h */
#endif

uint32_t bro_pad_get_state(uint32_t handle, uint32_t state)
{
    static int16_t last_thumbs[4];
    uint16_t digital;
    uint8_t analog, analog_values[8];
    int16_t thumbs[4] = { 0, 0, 0, 0 };
    int i;

    if (!is_pad(handle) || !state)
        return ERROR_NOT_CONNECTED;
    held_now(&digital, &analog, thumbs);
    for (i = 0; i < 8; i++)
        analog_values[i] = (analog >> i) & 1 ? 0xFF : 0x00;
#if defined(BRO_HAVE_SDL3)
#if defined(BRO_HAVE_F2)
    if (!bro_f2_game_gets_input()) {    /* [f2-menus] the F2 window has the keys, mouse and pads: a pad at rest */
        static const bool none[SDL_SCANCODE_COUNT];

        bro_binds_poll(none, 0, 0);     /* every held action let go */
        digital = 0;
        analog = 0;
        memset(analog_values, 0, sizeof analog_values);
        memset(thumbs, 0, sizeof thumbs);
    } else
#endif
    real_pad(&digital, &analog, analog_values, thumbs);
#endif
    if (digital != last_digital || analog != last_analog || memcmp(thumbs, last_thumbs, sizeof thumbs)) {
        packet++;
        last_digital = digital;
        last_analog = analog;
        memcpy(last_thumbs, thumbs, sizeof thumbs);
    }
    MEM32(state + STATE_PACKET) = packet;
    MEM16(state + STATE_BUTTONS) = digital;
    for (i = 0; i < 8; i++)
        MEM8(state + STATE_ANALOG + (uint32_t)i) = analog_values[i];
    for (i = 0; i < 4; i++)
        MEM16(state + STATE_THUMBS + (uint32_t)i * 2) = (uint16_t)thumbs[i];
    (void)STATE_BYTES;
    return ERROR_SUCCESS;
}

int bro_pad_waiting_for_text(void)
{
    return waiting >= 0;
}

void bro_pad_text_drawn(const char *text)
{
    if (waiting >= 0 && text && strstr(text, presses[waiting].wait))
        wait_seen = 1;
}

uint32_t bro_pad_set_state(uint32_t handle, uint32_t feedback)
{
    if (!is_pad(handle))
        return ERROR_NOT_CONNECTED;
    if (feedback)
        MEM32(feedback) = ERROR_SUCCESS;    /* XINPUT_FEEDBACK_HEADER.dwStatus: done */
    return ERROR_SUCCESS;
}

/* Sys_ReadLaunchInfo (0x002221B0, cdecl; its one argument the buffer for the command line), done natively so a test
 * run can start in a level. The game asks XGetLaunchInfo for its launch data, and when a debugger launched it (type
 * 3) that data is the command line, copied as it is. Here BRO_DEVMAP=<map> makes it "+devmap <map>", with the
 * names Sys_ReadLaunchInfo itself uses for the chapters: prologue, oran, kasserine, liberator, gela, piano_lupo,
 * troina, omaha_beach, mons1, eilendorf, buchholz, dragons_teeth. Without it the game wasn't launched by anything,
 * which is what XGetLaunchInfo answers here (no launch data page), and the buffer is left as it is. Either way the
 * three flags the original clears first are cleared. */
void bro_launch_info(void)
{
    uint32_t line = MEM32(g_esp + 4);
    const char *map = getenv("BRO_DEVMAP");

    MEM8(0x0095EE01u) = 0;
    MEM8(0x0095EE09u) = 0;
    MEM8(0x008DD1CFu) = 0;
    if (map && *map && line) {
        char text[64];
        size_t i;

        snprintf(text, sizeof text, "+devmap %s", map);
        for (i = 0; i < sizeof text; i++) {
            MEM8(line + (uint32_t)i) = (uint8_t)text[i];
            if (!text[i])
                break;
        }
        fprintf(stderr, "[launch] command line \"%s\"\n", text);
    }
    g_eax = 0;
    g_esp += 4u;    /* ret */
}
