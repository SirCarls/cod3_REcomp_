/* [waw-decomp/fh2] 2026-11-23 Finest Hour port: UTF-8 paths and a steady clock on Linux and Windows (fh_host.h says
 * why). Windows' own calls for wide (UTF-16) text are the ones its documentation names for this: CommandLineToArgvW,
 * _wfopen, _wgetenv, GetModuleFileNameW, MoveFileExW, QueryPerformanceCounter. */
#include "fh_host.h"

#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>
#include <shellapi.h>               /* CommandLineToArgvW (shell32) */
#else
#include <time.h>
#include <unistd.h>
#endif

/* platform-ok(os-only-block): helpers of the Windows branches below; Linux needs no conversion */
#if defined(_WIN32)
/* UTF-8 text as UTF-16, in memory the caller frees; NULL when the text isn't UTF-8 or memory ran out. */
static wchar_t *wide_from_utf8(const char *text)
{
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1, NULL, 0);
    wchar_t *wide;

    if (n <= 0)
        return NULL;
    wide = (wchar_t *)malloc((size_t)n * sizeof *wide);
    if (!wide)
        return NULL;
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1, wide, n) != n) {
        free(wide);
        return NULL;
    }
    return wide;
}

/* UTF-16 text as UTF-8, in memory the caller frees; NULL when memory ran out. */
static char *utf8_from_wide(const wchar_t *wide)
{
    int n = WideCharToMultiByte(CP_UTF8, 0, wide, -1, NULL, 0, NULL, NULL);
    char *text;

    if (n <= 0)
        return NULL;
    text = (char *)malloc((size_t)n);
    if (!text)
        return NULL;
    if (WideCharToMultiByte(CP_UTF8, 0, wide, -1, text, n, NULL, NULL) != n) {
        free(text);
        return NULL;
    }
    return text;
}
#endif

void fh_host_utf8_args(int *argc, char ***argv)
{
#if defined(_WIN32)
    int count = 0, i, ok;
    wchar_t **wide = CommandLineToArgvW(GetCommandLineW(), &count);
    char **utf8;

    if (!wide)
        return;
    utf8 = (char **)calloc((size_t)count + 1, sizeof *utf8);    /* argv[argc] is NULL, as the C library's */
    ok = utf8 != NULL;
    for (i = 0; ok && i < count; i++)
        ok = (utf8[i] = utf8_from_wide(wide[i])) != NULL;
    LocalFree(wide);
    if (!ok) {
        for (i = 0; utf8 && i < count; i++)
            free(utf8[i]);
        free(utf8);
        return;
    }
    *argc = count;
    *argv = utf8;
#else
    (void)argc;
    (void)argv;
#endif
}

FILE *fh_fopen(const char *path, const char *mode)
{
#if defined(_WIN32)
    wchar_t *wide_path = wide_from_utf8(path), *wide_mode = wide_from_utf8(mode);
    FILE *f;

    if (wide_path && wide_mode)
        f = _wfopen(wide_path, wide_mode);
    else    /* not UTF-8: a name some other code took from the ANSI code page */
        f = fopen(path, mode);  /* platform-ok(narrow-open): only for text that isn't UTF-8 (fh_host.h) */
    free(wide_path);
    free(wide_mode);
    return f;
#else
    return fopen(path, mode);
#endif
}

int fh_replace_file(const char *from, const char *to)
{
#if defined(_WIN32)
    const DWORD how = MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH;
    wchar_t *wide_from = wide_from_utf8(from), *wide_to = wide_from_utf8(to);
    BOOL done;

    if (wide_from && wide_to)
        done = MoveFileExW(wide_from, wide_to, how);   /* platform-ok(file-lock): a file we closed */
    else
        done = MoveFileExA(from, to, how);
    free(wide_from);
    free(wide_to);
    return done ? 0 : -1;
#else
    return rename(from, to);
#endif
}

int fh_remove(const char *path)
{
#if defined(_WIN32)
    wchar_t *wide = wide_from_utf8(path);
    int rc = wide ? _wremove(wide) : remove(path);

    free(wide);
    return rc;
#else
    return remove(path);
#endif
}

const char *fh_getenv_utf8(const char *name)
{
#if defined(_WIN32)
    enum { KEPT_MOST = 16, NAME_CHARS = 64 };
    static struct {
        char name[NAME_CHARS];
        char *value;                /* NULL: not set */
    } kept[KEPT_MOST];
    static int kept_count;
    static SRWLOCK lock = SRWLOCK_INIT;
    const char *answer = NULL;
    int i, found = 0;

    if (strlen(name) >= NAME_CHARS)
        return getenv(name);
    AcquireSRWLockExclusive(&lock);
    for (i = 0; i < kept_count && !found; i++)
        if (strcmp(kept[i].name, name) == 0) {
            answer = kept[i].value;
            found = 1;
        }
    if (!found && kept_count < KEPT_MOST) {
        wchar_t *wide_name = wide_from_utf8(name);
        const wchar_t *value = wide_name ? _wgetenv(wide_name) : NULL;

        i = kept_count++;
        memcpy(kept[i].name, name, strlen(name) + 1);
        kept[i].value = value ? utf8_from_wide(value) : NULL;
        answer = kept[i].value;
        found = 1;
        free(wide_name);
    }
    ReleaseSRWLockExclusive(&lock);
    return found ? answer : getenv(name);   /* more names than kept: the ANSI code page's text */
#else
    return getenv(name);
#endif
}

int fh_host_program_path(char *out, size_t size)
{
#if defined(_WIN32)
    DWORD room = 512, n;
    wchar_t *wide = NULL;
    char *text, *c;
    size_t length;

    if (size == 0)
        return 0;
    out[0] = '\0';
    for (;;) {                      /* GetModuleFileNameW cuts the path to the room it is given: ask with more */
        wchar_t *more = (wchar_t *)realloc(wide, room * sizeof *wide);

        if (!more) {
            free(wide);
            return 0;
        }
        wide = more;
        n = GetModuleFileNameW(NULL, wide, room);
        if (n == 0 || (n >= room && room >= 32768)) {
            free(wide);
            return 0;
        }
        if (n < room)
            break;
        room *= 2;
    }
    text = utf8_from_wide(wide);
    free(wide);
    if (!text)
        return 0;
    length = strlen(text);
    if (length >= size) {
        free(text);
        return 0;
    }
    for (c = text; *c; c++)
        if (*c == '\\')
            *c = '/';
    memcpy(out, text, length + 1);
    free(text);
    return 1;
#else
    ssize_t n;

    if (size == 0)
        return 0;
    out[0] = '\0';
    if (size < 2)
        return 0;
    n = readlink("/proc/self/exe", out, size - 1);
    if (n <= 0 || (size_t)n >= size - 1) {      /* not found, or perhaps cut to the room given */
        out[0] = '\0';
        return 0;
    }
    out[n] = '\0';
    return 1;
#endif
}

uint64_t fh_host_now_ns(void)
{
#if defined(_WIN32)
    static LARGE_INTEGER frequency;     /* the counter's ticks a second, fixed at the system's start */
    LARGE_INTEGER t;
    uint64_t ticks, rate;

    if (!frequency.QuadPart)
        QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&t);
    ticks = (uint64_t)t.QuadPart;
    rate = (uint64_t)frequency.QuadPart;
    /* whole seconds and the rest apart, so the multiplication can't overflow */
    return ticks / rate * 1000000000u + ticks % rate * 1000000000u / rate;
#else
    struct timespec t;

    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000u + (uint64_t)t.tv_nsec;
#endif
}
