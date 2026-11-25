/* [acct2-fh] 2026-11-13 Finest Hour port: the player's settings (fh_settings.h).
 *
 * The settings live in fh.toml, a small TOML file the player (or the setup tool) writes:
 *
 *     version = 1
 *     [display]
 *     mode = "window"
 *     render_resolution = "auto"
 *
 * Only what this port needs of TOML is read: [section] lines, key = value lines, values that are a number, true or
 * false, or a "quoted string", and # comments. A key is asked for as "section.name" ("display.mode").
 *
 * Every setting also has an environment variable (FH_DISPLAY_MODE, FH_RENDER_RESOLUTION, ...): when it is set, it
 * wins. The variables were the port's only settings before this file, and the test runs pass them, so a variable
 * always means what it meant.
 *
 * The file: FH_SETTINGS (its path) when set; else fh.toml in the folder the program is in, or the folder above it.
 * No file means every setting's default. Keys the port doesn't know are reported once, so a mistyped one is noticed.
 * "version" is the file's format; this port reads version 1, and a newer file is read as far as it can be.
 *
 * This is Big Red One's bro/src/settings/bro_settings.c (acct2-bro-game, f2-menus), the same reader and writer with
 * Finest Hour's names and keys, so one F2 window can keep both games' settings the same way. */
#include "fh_settings.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fh_host.h"     /* [waw-decomp/fh2] 11-23: UTF-8 paths and the program's folder, on both systems */

#define MAX_SETTINGS   128
#define KEY_CHARS      64
#define VALUE_CHARS    256
#define LINE_CHARS     512
#define FILE_VERSION   1

/* Every key the port reads, with the environment variable its readers pass (NULL: none); the others are reported
 * once. Keep in step with fh/config/fh.toml and the readers. [acct2-fh] 11-14: the variables are here too, for the
 * console (fh_console.c), which shows where a value comes from. */
static const struct {
    const char *key;
    const char *env;
} known_keys[] = {
    {"version", NULL},
    /* the window and the screen (fh_present.c) */
    {"display.mode", "FH_DISPLAY_MODE"}, {"display.monitor", "FH_MONITOR"},
    {"display.refresh_rate", "FH_REFRESH_RATE"}, {"display.fullscreen_size", "FH_FULLSCREEN_SIZE"},
    {"display.window_size", "FH_WINDOW_SIZE"}, {"display.vsync", "FH_VSYNC"}, {"display.fit", "FH_FIT"},
    {"display.scaling", "FH_SCALING"},
    {"display.cas_sharpness", "FH_CAS_SHARPNESS"},                                        /* [acct2-fh] stage 5b */
    {"display.fsr_sharpness_reduction", "FH_FSR_SHARPNESS_REDUCTION"}, {"display.fsr_passes", "FH_FSR_PASSES"},
    /* what the game is drawn at (fh_resolution.c) */
    {"display.render_resolution", "FH_RENDER_RESOLUTION"}, {"display.render_percent", "FH_RENDER_PERCENT"},
    {"display.live_resolution", "FH_LIVE_RESOLUTION"}, {"display.aspect_ratio", "FH_ASPECT_RATIO"},
    /* the frame rate (fh_frame_pace.c) */
    {"display.max_fps", "FH_MAX_FPS"}, {"display.fps_log", "FH_FPS_LOG"},                /* [acct2-fh] 11-14 */
    /* the game (main.c, fh_frame_clock.c) */
    {"game.language", "FH_LANGUAGE"}, {"game.fine_frame_clock", "FH_FINE_FRAME_CLOCK"},  /* [acct2-fh] 11-14 */
    {"game.frame_clocks_by_time", "FH_FRAME_CLOCKS_BY_TIME"},
    {"game.fixed_step_above_60", "FH_FIXED_STEP"},                                         /* [acct2-fh] 11-22 */
    {"game.fade_by_60ths", "FH_FADE_BY_60THS"},                                            /* [waw-decomp/fh13] 11-24 */
    {"game.pad_keep_presses", "FH_PAD_KEEP_PRESSES"},                                      /* [waw-decomp/fh15] 11-24 */
    {"game.draw_between", "FH_DRAW_BETWEEN"},                                              /* [waw-decomp/fh16] 11-24 */
    {"game.hd_textures", "FH_HD_TEXTURES"},                                                /* [waw-decomp/fh19] 11-24 */
    {"game.hd_textures_log", "FH_HD_TEXTURES_LOG"},
    /* the drawing (fh_d3d_state.c) */
    {"video.depth_clear", "FH_DEPTH_CLEAR"},                                               /* [acct2-fh] 11-22 */
    /* the keyboard's keys and the sticks' dead zone (fh_pad.c) */
    {"input.binds_kbm", "FH_BINDS_KBM"}, {"input.dead_zone", "FH_DEAD_ZONE"},             /* [input-port] 11-24 */
    /* the F2 window's own, and the console's (fh_console.c) */
    {"interface.theme", NULL}, {"interface.text_size", NULL}, {"interface.opacity", NULL},
    {"interface.pad_shortcut", NULL}, {"interface.console_log", "FH_CONSOLE_LOG"},     /* [acct2-fh] 11-14 */
};
#define KNOWN_COUNT ((int)(sizeof known_keys / sizeof known_keys[0]))

/* What the file says about a setting: the comment lines just above it, joined (fh.toml describes each one there). */
#define HELP_CHARS     1200

static struct {
    char key[KEY_CHARS];
    char value[VALUE_CHARS];
    char help[HELP_CHARS];
} settings[MAX_SETTINGS];
static int setting_count;
static int loaded;

/* The values set while the game runs (they win over the variables too, since the player chose them just now), and
 * the file they go back into. */
#define MAX_CHANGES    128
static struct {
    char key[KEY_CHARS];
    char value[VALUE_CHARS];
} changes[MAX_CHANGES];
static int change_count;
static unsigned generation;     /* fh_settings_set's changes, counted */
static char loaded_path[4200];

/* The key's place in known_keys, or -1. */
static int known_index(const char *key)
{
    int i;

    for (i = 0; i < KNOWN_COUNT; i++)
        if (strcmp(known_keys[i].key, key) == 0)
            return i;
    return -1;
}

static int known(const char *key)
{
    return known_index(key) >= 0;
}

static char *trim(char *text)
{
    char *end;

    while (*text && isspace((unsigned char)*text))
        text++;
    end = text + strlen(text);
    while (end > text && isspace((unsigned char)end[-1]))
        *--end = '\0';
    return text;
}

/* The line without its comment: a # outside quotes ends it. */
static void cut_comment(char *line)
{
    int quoted = 0;

    for (; *line; line++) {
        if (*line == '"')
            quoted = !quoted;
        else if (*line == '#' && !quoted) {
            *line = '\0';
            return;
        }
    }
}

static void keep(const char *section, const char *name, const char *value, const char *help, const char *path,
                 int line_number)
{
    char key[KEY_CHARS];
    int i;

    if (section[0])
        snprintf(key, sizeof key, "%s.%s", section, name);
    else
        snprintf(key, sizeof key, "%s", name);
    if (!known(key)) {
        fprintf(stderr, "[settings] %s line %d: no setting called \"%s\" (left out)\n", path, line_number, key);
        return;
    }
    for (i = 0; i < setting_count; i++)
        if (strcmp(settings[i].key, key) == 0)
            break;
    if (i == setting_count) {
        if (setting_count == MAX_SETTINGS) {
            fprintf(stderr, "[settings] %s: more than %d settings; the rest are left out\n", path, MAX_SETTINGS);
            return;
        }
        setting_count++;
    }
    snprintf(settings[i].key, sizeof settings[i].key, "%s", key);
    snprintf(settings[i].value, sizeof settings[i].value, "%s", value);
    snprintf(settings[i].help, sizeof settings[i].help, "%s", help);
}

/* A comment line's text (after its # and one space) added to the comment being gathered, with a space between. */
static void gather_comment(char *help, size_t size, const char *line)
{
    size_t used = strlen(help);

    line++;                         /* the # */
    if (*line == ' ')
        line++;
    if (!*line)
        return;
    snprintf(help + used, size - used, "%s%s", used ? " " : "", line);
}

static int read_file(const char *path)
{
    FILE *f = fh_fopen(path, "r");
    char line[LINE_CHARS], section[KEY_CHARS] = "", help[HELP_CHARS] = "";
    int number = 0, version;
    const char *text;

    if (!f)
        return 0;
    while (fgets(line, sizeof line, f)) {
        char *s, *equals, *name, *value;

        number++;
        s = trim(line);
        if (*s == '#') {                /* a comment: kept for the setting under it */
            gather_comment(help, sizeof help, s);
            continue;
        }
        cut_comment(s);
        s = trim(s);
        if (!*s) {                      /* a blank line ends a comment that belongs to no setting */
            help[0] = '\0';
            continue;
        }
        if (*s == '[') {
            help[0] = '\0';
            char *close = strchr(s, ']');

            if (!close) {
                fprintf(stderr, "[settings] %s line %d: a section without its ]\n", path, number);
                continue;
            }
            *close = '\0';
            snprintf(section, sizeof section, "%s", trim(s + 1));
            continue;
        }
        equals = strchr(s, '=');
        if (!equals) {
            fprintf(stderr, "[settings] %s line %d: not \"key = value\" (left out)\n", path, number);
            continue;
        }
        *equals = '\0';
        name = trim(s);
        value = trim(equals + 1);
        if (value[0] == '"') {          /* a quoted string: what is between the quotes */
            char *close = strchr(value + 1, '"');

            value++;
            if (close)
                *close = '\0';
        }
        keep(section, name, value, help, path, number);
        help[0] = '\0';
    }
    fclose(f);
    snprintf(loaded_path, sizeof loaded_path, "%s", path);    /* where Save writes */
    text = fh_setting_text("version", NULL);
    version = text ? atoi(text) : FILE_VERSION;
    if (version > FILE_VERSION)
        fprintf(stderr, "[settings] %s is version %d; this port reads version %d, so newer settings are left out\n",
                path, version, FILE_VERSION);
    fprintf(stderr, "[settings] read %s: %d settings\n", path, setting_count);
    return 1;
}

/* The folder the program is in, with its trailing separator, or "" when it can't be found. */
static void program_folder(char *out, size_t size)
{
    char *slash;

    if (!fh_host_program_path(out, size))  /* [waw-decomp/fh2] 11-23: UTF-8, '/' between folders (fh_host.c) */
        return;
    slash = strrchr(out, '/');
    if (slash)
        slash[1] = '\0';
    else
        out[0] = '\0';
}

void fh_settings_load(void)
{
    const char *given;
    char folder[4096], path[4200];

    if (loaded)
        return;
    loaded = 1;
    given = fh_getenv_utf8("FH_SETTINGS");
    if (given && given[0]) {
        if (!read_file(given))
            fprintf(stderr, "[settings] FH_SETTINGS: can't read %s; every setting at its default\n", given);
        return;
    }
    program_folder(folder, sizeof folder);
    if (!folder[0])
        return;
    snprintf(path, sizeof path, "%sfh.toml", folder);
    if (read_file(path))
        return;
    snprintf(path, sizeof path, "%s../fh.toml", folder);
    read_file(path);
}

const char *fh_setting_text(const char *key, const char *env_name)
{
    const char *value;
    int i;

    fh_settings_load();
    for (i = 0; i < change_count; i++)       /* set in the F2 window */
        if (strcmp(changes[i].key, key) == 0)
            return changes[i].value;
    if (env_name && (value = getenv(env_name)) != NULL && value[0])
        return value;
    for (i = 0; i < setting_count; i++)
        if (strcmp(settings[i].key, key) == 0)
            return settings[i].value;
    return NULL;
}

int fh_setting_given(const char *key, const char *env_name)
{
    return fh_setting_text(key, env_name) != NULL;
}

double fh_setting_number(const char *key, const char *env_name, double fallback)
{
    const char *text = fh_setting_text(key, env_name);
    char *end;
    double value;

    if (!text)
        return fallback;
    value = strtod(text, &end);
    if (end == text) {
        fprintf(stderr, "[settings] %s = \"%s\" isn't a number; %g is used\n", key, text, fallback);
        return fallback;
    }
    return value;
}

int fh_setting_int(const char *key, const char *env_name, int fallback)
{
    double value = fh_setting_number(key, env_name, (double)fallback);

    return (int)(value < 0.0 ? value - 0.5 : value + 0.5);
}

int fh_setting_bool(const char *key, const char *env_name, int fallback)
{
    const char *text = fh_setting_text(key, env_name);
    char word[8];
    size_t i;

    if (!text)
        return fallback;
    for (i = 0; i + 1 < sizeof word && text[i]; i++)
        word[i] = (char)tolower((unsigned char)text[i]);
    word[i] = '\0';
    if (!strcmp(word, "1") || !strcmp(word, "true") || !strcmp(word, "yes") || !strcmp(word, "on"))
        return 1;
    if (!strcmp(word, "0") || !strcmp(word, "false") || !strcmp(word, "no") || !strcmp(word, "off"))
        return 0;
    fprintf(stderr, "[settings] %s = \"%s\" isn't on or off; %s is used\n", key, text, fallback ? "on" : "off");
    return fallback;
}

/* ---------------------------------------------------------------------------------------------------------------
 * Changing and saving settings (the F2 window)
 * --------------------------------------------------------------------------------------------------------------- */

int fh_settings_set(const char *key, const char *value)
{
    int i;

    fh_settings_load();
    if (!known(key) || strlen(key) >= KEY_CHARS || strlen(value) >= VALUE_CHARS || strchr(value, '"'))
        return -1;
    for (i = 0; i < change_count; i++)
        if (strcmp(changes[i].key, key) == 0)
            break;
    if (i == change_count) {
        if (change_count == MAX_CHANGES)
            return -1;
        change_count++;
    }
    if (strcmp(changes[i].key, key) != 0 || strcmp(changes[i].value, value) != 0)
        generation++;
    snprintf(changes[i].key, sizeof changes[i].key, "%s", key);
    snprintf(changes[i].value, sizeof changes[i].value, "%s", value);
    return 0;
}

/* A value as fh.toml writes it: true, false and numbers bare, everything else in quotes. */
static void format_value(const char *value, char *out, size_t size)
{
    char *end;

    if (!strcmp(value, "true") || !strcmp(value, "false") ||
        (value[0] && (strtod(value, &end), *end == '\0')))
        snprintf(out, size, "%s", value);
    else
        snprintf(out, size, "\"%s\"", value);
}

/* The setting's name on a line ("name = ..." or, with commented, "# name = ..."), or NULL. */
static const char *line_names(const char *line, const char *name, int commented)
{
    size_t n = strlen(name);

    while (isspace((unsigned char)*line))
        line++;
    if (commented) {
        if (*line != '#')
            return NULL;
        line++;
        while (isspace((unsigned char)*line))
            line++;
    }
    if (strncmp(line, name, n) != 0)
        return NULL;
    line += n;
    while (*line == ' ' || *line == '\t')
        line++;
    return *line == '=' ? line : NULL;
}

/* "[section]" on a line: the section's name into out. */
static int line_section(const char *line, char *out, size_t size)
{
    const char *close;

    while (isspace((unsigned char)*line))
        line++;
    if (*line != '[' || !(close = strchr(line, ']')))
        return 0;
    snprintf(out, size, "%.*s", (int)(close - line - 1), line + 1);
    return 1;
}

#define MAX_LINES 2048

int fh_settings_save(void)
{
    static char lines[MAX_LINES][LINE_CHARS];
    char path[4200], tmp[4300], folder[4096];
    const char *given = fh_getenv_utf8("FH_SETTINGS");
    int count = 0, c, i;
    FILE *f;

    fh_settings_load();
    if (loaded_path[0])
        snprintf(path, sizeof path, "%s", loaded_path);
    else if (given && given[0])
        snprintf(path, sizeof path, "%s", given);
    else {
        program_folder(folder, sizeof folder);
        if (!folder[0])
            return -1;
        snprintf(path, sizeof path, "%s../fh.toml", folder);   /* an installed game's place */
    }
    if ((f = fh_fopen(path, "r")) != NULL) {
        while (count < MAX_LINES && fgets(lines[count], LINE_CHARS, f))
            count++;
        fclose(f);
    }
    for (c = 0; c < change_count; c++) {
        char section[KEY_CHARS] = "", name[KEY_CHARS], current[KEY_CHARS] = "", value[VALUE_CHARS + 2];
        const char *dot = strchr(changes[c].key, '.');
        int start = -1, end = count, found = -1, commented = -1, last = -1;

        if (dot) {
            snprintf(section, sizeof section, "%.*s", (int)(dot - changes[c].key), changes[c].key);
            snprintf(name, sizeof name, "%s", dot + 1);
        } else
            snprintf(name, sizeof name, "%s", changes[c].key);
        format_value(changes[c].value, value, sizeof value);
        /* the section's lines: [start, end); keys before the first section belong to none */
        if (!section[0])
            start = 0;
        for (i = 0; i < count; i++) {
            char here[KEY_CHARS];

            if (line_section(lines[i], here, sizeof here)) {
                if (start >= 0) {
                    end = i;
                    break;
                }
                snprintf(current, sizeof current, "%s", here);
                if (!strcmp(current, section))
                    start = i + 1;
                continue;
            }
        }
        if (start >= 0) {
            for (i = start; i < end; i++) {
                if (found < 0 && line_names(lines[i], name, 0))
                    found = i;
                else if (commented < 0 && line_names(lines[i], name, 1))
                    commented = i;
                if (lines[i][strspn(lines[i], " \t\r\n")])
                    last = i;
            }
        }
        if (found >= 0) {                       /* the line, its comment after the value kept */
            char comment[LINE_CHARS] = "";
            const char *hash = NULL, *p;
            int quoted = 0;

            for (p = lines[found]; *p; p++) {
                if (*p == '"')
                    quoted = !quoted;
                else if (*p == '#' && !quoted) {
                    hash = p;
                    break;
                }
            }
            /* a line too long for the file's line length is cut there (the return values say so; nothing else) */
            if (hash) {
                if (snprintf(comment, sizeof comment, "  %s", hash) >= (int)sizeof comment)
                    comment[sizeof comment - 2] = '\n';
            } else {
                snprintf(comment, sizeof comment, "\n");
            }
            {
                int indent = (int)strspn(lines[found], " \t");
                char rebuilt[LINE_CHARS];

                if (snprintf(rebuilt, sizeof rebuilt, "%.*s%s = %s%s", indent, lines[found], name, value, comment) >=
                    (int)sizeof rebuilt)
                    rebuilt[sizeof rebuilt - 2] = '\n';
                memcpy(lines[found], rebuilt, sizeof rebuilt);
            }
        } else if (commented >= 0) {            /* "# name = ..." made the setting */
            snprintf(lines[commented], LINE_CHARS, "%s = %s\n", name, value);
        } else if (count + 2 < MAX_LINES) {     /* a new line at the section's end, or a new section */
            int at = start >= 0 ? (last >= start ? last + 1 : start) : count;

            if (start < 0) {
                snprintf(lines[count++], LINE_CHARS, "\n");
                snprintf(lines[count++], LINE_CHARS, "[%s]\n", section);
                at = count;
            }
            memmove(lines[at + 1], lines[at], (size_t)(count - at) * LINE_CHARS);
            snprintf(lines[at], LINE_CHARS, "%s = %s\n", name, value);
            count++;
        }
    }
    snprintf(tmp, sizeof tmp, "%s.f2tmp", path);
    if (!(f = fh_fopen(tmp, "w")))
        return -1;
    for (i = 0; i < count; i++)
        fputs(lines[i], f);
    /* [waw-decomp/fh2] 11-23: fh_replace_file replaces the file at once on both systems (Windows' rename refuses to) */
    if (fclose(f) != 0 || fh_replace_file(tmp, path) != 0) {
        fh_remove(tmp);
        return -1;
    }
    snprintf(loaded_path, sizeof loaded_path, "%s", path);
    fprintf(stderr, "[settings] saved %d setting(s) to %s\n", change_count, path);
    return 0;
}

unsigned fh_settings_generation(void)
{
    return generation;
}

/* ---------------------------------------------------------------------------------------------------------------
 * [acct2-fh] 2026-11-14: for the console (fh_console.c)
 * --------------------------------------------------------------------------------------------------------------- */

int fh_settings_count(void)
{
    return KNOWN_COUNT;
}

const char *fh_settings_key(int index)
{
    return index >= 0 && index < KNOWN_COUNT ? known_keys[index].key : NULL;
}

const char *fh_settings_env(const char *key)
{
    int i = known_index(key);

    return i >= 0 ? known_keys[i].env : NULL;
}

const char *fh_settings_help(const char *key)
{
    int i;

    fh_settings_load();
    for (i = 0; i < setting_count; i++)
        if (strcmp(settings[i].key, key) == 0)
            return settings[i].help[0] ? settings[i].help : NULL;
    return NULL;
}

int fh_settings_source(const char *key)
{
    const char *env = fh_settings_env(key), *value;
    int i;

    fh_settings_load();
    for (i = 0; i < change_count; i++)
        if (strcmp(changes[i].key, key) == 0)
            return 'c';
    if (env && (value = getenv(env)) != NULL && value[0])
        return 'e';
    for (i = 0; i < setting_count; i++)
        if (strcmp(settings[i].key, key) == 0)
            return 'f';
    return 0;
}
