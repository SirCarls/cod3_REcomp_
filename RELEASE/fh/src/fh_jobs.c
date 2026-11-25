/* [waw-decomp/fh11] 2026-11-24: the game's job thread rests while it has nothing to do.
 *
 * What the game does (Europe v3 default.xbe; read from our recompiled C, gen3/recomp_0007.c, and the XBE):
 *   - The thread started at 0x00068D90 (the level loader's, fh/config/discs.tsv) takes its work from a queue of one:
 *     the pointer at 0x00348168, with the job's count at +0x48 and its kind at +0x40 (0x00068DC0..0x00068DE5, a
 *     switch of 8 kinds).
 *   - With no job (the pointer 0, or its +0x48 0) it jumps straight to 0x000691CF, then to 0x000691DB, whose `je`
 *     goes back to 0x00068DC0 while 0x00348174 holds 1. That path makes no kernel call: on the Xbox the timer's tick
 *     shared the one core by time slices, so the loop only cost the loader's slices. Here the thread is a host
 *     thread, and the loop kept one core at 100% for the whole game (top -H on the r11 runs: the second guest
 *     thread at 99.9% on Linux and on Wine). Under the guest CPU lock (RECOMP_GUEST_LOCK=1) it also holds the
 *     guest CPU until another thread has waited a whole quantum, so the main thread waits on it.
 *
 * What this does: the recompiled C's jump back at 0x000691DB already has its mark, RECOMP_BACK_EDGE(0x000691DBu)
 * (fh_gen3.sh checks it is there). fh/config/back_edge_hooks.tsv names fh_edge_000691DB for it, and
 * tools/fh_decomp/fh_wrap.py puts a call to it just before that mark in the copy of the file it compiles (the
 * recompiler's output is not changed). When the queue is empty the call lets the guest CPU go, sleeps a millisecond
 * and takes it back; with a job waiting it returns at once, so a job queued waits a millisecond at most and a run of
 * jobs is not slowed. FH_JOB_REST=0 leaves the loop spinning, as the game wrote it.
 *
 * The idea comes from FinestHourRemastered's notes (information only: nothing of its code is used; each address
 * above read again in our own decompile). */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "recomp_types.h"           /* MEM32 */
#include "kernel.h"                 /* xbox_GuestCpuRelease / xbox_GuestCpuAcquire; on Windows also Sleep */

#if !defined(_WIN32)
#include <errno.h>
#include <time.h>
#endif

#define JOB_QUEUE                   0x00348168u     /* the job waiting, or 0 */
#define JOB_COUNT                   0x48u           /* in the job: 0 when there is nothing to do */
#define ORDINAL_KE_DELAY_EXECUTION  99u             /* the kernel call the rest stands for, for the guest CPU */

void fh_edge_000691DB(void);

static int rest_on(void)
{
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("FH_JOB_REST");
        v = !(e && *e == '0');
        fprintf(stderr, "[fh] jobs: the job thread %s\n",
                v ? "sleeps 1 ms when its queue is empty" : "spins on its empty queue (FH_JOB_REST=0)");
    }
    return v;
}

static void sleep_1ms(void)
{
#if defined(_WIN32)
    Sleep(1);
#else
    struct timespec pause = { 0, 1000000l };
    while (nanosleep(&pause, &pause) != 0 && errno == EINTR)
        ;   /* a profiler's signal ends it early: sleep the rest */
#endif
}

void fh_edge_000691DB(void)
{
    static unsigned long rests;
    const uint32_t job = MEM32(JOB_QUEUE);

    if (job && MEM32(job + JOB_COUNT))
        return;                     /* a job waits: take it at once */
    if (!rest_on())
        return;
    int held = xbox_GuestCpuRelease(XBOX_GUEST_CPU_KERNEL, ORDINAL_KE_DELAY_EXECUTION);
    sleep_1ms();
    if (held)
        xbox_GuestCpuAcquire();
    if (++rests == 1)
        fprintf(stderr, "[fh] jobs: first rest of the job thread on its empty queue (0x000691DB)\n");
}
