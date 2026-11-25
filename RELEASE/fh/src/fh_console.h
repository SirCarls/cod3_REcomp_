/* [acct2-fh] 2026-11-14 Finest Hour's console: commands typed on the F2 window's CONSOLE page (F2, or ~ to open it
 * on that page), and the port's log, which they answer in. See fh_console.c.
 *
 * Finest Hour has no console of its own (its engine, RenderWare with Spark's code, keeps no cvars and reads no typed
 * commands), so this one is the port's: the port's settings are its variables (fh.toml's, by their names:
 * display.max_fps 144), and so are the game's own debug menu's values (debug.disable_sky_box 1, graph.total_logic 1:
 * fh_game_vars.c), with a few commands (dev/agents/acct2-fh/CONSOLE.md). */
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* At the program's start: the log's copy for the console. Everything written to stderr still goes where it went (the
 * terminal, or the test run's log file), and its last lines are kept here too. interface.console_log = false
 * (FH_CONSOLE_LOG=0) leaves stderr alone; the console then shows only its own answers. */
void fh_console_start(void);

/* Before a crash report (fh_debug.c's signal handler): stderr goes straight where it went again, so the report can't
 * be held up on its way. Safe in a signal handler. */
void fh_console_crashing(void);

/* [acct2-fh] 11-15: before the port starts itself again at a level change (fh_relaunch.c): stderr back where it went,
 * the copy's last bytes written out first. The new start's fh_console_start copies again. */
void fh_console_end_log_copy(void);

/* A typed line: one or more commands separated by ';'. Each runs at once on the calling thread: the game's, which
 * draws (the F2 window's page, fh_console_frame). */
void fh_console_run(const char *line);

/* Each frame, on the game's thread: FH_CONSOLE_EXEC's commands ("cmd; cmd ...") at the first frame. */
void fh_console_frame(void);

/* The names (commands, settings, the game's values) that start with prefix, case aside, each to fn, in order. How
 * many. */
int fh_console_complete(const char *prefix, void (*fn)(const char *name, void *user), void *user);

/* The kept lines, oldest first, each to fn. fh_console_count() is how many lines were ever kept, so the page can tell
 * a new one came without copying them. */
uint64_t fh_console_count(void);
void fh_console_lines(void (*fn)(const char *line, void *user), void *user);
void fh_console_clear(void);

/* printf into the log, and so into the console: "[console] " before it. */
void fh_console_printf(const char *format, ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 1, 2)))
#endif
    ;

#ifdef __cplusplus
}
#endif
