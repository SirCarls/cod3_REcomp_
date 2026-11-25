/* [waw-decomp/fh13] 2026-11-24: the screen fade lasts the same time at every frame rate (FRAMERATE-AUDIT.md F10).
 *
 * What the game does (Europe v3 default.xbe; read from our recompiled C, gen3/recomp_0015.c, and the XBE's floats):
 *   0x000DC320, called once a frame from the drawing (0x000DCB90 at 0x000DCE09), fades a whole-number level
 *   (0x003CBE1C, 0-255; its low byte goes to 0x000DC300, which draws it) over a time left in 60ths (0x003CC104).
 *   0x000DC420 and 0x000DC740 start a fade (the level, the time, and the time left under which the level starts to
 *   fall, 0x003CC11C). Each call, with s the frame's step (0x0032166C):
 *     - time left above 300.0 (0x002DBD54): set to 300; exactly 0: the level is 0 (no fade) and the sum is skipped;
 *     - time left under 0x003CC11C: level = whole part of (level - level / time left x s) (0x001E25B5, 0x001E1364);
 *     - time left -= s; at 0 or below, the time left and the level are 0.
 *   The cut to a whole number takes up to one more off the level each call, so the more calls a second the sooner
 *   the fade ends. From full (255, 300) the console's 60 calls a second end it after 4.25 s; 20 and 30 frames a
 *   second end it after 5.0 s, and above 60 it ended sooner still (2.1 s at 120) until game.fixed_step_above_60 gave
 *   those rates whole 60ths.
 *
 * What this does: the port's sub_000DC320 (fh/config/wrapped_functions.tsv; tools/fh_decomp/fh_wrap.py renames the
 * recompiled one fh_lifted_000DC320) gathers the steps and runs the game's own sum once per whole 60th gathered, as
 * the console did at 60: a frame of s = 3 (20 frames a second) runs it three times. All but the last run of a frame
 * are done here (the same sum on the same variables, read above; they draw nothing); the last is the game's own call
 * with a step of exactly 1, which also draws the level. A frame with less than a whole 60th gathered calls the
 * game's own with a step of 0, which draws the level unchanged. The step is put back after the call.
 * Worked through from full: 4.25 s at 20, 44, 60 and 61 frames a second, 4.27 s at 30 (a frame's rounding), against
 * the original's 5.0 / 4.98 / 4.25 / 4.18. A step of 0 (a frame held by game.fixed_step_above_60, or the game
 * stopped) calls the game's own unchanged. game.fade_by_60ths = false (FH_FADE_BY_60THS=0) is the game as it was.
 *
 * Ours, from the project's any-frame-rate rule; FinestHourRemastered does not do this. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "recomp_types.h"           /* MEM32: guest memory */
#include "fh_settings.h"

#define STEP            0x0032166Cu     /* float: the frame's step in 60ths */
#define FADE_LEVEL      0x003CBE1Cu     /* int: 0-255 */
#define FADE_TIME_LEFT  0x003CC104u     /* float: in 60ths */
#define FADE_FALL_FROM  0x003CC11Cu     /* float: the level falls only while the time left is under this */
#define FADE_TIME_MAX   300.0f          /* 0x002DBD54 */
#define MOST_RUNS       8               /* the step is at most 5 (the game's limit): a guard only */

void fh_lifted_000DC320(void);
void sub_000DC320(void);

static float get_float(uint32_t address)
{
    uint32_t bits = MEM32(address);
    float value;
    memcpy(&value, &bits, sizeof value);
    return value;
}

static void set_float(uint32_t address, float value)
{
    uint32_t bits;
    memcpy(&bits, &value, sizeof bits);
    MEM32(address) = bits;
}

static int fade_on(void)
{
    static int v = -1;
    if (v < 0) {
        v = fh_setting_bool("game.fade_by_60ths", "FH_FADE_BY_60THS", 1);
        fprintf(stderr, "[fh] fade: the screen fade (0x000DC320) %s\n",
                v ? "runs once per whole 60th of game time, as at 60 frames a second (game.fade_by_60ths)"
                  : "runs once a frame with the frame's step, as the game (game.fade_by_60ths off)");
    }
    return v;
}

/* One run of the game's sum with a step of 1, without the drawing (the part of 0x000DC320 before 0x000DC3CE). */
static void fade_one_60th(void)
{
    float left = get_float(FADE_TIME_LEFT);

    if (left > FADE_TIME_MAX) {
        left = FADE_TIME_MAX;
        set_float(FADE_TIME_LEFT, left);
    } else if (left == 0.0f) {
        MEM32(FADE_LEVEL) = 0;
        return;
    }
    if (!(left >= get_float(FADE_FALL_FROM))) {
        double level = (double)(int32_t)MEM32(FADE_LEVEL);
        MEM32(FADE_LEVEL) = (uint32_t)(int32_t)(level - level / (double)left * 1.0);
    }
    left = left - 1.0f;
    set_float(FADE_TIME_LEFT, left);
    if (left <= 0.0f) {
        set_float(FADE_TIME_LEFT, 0.0f);
        MEM32(FADE_LEVEL) = 0;
    }
}

void sub_000DC320(void)
{
    static double gathered;         /* 60ths of game time not yet run */
    static unsigned long frames_run_more;
    const float step = get_float(STEP);

    if (!(step > 0.0f) || !fade_on()) {
        fh_lifted_000DC320();
        return;
    }
    gathered += step;
    int runs = (int)gathered;
    gathered -= runs;
    if (runs > MOST_RUNS) {
        runs = MOST_RUNS;
        gathered = 0.0;
    }
    for (int i = 1; i < runs; i++)
        fade_one_60th();
    if (runs > 1 && MEM32(FADE_LEVEL) && ++frames_run_more == 1)
        fprintf(stderr, "[fh] fade: first frame with a fade run %d times (step %.3f, level %u)\n", runs, step,
                (unsigned)MEM32(FADE_LEVEL));

    const uint32_t step_bits = MEM32(STEP);
    set_float(STEP, runs ? 1.0f : 0.0f);
    fh_lifted_000DC320();
    MEM32(STEP) = step_bits;
}
