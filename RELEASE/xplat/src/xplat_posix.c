/* xplat on Linux (and other POSIX systems): the plain POSIX calls (see include/xplat/xplat.h). */
#if !defined(_WIN32)

#if !defined(_GNU_SOURCE)
#define _GNU_SOURCE /* clock_nanosleep, fopen's "e" */
#endif

#include <dirent.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#if defined(__APPLE__)
#include <mach-o/dyld.h> /* _NSGetExecutablePath */
#endif

#include "xplat_internal.h"

/* ---- files and folders ---------------------------------------------------------------------------------------- */

FILE *xplat_fopen(const char *path, const char *mode)
{
#if defined(__linux__)
    /* "e": close-on-exec, so a program that starts itself again (the ports' relaunch) passes no open file on */
    char with_e[16];
    size_t n = mode ? strlen(mode) : 0;

    if (!path || !mode) {
        errno = EINVAL;
        return NULL;
    }
    if (n + 2 <= sizeof with_e && !strchr(mode, 'e')) {
        memcpy(with_e, mode, n);
        with_e[n] = 'e';
        with_e[n + 1] = '\0';
        return fopen(path, with_e);
    }
#endif
    if (!path || !mode) {
        errno = EINVAL;
        return NULL;
    }
    return fopen(path, mode);
}

int xplat_stat(const char *path, xplat_stat_t *out)
{
    struct stat st;

    if (!path || !out) {
        errno = EINVAL;
        return -1;
    }
    memset(out, 0, sizeof *out);
    if (stat(path, &st) != 0)
        return -1;
    out->is_dir = S_ISDIR(st.st_mode) != 0;
    out->is_file = S_ISREG(st.st_mode) != 0;
    out->size = out->is_file ? (uint64_t)st.st_size : 0;
#if defined(__APPLE__)
    out->mtime_ns = (int64_t)st.st_mtimespec.tv_sec * 1000000000LL + st.st_mtimespec.tv_nsec;
#else
    out->mtime_ns = (int64_t)st.st_mtim.tv_sec * 1000000000LL + st.st_mtim.tv_nsec;
#endif
    return 0;
}

int xplat_mkdir(const char *path)
{
    struct stat st;

    if (!path) {
        errno = EINVAL;
        return -1;
    }
    if (mkdir(path, 0777) == 0) /* the umask decides the rest, as for every other program */
        return 0;
    if (errno == EEXIST) {
        if (stat(path, &st) == 0 && S_ISDIR(st.st_mode))
            return 0;
        errno = EEXIST;
    }
    return -1;
}

int xplat_remove(const char *path)
{
    if (!path) {
        errno = EINVAL;
        return -1;
    }
    return remove(path) == 0 ? 0 : -1;
}

int xplat_rename_replace(const char *from, const char *to)
{
    if (!from || !to) {
        errno = EINVAL;
        return -1;
    }
    return rename(from, to) == 0 ? 0 : -1;
}

struct xplat_dir {
    DIR *dir;
    char *path; /* for the entries' stat when the file system gives no type */
};

xplat_dir *xplat_opendir(const char *path)
{
    xplat_dir *d;

    if (!path) {
        errno = EINVAL;
        return NULL;
    }
    d = (xplat_dir *)calloc(1, sizeof *d);
    if (!d) {
        errno = ENOMEM;
        return NULL;
    }
    d->path = strdup(path);
    if (!d->path || !(d->dir = opendir(path))) {
        int error = d->path ? errno : ENOMEM;

        free(d->path);
        free(d);
        errno = error;
        return NULL;
    }
    return d;
}

const char *xplat_readdir(xplat_dir *d, int *is_dir)
{
    struct dirent *e;

    while ((e = readdir(d->dir)) != NULL) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
            continue;
        if (is_dir) {
#if defined(DT_DIR)
            if (e->d_type == DT_DIR) {
                *is_dir = 1;
            } else if (e->d_type != DT_UNKNOWN && e->d_type != DT_LNK) {
                *is_dir = 0;
            } else
#endif
            {
                /* the file system gives no type (some network and FUSE ones), or it's a link: ask */
                size_t size = strlen(d->path) + strlen(e->d_name) + 2;
                char *full = (char *)malloc(size);
                struct stat st;

                *is_dir = full && xplat__join(full, size, d->path, e->d_name) == 0 && stat(full, &st) == 0 &&
                          S_ISDIR(st.st_mode);
                free(full);
            }
        }
        return e->d_name;
    }
    return NULL;
}

void xplat_closedir(xplat_dir *d)
{
    if (!d)
        return;
    closedir(d->dir);
    free(d->path);
    free(d);
}

/* ---- the process ---------------------------------------------------------------------------------------------- */

int xplat_getenv(const char *name, char *buf, size_t size)
{
    const char *value = name ? getenv(name) : NULL;

    if (!name) {
        errno = EINVAL;
        return -1;
    }
    if (!value) {
        errno = ENOENT;
        return -1;
    }
    return xplat__copy_out(value, strlen(value), buf, size);
}

int xplat_exe_path(char *buf, size_t size)
{
#if defined(__linux__)
    char path[4096];
    ssize_t n = readlink("/proc/self/exe", path, sizeof path);

    if (n < 0)
        return -1;
    if ((size_t)n >= sizeof path) {
        errno = ENAMETOOLONG;
        return -1;
    }
    return xplat__copy_out(path, (size_t)n, buf, size);
#elif defined(__APPLE__)
    char path[4096];
    uint32_t n = sizeof path;

    if (_NSGetExecutablePath(path, &n) != 0) {
        errno = ENAMETOOLONG;
        return -1;
    }
    return xplat__copy_out(path, strlen(path), buf, size);
#else
    (void)buf;
    (void)size;
    errno = ENOSYS;
    return -1;
#endif
}

int xplat_utf8_args(int *argc, char ***argv)
{
    (void)argc; /* POSIX hands main its arguments as the bytes they were given: nothing to convert */
    (void)argv;
    return 0;
}

/* ---- time ----------------------------------------------------------------------------------------------------- */

uint64_t xplat_monotonic_ns(void)
{
    struct timespec now;

    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * 1000000000ull + (uint64_t)now.tv_nsec;
}

void xplat_sleep_until_ns(uint64_t deadline_ns)
{
#if defined(__APPLE__)
    /* no clock_nanosleep: a relative nanosleep, again after a signal */
    for (;;) {
        const uint64_t now = xplat_monotonic_ns();
        struct timespec left;

        if (now >= deadline_ns)
            return;
        left.tv_sec = (time_t)((deadline_ns - now) / 1000000000ull);
        left.tv_nsec = (long)((deadline_ns - now) % 1000000000ull);
        if (nanosleep(&left, NULL) == 0)
            return;
        if (errno != EINTR)
            return;
    }
#else
    struct timespec at;

    at.tv_sec = (time_t)(deadline_ns / 1000000000ull);
    at.tv_nsec = (long)(deadline_ns % 1000000000ull);
    /* absolute on the same clock xplat_monotonic_ns reads: no drift, and a signal just goes back to sleep */
    while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &at, NULL) == EINTR)
        ;
#endif
}

int xplat_request_fine_timer(void)
{
    return 1; /* Linux timers are already high resolution */
}

/* ---- the player's own folders --------------------------------------------------------------------------------- */

int xplat__user_base(xplat_user_dir_kind kind, char *buf, size_t size)
{
    static const char *const xdg[] = {"XDG_CONFIG_HOME", "XDG_DATA_HOME", "XDG_CACHE_HOME", "XDG_STATE_HOME"};
    static const char *const fallback[] = {".config", ".local/share", ".cache", ".local/state"};
    const char *value = getenv(xdg[kind]), *home;

    /* XDG Base Directory specification: a variable that is unset, empty or relative is ignored */
    if (value && value[0] == '/')
        return xplat__copy_out(value, strlen(value), buf, size);
    home = getenv("HOME");
    if (!home || home[0] != '/') {
        errno = ENOENT;
        return -1;
    }
    return xplat__join(buf, size, home, fallback[kind]);
}

#else
typedef int xplat_posix_unused; /* ISO C wants something in every file */
#endif
