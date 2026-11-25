/* [waw-decomp/fh2] 2026-11-23 Finest Hour port: what the port asks of the host system, the same way on Linux and
 * Windows.
 *
 * Paths: every path the port handles is UTF-8 text, on both systems. xboxrecomp's kernel reads the game and save
 * folders as UTF-8 (xbox_path_init in its kernel_path.c) and the films open their host paths that way (fh_movie.c).
 * Linux hands a program its arguments, its environment and its file names as bytes, so UTF-8 needs nothing there.
 * Windows hands a C program its arguments (main's argv), getenv's answers and fopen's names in the ANSI code page,
 * which can't hold every name Windows allows (a folder named in Japanese on a Western European system, say: its
 * letters arrive as '?'). So on Windows the arguments are taken from GetCommandLineW, the environment from _wgetenv
 * and the program's own path from GetModuleFileNameW, and files are opened with _wfopen, each turned to or from
 * UTF-8 here. Text that isn't UTF-8 (a name some other code took from the ANSI code page) is opened the old way.
 *
 * Time: CLOCK_MONOTONIC on Linux, QueryPerformanceCounter on Windows. Both run at a steady rate and never jump when
 * the system's clock is set; timespec_get(TIME_UTC), which the frame timers read on Windows before, is the wall
 * clock, which a time sync can move by seconds at once (and the game's frame step with it). */
#ifndef FH_HOST_H
#define FH_HOST_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Called first thing in main(). On Windows: *argc and *argv become the command line's words as UTF-8
 * (CommandLineToArgvW), kept until the program ends; if that fails, the C library's stay. On Linux: nothing. */
void fh_host_utf8_args(int *argc, char ***argv);

/* fopen for a UTF-8 path. */
FILE *fh_fopen(const char *path, const char *mode);

/* Puts the file `from` in the place of the file `to` (replacing it at once, on Windows too, where rename refuses
 * when `to` is there), both UTF-8 paths. 0 when done. */
int fh_replace_file(const char *from, const char *to);

/* remove for a UTF-8 path. 0 when done. */
int fh_remove(const char *path);

/* getenv for a variable that holds a path or other text, as UTF-8 (NULL when it isn't set). Each variable is read
 * once, the first time it is asked for, and the answer stays the same until the program ends. */
const char *fh_getenv_utf8(const char *name);

/* The program's own file as a UTF-8 path with '/' between folders, in out. 1 when found; 0 (and "") when not, or
 * when it doesn't fit in size bytes. */
int fh_host_program_path(char *out, size_t size);

/* Nanoseconds on a steady clock from an arbitrary start: for times between two moments, not for the date. */
uint64_t fh_host_now_ns(void);

#ifdef __cplusplus
}
#endif

#endif /* FH_HOST_H */
