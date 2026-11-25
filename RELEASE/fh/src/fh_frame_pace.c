/* [acct2-fh] 2026-11-13 Finest Hour port: each frame held to the presentation the game asked for.
 *
 * On the Xbox the game's frames could not come faster than the television: CreateDevice asks for a refresh rate
 * (FullScreen_RefreshRateInHz: 60, or 50 for PAL without PAL-60) and a presentation interval
 * (FullScreen_PresentationInterval: how many vertical blanks each Swap waits for), and Swap's frame is shown at the
 * vertical blank it waits for. Our Swap (fh_present.c) shows its frame at once, and nothing else waited, so the front
 * end ran as fast as the display let it: 240 frames a second on a 240 Hz display (waw-fhw-boot1, acct2-fh-boot1).
 * Finest Hour's game speed follows its frame rate (dev/agents/acct2-fh/SOURCES-REVIEW.md 2.2; the owner saw NPCs
 * get over lips differently at higher frame rates, 2026-11-10 16:55), so until the game's own timestep is found and
 * made independent of the frame rate, each frame is held to the console's pace here.
 *
 * The pace: one frame every interval / refresh_hz seconds, counted on the host's monotonic clock from the last
 * frame's due time, so the frame rate keeps its average however the waits round. A frame that comes late is not
 * caught up with a burst. The interval, as Direct3D 8 numbers it: 0 (DEFAULT) and 1 (ONE) every blank, 2 (TWO) every
 * other one, 4 (THREE), 8 (FOUR); 0x80000000 (IMMEDIATE) alone: no wait. A late frame (one that comes after its due
 * time) is shown at the next vertical blank, as the console's was, and the next one is due the interval after that.
 * The Xbox's Direct3D also takes the IMMEDIATE bit together with an interval: 0x80000001 is what Finest Hour asks for
 * (acct2-fh-boot3, 2026-11-13: "presentation interval 0x80000001"). Read here as its bits say, IMMEDIATE | ONE: every
 * blank, and a late frame shown at once instead of at the next blank (the next is then due a whole frame after it).
 * (The name ONE_OR_IMMEDIATE in the notes is that reading of the bits; no document read here spells the value out.)
 * Any other value is printed and taken as every blank. What the game asked for is printed once.
 *
 * The wait lets the game's other threads run, as the Xbox's did: with RECOMP_GUEST_LOCK=1 (one guest thread runs
 * guest code at a time, as on the console's one CPU) the guest CPU is let go for the wait, the way xboxrecomp's
 * kernel calls let it go (as KeDelayExecutionThread, ordinal 99, would: a call that may block), and taken back after.
 * The host sleeps all but the last SPIN_NS and spins the rest, so the frames keep their time to a few microseconds.
 *
 * [acct2-fh] 11-14 (the frame-rate audit, dev/agents/acct2-fh/FRAMERATE-AUDIT.md): the game's step is taken from the
 * time between frames (its frame timer 0x000A44C0, now fh_frame_clock.c), so the frame rate can be set:
 *   display.max_fps (FH_MAX_FPS)  not set or "console": the game's own presentation, as above (60 a second); 0: no
 *                                 limit (as FH_FRAME_PACING=0); 1-1000: at most that many frames a second, paced
 *                                 by the port (a frame starts no sooner than 1/max_fps after the one before; a
 *                                 late frame isn't caught up). Read again when the settings change while playing
 *                                 (the F2 window).
 *   display.fps_log (FH_FPS_LOG)  every that many seconds (0, the default: never) a line:
 *     [fps] 12 s: 143.2 fps, frame 6.98 ms (worst 9.10), wait 3.55 ms; the game's step 2.387 (min 2.301, max 2.466,
 *           sd 0.021; 0 of 143 at 0, 0 at the limit, 0 in other modes), game clock 1.000, frames counted 143.2 a second
 *   The step is the game's (in 60ths of a second: 1.000 at 60 frames a second), over the frames whose step comes from
 *   the time; the game clock is how fast the game's time (0x003B4788) went against real time (1.000 when the game
 *   runs at the right speed, whatever the frame rate: the frame-rate audit's first check); frames counted is the
 *   game's own frame count (0x003B4798), which some of its code uses as a clock.
 *
 * Setting (environment):
 *   FH_FRAME_PACING=0   no wait: frames come as fast as the game and the display give them (test runs that should go
 *                       as fast as they can); the same as display.max_fps = 0. */
#include "fh_frame_pace.h"

#include <errno.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "kernel.h"         /* xbox_GuestCpuRelease / xbox_GuestCpuAcquire; on Windows also Sleep */
#include "fh_d3d.h"         /* fh_d3d_presentation */
#include "fh_d3d_state.h"   /* [acct2-fh] 11-22: the game's clears, for the log */
#include "fh_frame_clock.h" /* [acct2-fh] 11-14: the game's step, for the log */
#include "fh_host.h"        /* [waw-decomp/fh2] 11-23: a steady clock */
#include "fh_settings.h"    /* [acct2-fh] 11-14: display.max_fps, display.fps_log */

#define NS_PER_SECOND               1000000000ll
#define SPIN_NS                     250000ll        /* the wait spins its last quarter millisecond */
#define ORDINAL_KE_DELAY_EXECUTION  99u             /* the kernel call the wait stands for, for the guest CPU */
#define INTERVAL_IMMEDIATE          0x80000000u
#define MAX_FPS_LIMIT               1000            /* the highest display.max_fps */

static int pacing = -1;         /* 1: the game's presentation; 0: none or the port's limit; -1: not read yet */
static unsigned seen_generation;
static int64_t frame_ns;        /* the shortest frame; 0: no wait */
static int64_t blank_ns;        /* one vertical blank */
static int late_at_once;        /* the IMMEDIATE bit with an interval: a late frame is shown at once */
static int64_t due;             /* when the next frame may end */
static int64_t limit_ns;        /* the port's limit (display.max_fps): the shortest frame; 0: none */
static int64_t next_start;      /* the soonest the next frame may start under the limit */

/* The frame-rate log's numbers, added up until they are shown (display.fps_log). */
static struct {
    int64_t log_every;          /* ns; 0: no log */
    int64_t started;            /* the first frame's end */
    int64_t since;              /* when the numbers started adding up */
    int64_t last_end;
    int64_t wait, worst;
    unsigned frames;
    double game_time;           /* the game's time then (60ths) */
    uint32_t frame_count;       /* the game's frame count then */
} log_;

/* [waw-decomp/fh2] 11-23: a steady clock on both systems (QueryPerformanceCounter on Windows, where this read the
 * wall clock, which a time sync can move; fh_host.c) */
static int64_t now_ns(void)
{
    return (int64_t)fh_host_now_ns();
}

/* Vertical blanks a frame for D3D8's presentation interval (0: no wait). The IMMEDIATE bit with an interval
 * (0x80000001 and the like) keeps the interval; read_presentation notes the bit. */
static unsigned blanks_per_frame(uint32_t interval)
{
    if (interval == INTERVAL_IMMEDIATE)
        return 0;
    switch (interval & ~INTERVAL_IMMEDIATE) {
    case 0: case 1: return 1;
    case 2: return 2;
    case 4: return 3;
    case 8: return 4;
    default:
        fprintf(stderr, "[fh] frame pacing: presentation interval 0x%X not known; every vertical blank\n", interval);
        return 1;
    }
}

/* display.max_fps given as a number: the port's limit; unset, "", "console" or "game": the game's own pace. */
static int limit_from_settings(void)
{
    const char *text = fh_setting_text("display.max_fps", "FH_MAX_FPS");

    return text && text[0] && strcmp(text, "console") != 0 && strcmp(text, "game") != 0;
}

/* The frame length from what the game asked CreateDevice for, or the port's limit; printed when it changes. */
static void read_presentation(void)
{
    const char *setting = getenv("FH_FRAME_PACING");
    uint32_t hz, interval;
    unsigned blanks;
    int was_pacing = pacing;
    int64_t was_limit = limit_ns;
    double every;

    seen_generation = fh_settings_generation();
    if (limit_from_settings()) {
        int max_fps = fh_setting_int("display.max_fps", "FH_MAX_FPS", 0);

        if (max_fps < 0)
            max_fps = 0;
        if (max_fps > MAX_FPS_LIMIT)
            max_fps = MAX_FPS_LIMIT;
        pacing = 0;
        limit_ns = max_fps ? NS_PER_SECOND / max_fps : 0;
    } else {
        pacing = !(setting && strcmp(setting, "0") == 0);
        limit_ns = 0;
    }
    every = fh_setting_number("display.fps_log", "FH_FPS_LOG", 0.0);
    log_.log_every = every > 0.0 ? (int64_t)(every * (double)NS_PER_SECOND) : 0;
    fh_d3d_presentation(&hz, &interval);
    if (hz < 24 || hz > 240)
        hz = 60;                /* before CreateDevice, or a rate no television had */
    blanks = blanks_per_frame(interval);
    blank_ns = NS_PER_SECOND / hz;
    frame_ns = pacing && blanks ? (int64_t)blanks * NS_PER_SECOND / hz : 0;
    late_at_once = (interval & INTERVAL_IMMEDIATE) != 0;
    if (pacing == was_pacing && limit_ns == was_limit)
        return;
    due = 0;
    next_start = 0;
    if (!pacing && limit_ns)
        fprintf(stderr, "[fh] frame limit: %lld frames a second (display.max_fps; the port paces the frames)\n",
                (long long)(NS_PER_SECOND / limit_ns));
    else if (!pacing)
        fprintf(stderr, "[fh] frame pacing off (%s): frames come as fast as they can\n",
                limit_from_settings() ? "display.max_fps = 0" : "FH_FRAME_PACING=0");
    else if (frame_ns)
        fprintf(stderr, "[fh] frames held to the game's presentation: %u Hz, every %u vertical blank(s) (interval "
                "0x%X): %.2f frames a second; a late frame shown %s\n", hz, blanks, interval,
                (double)NS_PER_SECOND / (double)frame_ns, late_at_once ? "at once" : "at the next blank");
    else
        fprintf(stderr, "[fh] the game asked for no wait at Swap (interval 0x%X): frames are not held\n", interval);
}

/* Sleeps until host time t: the host's sleep for all but SPIN_NS, a spin on the clock for the rest. */
static void wait_until(int64_t t)
{
    int64_t left = t - now_ns() - SPIN_NS;

    if (left > 0) {
#if defined(_WIN32)
        if (left >= 2000000)
            Sleep((DWORD)(left / 1000000 - 1));     /* Windows sleeps in its timer's steps: whole ms, less one */
#else
        struct timespec pause = { (time_t)(left / NS_PER_SECOND), (long)(left % NS_PER_SECOND) };

        /* [acct2-fh] 11-13: a signal ends the sleep early (FH_PROFILE's samples come 1000 times a second,
         * fh_profile.c): sleep the rest, rather than spin it */
        while (nanosleep(&pause, &pause) != 0 && errno == EINTR)
            ;
#endif
    }
    while (now_ns() < t)
        ;
}

/* wait_until with the guest CPU let go meanwhile (the game's other threads run, as during the Xbox's wait). */
static void wait_letting_go(int64_t t)
{
    int held = xbox_GuestCpuRelease(XBOX_GUEST_CPU_KERNEL, ORDINAL_KE_DELAY_EXECUTION);

    wait_until(t);
    if (held)
        xbox_GuestCpuAcquire();
}

/* One frame for the log (display.fps_log); the line when its time comes. start: when the pace was asked for (the
 * frame's work done), end: when it let the frame go. */
static void count_frame(int64_t start, int64_t end)
{
    struct fh_frame_clock_stats s;
    double seconds, mean, sd, clock;
    unsigned timed;
    int64_t frame;

    if (!log_.log_every)
        return;
    if (!log_.started) {
        log_.started = log_.since = log_.last_end = end;
        log_.game_time = fh_frame_clock_game_time();
        log_.frame_count = fh_frame_clock_frame_count();
        fh_frame_clock_take_stats(&s);
        return;
    }
    frame = end - log_.last_end;
    log_.last_end = end;
    log_.wait += end - start;
    if (frame > log_.worst)
        log_.worst = frame;
    log_.frames++;
    if (end - log_.since < log_.log_every)
        return;
    seconds = (double)(end - log_.since) / (double)NS_PER_SECOND;
    fh_frame_clock_take_stats(&s);
    timed = s.frames - s.other_modes - s.held_frames;
    mean = timed ? s.step_sum / timed : 0.0;
    sd = timed ? sqrt(fmax(0.0, s.step_sum2 / timed - mean * mean)) : 0.0;
    /* [acct2-fh] 11-22: when the game set its time or frame count anew in this window (a level started again: boot92's
     * last lines read a clock of -18 and 857 million frames a second, the count's fall taken as unsigned), the two are
     * not rates of this window and are said so instead */
    {
        const double game_time = fh_frame_clock_game_time();
        const uint32_t frame_count = fh_frame_clock_frame_count();
        char clock_text[48], count_text[48];

        if (game_time < log_.game_time || frame_count < log_.frame_count) {
            snprintf(clock_text, sizeof clock_text, "set anew in this window");
            snprintf(count_text, sizeof count_text, "set anew in this window");
        } else {
            clock = (game_time - log_.game_time) / 60.0 / seconds;
            snprintf(clock_text, sizeof clock_text, "%.3f", clock);
            snprintf(count_text, sizeof count_text, "%.1f a second", (double)(frame_count - log_.frame_count) / seconds);
        }
        fprintf(stderr, "[fps] %.0f s: %.1f fps, frame %.2f ms (worst %.2f), wait %.2f ms; the game's step %.3f (min "
                "%.3f, max %.3f, sd %.3f; %u of %u at 0, %u at the limit, %u in other modes, %u held for the fixed step "
                "(%u drawn), %u stepped from gathered time), game clock %s, frames counted %s\n",
                (double)(end - log_.started) / (double)NS_PER_SECOND, log_.frames / seconds,
                seconds * 1000.0 / log_.frames, (double)log_.worst / 1e6, (double)log_.wait / 1e6 / log_.frames, mean,
                timed ? s.step_min : 0.0, timed ? s.step_max : 0.0, sd, s.zero_steps, s.frames, s.limited_steps,
                s.other_modes, s.held_frames, s.held_drawn, s.gathered_updates, clock_text, count_text);
    }
    {   /* [acct2-fh] 11-22: the game's D3DDevice_Clear calls in this window (fh_d3d_state.c) */
        struct fh_d3d_clear_stats c;

        fh_d3d_take_clear_stats(&c);
        fprintf(stderr, "[clears] %.0f s: %u calls (%u with the color); %u of the depth: %u before the frame's first "
                "draw, %u in the middle of a frame (%.2f a frame, %u with rectangles), %u honoured (video.depth_clear "
                "%s)\n", (double)(end - log_.started) / (double)NS_PER_SECOND, c.calls, c.color, c.depth,
                c.depth_before_draws, c.depth_mid_frame, log_.frames ? (double)c.depth_mid_frame / log_.frames : 0.0,
                c.with_rects, c.honoured, fh_d3d_depth_clear_on() ? "on" : "off");
    }
    log_.since = end;
    log_.wait = log_.worst = 0;
    log_.frames = 0;
    log_.game_time = fh_frame_clock_game_time();
    log_.frame_count = fh_frame_clock_frame_count();
}

void fh_frame_pace(void)
{
    int64_t now, show_at;

    if (pacing < 0 || fh_settings_generation() != seen_generation)
        read_presentation();
    now = now_ns();
    if (!pacing) {
        /* the port's limit, or none: this frame's end is the next frame's start; no catching up after a slow one */
        if (limit_ns) {
            int64_t waited;

            if (next_start > now)
                wait_letting_go(next_start);
            waited = now_ns();
            next_start = (next_start > waited - limit_ns ? next_start : waited) + limit_ns;
        }
        count_frame(now, now_ns());
        return;
    }
    if (!frame_ns) {
        count_frame(now, now);
        return;
    }
    if (due == 0 || (now >= due && late_at_once)) {
        /* the first frame, or a late one the game lets be shown at once: no wait, the next is due a frame from now */
        due = now + frame_ns;
        count_frame(now, now);
        return;
    }
    show_at = due;
    if (now > due)              /* late, and shown at the next vertical blank on the frames' own beat */
        show_at = due + ((now - due) / blank_ns + 1) * blank_ns;
    wait_letting_go(show_at);
    due = show_at + frame_ns;
    count_frame(now, now_ns());
}
