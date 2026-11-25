/* [acct2-bro] Finest Hour port, phase 1: the logging behind the stand-ins. See fh_log.h. */
#include "fh_log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "recomp_types.h"
#include "xbox_memory_layout.h"     /* xbox_HeapAlloc: guest memory */
#include "fh_d3d_state.h"           /* [waw-decomp] fh_d3d_census_active */

#define FIRST_CALLS_SHOWN   3       /* level 1: calls of each function printed with their arguments */
#define SUMMARY_SECONDS     10      /* level 1: seconds between call counts */
#define BLOCK_BYTES         4096
#define SCRATCH_BYTES       (1u << 20)

static int log_level = -1;
static uint64_t total_calls[FH_ID_COUNT];
static uint32_t calls_since_summary[FH_ID_COUNT];
static const char *names[FH_ID_COUNT];
static uint32_t blocks[FH_ID_COUNT];
static uint32_t scratch;
static double last_summary = -1.0;

static int level(void)
{
    if (log_level < 0) {
        const char *v = getenv("FH_LOG");
        log_level = v && *v ? atoi(v) : 1;
    }
    return log_level;
}

static double seconds_now(void)
{
    struct timespec ts;

    if (timespec_get(&ts, TIME_UTC) != TIME_UTC)
        return 0.0;
    return (double)ts.tv_sec + ts.tv_nsec / 1e9;
}

static void print_summary(double span)
{
    fprintf(stderr, "[fh] calls in the last %.0f s:\n", span);
    for (int i = 0; i < FH_ID_COUNT; i++) {
        if (calls_since_summary[i])
            fprintf(stderr, "[fh]   %8u  %s\n", calls_since_summary[i], names[i] ? names[i] : "?");
        calls_since_summary[i] = 0;
    }
}

/* [waw-decomp] The stand-ins whose native answers (fh_d3d_state.c, fh_present.c) print their own census lines. */
static int census_describes(int id)
{
    switch (id) {
    case FH_D3D__D3DDEVICE_SETSTREAMSOURCE:
    case FH_D3D__D3DDEVICE_SETVERTEXSHADER:
    case FH_D3D__D3DDEVICE_SETTEXTURE:
    case FH_D3D__D3DDEVICE_SETPALETTE:
    case FH_D3D__D3DDEVICE_SETRENDERSTATE:
    case FH_D3D__D3DDEVICE_SETTEXTURESTAGESTATE:
    case FH_D3D__D3DDEVICE_SETTRANSFORM:
    case FH_D3D__D3DDEVICE_SETVIEWPORT:
    case FH_D3D__D3DDEVICE_DRAWINDEXEDVERTICES:
    case FH_D3D__D3DDEVICE_SWAP:
        return 1;
    default:
        return 0;
    }
}

void fh_log_call(int id, const char *name, int nargs,
                 uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3, uint32_t a4, uint32_t a5)
{
    double now;

    if (id < 0 || id >= FH_ID_COUNT || level() == 0)
        return;
    names[id] = name;
    total_calls[id]++;
    calls_since_summary[id]++;

    /* [waw-decomp] 2026-11-09: in a draw census frame (FH_DRAW_LOG, fh_d3d_state.c) every call is shown, among the
     * census's own lines, except the ones the census describes itself */
    if (fh_d3d_census_active()) {
        if (!census_describes(id)) {
            const uint32_t a[6] = {a0, a1, a2, a3, a4, a5};

            fprintf(stderr, "[draws] (stand-in) %s(", name);
            for (int i = 0; i < nargs && i < 6; i++)
                fprintf(stderr, "%s0x%08X", i ? ", " : "", a[i]);
            fprintf(stderr, ")  [ret to 0x%08X]\n", MEM32(g_esp));
        }
    } else if (level() >= 2 || total_calls[id] <= FIRST_CALLS_SHOWN) {
        const uint32_t a[6] = {a0, a1, a2, a3, a4, a5};

        fprintf(stderr, "[fh] %s(", name);
        for (int i = 0; i < nargs && i < 6; i++)
            fprintf(stderr, "%s0x%08X", i ? ", " : "", a[i]);
        fprintf(stderr, ")  [ret to 0x%08X]%s\n", MEM32(g_esp),
                total_calls[id] == FIRST_CALLS_SHOWN && level() < 2 ? "  (no more shown)" : "");
    }

    now = seconds_now();
    if (last_summary < 0.0)
        last_summary = now;
    else if (now - last_summary >= SUMMARY_SECONDS) {
        print_summary(now - last_summary);
        last_summary = now;
    }
}

uint32_t fh_stand_in_block(int id)
{
    if (id < 0 || id >= FH_ID_COUNT)
        return 0;
    if (!blocks[id]) {
        blocks[id] = xbox_HeapAlloc(BLOCK_BYTES, 16);
        if (blocks[id])
            memset((void *)XBOX_PTR(blocks[id]), 0, BLOCK_BYTES);
        else
            fprintf(stderr, "[fh] no guest memory for %s's stand-in block\n", names[id] ? names[id] : "?");
    }
    return blocks[id];
}

uint32_t fh_stand_in_scratch(void)
{
    if (!scratch) {
        scratch = xbox_HeapAlloc(SCRATCH_BYTES, 4096);
        if (scratch)
            memset((void *)XBOX_PTR(scratch), 0, SCRATCH_BYTES);
        else
            fprintf(stderr, "[fh] no guest memory for the Lock scratch buffer\n");
    }
    return scratch;
}

#define RW_ERRORS_TRACKED   64
/* From RwErrorSet's own code (0x001CFDE0): the engine's error slot is RwEngineInstance (0x00407DC0) plus the error
 * module's offset kept at 0x003F0F5C; "no error" is { 0, 0x80000000 }. */
#define RW_ENGINE_INSTANCE  0x00407DC0u
#define RW_ERROR_OFFSET_AT  0x003F0F5Cu
#define RW_NO_ERROR_CODE    0x80000000u

uint32_t fh_rw_error_set(uint32_t error)
{
    static struct { uint32_t plugin, code, caller, count; } seen[RW_ERRORS_TRACKED];
    static int n_seen;
    const uint32_t caller = MEM32(g_esp);   /* the stand-in has not popped the return address yet */
    const uint32_t slot = RW_ENGINE_INSTANCE + MEM32(RW_ERROR_OFFSET_AT);
    const uint32_t plugin = error ? MEM32(error) : 0, code = error ? MEM32(error + 4) : 0;

    if (level() > 0) {
        int i;

        for (i = 0; i < n_seen; i++)
            if (seen[i].plugin == plugin && seen[i].code == code && seen[i].caller == caller)
                break;
        if (i == n_seen && n_seen < RW_ERRORS_TRACKED) {
            seen[n_seen].plugin = plugin;
            seen[n_seen].code = code;
            seen[n_seen].caller = caller;
            seen[n_seen].count = 0;
            n_seen++;
        }
        if (i < n_seen) {
            uint32_t c = ++seen[i].count;

            while (c >= 10 && c % 10 == 0)
                c /= 10;
            if (c == 1)     /* 1, 10, 100, ... */
                fprintf(stderr, "[fh] RenderWare error: plugin 0x%X, code 0x%08X, from 0x%08X (%u time%s)\n", plugin,
                        code, caller, seen[i].count, seen[i].count == 1 ? "" : "s");
        }
    }
    /* What the game's RwErrorSet does: record the error only when none is pending. An error code with bit 31 set is
     * RenderWare's own and is recorded with plugin 0. */
    if (error && MEM32(slot) == 0 && MEM32(slot + 4) == RW_NO_ERROR_CODE) {
        MEM32(slot) = (code & RW_NO_ERROR_CODE) ? 0 : plugin;
        MEM32(slot + 4) = code;
    }
    return error;
}
