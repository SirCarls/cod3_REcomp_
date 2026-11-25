/* xplat: the project's small platform layer, for every game (Call of Duty 3 and the other Xbox 360 ports, Big Red
 * One, Finest Hour) and for ngl_vk. [rexglue-native-platform-fix] 2026-11-23 (owner 19:09: "Start fixing these
 * issues, please make sure they are NOT band-aid fixes. They need to account for long term crossplatform support.")
 *
 * One rule for the whole API: every string is UTF-8, on every system. On Windows each call goes through the wide
 * (UTF-16) Windows functions, so a path with any letter works whatever the PC's "language for non-Unicode programs"
 * is, and a path longer than 260 characters works where Windows allows long paths. On Linux the calls are the plain
 * POSIX ones (Linux file names are bytes; UTF-8 by convention).
 *
 * Results: functions that can fail return 0 on success and -1 on failure with errno set (on Windows the system's
 * error is turned into the nearest errno: ENOENT, EACCES, EEXIST, ENOTEMPTY, EINVAL, ENAMETOOLONG, EIO...).
 * Functions that fill a buffer never write past `size` and always end the text with a NUL; a buffer that is too
 * small is an error (ENAMETOOLONG), never a cut path.
 *
 * Nothing here depends on SDL, the ReXGlue SDK or a game. Thread safety: every function may be called from any
 * thread, except xplat_utf8_args (call it once, at the start of main). */
#ifndef XPLAT_XPLAT_H
#define XPLAT_XPLAT_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- files and folders ---------------------------------------------------------------------------------------- */

/* fopen for a UTF-8 path. `mode` is fopen's ("rb", "wb", "r", "a+b"...). Ask for "b" for anything that isn't text:
 * on Windows a text-mode stream turns "\n" into "\r\n" and stops reading at 0x1A. */
FILE *xplat_fopen(const char *path, const char *mode);

typedef struct xplat_stat_t {
    uint64_t size;     /* bytes (0 for a folder) */
    int64_t mtime_ns;  /* last change, nanoseconds since 1970-01-01 UTC */
    int is_dir;        /* 1 for a folder */
    int is_file;       /* 1 for a regular file */
} xplat_stat_t;

/* What is at `path` (links are followed). -1 with ENOENT when nothing is there. */
int xplat_stat(const char *path, xplat_stat_t *out);

/* Makes one folder. A folder that is already there is success; a file of that name is EEXIST. */
int xplat_mkdir(const char *path);

/* Makes a folder and every missing folder above it. */
int xplat_mkdirs(const char *path);

/* Removes a file, or an empty folder. */
int xplat_remove(const char *path);

/* Renames `from` to `to`, replacing `to` if it exists, in one step where the system can (POSIX rename; Windows
 * MoveFileExW with MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH). The way to save a file safely: write
 * "<name>.tmp", then xplat_rename_replace it over "<name>". On Windows it can fail with EACCES while another program
 * (a second copy of the game, an antivirus, the search indexer) holds `to` open: keep the old file and report it. */
int xplat_rename_replace(const char *from, const char *to);

/* A folder's entries, one at a time ("." and ".." left out), in no particular order. */
typedef struct xplat_dir xplat_dir;
xplat_dir *xplat_opendir(const char *path);           /* NULL with errno on failure */
/* The next entry's name (valid until the next call), or NULL at the end. *is_dir is set when it isn't NULL
 * (1 for a folder; for a link, what it points to). */
const char *xplat_readdir(xplat_dir *dir, int *is_dir);
void xplat_closedir(xplat_dir *dir);

/* ---- the process ---------------------------------------------------------------------------------------------- */

/* An environment variable's value, UTF-8. -1 with ENOENT when it isn't set (an empty value is success). */
int xplat_getenv(const char *name, char *buf, size_t size);

/* The running program's own file, as a full path. */
int xplat_exe_path(char *buf, size_t size);

/* The program's arguments as UTF-8. On Windows main()'s argv is in the ANSI code page (letters outside it become
 * "?"); this rebuilds it from the wide command line, once, and replaces *argc and *argv (the memory stays for the
 * program's life). On other systems it leaves them as they are. Call it first thing in main:
 *     int main(int argc, char **argv) { xplat_utf8_args(&argc, &argv); ... }
 * Returns 0, or -1 (argv unchanged) if Windows can't give the command line. */
int xplat_utf8_args(int *argc, char ***argv);

/* ---- time ----------------------------------------------------------------------------------------------------- */

/* A clock that only goes forward, in nanoseconds from an unspecified start (QueryPerformanceCounter on Windows,
 * CLOCK_MONOTONIC on Linux). For durations and deadlines; never for the time of day. */
uint64_t xplat_monotonic_ns(void);

/* Sleeps until xplat_monotonic_ns() reaches `deadline_ns`, as precisely as the system allows: Linux
 * clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME); Windows a high-resolution waitable timer (Windows 10 1803 and
 * later; a fraction of a millisecond late at most), else Sleep() of the whole milliseconds left. Returns at once for
 * a deadline in the past. For a frame limiter that must not oversleep, sleep to a millisecond before the deadline
 * and yield through the rest. */
void xplat_sleep_until_ns(uint64_t deadline_ns);

/* Sleeps about `ns` nanoseconds (xplat_sleep_until_ns of now + ns). */
void xplat_sleep_ns(uint64_t ns);

/* Windows: asks for 1 ms system timer ticks (timeBeginPeriod(1)) and tells Windows 11 to keep that while the window
 * seems hidden (SetProcessInformation, PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION), so sleeps and waits with a
 * time-out elsewhere in the program (SDL, the SDK, the C++ library) wake on time. Once per program; safe to call
 * again. Linux: nothing to do (returns 1). Returns 1 when the 1 ms request was accepted. */
int xplat_request_fine_timer(void);

/* ---- the player's own folders --------------------------------------------------------------------------------- */

typedef enum xplat_user_dir_kind {
    XPLAT_USER_CONFIG = 0, /* settings:      %APPDATA%\<app>       $XDG_CONFIG_HOME/<app> or ~/.config/<app> */
    XPLAT_USER_DATA = 1,   /* saves, data:   %LOCALAPPDATA%\<app>  $XDG_DATA_HOME/<app> or ~/.local/share/<app> */
    XPLAT_USER_CACHE = 2,  /* caches:        %LOCALAPPDATA%\<app>\cache   $XDG_CACHE_HOME/<app> or ~/.cache/<app> */
    XPLAT_USER_STATE = 3   /* logs, history: %LOCALAPPDATA%\<app>  $XDG_STATE_HOME/<app> or ~/.local/state/<app> */
} xplat_user_dir_kind;

/* The folder of `kind` for the program named `app` (a plain name such as "CoD3-recomp": no separators, no ".."),
 * by each system's rules: Windows' known folders through the environment (%APPDATA%, %LOCALAPPDATA%), Linux's XDG
 * Base Directory variables (used only when absolute, as the specification says) with their HOME defaults. The folder
 * isn't made (xplat_mkdirs it). -1 with ENOENT when the system gives no such folder (no HOME), EINVAL for a bad
 * `app`. */
int xplat_user_dir(xplat_user_dir_kind kind, const char *app, char *buf, size_t size);

/* ---- text ----------------------------------------------------------------------------------------------------- */

/* 1 when the `len` bytes at `text` are well-formed UTF-8 (no overlong forms, no surrogates, nothing past U+10FFFF). */
int xplat_utf8_valid(const char *text, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* XPLAT_XPLAT_H */
