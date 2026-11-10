/* [acct2-bro] Big Red One port, phase 1: the logging behind NGL's stand-ins. See ngl_log.h. */
#include "ngl_log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <xmmintrin.h>      /* _mm_getcsr / _mm_setcsr */

#include "recomp_types.h"
#include "xbox_memory_layout.h"     /* xbox_HeapAlloc: guest memory */
#include "ngl_mesh.h"

#define FIRST_CALLS_SHOWN   3       /* level 1: calls of each function printed with their arguments */
#define SUMMARY_EVERY       300     /* level 1: frames between per-frame counts */
#define BLOCK_BYTES         4096
#define SCRATCH_BYTES       (1u << 20)

static int log_level = -1;
static uint64_t total_calls[NGL_ID_COUNT];
static uint32_t calls_since_summary[NGL_ID_COUNT];
static const char *names[NGL_ID_COUNT];
static uint64_t frames;
static uint32_t blocks[NGL_ID_COUNT];
static uint32_t scratch;

static void *host(uint32_t guest)
{
    return (void *)XBOX_PTR(guest);     /* the same translation MEM32 uses */
}

static int level(void)
{
    if (log_level < 0) {
        const char *v = getenv("BRO_NGL_LOG");
        log_level = v && *v ? atoi(v) : 1;
    }
    return log_level;
}

static void print_summary(void)
{
    fprintf(stderr, "[ngl] frame %llu: calls per frame over the last %d frames\n",
            (unsigned long long)frames, SUMMARY_EVERY);
    for (int i = 0; i < NGL_ID_COUNT; i++) {
        if (calls_since_summary[i])
            fprintf(stderr, "[ngl]   %8.1f  %s\n", calls_since_summary[i] / (double)SUMMARY_EVERY, names[i]);
        calls_since_summary[i] = 0;
    }
}

/* [acct2-bro] 2026-11-08: the host's SSE rounding follows the game's (owner 10:12: "in BRO I also noticed the NPC
 * randomly get wide or narrow"). Big Red One sets MXCSR to round toward zero at start-up (0x00234450: MXCSR |= 0x6000,
 * after _controlfp chop and 24-bit precision for the x87) and flush-to-zero in its threads (0x000E7002, 0x000E73E2:
 * |= 0x8000). Its fast sine/cosine (0x000110F0, behind AngleVectors 0x002A2AF0 and so every soldier's matrix) needs the
 * truncation: it reduces the angle with ((t - 1.5 * 2^23) + 1.5 * 2^23). The recompiled code keeps the game's MXCSR in
 * g_mxcsr (for its conversions) but does the float math itself under the host's, round to nearest, so for about half
 * of all angles the sine and cosine came out wrong: t237 found a soldier's yaw axes 0.645 long, and
 * results/acct2-bro-tools/sincos_check.py, worked in 32-bit floats from the game's constants, gives 0.667 at -86
 * degrees rounding to nearest and exact values at every angle rounding toward zero. Here the host thread's rounding,
 * flush-to-zero and denormals-are-zero bits follow the game's whenever they change, all exceptions staying masked;
 * checked on every NGL call (one compare).
 *
 * Off unless BRO_HOST_MXCSR=1 (2026-11-08 12:45, for 2.38.1): with it on, all of the port's float math rounds toward
 * zero, and each frame trims the player's position toward 0, so a player standing still creeps at high frame rates
 * (d243: 1.4 units in 68 s at 20 fps, 4.4 at 60, 45.8 uncapped). The planned fix keeps the truncation inside the
 * fast sine/cosine 0x000110F0 alone (a hand-written version; acct2-bro NOTES 2026-11-08 12:15-12:45). */
#define MXCSR_FOLLOWED      0xE040u     /* RC (bits 13-14), FTZ (15), DAZ (6) */
#define MXCSR_ALL_MASKED    0x1F80u

static void follow_game_mxcsr(void)
{
    static _Thread_local uint32_t applied = 0xFFFFFFFFu;
    static int off = -1;
    uint32_t want;

    if (off < 0) {
        const char *v = getenv("BRO_HOST_MXCSR");

        off = !(v && *v == '1');   /* off unless BRO_HOST_MXCSR=1 */
    }
    if (off)
        return;
    want = g_mxcsr & MXCSR_FOLLOWED;
    if (want == applied)
        return;
    applied = want;
    _mm_setcsr((_mm_getcsr() & ~MXCSR_FOLLOWED) | MXCSR_ALL_MASKED | want);
    fprintf(stderr, "[ngl] the host's SSE rounding follows the game's MXCSR 0x%08X: rounding %s%s%s (host MXCSR now "
            "0x%08X)\n", g_mxcsr, (const char *[]){ "to nearest", "down", "up", "toward zero" }[(want >> 13) & 3],
            want & 0x8000u ? ", flush to zero" : "", want & 0x40u ? ", denormals are zero" : "", _mm_getcsr());
}

void ngl_log_call(int id, const char *name, int nargs,
                  uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3, uint32_t a4, uint32_t a5)
{
    follow_game_mxcsr();
    if (id == NGL_NGLLISTADDMESH)
        ngl_mesh_list_add(a0, a1, a2, a3);     /* kept for the frame's 3D drawing (ngl_mesh.c) */
    if (id < 0 || id >= NGL_ID_COUNT || level() == 0)
        return;
    names[id] = name;
    total_calls[id]++;
    calls_since_summary[id]++;

    if (level() >= 2 || total_calls[id] <= FIRST_CALLS_SHOWN) {
        const uint32_t a[6] = {a0, a1, a2, a3, a4, a5};
        fprintf(stderr, "[ngl] %s(", name);
        for (int i = 0; i < nargs && i < 6; i++)
            fprintf(stderr, "%s0x%08X", i ? ", " : "", a[i]);
        fprintf(stderr, ")%s\n", total_calls[id] == FIRST_CALLS_SHOWN && level() < 2 ? "  (no more shown)" : "");
    }

    if (id == NGL_NGLENDFRAME) {
        frames++;
        if (frames % SUMMARY_EVERY == 0)
            print_summary();
    }
}

uint32_t ngl_stand_in_block(int id)
{
    if (id < 0 || id >= NGL_ID_COUNT)
        return 0;
    if (!blocks[id]) {
        blocks[id] = xbox_HeapAlloc(BLOCK_BYTES, 16);
        if (blocks[id])
            memset(host(blocks[id]), 0, BLOCK_BYTES);
        else
            fprintf(stderr, "[ngl] no guest memory for %s's stand-in block\n", names[id] ? names[id] : "?");
    }
    return blocks[id];
}

uint32_t ngl_stand_in_scratch(void)
{
    if (!scratch) {
        scratch = xbox_HeapAlloc(SCRATCH_BYTES, 4096);
        if (scratch)
            memset(host(scratch), 0, SCRATCH_BYTES);
        else
            fprintf(stderr, "[ngl] no guest memory for the Lock scratch buffer\n");
    }
    return scratch;
}
