/* [waw-decomp] Finest Hour port, phase 2: the picture each frame ends with, through ngl_vk (../../ngl_vk).
 *
 * The game ends each frame with D3DDevice_Swap (0x0022D440; the front end calls it once a frame, the movie player
 * once a movie frame). On the Xbox that showed the back buffer. Here it shows the render target fh_d3d.c made for
 * the device (640 x 480, the size CreateDevice asked for): ngl_vk_draw_picture() draws it, scaled to fit and centered,
 * into ngl_vk's own picture (the target), and shows that in a window. What the game draws into the render target by
 * the CPU shows at once: its Bink movies (decoded by the game's own code straight into the back buffer) and its XFONT
 * text. [waw-decomp] 15:30: what it draws through Direct3D's draw calls goes to Vulkan too (fh_draw.c): a frame with
 * draws is drawn there and closed here, with the CPU picture on top when the game wrote one that frame.
 *
 * The window is an SDL3 window made for Vulkan, as Big Red One's (bro/src/ngl/ngl_present.c, the model for this
 * file): its surface made by SDL_Vulkan_CreateSurface, on Windows from the window's handle; the swapchain is ngl_vk's.
 * Without a display (the test sandbox), or without SDL3, frames are drawn offscreen. Closing the window ends the
 * game, as SIGINT and SIGTERM do.
 *
 * [acct2-fh] 2026-11-13 The display and resolution settings are in fh.toml's [display] (fh_settings.c,
 * fh/config/fh.toml), each with its variable, which wins (owner 20:38: "start working on the resolution options.
 * Copy everything cod3 has. Please don't limit options based on cod3 tho."; dev/agents/acct2-fh/RESOLUTION.md):
 *   display.mode (FH_DISPLAY_MODE)        window, borderless (fills the screen at the desktop's own mode) or exclusive
 *                                         (the monitor switched to fullscreen_size at refresh_rate); FH_FULLSCREEN=1,
 *                                         the setting before these, is borderless
 *   display.monitor (FH_MONITOR)          a monitor's name or its number from 1; "" the system's choice
 *   display.refresh_rate (FH_REFRESH_RATE)        exclusive: the monitor's refresh rate in Hz; 0 its own
 *   display.fullscreen_size (FH_FULLSCREEN_SIZE)  exclusive: "WxH", the mode nearest it; "" the desktop's size
 *   display.window_size (FH_WINDOW_SIZE)  the window's size, "WxH" (1280x960, twice the game's screen)
 *   display.vsync (FH_VSYNC)              frames in step with the display; false: the newest frame at each refresh
 *                                         (mailbox, no tearing); "immediate": each frame at once (it may tear)
 *   display.scaling (FH_SCALING)          [acct2-fh] 11-14: how the picture is scaled into the window: "smooth"
 *                                         (filtered), "sharp" (each pixel kept: nearest), "integer" (whole multiples),
 *                                         "cas" (AMD FidelityFX CAS: filtered, then sharpened), "fsr" (AMD FidelityFX
 *                                         FSR 1: scaled up by EASU, then sharpened by RCAS; CAS when not scaled up)
 *   display.cas_sharpness (FH_CAS_SHARPNESS)  "cas": 0..1, 0 the least (CAS still sharpens a little)
 *   display.fsr_sharpness_reduction (FH_FSR_SHARPNESS_REDUCTION)  "fsr": 0..2, the stops RCAS is less sharp by
 *   display.fsr_passes (FH_FSR_PASSES)    "fsr": 1..8, EASU passes at most (each doubles the size, the last one
 *                                         goes the rest of the way)
 *   display.fit, render_resolution, render_percent, live_resolution: what the game is drawn at (fh_resolution.c);
 *                                         FH_SIZE=<w>x<h> still makes ngl_vk's picture exactly that size
 * [acct2-fh] 2026-11-14 F2 opens the settings window (f2/fh_f2.cpp) over the picture; what it changes is applied
 * while the game runs (follow_window_settings, follow_plan).
 * Other settings (environment):
 *   FH_VK=0                     nothing is drawn (the phase 1 behavior)
 *   FH_WINDOW=0                 no window even when there is a display
 *   FH_START_IN_BACKGROUND=1    test runs on a PC someone is using: the window opens behind the window in use and
 *                               takes no focus until it is clicked (open_window); full screen waits for that click
 *   FH_TEST_WINDOW_SIZE=<frame>:<w>x<h>[,...]  test runs: the window is given that size at that frame (Swaps), to
 *                               check a live change of the render resolution
 *   FH_SHOW_FPS=1               the frame rate in the window's title, and every 5 seconds in the log
 *   FH_VK_DUMP=<frame>[,...]    save what Vulkan drew at those frames (Swaps, counting from 1) ...
 *   FH_VK_DUMP_PATH=<file.ppm>  ... to this file; a %lu in the name becomes the frame's number
 *   FH_FRAME_PACING=0           [acct2-fh] 11-13: frames not held to the game's presentation (fh_frame_pace.c; by
 *                               default 60 a second, as the game asked the Xbox for)
 *   FH_PROFILE=<file>           [acct2-fh] 11-13: the main thread sampled 1000 times a second into this file, to see
 *                               where its frames go (fh_profile.c; FH_PROFILE_HZ another rate)
 *   FH_GAMMA=0                  [acct2-fh] 11-13: the window doesn't show the game's gamma ramp (fh_present_gamma_ramp) */
#include "fh_present.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "fh_aspect.h"      /* [acct2-fh] the 3D view's shape, Hor+ */
#include "fh_console.h"     /* [acct2-fh] 11-14: the console's commands at a frame (FH_CONSOLE_EXEC) */
#include "fh_d3d.h"         /* fh_d3d_render_target_picture */
#include "fh_d3d_state.h"   /* fh_d3d_state_frame_end */
#include "fh_draw.h"        /* the game's draws on Vulkan */
#include "fh_text.h"                /* [waw-decomp-fh2 r19] the game's text layer */
#include "fh_frame_pace.h"  /* [acct2-fh] each frame held to the game's presentation */
#include "fh_host.h"        /* [waw-decomp/fh2] 11-23: UTF-8 paths */
#include "fh_profile.h"     /* [acct2-fh] FH_PROFILE: where the main thread's frames go */
#include "fh_resolution.h"  /* [acct2-fh] what size the game is drawn at */
#include "fh_settings.h"    /* [acct2-fh] fh.toml */
#include "fh_watch.h"       /* [acct2-fh] FH_WATCH: guest words printed as they change */
#include "f2/fh_f2.h"       /* [acct2-fh] 11-14: the F2 settings window (does nothing when not built) */
#include "recomp_types.h"   /* [acct2-fh] MEM8: the gamma ramp in guest memory */

#if defined(FH_HAVE_SDL3)
#include <SDL3/SDL.h>
#endif
#if defined(FH_HAVE_VULKAN)
#include "ngl_vk.h"
#if defined(FH_HAVE_SDL3)
#if defined(_WIN32)
#define VK_USE_PLATFORM_WIN32_KHR   /* vkCreateWin32SurfaceKHR, for the window's surface */
#include <vulkan/vulkan.h>
#endif
#include <SDL3/SDL_vulkan.h>
#endif
#endif

#define GAME_WIDTH          640u    /* the game's own screen; the window opens at twice this */
#define GAME_HEIGHT         480u
#define WINDOW_TITLE        "Call of Duty: Finest Hour"
#define FPS_LOG_SECONDS     5.0

static unsigned long frames;        /* Swaps so far */

/* An environment switch: unset or empty gives fallback; "0" is off; anything else is on. */
static int setting_on(const char *name, int fallback)
{
    const char *value = getenv(name);

    if (!value || !value[0])
        return fallback;
    return strcmp(value, "0") != 0;
}

static double seconds_now(void)
{
    struct timespec ts;

    if (timespec_get(&ts, TIME_UTC) != TIME_UTC)
        return 0.0;
    return (double)ts.tv_sec + ts.tv_nsec / 1e9;
}

/* Whether frame is one of the comma-separated numbers in list. */
static int in_list(const char *list, unsigned long frame)
{
    char *end;

    while (list && *list) {
        unsigned long n = strtoul(list, &end, 10);

        if (end == list)
            return 0;
        if (n == frame)
            return 1;
        if (*end != ',')
            return 0;
        list = end + 1;
    }
    return 0;
}

/* ------------------------------------------------------------------------------------------------------------------
 * The window */

#if defined(FH_HAVE_SDL3)
static SDL_Window *window;

/* SDL turns SIGINT and SIGTERM into SDL_EVENT_QUIT once any part of it is started, as it does closing the window.
 * Checked every frame: a quit ends the game. Other events are left for pump_window. */
static void check_quit(void)
{
    SDL_Event event;

    if (!SDL_WasInit(SDL_INIT_EVENTS))
        return;
    SDL_PumpEvents();
    if (SDL_PeepEvents(&event, 1, SDL_GETEVENT, SDL_EVENT_QUIT, SDL_EVENT_QUIT) > 0) {
        fprintf(stderr, "[fh] asked to quit (the window was closed, or SIGINT or SIGTERM): ending\n");
        fflush(NULL);
        exit(0);
    }
}
#else
static void check_quit(void)
{
}
#endif

#if defined(FH_HAVE_SDL3) && defined(FH_HAVE_VULKAN)
#if defined(_WIN32)
/* The SDK builds SDL3 for Windows without its Vulkan part, so on Windows the window is a plain one and its surface is
 * made here from the window's handle, with the two instance extensions a Windows surface needs. */
static const char *const window_extensions[] = {VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_WIN32_SURFACE_EXTENSION_NAME};
#define WINDOW_FLAGS (SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY)

static int make_window_surface(void *instance, void *surface, void *user)
{
    VkWin32SurfaceCreateInfoKHR info = {VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR};

    info.hinstance = GetModuleHandleW(NULL);
    info.hwnd = (HWND)SDL_GetPointerProperty(SDL_GetWindowProperties((SDL_Window *)user),
                                             SDL_PROP_WINDOW_WIN32_HWND_POINTER, NULL);
    if (!info.hwnd)
        return -1;
    return vkCreateWin32SurfaceKHR((VkInstance)instance, &info, NULL, (VkSurfaceKHR *)surface) == VK_SUCCESS ? 0 : -1;
}

static const char *const *window_instance_extensions(Uint32 *count)
{
    *count = (Uint32)(sizeof window_extensions / sizeof window_extensions[0]);
    return window_extensions;
}

/* FH_START_IN_BACKGROUND on Windows: the window, still hidden, goes to the bottom of the stacking order, so SDL shows
 * it there without activating it. */
static void send_behind_other_windows(SDL_Window *w)
{
    HWND hwnd = (HWND)SDL_GetPointerProperty(SDL_GetWindowProperties(w), SDL_PROP_WINDOW_WIN32_HWND_POINTER, NULL);

    if (hwnd)
        SetWindowPos(hwnd, HWND_BOTTOM, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
}
#else
#define WINDOW_FLAGS (SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY)

static int make_window_surface(void *instance, void *surface, void *user)
{
    return SDL_Vulkan_CreateSurface((SDL_Window *)user, (VkInstance)instance, NULL, (VkSurfaceKHR *)surface) ? 0 : -1;
}

static const char *const *window_instance_extensions(Uint32 *count)
{
    return SDL_Vulkan_GetInstanceExtensions(count);
}

/* FH_START_IN_BACKGROUND on X11 uses window manager hints SDL has no call for, through Xlib loaded at run time (the
 * port doesn't link it). Each does nothing when the window isn't an X11 one:
 * - _NET_WM_USER_TIME 0 on the window before it's mapped asks the window manager not to focus it when it's mapped
 *   (ask_x11_not_to_focus_on_map);
 * - _NET_WM_STATE_BELOW keeps it under the other windows whatever opens or closes (keep_x11_window_below).
 * The same hints Big Red One's and Call of Duty 3's windows use (the SDK's patch 0063). */
typedef unsigned long x11_atom;
typedef unsigned long (*x11_intern_atom_fn)(void *display, const char *name, int only_if_exists);
typedef int (*x11_change_property_fn)(void *display, unsigned long window, x11_atom property, x11_atom type,
                                      int format, int mode, const unsigned char *data, int count);
typedef int (*x11_flush_fn)(void *display);
typedef int (*x11_send_event_fn)(void *display, unsigned long window, int propagate, long mask, void *event);
typedef unsigned long (*x11_default_root_window_fn)(void *display);
#define X11_XA_CARDINAL         6ul                         /* Xatom.h */
#define X11_PROP_MODE_REPLACE   0                           /* Xlib.h */
#define X11_CLIENT_MESSAGE      33                          /* X.h */
#define X11_SUBSTRUCTURE_MASKS  ((1l << 19) | (1l << 20))   /* SubstructureNotifyMask | SubstructureRedirectMask */

static struct {
    SDL_SharedObject *library;
    void *display;
    unsigned long window, root;
    x11_intern_atom_fn intern_atom;
    x11_change_property_fn change_property;
    x11_flush_fn flush;
    x11_send_event_fn send_event;
} xlib;

/* Loads Xlib and finds the window's X11 display, window and root, once. Returns 1 when all of that worked. */
static int load_xlib(SDL_Window *w)
{
    SDL_PropertiesID props = SDL_GetWindowProperties(w);
    x11_default_root_window_fn default_root_window;

    if (xlib.library)
        return 1;
    xlib.display = SDL_GetPointerProperty(props, SDL_PROP_WINDOW_X11_DISPLAY_POINTER, NULL);
    xlib.window = (unsigned long)SDL_GetNumberProperty(props, SDL_PROP_WINDOW_X11_WINDOW_NUMBER, 0);
    if (!xlib.display || !xlib.window || !(xlib.library = SDL_LoadObject("libX11.so.6")))
        return 0;
    xlib.intern_atom = (x11_intern_atom_fn)SDL_LoadFunction(xlib.library, "XInternAtom");
    xlib.change_property = (x11_change_property_fn)SDL_LoadFunction(xlib.library, "XChangeProperty");
    xlib.flush = (x11_flush_fn)SDL_LoadFunction(xlib.library, "XFlush");
    xlib.send_event = (x11_send_event_fn)SDL_LoadFunction(xlib.library, "XSendEvent");
    default_root_window = (x11_default_root_window_fn)SDL_LoadFunction(xlib.library, "XDefaultRootWindow");
    if (!xlib.intern_atom || !xlib.change_property || !xlib.flush || !xlib.send_event || !default_root_window) {
        SDL_UnloadObject(xlib.library);
        xlib.library = NULL;
        return 0;
    }
    xlib.root = default_root_window(xlib.display);
    return 1;
}

static int ask_x11_not_to_focus_on_map(SDL_Window *w)
{
    x11_atom user_time;
    long zero = 0;

    if (!load_xlib(w) || !(user_time = xlib.intern_atom(xlib.display, "_NET_WM_USER_TIME", 0)))
        return 0;
    xlib.change_property(xlib.display, xlib.window, user_time, X11_XA_CARDINAL, 32, X11_PROP_MODE_REPLACE,
                         (const unsigned char *)&zero, 1);
    xlib.flush(xlib.display);
    return 1;
}

/* Adds (below = 1) or takes off (0) _NET_WM_STATE_BELOW on the mapped window. A client asks for it with a message to
 * the root window (EWMH), laid out as Xlib's XClientMessageEvent inside an XEvent's 24 longs. */
static void keep_x11_window_below(int below)
{
    union {
        struct {
            int type;
            unsigned long serial;
            int send_event;
            void *display;
            unsigned long window;
            x11_atom message_type;
            int format;
            long data[5];
        } message;
        long size[24];
    } event;
    x11_atom state, state_below;

    if (!xlib.library || !(state = xlib.intern_atom(xlib.display, "_NET_WM_STATE", 0)) ||
        !(state_below = xlib.intern_atom(xlib.display, "_NET_WM_STATE_BELOW", 0)))
        return;
    memset(&event, 0, sizeof event);
    event.message.type = X11_CLIENT_MESSAGE;
    event.message.window = xlib.window;
    event.message.message_type = state;
    event.message.format = 32;
    event.message.data[0] = below ? 1 : 0;      /* _NET_WM_STATE_ADD or _NET_WM_STATE_REMOVE */
    event.message.data[1] = (long)state_below;
    event.message.data[3] = 1;                  /* asked by an application */
    xlib.send_event(xlib.display, xlib.root, 0, X11_SUBSTRUCTURE_MASKS, &event);
    xlib.flush(xlib.display);
}
#endif

static int background_window;       /* FH_START_IN_BACKGROUND: opened behind the window in use */
static int kept_in_background;      /* X11: can't take the focus and stays under the others until it's clicked */
static int background_done;         /* the first click brought it forward: not again */
static int fullscreen_waits;        /* full screen asked for with FH_START_IN_BACKGROUND: once it's clicked */

/* [acct2-fh] 2026-11-13 How the window meets the screen (display.mode). */
enum display_mode { MODE_WINDOW, MODE_BORDERLESS, MODE_EXCLUSIVE };
static enum display_mode display_mode;
static uint32_t view_width, view_height;    /* the window's drawable size in pixels (0 x 0 minimized) */
static void keep_window_settings(void);     /* [acct2-fh] 11-14: below, with follow_window_settings */

/* [acct2-fh] 2026-11-14 display.vsync as ngl_vk's present mode: true (the default) 0, in step with the display;
 * false 1, the newest frame at each refresh (mailbox: no tearing, no wait); "immediate" 2, each frame the moment it
 * is done (it may tear). */
static int present_mode_setting(void)
{
    const char *text = fh_setting_text("display.vsync", "FH_VSYNC");

    if (text && !strcmp(text, "immediate"))
        return 2;
    return fh_setting_bool("display.vsync", "FH_VSYNC", 1) ? 0 : 1;
}

/* [acct2-fh] 2026-11-14 display.scaling: how ngl_vk scales the finished picture into the window. */
static int scaling_setting(void)
{
    static char warned[64];
    const char *text = fh_setting_text("display.scaling", "FH_SCALING");

    if (!text || !text[0] || !strcmp(text, "smooth") || !strcmp(text, "bilinear"))
        return 0;
    if (!strcmp(text, "sharp") || !strcmp(text, "nearest"))
        return 1;
    if (!strcmp(text, "integer"))
        return 2;
    if (!strcmp(text, "cas"))       /* [acct2-fh] stage 5b */
        return 3;
    if (!strcmp(text, "fsr") || !strcmp(text, "fsr1"))
        return 4;
    if (strcmp(warned, text)) {     /* said once for each new value, not at each look */
        fprintf(stderr, "[fh] display.scaling = \"%s\" isn't smooth, sharp, integer, cas or fsr: smooth\n", text);
        snprintf(warned, sizeof warned, "%s", text);
    }
    return 0;
}

static const char *scaling_name(int scaling)
{
    static const char *const names[] = {"smooth", "sharp", "integer", "AMD CAS", "AMD FSR 1"};

    return scaling >= 0 && scaling < 5 ? names[scaling] : "smooth";
}

/* [acct2-fh] 2026-11-14 stage 5b: CAS's and FSR 1's three numbers, as Call of Duty 3's presenter has them
 * (present_cas_additional_sharpness, present_fsr_sharpness_reduction, present_fsr_max_upsampling_passes, with their
 * defaults), FSR's passes up to 8 rather than 4 (8 doublings take 64 pixels to 16384). */
struct window_sharpness {
    double cas, fsr_reduction;
    int fsr_passes;
};

static struct window_sharpness sharpness_setting(void)
{
    struct window_sharpness s;

    s.cas = fh_setting_number("display.cas_sharpness", "FH_CAS_SHARPNESS", 0.0);
    s.cas = !(s.cas > 0.0) ? 0.0 : s.cas > 1.0 ? 1.0 : s.cas;
    s.fsr_reduction = fh_setting_number("display.fsr_sharpness_reduction", "FH_FSR_SHARPNESS_REDUCTION", 0.2);
    s.fsr_reduction = !(s.fsr_reduction > 0.0) ? 0.0 : s.fsr_reduction > 2.0 ? 2.0 : s.fsr_reduction;
    s.fsr_passes = fh_setting_int("display.fsr_passes", "FH_FSR_PASSES", 4);
    s.fsr_passes = s.fsr_passes < 1 ? 1 : s.fsr_passes > 8 ? 8 : s.fsr_passes;
    return s;
}

static int same_sharpness(struct window_sharpness a, struct window_sharpness b)
{
    return a.cas == b.cas && a.fsr_reduction == b.fsr_reduction && a.fsr_passes == b.fsr_passes;
}

/* Both handed to ngl_vk (their ngl_vk_set_* keep them across a remake); without the calls in ngl_vk, V-Sync waits
 * for the next start and the scaling stays smooth (and CAS / FSR 1 too, without ngl_vk's window filters). */
static void apply_present_settings(int present_mode, int scaling, struct window_sharpness sharpness)
{
#if defined(NGL_VK_HAS_PRESENT_MODE)
    ngl_vk_set_present_mode(present_mode);
#else
    (void)present_mode;
#endif
#if defined(NGL_VK_HAS_WINDOW_FILTERS)
    ngl_vk_set_window_sharpness((float)sharpness.cas, (float)sharpness.fsr_reduction, sharpness.fsr_passes);
#else
    (void)sharpness;
    if (scaling > 2) {
        fprintf(stderr, "[fh] display.scaling %s: this ngl_vk doesn't have it: smooth\n", scaling_name(scaling));
        scaling = 0;
    }
#endif
#if defined(NGL_VK_HAS_WINDOW_SCALING)
#if defined(NGL_VK_HAS_WINDOW_FILTERS)
    if (scaling == 3 || scaling == 4) {
        ngl_vk_set_window_scaling(scaling == 3 ? NGL_VK_SCALE_CAS : NGL_VK_SCALE_FSR);
        return;
    }
#endif
    ngl_vk_set_window_scaling(scaling == 1 ? NGL_VK_SCALE_SHARP : scaling == 2 ? NGL_VK_SCALE_INTEGER
                                                                               : NGL_VK_SCALE_SMOOTH);
#else
    (void)scaling;
#endif
}

static enum display_mode display_mode_setting(void)
{
    const char *variable = getenv("FH_DISPLAY_MODE"), *old = getenv("FH_FULLSCREEN");
    const char *mode;

    /* FH_FULLSCREEN, the variable before display.mode: a variable wins over fh.toml, as every other one does */
    if ((!variable || !variable[0]) && old && old[0])
        return setting_on("FH_FULLSCREEN", 0) ? MODE_BORDERLESS : MODE_WINDOW;
    mode = fh_setting_text("display.mode", "FH_DISPLAY_MODE");
    if (!mode || !mode[0])
        return MODE_WINDOW;
    if (!strcmp(mode, "window") || !strcmp(mode, "windowed"))
        return MODE_WINDOW;
    if (!strcmp(mode, "borderless") || !strcmp(mode, "fullscreen"))
        return MODE_BORDERLESS;
    if (!strcmp(mode, "exclusive"))
        return MODE_EXCLUSIVE;
    fprintf(stderr, "[fh] display.mode = \"%s\" isn't window, borderless or exclusive: window\n", mode);
    return MODE_WINDOW;
}

static const char *mode_name(enum display_mode mode)
{
    return mode == MODE_EXCLUSIVE ? "exclusive full screen" : mode == MODE_BORDERLESS ? "borderless full screen"
                                                                                      : "a window";
}

/* X11, FH_START_IN_BACKGROUND: from the window's first SHOWN to its first click, it can't take the focus and stays
 * under the other windows; the first click undoes both. (Not from its creation: Big Red One found that a window made
 * unfocusable from the start stayed on top of everything.) */
static void keep_in_background(int keep)
{
#if !defined(_WIN32)
    if (keep == kept_in_background || !xlib.library)
        return;
    SDL_SetWindowFocusable(window, !keep);
    keep_x11_window_below(keep);
    kept_in_background = keep;
    if (keep) {
        fprintf(stderr, "[fh] the window takes no focus and stays under the others until it's clicked\n");
    } else {
        background_done = 1;
        SDL_RaiseWindow(window);
        fprintf(stderr, "[fh] the window was clicked: it can take the focus now\n");
    }
#else
    (void)keep;
#endif
}

/* [acct2-fh] display.monitor: the monitor's name (as the system gives it) or its number from 1, as CoD3's F2 keeps it
 * by name so it stays put when other screens are plugged in or out. 0: the system's choice. */
static SDL_DisplayID chosen_display(void)
{
    const char *want = fh_setting_text("display.monitor", "FH_MONITOR");
    SDL_DisplayID *ids, id = 0;
    int count = 0;

    if (!want || !want[0])
        return 0;
    ids = SDL_GetDisplays(&count);
    if (!ids)
        return 0;
    for (int i = 0; i < count && !id; i++) {
        const char *name = SDL_GetDisplayName(ids[i]);

        if (name && !strcmp(name, want))
            id = ids[i];
    }
    if (!id) {
        char *end;
        long number = strtol(want, &end, 10);

        if (end != want && !*end && number >= 1 && number <= count)
            id = ids[number - 1];
    }
    if (!id)
        fprintf(stderr,
                "[fh] display.monitor = \"%s\": no such monitor connected (%d connected); the system's choice\n",
                want, count);
    SDL_free(ids);
    return id;
}

/* The monitor the window is on, its desktop mode and the mode it shows now, for the log. */
static void say_display(const char *when)
{
    SDL_DisplayID id = SDL_GetDisplayForWindow(window);
    const SDL_DisplayMode *desktop = id ? SDL_GetDesktopDisplayMode(id) : NULL;
    const SDL_DisplayMode *now = id ? SDL_GetCurrentDisplayMode(id) : NULL;
    const char *name = id ? SDL_GetDisplayName(id) : NULL;

    fprintf(stderr, "[fh] display %s: %s on %s", when, mode_name(display_mode), name ? name : "(unknown monitor)");
    if (desktop)
        fprintf(stderr, ", desktop %dx%d at %.2f Hz", desktop->w, desktop->h, desktop->refresh_rate);
    if (now && desktop && (now->w != desktop->w || now->h != desktop->h || now->refresh_rate != desktop->refresh_rate))
        fprintf(stderr, ", now %dx%d at %.2f Hz", now->w, now->h, now->refresh_rate);
    fprintf(stderr, "; drawable %ux%u\n", view_width, view_height);
}

/* [acct2-fh] Puts the window in its display.mode: a window, borderless full screen (the desktop's own mode), or
 * exclusive full screen at the monitor's mode nearest display.fullscreen_size and display.refresh_rate (the desktop's
 * size and rate when they are left out). */
static void apply_display_mode(void)
{
    SDL_DisplayID id = SDL_GetDisplayForWindow(window);

    if (display_mode == MODE_WINDOW) {
        SDL_SetWindowFullscreen(window, false);
        return;
    }
    if (display_mode == MODE_EXCLUSIVE) {
        const SDL_DisplayMode *desktop = id ? SDL_GetDesktopDisplayMode(id) : NULL;
        uint32_t w = desktop ? (uint32_t)desktop->w : 0, h = desktop ? (uint32_t)desktop->h : 0;
        float hz = (float)fh_setting_number("display.refresh_rate", "FH_REFRESH_RATE", 0.0);
        const char *size = fh_setting_text("display.fullscreen_size", "FH_FULLSCREEN_SIZE");
        SDL_DisplayMode mode;

        if (size && size[0] && !fh_resolution_parse_size(size, 320, 200, FH_MAX_SIZE, &w, &h))
            fprintf(stderr, "[fh] display.fullscreen_size = \"%s\" isn't WxH: the desktop's size\n", size);
        if (hz <= 0.0f && desktop)
            hz = desktop->refresh_rate;
        if (id && w && h && SDL_GetClosestFullscreenDisplayMode(id, (int)w, (int)h, hz, true, &mode)) {
            SDL_SetWindowFullscreenMode(window, &mode);
            fprintf(stderr, "[fh] exclusive full screen: %dx%d at %.2f Hz (asked for %ux%u at %.2f Hz)\n", mode.w,
                    mode.h, mode.refresh_rate, w, h, hz);
        } else {
            SDL_SetWindowFullscreenMode(window, NULL);
            fprintf(stderr, "[fh] exclusive full screen: no mode near %ux%u at %.2f Hz (%s); borderless instead\n",
                    w, h, hz, SDL_GetError());
        }
    } else {
        const char *size = fh_setting_text("display.fullscreen_size", "FH_FULLSCREEN_SIZE");

        /* fh.toml lists both at their defaults (0, ""): only a value of the player's own is worth the line */
        if (fh_setting_number("display.refresh_rate", "FH_REFRESH_RATE", 0.0) > 0.0 || (size && size[0]))
            fprintf(stderr, "[fh] display.refresh_rate and display.fullscreen_size are for mode = \"exclusive\"; "
                    "borderless keeps the desktop's own mode\n");
        SDL_SetWindowFullscreenMode(window, NULL);
    }
    SDL_SetWindowFullscreen(window, true);
}

/* A window for ngl_vk to show the frames in, when SDL3 has a display and FH_WINDOW isn't 0, on display.monitor, of
 * display.window_size, then in display.mode. With FH_START_IN_BACKGROUND it is made hidden, then shown without being
 * activated (SDL_HINT_WINDOW_ACTIVATE_WHEN_SHOWN "0", no XDG_ACTIVATION_TOKEN) and below the window in use (X11:
 * _NET_WM_USER_TIME 0 before it's mapped; Windows: the bottom of the stacking order), and full screen waits for its
 * first click. Returns 1 when there is one. Called before ngl_vk_init. */
static int open_window(void)
{
    const char *const *extensions;
    const char *size = fh_setting_text("display.window_size", "FH_WINDOW_SIZE");
    int present_mode = present_mode_setting();
    uint32_t window_width = 2 * GAME_WIDTH, window_height = 2 * GAME_HEIGHT;
    int width = 0, height = 0;
    Uint32 count = 0;
    SDL_WindowFlags flags = WINDOW_FLAGS;
    SDL_DisplayID display;

    if (!setting_on("FH_WINDOW", 1))
        return 0;
    background_window = setting_on("FH_START_IN_BACKGROUND", 0);
    display_mode = display_mode_setting();
    if (size && size[0] && !fh_resolution_parse_size(size, 160, 120, FH_MAX_SIZE, &window_width, &window_height))
        fprintf(stderr, "[fh] display.window_size = \"%s\" isn't WxH: %ux%u\n", size, window_width, window_height);
    if (!SDL_InitSubSystem(SDL_INIT_VIDEO)) {
        fprintf(stderr, "[fh] no window (%s): drawing offscreen\n", SDL_GetError());
        return 0;
    }
    display = chosen_display();
    flags |= SDL_WINDOW_HIDDEN;             /* shown below, once it's on its monitor (and sent behind) */
    fullscreen_waits = background_window && display_mode != MODE_WINDOW;
    {
        SDL_PropertiesID props = SDL_CreateProperties();

        SDL_SetStringProperty(props, SDL_PROP_WINDOW_CREATE_TITLE_STRING, WINDOW_TITLE);
        SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_WIDTH_NUMBER, window_width);
        SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_HEIGHT_NUMBER, window_height);
        SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_FLAGS_NUMBER, (Sint64)flags);
        if (display) {
            SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_X_NUMBER, SDL_WINDOWPOS_CENTERED_DISPLAY(display));
            SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_Y_NUMBER, SDL_WINDOWPOS_CENTERED_DISPLAY(display));
        }
        window = SDL_CreateWindowWithProperties(props);
        SDL_DestroyProperties(props);
    }
    if (!window || !(extensions = window_instance_extensions(&count))) {
        fprintf(stderr, "[fh] no window for Vulkan (%s): drawing offscreen\n", SDL_GetError());
        if (window)
            SDL_DestroyWindow(window);
        window = NULL;
        return 0;
    }
    if (!fullscreen_waits && display_mode != MODE_WINDOW)
        apply_display_mode();
    if (background_window) {
        SDL_SetHint(SDL_HINT_WINDOW_ACTIVATE_WHEN_SHOWN, "0");
        if (SDL_getenv("XDG_ACTIVATION_TOKEN"))
            SDL_unsetenv_unsafe("XDG_ACTIVATION_TOKEN");    /* Wayland: it would let the compositor focus it */
#if defined(_WIN32)
        send_behind_other_windows(window);
#else
        ask_x11_not_to_focus_on_map(window);
#endif
        SDL_ShowWindow(window);
        SDL_ResetHint(SDL_HINT_WINDOW_ACTIVATE_WHEN_SHOWN);
        fprintf(stderr, "[fh] window opened in the background (FH_START_IN_BACKGROUND)%s%s\n",
                fullscreen_waits ? "; " : "", fullscreen_waits ? mode_name(display_mode) : "");
        if (fullscreen_waits)
            fprintf(stderr, "[fh] ... when it's first clicked\n");
    } else {
        SDL_ShowWindow(window);
    }
    SDL_GetWindowSizeInPixels(window, &width, &height);
    view_width = (uint32_t)width;
    view_height = (uint32_t)height;
    say_display("at the start");
    keep_window_settings();     /* [acct2-fh] what follow_window_settings compares with */
    ngl_vk_set_window(extensions, count, make_window_surface, window, (uint32_t)width, (uint32_t)height,
                      present_mode);
    apply_present_settings(present_mode, scaling_setting(), sharpness_setting());   /* [acct2-fh] 11-14 */
    return 1;
}

/* The window's events since the last frame: a new size goes to ngl_vk (and to the render resolution's plan); a
 * background window's first SHOWN and first click change how it takes the focus, and full screen waits for that
 * click. The rest are dropped. */
static void pump_window(void)
{
    SDL_Event event;
    int clicked = 0;

    if (!window)
        return;
    while (SDL_PeepEvents(&event, 1, SDL_GETEVENT, SDL_EVENT_MOUSE_BUTTON_DOWN, SDL_EVENT_MOUSE_BUTTON_DOWN) > 0)
        clicked = 1;
    if (clicked && kept_in_background)
        keep_in_background(0);
    while (SDL_PeepEvents(&event, 1, SDL_GETEVENT, SDL_EVENT_WINDOW_FIRST, SDL_EVENT_WINDOW_LAST) > 0) {
        if (event.type == SDL_EVENT_WINDOW_SHOWN && background_window && !background_done)
            keep_in_background(1);
        if (event.type == SDL_EVENT_WINDOW_FOCUS_GAINED && fullscreen_waits) {
            fullscreen_waits = 0;
            apply_display_mode();
        }
        if (event.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED || event.type == SDL_EVENT_WINDOW_RESIZED ||
            event.type == SDL_EVENT_WINDOW_MINIMIZED || event.type == SDL_EVENT_WINDOW_RESTORED ||
            event.type == SDL_EVENT_WINDOW_MAXIMIZED || event.type == SDL_EVENT_WINDOW_ENTER_FULLSCREEN ||
            event.type == SDL_EVENT_WINDOW_LEAVE_FULLSCREEN || event.type == SDL_EVENT_WINDOW_DISPLAY_CHANGED) {
            int width = 0, height = 0;

            if (!(SDL_GetWindowFlags(window) & SDL_WINDOW_MINIMIZED))
                SDL_GetWindowSizeInPixels(window, &width, &height);
            ngl_vk_window_resized((uint32_t)width, (uint32_t)height);   /* 0 x 0 while minimized: nothing shown */
            if (width && height) {      /* minimized: the plan keeps the last size */
                view_width = (uint32_t)width;
                view_height = (uint32_t)height;
            }
            if (event.type == SDL_EVENT_WINDOW_ENTER_FULLSCREEN || event.type == SDL_EVENT_WINDOW_DISPLAY_CHANGED)
                say_display(event.type == SDL_EVENT_WINDOW_ENTER_FULLSCREEN ? "in full screen" : "moved");
        }
    }
    /* everything else but a quit, which check_quit reads at the next frame */
    SDL_FlushEvents(SDL_EVENT_FIRST, SDL_EVENT_QUIT - 1);
    SDL_FlushEvents(SDL_EVENT_QUIT + 1, SDL_EVENT_LAST);
}

/* [acct2-fh] FH_TEST_WINDOW_SIZE=<frame>:<w>x<h>[,...]: the window is given that size at that frame, to check a live
 * change of the render resolution in a test run (the window keeps its place and doesn't take the focus). */
static void test_window_size(unsigned long frame)
{
    const char *list = getenv("FH_TEST_WINDOW_SIZE");
    char *end;

    while (window && list && *list) {
        unsigned long at = strtoul(list, &end, 10);
        unsigned w = 0, h = 0;

        if (end == list || *end != ':' || sscanf(end + 1, "%ux%u", &w, &h) != 2)
            return;
        if (at == frame) {
            fprintf(stderr, "[fh] FH_TEST_WINDOW_SIZE: the window made %ux%u at frame %lu\n", w, h, frame);
            SDL_SetWindowSize(window, (int)w, (int)h);
        }
        list = strchr(end, ',');
        if (list)
            list++;
    }
}

/* [acct2-fh] 2026-11-14 The window's settings changed while the game runs (the F2 window, through fh_settings_set):
 * display.mode, display.monitor, display.fullscreen_size, display.refresh_rate and display.window_size are applied at
 * once, and display.vsync and display.scaling too (ngl_vk makes its swapchain again for a new present mode); the
 * render size and the picture's shape follow by themselves (follow_plan). A window opened in the background (FH_START_IN_BACKGROUND, the
 * test runs) still never goes full screen before its first click. */
static unsigned window_generation;
static struct {
    enum display_mode mode;
    char monitor[128], window_size[64], fullscreen_size[64];
    double refresh_rate;
    int present_mode, scaling;      /* [acct2-fh] stage 5 */
    struct window_sharpness sharpness;  /* [acct2-fh] stage 5b */
} window_kept;

static void keep_window_settings(void)
{
    const char *text;

    window_generation = fh_settings_generation();
    window_kept.mode = display_mode;
    text = fh_setting_text("display.monitor", "FH_MONITOR");
    snprintf(window_kept.monitor, sizeof window_kept.monitor, "%s", text ? text : "");
    text = fh_setting_text("display.window_size", "FH_WINDOW_SIZE");
    snprintf(window_kept.window_size, sizeof window_kept.window_size, "%s", text ? text : "");
    text = fh_setting_text("display.fullscreen_size", "FH_FULLSCREEN_SIZE");
    snprintf(window_kept.fullscreen_size, sizeof window_kept.fullscreen_size, "%s", text ? text : "");
    window_kept.refresh_rate = fh_setting_number("display.refresh_rate", "FH_REFRESH_RATE", 0.0);
    window_kept.present_mode = present_mode_setting();
    window_kept.scaling = scaling_setting();
    window_kept.sharpness = sharpness_setting();
}

/* display.window_size given to the window (a window only: SDL leaves a full-screen one as it is). */
static void apply_window_size(void)
{
    const char *size = fh_setting_text("display.window_size", "FH_WINDOW_SIZE");
    uint32_t w = 0, h = 0;

    if (display_mode != MODE_WINDOW || !size || !size[0])
        return;
    if (!fh_resolution_parse_size(size, 160, 120, FH_MAX_SIZE, &w, &h)) {
        fprintf(stderr, "[fh] display.window_size = \"%s\" isn't WxH: the window keeps its size\n", size);
        return;
    }
    SDL_SetWindowSize(window, (int)w, (int)h);
}

static void follow_window_settings(void)
{
    enum display_mode mode, was;
    const char *text;
    int moved, resized, refit, present_mode, scaling;
    struct window_sharpness sharpness;

    if (!window || fh_settings_generation() == window_generation)
        return;
    was = window_kept.mode;
    present_mode = present_mode_setting();
    scaling = scaling_setting();
    sharpness = sharpness_setting();
    if (present_mode != window_kept.present_mode || scaling != window_kept.scaling ||
        !same_sharpness(sharpness, window_kept.sharpness)) {
        fprintf(stderr, "[fh] display settings changed while playing: V-Sync %s, scaling %s (CAS sharpness %.2f, "
                "FSR 1 %.2f stops less sharp, at most %d EASU passes)\n",
                present_mode == 0 ? "on" : present_mode == 1 ? "off (mailbox)" : "immediate", scaling_name(scaling),
                sharpness.cas, sharpness.fsr_reduction, sharpness.fsr_passes);
        apply_present_settings(present_mode, scaling, sharpness);
    }
    mode = display_mode_setting();
    text = fh_setting_text("display.monitor", "FH_MONITOR");
    moved = strcmp(text ? text : "", window_kept.monitor) != 0;
    text = fh_setting_text("display.window_size", "FH_WINDOW_SIZE");
    resized = strcmp(text ? text : "", window_kept.window_size) != 0;
    text = fh_setting_text("display.fullscreen_size", "FH_FULLSCREEN_SIZE");
    refit = mode != was || moved ||
            (mode == MODE_EXCLUSIVE &&
             (strcmp(text ? text : "", window_kept.fullscreen_size) != 0 ||
              fh_setting_number("display.refresh_rate", "FH_REFRESH_RATE", 0.0) != window_kept.refresh_rate));
    display_mode = mode;
    keep_window_settings();
    if (!moved && !resized && !refit)
        return;                     /* another setting changed */
    fprintf(stderr, "[fh] display settings changed while playing:%s%s%s\n", mode != was ? " the mode" : "",
            moved ? " the monitor" : "", resized ? " the window's size" : (refit && mode == was && !moved)
                                                                          ? " the full-screen mode" : "");
    if (moved) {
        SDL_DisplayID id = chosen_display();

        if (was != MODE_WINDOW)
            SDL_SetWindowFullscreen(window, false);     /* out of full screen to move, back in below */
        if (id)
            SDL_SetWindowPosition(window, (int)SDL_WINDOWPOS_CENTERED_DISPLAY(id),
                                  (int)SDL_WINDOWPOS_CENTERED_DISPLAY(id));
    }
    if (refit) {
        if (background_window && !background_done) {
            fullscreen_waits = mode != MODE_WINDOW;     /* full screen still waits for the first click */
            if (fullscreen_waits)
                fprintf(stderr, "[fh] ... %s when the window is first clicked\n", mode_name(mode));
        } else {
            apply_display_mode();
        }
    }
    if (resized || (refit && mode == MODE_WINDOW && was != MODE_WINDOW))
        apply_window_size();
    say_display("after the change");
}

static void set_window_title(const char *title)
{
    if (window)
        SDL_SetWindowTitle(window, title);
}
#else
static uint32_t view_width, view_height;

static int open_window(void)
{
    return 0;
}

static void test_window_size(unsigned long frame)
{
    (void)frame;
}

static void pump_window(void)
{
}

static void set_window_title(const char *title)
{
    (void)title;
}

static void follow_window_settings(void)
{
}
#endif

/* ------------------------------------------------------------------------------------------------------------------
 * The frame rate (FH_SHOW_FPS) */

static void count_frame_rate(void)
{
    static double second_start, log_start;
    static unsigned long second_frames, log_frames;
    static int show = -1;
    double now = seconds_now();

    if (show < 0)
        show = setting_on("FH_SHOW_FPS", 0);
    if (!show)
        return;
    if (second_start == 0.0)
        second_start = log_start = now;
    second_frames++;
    log_frames++;
    if (now - second_start >= 1.0) {
        char title[128];

        snprintf(title, sizeof title, "%s - %.0f fps", WINDOW_TITLE, second_frames / (now - second_start));
        set_window_title(title);
        second_start = now;
        second_frames = 0;
    }
    if (now - log_start >= FPS_LOG_SECONDS) {
        fprintf(stderr, "[fh] frame rate: %.1f frames a second (%lu frames in %.1f s; frame %lu)\n",
                log_frames / (now - log_start), log_frames, now - log_start, frames);
        fh_draw_report();
        log_start = now;
        log_frames = 0;
    }
}

/* ------------------------------------------------------------------------------------------------------------------
 * Drawing */

#if defined(FH_HAVE_VULKAN)
/* What Vulkan drew, saved as a binary PPM (FH_VK_DUMP), with how much of it isn't black. ngl_vk's target is red,
 * green, blue, alpha. */
static void save_target(const char *path)
{
    uint32_t width = 0, height = 0;
    const uint8_t *pixels = ngl_vk_target_pixels(&width, &height);
    unsigned long lit = 0;
    FILE *out;

    if (!pixels || !width || !height) {
        fprintf(stderr, "[fh] Vulkan: frame %lu: no picture to save\n", frames);
        return;
    }
    out = fh_fopen(path, "wb");
    if (!out) {
        fprintf(stderr, "[fh] Vulkan: frame %lu: cannot write %s\n", frames, path);
        return;
    }
    fprintf(out, "P6\n%u %u\n255\n", width, height);
    for (size_t i = 0; i < (size_t)width * height; i++) {
        lit += pixels[i * 4] || pixels[i * 4 + 1] || pixels[i * 4 + 2];
        fwrite(pixels + i * 4, 1, 3, out);
    }
    fclose(out);
    fprintf(stderr, "[fh] Vulkan: frame %lu saved to %s, %ux%u (%lu pixels not black)\n", frames, path, width, height,
            lit);
}

/* [acct2-fh] 2026-11-13 The render resolution (fh_resolution.c): the plan ngl_vk's picture was made for, and a new one
 * waiting to be applied (it is, once it has stayed the same for RESIZE_SETTLE_SECONDS: a window being dragged). */
#define RESIZE_SETTLE_SECONDS   0.3
static struct fh_render_plan plan_now, plan_waiting;
static double waiting_since;
static uint32_t planned_view_width, planned_view_height;
static unsigned planned_generation;

static void apply_window_table(void);   /* the gamma ramp, below */
static double make_open_ms, make_start_ms;  /* [acct2-fh] the last make_target: ngl_vk_init, then the rest */

/* Makes ngl_vk's picture for plan (ngl_vk_init), then the drawing layer on it. Returns 0 when both are ready. */
static int make_target(const struct fh_render_plan *plan)
{
    uint8_t const *pixels = NULL;
    uint32_t width = 0, height = 0, pitch = 0, format = 0;
    double start = seconds_now(), opened;

    if (ngl_vk_init(plan->width, plan->height))
        return -1;
    opened = seconds_now();
    make_open_ms = (opened - start) * 1e3;
    fh_aspect_set_shape(plan->picture_shape);   /* [acct2-fh] the 3D view's shape (fh_aspect.c) */
    apply_window_table();   /* the gamma ramp lives in ngl_vk's window settings, but say it again after a remake */
    /* the game's own draws (fh_draw.c); the game's screen is the render target's size */
    if (fh_d3d_render_target_picture(&pixels, &width, &height, &pitch, &format))
        fh_draw_start(width, height);
    else
        fh_draw_start(GAME_WIDTH, GAME_HEIGHT);
    fh_f2_vulkan_start();       /* [acct2-fh] the F2 window's part, after a remake (nothing before fh_f2_init) */
    make_start_ms = (seconds_now() - opened) * 1e3;
    return 0;
}

/* [acct2-fh] 2026-11-14 After make_target failed: once more at the same size, before a smaller one. The first try can
 * fail for a moment when another program is starting on the GPU, not for the size (boot47 a: vkCreateDevice -4, then
 * 640x480 until a later setting change made 1280x960 at once). Returns 0 when the picture is made. */
static int make_target_again(const struct fh_render_plan *plan)
{
    fprintf(stderr, "[fh] Vulkan didn't start at %ux%u: once more at that size\n", plan->width, plan->height);
    ngl_vk_shutdown();
    return make_target(plan);
}

/* Between two frames: ngl_vk closed and made again at the new plan's size (its window, the window's size and its
 * settings are kept; what was made on its device goes, and fh_draw.c makes its textures again as they are drawn).
 * When the GPU can't make that size, the old one is made again. Returns 0 when there is a picture. */
static int remake_target(const struct fh_render_plan *plan)
{
    char text[400];
    struct timespec start, end;

    double stopped, closed;

    timespec_get(&start, TIME_UTC);
    fh_draw_stop();
    fh_f2_vulkan_stop();        /* [acct2-fh] what the F2 window made on ngl_vk's device */
    stopped = seconds_now();
    ngl_vk_shutdown();
    closed = seconds_now();
    if (make_target(plan) && make_target_again(plan)) {
        fh_resolution_describe(plan, text, sizeof text);
        fprintf(stderr, "[fh] resolution: the GPU couldn't make %ux%u (%s); back to %ux%u\n", plan->width,
                plan->height, text, plan_now.width, plan_now.height);
        ngl_vk_shutdown();
        if (make_target(&plan_now)) {
            fprintf(stderr, "[fh] Vulkan didn't start again; frames are not drawn from here on\n");
            return -1;
        }
        plan_waiting = *plan;   /* not asked for again until something changes */
        return 0;
    }
    timespec_get(&end, TIME_UTC);
    fh_resolution_describe(plan, text, sizeof text);
    fprintf(stderr, "[fh] resolution changed at frame %lu: %s (%.0f ms: drawing layer stopped %.0f, ngl_vk closed "
            "%.0f, opened %.0f, drawing layer started %.0f)\n", frames, text,
            (end.tv_sec - start.tv_sec) * 1e3 + (end.tv_nsec - start.tv_nsec) / 1e6,
            (stopped - (start.tv_sec + start.tv_nsec / 1e9)) * 1e3, (closed - stopped) * 1e3, make_open_ms,
            make_start_ms);
    plan_now = *plan;
    return 0;
}

/* Each frame, with live_resolution on: the plan for the window's size and the settings now; a new one is applied once
 * it has stayed the same for a moment. Returns -1 when the picture is lost. */
static int follow_plan(void)
{
    struct fh_render_plan plan;
    double now;

    if (view_width == planned_view_width && view_height == planned_view_height &&
        fh_settings_generation() == planned_generation && fh_resolution_same_target(&plan_waiting, &plan_now))
        return 0;
    planned_view_width = view_width;
    planned_view_height = view_height;
    planned_generation = fh_settings_generation();
    if (!fh_resolution_live())
        return 0;
    fh_resolution_plan(view_width, view_height, &plan);
    if (fh_resolution_same_target(&plan, &plan_now)) {
        plan_now = plan;        /* the same size: the view, the note or the picture's shape may differ */
        plan_waiting = plan;
        fh_aspect_set_shape(plan.picture_shape);
        return 0;
    }
    now = seconds_now();
    if (!fh_resolution_same_target(&plan, &plan_waiting)) {
        plan_waiting = plan;
        waiting_since = now;
        planned_view_width = 0; /* look again next frame, until it has settled */
        return 0;
    }
    if (now - waiting_since < RESIZE_SETTLE_SECONDS) {
        planned_view_width = 0;
        return 0;
    }
    return remake_target(&plan);
}

/* Shows the render target: started on the first frame (the window, then Vulkan), then once a frame. */
static void show_render_target(void)
{
    static int state;               /* 0: not started, 1: drawing, -1: off */
    static int wrong_format_said;
    const char *dump_frames = getenv("FH_VK_DUMP"), *dump_path = fh_getenv_utf8("FH_VK_DUMP_PATH");
    static uint32_t last_locks;
    const uint8_t *pixels = NULL;
    uint32_t width = 0, height = 0, pitch = 0, format = 0, locks;
    int dumping, have_picture, written;

    if (state == 0) {
        char text[400];

        if (!setting_on("FH_VK", 1)) {
            state = -1;
            return;
        }
        open_window();
        fh_resolution_plan(view_width, view_height, &plan_now);
        if (make_target(&plan_now) && make_target_again(&plan_now)) {
            struct fh_render_plan game = plan_now;    /* [acct2-fh] too big for this GPU: the game's own size */

            game.width = GAME_WIDTH;
            game.height = GAME_HEIGHT;
            game.shape = game.picture_shape = (float)GAME_WIDTH / (float)GAME_HEIGHT;
            snprintf(game.note, sizeof game.note, "the GPU couldn't make %ux%u", plan_now.width, plan_now.height);
            ngl_vk_shutdown();
            if (make_target(&game)) {
                fprintf(stderr, "[fh] Vulkan didn't start; frames are not drawn\n");
                state = -1;
                return;
            }
            plan_now = game;
        }
        plan_waiting = plan_now;
        planned_view_width = view_width;
        planned_view_height = view_height;
        planned_generation = fh_settings_generation();
        fh_resolution_describe(&plan_now, text, sizeof text);
        fprintf(stderr, "[fh] Vulkan: drawing on %s, %ux%u, %s\n", ngl_vk_device_name(), plan_now.width,
                plan_now.height, ngl_vk_showing() ? "shown in the window" : "offscreen");
        fprintf(stderr, "[fh] resolution: %s\n", text);
        state = 1;
#if defined(FH_HAVE_F2)
        if (window && ngl_vk_showing())
            fh_f2_init(window);     /* [acct2-fh] 11-14: the F2 settings window (f2/fh_f2.cpp) */
#endif
    }
    fh_f2_events();             /* [acct2-fh] F2's keys and its next picture, before pump_window drops the events */
    fh_console_frame();         /* [acct2-fh] 11-14: FH_CONSOLE_EXEC's commands, on the game's thread */
    pump_window();
    follow_window_settings();   /* [acct2-fh] a window setting changed in F2 */
    if (state < 0)
        return;
    if (follow_plan()) {        /* [acct2-fh] a new render resolution, between two frames */
        state = -1;
        return;
    }
    fh_aspect_follow();         /* [acct2-fh] a new shape reaches the cameras already set, on the game's thread */
    have_picture = fh_d3d_render_target_picture(&pixels, &width, &height, &pitch, &format);
    if (have_picture && !fh_d3d_format_is_xrgb32(format)) {
        if (!wrong_format_said++)
            fprintf(stderr, "[fh] the render target's format 0x%X isn't a 32-bit RGB one: not shown\n", format);
        have_picture = 0;
    }
    locks = fh_d3d_render_target_locks();
    written = locks != last_locks;  /* the game wrote its picture by the CPU this frame */
    last_locks = locks;
    dumping = dump_frames && dump_path && in_list(dump_frames, frames);
    ngl_vk_set_readback(dumping);   /* the window gets its picture on the GPU; copy it back only to save it */
    if (fh_draw_frame_open()) {
        /* a frame with draws of its own: the CPU picture goes on top when the game wrote one */
        if (fh_draw_frame_end(written && have_picture ? pixels : NULL, width, height, pitch)) {
            fprintf(stderr, "[fh] Vulkan: drawing frame %lu failed; frames are not drawn from here on\n", frames);
            state = -1;
            return;
        }
    } else if (fh_text_waiting() && fh_draw_begin_frame()) {
        /* [waw-decomp-fh2 r19] 2026-11-24 a frame without draws of its own (a film) with the game's text: the film's
         * picture in the 4:3 part as ngl_vk_draw_picture puts it, the text over it at the picture's size */
        if (fh_draw_frame_end(have_picture ? pixels : NULL, width, height, pitch)) {
            fprintf(stderr, "[fh] Vulkan: drawing frame %lu failed; frames are not drawn from here on\n", frames);
            state = -1;
            fh_text_swap();
            return;
        }
    } else {
        if (!have_picture)
            return;                 /* no render target yet */
        if (ngl_vk_draw_picture(pixels, width, height, pitch)) {
            fprintf(stderr, "[fh] Vulkan: drawing frame %lu failed; frames are not drawn from here on\n", frames);
            state = -1;
            return;
        }
    }
    fh_text_swap();             /* [waw-decomp-fh2 r19] this frame's text drawn: the next frame starts empty */
    if (dumping) {
        char path[1024];

        if (strstr(dump_path, "%lu"))
            snprintf(path, sizeof path, dump_path, frames);
        else
            snprintf(path, sizeof path, "%s", dump_path);
        save_target(path);
        fh_draw_report();
    }
}
void fh_present_resolution_note(char *out, size_t size)
{
    if (!out || !size)
        return;
    out[0] = '\0';
    if (plan_now.width && plan_now.height)
        fh_resolution_describe(&plan_now, out, size);
}
#else
void fh_present_resolution_note(char *out, size_t size)
{
    if (out && size)
        out[0] = '\0';
}
static void show_render_target(void)
{
    static int said;

    if (!said++ && setting_on("FH_VK", 1))
        fprintf(stderr, "[fh] built without Vulkan (ngl_vk): frames are not drawn\n");
}
#endif

/* [acct2-fh] 2026-11-13: the game's gamma ramp (fh_present.h). On the Xbox, D3DDevice_SetGammaRamp keeps the ramp
 * in the device for the video output, which shows every frame through it; its stand-in did nothing, so the window
 * has shown the render target as it is. Finest Hour's only call (0x0012C810, once at start-up, from 0x000A2C37) makes
 * the same ramp for red, green and blue: ramp[i] = clamp(i x 1.0811 - 20.68 x b, 0, 255), truncated, b being the
 * float at 0x003EAF70 (boot31: 0->0 16->7 128->129 235->244 255->255, so b is about 0.45: a little more contrast).
 * The window shows the picture through it with acct2-ocr's ngl_vk_set_window_table (landed 17:43 for Big Red One's
 * ramp); the target, and so the saved pictures, stay what the game drew. */
static uint8_t gamma_ramp[3][256];
static int gamma_ramp_kept;
static int gamma_ramp_set;          /* [acct2-fh] the game has set one (it is shown again after a new resolution) */

#if defined(FH_HAVE_VULKAN)
/* The window shows the picture through the game's ramp (ngl_vk's table: red in the low byte, then green and blue), or
 * as drawn with FH_GAMMA=0; nothing before the game has set one. */
static void apply_window_table(void)
{
#if defined(NGL_VK_HAS_WINDOW_TABLE)
    uint32_t words[256];

    if (!gamma_ramp_set)
        return;
    if (!gamma_ramp_kept) {
        ngl_vk_set_window_table(NULL);
        return;
    }
    for (unsigned i = 0; i < 256; i++)
        words[i] = gamma_ramp[0][i] | (uint32_t)gamma_ramp[1][i] << 8 | (uint32_t)gamma_ramp[2][i] << 16 | 0xFF000000u;
    ngl_vk_set_window_table(words);
#endif
}
#endif

uint32_t fh_present_gamma_ramp(uint32_t flags, uint32_t ramp)
{
    static const unsigned points[] = {0, 16, 32, 64, 128, 192, 224, 235, 255};
    int identity = 1, same = 1;

    if (!ramp)
        return 0;
    for (unsigned c = 0; c < 3; c++)
        for (unsigned i = 0; i < 256; i++) {
            gamma_ramp[c][i] = MEM8(ramp + c * 256 + i);
            identity &= gamma_ramp[c][i] == i;
            same &= gamma_ramp[c][i] == gamma_ramp[0][i];
        }
    gamma_ramp_kept = setting_on("FH_GAMMA", 1);
    fprintf(stderr, "[fh] gamma ramp (flags 0x%X)%s%s:", flags, identity ? ", the identity" : "",
            same ? ", the same for red, green and blue" : "");
    for (unsigned c = 0; c < (same ? 1u : 3u); c++) {
        if (!same)
            fprintf(stderr, " %s", c == 0 ? "red" : c == 1 ? "green" : "blue");
        for (unsigned k = 0; k < sizeof points / sizeof points[0]; k++)
            fprintf(stderr, " %u->%u", points[k], gamma_ramp[c][points[k]]);
        fputc(c + 1 < (same ? 1u : 3u) ? ';' : '\n', stderr);
    }
    gamma_ramp_set = 1;
#if defined(FH_HAVE_VULKAN)
    apply_window_table();
#endif
    return 0;
}

const uint8_t *fh_present_gamma(void)
{
    return gamma_ramp_kept ? &gamma_ramp[0][0] : NULL;
}

uint32_t fh_present_swap(uint32_t flags)
{
    (void)flags;        /* 0 is the default swap (the front end's); the port has one way to show a frame */
    frames++;
    check_quit();
    fh_d3d_state_frame_end(frames);
    fh_watch_frame(frames);  /* [acct2-fh] FH_WATCH */
    test_window_size(frames);   /* [acct2-fh] FH_TEST_WINDOW_SIZE */
    show_render_target();
    fh_frame_pace();        /* [acct2-fh] after the frame is shown, as the Xbox's Swap queued it, then waited */
    count_frame_rate();
    fh_profile_frame();     /* [acct2-fh] FH_PROFILE: starts sampling at the first Swap, writes once a second */
    return 0;
}
