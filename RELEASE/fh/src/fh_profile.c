/* [acct2-fh] 2026-11-13 Finest Hour port: where the game's main thread spends its frames (a sampling profiler).
 *
 * The timing line (fh_draw.c, "[fh] draw time a frame") covers the draw code only. In acct2-fh-boot22d's busiest
 * stretch it was about 7 ms of a frame of about 20 ms (49 frames a second); this file finds where the rest goes.
 *
 * With FH_PROFILE set, the thread that calls Swap (the game's main thread: its logic, its drawing, its Swap) is
 * stopped about every millisecond of wall-clock time by a signal (SIGPROF, from a POSIX timer aimed at that thread
 * alone), and the handler writes down where the thread was: the interrupted instruction and the return addresses
 * above it, up to DEPTH in all (glibc's backtrace(), which unwinds through the signal frame). Wall-clock time, not CPU
 * time, so waiting counts too: the frame pacing's sleep (fh_frame_pace.c), the wait for the guest CPU
 * (RECOMP_GUEST_LOCK=1: another guest thread is running guest code) and the Vulkan driver's waits each show as the
 * call they wait in.
 *
 * The handler fills a ring; Swap empties it (fh_profile_frame, a second of samples at a time) into the file FH_PROFILE
 * names, as text:
 *     # ...                                   what the file is: the rate, the process, the program, its load address
 *     m <a line of /proc/self/maps>           written once at the start: which library an address is in
 *     s <ms> <swap> <address> <address> ...   one sample: ms since sampling began, the Swap count (frames so far),
 *                                             then the addresses, innermost first
 * An address inside fh is written f+<hex offset from fh's load address>, which is the address `nm -n build/fh` gives
 * (fh is position independent: its link addresses start at 0); the recompiled game functions are sub_<guest address>.
 * Any other address is written as plain hex, and the "m" lines say what is mapped there.
 * dev/agents/acct2-fh/tools/fh_profile.py reads the file and prints the totals: by kind of time (game code, draw code,
 * vertex programs, pacing, guest CPU wait, ...), by function, and by game function.
 *
 * Costs and limits:
 *   - about 10-20 microseconds a sample (a backtrace of up to 16 frames), 1-2% of the thread's time at 1000 a second;
 *   - backtrace() is called once before the timer starts, because its first call loads libgcc_s (which allocates).
 *     The handler takes no lock of its own, but glibc's unwinder may take the dynamic loader's: a sample that lands
 *     while the thread itself is inside dlopen or dl_iterate_phdr could hang it. Measuring runs only; off by default;
 *   - a sleep the signal interrupts ends early. The frame pacing sleeps again for the rest (fh_frame_pace.c), but a
 *     guest Sleep (xboxrecomp's KeDelayExecutionThread on this thread) may come back sooner than asked;
 *   - the ring holds RING samples (32 s at 1000 a second); samples that find it full are counted and dropped, and the
 *     count is written at each write;
 *   - Linux x86-64 only; elsewhere FH_PROFILE prints a line and does nothing.
 *
 * Settings (environment):
 *   FH_PROFILE=<file>      sample the main thread into this file (made anew)
 *   FH_PROFILE_HZ=<n>      samples a second, 50 to 10000 (default 1000) */
#include "fh_profile.h"

#if defined(__linux__) && defined(__x86_64__)
#define _GNU_SOURCE
#include <errno.h>
#include <execinfo.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <time.h>
#include <ucontext.h>
#include <unistd.h>

#define DEPTH               16u             /* addresses kept a sample, the interrupted one first */
#define RING                32768u          /* samples the ring holds (a power of two) */
#define DEFAULT_HZ          1000
#define WRITE_EVERY_NS      1000000000ll    /* Swap writes the ring out once a second */
#define NS_PER_SECOND       1000000000ll

struct sample {
    uint32_t ms;                /* since sampling began */
    uint32_t swap;              /* frames (Swaps) so far */
    uint32_t depth;             /* addresses in pc[] */
    uintptr_t pc[DEPTH];        /* the interrupted instruction, then the return addresses above it */
};

/* The ring is filled by the handler and emptied by Swap, both on the game's main thread (the timer signals that
 * thread alone), so the handler only ever interrupts the emptying: plain volatile indices with signal fences are
 * enough. head: the next slot the handler fills; tail: the next one Swap writes out. */
static struct sample ring[RING];
static volatile uint32_t head, tail;
static volatile uint32_t dropped;       /* samples that found the ring full */
static volatile uint32_t swaps;         /* fh_profile_frame calls */

static int state = -1;                  /* -1: FH_PROFILE not read yet; 0: off; 1: sampling */
static FILE *out;
static timer_t timer;
static int64_t start_ns, last_write_ns;
static uintptr_t fh_lo, fh_hi;          /* where fh itself is mapped */
static atomic_flag writing = ATOMIC_FLAG_INIT;  /* Swap's write and the one at exit never run together */

static int64_t now_ns(void)
{
    struct timespec t;

    clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec * NS_PER_SECOND + t.tv_nsec;
}

/* SIGPROF from the timer: where the main thread is now. */
static void on_sample(int sig, siginfo_t *info, void *context)
{
    const ucontext_t *uc = context;
    uintptr_t pc = (uintptr_t)uc->uc_mcontext.gregs[REG_RIP];
    void *trace[DEPTH + 4];
    uint32_t h = head, depth = 1;
    struct sample *s;
    int saved_errno = errno, n, i, from = -1;

    (void)sig;
    (void)info;
    if (h - tail >= RING) {
        dropped++;
        return;
    }
    s = &ring[h % RING];
    s->ms = (uint32_t)((now_ns() - start_ns) / 1000000);
    s->swap = swaps;
    s->pc[0] = pc;
    /* backtrace's list: this handler, the signal return trampoline, then the interrupted function (at pc itself) and
     * the functions above it. What follows pc is kept; if pc is not in the list, the unwinder did not get through
     * the signal frame and only pc is kept. */
    n = backtrace(trace, (int)(DEPTH + 4));
    for (i = 0; i < n; i++)
        if ((uintptr_t)trace[i] == pc) {
            from = i + 1;
            break;
        }
    if (from > 0)
        for (i = from; i < n && depth < DEPTH; i++)
            s->pc[depth++] = (uintptr_t)trace[i];
    s->depth = depth;
    atomic_signal_fence(memory_order_seq_cst);  /* the sample is whole before Swap can see it */
    head = h + 1;
    errno = saved_errno;
}

/* Writes the samples taken since the last write, then the dropped count. The caller holds `writing`. */
static void write_held(void)
{
    uint32_t t, h, i;

    h = head;
    atomic_signal_fence(memory_order_seq_cst);
    for (t = tail; t != h; t++) {
        const struct sample *s = &ring[t % RING];

        fprintf(out, "s %u %u", s->ms, s->swap);
        for (i = 0; i < s->depth && i < DEPTH; i++) {
            uintptr_t a = s->pc[i];

            if (a >= fh_lo && a < fh_hi)
                fprintf(out, " f+%lx", (unsigned long)(a - fh_lo));
            else
                fprintf(out, " %lx", (unsigned long)a);
        }
        fputc('\n', out);
    }
    atomic_signal_fence(memory_order_seq_cst);
    tail = h;                   /* the slots are free for the handler again */
    if (dropped)
        fprintf(out, "# dropped %u samples so far (the ring was full)\n", dropped);
    fflush(out);
}

/* Swap's write, a second's samples at a time; skipped if the write at exit has begun. */
static void write_samples(void)
{
    if (atomic_flag_test_and_set(&writing))
        return;
    write_held();
    atomic_flag_clear(&writing);
}

/* At exit (the watchdog's exit, a window closed), on whichever thread called exit: the timer stopped and the last
 * samples written. `writing` is kept, so a Swap still running on the main thread writes nothing more to the closed
 * file; if Swap is writing just then, this gives way and the last second is lost. */
static void write_at_exit(void)
{
    if (state != 1)
        return;
    timer_delete(timer);
    if (atomic_flag_test_and_set(&writing))
        return;
    state = 0;
    write_held();
    fprintf(out, "# end: %u frames\n", swaps);
    fclose(out);
}

/* Copies /proc/self/maps into the file as "m" lines and finds where fh itself is mapped. */
static void write_maps(void)
{
    char exe[1024], line[2048];
    ssize_t len = readlink("/proc/self/exe", exe, sizeof exe - 1);
    FILE *maps = fopen("/proc/self/maps", "r");

    exe[len > 0 ? len : 0] = '\0';
    if (!maps)
        return;
    while (fgets(line, sizeof line, maps)) {
        unsigned long lo, hi;
        const char *path = strchr(line, '/');

        fprintf(out, "m %s", line);
        if (exe[0] && path && strncmp(path, exe, strlen(exe)) == 0 && path[strlen(exe)] == '\n'
            && sscanf(line, "%lx-%lx", &lo, &hi) == 2) {
            if (!fh_lo || lo < fh_lo)
                fh_lo = lo;
            if (hi > fh_hi)
                fh_hi = hi;
        }
    }
    fclose(maps);
    fprintf(out, "# fh (%s) is mapped at %lx-%lx; f+ offsets are from %lx\n", exe, (unsigned long)fh_lo,
            (unsigned long)fh_hi, (unsigned long)fh_lo);
}

/* Reads FH_PROFILE and, when it names a file, starts sampling the calling thread. 1: sampling; 0: off. */
static int start(void)
{
    const char *path = getenv("FH_PROFILE"), *rate = getenv("FH_PROFILE_HZ");
    long hz = rate ? strtol(rate, NULL, 10) : DEFAULT_HZ;
    struct sigaction action;
    struct sigevent event;
    struct itimerspec every;
    void *prime[4];
    int64_t period;

    if (!path || !path[0])
        return 0;
    if (hz < 50 || hz > 10000)
        hz = DEFAULT_HZ;
    period = NS_PER_SECOND / hz;
    out = fopen(path, "w");
    if (!out) {
        fprintf(stderr, "[fh] FH_PROFILE: cannot write %s (%s); not sampling\n", path, strerror(errno));
        return 0;
    }
    fprintf(out, "# fh profile: the game's main thread (process %ld, thread %ld), sampled every %lld ns of "
            "wall-clock time; lines: m <maps>, s <ms> <frames so far> <addresses, innermost first>\n",
            (long)getpid(), (long)syscall(SYS_gettid), (long long)period);
    write_maps();
    fflush(out);

    backtrace(prime, 4);        /* loads libgcc_s now, not in the handler */

    memset(&action, 0, sizeof action);
    action.sa_sigaction = on_sample;
    action.sa_flags = SA_SIGINFO | SA_RESTART;
    sigemptyset(&action.sa_mask);
    sigaction(SIGPROF, &action, NULL);

    memset(&event, 0, sizeof event);
    event.sigev_notify = SIGEV_THREAD_ID;
    event.sigev_signo = SIGPROF;
#if defined(sigev_notify_thread_id)
    event.sigev_notify_thread_id = (pid_t)syscall(SYS_gettid);
#else
    event._sigev_un._tid = (pid_t)syscall(SYS_gettid);
#endif
    if (timer_create(CLOCK_MONOTONIC, &event, &timer) != 0) {
        fprintf(stderr, "[fh] FH_PROFILE: no timer (%s); not sampling\n", strerror(errno));
        fclose(out);
        return 0;
    }
    start_ns = last_write_ns = now_ns();
    every.it_interval.tv_sec = every.it_value.tv_sec = 0;
    every.it_interval.tv_nsec = every.it_value.tv_nsec = (long)period;
    timer_settime(timer, 0, &every, NULL);
    atexit(write_at_exit);
    fprintf(stderr, "[fh] FH_PROFILE: sampling the main thread %ld times a second into %s\n", hz, path);
    return 1;
}

void fh_profile_frame(void)
{
    int64_t now;

    swaps++;
    if (state < 0)
        state = start();
    if (state != 1)
        return;
    now = now_ns();
    if (now - last_write_ns >= WRITE_EVERY_NS) {
        last_write_ns = now;
        write_samples();
    }
}

#else /* not Linux x86-64 */
#include <stdio.h>
#include <stdlib.h>

void fh_profile_frame(void)
{
    static int said;
    const char *path;

    if (said)
        return;
    said = 1;
    path = getenv("FH_PROFILE");
    if (path && path[0])
        fprintf(stderr, "[fh] FH_PROFILE: the profiler is for Linux x86-64 only; not sampling\n");
}
#endif
