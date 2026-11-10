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
 */
#include <stdint.h>
#include <stdio.h>
#include <time.h>

#include "recomp_types.h"
#include "bro_settings.h"
#include "ngl_frame_clock.h"
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

static int setting = -1;        /* display.movie_pacing, read on first use */
static int skip = -1;           /* display.skip_movies, read on first use */
static int64_t due;             /* host time the next picture is due, 0 before the first */

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

static void report_movie(void)
{
    print_movie("");
    shown.pictures = shown.held = shown.late = shown.skipped = shown.restarts = 0;
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
}

/* The decoder through the gate: thiscall on the movie (ecx), no stack arguments, its answer in eax. */
static void decode_when_due(void)
{
    recomp_func_t decoder = recomp_lookup(MOVIE_DECODER);
    uint32_t movie = g_ecx, back = MEM32(g_esp);   /* the caller's return address */
    int64_t now = now_ns(), period = picture_ns();
    int gated = setting && !ngl_frame_clock_paced(), extra;

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
    if (gated && due && now < due - EARLY_NS) {
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
    decoder();                              /* pops the return address itself */
    if (g_eax != DECODER_PICTURE)
        return;                             /* nothing yet, or the end: try again next frame */
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
        shown.reported = now;
    }
}
