/* [acct2-bro-game] Big Red One port: the player's settings, from bro.toml, with the BRO_* environment variables on
 * top. See bro_settings.c and bro/config/bro.toml (every setting, with its default and its variable). */
#pragma once

/* Reads the settings file once: BRO_SETTINGS (a file's path) if set, else bro.toml in the program's own folder or
 * the folder above it (an installed game has bin/bro and bro.toml next to bin/). Safe to call again; the later
 * calls do nothing. The getters below call it themselves. */
void bro_settings_load(void);

/* A setting as text: the environment variable env_name when it is set (and env_name isn't NULL), else the file's
 * "section.name" key, else NULL. The text lives until the program ends. */
const char *bro_setting_text(const char *key, const char *env_name);

/* The same as a number, a whole number or a switch, with fallback when it is missing or can't be read as one.
 * Switches take 1/0, true/false, yes/no, on/off. */
double bro_setting_number(const char *key, const char *env_name, double fallback);
int bro_setting_int(const char *key, const char *env_name, int fallback);
int bro_setting_bool(const char *key, const char *env_name, int fallback);

/* 1 when the setting is given at all (by its variable or the file). */
int bro_setting_given(const char *key, const char *env_name);

/* [f2-menus] 2026-11-09 (the F2 window): a setting changed while the game runs. It wins over the file and over
 * its environment variable from now on (the player just chose it). 0, or -1 for a key the port doesn't have or a value
 * that can't be kept (too long, or with a quote). The setting's reader takes it at its next read (its reload). */
int bro_settings_set(const char *key, const char *value);

/* The settings changed with bro_settings_set written into bro.toml (the file that was read; else BRO_SETTINGS; else
 * <program folder>/../bro.toml), every other line and comment kept: a key's line gets the new value, a "# key = ..."
 * line in its section becomes the setting, or the line goes at its section's end (a new section if need be). Written
 * through a temporary file and a rename. 0, or -1 when it can't be written. */
int bro_settings_save(void);

/* [f2-menus] Every setting whose key starts with prefix ("cvars."): fn(the rest of its key, its value, user). The ones
 * changed this run first, then the file's. */
void bro_settings_each(const char *prefix, void (*fn)(const char *name, const char *value, void *user), void *user);
