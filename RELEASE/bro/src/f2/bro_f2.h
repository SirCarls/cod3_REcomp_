/* [f2-menus] Big Red One's F2 settings window, for the port's C code (bro_f2.cpp is C++).
 * dev/agents/f2-menus/bro/BRO-F2.md says where each call goes. Every call on the game's thread, which also draws. */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

struct SDL_Window;

/* Once ngl_vk and its window are up (ngl_present_frame, after ngl_vk_init): Dear ImGui with its SDL3 and Vulkan
 * backends on ngl_vk's device, and the overlay hook (ngl_vk_set_overlay). 0, or -1 when it can't start. */
int bro_f2_init(struct SDL_Window *window);

/* Each frame in check_quit(), after SDL_PumpEvents and before the port drops the queue's events: F2 and Back + Start
 * open and close the window; while it is open the key, text, mouse and wheel events go to it. The window's next
 * picture is made here and drawn over the game's when ngl_vk ends the frame. */
void bro_f2_events(void);

/* 1 while the game may have the keyboard, mouse and pads; 0 while the window is open (bro_pad.c then reports a pad
 * at rest and lets every bound action go). */
int bro_f2_game_gets_input(void);

/* [f2-menus] 2026-11-11 The mouse's motion bro_pad.c has just read for the view (raw mouse), changed in place: what
 * the in-game acceleration and the sensitivity randomizer (the F2 window's CONTROLS page) add to it. They are worked
 * out in bro_f2_events from each mouse event's own timing, so the frame rate has no part in it. Nothing is changed
 * while both are off. */
void bro_f2_mouse(float *dx, float *dy);

/* At the end: saves what changed. */
void bro_f2_shutdown(void);

#ifdef __cplusplus
}
#endif
