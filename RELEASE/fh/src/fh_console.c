/* [acct2-fh] 2026-11-14 Finest Hour's console (fh_console.h).
 *
 * The owner (11-14 17:13): "do we have a console within finest hour similar to WAW?" Then (17:21): "yep keep going".
 * World at War's console shows the game's log and takes typed commands and variables. Finest Hour has neither (its
 * engine reads no typed commands and keeps no cvars), so this file is the port's own console, written to be read:
 *
 *   The log. Everything the port and the game's code print goes to stderr. fh_console_start() puts a pipe there: a
 *   thread reads it, writes every byte on to where stderr went before (the terminal, or the test run's log file, so
 *   logs stay as they were), and keeps the last KEPT_LINES lines for the CONSOLE page. A crash report goes straight
 *   to the old stderr (fh_console_crashing, from fh_debug.c), and so does everything after the program's end begins
 *   (an atexit handler empties the pipe first) or the port starts itself again (fh_console_end_log_copy, from
 *   fh_relaunch.c: the same process runs the program again, and the pipe's reader would be gone: boot70's restarts
 *   died of SIGPIPE at their first line).
 *
 *   The commands. A line is split at ';' into commands, and each command into words (a "quoted string" is one word).
 *   The first word is a command from the table below, or the name of a setting: a setting's name alone shows its
 *   value, with a value after it sets it, as World at War's variables do ("display.max_fps 144"). The settings are
 *   fh.toml's, through fh_settings.c, so a change here is the same as the F2 window's: it applies as the F2 window's
 *   rows do (most at once) and is saved by save, or when the F2 window closes.
 *
 *   The game's own values. The developers' debug menu is still in the game's program (fh_game_vars.c has how it
 *   works): its items and its 49 performance graphs are variables here as well, by names of the port's
 *   ("debug.disable_sky_box 1", "graph.total_logic 1"), read and written in the game's memory at once. They aren't
 *   settings: nothing saves them, and the game sets some itself (a level's load turns the graphs off). timescale is
 *   the menu's Game Speed, and debugmenu opens the menu itself.
 *
 *   The game's cheats. cheat turns the Extras screen's seven cheats on and off (in the game's memory, at once; the
 *   game saves them with the profile), and unlockcheats unlocks them as the game's own pad code does. unlocklevels
 *   opens every level at the level select, and unlockall does both and opens the three campaigns' films.
 *
 *   god turns on the game's own "the player takes no damage" switch (the level scripts' DisablePlayerDamage) and
 *   keeps it on: the game clears it when it makes the player at each level's start, so fh_console_frame sets it
 *   again (one word read a frame while god is on, nothing otherwise). ammo (or noreload) does the same with the debug
 *   menu's Unlimited Ammo: the player's magazine never runs down, so there is no reload. pos shows where the player
 *   is (its frame's world position), the weapon in hand and that gun's magazine, read only. teleport moves the
 *   player by the game's own "set the position" (the one the level scripts' TeleportToNamedObject calls), to a world
 *   position or one counted from where the player is (~). noclip holds the player at a place of the console's,
 *   put there through the same function once a frame and moved by the left stick (A up, B down) by the game's time.
 *   give puts a weapon the level loaded into the player's hand, as the level scripts' SetPlayerWeapon does. map
 *   goes to another level the way the scripts' QuitAndLoadNextLevel does.
 *
 * Phases 1 to 4 of dev/agents/acct2-fh/CONSOLE.md. */
#include "fh_console.h"

#include <ctype.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fh_frame_clock.h"
#include "fh_game_vars.h"
#include "fh_host.h"          /* [waw-decomp/fh2] 11-23: UTF-8 paths */
#include "fh_pad.h"
#include "fh_settings.h"

#if defined(FH_HAVE_SDL3)
#include <SDL3/SDL.h>
#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#define fh_pipe(fds) _pipe((fds), 65536, _O_BINARY)
#define fh_dup _dup
#define fh_dup2 _dup2
#define fh_read _read
#define fh_write _write
#define fh_close _close
#else
#include <unistd.h>
#define fh_pipe(fds) pipe(fds)
#define fh_dup dup
#define fh_dup2 dup2
#define fh_read read
#define fh_write write
#define fh_close close
#endif
#endif

#define KEPT_LINES   1000       /* the log's last lines, for the page */
#define LINE_CHARS   512        /* a longer line is kept in pieces of this size */
#define MAX_WORDS    16
#define WORD_CHARS   256

/* ---------------------------------------------------------------------------------------------------------------
 * The kept lines
 * --------------------------------------------------------------------------------------------------------------- */

static char kept[KEPT_LINES][LINE_CHARS];
static uint64_t kept_count;     /* lines ever kept; the newest is at (kept_count - 1) % KEPT_LINES */

#if defined(FH_HAVE_SDL3)
static SDL_Mutex *kept_lock;
static void lock(void)
{
    if (kept_lock)
        SDL_LockMutex(kept_lock);
}
static void unlock(void)
{
    if (kept_lock)
        SDL_UnlockMutex(kept_lock);
}
#else
static void lock(void) {}
static void unlock(void) {}
#endif

static void keep_line(const char *text, size_t length)
{
    char *slot;

    if (length >= LINE_CHARS)
        length = LINE_CHARS - 1;
    lock();
    slot = kept[kept_count % KEPT_LINES];
    memcpy(slot, text, length);
    slot[length] = '\0';
    kept_count++;
    unlock();
}

uint64_t fh_console_count(void)
{
    uint64_t count;

    lock();
    count = kept_count;
    unlock();
    return count;
}

void fh_console_lines(void (*fn)(const char *line, void *user), void *user)
{
    uint64_t first, i;

    lock();
    first = kept_count > KEPT_LINES ? kept_count - KEPT_LINES : 0;
    for (i = first; i < kept_count; i++)
        fn(kept[i % KEPT_LINES], user);
    unlock();
}

void fh_console_clear(void)
{
    lock();
    memset(kept, 0, sizeof kept);
    kept_count = 0;
    unlock();
}

/* ---------------------------------------------------------------------------------------------------------------
 * The log's copy: stderr through a pipe, read by a thread
 * --------------------------------------------------------------------------------------------------------------- */

#if defined(FH_HAVE_SDL3)
static int old_stderr = -1;     /* where stderr went before fh_console_start */
static int pipe_read = -1;
static SDL_Thread *reader;
static volatile int copying;    /* stderr is the pipe now */

static void write_all(int fd, const char *data, int length)
{
    while (length > 0) {
        int n = (int)fh_write(fd, data, (unsigned)length);

        if (n <= 0)
            return;
        data += n;
        length -= n;
    }
}

/* The thread: every byte on to the old stderr at once, and each whole line kept. */
static int read_log(void *unused)
{
    static char line[LINE_CHARS];
    char buffer[4096];
    size_t length = 0;
    int n, i;

    (void)unused;
    while ((n = (int)fh_read(pipe_read, buffer, sizeof buffer)) > 0) {
        write_all(old_stderr, buffer, n);
        for (i = 0; i < n; i++) {
            char c = buffer[i];

            if (c == '\n' || length == LINE_CHARS - 1) {
                keep_line(line, length);
                length = 0;
                if (c == '\n')
                    continue;
            }
            if (c != '\r')
                line[length++] = c;
        }
    }
    if (length)
        keep_line(line, length);
    return 0;
}

/* At the program's end: stderr back where it was, and the thread lets the pipe run dry before the program goes. */
static void stop_copying(void)
{
    if (!copying)
        return;
    copying = 0;
    fflush(stderr);
    fh_dup2(old_stderr, 2);     /* the pipe's last writer goes, so the thread reads to its end */
    if (reader)
        SDL_WaitThread(reader, NULL);
    reader = NULL;
}
#endif

void fh_console_start(void)
{
#if defined(FH_HAVE_SDL3)
    int fds[2];

    if (copying || reader)
        return;
    if (!kept_lock)
        kept_lock = SDL_CreateMutex();
    if (!fh_setting_bool("interface.console_log", "FH_CONSOLE_LOG", 1)) {
        fprintf(stderr, "[console] the log isn't copied into the console (interface.console_log)\n");
        return;
    }
    fflush(stderr);
    old_stderr = fh_dup(2);
    if (old_stderr < 0)
        return;                 /* no stderr to copy (a program started without one) */
    if (fh_pipe(fds) != 0) {
        fh_close(old_stderr);
        old_stderr = -1;
        return;
    }
    pipe_read = fds[0];
    if (fh_dup2(fds[1], 2) < 0) {
        fh_close(fds[0]);
        fh_close(fds[1]);
        fh_close(old_stderr);
        old_stderr = -1;
        return;
    }
    fh_close(fds[1]);           /* fd 2 is the pipe's only writer now */
    reader = SDL_CreateThread(read_log, "fh_console_log", NULL);
    if (!reader) {              /* nothing would read the pipe: stderr back */
        fh_dup2(old_stderr, 2);
        return;
    }
    copying = 1;
    atexit(stop_copying);
#endif
}

void fh_console_end_log_copy(void)
{
#if defined(FH_HAVE_SDL3)
    stop_copying();
#endif
}

void fh_console_crashing(void)
{
#if defined(FH_HAVE_SDL3)
    if (copying && old_stderr >= 0) {
        copying = 0;
        fh_dup2(old_stderr, 2);
    }
#endif
}

/* ---------------------------------------------------------------------------------------------------------------
 * Printing
 * --------------------------------------------------------------------------------------------------------------- */

/* A line of the console's own into the log like everything else (and so onto the page): one stderr write. Without
 * the log's copy it is kept here directly, so the page still shows the console's answers. */
static void put(const char *text)
{
    char line[LINE_CHARS];

    fprintf(stderr, "[console] %s\n", text);
#if defined(FH_HAVE_SDL3)
    if (!copying)
#endif
    {
        snprintf(line, sizeof line, "[console] %s", text);
        keep_line(line, strlen(line));
    }
}

/* A long text (a setting's description) as lines of at most width characters, broken at spaces, each after indent. */
static void put_wrapped(const char *indent, const char *text, int width)
{
    char line[LINE_CHARS];

    while (*text) {
        int length = (int)strlen(text), cut = length;

        if (length > width) {
            cut = width;
            while (cut > 0 && text[cut] != ' ')
                cut--;
            if (cut == 0)
                cut = width;
        }
        snprintf(line, sizeof line, "%s%.*s", indent, cut, text);
        put(line);
        text += cut;
        while (*text == ' ')
            text++;
    }
}

void fh_console_printf(const char *format, ...)
{
    char text[2048];
    va_list args;

    va_start(args, format);
    vsnprintf(text, sizeof text, format, args);
    va_end(args);
    put(text);
}

/* ---------------------------------------------------------------------------------------------------------------
 * The commands
 * --------------------------------------------------------------------------------------------------------------- */

struct command {
    const char *name;
    const char *usage;
    const char *help;
    void (*run)(int count, char **words);
};

/* The table is at the end of this part, after the commands it names; help and the lookups reach it through this. */
static const struct command *command_table(int *count);

static int same_name(const char *a, const char *b)
{
    for (; *a && *b; a++, b++)
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b))
            return 0;
    return *a == *b;
}

static int starts_with(const char *name, const char *prefix)
{
    for (; *prefix; name++, prefix++)
        if (!*name || tolower((unsigned char)*name) != tolower((unsigned char)*prefix))
            return 0;
    return 1;
}

static const struct command *find_command(const char *name)
{
    int i, count;
    const struct command *commands = command_table(&count);

    for (i = 0; i < count; i++)
        if (same_name(commands[i].name, name))
            return &commands[i];
    return NULL;
}

/* The setting called name, case aside: its key as the port spells it, or NULL. */
static const char *find_setting(const char *name)
{
    int i, n = fh_settings_count();

    for (i = 0; i < n; i++)
        if (same_name(fh_settings_key(i), name))
            return fh_settings_key(i);
    return NULL;
}

/* The game's value called name, case aside (fh_game_vars.c): its index, or -1. */
static int find_game(const char *name)
{
    return fh_game_var_find(name);
}

static const char *kind_words(enum fh_game_var_kind kind)
{
    return kind == FH_GAME_VAR_SWITCH ? "a switch, 0 or 1" : kind == FH_GAME_VAR_INT ? "a whole number" : "a decimal";
}

static void show_game(int index)
{
    char value[64];

    fh_game_var_get(index, value, sizeof value);
    fh_console_printf("%s = %s (the game's: %s)", fh_game_var_name(index), value,
                      kind_words(fh_game_var_kind(index)));
}

static void set_game(int index, int count, char **words)
{
    char value[WORD_CHARS * 2] = "";
    size_t used = 0;
    int i;

    for (i = 0; i < count; i++)
        used += (size_t)snprintf(value + used, used < sizeof value ? sizeof value - used : 0, "%s%s", i ? " " : "",
                                 words[i]);
    if (fh_game_var_set(index, value) != 0) {
        fh_console_printf("%s can't be \"%s\": it is %s%s", fh_game_var_name(index), value,
                          kind_words(fh_game_var_kind(index)),
                          fh_game_var_kind(index) == FH_GAME_VAR_SWITCH ? " (or on/off, true/false)" : "");
        return;
    }
    show_game(index);
}

/* Where a setting's value comes from, in words. */
static const char *source_words(const char *key, char *out, size_t size)
{
    const char *env = fh_settings_env(key);

    switch (fh_settings_source(key)) {
    case 'c':
        return "set while playing";
    case 'e':
        snprintf(out, size, "from %s", env ? env : "its variable");
        return out;
    case 'f':
        return "from fh.toml";
    default:
        return "not given: the port's default";
    }
}

static void show_setting(const char *key)
{
    const char *value = fh_setting_text(key, fh_settings_env(key));
    char where[96];

    if (value)
        fh_console_printf("%s = \"%s\" (%s)", key, value, source_words(key, where, sizeof where));
    else
        fh_console_printf("%s isn't given (the port's default)", key);
}

static void set_setting(const char *key, int count, char **words)
{
    char value[WORD_CHARS * 2] = "";
    size_t used = 0;
    int i;

    for (i = 0; i < count; i++)        /* the words after the name, with a space between */
        used += (size_t)snprintf(value + used, used < sizeof value ? sizeof value - used : 0, "%s%s", i ? " " : "",
                                 words[i]);
    if (fh_settings_set(key, value) != 0) {
        fh_console_printf("%s can't be \"%s\" (too long, or a quote in it)", key, value);
        return;
    }
    if (fh_settings_source(key) == 'c' && fh_settings_env(key) && getenv(fh_settings_env(key)))
        fh_console_printf("%s = \"%s\" (over %s, which is set too)", key, value, fh_settings_env(key));
    else
        fh_console_printf("%s = \"%s\"", key, value);
}

/* A setting's key, or else the game's value's index: one of them, or a line saying there is neither. */
static int need_var(const char *command, const char *name, const char **key, int *game)
{
    *key = find_setting(name);
    *game = *key ? -1 : find_game(name);
    if (*key || *game >= 0)
        return 1;
    fh_console_printf("%s: no setting or game value called \"%s\" (list shows them)", command, name);
    return 0;
}

static void run_help(int count, char **words)
{
    const struct command *command, *commands;
    const char *key, *help, *env;
    int i, command_count;

    commands = command_table(&command_count);
    if (count < 2) {
        fh_console_printf("Commands (help <name> for one; a variable's name alone shows it, with a value sets it):");
        for (i = 0; i < command_count; i++)
            fh_console_printf("  %-10s %s", commands[i].name, commands[i].help);
        fh_console_printf("Settings: list shows all %d (list <text> those with text in their name).",
                          fh_settings_count());
        fh_console_printf("The game's own: list debug., list perf. and list graph. (%d in all; they aren't saved).",
                          fh_game_var_count());
        return;
    }
    if ((command = find_command(words[1])) != NULL) {
        fh_console_printf("%s: %s", command->usage, command->help);
        return;
    }
    if ((key = find_setting(words[1])) != NULL) {
        show_setting(key);
        help = fh_settings_help(key);
        env = fh_settings_env(key);
        put_wrapped("  ", help ? help : "(fh.toml has no description of it)", 100);
        if (env)
            fh_console_printf("  Its variable: %s.", env);
        return;
    }
    if ((i = find_game(words[1])) >= 0) {
        show_game(i);
        put_wrapped("  ", fh_game_var_help(i), 100);
        return;
    }
    fh_console_printf("help: no command, setting or game value called \"%s\"", words[1]);
}

/* name has text in it, case aside. */
static int has_text(const char *name, const char *text)
{
    size_t n = strlen(text);

    for (; *name; name++)
        if (starts_with(name, text))
            return 1;
    return n == 0;
}

static void run_list(int count, char **words)
{
    int i, n = fh_settings_count(), shown = 0, games = 0;

    for (i = 0; i < n; i++) {
        const char *key = fh_settings_key(i);
        const char *value = fh_setting_text(key, fh_settings_env(key));
        char where[96];

        if (count > 1 && !has_text(key, words[1]))
            continue;
        fh_console_printf("  %-34s %-14s %s", key, value ? value : "-", source_words(key, where, sizeof where));
        shown++;
    }
    n = fh_game_var_count();
    for (i = 0; i < n; i++) {
        char value[64];

        if (count > 1 && !has_text(fh_game_var_name(i), words[1]))
            continue;
        fh_game_var_get(i, value, sizeof value);
        fh_console_printf("  %-34s %-14s the game's", fh_game_var_name(i), value);
        games++;
    }
    fh_console_printf("%d setting(s), %d of the game's value(s)", shown, games);
}

static void run_cmdlist(int count, char **words)
{
    int i, command_count;
    const struct command *commands = command_table(&command_count);

    (void)count;
    (void)words;
    for (i = 0; i < command_count; i++)
        fh_console_printf("  %s", commands[i].usage);
}

static void run_get(int count, char **words)
{
    const char *key;
    int game;

    if (count < 2) {
        fh_console_printf("get <name>");
        return;
    }
    if (!need_var("get", words[1], &key, &game))
        return;
    if (key)
        show_setting(key);
    else
        show_game(game);
}

static void run_set(int count, char **words)
{
    const char *key;
    int game;

    if (count < 3) {
        fh_console_printf("set <name> <value>");
        return;
    }
    if (!need_var("set", words[1], &key, &game))
        return;
    if (key)
        set_setting(key, count - 2, words + 2);
    else
        set_game(game, count - 2, words + 2);
}

static void run_toggle(int count, char **words)
{
    const char *key;
    char value[64], *flipped[1] = {value};
    int game;

    if (count < 2) {
        fh_console_printf("toggle <name>");
        return;
    }
    if (!need_var("toggle", words[1], &key, &game))
        return;
    if (key) {
        snprintf(value, sizeof value, "%s", fh_setting_bool(key, fh_settings_env(key), 0) ? "false" : "true");
        set_setting(key, 1, flipped);
        return;
    }
    if (fh_game_var_kind(game) != FH_GAME_VAR_SWITCH) {
        fh_console_printf("toggle: %s isn't a switch (it is %s)", fh_game_var_name(game),
                          kind_words(fh_game_var_kind(game)));
        return;
    }
    fh_game_var_get(game, value, sizeof value);
    snprintf(value, sizeof value, "%s", strcmp(value, "0") ? "0" : "1");
    set_game(game, 1, flipped);
}

/* The debug menu's Game Speed (debug.game_speed, 0x00321680): the frame timer's time scale, 1 the normal speed. */
static void run_timescale(int count, char **words)
{
    int game = find_game("debug.game_speed");
    char *end;
    double value;

    if (game < 0)
        return;
    if (count < 2) {
        show_game(game);
        return;
    }
    value = strtod(words[1], &end);
    if (end == words[1] || *end || value < 0.0) {
        fh_console_printf("timescale: a number from 0 (the game stopped) up, 1 the normal speed");
        return;
    }
    set_game(game, 1, words + 1);
}

/* The game's debug menu, on screen: debugmenu on, off, back (the menu's own Back: up a page, or shut at the first),
 * or alone to turn it over. The pad works it (fh_game_vars.c). */
static void run_debugmenu(int count, char **words)
{
    int open = !fh_game_debug_menu_open();

    if (count > 1) {
        if (same_name(words[1], "on") || same_name(words[1], "1"))
            open = 1;
        else if (same_name(words[1], "off") || same_name(words[1], "0"))
            open = 0;
        else if (same_name(words[1], "back")) {
            int went = fh_game_debug_menu_back();

            fh_console_printf("debugmenu: %s", went < 0 ? "it isn't open" : went ? "up a page" : "shut (it was on its "
                              "first page)");
            return;
        } else {
            fh_console_printf("debugmenu [on|off|back]");
            return;
        }
    }
    if (fh_game_debug_menu_show(open) != 0) {
        fh_console_printf("debugmenu: the game hasn't made its menu yet");
        return;
    }
    fh_console_printf("debugmenu: %s", open ? "open (the pad moves in it; debugmenu back goes up a page)" : "shut");
}

/* One cheat's line: "  3 head_shot      on   (unlocked)  Head Shot (Easy): ..." */
static void show_cheat(int index, int wide)
{
    int state = fh_game_cheat_state(index);

    if (wide)
        fh_console_printf("  %d %-13s %-4s %-12s %s", index + 1, fh_game_cheat_name(index), state & 1 ? "on" : "off",
                          state & 2 ? "(unlocked)" : "(locked)", fh_game_cheat_help(index));
    else
        fh_console_printf("cheat %d %s: %s%s", index + 1, fh_game_cheat_name(index), state & 1 ? "on" : "off",
                          state & 2 ? "" : " (locked on the profile)");
}

/* The Extras screen's cheats: cheat alone lists them, cheat <name|1-7> shows one, and on, off or toggle after the
 * name sets it ("cheat big_head on", "cheat 3 toggle"). */
static void run_cheat(int count, char **words)
{
    int i, index, on;

    if (fh_game_cheat_state(0) < 0) {
        fh_console_printf("cheat: the game hasn't set up its cheats yet");
        return;
    }
    if (count < 2) {
        fh_console_printf("the game's cheats (cheat <name|1-7> on|off|toggle; unlockcheats unlocks all):");
        for (i = 0; i < fh_game_cheat_count(); i++)
            show_cheat(i, 1);
        return;
    }
    index = fh_game_cheat_find(words[1]);
    if (index < 0) {
        fh_console_printf("cheat: no cheat \"%s\" (cheat alone lists them)", words[1]);
        return;
    }
    if (count < 3) {
        show_cheat(index, 0);
        return;
    }
    if (same_name(words[2], "on") || same_name(words[2], "1"))
        on = 1;
    else if (same_name(words[2], "off") || same_name(words[2], "0"))
        on = 0;
    else if (same_name(words[2], "toggle"))
        on = !(fh_game_cheat_state(index) & 1);
    else {
        fh_console_printf("cheat <name|1-7> on|off|toggle");
        return;
    }
    i = fh_game_cheat_state(index);
    fh_game_cheat_set(index, on);
    show_cheat(index, 0);
    if (on && !(i & 2) && (fh_game_cheat_state(index) & 2))
        fh_console_printf("  (unlocked on the profile too, so the Extras screen shows it and can turn it off)");
}

static void run_unlockcheats(int count, char **words)
{
    (void)count;
    (void)words;
    if (fh_game_cheats_unlock() != 0) {
        fh_console_printf("unlockcheats: the game has no profile yet");
        return;
    }
    fh_console_printf("unlockcheats: all seven unlocked on the profile (the Extras screen shows them; the game saves it)");
}

/* Every level at the level select (the profile's furthest level), and with unlockall the cheats and the films too.
 * They change the profile, which the game saves, as playing it all would. */
static void run_unlocklevels(int count, char **words)
{
    int was = fh_game_levels_reached();

    (void)count;
    (void)words;
    if (fh_game_levels_unlock() != 0) {
        fh_console_printf("unlocklevels: the game has no profile yet");
        return;
    }
    fh_console_printf("unlocklevels: the level select opens every level (the furthest level %d, was %d; the game "
                      "saves it)", fh_game_levels_reached(), was);
}

static void run_unlockall(int count, char **words)
{
    (void)count;
    (void)words;
    if (fh_game_levels_unlock() != 0 || fh_game_cheats_unlock() != 0 || fh_game_campaigns_unlock() != 0) {
        fh_console_printf("unlockall: the game has no profile yet");
        return;
    }
    fh_console_printf("unlockall: every level at the level select, the seven cheats in Extras and the three "
                      "campaigns' films (the game saves it with the profile)");
}

/* god: kept on across levels by fh_console_frame. */
static int god_wanted;

static void run_god(int count, char **words)
{
    int state = fh_game_god_get();

    if (count > 1) {
        if (same_name(words[1], "on") || same_name(words[1], "1"))
            god_wanted = 1;
        else if (same_name(words[1], "off") || same_name(words[1], "0"))
            god_wanted = 0;
        else {
            fh_console_printf("god [on|off]");
            return;
        }
    } else {
        god_wanted = !(god_wanted || state == 1);
    }
    if (fh_game_god_set(god_wanted) != 0) {
        fh_console_printf("god: %s (no player yet: it starts with the next level)", god_wanted ? "on" : "off");
        return;
    }
    fh_console_printf("god: %s", god_wanted ? "on (the player takes no damage; kept on through levels)" : "off");
}

/* ammo and noreload: the debug menu's Unlimited Ammo, kept on across levels by fh_console_frame. */
static int ammo_wanted;

static void run_ammo(int count, char **words)
{
    if (count > 1) {
        if (same_name(words[1], "on") || same_name(words[1], "1"))
            ammo_wanted = 1;
        else if (same_name(words[1], "off") || same_name(words[1], "0"))
            ammo_wanted = 0;
        else {
            fh_console_printf("%s [on|off]", words[0]);
            return;
        }
    } else {
        ammo_wanted = !(ammo_wanted || fh_game_ammo_get());
    }
    fh_game_ammo_set(ammo_wanted);
    fh_console_printf("%s: %s", words[0],
                      ammo_wanted ? "on (the magazine never runs down, so no reload; kept on through levels)" : "off");
}

/* pos: where the player is (read only). */
static const char *weapon_said(int weapon, char *out, size_t size);

static void run_pos(int count, char **words)
{
    float pos[3], health;
    unsigned table;
    int weapon, magazine, hit_points;
    char said[56];

    (void)count;
    (void)words;
    if (fh_game_player_where(pos, &table, &weapon, &magazine) != 0) {
        fh_console_printf("pos: no player yet");
        return;
    }
    if (fh_game_player_health(&health, &hit_points) != 0) {
        health = 0.0f;
        hit_points = 0;
    }
    fh_console_printf("pos: %.2f %.2f %.2f (the player's class table 0x%08X; weapon %s in hand, %d in its magazine; "
                      "health %.3f of %d)", pos[0], pos[1], pos[2], table, weapon_said(weapon, said, sizeof said),
                      magazine, health, hit_points);
}

/* One of teleport's numbers: "12.5" is that place, "~" where the player is, "~-40" that far from it. */
static int teleport_number(const char *word, float now, float *out)
{
    char *end;
    float f = 0.0f;

    if (*word == '~') {
        word++;
        if (*word)
            f = strtof(word, &end);
        else
            end = (char *)word;
        if (*end)
            return -1;
        *out = now + f;
        return 0;
    }
    f = strtof(word, &end);
    if (end == word || *end)
        return -1;
    *out = f;
    return 0;
}

/* teleport: moves the player, by the game's own "set the position" (fh_game_player_teleport). */
static void run_teleport(int count, char **words)
{
    float now[3], to[3];
    unsigned table;
    int weapon, magazine, i, done;

    if (count != 4) {
        fh_console_printf("teleport <x> <y> <z>: a world position (pos shows yours; y is up), ~ for where you are, "
                          "~10 for 10 from it (teleport ~ ~200 ~ is 200 up)");
        return;
    }
    if (fh_game_player_where(now, &table, &weapon, &magazine) != 0) {
        fh_console_printf("teleport: no player yet");
        return;
    }
    for (i = 0; i < 3; i++)
        if (teleport_number(words[1 + i], now[i], &to[i]) != 0) {
            fh_console_printf("teleport: \"%s\" isn't a number (or ~, or ~ and a number)", words[1 + i]);
            return;
        }
    done = fh_game_player_teleport(to);
    if (done == -2)
        fh_console_printf("teleport: the game's function for it isn't in the port (class table 0x%08X)", table);
    else if (done != 0)
        fh_console_printf("teleport: no player yet");
    else
        fh_console_printf("teleport: from %.2f %.2f %.2f to %.2f %.2f %.2f", now[0], now[1], now[2], to[0], to[1],
                          to[2]);
}

/* hurt: a hit on the player through the game's own "take a hit", made as its own "hurt a group" makes one
 * (fh_game_player_hurt), mostly to show god works; with god off enough of it kills the player, as the game does. */
#define HURT_DEFAULT 100.0f             /* of the player's hit points (1000 in L_1_2), before the difficulty's scale */
#define HURT_MOST 100000.0f

static void run_hurt(int count, char **words)
{
    float damage = HURT_DEFAULT, before, after, health, scale, after_hit;
    int hit_points, level, done;
    unsigned gates;
    char *end, why[200];

    if (count > 2) {
        fh_console_printf("hurt [<damage>]");
        return;
    }
    if (count == 2) {
        damage = strtof(words[1], &end);
        if (end == words[1] || *end != '\0' || !(damage >= 0.0f && damage <= HURT_MOST)) {
            fh_console_printf("hurt [<damage>]: 0 to %.0f (alone: %.0f)", HURT_MOST, HURT_DEFAULT);
            return;
        }
    }
    gates = fh_game_player_hurt_gates(&after_hit);      /* as they are before the hit */
    done = fh_game_player_hurt(damage, &before, &after);
    if (done == -2) {
        fh_console_printf("hurt: the game's function for it isn't in the port");
        return;
    }
    if (done != 0) {
        fh_console_printf("hurt: no player yet");
        return;
    }
    if (fh_game_player_health(&health, &hit_points) != 0)
        hit_points = 0;
    if (fh_game_difficulty(&level, &scale) != 0)
        scale = 1.0f;
    why[0] = '\0';
    if (gates & FH_HURT_GOD)
        strcat(why, "; god is on");
    if (gates & FH_HURT_AFTER_HIT)
        snprintf(why + strlen(why), sizeof why - strlen(why), "; just after a hit from an attacker (%.2f left)",
                 after_hit);
    if (gates & FH_HURT_NO_LOSS)
        strcat(why, "; the game's no-loss byte (0x003EA960) is set");
    if (gates & FH_HURT_CAMERA)
        strcat(why, "; the thing at the player's +0x210C has +0x414 1");
    fh_console_printf("hurt: %.0f damage: health %.3f to %.3f of %d (difficulty %d, its scale %.2f)%s%s", damage,
                      before, after, hit_points, level, scale, after == before ? "; the game took none" : "",
                      gates ? why : "; nothing in the game's way");
}

/* hits: every change of the player's health as it comes, one line each, by fh_console_frame (the frame-rate audit,
 * [acct2-fh] 11-15: boot83's 30 fps runs took hits of 35 points that its uncapped runs didn't, and its 2 s readings
 * can't tell one hit from two). Each line: the seconds since the program started, the health before and after, the
 * points that makes (of the player's hit points), the game's time and frame count, and what is left of the player's
 * "just after an attacker's hit" time (+0x19EC, which keeps another attacker's hit off while it lasts). Read only. */
static int hits_wanted;
static unsigned hits_player;            /* the player the last health is for (0: none yet) */
static float hits_last;

static double hits_seconds(void)        /* since the program started (a build without SDL: the game's time) */
{
#if defined(FH_HAVE_SDL3)
    return (double)SDL_GetTicks() / 1000.0;
#else
    return fh_frame_clock_game_time() / 60.0;
#endif
}

static void hits_step(void)
{
    unsigned player = fh_game_player();
    float health, after_hit;
    int hit_points;

    if (player == 0u || fh_game_player_health(&health, &hit_points) != 0) {
        hits_player = 0u;
        return;
    }
    if (player != hits_player) {        /* a new player (a level's start, a respawn): start from its health */
        if (hits_player != 0u)
            fh_console_printf("hits: %.2f s, a new player, health %.3f", hits_seconds(), health);
        hits_player = player;
        hits_last = health;
        return;
    }
    if (health == hits_last)
        return;
    fh_game_player_hurt_gates(&after_hit);
    fh_console_printf("hits: %.2f s, health %.3f to %.3f (%+.0f of %d), the game's time %.2f s, frame %u, after-hit "
                      "time %.2f", hits_seconds(), hits_last, health,
                      (double)(health - hits_last) * hit_points, hit_points, fh_frame_clock_game_time() / 60.0,
                      (unsigned)fh_frame_clock_frame_count(), after_hit);
    hits_last = health;
}

/* weapons: each of the game's 39 weapons whose data gives its bullets a damage, by number and name (read only; the
 * frame-rate audit's look at the two hits on the player in L_1_2, of 60 and 35 points). */
static void run_weapons(int count, char **words)
{
    char name[40];
    float damage;
    int i, shown = 0;

    (void)count;
    (void)words;
    for (i = 0; fh_game_weapon_damage(i, &damage) == 0; i++) {
        if (damage == 0.0f)
            continue;
        if (fh_game_weapon_name(i, name, sizeof name) != 0)
            strcpy(name, "no name");
        fh_console_printf("weapons: %d (%s): %.1f a bullet", i, name, (double)damage);
        shown++;
    }
    if (shown == 0)
        fh_console_printf("weapons: none has a bullet damage (no level loaded yet?)");
    else
        fh_console_printf("weapons: %d with a bullet damage, before the difficulty's scale", shown);
}

static void run_hits(int count, char **words)
{
    if (count > 1) {
        if (same_name(words[1], "on") || same_name(words[1], "1"))
            hits_wanted = 1;
        else if (same_name(words[1], "off") || same_name(words[1], "0"))
            hits_wanted = 0;
        else {
            fh_console_printf("hits [on|off]");
            return;
        }
    } else {
        hits_wanted = !hits_wanted;
    }
    hits_player = 0u;
    fh_console_printf("hits: %s", hits_wanted ? "on (each change of the player's health gets a line)" : "off");
}

/* hitcalls: every hit the game hands the player as it comes, taken or not, one line each, by fh_console_frame (the
 * frame-rate audit, [acct2-fh] 11-15: no weapon's bullet does the 60 or 35 points the player takes in L_1_2, so where
 * do they come from, and how often?). The game's thread notes each call of the player's "take a hit" (0x000C3620,
 * fh_game_vars.c) and runs it as it was; this prints the notes: the game's time and frame of the call, its address
 * (which of the game's 11 calls), the block's kind words and +0x08, its damage (+0x2C, and +0x28), the shooter and its
 * class table, and the health before and after (the same when the game took none). [acct2-fh] 11-15: boot86 found them
 * all from 0x00103D44, a projectile's step (0x00103850), with the projectile at the block's +0x04; a second line gives
 * its age when it hit, the time it may fly and its speed, its damage record and class. Every 5 s of the game's time
 * a line counts the rounds made since, by kind (damage, record, class), seen through the projectile step 0x00103850.
 * Read only. */
static int hitcalls_wanted;

static double hitcalls_census_at;       /* the game's time of the last rounds count (seconds) */

/* Every 5 s of the game's time: the rounds made since, by kind (with how far from the player they started: the mean
 * and the nearest), then the launches since, by the launch function, where it returned to and the damage record. */
static void hitcalls_census(void)
{
    struct fh_game_shots shots;
    struct fh_game_launches launches;
    char line[1600];
    double now = fh_frame_clock_game_time() / 60.0;
    unsigned i;
    size_t used;

    if (now >= hitcalls_census_at && now - hitcalls_census_at < 5.0)
        return;
    hitcalls_census_at = now;
    fh_game_shots_take(&shots);
    fh_game_launches_take(&launches);
    if (shots.kinds != 0u || shots.other != 0u) {
        used = (size_t)snprintf(line, sizeof line, "hitcalls: rounds made by the game's time %.2f s:", now);
        for (i = 0; i < shots.kinds && used < sizeof line; i++) {
            used += (size_t)snprintf(line + used, sizeof line - used, "%s %u of damage %.1f (record 0x%08X, class "
                                     "0x%08X", i ? "," : "", shots.kind[i].made, (double)shots.kind[i].damage,
                                     shots.kind[i].record, shots.kind[i].class_table);
            if (used < sizeof line && shots.kind[i].placed != 0u)
                used += (size_t)snprintf(line + used, sizeof line - used, "; from the player %.0f on average, "
                                         "nearest %.0f", (double)(shots.kind[i].distance_sum / shots.kind[i].placed),
                                         (double)shots.kind[i].distance_least);
            if (used < sizeof line)
                used += (size_t)snprintf(line + used, sizeof line - used, ")");
        }
        if (shots.other != 0u && used < sizeof line)
            snprintf(line + used, sizeof line - used, ", %u of other kinds", shots.other);
        fh_console_printf("%s", line);
    }
    if (launches.sites != 0u || launches.other != 0u) {
        used = (size_t)snprintf(line, sizeof line, "hitcalls: launched by the game's time %.2f s:", now);
        for (i = 0; i < launches.sites && used < sizeof line; i++) {
            used += (size_t)snprintf(line + used, sizeof line - used, "%s %u through 0x%08X back to 0x%08X",
                                     i ? "," : "", launches.site[i].made, launches.site[i].launch,
                                     launches.site[i].back);
            if (used < sizeof line && launches.site[i].from != 0u)   /* the factory's: where it was called from */
                used += (size_t)snprintf(line + used, sizeof line - used, " from 0x%08X", launches.site[i].from);
            if (used < sizeof line)
                used += (size_t)snprintf(line + used, sizeof line - used, " (record 0x%08X, damage %.1f)",
                                         launches.site[i].record, (double)launches.site[i].damage);
        }
        if (launches.other != 0u && used < sizeof line)
            snprintf(line + used, sizeof line - used, ", %u from other places", launches.other);
        fh_console_printf("%s", line);
    }
}

static void hitcalls_step(void)
{
    struct fh_game_hit hit;
    unsigned dropped;

    hitcalls_census();

    while (fh_game_hit_next(&hit, &dropped)) {
        if (dropped != 0u)
            fh_console_printf("hitcalls: %u calls not shown (more than 64 between two frames)", dropped);
        fh_console_printf("hitcalls: the game's time %.2f s, frame %u, from 0x%08X, %s 0x%08X, kind %04X/%04X, "
                          "+0x08 0x%X, damage %.1f (+0x28 %.1f), by 0x%08X (class 0x%08X), health %.3f to %.3f%s",
                          hit.game_time, hit.frame, hit.from, hit.player ? "the player" : "not the player", hit.taker,
                          hit.kind & 0xFFFFu, hit.kind >> 16, hit.what, (double)hit.damage, (double)hit.damage_28,
                          hit.by, hit.by_class, (double)hit.before, (double)hit.after,
                          hit.after == hit.before ? " (none taken)" : "");
        if (hit.source_life > 0.0f)     /* a projectile's hit (0x00103850): how old it was of how long it may fly */
            fh_console_printf("hitcalls:   +0x04 0x%08X: %.1f 60ths old of its %.1f (started %.1f), speed %.2f a step, "
                              "record 0x%08X, class 0x%08X", hit.source, hit.game_time * 60.0 - hit.source_born,
                              (double)hit.source_life, (double)hit.source_born, (double)hit.source_speed,
                              hit.source_record, hit.source_class);
    }
}

static void run_hitcalls(int count, char **words)
{
    if (count > 1) {
        if (same_name(words[1], "on") || same_name(words[1], "1"))
            hitcalls_wanted = 1;
        else if (same_name(words[1], "off") || same_name(words[1], "0"))
            hitcalls_wanted = 0;
        else {
            fh_console_printf("hitcalls [on|off]");
            return;
        }
    } else {
        hitcalls_wanted = !hitcalls_wanted;
    }
    fh_game_hit_watch(hitcalls_wanted);
    hitcalls_census_at = fh_frame_clock_game_time() / 60.0;
    fh_console_printf("hitcalls: %s", hitcalls_wanted ? "on (each hit handed to the player gets a line, taken or not)"
                                                      : "off");
}

/* difficulty: the game's difficulty (0-2). Set, it goes where the level select puts it (what each level starts with)
 * and into the level being played (31 places in the game read it there). What a level sets up as it loads (the enemies
 * it places, say) follows the new one from the next level or restart on. */
static void run_difficulty(int count, char **words)
{
    float scale, scales[3];
    int level, setting, wanted, done;
    char *end;

    if (count > 2) {
        fh_console_printf("difficulty [0|1|2]");
        return;
    }
    if (count == 2) {
        wanted = (int)strtol(words[1], &end, 10);
        if (end == words[1] || *end != '\0') {
            fh_console_printf("difficulty [0|1|2]");
            return;
        }
        done = fh_game_difficulty_set(wanted);
        if (done == -1) {
            fh_console_printf("difficulty: %d is outside the game's 0-2", wanted);
            return;
        }
        if (done == -2) {
            fh_console_printf("difficulty: the game's function for it isn't in the port");
            return;
        }
        if (done == -3) {
            fh_console_printf("difficulty: the game's settings aren't made yet");
            return;
        }
    }
    setting = fh_game_difficulty_setting(scales);
    if (fh_game_difficulty(&level, &scale) != 0) {
        fh_console_printf("difficulty: %d now (outside the game's 0-2: no level yet?), levels start with %d", level,
                          setting);
        return;
    }
    fh_console_printf("difficulty: %d of 0-2 now, levels start with %d; a hit on the player does %.2f / %.2f / %.2f of "
                      "its damage at 0 / 1 / 2", level, setting, scales[0], scales[1], scales[2]);
}

/* noclip: the player held at a place the console keeps, moved by the pad, through the game's own "set the position"
 * (fh_game_player_teleport) once a frame, so neither gravity nor the game's own movement and collision move the
 * player between frames. The left stick moves along the way the player faces (flat), A up, B down; the place moves by
 * the game's time, so it goes the same distance a second at any frame rate (and not at all while the game is
 * paused). Off again by itself when the player is made anew (a level's start, a death). */
#define NOCLIP_SPEED_DEFAULT 400.0f     /* units a second at full stick (walking is about 260) */
#define NOCLIP_DEAD_ZONE 0.2f           /* of a stick's travel, as most of the game's pad code ignores */
#define NOCLIP_BUTTON_DOWN 32           /* an analog button's value (0-255) that counts as pressed */
#define NOCLIP_STEP_MOST 0.1            /* seconds: a longer gap (a load) moves the place no further */

static int noclip_wanted;
static unsigned noclip_player;          /* the player the place is for (0: none yet) */
static float noclip_place[3];
static double noclip_time;              /* the game's time (60ths of a second) at the last step */
static float noclip_speed = NOCLIP_SPEED_DEFAULT;

static float noclip_stick(int16_t value)
{
    float f = (float)value / 32767.0f;

    if (fabsf(f) < NOCLIP_DEAD_ZONE)
        return 0.0f;
    return f > 1.0f ? 1.0f : f < -1.0f ? -1.0f : f;
}

/* A flat unit vector along v (y left out), or 0 when v is (nearly) straight up or down. */
static int noclip_flat(const float v[3], float out[3])
{
    float length = sqrtf(v[0] * v[0] + v[2] * v[2]);

    if (length < 0.001f)
        return -1;
    out[0] = v[0] / length;
    out[1] = 0.0f;
    out[2] = v[2] / length;
    return 0;
}

static void noclip_step(void)
{
    float now_place[3], at[3], right[3], forward[3], side[3], ahead, across, up, step;
    unsigned player = fh_game_player(), table;
    int weapon, magazine, i;
    uint16_t digital;
    uint8_t analog[8];
    int16_t thumbs[4];
    double now = fh_frame_clock_game_time(), seconds;

    if (player == 0u)
        return;                         /* a load: the place is kept for the same player */
    if (fh_game_player_where(now_place, &table, &weapon, &magazine) != 0 || fh_game_player_axes(at, right) != 0)
        return;
    if (noclip_player == 0u) {          /* the first frame: held where the player is */
        noclip_player = player;
        memcpy(noclip_place, now_place, sizeof noclip_place);
        noclip_time = now;
    } else if (player != noclip_player) {
        noclip_wanted = 0;
        noclip_player = 0u;
        fh_console_printf("noclip: off (the player was made anew: a level's start or a death)");
        return;
    }
    seconds = (now - noclip_time) / 60.0;
    noclip_time = now;
    if (seconds < 0.0)
        seconds = 0.0;
    if (seconds > NOCLIP_STEP_MOST)
        seconds = NOCLIP_STEP_MOST;
    fh_pad_last_state(&digital, analog, thumbs);
    (void)digital;
    ahead = noclip_stick(thumbs[1]);
    across = noclip_stick(thumbs[0]);
    up = (analog[0] >= NOCLIP_BUTTON_DOWN ? 1.0f : 0.0f) - (analog[1] >= NOCLIP_BUTTON_DOWN ? 1.0f : 0.0f);
    step = noclip_speed * (float)seconds;
    if (noclip_flat(at, forward) != 0)
        ahead = 0.0f;
    if (noclip_flat(right, side) != 0)
        across = 0.0f;
    for (i = 0; i < 3; i += 2)
        noclip_place[i] += (forward[i] * ahead + side[i] * across) * step;
    noclip_place[1] += up * step;
    fh_game_player_teleport(noclip_place);
}

static void run_noclip(int count, char **words)
{
    char *end;
    float speed;
    int on;

    if (count > 2) {
        fh_console_printf("noclip [on|off|<speed>]: the player goes through anything, by the left stick, A up, B down "
                          "(a speed in units a second turns it on at that speed; %.0f at first)", NOCLIP_SPEED_DEFAULT);
        return;
    }
    if (count == 1) {
        on = !noclip_wanted;
    } else if (!strcmp(words[1], "on") || !strcmp(words[1], "1")) {
        on = 1;
    } else if (!strcmp(words[1], "off") || !strcmp(words[1], "0")) {
        on = 0;
    } else {
        speed = strtof(words[1], &end);
        if (end == words[1] || *end || !(speed > 0.0f) || speed > 100000.0f) {
            fh_console_printf("noclip: \"%s\" isn't on, off or a speed (units a second, above 0)", words[1]);
            return;
        }
        noclip_speed = speed;
        on = 1;
    }
    if (on && fh_game_player() == 0u) {
        fh_console_printf("noclip: no player yet");
        return;
    }
    noclip_wanted = on;
    noclip_player = 0u;                 /* held from where the player is at the next frame */
    if (on)
        fh_console_printf("noclip: on, %.0f units a second (the left stick, A up, B down; noclip again to stop)",
                          noclip_speed);
    else
        fh_console_printf("noclip: off");
}

/* give: a weapon the level loaded into the player's hand (fh_game_player_give, the scripts' SetPlayerWeapon). */
/* A weapon as "17 (Nagant Sniper)", by the game's own list of names; the number alone when it has none. */
static const char *weapon_said(int weapon, char *out, size_t size)
{
    char name[40];

    if (weapon >= 0 && fh_game_weapon_name(weapon, name, sizeof name) == 0 && name[0])
        snprintf(out, size, "%d (%s)", weapon, name);
    else
        snprintf(out, size, "%d", weapon);
    return out;
}

static void give_list(char *out, size_t size, const unsigned loaded[2])
{
    size_t used = 0;
    int i, n = 0;
    char one[56];

    out[0] = '\0';
    for (i = 0; i < 64 && used + sizeof one + 2 < size; i++)
        if (loaded[i / 32] & (1u << (i % 32)))
            used += (size_t)snprintf(out + used, size - used, "%s%s", n++ ? ", " : "", weapon_said(i, one, sizeof one));
    if (n == 0)
        snprintf(out, size, "none");
}

/* A weapon by name (any case, _ for a space: give t34_cannon), among the 64 numbers; -1 when none is called that. */
static int weapon_by_name(const char *word)
{
    char name[40];
    int i, k;

    for (i = 0; i < 64; i++) {
        if (fh_game_weapon_name(i, name, sizeof name) != 0 || !name[0])
            continue;
        for (k = 0; name[k] && word[k]; k++)
            if (tolower((unsigned char)name[k]) != tolower((unsigned char)(word[k] == '_' ? ' ' : word[k])))
                break;
        if (!name[k] && !word[k])
            return i;
    }
    return -1;
}

static void run_give(int count, char **words)
{
    unsigned loaded[2];
    int carried[2], in_hand, weapon, i, done;
    char list[1024], *end, said[3][56];
    long number;

    if (count > 2) {
        fh_console_printf("give [<weapon>|<name>|next]: a weapon this level loaded into your hand (give alone lists "
                          "them)");
        return;
    }
    if (fh_game_player_weapons(carried, &in_hand, loaded) != 0) {
        fh_console_printf("give: no player yet");
        return;
    }
    give_list(list, sizeof list, loaded);
    if (count == 1) {
        fh_console_printf("give: in hand %s, carried %s and %s; this level's weapons: %s",
                          weapon_said(in_hand, said[0], sizeof said[0]),
                          weapon_said(carried[0], said[1], sizeof said[1]),
                          weapon_said(carried[1], said[2], sizeof said[2]), list);
        return;
    }
    if (!strcmp(words[1], "next")) {    /* the next loaded weapon after the one in hand, the no-gun number passed */
        weapon = -1;
        for (i = 1; i <= 64 && weapon < 0; i++) {
            int n = ((in_hand < 0 ? -1 : in_hand) + i) % 64;

            if (n != fh_game_weapon_no_gun() && n != in_hand && (loaded[n / 32] & (1u << (n % 32))))
                weapon = n;
        }
        if (weapon < 0) {
            fh_console_printf("give next: no other weapon in this level (its weapons: %s)", list);
            return;
        }
    } else {
        number = strtol(words[1], &end, 10);
        if (end != words[1] && !*end && number >= 0 && number <= 63)
            weapon = (int)number;
        else if ((weapon = weapon_by_name(words[1])) < 0) {
            fh_console_printf("give: \"%s\" isn't a weapon's number or name, or next (this level's weapons: %s)",
                              words[1], list);
            return;
        }
    }
    done = fh_game_player_give(weapon);
    if (done == -2)
        fh_console_printf("give: this level didn't load weapon %s (its weapons: %s)",
                          weapon_said(weapon, said[0], sizeof said[0]), list);
    else if (done == -3)
        fh_console_printf("give: the game's function for it isn't in the port");
    else if (done != 0)
        fh_console_printf("give: no player yet");
    else
        fh_console_printf("give: weapon %s into your hand (it was %s)", weapon_said(weapon, said[0], sizeof said[0]),
                          weapon_said(in_hand, said[1], sizeof said[1]));
}

/* map: another level, as the level scripts' QuitAndLoadNextLevel starts one (fh_game_level_goto). */
static int map_level(const char *word)
{
    char name[40], *end;
    long number = strtol(word, &end, 10);
    int i;

    if (end != word && !*end)
        return number >= 1 && number <= fh_game_level_most() ? (int)number : -1;
    for (i = 1; i <= fh_game_level_most(); i++) {
        const char *a = word, *b = name;

        if (fh_game_level_name(i, name, sizeof name) != 0)
            continue;
        if (!strncmp(b, "L_", 2) && strncmp(a, "L_", 2) && strncmp(a, "l_", 2))
            b += 2;                     /* "2_3" for "L_2_3" */
        while (*a && *b && tolower((unsigned char)*a) == tolower((unsigned char)*b))
            a++, b++;
        if (!*a && !*b)
            return i;
    }
    return -1;
}

static void run_map(int count, char **words)
{
    char name[40], list[640];
    size_t used = 0;
    int now, level, i, done;

    if (count > 2) {
        fh_console_printf("map [<level>]: another level, by its number (1 to %d) or its name (L_2_3, or 2_3)",
                          fh_game_level_most());
        return;
    }
    if (count == 1) {
        now = fh_game_level_now();
        if (now >= 0 && fh_game_level_name(now, name, sizeof name) == 0)
            fh_console_printf("map: now %d (%s)", now, name);
        else
            fh_console_printf("map: no level now");
        list[0] = '\0';
        for (i = 1; i <= fh_game_level_most() && used + 16 < sizeof list; i++)
            if (fh_game_level_name(i, name, sizeof name) == 0)
                used += (size_t)snprintf(list + used, sizeof list - used, "%s%d %s", i > 1 ? ", " : "", i, name);
        fh_console_printf("map: the levels: %s", list);
        return;
    }
    level = map_level(words[1]);
    if (level < 0) {
        fh_console_printf("map: no level \"%s\" (map alone lists them)", words[1]);
        return;
    }
    fh_game_level_name(level, name, sizeof name);
    done = fh_game_level_goto(level);
    if (done == -1)
        fh_console_printf("map: in a level only (the front end starts levels its own way)");
    else if (done == -3)
        fh_console_printf("map: the game's function for it isn't in the port");
    else if (done == -4)
        fh_console_printf("map: a level change is already under way");
    else if (done != 0)
        fh_console_printf("map: no level %d", level);
    else
        fh_console_printf("map: to %d (%s); this level fades out and that one loads", level, name);
}

static void run_save(int count, char **words)
{
    (void)count;
    (void)words;
    if (fh_settings_save() == 0)
        fh_console_printf("saved to fh.toml");
    else
        fh_console_printf("save: fh.toml can't be written");
}

static void run_echo(int count, char **words)
{
    char text[WORD_CHARS * 4] = "";
    size_t used = 0;
    int i;

    for (i = 1; i < count; i++)
        used += (size_t)snprintf(text + used, used < sizeof text ? sizeof text - used : 0, "%s%s", i > 1 ? " " : "",
                                 words[i]);
    fh_console_printf("%s", text);
}

static void run_clear(int count, char **words)
{
    (void)count;
    (void)words;
    fh_console_clear();
}

static void dump_line(const char *line, void *user)
{
    fprintf((FILE *)user, "%s\n", line);
}

/* World at War's condump: the console's lines into a file. */
static void run_condump(int count, char **words)
{
    FILE *f;

    if (count < 2) {
        fh_console_printf("condump <file>");
        return;
    }
    if (!(f = fh_fopen(words[1], "w"))) {
        fh_console_printf("condump: can't write %s", words[1]);
        return;
    }
#if defined(FH_HAVE_SDL3)
    if (copying)
        SDL_Delay(100);         /* the lines printed just now are still on their way through the pipe */
#endif
    fh_console_lines(dump_line, f);
    fclose(f);
    fh_console_printf("condump: %llu line(s) kept, written to %s", (unsigned long long)fh_console_count(), words[1]);
}

static int exec_depth;

static void run_exec(int count, char **words)
{
    char line[1024];
    FILE *f;
    int n = 0;

    if (count < 2) {
        fh_console_printf("exec <file>");
        return;
    }
    if (exec_depth >= 4) {
        fh_console_printf("exec: files run from files, 4 deep: %s left out", words[1]);
        return;
    }
    if (!(f = fh_fopen(words[1], "r"))) {
        fh_console_printf("exec: can't read %s", words[1]);
        return;
    }
    exec_depth++;
    while (fgets(line, sizeof line, f)) {
        char *s = line;

        line[strcspn(line, "\r\n")] = '\0';
        while (isspace((unsigned char)*s))
            s++;
        if (!*s || *s == '#' || (s[0] == '/' && s[1] == '/'))
            continue;
        fh_console_run(s);
        n++;
    }
    exec_depth--;
    fclose(f);
    fh_console_printf("exec: %d line(s) of %s", n, words[1]);
}

static const struct command commands[] = {
    {"help", "help [name]", "the commands; with a name, a command's, a setting's or a game value's own help",
     run_help},
    {"list", "list [text]", "every setting and game value, its value and where it comes from (cvarlist too)",
     run_list},
    {"cvarlist", "cvarlist [text]", "the same as list", run_list},
    {"cmdlist", "cmdlist", "the commands, as they are typed", run_cmdlist},
    {"get", "get <name>", "a setting's or game value's value (its name alone does the same)", run_get},
    {"set", "set <name> <value>", "changes a setting or game value (its name and a value do the same)", run_set},
    {"toggle", "toggle <name>", "turns a switch on or off", run_toggle},
    {"timescale", "timescale [speed]", "the game's speed (debug.game_speed): 1 normal, 0.5 half, 2 double",
     run_timescale},
    {"debugmenu", "debugmenu [on|off|back]", "opens or shuts the game's own debug menu (worked with the pad; back goes "
     "up a page)", run_debugmenu},
    {"cheat", "cheat [name|1-7] [on|off|toggle]", "the game's seven cheats (its Extras screen's): alone lists them",
     run_cheat},
    {"unlockcheats", "unlockcheats", "unlocks the seven cheats on the profile, as the game's own pad code does",
     run_unlockcheats},
    {"unlocklevels", "unlocklevels", "opens every level at the level select (the profile's furthest level)",
     run_unlocklevels},
    {"god", "god [on|off]", "the player takes no damage (the game's own switch, kept on through levels)", run_god},
    {"ammo", "ammo [on|off]", "the magazine never runs down (the debug menu's Unlimited Ammo, kept on through levels)",
     run_ammo},
    {"noreload", "noreload [on|off]", "the same as ammo: with the magazine never running down there is no reload",
     run_ammo},
    {"pos", "pos", "where the player is, the weapon in hand, its magazine and the health left (read only)", run_pos},
    {"teleport", "teleport <x> <y> <z>", "moves the player there (~ is where you are: teleport ~ ~200 ~ is 200 up)",
     run_teleport},
    {"hurt", "hurt [<damage>]", "a hit on the player as the game makes one (100 alone; god stops it, enough kills)",
     run_hurt},
    {"hits", "hits [on|off]", "a line for each change of the player's health, as it comes (read only)", run_hits},
    {"weapons", "weapons", "the damage each weapon's bullet does, from the game's weapon data (read only)",
     run_weapons},
    {"hitcalls", "hitcalls [on|off]", "a line for each hit the game hands the player, taken or not: where from, its "
     "damage and shooter (read only)", run_hitcalls},
    {"difficulty", "difficulty [0|1|2]", "the game's difficulty: alone shows it, with a number sets it (as the level "
     "select does, and for the level being played)", run_difficulty},
    {"noclip", "noclip [on|off|<speed>]", "the player goes through anything, by the left stick, A up and B down",
     run_noclip},
    {"give", "give [<weapon>|<name>|next]", "a weapon this level loaded into your hand (alone: lists them, with the "
     "game's names)", run_give},
    {"map", "map [<level>]", "another level, by number or name (alone: lists them and says which is played)", run_map},
    {"unlockall", "unlockall", "unlocklevels, unlockcheats and the three campaigns' films", run_unlockall},
    {"save", "save", "writes the changed settings into fh.toml (closing F2 does too)", run_save},
    {"exec", "exec <file>", "runs a file's lines as commands (# and // start a comment)", run_exec},
    {"echo", "echo <text>", "prints the text", run_echo},
    {"clear", "clear", "empties the console's lines (the log file keeps them)", run_clear},
    {"condump", "condump <file>", "writes the console's lines into a file", run_condump},
};

static const struct command *command_table(int *count)
{
    *count = (int)(sizeof commands / sizeof commands[0]);
    return commands;
}

/* One command's words: split at spaces, a "quoted string" one word. How many (at most MAX_WORDS). */
static int split_words(const char *text, char words[MAX_WORDS][WORD_CHARS])
{
    int count = 0;

    while (*text && count < MAX_WORDS) {
        size_t length = 0;

        while (isspace((unsigned char)*text))
            text++;
        if (!*text)
            break;
        if (*text == '"') {
            text++;
            while (*text && *text != '"') {
                if (length + 1 < WORD_CHARS)
                    words[count][length++] = *text;
                text++;
            }
            if (*text == '"')
                text++;
        } else {
            while (*text && !isspace((unsigned char)*text)) {
                if (length + 1 < WORD_CHARS)
                    words[count][length++] = *text;
                text++;
            }
        }
        words[count++][length] = '\0';
    }
    return count;
}

static void run_one(const char *text)
{
    char words[MAX_WORDS][WORD_CHARS];  /* on the stack: exec runs commands from inside a command */
    char *pointers[MAX_WORDS];
    const struct command *command;
    const char *key;
    int count = split_words(text, words), i, game;

    if (count == 0)
        return;
    for (i = 0; i < count; i++)
        pointers[i] = words[i];
    if ((command = find_command(words[0])) != NULL) {
        command->run(count, pointers);
        return;
    }
    if ((key = find_setting(words[0])) != NULL) {     /* World at War's way: a variable's name, and a value */
        if (count == 1)
            show_setting(key);
        else
            set_setting(key, count - 1, pointers + 1);
        return;
    }
    if ((game = find_game(words[0])) >= 0) {             /* the game's own, the same way */
        if (count == 1)
            show_game(game);
        else
            set_game(game, count - 1, pointers + 1);
        return;
    }
    fh_console_printf("no command, setting or game value called \"%s\" (help lists the commands, list the rest)",
                      words[0]);
}

void fh_console_run(const char *line)
{
    char command[1024];
    int quoted = 0;
    size_t length = 0;

    fh_console_printf("] %s", line);    /* the typed line, as World at War's console shows it */
    for (;; line++) {
        if (*line == '"')
            quoted = !quoted;
        if ((*line == ';' && !quoted) || !*line) {
            command[length] = '\0';
            run_one(command);
            length = 0;
            if (!*line)
                break;
            continue;
        }
        if (length + 1 < sizeof command)
            command[length++] = *line;
    }
}

void fh_console_frame(void)
{
    static int done;
    const char *text;

    if (god_wanted && fh_game_god_get() == 0)   /* a new player (a level's start): god again */
        fh_game_god_set(1);
    if (ammo_wanted && !fh_game_ammo_get())     /* the same for Unlimited Ammo */
        fh_game_ammo_set(1);
    if (noclip_wanted)                          /* the player held at the console's place, moved by the pad */
        noclip_step();
    if (hits_wanted)                            /* each change of the player's health */
        hits_step();
    if (hitcalls_wanted)                        /* each hit handed to the player */
        hitcalls_step();
    if (done)
        return;
    done = 1;
    text = fh_getenv_utf8("FH_CONSOLE_EXEC");
    if (text && *text)
        fh_console_run(text);
}

int fh_console_complete(const char *prefix, void (*fn)(const char *name, void *user), void *user)
{
    int i, n = fh_settings_count(), found = 0, command_count;
    const struct command *commands = command_table(&command_count);

    for (i = 0; i < command_count; i++)
        if (starts_with(commands[i].name, prefix)) {
            fn(commands[i].name, user);
            found++;
        }
    for (i = 0; i < n; i++)
        if (starts_with(fh_settings_key(i), prefix)) {
            fn(fh_settings_key(i), user);
            found++;
        }
    n = fh_game_var_count();
    for (i = 0; i < n; i++)
        if (starts_with(fh_game_var_name(i), prefix)) {
            fn(fh_game_var_name(i), user);
            found++;
        }
    return found;
}
