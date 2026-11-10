/* [acct2-bro-game] Big Red One port: the frame-rate counter drawn over the picture (display.show_fps). See
 * ngl_fps_counter.c. */
#pragma once

#include <stdint.h>

struct ngl_vk_vertex;

/* The numbers to show, set once a second by the frame clock: frames a second and the average frame's time. */
void ngl_fps_counter_set(double frames_per_second, double frame_ms);

/* 1 when the counter is on (display.show_fps, BRO_SHOW_FPS). */
int ngl_fps_counter_on(void);

/* Writes the counter's triangles (six vertices a quad, the game's 640 x 480 screen) into out, at most room vertices;
 * returns how many. Makes its small texture the first time (ngl_vk must be started). */
uint32_t ngl_fps_counter_vertices(struct ngl_vk_vertex *out, uint32_t room);

/* The window title's text with the numbers, when they changed since the last call (returns 1), else 0. */
int ngl_fps_counter_title(char *out, unsigned size);
