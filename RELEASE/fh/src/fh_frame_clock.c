/* [acct2-fh] 2026-11-14 Finest Hour port: the game's frame timer (0x000A44C0), read from its code and written out here
 * as a stand-in, so the time between frames can be measured finer than the console's millisecond.
 *
 * What the game does (dev/agents/acct2-fh/FRAMERATE-AUDIT.md says how it was read): once a frame, each of the game's
 * loops (the calls at 0x000A2FE0, 0x000A53D0, 0x000A5730, 0x000A5880 and 0x0011B85E) calls this timer. It reads
 * GetTickCount (0x000E34F0, a jump to XAPI's 0x001320EB, which reads the kernel's KeTickCount: whole milliseconds),
 * takes the milliseconds since the last frame's end, and turns them into the frame's step in 60ths of a second
 * (x 0.06): 1.0 for a frame of 16.7 ms. The step is limited to 5 (83 ms; a longer frame moves the game less than the
 * time that went by). Everything the game moves by time reads that step (0x0032166C, read by 41 functions) or the
 * game's time (0x003B4788, the steps added up). The timer also counts frames (0x003B4798, +1 a frame).
 *
 * The step comes from the milliseconds between frames, so above 60 frames a second it is coarse: at 240 a frame is
 * 4.17 ms and the console's clock gives 4 or 5 (a step of 0.24 or 0.30, 20% apart), at 500 it gives 2 or 3 (40%
 * apart), and from 1000 up some frames see 0 ms and move nothing at all. On the console (60 a second at most) a frame
 * was 16 or 17 ms, 6% apart. Here the time between frames is taken from the host's clock to the nanosecond, so every
 * step is the time that really went by; the game's own millisecond values are still written where it keeps them
 * (other code reads them), and its modes are kept exactly:
 *   mode (0x003B479C) 0, 6   the step from the time, x the time scale (0x00321680, 1); 6 is what the game's pause
 *                            (0x000A2B20) sets while paused, and the timer treats it as 0
 *   mode 1                   a fixed step (0x0032167C, 0.2) every frame
 *   mode 2                   the step from the time x a scale (0x00321678) while a countdown (0x00321674, in 60ths)
 *                            lasts (0x000A46E0 starts it); when it runs out 0x0034AA00 is set to 3 and the mode to 0
 *   modes 3, 4               one plain step (no time scale), then mode 0
 *   mode 5                   one step of 1 (while 0x00321684 is set), then steps of 0: the game frozen
 *   before the first frame   (both millisecond values 0) a step of 1
 *
 * The time between frames is the host's when the last frame's end value (0x003B478C) is still the one this timer
 * wrote; when the game wrote it itself (its reset 0x000A4450 at a level's start writes 0) the game's own
 * milliseconds are used for that frame, as the original.
 *
 * Setting: game.fine_frame_clock in fh.toml (FH_FINE_FRAME_CLOCK), true by default; false takes the game's whole
 * milliseconds as the console did (to compare).
 *
 * The frame count as a clock (0x001119B0, a stand-in too: fh_frame_clock_frames_as_seconds below). Some of the game's
 * code counts frames instead of time (FRAMERATE-AUDIT.md lists every reader of 0x003B4798). Most of it is a check
 * every so many frames, or a "once a frame" mark, which only runs more often at a higher frame rate. One is a clock:
 * 0x001119B0 returns the frame count x 1/60 (0x002DFB3C) as seconds, and the object at 0x003E3870 holds it as its
 * time function (0x001119D0, run at the game's start from 0x0021DF00). Its only reader, 0x00111BB0, makes a smooth
 * noise from that time (octaves of 0x00111B30, the time x a speed plus a random start); 0x001113C0 adds two of them,
 * scaled, for the camera shake of 0x00111640 (its object at 0x003E27A0; the two noises are kept at its +0x1C and
 * +0x48). At 60 frames a second the frame count / 60 is the time; at 240 it runs 4 times too fast, and so does the
 * shake. Here the clock
 * adds up the time each frame really took (the frame timer's own measure, at most a limited step's 5/60 s, as the
 * step), and starts again from the game's count whenever the game sets the count itself (its reset, 0x000A4450,
 * writes 0 at a level's start). The frames still count as before for everything else.
 *
 * The camera shake's frame time (read 2026-11-14 16:45 and 16:55). 0x00111640, once a frame from 0x00110EE8, runs the
 * camera shake of the object at 0x003E27A0: three kinds, at +0x3C, +0x10 and +0x68, each turned on by the game's
 * events through 0x00111430 (a hit: 0x000D1900's kinds 2 and 3; an explosion at a place: 0x001041F0 and 0x00105D20's
 * kind 8). While none is on, or the game is paused (0x003B46D0 & 0x10), it returns at once. The first two are timed:
 * once a frame each one's time left (+0x40, +0x14) falls by a frame's time in seconds, and its phase (+0x44, +0x18)
 * grows by that x its rate (+0x08, +0x0C); at 0 it ends (0x00111350, 0x001113B0). The third has no time of its own.
 * That frame's time is a value the function works out only once, the first frame a shake is on (a C++ local static:
 * the step x 16.667 (0x002E8D60) x 0.001 (0x002DBCA8), kept at 0x003E2854, with bit 0 of 0x003E2858 as its guard),
 * and then uses for every frame after, all the session. On the console a frame was a 60th, so it was right (unless
 * the first shake came on a slow frame); at any other frame rate the shakes go (frames a second now / frames a second
 * at the first shake) times too fast and end that much sooner: 4 times at 240 after a first shake at 60, and changing
 * with the frame rate when it isn't steady. Here, with game.frame_clocks_by_time, the timer writes that value every
 * frame and sets the guard, so the game never fixes it: the frame's real time in seconds (at most the step's limit,
 * as the frame count's clock above). That is what the console's value was at its 60 frames a second, also in the
 * game's slow motion and while its clock is stopped (modes 2, 5 and the time scale), since that value never changed;
 * the frame's step would slow the shakes there.
 *
 * Setting: game.frame_clocks_by_time (FH_FRAME_CLOCKS_BY_TIME), true by default, for both; false is the console's
 * frame count / 60 and the shake's first-frame time (to compare).
 *
 * [acct2-fh] 2026-11-22: the game's update in whole 60ths above 60 frames a second (FRAMERATE-AUDIT.md F3, F10, F11).
 * On the Xbox a frame was never shorter than a 60th, so the game's step was never under about 1. Some of its code
 * goes wrong with smaller steps: a screen fade that cuts its level to a whole number each frame (0x000DC320) ends
 * sooner, enemy fire decided in the level's scripts almost stops with no frame limit, NPCs get over lips
 * differently. With game.fixed_step_above_60 (FH_FIXED_STEP, on by default again from build68) the timer gathers frames shorter than
 * a 60th and hands the game a step only once a whole 60th has gathered:
 *   - a frame of a 60th or longer (60 frames a second and below) is exactly as before: its own step, the game's
 *     time and frame count as the original (anything still gathered from shorter frames is added to it);
 *   - a shorter frame adds its time to what is gathered. When a whole 60th is there, the frame gets a step of 1
 *     (x the time scale, as the original's step), and the rest stays gathered. Otherwise it gets a step of 0.
 * The in-play loop (0x000A2FE0) runs the game's update only when the step is above 0 (its block 7). So the update
 * runs 60 times a second, each time with the step it had at 60 frames a second, and every frame is drawn (below);
 * there is no drawing between updates yet, so moving things change 60 times a second at any frame rate above 60.
 * boot92 (11-22) found the drawing does not run on a held frame after all: the in-play loop (0x000A2FE0) tests the
 * step at 0x000A3048 and with a step of 0 skips the update and the drawing (0x000DCB90, which ends in the game's
 * Swap) both, going to 0x000A3237; the loop then comes straight round to the timer again (about 60,000 times a
 * second at 144 and with no limit), so every rate above 60 drew at 59.9. That path draws only when the game's flag
 * 0x10 at 0x003B46D0 is set (its own draw-only path: the same call of 0x000DCB90 at 0x000A3287, returning to
 * 0x000A328C). So on a frame held at 0 in the in-play loop, this timer calls the drawing itself, as that path does
 * (0x000DCB90 with 0, cdecl; the two calls of 0x0005A260 around it are a bare ret), before it returns: the frame is
 * drawn and shown, and the loop then skips the update as before. Only the in-play loop's call (it returns to
 * 0x000A2FE5) holds frames; the game's other loops (0x000A53D0, 0x000A5730, 0x000A5880, 0x0011B85E) get their own
 * step. Frames of at least 0.9 of a 60th (66.7 frames a second and below) are not gathered, so 60 frames a second
 * and below are exactly as with this off (a 60th measured at 16.66 ms is not held).
 * A frame held at 0:
 *   - doesn't count in the frame count (0x003B4798): the game's "once a frame" marks and its "last run more than 2
 *     frames ago" check (0x000C4270) count updates, as on the console;
 *   - leaves the camera shake's frame time alone; a frame that updates gets the real time since the last update;
 *   - holds the pad (fh_pad.c): the game sees the pad's state only in frames that update, with any button pressed in
 *     between still pressed, so a press can't fall between two updates.
 * Only modes 0 and 6 gather (the time from the clock); the fixed, scaled and frozen modes are as the original. With
 * the setting off, or with game.fine_frame_clock off, every frame is as before. */
#include "fh_frame_clock.h"

#include <stdint.h>
#include <stdio.h>
#include <time.h>

#include "recomp_types.h"           /* MEM8, MEM32: guest memory */
#include "fh_host.h"                /* [waw-decomp/fh2] 11-23: a steady clock */
#include "fh_settings.h"

/* The game's code and data (Europe v3). */
#define KE_TICK_COUNT_SLOT  0x002DB304u   /* the kernel import KeTickCount (ordinal 156): the count's address */
#define LAST_TICKS          0x003B478Cu   /* GetTickCount at the last frame's end (0 before the first frame) */
#define NOW_TICKS           0x003B4790u   /* GetTickCount at this frame's start */
#define ELAPSED_TICKS       0x003B4794u   /* the two apart, ms */
#define FRAME_COUNT         0x003B4798u   /* frames, +1 each */
#define MODE                0x003B479Cu   /* see the top of the file */
#define GAME_TIME           0x003B4788u   /* float: the steps added up (60ths of a second) */
#define STEP                0x0032166Cu   /* float: this frame's step, at most STEP_LIMIT */
#define STEP_UNLIMITED      0x00321670u   /* float: the step before the limit */
#define COUNTDOWN           0x00321674u   /* float: mode 2's countdown (60ths); -1 when none */
#define MODE2_SCALE         0x00321678u   /* float: mode 2's scale */
#define FIXED_STEP          0x0032167Cu   /* float: mode 1's step */
#define TIME_SCALE          0x00321680u   /* float: modes 0 and 6's scale */
#define FROZEN_FIRST        0x00321684u   /* byte: mode 5's next step is 1 */
#define COUNTDOWN_DONE      0x0034AA00u   /* int: set to 3 when mode 2's countdown runs out */
#define MS_TO_STEP          0x002DC4E8u   /* float 0.06 (the game's .rdata) */
#define STEP_LIMIT          0x002E38F0u   /* float 5 (the game's .rdata) */
#define FRAMES_TO_SECONDS   0x002DFB3Cu   /* float 1/60 (the game's .rdata), 0x001119B0's */
#define SHAKE_FRAME_SECONDS 0x003E2854u   /* float: 0x00111640's frame time in seconds (a local static) */
#define SHAKE_GUARD         0x003E2858u   /* its guard: bit 0 set once it is worked out */
#define IN_PLAY_RETURN      0x000A2FE5u   /* the in-play loop's call of this timer (at 0x000A2FE0) returns here */
#define PLAY_FLAGS          0x003B46D0u   /* the loop's flags: bit 4 (0x10) its own draw-only path on a step of 0 */
#define PLAY_DRAW_ONLY      0x10u
#define DRAW_FRAME          0x000DCB90u   /* the frame's drawing, cdecl, one argument (0 from the in-play loop) */
#define DRAW_FRAME_RETURN   0x000A328Cu   /* where the loop's draw-only path's call of it returns */
#define GATHER_BELOW        0.9           /* frames shorter than this many 60ths are gathered (above 66.7 fps) */

static int fine = -1;                       /* game.fine_frame_clock, read once */
static int by_time = -1;                    /* game.frame_clocks_by_time */
static unsigned seen_generation;
static uint32_t written_last;               /* the end value this timer last wrote to LAST_TICKS */
static int64_t written_last_ns;             /* the host's time when it wrote it */
static struct fh_frame_clock_stats stats;
static double frames_seconds;               /* the frame count's clock: the frames' real time added up */
static uint32_t frames_counted;             /* the frame count this clock last saw (to see the game set it) */
static int fixed_step = -1;                 /* game.fixed_step_above_60 */
static double gathered;                     /* 60ths gathered from frames shorter than a 60th, not yet stepped */
static double gathered_seconds;             /* their real time, for the camera shake's frame time */
static int updates_this_frame = 1;          /* 0 when this frame's step was held at 0 (fh_frame_clock_updates) */
static int drawing_held = 0;                /* 1 while this timer draws a held frame (fh_frame_clock_drawing_held) */

/* [waw-decomp/fh2] 11-23: a steady clock on both systems (QueryPerformanceCounter on Windows, where this read the
 * wall clock, which a time sync can move; fh_host.c) */
static int64_t now_ns(void)
{
    return (int64_t)fh_host_now_ns();
}

static float get_float(uint32_t va)
{
    union { uint32_t bits; float value; } u;

    u.bits = MEM32(va);
    return u.value;
}

static void set_float(uint32_t va, float value)
{
    union { uint32_t bits; float value; } u;

    u.value = value;
    MEM32(va) = u.bits;
}

/* GetTickCount as the game reads it (XAPI's 0x001320EB: the kernel's KeTickCount, through its import). */
static uint32_t game_tick_count(void)
{
    return MEM32(MEM32(KE_TICK_COUNT_SLOT));
}

static void read_setting(void)
{
    unsigned generation = fh_settings_generation();

    if (fine >= 0 && generation == seen_generation)
        return;
    seen_generation = generation;
    {
        int was = fine;

        fine = fh_setting_bool("game.fine_frame_clock", "FH_FINE_FRAME_CLOCK", 1);
        if (fine != was)
            fprintf(stderr, "[fh] the game's frame timer: %s\n", fine
                    ? "the time between frames to the microsecond (game.fine_frame_clock)"
                    : "whole milliseconds, as the console (game.fine_frame_clock off)");
    }
    {
        int was = by_time;

        by_time = fh_setting_bool("game.frame_clocks_by_time", "FH_FRAME_CLOCKS_BY_TIME", 1);
        if (by_time != was)
            fprintf(stderr, "[fh] the game's clock of frames (its noise wobble): %s\n", by_time
                    ? "the frames' real time (game.frame_clocks_by_time)"
                    : "the frame count / 60, as the console (game.frame_clocks_by_time off)");
    }
    {
        int was = fixed_step;

        fixed_step = fh_setting_bool("game.fixed_step_above_60", "FH_FIXED_STEP", 1);
        if (fixed_step != was)
            fprintf(stderr, "[fh] the game's update above 60 frames a second: %s\n", fixed_step
                    ? "in whole 60ths above 66.7 frames a second, every frame drawn (game.fixed_step_above_60)"
                    : "every frame, with the frame's own step (game.fixed_step_above_60 off)");
        if (!fixed_step) {
            gathered = gathered_seconds = 0.0;
            updates_this_frame = 1;
        }
    }
}

/* The step from the time and the limit: STEP_UNLIMITED = step, STEP = step or the limit (the original compares the
 * unrounded value with the limit; "not above the limit" keeps it). */
static void set_step(double step)
{
    double limit = get_float(STEP_LIMIT);

    set_float(STEP_UNLIMITED, (float)step);
    if (step > limit) {
        set_float(STEP, (float)limit);
        stats.limited_steps++;
    } else {
        MEM32(STEP) = MEM32(STEP_UNLIMITED);
    }
}

/* game.fixed_step_above_60 (see the top of the file): a frame shorter than a 60th adds to what is gathered and gets a
 * step only once a whole 60th is there; a frame of a 60th or more takes its own time and whatever is gathered. */
static void gather_step(double sixtieths, double seconds)
{
    if (sixtieths >= GATHER_BELOW) {        /* a long frame after short ones: all of it now, as the original would */
        set_step((sixtieths + gathered) * get_float(TIME_SCALE));
        gathered = gathered_seconds = 0.0;
        updates_this_frame = 1;
        return;
    }
    gathered += sixtieths;
    gathered_seconds += seconds;
    if (gathered >= 1.0) {
        gathered -= 1.0;
        set_step(1.0 * get_float(TIME_SCALE));
        updates_this_frame = 1;
        stats.gathered_updates++;
    } else {
        set_float(STEP, 0.0f);
        set_float(STEP_UNLIMITED, 0.0f);
        updates_this_frame = 0;
        stats.held_frames++;
    }
}

/* Calls the frame's drawing (0x000DCB90, cdecl) with 0, as the in-play loop's draw-only path does at 0x000A3287: the
 * argument, then that call's return address; its ret takes the return address, and the caller's add esp, 4 the
 * argument (here: the stack put back). Every register is put back after, so the loop goes on as if this timer had
 * only returned. 1 when it drew. */
static int draw_held_frame(void)
{
    uint32_t eax_was = g_eax, ebx_was = g_ebx, ecx_was = g_ecx, edx_was = g_edx;
    uint32_t esi_was = g_esi, edi_was = g_edi, ebp_was = g_ebp, esp_was = g_esp;
    recomp_func_t draw = recomp_lookup_manual(DRAW_FRAME);

    if (!draw)
        draw = recomp_lookup(DRAW_FRAME);
    if (!draw)
        return 0;
    PUSH32(g_esp, 0u);
    PUSH32(g_esp, DRAW_FRAME_RETURN);
    drawing_held = 1;
    draw();
    drawing_held = 0;
    g_eax = eax_was;
    g_ebx = ebx_was;
    g_ecx = ecx_was;
    g_edx = edx_was;
    g_esi = esi_was;
    g_edi = edi_was;
    g_ebp = ebp_was;
    g_esp = esp_was;
    return 1;
}

uint32_t fh_frame_clock_step(void)
{
    uint32_t last = MEM32(LAST_TICKS), now_ticks, ticks, end;
    int32_t mode = (int32_t)MEM32(MODE);
    double elapsed_ms = 1000.0 / 60.0;     /* before the first frame: as a step of 1 */
    double frame_seconds;
    int64_t now;
    const int in_play = MEM32(g_esp) == IN_PLAY_RETURN;    /* [esp]: where this call returns */

    read_setting();
    updates_this_frame = 1;
    if (last == 0 && MEM32(NOW_TICKS) == 0) {
        /* before the first frame */
        set_float(STEP, 1.0f);
        set_float(STEP_UNLIMITED, 1.0f);
        now = now_ns();
    } else {
        now_ticks = game_tick_count();
        now = now_ns();
        MEM32(NOW_TICKS) = now_ticks;
        ticks = now_ticks - last;
        MEM32(ELAPSED_TICKS) = ticks;
        /* the original takes the difference as an unsigned 32-bit number */
        if (fine && written_last_ns && last == written_last)
            elapsed_ms = (double)(now - written_last_ns) / 1e6;
        else
            elapsed_ms = (double)ticks;
        if (mode == 1) {
            MEM32(STEP_UNLIMITED) = MEM32(FIXED_STEP);
            MEM32(STEP) = MEM32(FIXED_STEP);
        } else if (mode == 2) {
            set_step(elapsed_ms * get_float(MODE2_SCALE) * get_float(MS_TO_STEP));
        } else if (mode == 0 || mode == 6) {
            double sixtieths = elapsed_ms * get_float(MS_TO_STEP);     /* the frame's time in 60ths, unscaled */

            if (fixed_step && fine && in_play && (sixtieths < GATHER_BELOW || gathered > 0.0)) {
                gather_step(sixtieths, elapsed_ms / 1000.0);
            } else {
                if (gathered > 0.0) {       /* another loop's frame after held ones: what was gathered, now */
                    sixtieths += gathered;
                    gathered = gathered_seconds = 0.0;
                }
                set_step(sixtieths * get_float(TIME_SCALE));
            }
        } else if (mode == 3 || mode == 4) {
            mode = 0;
            MEM32(MODE) = 0;
            set_step(elapsed_ms * get_float(MS_TO_STEP));
        } else if (mode == 5) {
            if (MEM8(FROZEN_FIRST)) {
                set_float(STEP, 1.0f);
                set_float(STEP_UNLIMITED, 1.0f);
                MEM8(FROZEN_FIRST) = 0;
            } else {
                set_float(STEP, 0.0f);
                set_float(STEP_UNLIMITED, 0.0f);
            }
        }
        /* any other mode leaves both steps as they were, as the original */
    }

    /* mode 2's countdown (and mode 3's end when there is none), as the original's tail */
    if (get_float(COUNTDOWN) > 0.0f) {
        if (mode >= 0 && (mode <= 3 || mode == 5)) {
            float left = get_float(COUNTDOWN) - get_float(STEP);

            set_float(COUNTDOWN, left);
            if (left <= 0.0f) {     /* run out (a value that isn't a number leaves it, as the original) */
                set_float(COUNTDOWN, -1.0f);
                if (mode == 2)
                    MEM32(COUNTDOWN_DONE) = 3;
                MEM32(MODE) = 0;
            }
        }
    } else if (mode == 3) {
        MEM32(MODE) = 0;
    }

    set_float(GAME_TIME, get_float(GAME_TIME) + get_float(STEP));

    /* this frame's real time in seconds, at most the step's limit (so a long stall moves the two clocks below no
     * more than the game) */
    {
        double seconds = elapsed_ms / 1000.0, most = get_float(STEP_LIMIT) / 60.0;

        frame_seconds = seconds < 0.0 ? 0.0 : seconds > most ? most : seconds;
    }

    /* the frame count's clock: from the game's count when the game set it, then this frame's time */
    if (MEM32(FRAME_COUNT) != frames_counted)
        frames_seconds = (double)MEM32(FRAME_COUNT) / 60.0;
    frames_seconds += frame_seconds;

    /* the camera shake's frame time: this frame's time (see the top of the file); with the fixed step, a frame that
     * updates after held ones gets the real time since the last update (the shake runs once per frame count) */
    if (!updates_this_frame) {
        frames_counted = MEM32(FRAME_COUNT);    /* a held frame: no new frame for the game */
    } else {
        if (by_time) {
            double seconds = frame_seconds;

            if (gathered_seconds > 0.0) {
                double most = get_float(STEP_LIMIT) / 60.0;

                seconds = gathered_seconds < most ? gathered_seconds : most;
                gathered_seconds = 0.0;
            }
            set_float(SHAKE_FRAME_SECONDS, (float)seconds);
            MEM32(SHAKE_GUARD) = MEM32(SHAKE_GUARD) | 1u;
        }
        MEM32(FRAME_COUNT) = MEM32(FRAME_COUNT) + 1;
        frames_counted = MEM32(FRAME_COUNT);
    }
    end = game_tick_count();
    MEM32(LAST_TICKS) = end;
    written_last = end;
    written_last_ns = now_ns();

    /* for the frame-rate log */
    stats.frames++;
    if (!updates_this_frame) {
        /* held for the fixed step: counted as held, not in the step's spread */
    } else if (mode == 0 || mode == 6) {
        double step = get_float(STEP);

        if (step <= 0.0)
            stats.zero_steps++;
        if (stats.frames - stats.other_modes - stats.held_frames == 1 || step < stats.step_min)
            stats.step_min = step;
        if (stats.frames - stats.other_modes - stats.held_frames == 1 || step > stats.step_max)
            stats.step_max = step;
        stats.step_sum += step;
        stats.step_sum2 += step * step;
    } else {
        stats.other_modes++;
    }
    /* a frame held at 0 in the in-play loop: drawn and shown here, as the loop's own draw-only path draws */
    if (!updates_this_frame && in_play && !(MEM32(PLAY_FLAGS) & PLAY_DRAW_ONLY) && draw_held_frame())
        stats.held_drawn++;
    return end;     /* the original leaves eax as its last GetTickCount */
}

uint32_t fh_frame_clock_frames_as_seconds(void)
{
    uint32_t count = MEM32(FRAME_COUNT);
    double seconds;

    read_setting();
    if (by_time && count == frames_counted && frames_counted != 0)
        seconds = frames_seconds;
    else    /* as the original: the count (unsigned, as its fild and the 2^32 it adds below 0) x 1/60 */
        seconds = (double)count * (double)get_float(FRAMES_TO_SECONDS);
    /* the original's fild/fmul leave the value on the x87 stack (the recompiled code's g_fp_stack) */
    g_fp_top = (g_fp_top + 7) & 7;
    g_fp_stack[g_fp_top] = seconds;
    return count;   /* the original leaves eax as the frame count */
}

void fh_frame_clock_take_stats(struct fh_frame_clock_stats *out)
{
    *out = stats;
    stats = (struct fh_frame_clock_stats){0};
}

int fh_frame_clock_updates(void)
{
    return updates_this_frame;
}

int fh_frame_clock_drawing_held(void)
{
    return drawing_held;
}

double fh_frame_clock_game_time(void)
{
    return get_float(GAME_TIME);
}

uint32_t fh_frame_clock_frame_count(void)
{
    return MEM32(FRAME_COUNT);
}
