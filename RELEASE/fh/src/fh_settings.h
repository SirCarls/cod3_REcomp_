/* [acct2-fh] 2026-11-13 Finest Hour port: the player's settings, from fh.toml, with the FH_* environment variables on
 * top. See fh_settings.c and fh/config/fh.toml (every setting, with its default and its variable). The same file and
 * calls as Big Red One's bro_settings (bro/src/settings), so the F2 window can keep both the same way. */
#pragma once

/* Reads the settings file once: FH_SETTINGS (a file's path) if set, else fh.toml in the program's own folder or the
 * folder above it (an installed game has bin/fh and fh.toml next to bin/). Safe to call again; the later calls do
 * nothing. The getters below call it themselves. */
void fh_settings_load(void);

/* A setting as text: the environment variable env_name when it is set (and env_name isn't NULL), else the file's
 * "section.name" key, else NULL. The text lives until the program ends. */
const char *fh_setting_text(const char *key, const char *env_name);

/* The same as a number, a whole number or a switch, with fallback when it is missing or can't be read as one.
 * Switches take 1/0, true/false, yes/no, on/off. */
double fh_setting_number(const char *key, const char *env_name, double fallback);
int fh_setting_int(const char *key, const char *env_name, int fallback);
int fh_setting_bool(const char *key, const char *env_name, int fallback);

/* 1 when the setting is given at all (by its variable or the file). */
int fh_setting_given(const char *key, const char *env_name);

/* A setting changed while the game runs (the F2 window, later). It wins over the file and over its environment
 * variable from now on, and fh_settings_generation() counts it, so readers that keep a value can see it changed.
 * 0, or -1 for a key the port doesn't have or a value that can't be kept (too long, or with a quote). */
int fh_settings_set(const char *key, const char *value);

/* How many times fh_settings_set has changed something: a reader compares it with the last it saw. */
unsigned fh_settings_generation(void);

/* The settings changed with fh_settings_set written into fh.toml (the file that was read; else FH_SETTINGS; else
 * <program folder>/../fh.toml), every other line and comment kept. 0, or -1 when it can't be written. */
int fh_settings_save(void);

/* [acct2-fh] 2026-11-14: for the console (fh_console.c), which lists, shows and sets the settings by name. */

/* The port's settings by number, 0 to fh_settings_count() - 1: every key the port reads ("display.max_fps"). */
int fh_settings_count(void);
const char *fh_settings_key(int index);

/* A setting's environment variable ("FH_MAX_FPS"), or NULL when it has none (or there is no such setting). */
const char *fh_settings_env(const char *key);

/* What the settings file says about it: fh.toml's comment lines just above it, joined into one line; NULL when the
 * file has none for it (or has no such line). */
const char *fh_settings_help(const char *key);

/* Where its value comes from now: 'c' changed while the game runs (the F2 window or the console), 'e' its
 * environment variable, 'f' the file, or 0 when it isn't given (its reader's own default). */
int fh_settings_source(const char *key);
