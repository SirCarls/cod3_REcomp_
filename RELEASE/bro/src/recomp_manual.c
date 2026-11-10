/* Big Red One port: the hooks the recompiled code calls.
 *
 * recomp_lookup_manual() is asked first on every call to a function listed in bro/config/stand_in_functions.tsv
 * (the recompiler is run with that list as --manual-functions, so those calls always come here) and on every
 * indirect call. It hands back the stand-in for NGL's functions and for the Direct3D and DirectSound functions
 * called from outside NGL, and the native version of a game function the disassembly never found (reached only
 * through a pointer); anything else goes on to the generated dispatch table.
 *
 * The other three report trouble the generated code runs into: an indirect call it can't resolve, a call into
 * something that isn't code, and an instruction the recompiler couldn't translate. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "recomp_types.h"
#include "xbox_memory_layout.h"     /* XBOX_PTR: guest memory */
#include "bro_media.h"
#include "ngl_movie.h"

recomp_func_t ngl_stand_in_for(uint32_t xbox_va);      /* generated: stand_ins.c */

/* ScrCall_ScrIface_atof 0x0011DA40, called only through the script interface's function table, so the disassembly
 * never found it: atof (through pScr_ScrIface_atof 0x009229DC, the C runtime's) on its argument, a string, with the
 * result stored as a float and handed back in eax. First reached loading the first mission (D:\sp\prologue). */
static void scr_iface_atof(void)
{
    uint32_t text = MEM32(g_esp + 4);
    float value = text ? (float)atof((const char *)XBOX_PTR(text)) : 0.0f;
    uint32_t bits;

    memcpy(&bits, &value, sizeof bits);
    g_eax = bits;
    g_esp += 4;     /* ret: the caller pops the argument */
}

/* 0x001F6ABC, a piece of the C runtime's x87 special-case code: __trandisp2 (0x001F6A1F) jumps to it through a math
 * function's result table (the entry at 0x008FF44C) to drop both operands and return 0.0: fstp st(0); fstp st(0);
 * fldz; ret. The only target of those tables the disassembly missed (the others, 0x001F6AF5, 0x001F6AF7, 0x001F6B1F
 * and 0x001F6B5E, are lifted). */
static void crt_trandisp_zero(void)
{
    g_fp_top = (int)(((unsigned)g_fp_top + 2u) & 7u);     /* pop, pop */
    g_fp_top = (int)(((unsigned)g_fp_top + 7u) & 7u);     /* push 0.0 */
    g_fp_stack[g_fp_top] = 0.0;
    g_esp += 4;     /* ret */
}

/* Game functions done natively until a reseeded disassembly lifts them. */
static recomp_func_t unlifted_function(uint32_t xbox_va)
{
    switch (xbox_va) {
    case 0x0011DA40u: return scr_iface_atof;
    case 0x001F6ABCu: return crt_trandisp_zero;
    default: return 0;
    }
}

recomp_func_t recomp_lookup_manual(uint32_t xbox_va)
{
    recomp_func_t media = bro_media_method_for(xbox_va);    /* the native media objects' vtables */
    recomp_func_t movie = ngl_movie_method_for(xbox_va);    /* [acct2-bro-game] the movie decoder's gate */
    recomp_func_t stand_in;

    if (media)
        return media;
    if (movie)
        return movie;
    stand_in = ngl_stand_in_for(xbox_va);
    return stand_in ? stand_in : unlifted_function(xbox_va);
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

/* A call through a null or wild pointer is skipped. Reported the 1st, 10th, 100th, ... time per address, so a
 * skip inside a loop shows up as a loop. The return address on the stack says which call it was. */
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
        /* [esp] is the return address when the call pushes one first; the last targets show where it came from */
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
