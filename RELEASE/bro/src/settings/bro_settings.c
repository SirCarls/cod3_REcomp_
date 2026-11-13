/* [acct2-bro-game] Big Red One port: the player's settings.
 *
 * The settings live in bro.toml, a small TOML file the player (or the setup tool) writes:
 *
 *     version = 1
 *     [display]
 *     max_fps = 0          # 0: no limit
 *     show_fps = true
 *
 * Only what this port needs of TOML is read: [section] lines, key = value lines, values that are a number, true or
 * false, or a "quoted string", and # comments. A key is asked for as "section.name" ("display.max_fps").
 *
 * Every setting also has an environment variable (BRO_MAX_FPS, BRO_SHOW_FPS, ...): when it is set, it wins. The
 * variables were the port's only settings before this file, and the setup tool's Play page and the test runs still
 * pass them, so a variable always means what it meant.
 *
 * The file: BRO_SETTINGS (its path) when set; else bro.toml in the folder the program is in, or the folder above it
 * (an installed game is <output>/bro/bin/bro with <output>/bro/bro.toml). No file means every setting's default.
 * Keys the port doesn't know are reported once, so a mistyped one is noticed. "version" is the file's format; this
 * port reads version 1, and a newer file is read as far as it can be.
 *
 * [f2-menus] 2026-11-09 (in the project since 2026-11-10, owner 15:20): the F2 window changes settings while the
 * game runs (bro_settings_set, which then also win over their variables) and writes them back into the file it read,
 * comments kept (bro_settings_save). The known keys below also get the ones bro/config/bro.toml has had since
 * 11-08 (they were left out when read from the file) and the window's own [interface] keys. */
#include "bro_settings.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>
#else
#include <unistd.h>
#endif

#define MAX_SETTINGS   512     /* [f2-menus] 128 -> 512: [cvars] (the game's own, as the F2 window keeps them) */
#define KEY_CHARS      64
#define VALUE_CHARS    256
#define LINE_CHARS     512
#define FILE_VERSION   1

/* Every key the port reads; the others are reported once. Keep in step with bro/config/bro.toml. */
static const char *const known_keys[] = {
    "version",
    "display.max_fps", "display.show_fps", "display.fps_log",
    "display.vsync", "display.fullscreen", "display.window", "display.size",
    "input.dead_zone", "input.raw_mouse", "input.mouse_sensitivity", "input.mouse_scale_x", "input.mouse_scale_y",
    "input.invert_y", "input.mouse_speed",
    /* the controller's button for each of the pad's inputs (bro_pad.c) */
    "pad.up", "pad.down", "pad.left", "pad.right", "pad.start", "pad.back", "pad.lthumb", "pad.rthumb",
    "pad.a", "pad.b", "pad.x", "pad.y", "pad.black", "pad.white", "pad.lt", "pad.rt",
    /* the keys and mouse buttons for each of them, and for the sticks' directions */
    "keys.up", "keys.down", "keys.left", "keys.right", "keys.start", "keys.back", "keys.lthumb", "keys.rthumb",
    "keys.a", "keys.b", "keys.x", "keys.y", "keys.black", "keys.white", "keys.lt", "keys.rt",
    "keys.move_forward", "keys.move_back", "keys.move_left", "keys.move_right",
    "keys.look_up", "keys.look_down", "keys.look_left", "keys.look_right",
    /* [f2-menus] in bro/config/bro.toml since 11-08, but missing here */
    "display.fov", "display.movie_pacing", "display.skip_movies", "display.start_in_background",
    "input.zoom_sensitivity", "input.binds", "input.binds_kbm", "input.binds_menu_check",
    "binds.fire", "binds.aim", "binds.use", "binds.reload", "binds.usereload", "binds.melee", "binds.usemelee",
    "binds.frag", "binds.sprint", "binds.leanleft", "binds.leanright", "binds.leanleftnext", "binds.leanrightnext",
    "binds.jump", "binds.moveup", "binds.usemoveup", "binds.crouch", "binds.crouchhold", "binds.prone",
    "binds.pronehold", "binds.lowerstance", "binds.raisestance", "binds.weapnext", "binds.weapprev", "binds.weapalt",
    "binds.scores",
    /* [f2-menus] the F2 window's own */
    "interface.theme", "interface.text_size", "interface.opacity", "interface.pad_shortcut",
};

static struct {
    char key[KEY_CHARS];
    char value[VALUE_CHARS];
} settings[MAX_SETTINGS];
static int setting_count;
static int loaded;

/* [f2-menus] the values set while the game runs (they win over the variables too, since the player chose them
 * just now), and the file they go back into. */
#define MAX_CHANGES    512
static struct {
    char key[KEY_CHARS];
    char value[VALUE_CHARS];
} changes[MAX_CHANGES];
static int change_count;
static char loaded_path[4200];

static int known(const char *key)
{
    size_t i;

    /* [f2-menus] [cvars]: any of the game's own cvars (bro_game_cvars.c), by its name */
    if (!strncmp(key, "cvars.", 6) && key[6]) {
        for (i = 6; key[i]; i++)
            if (!isalnum((unsigned char)key[i]) && key[i] != '_')
                return 0;
        return 1;
    }

    /* [f2-menus] 2026-11-11 [mouse]: the in-game mouse acceleration, the sensitivity randomizer and the sensitivity
     * scales (the settings the shared f2_accel.cpp makes itself, kept by bro_f2_store.cpp), by name */
    if (!strncmp(key, "mouse.", 6) && key[6]) {
        for (i = 6; key[i]; i++)
            if (!isalnum((unsigned char)key[i]) && key[i] != '_')
                return 0;
        return 1;
    }

    for (i = 0; i < sizeof known_keys / sizeof known_keys[0]; i++)
        if (strcmp(known_keys[i], key) == 0)
            return 1;
    return 0;
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

static void keep(const char *section, const char *name, const char *value, const char *path, int line_number)
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
}

static int read_file(const char *path)
{
    FILE *f = fopen(path, "r");
    char line[LINE_CHARS], section[KEY_CHARS] = "";
    int number = 0, version;
    const char *text;

    if (!f)
        return 0;
    while (fgets(line, sizeof line, f)) {
        char *s, *equals, *name, *value;

        number++;
        cut_comment(line);
        s = trim(line);
        if (!*s)
            continue;
        if (*s == '[') {
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
        keep(section, name, value, path, number);
    }
    fclose(f);
    snprintf(loaded_path, sizeof loaded_path, "%s", path);    /* [f2-menus] where Save writes */
    text = bro_setting_text("version", NULL);
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

    out[0] = '\0';
#if defined(_WIN32)
    {
        DWORD n = GetModuleFileNameA(NULL, out, (DWORD)size);

        if (n == 0 || n >= size) {
            out[0] = '\0';
            return;
        }
    }
    for (slash = out; *slash; slash++)
        if (*slash == '\\')
            *slash = '/';
#else
    {
        ssize_t n = readlink("/proc/self/exe", out, size - 1);

        if (n <= 0) {
            out[0] = '\0';
            return;
        }
        out[n] = '\0';
    }
#endif
    slash = strrchr(out, '/');
    if (slash)
        slash[1] = '\0';
    else
        out[0] = '\0';
}

void bro_settings_load(void)
{
    const char *given;
    char folder[4096], path[4200];

    if (loaded)
        return;
    loaded = 1;
    given = getenv("BRO_SETTINGS");
    if (given && given[0]) {
        if (!read_file(given))
            fprintf(stderr, "[settings] BRO_SETTINGS: can't read %s; every setting at its default\n", given);
        return;
    }
    program_folder(folder, sizeof folder);
    if (!folder[0])
        return;
    snprintf(path, sizeof path, "%sbro.toml", folder);
    if (read_file(path))
        return;
    snprintf(path, sizeof path, "%s../bro.toml", folder);
    read_file(path);
}

const char *bro_setting_text(const char *key, const char *env_name)
{
    const char *value;
    int i;

    bro_settings_load();
    for (i = 0; i < change_count; i++)       /* [f2-menus] set in the F2 window */
        if (strcmp(changes[i].key, key) == 0)
            return changes[i].value;
    if (env_name && (value = getenv(env_name)) != NULL && value[0])
        return value;
    for (i = 0; i < setting_count; i++)
        if (strcmp(settings[i].key, key) == 0)
            return settings[i].value;
    return NULL;
}

int bro_setting_given(const char *key, const char *env_name)
{
    return bro_setting_text(key, env_name) != NULL;
}

double bro_setting_number(const char *key, const char *env_name, double fallback)
{
    const char *text = bro_setting_text(key, env_name);
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

int bro_setting_int(const char *key, const char *env_name, int fallback)
{
    double value = bro_setting_number(key, env_name, (double)fallback);

    return (int)(value < 0.0 ? value - 0.5 : value + 0.5);
}

int bro_setting_bool(const char *key, const char *env_name, int fallback)
{
    const char *text = bro_setting_text(key, env_name);
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
 * [f2-menus] 2026-11-09: changing and saving settings (the F2 window)
 * --------------------------------------------------------------------------------------------------------------- */

int bro_settings_set(const char *key, const char *value)
{
    int i;

    bro_settings_load();
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
    snprintf(changes[i].key, sizeof changes[i].key, "%s", key);
    snprintf(changes[i].value, sizeof changes[i].value, "%s", value);
    return 0;
}

/* A value as bro.toml writes it: true, false and numbers bare, everything else in quotes. */
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

int bro_settings_save(void)
{
    static char lines[MAX_LINES][LINE_CHARS];
    char path[4200], tmp[4300], folder[4096];
    const char *given = getenv("BRO_SETTINGS");
    int count = 0, c, i;
    FILE *f;

    bro_settings_load();
    if (loaded_path[0])
        snprintf(path, sizeof path, "%s", loaded_path);
    else if (given && given[0])
        snprintf(path, sizeof path, "%s", given);
    else {
        program_folder(folder, sizeof folder);
        if (!folder[0])
            return -1;
        snprintf(path, sizeof path, "%s../bro.toml", folder);   /* an installed game's place */
    }
    if ((f = fopen(path, "r")) != NULL) {
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
            if (hash)
                snprintf(comment, sizeof comment, "  %s", hash);
            else
                snprintf(comment, sizeof comment, "\n");
            {
                int indent = (int)strspn(lines[found], " \t");
                char rebuilt[LINE_CHARS];

                snprintf(rebuilt, sizeof rebuilt, "%.*s%s = %s%s", indent, lines[found], name, value, comment);
                snprintf(lines[found], LINE_CHARS, "%s", rebuilt);
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
    if (!(f = fopen(tmp, "w")))
        return -1;
    for (i = 0; i < count; i++)
        fputs(lines[i], f);
#if defined(_WIN32)
    if (fclose(f) == 0)
        remove(path);       /* Windows' rename doesn't replace a file */
    else
        f = NULL;
    if (!f || rename(tmp, path) != 0) {
#else
    if (fclose(f) != 0 || rename(tmp, path) != 0) {
#endif
        remove(tmp);
        return -1;
    }
    snprintf(loaded_path, sizeof loaded_path, "%s", path);
    fprintf(stderr, "[settings] saved %d setting(s) to %s\n", change_count, path);
    return 0;
}

/* [f2-menus] Every setting whose key starts with prefix ("cvars."), the part after it and its value: the ones changed
 * this run (bro_settings_set) first, then the file's that weren't. */
void bro_settings_each(const char *prefix, void (*fn)(const char *name, const char *value, void *user), void *user)
{
    size_t n = strlen(prefix);
    int i, j;

    bro_settings_load();
    for (i = 0; i < change_count; i++)
        if (!strncmp(changes[i].key, prefix, n))
            fn(changes[i].key + n, changes[i].value, user);
    for (i = 0; i < setting_count; i++) {
        if (strncmp(settings[i].key, prefix, n))
            continue;
        for (j = 0; j < change_count; j++)
            if (!strcmp(changes[j].key, settings[i].key))
                break;
        if (j == change_count)
            fn(settings[i].key + n, settings[i].value, user);
    }
}
