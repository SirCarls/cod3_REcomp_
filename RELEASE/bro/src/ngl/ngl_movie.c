/* [acct2-bro-game] Big Red One port: movies at their own pace whatever the frame rate.
 *
 * The movie player calls its decoder once a frame: the full-screen loop (0x00332230) through the movie's draw
 * (0x0030FE00), through the movie's vtable (+8, the decoder 0x000CE210, thiscall on the movie; 0x0030FE50 calls the
 * same slot). A call decodes one picture into the locked texture and returns 0; it returns 3 at the end of the movie
 * (the loop stops on 3) and 1 on its early way out, with nothing decoded. Nothing gates the pictures by a clock: on
 * the Xbox a movie played at 30 pictures a second because nglEndFrame waited for the game's frame interval (2
 * vblanks). With a frame limit (display.max_fps) that wait is off, so the movies played at the frame rate: the
 * opening's 2,896 pictures took 48 s at 60 frames a second and 20 s uncapped, against 30 pictures a second at the
 * game's own pace (runs mv1, 2026-11-07).
 *
 * So when the port paces the frames itself, the decoder is called through this gate (indirect calls ask
 * recomp_lookup_manual first). A call that comes before the next picture is due answers 1, the decoder's own "nothing
 * yet", and the last picture stays on the screen; the frame goes on at the full rate. A call that is due decodes, and
 * the next picture is due one frame interval later (the game's interval at 60 vblanks a second: 30 pictures a
 * second). So the decoder runs as often as it did on the Xbox, its own state (its frame skipping, its read-ahead)
 * sees the same calls, and the frames in between, menus included, keep the limit. At the game's own pace (no
 * display.max_fps) every call goes straight through, as before.
 *
 * The pictures keep to the movie's clock, not to the last picture, so a movie stays in step with its sound (the
 * sound streams play by the clock, bro_media.c). After a slow frame the pictures that are due decode on the next
 * frames, one a frame, each shown at least one frame, until the movie is back on time. When the frames are too slow
 * for that (a busy PC: frames over 1/30 s), a call more than SKIP_PICTURES pictures behind also decodes the next
 * ones at once, up to MAX_SKIPPED a call, and only the last of them is shown, as a film player drops pictures to
 * keep to its sound. Only a movie's first picture, or one more than CATCH_UP_NS behind (a load), starts the clock
 * again. (The first version started again whenever it was a picture behind: on a busy PC the opening fell 2 s
 * behind. The second only caught up one picture a frame: frames of 60 ms for 2 s put it 0.5 s behind.)
 *
 * display.movie_pacing = false (BRO_MOVIE_PACING=0) lets every call through (movies at the frame rate, to compare).
 * display.skip_movies = true (BRO_SKIP_MOVIES=1; test runs that aren't about the movies) answers every call with the
 * decoder's "end", so each movie stops at once, as if it had played out, and nothing of it is decoded.
 * Either way each movie's pictures go to the log when it stops showing them, and every PROGRESS_NS while it runs:
 *   [movie] 0x<movie>: <n> pictures in <s> s, <rate> a second (<calls> calls held, <late> late, <skipped> skipped,
 *   <restarts> restarts)
 * late: decoded a picture or more after it was due; skipped: decoded but not shown; restarts: the clock's new
 * starts within a movie.
 *
 * [acct2-bro] 2026-11-14 (the owner, 17:33: "BROs intro movies when above 60fps struggle to draw properly"; 17:52:
 * the level intros too). A picture takes the decoder about 15 ms of the frame it is decoded in, and it reached the
 * screen at the end of that frame: when the picture was due, plus up to a frame (the first frame to start after its
 * due time), plus that decode's own time. At 60 the pictures fall on every other frame, at a steady 33.3 ms; above 60
 * the frames don't line up with the pictures and a long decode frame moves the frames after it, so the pictures came
 * unevenly (bm1-bm3: films at 157 fps with a limit of 240, worst frames 18-21 ms). So in the full-screen film loop
 * (the intros, the chapter films; not a menu's background) the decoder is now called ahead of the picture's time, by
 * the frame limit's frame plus the recent decodes' 90th percentile plus 2 ms (at most display.movie_lead_ms,
 * BRO_MOVIE_LEAD_MS, default 30; 0 = never), and it goes on the screen at that time: each
 * picture reaches the screen when it is due, every 33.3 ms at any frame limit, and in step with the sound. Until then
 * the frames go on showing the picture before it (a copy taken before the decoder writes over it,
 * ngl_movie_held_picture, ngl_present.c), and only the last frame before its time waits for it, no longer than the
 * frame would have taken anyway (ngl_frame_clock.c), so the films keep their frame rate (bb1 held the whole frame:
 * 90 fps under a limit of 240). A picture decoded late (a busy PC) is shown at once. When that lead doesn't fit in
 * display.movie_lead_ms (a limit of about 90 or less, a slow decoder) nothing changes: at 60 the pictures already fall
 * evenly on every other frame, and bt2 found a lead there worse (off even by 20% p90 against 10%), so at low frame
 * rates the films play exactly as before (the owner, 18:15: no harm at lower fps, project wide). Each
 * movie's log then gets a second line, how evenly its pictures reached the screen and the decoder's times:
 *   [movie] 0x<movie>: shown every <ms> ms (p10 <ms>, p90 <ms>, max <ms>; off even by median <%>, p90 <%>), decode
 *   median <ms> (p90 <ms>, max <ms>), <n> pictures held to their time (lead <ms> ms, the last one used)
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>              /* qsort, realloc */
#include <string.h>              /* memcpy */
#include <time.h>

#include "recomp_types.h"
#include "bro_settings.h"
#include "xbox_memory_layout.h"     /* XBOX_PTR: guest memory */
#include "ngl_frame_clock.h"
#include "ngl_texture.h"            /* the locked picture the decoder writes into */
#include "ngl_movie.h"

/* The game's code and data (the campaign program). */
#define MOVIE_DECODER        0x000CE210u    /* the movie's vtable +8: decode the next picture */
#define MOVIE_CURRENT        0x0095F454u    /* the movie that is open (Movie_Open, 0x00332030), 0 when none */
#define NGL_FRAME_INTERVAL   0x008A2CB0u    /* vblanks per frame (int), set by nglSetVBlankMode */
#define DECODER_PICTURE      0u             /* the decoder's answers */
#define DECODER_NOTHING_YET  1u
#define DECODER_END          3u

#define NS_PER_SECOND        1000000000ll
#define VBLANKS_PER_SECOND   60ll
#define DEFAULT_INTERVAL     2u             /* when the game asks for no wait: BRO's own 30 a second */
#define MAX_INTERVAL         4u
#define EARLY_NS             1000000ll      /* a call this close to the due time decodes (frame timing jitter) */
#define GAP_NS               500000000ll    /* half a second with no new picture: that movie is over */
#define CATCH_UP_NS          500000000ll    /* further behind than this: start the clock again, don't catch up */
#define PROGRESS_NS          30000000000ll  /* a movie still running reports every 30 s (menu backgrounds) */
#define SKIP_PICTURES        2              /* further behind than this many pictures: decode the next ones at once */
#define MAX_SKIPPED          4              /* the most pictures a call decodes without showing them */
/* [acct2-bro] 2026-11-14: the full-screen film loop (0x00332230) calls the decoder through the movie's draw
 * (0x0030FE00), whose indirect call returns here; a menu's background film comes through other callers */
#define FULL_SCREEN_RETURN   0x0030FE23u
#define DEFAULT_LEAD_MS      30             /* display.movie_lead_ms: the most a picture is decoded ahead (0: off) */
#define MAX_LEAD_MS          30             /* less than a picture (33.3 ms), so the one before is shown first */
#define DECODE_GUESS_NS      20000000ll     /* the decoder's time before RECENT_MIN decodes have been measured */
#define LEAD_SPARE_NS        2000000ll      /* added to the frame and the decode */
#define RECENT               32             /* the decodes the lead's 90th percentile is taken over */
#define RECENT_MIN           8
#define MAX_TIMES            4096           /* the pictures a movie's evenness numbers are kept for */

static int setting = -1;        /* display.movie_pacing, read on first use */
static int skip = -1;           /* display.skip_movies, read on first use */
static int64_t due;             /* host time the next picture is due, 0 before the first */
static int64_t lead_ns = -1;    /* [acct2-bro] 2026-11-14 display.movie_lead_ms, read on first use */
static int64_t last_lead;       /* the lead the last early picture got (for the log) */
static uint32_t recent[RECENT]; /* the last decodes' times (us), all films */
static unsigned recent_count, recent_next;
static int64_t show_at;         /* the decoded picture goes on the screen at this host time (0: none waiting) */
static int picture_new;         /* a picture has been decoded since the last frame was drawn */
/* the picture before it, copied before the decoder wrote over it, shown until show_at (bb3) */
static struct {
    uint8_t *pixels;
    size_t size;                /* allocated bytes */
    uint32_t width, height, pitch;
    int valid;
} before;

/* [acct2-bro] 2026-11-14: how evenly the current movie's pictures reached the screen (the time from one picture's
 * frame drawn to the next's, us), and the decoder's times (us), for the movie's second log line */
static struct {
    uint32_t gaps[MAX_TIMES], decodes[MAX_TIMES];
    unsigned gap_count, decode_count, held_to_time;
    int64_t last_drawn;         /* host time the last new picture's frame was drawn (0: none yet) */
} times;

static struct {
    uint32_t movie;             /* the movie the pictures are from */
    unsigned long pictures, held, late, skipped, restarts;
    int64_t first, last;        /* host time of the first and the latest picture */
    int64_t reported;           /* host time of the last report while it runs */
} shown;

static int64_t now_ns(void)
{
    struct timespec t;

    clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec * NS_PER_SECOND + t.tv_nsec;
}

static void print_movie(const char *so_far)
{
    double seconds = (double)(shown.last - shown.first) / (double)NS_PER_SECOND;

    fprintf(stderr, "[movie] 0x%08X: %lu pictures%s in %.2f s, %.1f a second "
                    "(%lu calls held, %lu late, %lu skipped, %lu restarts)\n",
            shown.movie, shown.pictures, so_far, seconds,
            seconds > 0.0 ? (double)(shown.pictures - 1) / seconds : 0.0, shown.held, shown.late, shown.skipped,
            shown.restarts);
}

static int64_t picture_ns(void);    /* below */

static int compare_u32(const void *a, const void *b)
{
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;

    return x < y ? -1 : x > y;
}

/* The p-th percentile (0-100) of n values (sorted here, in place), in ms. */
static double percentile_ms(uint32_t *values, unsigned n, unsigned p)
{
    if (!n)
        return 0.0;
    qsort(values, n, sizeof *values, compare_u32);
    return values[(n - 1) * p / 100] / 1000.0;
}

/* [acct2-bro] 2026-11-14: the movie's second line: how evenly its pictures reached the screen, and the decoder. */
static void print_times(void)
{
    static uint32_t off[MAX_TIMES];
    double period_us = (double)picture_ns() / 1000.0;
    unsigned i;

    if (times.gap_count < 2)
        return;
    for (i = 0; i < times.gap_count; i++) {   /* each gap's distance from the picture's time, in 0.1 % */
        double d = (double)times.gaps[i] - period_us;

        off[i] = (uint32_t)((d < 0.0 ? -d : d) * 1000.0 / period_us);
    }
    qsort(off, times.gap_count, sizeof *off, compare_u32);
    fprintf(stderr, "[movie] 0x%08X: shown every %.1f ms (p10 %.1f, p90 %.1f, max %.1f; off even by median %.1f%%, "
                    "p90 %.1f%%), decode median %.1f ms (p90 %.1f, max %.1f), %u pictures held to their time "
                    "(lead %lld ms)\n", shown.movie, percentile_ms(times.gaps, times.gap_count, 50),
            percentile_ms(times.gaps, times.gap_count, 10), percentile_ms(times.gaps, times.gap_count, 90),
            percentile_ms(times.gaps, times.gap_count, 100), off[(times.gap_count - 1) / 2] / 10.0,
            off[(times.gap_count - 1) * 90 / 100] / 10.0, percentile_ms(times.decodes, times.decode_count, 50),
            percentile_ms(times.decodes, times.decode_count, 90), percentile_ms(times.decodes, times.decode_count, 100),
            times.held_to_time, (long long)(last_lead / 1000000));
}

static void report_movie(void)
{
    print_movie("");
    print_times();
    shown.pictures = shown.held = shown.late = shown.skipped = shown.restarts = 0;
    times.gap_count = times.decode_count = times.held_to_time = 0;
    times.last_drawn = 0;
    show_at = 0;
    picture_new = 0;
    before.valid = 0;
    last_lead = 0;
}

/* One picture's time: the game's frame interval in vblanks, at 60 a second. */
static int64_t picture_ns(void)
{
    uint32_t interval = MEM32(NGL_FRAME_INTERVAL);

    if (!interval || interval > MAX_INTERVAL)
        interval = DEFAULT_INTERVAL;
    return (int64_t)interval * NS_PER_SECOND / VBLANKS_PER_SECOND;
}

/* A picture has been decoded at now: the movie's clock moves on one picture. */
static void count_picture(uint32_t movie, int64_t now, int64_t period)
{
    if (!shown.pictures || !due || now - due > CATCH_UP_NS) {
        if (shown.pictures)
            shown.restarts++;
        due = now + period;                 /* the movie's first picture, or far behind: the clock starts here */
    } else {
        if (now - due >= period)
            shown.late++;                   /* behind: the next ones decode on the next frames */
        due += period;
    }
    if (!shown.pictures) {
        shown.movie = movie;
        shown.first = shown.reported = now;
    }
    shown.pictures++;
    shown.last = now;
    picture_new = 1;                        /* [acct2-bro] 2026-11-14: for ngl_movie_frame_drawn */
}

/* [acct2-bro] 2026-11-14: how far ahead of its time the next picture is decoded: the frame limit's frame (the decode
 * may start that late after the lead), the recent decodes' 90th percentile and LEAD_SPARE_NS, if that fits in
 * display.movie_lead_ms; 0 if not (at about 90 fps and below, or a slow decoder: as before). */
static int64_t picture_lead(void)
{
    int64_t decode = DECODE_GUESS_NS, want;

    if (lead_ns <= 0)
        return 0;
    if (recent_count >= RECENT_MIN) {
        uint32_t sorted[RECENT];

        memcpy(sorted, recent, recent_count * sizeof *sorted);
        qsort(sorted, recent_count, sizeof *sorted, compare_u32);
        decode = (int64_t)sorted[(recent_count - 1) * 90 / 100] * 1000;
    }
    want = ngl_frame_clock_limit_ns() + decode + LEAD_SPARE_NS;
    return want <= lead_ns ? want : 0;
}

/* The decoder through the gate: thiscall on the movie (ecx), no stack arguments, its answer in eax. */
static void decode_when_due(void)
{
    recomp_func_t decoder = recomp_lookup(MOVIE_DECODER);
    uint32_t movie = g_ecx, back = MEM32(g_esp);   /* the caller's return address */
    int64_t now = now_ns(), period = picture_ns();
    int gated = setting && !ngl_frame_clock_paced(), extra;
    /* [acct2-bro] 2026-11-14: in the full-screen film loop, with the port pacing the frames, decode ahead of the
     * picture's time and hold the frame's picture until then (see the top of the file) */
    int64_t lead = gated && back == FULL_SCREEN_RETURN ? picture_lead() : 0, picture_due, started;

    if (skip) {
        static unsigned skipped_calls;

        if (++skipped_calls <= 50)
            fprintf(stderr, "[movie] 0x%08X skipped (display.skip_movies)\n", movie);
        g_eax = DECODER_END;                /* the movie is over: the player closes it */
        g_esp += 4;
        return;
    }
    if (shown.pictures && movie != shown.movie)
        report_movie();
    if (gated && due && now < due - (lead > EARLY_NS ? lead : EARLY_NS)) {
        g_eax = DECODER_NOTHING_YET;        /* not due: the last picture stays */
        g_esp += 4;                         /* ret: the caller pushed only the return address */
        shown.held++;
        return;
    }
    if (!decoder) {
        g_eax = DECODER_NOTHING_YET;
        g_esp += 4;
        return;
    }
    picture_due = due;                      /* [acct2-bro] 2026-11-14: this picture's time (0: the first) */
    before.valid = 0;
    if (lead && picture_due > now) {        /* early: keep the picture on the screen until the new one's time */
        uint32_t buffer, width, height, pitch;

        if (ngl_texture_last_lock(&buffer, &width, &height, &pitch)) {
            size_t size = (size_t)pitch * height;

            if (size > before.size) {
                uint8_t *grown = realloc(before.pixels, size);

                if (grown) {
                    before.pixels = grown;
                    before.size = size;
                }
            }
            if (size <= before.size) {
                memcpy(before.pixels, (const void *)XBOX_PTR(buffer), size);
                before.width = width;
                before.height = height;
                before.pitch = pitch;
                before.valid = 1;
            }
        }
    }
    started = now_ns();
    decoder();                              /* pops the return address itself */
    if (g_eax != DECODER_PICTURE)
        return;                             /* nothing yet, or the end: try again next frame */
    {
        uint32_t took = (uint32_t)((now_ns() - started) / 1000);

        if (times.decode_count < MAX_TIMES)
            times.decodes[times.decode_count++] = took;
        recent[recent_next] = took;         /* for picture_lead */
        recent_next = (recent_next + 1) % RECENT;
        if (recent_count < RECENT)
            recent_count++;
    }
    if (lead && picture_due) {
        show_at = picture_due;              /* the frame shows it at its time (ngl_movie_show_at), if still ahead */
        last_lead = lead;
        if (picture_due > now_ns())
            times.held_to_time++;
    }
    count_picture(movie, now, period);
    for (extra = 0; gated && extra < MAX_SKIPPED && now - due > SKIP_PICTURES * period; extra++) {
        g_esp -= 4;                         /* the same call again, as the movie's draw makes it */
        MEM32(g_esp) = back;
        g_ecx = movie;
        decoder();
        if (g_eax != DECODER_PICTURE) {
            if (g_eax == DECODER_NOTHING_YET)
                g_eax = DECODER_PICTURE;    /* the picture already decoded is the one shown */
            return;                         /* (the end of the movie answers the end) */
        }
        shown.skipped++;                    /* the one before it was decoded but never shown */
        show_at = 0;                        /* [acct2-bro] 2026-11-14: behind: the last one at once */
        before.valid = 0;
        now = now_ns();
        count_picture(movie, now, period);
    }
}

recomp_func_t ngl_movie_method_for(uint32_t xbox_va)
{
    if (xbox_va != MOVIE_DECODER)
        return NULL;
    if (setting < 0) {
        setting = bro_setting_bool("display.movie_pacing", "BRO_MOVIE_PACING", 1);
        fprintf(stderr, "[movie] %s\n", setting ? "pictures at the movie's own pace, whatever the frame rate "
                                                  "(display.movie_pacing)"
                                                : "pictures at the frame rate (display.movie_pacing is off)");
    }
    if (skip < 0) {
        skip = bro_setting_bool("display.skip_movies", "BRO_SKIP_MOVIES", 0);
        if (skip)
            fprintf(stderr, "[movie] movies are skipped (display.skip_movies)\n");
    }
    if (lead_ns < 0) {      /* [acct2-bro] 2026-11-14 */
        int ms = bro_setting_int("display.movie_lead_ms", "BRO_MOVIE_LEAD_MS", DEFAULT_LEAD_MS);

        if (ms < 0)
            ms = 0;
        if (ms > MAX_LEAD_MS)
            ms = MAX_LEAD_MS;
        lead_ns = (int64_t)ms * 1000000;
        fprintf(stderr, "[movie] %s (display.movie_lead_ms %d)\n", ms ? "full-screen films: each picture decoded "
                "ahead and shown at its own time when the frame limit leaves room (above about 90 fps)"
                : "full-screen films: each picture shown as it is decoded", ms);
    }
    return decode_when_due;         /* off, it only counts the pictures */
}

void ngl_movie_frame_end(int64_t now)
{
    if (!shown.pictures)
        return;
    if (!MEM32(MOVIE_CURRENT) || now - shown.last > GAP_NS) {
        report_movie();
    } else if (now - shown.reported > PROGRESS_NS) {
        print_movie(" so far");
        print_times();                      /* [acct2-bro] 2026-11-14 */
        shown.reported = now;
    }
}

/* [acct2-bro] 2026-11-14: the host time the decoded picture is to go on the screen (0: none waiting). */
int64_t ngl_movie_show_at(void)
{
    return show_at;
}

/* [acct2-bro] 2026-11-14: the decoded picture goes on the screen with this frame. */
void ngl_movie_show_now(void)
{
    show_at = 0;
    before.valid = 0;
}

/* [acct2-bro] 2026-11-14: while a decoded picture waits for its time, the picture before it (a host copy) for the
 * frame to show; 0 when the locked picture is the one to show. */
int ngl_movie_held_picture(const uint8_t **pixels, uint32_t *width, uint32_t *height, uint32_t *pitch)
{
    if (!show_at || !before.valid)
        return 0;
    *pixels = before.pixels;
    *width = before.width;
    *height = before.height;
    *pitch = before.pitch;
    return 1;
}

/* [acct2-bro] 2026-11-14: the frame has been drawn at host time drawn (ns): when it carries a new picture, the time
 * since the last one's goes to the movie's evenness numbers. */
void ngl_movie_frame_drawn(int64_t drawn)
{
    if (!picture_new || show_at)
        return;                             /* no new picture, or it is still waiting for its time */
    picture_new = 0;
    if (times.last_drawn && times.gap_count < MAX_TIMES)
        times.gaps[times.gap_count++] = (uint32_t)((drawn - times.last_drawn) / 1000);
    times.last_drawn = drawn;
}
