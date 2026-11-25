/* [acct2-fh] 2026-11-13 Finest Hour port: what size the game is drawn at (owner 20:38: "start working on the resolution
 * options. Copy everything cod3 has. Please don't limit options based on cod3 tho."). See fh_resolution.c and
 * dev/agents/acct2-fh/RESOLUTION.md.
 *
 * The game draws a 640 x 480 back buffer; fh_draw.c redraws every draw on Vulkan into ngl_vk's picture (the target) at
 * any size, so the target is the render resolution. This file says what that size is, from the settings and the
 * window: the plan. fh_present.c makes the target that size, at the start and, when the plan changes, between two
 * frames. */
#pragma once
#include <stddef.h>
#include <stdint.h>

/* The game's own screen (its CreateDevice). */
#define FH_GAME_WIDTH       640u
#define FH_GAME_HEIGHT      480u

/* The smallest and largest target sizes: Vulkan guarantees 4096 a side and most PCs have 16384; a size the GPU can't
 * make falls back (fh_present.c). */
#define FH_MIN_SIZE         16u
#define FH_MIN_CUSTOM_WIDTH 64u
#define FH_MIN_CUSTOM_HEIGHT 48u
#define FH_MAX_SIZE         16384u
#define FH_MIN_PERCENT      10u
#define FH_MAX_PERCENT      400u
#define FH_MIN_SHAPE        0.25f   /* [acct2-fh] the picture's shape: from 1:4 ... */
#define FH_MAX_SHAPE        8.0f    /* ... to 8:1 (CoD3 keeps 4:3 to 48:9) */

/* How the picture meets the window (display.fit). */
enum fh_fit {
    FH_FIT_BARS,        /* the picture keeps its shape, with black bars where the window has another */
    FH_FIT_STRETCH,     /* the picture fills the window */
};

struct fh_render_plan {
    char choice[40];            /* display.render_resolution as read: "auto", "1080p", "2560x1080", "original" */
    uint32_t view_width;        /* where it is shown: the window's drawable size, or the screen's */
    uint32_t view_height;
    uint32_t size_width;        /* the choice's size, before the shape and the scale */
    uint32_t size_height;
    uint32_t percent;           /* display.render_percent (100 for "original") */
    enum fh_fit fit;
    float shape;                /* the target's shape, width / height */
    float picture_shape;        /* the picture's (display.aspect_ratio): the 3D view's shape, Hor+ (fh_aspect.c) */
    uint32_t width;             /* the target: what the game is drawn at */
    uint32_t height;
    char note[200];             /* what was changed from the settings and why ("" when nothing) */
};

/* The plan for a window or screen of view_width x view_height pixels (0 x 0: none, as offscreen; then 1280 x 960 is
 * taken as the view), from display.render_resolution, display.render_percent, display.fit and the picture's shape. */
void fh_resolution_plan(uint32_t view_width, uint32_t view_height, struct fh_render_plan *plan);

/* 1 when two plans make the same target (the same size). */
int fh_resolution_same_target(const struct fh_render_plan *a, const struct fh_render_plan *b);

/* What the plan draws, in plain words, for the log: "auto -> 1440x1080 (100%), shown in 1920x1080 with bars". */
void fh_resolution_describe(const struct fh_render_plan *plan, char *out, size_t size);

/* display.live_resolution (FH_LIVE_RESOLUTION): 1 when a new plan is applied while the game runs. */
int fh_resolution_live(void);

/* display.fit (FH_FIT). */
enum fh_fit fh_resolution_fit(void);

/* The picture's shape (width / height) from display.aspect_ratio (FH_ASPECT_RATIO): "auto" (the view's shape, also
 * when not set: [acct2-fh] 11-13 build31, as CoD3's), "game" or "4:3" (the game's 4:3), "resolution" (the render
 * size's: size_width x size_height), "W:H", "WxH", "W/H" or a number, kept within FH_MIN_SHAPE and FH_MAX_SHAPE. */
float fh_resolution_picture_shape(uint32_t view_width, uint32_t view_height, uint32_t size_width,
                                  uint32_t size_height);

/* A shape as written ("21:9", "2560x1080", "16/10", "2.39"); 0 when the text isn't one. */
int fh_resolution_parse_shape(const char *text, float *shape);

/* "WxH" as two numbers within the limits given; 0 when the text isn't one. Takes x, X or * between them. */
int fh_resolution_parse_size(const char *text, uint32_t min_width, uint32_t min_height, uint32_t max_side,
                             uint32_t *width, uint32_t *height);
