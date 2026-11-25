/* [acct2-fh] 2026-11-13 Finest Hour port: the picture's shape, Hor+ (owner 20:38: "start working on the resolution
 * options. Copy everything cod3 has. Please don't limit options based on cod3 tho."; dev/agents/acct2-fh/RESOLUTION.md,
 * stage 3).
 *
 * The Xbox game draws a 4:3 picture and has no widescreen of its own (it asks CreateDevice for 640 x 480 with the
 * 10:11 pixel flag even when the console reports a widescreen TV). Another shape is drawn as Call of Duty 3's port
 * draws one: the view up and down stays the game's, and the sides show more of the world (a wider picture) or less
 * (a narrower one).
 *
 * The view's width comes from RenderWare's camera: its view window (x, y) at +0x68/+0x6C, the half-width and
 * half-height of the view one unit in front of the camera. The game sets it through 0x001C6A20 (camera, &window),
 * which copies the window, stores 1/x and 1/y at +0x70/+0x74 (the projection's first two terms, 0x001CEEE0 builds the
 * matrix from them) and calls 0x001BF690 with the camera's frame (+0x04) to mark it for its next update, when
 * RenderWare builds the camera's frustum, and so its culling, from the same window. That function is a stand-in now
 * (fh/config/stand_in_functions.tsv): it does the same with x times (shape / (4/3)) for a window of the screen's own
 * shape, so the projection and the culling are both the wider (or narrower) view. Windows of another shape (a camera
 * drawing into a texture) are kept as the game sets them.
 *
 * A window the game passes back as it got it from the camera (its x the widened one) is not widened twice. A change
 * of shape while playing reaches each camera already set at the next Swap (fh_aspect_follow), on the game's thread,
 * as long as the camera still holds the window this file gave it.
 *
 * The 2D (the HUD, the menus, the films) is kept 4:3 in the middle of the picture by fh_draw.c (set_viewport).
 *
 * FH_ASPECT=0: windows go through unchanged whatever the shape (for comparison runs). */
#include "fh_aspect.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "recomp_types.h"   /* MEM32, MEMF, PUSH32, g_esp: guest memory and the guest stack */

void sub_001BF690(void);    /* the game's (lifted): marks a frame, and what is attached to it, for its next update */

#define CAMERA_FRAME            0x04u
#define CAMERA_WINDOW_X         0x68u
#define CAMERA_WINDOW_Y         0x6Cu
#define CAMERA_RECIPROCAL_X     0x70u
#define CAMERA_RECIPROCAL_Y     0x74u
#define FRAME_UPDATE_RETURN     0x001C6A60u     /* where the game's own call to 0x001BF690 returns to */

/* A window of the screen's shape: x / y within this of 4:3 (the game's windows for its screen). */
#define SCREEN_SHAPE_TOLERANCE  0.02f
#define MAX_CAMERAS             32
#define CAMERAS_LOGGED          8

static struct {
    uint32_t camera;
    float x, y;         /* the window as the game set it */
    float widened;      /* the x this file gave the camera */
} cameras[MAX_CAMERAS];
static unsigned camera_count, cameras_logged;
static float shape = FH_ASPECT_GAME, shape_applied = FH_ASPECT_GAME;

static int widening_on(void)
{
    static int on = -1;

    if (on < 0) {
        const char *v = getenv("FH_ASPECT");

        on = !(v && !strcmp(v, "0"));
    }
    return on;
}

static int screen_shaped(float x, float y)
{
    return x > 0.0f && y > 0.0f && fabsf(x / y / FH_ASPECT_GAME - 1.0f) < SCREEN_SHAPE_TOLERANCE;
}

/* Writes the window (x, y) and its reciprocals into the camera, then marks its frame, as the game's code does. */
static void write_window(uint32_t camera, uint32_t x_bits, uint32_t y_bits)
{
    uint32_t frame;

    MEM32(camera + CAMERA_WINDOW_X) = x_bits;
    MEM32(camera + CAMERA_WINDOW_Y) = y_bits;
    MEMF(camera + CAMERA_RECIPROCAL_X) = 1.0f / MEMF(camera + CAMERA_WINDOW_X);
    MEMF(camera + CAMERA_RECIPROCAL_Y) = 1.0f / MEMF(camera + CAMERA_WINDOW_Y);
    frame = MEM32(camera + CAMERA_FRAME);
    if (frame) {    /* call 0x001BF690(frame), cdecl, as the recompiled code calls it */
        PUSH32(g_esp, frame);
        PUSH32(g_esp, FRAME_UPDATE_RETURN);
        sub_001BF690();     /* its ret pops the return address */
        g_esp += 4;
    }
}

static uint32_t float_bits(float f)
{
    uint32_t bits;

    memcpy(&bits, &f, sizeof bits);
    return bits;
}

static int find_camera(uint32_t camera)
{
    for (unsigned i = 0; i < camera_count; i++)
        if (cameras[i].camera == camera)
            return (int)i;
    return -1;
}

uint32_t fh_aspect_set_view_window(uint32_t camera, uint32_t window)
{
    uint32_t x_bits = MEM32(window), y_bits = MEM32(window + 4);
    float x = MEMF(window), y = MEMF(window + 4), wide = x;
    int i = find_camera(camera);

    /* the window the camera holds from this file, handed back (read from the camera, then set again): as it is */
    if (i >= 0 && x == cameras[i].widened && y == cameras[i].y) {
        write_window(camera, x_bits, y_bits);
        return camera;
    }
    if (screen_shaped(x, y)) {
        if (widening_on() && shape != FH_ASPECT_GAME)
            wide = x * shape / FH_ASPECT_GAME;
        /* kept even at the game's shape, so a later change of shape reaches this camera (fh_aspect_follow) */
        if (i < 0 && camera_count < MAX_CAMERAS) {
            i = (int)camera_count++;
            cameras[i].camera = camera;
        }
        if (i >= 0)
            cameras[i].x = x, cameras[i].y = y, cameras[i].widened = wide;
    }
    if (wide == x) {
        write_window(camera, x_bits, y_bits);
        return camera;
    }
    if (cameras_logged < CAMERAS_LOGGED) {
        cameras_logged++;
        fprintf(stderr, "[aspect] camera 0x%08X: view window %.4f x %.4f (%.3f:1), x made %.4f for %.3f:1\n", camera,
                x, y, x / y, wide, shape);
    }
    write_window(camera, float_bits(wide), y_bits);
    return camera;
}

void fh_aspect_set_shape(float new_shape)
{
    if (!(new_shape > 0.0f))
        new_shape = FH_ASPECT_GAME;
    if (fabsf(new_shape / FH_ASPECT_GAME - 1.0f) < 0.001f)
        new_shape = FH_ASPECT_GAME;
    if (new_shape != shape)
        fprintf(stderr, "[aspect] the 3D view's shape: %.3f:1 (the game's 4:3 %s)\n", new_shape,
                new_shape > FH_ASPECT_GAME ? "widened" : new_shape < FH_ASPECT_GAME ? "narrowed" : "as it is");
    shape = new_shape;
}

float fh_aspect_shape(void)
{
    return widening_on() ? shape : FH_ASPECT_GAME;
}

void fh_aspect_follow(void)
{
    unsigned changed = 0;

    if (shape == shape_applied)
        return;
    shape_applied = shape;
    if (!widening_on())
        return;
    for (unsigned i = 0; i < camera_count; i++) {
        uint32_t camera = cameras[i].camera;
        float wide = shape == FH_ASPECT_GAME ? cameras[i].x : cameras[i].x * shape / FH_ASPECT_GAME;

        /* only a camera that still holds this file's window (one freed and its memory used again won't) */
        if (MEMF(camera + CAMERA_WINDOW_X) != cameras[i].widened || MEMF(camera + CAMERA_WINDOW_Y) != cameras[i].y)
            continue;
        cameras[i].widened = wide;
        write_window(camera, float_bits(wide), float_bits(cameras[i].y));
        changed++;
    }
    fprintf(stderr, "[aspect] %u camera(s) given the %.3f:1 view\n", changed, shape);
}
