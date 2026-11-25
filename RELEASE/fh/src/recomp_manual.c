/* [acct2-bro] Finest Hour port: the hooks the recompiled code calls.
 *
 * recomp_lookup_manual() is asked first on every call to a function listed in fh/config/stand_in_functions.tsv (the
 * recompiler is run with that list as --manual-functions, so those calls always come here) and on every indirect
 * call. It hands back the stand-in for the game's Direct3D and DirectSound functions (fh_stand_ins.c, generated);
 * anything else goes on to the generated dispatch table. [acct2-fh] 2026-11-15: first, the kernel's
 * HalReturnToFirmware, which a level change reboots through (src/fh_relaunch.c: one comparison on other calls).
 * [acct2-fh] 11-15: then the console's take-a-hit watch (hitcalls, src/fh_game_vars.c: one comparison on other calls;
 * while it is on, the player's 0x000C3620 comes back noted, and runs as the game's own).
 *
 * The other three report trouble the generated code runs into: an indirect call it can't resolve, a call into
 * something that isn't code, and an instruction the recompiler couldn't translate. They are worded as Big Red One's
 * (bro/src/recomp_manual.c), so the same tools read both ports' logs. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "recomp_types.h"
#include "xbox_memory_layout.h"     /* XBOX_PTR: guest memory */
#include "fh_relaunch.h"
#include "fh_game_vars.h"           /* fh_game_hit_watch_lookup */

recomp_func_t fh_stand_in_for(uint32_t xbox_va);       /* generated: fh_stand_ins.c */

recomp_func_t recomp_lookup_manual(uint32_t xbox_va)
{
    recomp_func_t reboot = fh_relaunch_lookup(xbox_va), watched;

    if (reboot)
        return reboot;
    watched = fh_game_hit_watch_lookup(xbox_va);
    return watched ? watched : fh_stand_in_for(xbox_va);
}

/* Worded as xboxrecomp words it ("Failed to resolve VA 0x..."), so its tools.seed_from_log can read a run's log and
 * seed the missing function for the next disassembly. */
void recomp_icall_fail_log(uint32_t va)
{
    /* [esp] is the return address of a call, or the return address of the function a tail jump leaves */
    fprintf(stderr, "[icall] Failed to resolve VA 0x%08X (call %llu; [esp] 0x%08X, eax 0x%08X ecx 0x%08X edx 0x%08X); "
            "last targets:", va, (unsigned long long)g_icall_count, MEM32(g_esp), g_eax, g_ecx, g_edx);
    for (int i = 0; i < 16; i++) {
        uint32_t t = g_icall_trace[(g_icall_trace_idx - 16 + i) & 15];
        if (t)
            fprintf(stderr, " %08X", t);
    }
    fprintf(stderr, "\n");
}

/* A call through a null or wild pointer is skipped. Reported the 1st, 10th, 100th, ... time per address, so a skip
 * inside a loop shows up as a loop. The return address on the stack says which call it was. */
void recomp_icall_not_code_log(uint32_t va)
{
    enum { SLOTS = 32 };
    static uint32_t seen[SLOTS];
    static uint64_t hits[SLOTS];
    static int used;
    uint64_t n;
    int i;

    for (i = 0; i < used && seen[i] != va; i++)
        ;
    if (i == used) {
        if (used == SLOTS)
            return;
        seen[used++] = va;
    }
    n = ++hits[i];
    while (n >= 10 && n % 10 == 0)
        n /= 10;
    if (n == 1)
        fprintf(stderr, "[icall] 0x%08X is not code: skipped %llu time(s); [esp] 0x%08X, last targets %08X %08X %08X\n",
                va, (unsigned long long)hits[i], MEM32(g_esp), g_icall_trace[(g_icall_trace_idx - 3) & 15],
                g_icall_trace[(g_icall_trace_idx - 2) & 15], g_icall_trace[(g_icall_trace_idx - 1) & 15]);
}

/* An instruction the recompiler left untranslated was reached. It does nothing; RECOMP_UNIMPL_TRAP=1 stops here. */
void recomp_unimpl(const char *text, uint32_t va)
{
    static int shown;
    const char *trap = getenv("RECOMP_UNIMPL_TRAP");
    int stop = trap && *trap && *trap != '0';

    if (shown < 50 || stop) {
        shown++;
        fprintf(stderr, "[unimpl] `%s` at 0x%08X\n", text, va);
    }
    if (stop)
        abort();
}
