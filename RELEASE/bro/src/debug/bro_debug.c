/* Big Red One port: crash reports and write guards, for finding what breaks a run.
 *
 * A crash (SIGSEGV, SIGBUS, SIGILL, SIGFPE) is reported before the program dies: what was touched (as a guest
 * address when it is guest memory), the host instruction as an offset into the program (`addr2line -f -e bro
 * OFFSET`, or nm), the guest registers, the last indirect-call targets, and a host backtrace. The default action
 * then runs, so a core is still written.
 *
 * Write guards make guest memory read-only, report each write that lands there, and let that write through:
 *   BRO_WRITE_GUARD=ADDR[,ADDR...]  the 4 KB page holding each guest address, in RAM and in each of its 28 mirrors
 *                                   (a write through a mirror changes the same bytes, so it is guarded too)
 *   BRO_GUARD_MIRRORS=1             all 28 RAM mirrors (guest 64 MB and up): nothing the game does should land there
 * A guarded page reports its first write and is then opened, so the run goes on as it would have. At most 64
 * reports. Guards are set once the game's image is loaded, before it starts.
 *
 * Where the game is, for a run that hangs or stalls:
 *   BRO_WHERE=SECONDS[,SECONDS...]  at each time (seconds since the start), the game's thread (the one that called
 *                                   bro_debug_init) stops for a moment and reports what a crash would: the host
 *                                   instruction and backtrace (`nm -n bro` names the recompiled functions, sub_<guest
 *                                   address>), the guest registers, the stack top and the last indirect calls
 * BRO_FAULT_REPORT=0 turns all of this off.
 *
 * Linux x86-64 only; elsewhere bro_debug_init() does nothing. */
#if defined(__linux__) && defined(__x86_64__)
#define _GNU_SOURCE
#include <dlfcn.h>
#include <execinfo.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <ucontext.h>
#include <unistd.h>

#include "recomp_types.h"
#include "xbox_memory_layout.h"
#include "bro_debug.h"

enum { MAX_GUARDS = 256, MAX_REPORTS = 64, PAGE = 4096, MIRRORS = 28, MAX_WHERE = 32 };

struct guard {
    uintptr_t lo, hi;   /* host addresses */
};

static struct guard s_guards[MAX_GUARDS];
static int s_guard_count;
static volatile sig_atomic_t s_reports;
static volatile sig_atomic_t s_crashing;
static struct sigaction s_previous[32];
static uintptr_t s_ram_size;
static pthread_t s_game_thread;
static double s_where_at[MAX_WHERE];
static int s_where_count;
static struct timespec s_start;

static void say(const char *format, ...)
{
    char line[512];
    va_list args;
    int n;

    va_start(args, format);
    n = vsnprintf(line, sizeof line, format, args);
    va_end(args);
    if (n > (int)sizeof line - 1)
        n = (int)sizeof line - 1;
    if (n > 0 && write(2, line, (size_t)n) < 0)
        return;
}

/* A host address as a guest address, when it is one (0 otherwise). */
static uint32_t guest_of(uintptr_t host)
{
    uintptr_t base = (uintptr_t)g_xbox_mem_offset;

    if (host >= base && host - base < 0x100000000ull)
        return (uint32_t)(host - base);
    return 0;
}

static void say_host(const char *what, uintptr_t pc)
{
    Dl_info info;

    if (dladdr((void *)pc, &info) && info.dli_fbase) {
        const char *file = info.dli_fname ? strrchr(info.dli_fname, '/') : NULL;
        say("%s %s+0x%lX", what, file ? file + 1 : (info.dli_fname ? info.dli_fname : "?"),
            (unsigned long)(pc - (uintptr_t)info.dli_fbase));
        if (info.dli_sname)
            say(" (%s+0x%lX)", info.dli_sname, (unsigned long)(pc - (uintptr_t)info.dli_saddr));
    } else {
        say("%s %p", what, (void *)pc);
    }
}

static void say_guest(void)
{
    say("  guest: eax %08X ecx %08X edx %08X ebx %08X esi %08X edi %08X esp %08X ebp %08X", g_eax, g_ecx, g_edx,
        g_ebx, g_esi, g_edi, g_esp, g_ebp);
    if (g_esp >= 0x10000 && g_esp < s_ram_size - 16)
        say("; [esp] %08X %08X %08X %08X", MEM32(g_esp), MEM32(g_esp + 4), MEM32(g_esp + 8), MEM32(g_esp + 12));
    say("\n  indirect call %llu, last targets:", (unsigned long long)g_icall_count);
    for (int i = 0; i < 8; i++)
        say(" %08X", g_icall_trace[(g_icall_trace_idx - 8 + i) & (ICALL_TRACE_SIZE - 1)]);
    say("\n");
}

static void say_backtrace(void)
{
    void *frames[24];
    int n = backtrace(frames, 24);

    say("  host backtrace (offsets into the program):\n");
    for (int i = 0; i < n; i++) {
        say("   ");
        say_host("", (uintptr_t)frames[i]);
        say("\n");
    }
}

static int find_guard(uintptr_t host)
{
    for (int i = 0; i < s_guard_count; i++)
        if (host >= s_guards[i].lo && host < s_guards[i].hi)
            return i;
    return -1;
}

static void on_fault(int sig, siginfo_t *info, void *context)
{
    ucontext_t *uc = context;
    uintptr_t touched = (uintptr_t)info->si_addr;
    uintptr_t pc = (uintptr_t)uc->uc_mcontext.gregs[REG_RIP];
    int wrote = (uc->uc_mcontext.gregs[REG_ERR] & 2) != 0;
    int guard = sig == SIGSEGV ? find_guard(touched) : -1;
    uint32_t guest = guest_of(touched);
    struct sigaction *previous = &s_previous[sig & 31];

    if (guard >= 0 && wrote) {
        uintptr_t page = touched & ~(uintptr_t)(PAGE - 1);

        if (s_reports < MAX_REPORTS) {
            s_reports++;
            say("[guard] write to guest 0x%08X", guest);
            if (guest >= s_ram_size)
                say(" (a mirror of 0x%08X)", (uint32_t)(guest % s_ram_size));
            say_host(" from", pc);
            say("\n");
            say_guest();
            say_backtrace();
        }
        mprotect((void *)page, PAGE, PROT_READ | PROT_WRITE);
        return;     /* the write runs again, now allowed */
    }
    if (xbox_PosixMmioFault(context, touched))
        return;     /* a hardware register xboxrecomp models */
    if (!s_crashing) {
        s_crashing = 1;
        say("[crash] signal %d touching %p", sig, info->si_addr);
        if (guest)
            say(" (guest 0x%08X)", guest);
        say_host(" at", pc);
        say("\n");
        say_guest();
        say_backtrace();
    }
    if (previous->sa_flags & SA_SIGINFO) {
        if (previous->sa_sigaction) {
            previous->sa_sigaction(sig, info, context);
            return;
        }
    } else if (previous->sa_handler != SIG_DFL && previous->sa_handler != SIG_IGN) {
        previous->sa_handler(sig);
        return;
    }
    signal(sig, SIG_DFL);   /* the instruction runs again and the program dies of it, writing a core */
}

static double seconds_since_start(void)
{
    struct timespec now;

    clock_gettime(CLOCK_MONOTONIC, &now);
    return (double)(now.tv_sec - s_start.tv_sec) + (double)(now.tv_nsec - s_start.tv_nsec) / 1e9;
}

/* SIGUSR2, sent to the game's thread by where_timer: where it is now. */
static void on_where(int sig, siginfo_t *info, void *context)
{
    ucontext_t *uc = context;
    double t = seconds_since_start();

    (void)sig;
    (void)info;
    say("[where] %d.%d s in", (int)t, (int)(t * 10) % 10);
    say_host(", at", (uintptr_t)uc->uc_mcontext.gregs[REG_RIP]);
    say("\n");
    say_guest();
    say_backtrace();
}

static void *where_timer(void *unused)
{
    (void)unused;
    for (int i = 0; i < s_where_count; i++) {
        double wait = s_where_at[i] - seconds_since_start();

        if (wait > 0) {
            struct timespec pause = { (time_t)wait, (long)((wait - (double)(time_t)wait) * 1e9) };

            while (nanosleep(&pause, &pause) != 0)
                ;
        }
        pthread_kill(s_game_thread, SIGUSR2);
    }
    return NULL;
}

static void start_where(const char *times)
{
    struct sigaction action;
    pthread_t timer;
    char *rest = (char *)times;

    while (*rest && s_where_count < MAX_WHERE) {
        double t = strtod(rest, &rest);

        if (t > 0)
            s_where_at[s_where_count++] = t;
        while (*rest == ',' || *rest == ' ')
            rest++;
        if (*rest && (*rest < '0' || *rest > '9'))
            break;
    }
    if (!s_where_count)
        return;
    s_game_thread = pthread_self();
    memset(&action, 0, sizeof action);
    action.sa_sigaction = on_where;
    action.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_RESTART;
    sigemptyset(&action.sa_mask);
    sigaction(SIGUSR2, &action, NULL);
    if (pthread_create(&timer, NULL, where_timer, NULL) == 0) {
        pthread_detach(timer);
        say("[where] the game's thread reports where it is at %d time(s), the first at %d s\n", s_where_count,
            (int)s_where_at[0]);
    }
}

static void add_guard(uint32_t guest, uintptr_t size)
{
    uintptr_t host = (uintptr_t)XBOX_PTR(guest);

    if (s_guard_count == MAX_GUARDS)
        return;
    if (mprotect((void *)host, size, PROT_READ) != 0) {
        say("[guard] could not guard guest 0x%08X (+0x%lX)\n", guest, (unsigned long)size);
        return;
    }
    s_guards[s_guard_count].lo = host;
    s_guards[s_guard_count].hi = host + size;
    s_guard_count++;
}

static int env_on(const char *name)
{
    const char *v = getenv(name);
    return v && *v && *v != '0';
}

void bro_debug_init(void)
{
    static char alternate_stack[64 * 1024];
    const char *report = getenv("BRO_FAULT_REPORT");
    const char *pages = getenv("BRO_WRITE_GUARD");
    void *warm[1];
    stack_t stack;
    struct sigaction action;
    static const int signals[] = { SIGSEGV, SIGBUS, SIGILL, SIGFPE };

    if (report && *report == '0')
        return;
    clock_gettime(CLOCK_MONOTONIC, &s_start);
    s_ram_size = xbox_GetMappedSize();
    if (!s_ram_size)
        s_ram_size = g_xbox_total_ram;
    backtrace(warm, 1);     /* loads the unwinder now, not inside the handler */

    stack.ss_sp = alternate_stack;
    stack.ss_size = sizeof alternate_stack;
    stack.ss_flags = 0;
    sigaltstack(&stack, NULL);
    memset(&action, 0, sizeof action);
    action.sa_sigaction = on_fault;
    action.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_NODEFER;
    sigemptyset(&action.sa_mask);
    for (size_t i = 0; i < sizeof signals / sizeof signals[0]; i++)
        sigaction(signals[i], &action, &s_previous[signals[i] & 31]);

    if (pages && *pages) {
        char *rest = (char *)pages;

        while (*rest) {
            uint32_t page = (uint32_t)strtoul(rest, &rest, 0) & ~(uint32_t)(PAGE - 1);

            for (int m = 0; m <= MIRRORS; m++)
                add_guard(page + (uint32_t)(m * s_ram_size), PAGE);
            say("[guard] guest page 0x%08X and its mirrors are read-only\n", page);
            while (*rest == ',' || *rest == ' ')
                rest++;
            if (*rest && (*rest < '0' || *rest > '9'))
                break;
        }
    }
    if (getenv("BRO_WHERE"))
        start_where(getenv("BRO_WHERE"));
    if (env_on("BRO_GUARD_MIRRORS")) {
        for (int m = 1; m <= MIRRORS; m++)
            add_guard((uint32_t)(m * s_ram_size), s_ram_size);
        say("[guard] the RAM mirrors (guest 0x%08X and up) are read-only\n", (uint32_t)s_ram_size);
    }
}

#else
#include "bro_debug.h"

void bro_debug_init(void)
{
}
#endif
