/* [acct2-fh] 2026-11-15: Finest Hour starts itself again to change levels; this starts the port again with the game's
 * launch data, as the console does (fh_relaunch.h; dev/agents/acct2-fh/CONSOLE.md, "Phase 4: `map`").
 *
 * What the game does (Europe v3 default.xbe, read from its code; seen in boot69, dev/agents/acct2-fh/TEST-RUNS.md):
 *   - At the end of a level (the scripts' QuitAndLoadNextLevel, or the console's `map`) it saves the profile, fills a
 *     launch data block (on the stack at 0x000A2D89: the next level among it) and calls
 *     XLaunchNewImage("d:\default.xbe", that block) at 0x000A2DE8 (0x00131EF0). XAPI copies it into the launch data
 *     page (the kernel's LaunchDataPage: type 0 at +0, the game's title id 0x4156002A at +4, the path
 *     "\Device\CdRom0;default.xbe" at +8, the block at +0x400) and calls HalReturnToFirmware(2), a quick reboot,
 *     through the kernel import at 0x002DB2F4.
 *   - At its start the game asks XGetLaunchInfo (0x00132013, called from 0x0012CC83). XGetLaunchInfo takes the page
 *     when its type is 2 or 3, or when its title id is the game's own; copies the 0xC00 bytes at +0x400; frees the page
 *     (MmFreeContiguousMemory) and clears LaunchDataPage. With type 0 the game takes its level from the block. XAPI's
 *     own start (0x00135171) leaves the page alone unless its type is -1 or the flags at +0x214 say otherwise.
 *   xboxrecomp (v0.16.0, src/kernel/kernel_bridge.c, bridge_HalReturnToFirmware) logs that page and ends the process,
 *   and at its start fills the page only from RECOMP_CMDLINE (type 3, another title's id): the port couldn't change
 *   levels, by `map` or by finishing one.
 *
 * What this does, with xboxrecomp left as it is:
 *   - Every call the game makes through a kernel import is a call through a pointer, and recomp_lookup_manual() is
 *     asked first for each (recomp_types.h, RECOMP_ICALL_SAFE_AT). The import at 0x002DB2F4 is ordinal 49,
 *     HalReturnToFirmware (checked in the mapped XBE before xboxrecomp fills the table); once filled it holds
 *     xboxrecomp's address for it, 0xFE000000 + 4 x its slot. fh_relaunch_lookup() hands that address to
 *     fh_relaunch_hal() instead of xboxrecomp's.
 *   - fh_relaunch_hal(): when the routine is 2 and the launch data page asks for the game's own image (type 0, the
 *     game's title id, a path ending ";default.xbe"), the page goes into FH_LAUNCH_PAGE (hexadecimal, up to its last
 *     byte that isn't 0) and the port starts again: on Linux and other POSIX systems the same process runs the same
 *     program again (execv of /proc/self/exe with the same arguments: the process number, its output and whoever
 *     waits for it stay; every file but the three standard ones is closed); on Windows a new process starts with the
 *     same command line and this one ends. Anything else goes on to xboxrecomp's HalReturnToFirmware, as before.
 *   - fh_relaunch_start(), in the new start: the page from FH_LAUNCH_PAGE is put in a contiguous block (where
 *     MmAllocateContiguousMemory would put it, so the game's own MmFreeContiguousMemory frees it) and LaunchDataPage
 *     points at it, before the game runs.
 *   - The test runs' scripts (FH_PAD_SCRIPT, FH_F2_TEST) are timed from the program's start and would run again after
 *     a restart (a `map` in one would change level again and again), so a restart leaves them out, or uses
 *     FH_PAD_SCRIPT_NEXT and FH_F2_TEST_NEXT in their place when they are set (for the restart after that, neither).
 *   - Before it starts again, stderr goes back where it went (fh_console_end_log_copy): the console's copy of the log
 *     is a pipe read by a thread, neither of which outlives the exec.
 *   - FH_RELAUNCH=0: the old way (the process ends at the reboot). FH_RELAUNCHES counts the restarts (in the log).
 *
 * The window closes and opens again at a level change, and the new start takes as long as a start does, less the
 * front end. Tested on Linux; the Windows way is written, not yet run. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#ifndef _WIN32
#include <dirent.h>
#include <fcntl.h>
#include <time.h>
#include <unistd.h>
#endif

#include "recomp_types.h"           /* MEM32, XBOX_PTR, g_esp, recomp_lookup_kernel */
#include "xbox_memory_layout.h"     /* XBOX_KERNEL_DATA_BASE, KDATA_LAUNCH_DATA_PAGE, xbox_ContiguousAlloc */
#ifdef _WIN32
#include <windows.h>
#endif
#include "fh_console.h"            /* fh_console_end_log_copy */
#include "fh_relaunch.h"

#define HAL_IMPORT_SLOT      0x002DB2F4u    /* the game's import of HalReturnToFirmware */
#define HAL_IMPORT_ORDINAL   0x80000031u    /* what the XBE has there before xboxrecomp fills it: ordinal 49 */
#define KERNEL_IMPORT_LOWEST 0xFE000000u    /* xboxrecomp's addresses for the kernel's functions start here */
#define HAL_QUICK_REBOOT     2u             /* the routine XLaunchNewImage reboots with */
#define XBE_CERTIFICATE      0x00010118u    /* the XBE header's pointer to its certificate */
#define CERT_TITLE_ID        8u             /* the title id in the certificate */

#define PAGE_SIZE_BYTES      0x1000u        /* the launch data page: */
#define PAGE_TYPE            0x000u         /*   the launch data's type (0: a title's own) */
#define PAGE_TITLE_ID        0x004u         /*   the title it is for */
#define PAGE_PATH            0x008u         /*   the image to start ("\Device\CdRom0;default.xbe") */
#define PAGE_PATH_MOST       520u
#define PAGE_DATA            0x400u         /*   the title's 3072 bytes */
#define LAUNCH_TYPE_TITLE    0u

#define RELAUNCH_MOST        1000           /* a game asking more often than this is stuck: let it end */
#define VIDEO_WAIT_MS        60000          /* as long as xboxrecomp waits for a host-played film at its reboot */

#define ENV_PAGE        "FH_LAUNCH_PAGE"
#define ENV_COUNT       "FH_RELAUNCHES"
#define ENV_SWITCH      "FH_RELAUNCH"

uint32_t fh_relaunch_hal_va;                /* 0 until fh_relaunch_start() finds the import filled */

static char **g_argv;
static int g_import_checked;                /* 1: the slot held ordinal 49 before xboxrecomp filled it */

extern int xbox_VideoIsPlaying(void);       /* xboxrecomp's films (src/kernel); 1 while one plays */

static void set_env(const char *name, const char *value)
{
#ifdef _WIN32
    _putenv_s(name, value);
#else
    setenv(name, value, 1);
#endif
}

static void unset_env(const char *name)
{
#ifdef _WIN32
    _putenv_s(name, "");
#else
    unsetenv(name);
#endif
}

static void sleep_ms(int ms)
{
#ifdef _WIN32
    Sleep((DWORD)ms);
#else
    struct timespec t = { ms / 1000, (long)(ms % 1000) * 1000000L };

    nanosleep(&t, NULL);
#endif
}

void fh_relaunch_init(int argc, char **argv)
{
    (void)argc;
    g_argv = argv;
    g_import_checked = MEM32(HAL_IMPORT_SLOT) == HAL_IMPORT_ORDINAL;
    if (!g_import_checked)
        fprintf(stderr, "[fh] relaunch: the import at 0x%08X is 0x%08X, not HalReturnToFirmware (0x%08X); level "
                "changes will end the game as before\n", HAL_IMPORT_SLOT, MEM32(HAL_IMPORT_SLOT), HAL_IMPORT_ORDINAL);
}

/* One hexadecimal digit's value, or -1. */
static int hex_value(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* FH_LAUNCH_PAGE back into a page; the bytes it leaves out are 0. 0 if it isn't one. */
static int page_from_text(const char *text, uint8_t page[PAGE_SIZE_BYTES])
{
    size_t n = strlen(text), i;

    if (n == 0 || n % 2 || n / 2 > PAGE_SIZE_BYTES)
        return 0;
    memset(page, 0, PAGE_SIZE_BYTES);
    for (i = 0; i < n / 2; i++) {
        int hi = hex_value(text[2 * i]), lo = hex_value(text[2 * i + 1]);

        if (hi < 0 || lo < 0)
            return 0;
        page[i] = (uint8_t)(hi << 4 | lo);
    }
    return 1;
}

static uint32_t page_word(const uint8_t *page, uint32_t at)
{
    return (uint32_t)page[at] | (uint32_t)page[at + 1] << 8 | (uint32_t)page[at + 2] << 16 |
           (uint32_t)page[at + 3] << 24;
}

void fh_relaunch_start(void)
{
    static uint8_t page[PAGE_SIZE_BYTES];
    const char *text = getenv(ENV_PAGE);
    const char *count = getenv(ENV_COUNT);
    uint32_t va;

    if (g_import_checked) {
        uint32_t filled = MEM32(HAL_IMPORT_SLOT);
        const char *on = getenv(ENV_SWITCH);

        if (filled < KERNEL_IMPORT_LOWEST)
            fprintf(stderr, "[fh] relaunch: xboxrecomp left HalReturnToFirmware's import at 0x%08X; level changes "
                    "will end the game as before\n", filled);
        else if (on && strcmp(on, "0") == 0)
            printf("[fh] relaunch: FH_RELAUNCH=0: a level change ends the game, as before\n");
        else
            fh_relaunch_hal_va = filled;
    }

    if (!text || !text[0])
        return;
    if (!page_from_text(text, page)) {
        fprintf(stderr, "[fh] relaunch: FH_LAUNCH_PAGE isn't a launch data page (%zu characters); left out\n",
                strlen(text));
        unset_env(ENV_PAGE);
        return;
    }
    unset_env(ENV_PAGE);        /* this start's; a later one gets its own */
    va = xbox_ContiguousAlloc(PAGE_SIZE_BYTES, PAGE_SIZE_BYTES);
    if (!va) {
        fprintf(stderr, "[fh] relaunch: no contiguous memory for the launch data page; the game starts without it\n");
        return;
    }
    memcpy((void *)XBOX_PTR(va), page, PAGE_SIZE_BYTES);
    MEM32(XBOX_KERNEL_DATA_BASE + KDATA_LAUNCH_DATA_PAGE) = va;
    printf("[fh] relaunch %s: the launch data page given back at 0x%08X: type %u, title 0x%08X, data %08X %08X %08X "
           "%08X %08X %08X %08X %08X\n", count && count[0] ? count : "?", va, page_word(page, PAGE_TYPE),
           page_word(page, PAGE_TITLE_ID), page_word(page, PAGE_DATA), page_word(page, PAGE_DATA + 4),
           page_word(page, PAGE_DATA + 8), page_word(page, PAGE_DATA + 12), page_word(page, PAGE_DATA + 16),
           page_word(page, PAGE_DATA + 20), page_word(page, PAGE_DATA + 24), page_word(page, PAGE_DATA + 28));
}

/* 1 when the page asks to start the game's own image again with a title's launch data. */
static int page_is_own_image(uint32_t page)
{
    char path[PAGE_PATH_MOST + 1];
    const char *file;
    uint32_t cert = MEM32(XBE_CERTIFICATE), i;

    if (MEM32(page + PAGE_TYPE) != LAUNCH_TYPE_TITLE || !cert ||
        MEM32(page + PAGE_TITLE_ID) != MEM32(cert + CERT_TITLE_ID))
        return 0;
    for (i = 0; i < PAGE_PATH_MOST; i++) {
        path[i] = (char)*(volatile uint8_t *)XBOX_PTR(page + PAGE_PATH + i);
        if (!path[i])
            break;
    }
    path[i] = '\0';
    file = strrchr(path, ';');
    if (!file)
        return 0;
    file++;
    for (i = 0; file[i] && "default.xbe"[i]; i++)
        if ((file[i] | 0x20) != "default.xbe"[i])
            return 0;
    return !file[i] && !"default.xbe"[i];
}

/* FH_LAUNCH_PAGE: the page as hexadecimal, up to its last byte that isn't 0. */
static void page_to_env(uint32_t page)
{
    static const char digits[] = "0123456789abcdef";
    static char text[PAGE_SIZE_BYTES * 2 + 1];
    uint32_t n = PAGE_SIZE_BYTES, i;

    while (n > 0 && *(volatile uint8_t *)XBOX_PTR(page + n - 1) == 0)
        n--;
    for (i = 0; i < n; i++) {
        uint8_t b = *(volatile uint8_t *)XBOX_PTR(page + i);

        text[2 * i] = digits[b >> 4];
        text[2 * i + 1] = digits[b & 15];
    }
    text[2 * n] = '\0';
    set_env(ENV_PAGE, n ? text : "00");
}

/* A test script for the restart: NAME_NEXT's, or none. */
static void script_for_restart(const char *name, const char *next_name)
{
    const char *next = getenv(next_name);

    if (next && next[0]) {
        size_t n = strlen(next) + 1;
        char *copy = (char *)malloc(n);

        if (copy) {
            memcpy(copy, next, n);
            set_env(name, copy);
            free(copy);
        }
        unset_env(next_name);
    } else {
        unset_env(name);
    }
}

#ifndef _WIN32
/* Every file this process has open, but the three standard ones, closes at the exec: the new start opens its own. */
static void close_files_at_exec(void)
{
    DIR *dir = opendir("/proc/self/fd");
    long most, fd;

    if (dir) {
        struct dirent *e;
        int own = dirfd(dir);

        while ((e = readdir(dir)) != NULL) {
            char *end;

            fd = strtol(e->d_name, &end, 10);
            if (end == e->d_name || *end || fd <= 2 || fd == own)
                continue;
            int flags = fcntl((int)fd, F_GETFD);
            if (flags >= 0)
                fcntl((int)fd, F_SETFD, flags | FD_CLOEXEC);
        }
        closedir(dir);
        return;
    }
    most = sysconf(_SC_OPEN_MAX);
    if (most < 0 || most > 65536)
        most = 65536;
    for (fd = 3; fd < most; fd++) {
        int flags = fcntl((int)fd, F_GETFD);
        if (flags >= 0)
            fcntl((int)fd, F_SETFD, flags | FD_CLOEXEC);
    }
}
#endif

/* Start the port again. Returns only if it couldn't. stderr goes back where it went first: the console's copy of
 * the log reads it through a pipe on a thread (fh_console.c), and neither the thread nor the pipe's reading end
 * outlive the exec, so the new start died of SIGPIPE at its first line (boot70). */
static void start_again(void)
{
    fh_console_end_log_copy();
    fflush(NULL);
#ifdef _WIN32
    {
        STARTUPINFOW si;
        PROCESS_INFORMATION pi;
        const wchar_t *line = GetCommandLineW();
        size_t n = wcslen(line) + 1;
        wchar_t *copy = (wchar_t *)malloc(n * sizeof *copy);

        if (!copy)
            return;
        memcpy(copy, line, n * sizeof *copy);
        memset(&si, 0, sizeof si);
        si.cb = sizeof si;
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
        si.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
        si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
        if (CreateProcessW(NULL, copy, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi)) {
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
            ExitProcess(0);
        }
        fprintf(stderr, "[fh] relaunch: couldn't start the port again (error %lu)\n", (unsigned long)GetLastError());
        free(copy);
    }
#else
    close_files_at_exec();
    execv("/proc/self/exe", g_argv);
    if (g_argv && g_argv[0])
        execvp(g_argv[0], g_argv);
    fprintf(stderr, "[fh] relaunch: couldn't start the port again (%s)\n", strerror(errno));
#endif
}

void fh_relaunch_hal(void)
{
    uint32_t routine = MEM32(g_esp + 4);
    uint32_t page = MEM32(XBOX_KERNEL_DATA_BASE + KDATA_LAUNCH_DATA_PAGE);
    recomp_func_t xboxrecomps;

    if (routine == HAL_QUICK_REBOOT && page && g_argv && page_is_own_image(page)) {
        const char *count = getenv(ENV_COUNT);
        long done = count ? strtol(count, NULL, 10) : 0;

        if (done >= RELAUNCH_MOST) {
            fprintf(stderr, "[fh] relaunch: the game has asked to start again %ld times; it ends this time\n", done);
        } else {
            char number[24];
            int waited = 0;

            /* a host-played film, as xboxrecomp waits for one at its reboot */
            while (xbox_VideoIsPlaying() && waited < VIDEO_WAIT_MS) {
                sleep_ms(50);
                waited += 50;
            }
            printf("[fh] relaunch %ld: the game starts itself again (routine %u; launch data type %u, title 0x%08X; "
                   "data %08X %08X %08X %08X %08X %08X %08X %08X)%s; starting the port again with it\n", done + 1,
                   routine, MEM32(page + PAGE_TYPE), MEM32(page + PAGE_TITLE_ID), MEM32(page + PAGE_DATA),
                   MEM32(page + PAGE_DATA + 4), MEM32(page + PAGE_DATA + 8), MEM32(page + PAGE_DATA + 12),
                   MEM32(page + PAGE_DATA + 16), MEM32(page + PAGE_DATA + 20), MEM32(page + PAGE_DATA + 24),
                   MEM32(page + PAGE_DATA + 28), waited ? " after a film" : "");
            page_to_env(page);
            snprintf(number, sizeof number, "%ld", done + 1);
            set_env(ENV_COUNT, number);
            script_for_restart("FH_PAD_SCRIPT", "FH_PAD_SCRIPT_NEXT");
            script_for_restart("FH_F2_TEST", "FH_F2_TEST_NEXT");
            start_again();
            unset_env(ENV_PAGE);
        }
    }
    /* anything else, or a restart that failed: xboxrecomp's HalReturnToFirmware, as before */
    xboxrecomps = recomp_lookup_kernel(fh_relaunch_hal_va);
    if (xboxrecomps)
        xboxrecomps();
}
