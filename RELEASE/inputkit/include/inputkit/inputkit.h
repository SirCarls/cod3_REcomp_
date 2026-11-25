/* inputkit - the C interface, for the ports written in C (Big Red One, Finest Hour). [input-port] 2026-11-24
 *
 * The same code as the C++ headers (keys.h, binds.h, mouse.h, stick.h, sens_scale.h); see them for the rules. Strings returned
 * here stay valid until the next call on the same object (or, for ik_key_name, the next ik_key_name on the same
 * thread). Every function is safe to call with a null object (it does nothing, or returns 0 / "").
 */
#pragma once

#include <stdint.h>

#include "inputkit/stick.h"

#ifdef __cplusplus
extern "C" {
#endif

/* --- keys (keys.h) ------------------------------------------------------------------------------------------ */
#define IK_NO_KEY 0
#define IK_MOUSE1 0x101
#define IK_MOUSE2 0x102
#define IK_MOUSE3 0x103
#define IK_MOUSE4 0x104
#define IK_MOUSE5 0x105
#define IK_WHEEL_UP 0x106
#define IK_WHEEL_DOWN 0x107
#define IK_KEY_CODE_COUNT 0x108

uint16_t ik_key_parse(const char *name);
const char *ik_key_name(uint16_t code);       /* "" for none */
int ik_key_is_combination(const char *name);
int ik_key_always_reserved(uint16_t code);
uint16_t ik_key_from_sdl_scancode(int scancode);
uint16_t ik_key_from_sdl_mouse_button(int button);

/* --- an action table and its keys (binds.h) ------------------------------------------------------------------- */
#define IK_SLOTS 2
#define IK_MAX_ACTIONS_PER_KEY 3

typedef struct ik_action {
    const char *id;         /* "fire" */
    const char *label;      /* "Fire" */
    uint8_t kind;           /* 0 on foot */
    const char *defaults;   /* key names, comma-separated: "V,MOUSE3"; "" none */
} ik_action;

typedef struct ik_binds ik_binds;

/* The table's pointers are kept, not copied: `actions` and its strings must outlive the object. */
ik_binds *ik_binds_new(const ik_action *actions, int count, const char *setting_name);
void ik_binds_free(ik_binds *b);
/* Text -> keys ("" = the defaults). Returns how many notes (things dropped); ik_binds_note reads them. */
int ik_binds_parse(ik_binds *b, const char *text);
const char *ik_binds_note(const ik_binds *b, int i);
/* One action's keys from "B" / "B, MOUSE4" / "" (F2 or a [binds] line): 1 done, 0 refused (nothing changed); the
 * reason or what else the keys do in ik_binds_last_note. */
int ik_binds_assign_text(ik_binds *b, int action, const char *text);
const char *ik_binds_last_note(const ik_binds *b);
int ik_binds_action_by_id(const ik_binds *b, const char *id);  /* -1 none */
uint16_t ik_binds_key(const ik_binds *b, int action, int slot);
const char *ik_binds_keys_text(ik_binds *b, int action);       /* "V, MOUSE3" */
/* The actions `key` does (table order), at most `max`; returns the count. */
int ik_binds_actions_of_key(const ik_binds *b, uint16_t key, int *out, int max);
const char *ik_binds_serialize(ik_binds *b);                   /* "" when the defaults */

/* --- the mouse's motion (mouse.h) ------------------------------------------------------------------------------ */
typedef struct ik_motion ik_motion;

ik_motion *ik_motion_new(void);
void ik_motion_free(ik_motion *m);
void ik_motion_add(ik_motion *m, float dx, float dy, int64_t now_ns, int resample);
void ik_motion_take(ik_motion *m, int64_t now_ns, float *dx, float *dy);
void ik_motion_clear(ik_motion *m);

typedef struct ik_turn {
    double sensitivity, m_yaw, m_pitch, zoom_ratio, zoom_sensitivity, multiplier, scale_x, scale_y;
    int invert;
} ik_turn;
/* sensitivity 5, m_yaw / m_pitch 0.022, the rest 1, not inverted. */
void ik_turn_defaults(ik_turn *t);
void ik_turn_from_counts(const ik_turn *t, float dx, float dy, float *yaw_delta, float *pitch_delta);
double ik_zoom_ratio(double fov, double base_fov);

/* --- other games' sensitivity scales (sens_scale.h) [input-port] 2026-11-24 ------------------------------------- */
#define IK_SENS_SCALE_COUNT 12
#define IK_ZOOM_GAME 0
#define IK_ZOOM_MONITOR_DISTANCE 1
/* Scales by index, 0 to IK_SENS_SCALE_COUNT - 1 (0 is our own 0.022 scale); an index out of range reads as 0. */
const char *ik_sens_scale_id(int scale);     /* "valorant" */
const char *ik_sens_scale_label(int scale);  /* "Valorant" */
const char *ik_sens_scale_games(int scale);
void ik_sens_scale_range(int scale, double *min, double *max, double *step);
int ik_sens_scale_zoom(int scale, double *coefficient); /* IK_ZOOM_*; the coefficient for monitor distance */
int ik_sens_scale_from_id(const char *id);   /* unknown or null: 0 */
double ik_sens_degrees_per_count(int scale, double value);
double ik_sens_to_source(int scale, double value);
double ik_sens_from_source(int scale, double source);
double ik_sens_convert(int from_scale, int to_scale, double value);
double ik_monitor_distance_ratio(double hip_vfov_deg, double zoom_vfov_deg, double coefficient);
double ik_vertical_fov(double hfov_deg, double aspect);

#ifdef __cplusplus
}
#endif
