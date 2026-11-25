/* [acct2-fh] 2026-11-14 Finest Hour's F2 settings window, for the port's C code (fh_f2.cpp is C++).
 *
 * Call of Duty 3's F2 window, as Big Red One has it (bro/src/f2/bro_f2.h): the same look and rows
 * (COD_3_WAW_Setup-tool/recomp/src/f2/), drawn with Dear ImGui over ngl_vk's picture at the end of each frame
 * (ngl_vk_set_overlay). Its pages: VIDEO (the display and the resolution, dev/agents/acct2-fh/RESOLUTION.md stage 4)
 * and INTERFACE (the window's own look). What it changes goes through fh_settings_set, so fh_present.c applies it
 * while the game runs, and Save (or closing the window) writes it into fh.toml.
 *
 * Every call on the game's thread, the one that draws (fh_present.c). Without the window in the build (no C++
 * compiler, no Dear ImGui: fh/CMakeLists.txt) the calls below do nothing. */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

struct SDL_Window;

#if defined(FH_HAVE_F2)

/* Once ngl_vk and its window are up (after the first make_target): Dear ImGui with its SDL3 and Vulkan backends on
 * ngl_vk's device, and the overlay hook. 0, or -1 when it can't start (then F2 does nothing). FH_F2=0: not started. */
int fh_f2_init(struct SDL_Window *window);

/* Each frame, after SDL's events are pumped and before fh_present.c drops them: F2, or Back + Start held a second on
 * a controller, opens and closes the window; while it is open the keys, the text, the mouse's buttons and wheel go
 * to it. The window's next picture is made here and drawn when ngl_vk ends this frame. */
void fh_f2_events(void);

/* 1 while the game may have the keyboard and the controller; 0 while the window is open, and after it closed until
 * the keys that were down are let go (fh_pad.c then reports a pad at rest). */
int fh_f2_game_gets_input(void);

/* Around a new render resolution (fh_present.c's remake_target: ngl_vk closed and opened again): what the window
 * made on ngl_vk's device goes before ngl_vk_shutdown and is made again on the new one after. */
void fh_f2_vulkan_stop(void);
void fh_f2_vulkan_start(void);

/* At the end: saves what changed. */
void fh_f2_shutdown(void);

#else

static inline int fh_f2_init(struct SDL_Window *window)
{
    (void)window;
    return -1;
}
static inline void fh_f2_events(void) {}
static inline int fh_f2_game_gets_input(void) { return 1; }
static inline void fh_f2_vulkan_stop(void) {}
static inline void fh_f2_vulkan_start(void) {}
static inline void fh_f2_shutdown(void) {}

#endif

#ifdef __cplusplus
}
#endif
