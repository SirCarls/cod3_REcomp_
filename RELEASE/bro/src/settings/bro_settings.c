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
 * port reads version 1, and a newer file is read as far as it can be. */
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

#define MAX_SETTINGS   128
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
};

static struct {
    char key[KEY_CHARS];
    char value[VALUE_CHARS];
} settings[MAX_SETTINGS];
static int setting_count;
static int loaded;

static int known(const char *key)
{
    size_t i;

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
