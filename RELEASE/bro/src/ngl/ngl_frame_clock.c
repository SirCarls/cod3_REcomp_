/* [acct2-bro] Big Red One port: NGL's frame clock.
 *
 * On the Xbox, NGL counts vertical blanks. nglInit hands D3DDevice_SetVerticalBlankCallback a callback (0x000643E0)
 * that adds one to NGL's vblank count (0x009028E4) at every vblank, 60 times a second. nglBeginFrame copies the
 * count (to 0x009028E8) and the time stamp counter (to 0x00902BA8) when a frame begins, and the game turns the count
 * into its frame time (nglBeginSceneRender: vblanks x 16.667 ms). nglEndFrame waits until the count has moved on by
 * the frame interval nglSetVBlankMode chose (0x008A2CB0: 0 = don't wait, 1 = every vblank, 2 = every other one;
 * Big Red One asks for 2, so it runs at 30 frames a second), presents, and records the frame's times (0x00064330).
 *
 * nglInit, nglBeginFrame and nglEndFrame are stand-ins in this port (NGL is being rewritten on Vulkan), so with
 * nothing else the vblank count stayed 0, every frame took no time, and the frame loop ran millions of times a
 * second. This file does the timing part of those three functions natively: a 60 Hz vblank clock from the host's
 * monotonic clock, the frame interval's wait, and, at the end of each frame, the game's own vblank callback once
 * for each vblank that went by and then the game's own end-of-frame bookkeeping. The game's code still does the
 * counting; this only supplies the vblanks, as the Xbox's vblank interrupt did.
 *
 * The wait goes through the kernel's KeDelayExecutionThread, so the game's other threads run meanwhile, as they did
 * while the Xbox's Present waited for the vblank.
 *
 * After the wait, the thread's deferred file-read completions (APCs) run, as they would at an alertable wait. The
 * kernel defers them to the thread's next alertable wait (main.c turns that on), and the movie player reads its
 * file asynchronously (Movie_QueueReads 0x000CF1E0, completion Movie_OnReadComplete 0x000CF7C0). Once the movie
 * code is the only thing running, nothing on this thread waits alertably any more, so the second movie's reads
 * never completed and it never showed a frame (runs 19-25).
 *
 * The frame's picture is drawn (ngl_present.c) before the wait, as the Xbox's Present queued the flip and then
 * waited for its vblank.
 *
 * BRO_FRAME_PACING=0 turns the wait off (for test runs that should go as fast as they can); the vblank count still
 * follows real time.
 *
 * [acct2-bro-game] The frame limit: display.max_fps in bro.toml (BRO_MAX_FPS). Not set, frames come at the game's
 * own pace as above (the Xbox's 30 a second). Set, the game's interval is not waited for and the port paces the
 * frames itself: 0 is no limit (as BRO_FRAME_PACING=0), 1 to 1000 at most that many frames a second (a frame starts
 * no sooner than 1/max_fps after the one before; the wait sleeps, then spins its last quarter millisecond, so a high
 * limit is kept to within a few microseconds). The vblanks follow real time either way, so the game's clock doesn't
 * change with the limit.
 *
 * The frame's parts are measured each frame, host time: the game (from the end of one frame to nglEndFrame of the
 * next: the game's own code and NGL's scene building), the drawing (ngl_present.c and the end-of-frame lists), and
 * the wait. Once a second they go to the counter on screen (display.show_fps, ngl_fps_counter.c), and every
 * display.fps_log seconds (BRO_FPS_LOG; 0, the default, is never) to a log line:
 *   [fps] 12 s: 143.2 fps, frame 6.98 ms (worst 9.10), game 2.31, drawing 1.12, wait 3.55, game clock 1.000,
 *         GPU wait 0.85 (143), staged 0.42 MB (0 flushes)
 * The game clock is how fast the game's level time (ms) went against real time: 1.000 when the game's speed is
 * right, whatever the frame rate (the frame-rate audit's first check). The GPU wait is how long ngl_vk waited for the
 * GPU to finish the frame before (ms a frame, and how many waits), and staged is how much of the meshes' corners and
 * indices ngl_vk copied to the GPU's own memory (MB a frame, and how many times a full staging buffer was copied and
 * waited for at once); both from ngl_vk, over the same last second as the other numbers.
 *
 * The view's settings (display.fov, bro_view.c) are put in once a frame here, at the frame's end, before the next
 * frame's view is made.
 *
 * Movies keep their own pace with a limit too: the movie player decodes a picture each time it is called, once a
 * frame, so ngl_movie.c holds its decoder calls back to the game's frame interval when the port paces the frames
 * (ngl_frame_clock_paced() is 0). The frames themselves keep the limit.
 *
 * NGL's shader time (0x00901E90, seconds) is the clock the sky's drifting cloud layers, the ocean's waves and other
 * moving materials read. The game advances it once a frame (0x0023BDFB) through nglAdvanceShaderTime (0x00045640)
 * by its frame time (0x008DDAA0), so it follows game time, not the frame count, as on the Xbox. That function is a
 * stand-in too, and until run 115 it did nothing, so the time stayed 0 and nothing moved (run 114). */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "kernel.h"             /* the guest kernel's waits; on Windows also Windows' own Sleep */
#include "recomp_types.h"
#include "ngl_vk.h"             /* the GPU wait and the staging, for the log line */
#include "bro_settings.h"
#include "bro_view.h"
#include "bro_game_cvars.h"         /* [f2-menus] */
#include "ngl_frame_clock.h"
#include "ngl_fps_counter.h"
#include "ngl_present.h"
#include "ngl_quad.h"
#include "ngl_mesh.h"
#include "ngl_quad_batch.h"
#include "ngl_movie.h"

/* The game's code and data (the campaign program). */
#define NGL_VBLANK_CALLBACK  0x000643E0u    /* the callback nglInit registers for every vblank */
#define NGL_FRAME_DONE       0x00064330u    /* nglEndFrame once its wait is over: frame times, the count at the flip */
#define NGL_FRAME_INTERVAL   0x008A2CB0u    /* vblanks per frame (int), set by nglSetVBlankMode */
#define NGL_VBLANK_COUNT     0x009028E4u    /* vblanks so far, counted by the callback */
#define NGL_FRAME_VBLANK     0x009028E8u    /* the count when the frame began */
#define NGL_FRAME_START_TSC  0x00902BA8u    /* the time stamp counter when the frame began, 64 bits */
#define NGL_SHADER_TIME      0x00901E90u    /* seconds, a float: what moving materials read */
#define NGL_SHADER_VECTOR    0x00901EA0u    /* four words nglAdvanceShaderTime keeps for the shaders */
#define NGL_CUR_SCENE        0x0090259Cu    /* ngl_curScene */
#define SCENE_TIME           0x3CCu         /* a scene's time, set by nglSetSceneTime (0x0005E550) */
#define GAME_LEVEL_TIME      0x0098540Cu    /* the game's level time (ms; the CoD2-BRO-Native-Loader's name) */

#define NS_PER_SECOND        1000000000ull
#define VBLANKS_PER_SECOND   60ull          /* NTSC, the rate NGL's own arithmetic assumes (16.667 ms) */
#define MAX_INTERVAL         4u             /* a frame interval above this is not one nglSetVBlankMode sets */
#define MAX_CATCH_UP         600ull         /* vblanks run at once after a long stall: 10 seconds' worth */
#define MAX_FPS_LIMIT        1000           /* the highest display.max_fps */
#define SPIN_NS              250000ll       /* the frame limit's wait spins its last quarter millisecond */
#define NS_PER_MS            1000000.0

static int64_t clock_start;     /* host time of vblank 0: the first frame's end */
static uint64_t vblanks_run;    /* vblank callbacks run so far */
static uint64_t flip_vblank;    /* the vblank the last frame ended at */
static int pacing = 1;          /* wait for the game's own frame interval */
static int64_t limit_ns;        /* the port's frame limit: the shortest frame (0: none) */
static int64_t next_start;      /* the soonest the next frame may start under the limit */

/* The frame's parts (see the top of the file), added up until they are shown. */
static struct {
    int64_t frame_end;          /* host time the last frame ended (after its wait and callbacks) */
    int64_t since;              /* when the numbers started adding up */
    int64_t game, drawing, wait, worst;
    unsigned frames;
    int64_t log_every;          /* display.fps_log in ns (0: no log line) */
    int64_t next_log;
    uint32_t level_time;        /* the game's level time when the numbers started adding up */
} parts;

static int64_t now_ns(void)
{
    struct timespec t;

    clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec * (int64_t)NS_PER_SECOND + t.tv_nsec;
}

/* The vblank number at host time t, and the host time vblank n comes (rounded up). */
static uint64_t vblank_at(int64_t t)
{
    return t <= clock_start ? 0 : (uint64_t)(t - clock_start) * VBLANKS_PER_SECOND / NS_PER_SECOND;
}

static int64_t time_of_vblank(uint64_t n)
{
    return clock_start + (int64_t)((n * NS_PER_SECOND + VBLANKS_PER_SECOND - 1) / VBLANKS_PER_SECOND);
}

/* Sleep until host time t, letting the game's other threads run. */
static void wait_until(int64_t t)
{
    int64_t left = t - now_ns();
    LARGE_INTEGER interval;

    if (left <= 0)
        return;
    interval.QuadPart = -((left + 99) / 100);     /* relative, in 100 ns units */
    xbox_KeDelayExecutionThread((KPROCESSOR_MODE)0, 0, &interval);
}

/* Wait until host time t to within a few microseconds, for the frame limit. The guest kernel's delay counts whole
 * milliseconds (and at least one), too coarse for a limit of hundreds of frames a second, so the host's own sleep
 * takes all but the last SPIN_NS, and a spin on the clock the rest. */
static void wait_precisely_until(int64_t t)
{
    int64_t left = t - now_ns() - SPIN_NS;

    if (left > 0) {
#if defined(_WIN32)
        if (left >= 2000000)
            Sleep((DWORD)(left / 1000000 - 1));     /* Windows sleeps in its timer's steps: whole ms, less one */
#else
        struct timespec pause = { (time_t)(left / (int64_t)NS_PER_SECOND), (long)(left % (int64_t)NS_PER_SECOND) };

        nanosleep(&pause, NULL);
#endif
    }
    while (now_ns() < t)
        ;
}

/* The frame limit, read once: display.max_fps (BRO_MAX_FPS), or BRO_FRAME_PACING=0 for no limit. */
static void read_frame_limit(void)
{
    const char *old_switch = getenv("BRO_FRAME_PACING");
    double fps_log;

    if (bro_setting_given("display.max_fps", "BRO_MAX_FPS")) {
        int max_fps = bro_setting_int("display.max_fps", "BRO_MAX_FPS", 0);

        if (max_fps < 0)
            max_fps = 0;
        if (max_fps > MAX_FPS_LIMIT)
            max_fps = MAX_FPS_LIMIT;
        pacing = 0;
        limit_ns = max_fps ? (int64_t)NS_PER_SECOND / max_fps : 0;
        if (max_fps)
            fprintf(stderr, "[fps] frame limit: %d frames a second (the port paces the frames)\n", max_fps);
        else
            fprintf(stderr, "[fps] frame limit: none\n");
    } else if (old_switch && old_switch[0] == '0') {
        pacing = 0;
        fprintf(stderr, "[fps] frame limit: none (BRO_FRAME_PACING=0)\n");
    }
    fps_log = bro_setting_number("display.fps_log", "BRO_FPS_LOG", 0.0);
    parts.log_every = fps_log > 0.0 ? (int64_t)(fps_log * (double)NS_PER_SECOND) : 0;
}

/* One frame's parts added up; once a second they go to the counter, and every display.fps_log seconds to the log. */
static void count_frame(int64_t draw_start, int64_t draw_end, int64_t wait_end, int64_t frame_end)
{
    int64_t frame;
    double seconds;

    if (!parts.frame_end) {         /* the first frame: nothing to measure from yet */
        parts.frame_end = parts.since = frame_end;
        parts.next_log = frame_end + parts.log_every;
        parts.level_time = MEM32(GAME_LEVEL_TIME);
        return;
    }
    frame = frame_end - parts.frame_end;
    parts.game += draw_start - parts.frame_end;
    parts.drawing += draw_end - draw_start;
    parts.wait += wait_end - draw_end;
    if (frame > parts.worst)
        parts.worst = frame;
    parts.frames++;
    parts.frame_end = frame_end;
    seconds = (double)(frame_end - parts.since) / (double)NS_PER_SECOND;
    if (seconds < 1.0)
        return;
    ngl_fps_counter_set(parts.frames / seconds, seconds * 1000.0 / parts.frames);
    if (parts.log_every) {
        /* ngl_vk's counts since the last second, taken every second so they cover the same frames as the rest */
        uint32_t gpu_waits = 0;
        uint64_t flushes = 0;
        uint64_t gpu_wait = ngl_vk_take_gpu_wait(&gpu_waits);
        uint64_t staged = ngl_vk_take_staging(&flushes);

        if (frame_end >= parts.next_log) {
            /* the game's level clock against real time (1.000: the game's speed is right; it stands still in menus) */
            uint32_t level_time = MEM32(GAME_LEVEL_TIME);
            double clock = (double)(uint32_t)(level_time - parts.level_time) / (seconds * 1000.0);

            fprintf(stderr, "[fps] %.0f s: %.1f fps, frame %.2f ms (worst %.2f), game %.2f, drawing %.2f, wait %.2f, "
                    "game clock %.3f, GPU wait %.2f (%u), staged %.2f MB (%llu flushes)\n",
                    (double)(frame_end - clock_start) / (double)NS_PER_SECOND,
                    parts.frames / seconds, seconds * 1000.0 / parts.frames, (double)parts.worst / NS_PER_MS,
                    (double)parts.game / NS_PER_MS / parts.frames, (double)parts.drawing / NS_PER_MS / parts.frames,
                    (double)parts.wait / NS_PER_MS / parts.frames, clock,
                    (double)gpu_wait / NS_PER_MS / parts.frames, gpu_waits,
                    (double)staged / (1024.0 * 1024.0) / parts.frames, (unsigned long long)flushes);
            parts.next_log = frame_end + parts.log_every;
        }
    }
    parts.level_time = MEM32(GAME_LEVEL_TIME);
    parts.since = frame_end;
    parts.game = parts.drawing = parts.wait = parts.worst = 0;
    parts.frames = 0;
}

/* Run one of the game's own functions that takes no arguments, as a call from here. Registers a caller may expect
 * kept across a stand-in are put back. */
static void run_game_function(uint32_t va)
{
    static int reported;
    recomp_func_t fn = recomp_lookup(va);
    uint32_t eax = g_eax, ecx = g_ecx, edx = g_edx;

    if (!fn) {
        if (reported++ < 4)
            fprintf(stderr, "[ngl] frame clock: no recompiled function at 0x%08X\n", va);
        return;
    }
    g_esp -= 4;
    MEM32(g_esp) = 0;       /* the return address; the function's ret takes it off again */
    fn();
    g_eax = eax;
    g_ecx = ecx;
    g_edx = edx;
}

/* This thread's deferred file-read completions, run now as an alertable wait would run them. */
static void run_read_completions(void)
{
    static int announced;
    int ran = xbox_RunDeferredIoApcs();

    if (ran && !announced) {
        announced = 1;
        fprintf(stderr, "[ngl] frame end: ran %d deferred read completion(s); from now on every frame does\n", ran);
    }
}

/* BRO_MOVIE_TRACE=1: every 30 frames, the current movie's read slots and state (Movie_Open 0x00332030 keeps the
 * movie at 0x0095F454 and loops on Movie_IsReady 0x000CF750 until its reads are in). Offsets from Movie_IsReady and
 * Movie_HasPendingReads, Movie_QueueReads and Movie_FetchBytes: +0x44 file, +0x58..+0x64 pending reads (-1 = none),
 * +0x68..+0x6B slot ready flags, +0x84 read slot, +0x88 slot to move to, +0x80 write slot, +0x90 slot holding the
 * file's last chunk, +0x78 file size, +0x7C next read offset, +0x70 state. */
static void trace_movie(void)
{
    static int setting = -1;
    static unsigned frames;
    uint32_t m;

    if (setting < 0) {
        const char *value = getenv("BRO_MOVIE_TRACE");

        setting = value && value[0] == '1';
    }
    if (!setting || frames++ % 30)
        return;
    m = MEM32(0x0095F454u);
    if (!m) {
        fprintf(stderr, "[movie] frame %u: no movie\n", frames - 1);
        return;
    }
    fprintf(stderr, "[movie] frame %u: movie 0x%08X file %08X pending %08X %08X %08X %08X ready %u%u%u%u read %u "
            "next %u write %u last-slot %u size %u at %u state %u open-done %u\n", frames - 1, m, MEM32(m + 0x44),
            MEM32(m + 0x58), MEM32(m + 0x5C), MEM32(m + 0x60), MEM32(m + 0x64), MEM8(m + 0x68), MEM8(m + 0x69),
            MEM8(m + 0x6A), MEM8(m + 0x6B), MEM32(m + 0x84), MEM32(m + 0x88), MEM32(m + 0x80), MEM32(m + 0x90),
            MEM32(m + 0x78), MEM32(m + 0x7C), MEM32(m + 0x70), MEM8(0x0095F46Cu));
    /* What decides whether the main menu (its draw: 0x00331260) decodes its background movie: 0x008DCE55 must be
     * clear, the menu's movie flag (+0x38) set, and 0x0095F916 clear; and who is ending this frame, from the code
     * addresses on the guest stack (the newest first). */
    {
        uint32_t i, found = 0, sp = g_esp;

        /* SCR_UpdateScreen 0x002F1E60 draws only with 0x00933EE4 set and 0x009403F4 clear; SCR_DrawScreenField
         * 0x002F1D90 then draws the menus through 0x003153D0 (0x0095F919 set) and 0x00315440 (0x0095F918 set),
         * whatever cls_state (0x0092BDF0) is, unless it is 5; both need ngl_curScene (0x0090259C) set. */
        fprintf(stderr, "[movie]   gates: 8DCE55 %u 95F916 %u 95F91E %u; screen 933EE4 %u 9403F4 %u cls_state %u "
                "menus 95F919 %u 95F918 %u ngl_curScene %08X; stack:", MEM8(0x008DCE55u), MEM8(0x0095F916u),
                MEM8(0x0095F91Eu), MEM32(0x00933EE4u), MEM8(0x009403F4u), MEM32(0x0092BDF0u), MEM8(0x0095F919u),
                MEM8(0x0095F918u), MEM32(0x0090259Cu));
        for (i = 0; i < 256 && found < 8; i++) {
            uint32_t value = MEM32(sp + i * 4);

            if (value >= 0x00011000u && value < 0x007B0000u) {
                fprintf(stderr, " %08X", value);
                found++;
            }
        }
        fprintf(stderr, "\n");
    }
}

int ngl_frame_clock_paced(void)
{
    return pacing;
}

void ngl_frame_clock_begin_frame(void)
{
    uint64_t tsc = xbox_ReadTimeStampCounter();

    MEM32(NGL_FRAME_VBLANK) = MEM32(NGL_VBLANK_COUNT);
    MEM32(NGL_FRAME_START_TSC) = (uint32_t)tsc;
    MEM32(NGL_FRAME_START_TSC + 4) = (uint32_t)(tsc >> 32);
}

void ngl_frame_clock_end_frame(void)
{
    int64_t now = now_ns(), drawn, waited;
    uint32_t interval = MEM32(NGL_FRAME_INTERVAL);
    uint64_t vblank, due;

    if (!clock_start) {
        read_frame_limit();
        clock_start = now;
    }
    if (interval > MAX_INTERVAL)
        interval = MAX_INTERVAL;
    ngl_present_frame();
    ngl_quad_end_frame();
    ngl_mesh_end_frame();
    ngl_quad_batch_end_frame();
    drawn = now_ns();

    vblank = vblank_at(now);
    due = flip_vblank + interval;
    if (pacing && vblank < due) {
        wait_until(time_of_vblank(due));
        vblank = vblank_at(now_ns());
        if (vblank < due)
            vblank = due;           /* woke a moment early */
    } else if (limit_ns) {
        /* the port's limit: this frame's end is the next frame's start; no catching up after a slow frame */
        if (next_start > drawn)
            wait_precisely_until(next_start);
        waited = now_ns();
        next_start = (next_start > waited - limit_ns ? next_start : waited) + limit_ns;
        vblank = vblank_at(waited);
    }
    waited = now_ns();
    run_read_completions();
    trace_movie();
    ngl_movie_frame_end(waited);

    /* The vblanks that went by, each through the game's own callback, as the vblank interrupt ran it. */
    if (vblank > vblanks_run + MAX_CATCH_UP)
        vblanks_run = vblank - MAX_CATCH_UP;
    while (vblanks_run < vblank) {
        run_game_function(NGL_VBLANK_CALLBACK);
        vblanks_run++;
    }
    run_game_function(NGL_FRAME_DONE);
    flip_vblank = vblank;
    bro_view_apply();
    bro_game_cvars_frame();     /* [f2-menus] the F2 window's changes to the game's cvars go in at the frame's end */
    count_frame(now, drawn, waited, now_ns());
}

/* nglAdvanceShaderTime (0x00045640): time += seconds, the vector's four words kept at 0x00901EA0, and then, as the
 * original's call to nglSetSceneTime does, the current scene's time set to the new time. */
void ngl_frame_clock_advance_shader_time(uint32_t vector, uint32_t seconds_bits)
{
    static unsigned calls;
    union { uint32_t bits; float value; } time, seconds;
    uint32_t scene = MEM32(NGL_CUR_SCENE);
    int i;

    time.bits = MEM32(NGL_SHADER_TIME);
    seconds.bits = seconds_bits;
    time.value += seconds.value;
    MEM32(NGL_SHADER_TIME) = time.bits;
    if (vector)
        for (i = 0; i < 4; i++)
            MEM32(NGL_SHADER_VECTOR + 4 * i) = MEM32(vector + 4 * i);
    if (scene)
        MEM32(scene + SCENE_TIME) = time.bits;
    if (calls++ % 900 == 0 && calls < 4 * 900)
        fprintf(stderr, "[ngl] shader time %g s (frame time %g s)\n", time.value, seconds.value);
}
