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
