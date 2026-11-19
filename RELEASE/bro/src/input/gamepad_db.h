#ifndef REX_INPUT_SDL_GAMEPAD_DB_H
#define REX_INPUT_SDL_GAMEPAD_DB_H
/**
 * @file        rex/input/sdl/gamepad_db.h
 * @brief       Controller mappings (SDL GameControllerDB files), the face
 *              buttons of Nintendo-style pads, and why a pad isn't usable.
 *
 * [rexglue-native-input] 2026-11-16 (owner 14:27: "SDL_GameControllerDB ...
 * might help with controller support and mappings for gamecube or switch
 * controllers"; 13:26/14:16: Linux and Windows, I/O errors checked).
 *
 * Plain C that also compiles as C++, SDL3 only (no SDK types), so the SDK's
 * SDL input driver, Big Red One's pad (bro/src/input) and a test program
 * share one copy of the logic. Everything is `static inline`.
 *
 * Mapping files are SDL GameControllerDB text: one mapping a line,
 * "GUID,name,a:b0,b:b1,...,platform:Linux,". gdb_load_file reads a file
 * itself (UTF-8 path; SDL opens it, so non-ASCII Windows paths work), drops a
 * UTF-8 BOM and CR line ends, skips comments, blank lines, lines for other
 * systems and lines over 1024 bytes, and gives SDL the rest one by one, so
 * the log can say how many were new, replaced or refused. A missing file is
 * not an error; one that exists but can't be read is logged with SDL's
 * reason (permission denied, a folder, too big).
 *
 * Precedence (gdb_load_all), lowest first: SDL's built-in mappings, the
 * shipped file, the player's file, the file SDL_GAMECONTROLLERCONFIG_FILE
 * names (SDL reads it at start; read again here so it beats the two files),
 * then SDL_GAMECONTROLLERCONFIG's lines, which SDL keeps at a higher
 * priority than any file (Steam passes its own mappings that way).
 *
 * Face buttons: SDL3 reports buttons by position (SOUTH is the bottom one),
 * whatever is printed on them. gdb_face_map gives the Xbox 360 button each
 * position presses: by position (the bottom button is A, as on an Xbox pad)
 * or, for a pad whose letters differ (Switch: B A Y X; GameCube: A X B Y in
 * the SOUTH EAST WEST NORTH places), by the printed letter.
 *
 * Not thread safe beyond what SDL itself is; call from the thread that owns
 * the gamepads (the logging callback runs on the caller's thread).
 */

#include <SDL3/SDL.h>
#include <string.h> /* memchr: SDL has no SDL_memchr */

#if defined(__linux__)
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <unistd.h>
#endif

enum { GDB_INFO = 0, GDB_WARN = 1, GDB_ERROR = 2 };
typedef void (*gdb_log_fn)(void *ctx, int level, const char *text);

/* The Xbox 360 controller's button bits (XInput's wButtons). */
#define GDB_X_DPAD_UP 0x0001
#define GDB_X_DPAD_DOWN 0x0002
#define GDB_X_DPAD_LEFT 0x0004
#define GDB_X_DPAD_RIGHT 0x0008
#define GDB_X_START 0x0010
#define GDB_X_BACK 0x0020
#define GDB_X_LEFT_THUMB 0x0040
#define GDB_X_RIGHT_THUMB 0x0080
#define GDB_X_LEFT_SHOULDER 0x0100
#define GDB_X_RIGHT_SHOULDER 0x0200
#define GDB_X_GUIDE 0x0400
#define GDB_X_A 0x1000
#define GDB_X_B 0x2000
#define GDB_X_X 0x4000
#define GDB_X_Y 0x8000

#define GDB_MAX_LINE 1024
#define GDB_MAX_FILE (8 * 1024 * 1024)

typedef struct gdb_file_stats {
  int found;            /* the path exists */
  int loaded;           /* it was read */
  int lines;            /* mapping lines (not blank, not comments) */
  int this_platform;    /* lines for this system, given to SDL */
  int other_platforms;  /* lines for other systems, skipped */
  int no_platform;      /* lines without a platform: field, skipped (as SDL does) */
  int too_long;         /* lines over GDB_MAX_LINE bytes, skipped */
  int added;            /* SDL had no mapping for that GUID */
  int replaced;         /* SDL had one: replaced (or kept, when SDL_GAMECONTROLLERCONFIG's wins) */
  int rejected;         /* SDL refused the line */
} gdb_file_stats;

static inline void gdb_logf(gdb_log_fn log, void *ctx, int level, SDL_PRINTF_FORMAT_STRING const char *fmt, ...)
    SDL_PRINTF_VARARG_FUNC(4);
static inline void gdb_logf(gdb_log_fn log, void *ctx, int level, const char *fmt, ...) {
  char text[1024];
  va_list ap;
  if (!log) {
    return;
  }
  va_start(ap, fmt);
  SDL_vsnprintf(text, sizeof(text), fmt, ap);
  va_end(ap);
  log(ctx, level, text);
}

/* 1: the line's platform: field names this system; 0: another system; -1: no field. */
static inline int gdb_platform_of_line(const char *line) {
  const char *field = SDL_strstr(line, "platform:");
  const char *end;
  const char *self = SDL_GetPlatform();
  size_t len;
  if (!field) {
    return -1;
  }
  field += 9;
  end = SDL_strchr(field, ',');
  len = end ? (size_t)(end - field) : SDL_strlen(field);
  return (len == SDL_strlen(self) && SDL_strncasecmp(field, self, len) == 0) ? 1 : 0;
}

/* Gives SDL the mappings in `text` (changed in place: line ends become NULs). */
static inline void gdb_load_text(char *text, size_t size, const char *what, const char *path, gdb_log_fn log,
                                 void *ctx, gdb_file_stats *stats) {
  char *line = text;
  char *end = text + size;
  int shown = 0;
  if (size >= 3 && (unsigned char)text[0] == 0xEF && (unsigned char)text[1] == 0xBB &&
      (unsigned char)text[2] == 0xBF) {
    line += 3; /* UTF-8 BOM */
  }
  while (line < end) {
    char *nl = (char *)memchr(line, '\n', (size_t)(end - line));
    char *stop = nl ? nl : end;
    char *next = nl ? nl + 1 : end;
    size_t len;
    int platform, result;
    while (stop > line && (stop[-1] == '\r' || stop[-1] == ' ' || stop[-1] == '\t')) {
      --stop;
    }
    while (line < stop && (*line == ' ' || *line == '\t')) {
      ++line;
    }
    *stop = '\0';
    len = (size_t)(stop - line);
    if (len == 0 || line[0] == '#') {
      line = next;
      continue;
    }
    stats->lines++;
    if (len > GDB_MAX_LINE || memchr(line, '\0', len) != NULL) {
      stats->too_long++;
      line = next;
      continue;
    }
    platform = gdb_platform_of_line(line);
    if (platform < 0) {
      stats->no_platform++;
    } else if (platform == 0) {
      stats->other_platforms++;
    } else {
      stats->this_platform++;
      result = SDL_AddGamepadMapping(line);
      if (result > 0) {
        stats->added++;
      } else if (result == 0) {
        stats->replaced++;
      } else {
        stats->rejected++;
        if (shown++ < 3) {
          gdb_logf(log, ctx, GDB_WARN, "%s %s: SDL refused a line (%s): %.80s", what, path, SDL_GetError(), line);
        }
      }
    }
    line = next;
  }
}

/* Loads one mapping file. 1: read; 0: not there (not an error); -1: there but unreadable (logged). */
static inline int gdb_load_file(const char *path, const char *what, gdb_log_fn log, void *ctx, gdb_file_stats *out) {
  SDL_PathInfo info;
  gdb_file_stats stats;
  size_t size = 0;
  char *data;
  SDL_memset(&stats, 0, sizeof(stats));
  if (out) {
    *out = stats;
  }
  if (!path || !path[0]) {
    return 0;
  }
  if (!SDL_GetPathInfo(path, &info)) {
    gdb_logf(log, ctx, GDB_INFO, "%s: none at %s", what, path);
    return 0;
  }
  stats.found = 1;
  if (info.type != SDL_PATHTYPE_FILE) {
    gdb_logf(log, ctx, GDB_WARN, "%s: %s is not a file; not read", what, path);
    if (out) {
      *out = stats;
    }
    return -1;
  }
  if (info.size > GDB_MAX_FILE) {
    gdb_logf(log, ctx, GDB_WARN, "%s: %s is %llu bytes, more than a mapping file can be (%d); not read", what, path,
             (unsigned long long)info.size, GDB_MAX_FILE);
    if (out) {
      *out = stats;
    }
    return -1;
  }
  data = (char *)SDL_LoadFile(path, &size);
  if (!data) {
    gdb_logf(log, ctx, GDB_WARN, "%s: can't read %s: %s", what, path, SDL_GetError());
    if (out) {
      *out = stats;
    }
    return -1;
  }
  stats.loaded = 1;
  gdb_load_text(data, size, what, path, log, ctx, &stats);
  SDL_free(data);
  gdb_logf(log, ctx, GDB_INFO,
           "%s: %s: %d mappings for %s (%d new, %d replacing one SDL had, %d refused), %d for other systems "
           "skipped%s",
           what, path, stats.this_platform, SDL_GetPlatform(), stats.added, stats.replaced, stats.rejected,
           stats.other_platforms,
           (stats.no_platform || stats.too_long) ? "; some lines skipped (no platform: field, or too long)" : "");
  if (out) {
    *out = stats;
  }
  return 1;
}

/* At start, once SDL_INIT_GAMEPAD is up and before any gamepad is opened: the shipped file, the player's file,
 * SDL_GAMECONTROLLERCONFIG_FILE again, and a word on SDL_GAMECONTROLLERCONFIG. Either path may be NULL or "". */
static inline void gdb_load_all(const char *shipped, const char *user, gdb_log_fn log, void *ctx) {
  const char *env_file = SDL_GetHint(SDL_HINT_GAMECONTROLLERCONFIG_FILE);
  const char *env_text = SDL_GetHint(SDL_HINT_GAMECONTROLLERCONFIG);
  int count = 0;
  char **all;
  if (shipped && shipped[0]) {
    gdb_load_file(shipped, "Controller mappings (shipped)", log, ctx, NULL);
  }
  if (user && user[0] && !(shipped && SDL_strcmp(shipped, user) == 0)) {
    gdb_load_file(user, "Controller mappings (yours)", log, ctx, NULL);
  }
  if (env_file && env_file[0]) {
    gdb_load_file(env_file, "Controller mappings (SDL_GAMECONTROLLERCONFIG_FILE)", log, ctx, NULL);
  }
  if (env_text && env_text[0]) {
    int lines = 1;
    const char *c;
    for (c = env_text; *c; ++c) {
      lines += (*c == '\n' && c[1] != '\0');
    }
    gdb_logf(log, ctx, GDB_INFO,
             "Controller mappings (SDL_GAMECONTROLLERCONFIG): %d line(s), read by SDL at start; they win over "
             "every file",
             lines);
  }
  all = SDL_GetGamepadMappings(&count);
  SDL_free(all);
  gdb_logf(log, ctx, GDB_INFO, "Controller mappings: SDL %d.%d.%d knows %d in all (built-in and loaded)",
           SDL_VERSIONNUM_MAJOR(SDL_GetVersion()), SDL_VERSIONNUM_MINOR(SDL_GetVersion()),
           SDL_VERSIONNUM_MICRO(SDL_GetVersion()), count);
}

/* ---- face buttons ---------------------------------------------------------------------------------------------- */

enum { GDB_FACE_BY_POSITION = 0, GDB_FACE_BY_LABEL = 1 };

static inline Uint16 gdb_label_bit(SDL_GamepadButtonLabel label) {
  switch (label) {
    case SDL_GAMEPAD_BUTTON_LABEL_A:
      return GDB_X_A;
    case SDL_GAMEPAD_BUTTON_LABEL_B:
      return GDB_X_B;
    case SDL_GAMEPAD_BUTTON_LABEL_X:
      return GDB_X_X;
    case SDL_GAMEPAD_BUTTON_LABEL_Y:
      return GDB_X_Y;
    default:
      return 0;
  }
}

/* face[i]: the Xbox 360 button SDL face button i (SOUTH, EAST, WEST, NORTH) presses. By position: A B X Y, as on an
 * Xbox pad. By label: the button printed A presses A, and so on, when all four carry A B X Y in another order (a
 * Nintendo or GameCube pad); any other pad stays by position. Returns 1 when the pad's letters differ from the Xbox
 * places (whichever mode), 0 otherwise. */
static inline int gdb_face_map(SDL_Gamepad *pad, int mode, Uint16 face[4]) {
  static const Uint16 kByPosition[4] = {GDB_X_A, GDB_X_B, GDB_X_X, GDB_X_Y};
  Uint16 labeled[4];
  Uint16 seen = 0;
  int differs = 0;
  int i;
  for (i = 0; i < 4; ++i) {
    face[i] = kByPosition[i];
    labeled[i] = gdb_label_bit(SDL_GetGamepadButtonLabel(pad, (SDL_GamepadButton)i));
    seen = (Uint16)(seen | labeled[i]);
    if (labeled[i] && labeled[i] != kByPosition[i]) {
      differs = 1;
    }
  }
  if (mode == GDB_FACE_BY_LABEL && differs && seen == (GDB_X_A | GDB_X_B | GDB_X_X | GDB_X_Y)) {
    for (i = 0; i < 4; ++i) {
      face[i] = labeled[i];
    }
  }
  return differs;
}

/* The Xbox 360 button SDL gamepad button `button` presses; 0: none (MISC2-6, among them the GameCube's L and R
 * end clicks, MISC3 and MISC4: its analog L and R are already the triggers). Paddles are the Xbox Elite's
 * (Y B X A, as the SDK had them); MISC1 (Switch capture, Xbox share) and the touchpad click are the guide button. */
static inline Uint16 gdb_xinput_bit(int button, const Uint16 face[4]) {
  switch (button) {
    case SDL_GAMEPAD_BUTTON_SOUTH:
    case SDL_GAMEPAD_BUTTON_EAST:
    case SDL_GAMEPAD_BUTTON_WEST:
    case SDL_GAMEPAD_BUTTON_NORTH:
      return face[button];
    case SDL_GAMEPAD_BUTTON_BACK:
      return GDB_X_BACK;
    case SDL_GAMEPAD_BUTTON_GUIDE:
      return GDB_X_GUIDE;
    case SDL_GAMEPAD_BUTTON_START:
      return GDB_X_START;
    case SDL_GAMEPAD_BUTTON_LEFT_STICK:
      return GDB_X_LEFT_THUMB;
    case SDL_GAMEPAD_BUTTON_RIGHT_STICK:
      return GDB_X_RIGHT_THUMB;
    case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER:
      return GDB_X_LEFT_SHOULDER;
    case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER:
      return GDB_X_RIGHT_SHOULDER;
    case SDL_GAMEPAD_BUTTON_DPAD_UP:
      return GDB_X_DPAD_UP;
    case SDL_GAMEPAD_BUTTON_DPAD_DOWN:
      return GDB_X_DPAD_DOWN;
    case SDL_GAMEPAD_BUTTON_DPAD_LEFT:
      return GDB_X_DPAD_LEFT;
    case SDL_GAMEPAD_BUTTON_DPAD_RIGHT:
      return GDB_X_DPAD_RIGHT;
    case SDL_GAMEPAD_BUTTON_MISC1:
      return GDB_X_GUIDE;
    case SDL_GAMEPAD_BUTTON_RIGHT_PADDLE1:
      return GDB_X_Y;
    case SDL_GAMEPAD_BUTTON_LEFT_PADDLE1:
      return GDB_X_B;
    case SDL_GAMEPAD_BUTTON_RIGHT_PADDLE2:
      return GDB_X_X;
    case SDL_GAMEPAD_BUTTON_LEFT_PADDLE2:
      return GDB_X_A;
    case SDL_GAMEPAD_BUTTON_TOUCHPAD:
      return GDB_X_GUIDE;
    default:
      return 0;
  }
}

/* The Xbox 360 buttons held, from the SDL buttons held (bit n = SDL_GamepadButton n). The guide button only with
 * `guide` (the SDK's guide_button setting). */
static inline Uint16 gdb_xinput_buttons(Uint32 sdl_down, const Uint16 face[4], int guide) {
  Uint16 out = 0;
  int b;
  for (b = 0; b < SDL_GAMEPAD_BUTTON_COUNT && b < 32; ++b) {
    if (sdl_down & ((Uint32)1 << b)) {
      out = (Uint16)(out | gdb_xinput_bit(b, face));
    }
  }
  if (!guide) {
    out = (Uint16)(out & ~GDB_X_GUIDE);
  }
  return out;
}

/* The kind of buttons a gamepad wears, for the glyphs a game shows: the numbers are the SDK's ControllerFamily
 * (rex/input/device.h). By SDL's type; a pad of no known type (a third-party Switch or GameCube pad mapped by
 * gamecontrollerdb.txt) by the letters SDL gives its face buttons. */
enum {
  GDB_FAMILY_UNKNOWN = 0,
  GDB_FAMILY_XBOX = 1,
  GDB_FAMILY_PLAYSTATION = 2,
  GDB_FAMILY_NINTENDO = 3,
  GDB_FAMILY_GAMECUBE = 5
};

static inline int gdb_family(SDL_Gamepad *pad) {
  SDL_GamepadButtonLabel south, east;
  switch (SDL_GetGamepadType(pad)) {
    case SDL_GAMEPAD_TYPE_XBOX360:
    case SDL_GAMEPAD_TYPE_XBOXONE:
      return GDB_FAMILY_XBOX;
    case SDL_GAMEPAD_TYPE_PS3:
    case SDL_GAMEPAD_TYPE_PS4:
    case SDL_GAMEPAD_TYPE_PS5:
      return GDB_FAMILY_PLAYSTATION;
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO:
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_LEFT:
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_RIGHT:
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_PAIR:
      return GDB_FAMILY_NINTENDO;
    case SDL_GAMEPAD_TYPE_GAMECUBE:
      return GDB_FAMILY_GAMECUBE;
    default:
      break;
  }
  south = SDL_GetGamepadButtonLabel(pad, SDL_GAMEPAD_BUTTON_SOUTH);
  east = SDL_GetGamepadButtonLabel(pad, SDL_GAMEPAD_BUTTON_EAST);
  if (south == SDL_GAMEPAD_BUTTON_LABEL_B) {
    return GDB_FAMILY_NINTENDO;
  }
  if (south == SDL_GAMEPAD_BUTTON_LABEL_A && east == SDL_GAMEPAD_BUTTON_LABEL_X) {
    return GDB_FAMILY_GAMECUBE;
  }
  if (south == SDL_GAMEPAD_BUTTON_LABEL_CROSS) {
    return GDB_FAMILY_PLAYSTATION;
  }
  return GDB_FAMILY_UNKNOWN;
}

/* ---- one pad's buttons, event by event (the SDK driver's state per pad) ------------------------------------------ */

typedef struct gdb_pad {
  Uint32 sdl_down;          /* SDL buttons held: bit n = SDL_GamepadButton n */
  Uint32 unmapped_noted;    /* buttons with no Xbox 360 button already noted (log once) */
  Uint16 face_by_position[4];
  Uint16 face_by_label[4];
  int letters_differ;       /* the pad's letters aren't Xbox's in Xbox's places (Nintendo, GameCube) */
} gdb_pad;

static inline void gdb_pad_init(gdb_pad *p, SDL_Gamepad *pad) {
  SDL_memset(p, 0, sizeof(*p));
  p->letters_differ = gdb_face_map(pad, GDB_FACE_BY_POSITION, p->face_by_position);
  gdb_face_map(pad, GDB_FACE_BY_LABEL, p->face_by_label);
}

static inline const Uint16 *gdb_pad_face(const gdb_pad *p, int mode) {
  return mode == GDB_FACE_BY_LABEL ? p->face_by_label : p->face_by_position;
}

/* The Xbox 360 buttons held now, the face buttons read the `mode` way. */
static inline Uint16 gdb_pad_buttons(const gdb_pad *p, int mode, int guide) {
  return gdb_xinput_buttons(p->sdl_down, gdb_pad_face(p, mode), guide);
}

/* SDL button `button` went down or up. Returns the Xbox 360 buttons held now. *first_unmapped is set to 1 the first
 * time a button with no Xbox 360 button goes down on this pad (for one log line), else 0. */
static inline Uint16 gdb_pad_button(gdb_pad *p, int button, int down, int mode, int guide, int *first_unmapped) {
  Uint32 bit;
  if (first_unmapped) {
    *first_unmapped = 0;
  }
  if (button < 0 || button >= SDL_GAMEPAD_BUTTON_COUNT || button >= 32) {
    return gdb_pad_buttons(p, mode, guide); /* a newer SDL's button */
  }
  bit = (Uint32)1 << button;
  if (down) {
    p->sdl_down |= bit;
  } else {
    p->sdl_down &= ~bit;
  }
  if (down && !gdb_xinput_bit(button, gdb_pad_face(p, mode)) && !(p->unmapped_noted & bit)) {
    p->unmapped_noted |= bit;
    if (first_unmapped) {
      *first_unmapped = 1;
    }
  }
  return gdb_pad_buttons(p, mode, guide);
}

/* ---- names for the log ----------------------------------------------------------------------------------------- */

static inline const char *gdb_type_name(SDL_GamepadType type) {
  switch (type) {
    case SDL_GAMEPAD_TYPE_STANDARD:
      return "standard";
    case SDL_GAMEPAD_TYPE_XBOX360:
      return "Xbox 360";
    case SDL_GAMEPAD_TYPE_XBOXONE:
      return "Xbox One";
    case SDL_GAMEPAD_TYPE_PS3:
      return "PS3";
    case SDL_GAMEPAD_TYPE_PS4:
      return "PS4";
    case SDL_GAMEPAD_TYPE_PS5:
      return "PS5";
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO:
      return "Switch Pro";
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_LEFT:
      return "Joy-Con (L) alone";
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_RIGHT:
      return "Joy-Con (R) alone";
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_PAIR:
      return "Joy-Con pair";
    case SDL_GAMEPAD_TYPE_GAMECUBE:
      return "GameCube";
    default:
      return "unknown";
  }
}

static inline const char *gdb_label_name(SDL_GamepadButtonLabel label) {
  switch (label) {
    case SDL_GAMEPAD_BUTTON_LABEL_A:
      return "A";
    case SDL_GAMEPAD_BUTTON_LABEL_B:
      return "B";
    case SDL_GAMEPAD_BUTTON_LABEL_X:
      return "X";
    case SDL_GAMEPAD_BUTTON_LABEL_Y:
      return "Y";
    case SDL_GAMEPAD_BUTTON_LABEL_CROSS:
      return "Cross";
    case SDL_GAMEPAD_BUTTON_LABEL_CIRCLE:
      return "Circle";
    case SDL_GAMEPAD_BUTTON_LABEL_SQUARE:
      return "Square";
    case SDL_GAMEPAD_BUTTON_LABEL_TRIANGLE:
      return "Triangle";
    default:
      return "?";
  }
}

/* Which of SDL's joystick drivers a device came through, from its GUID. */
static inline const char *gdb_backend_name(SDL_GUID guid) {
  switch (guid.data[14]) {
    case 'h':
      return "HIDAPI";
    case 'v':
      return "virtual";
    case 'x':
      return "XInput";
    case 'r':
      return "RawInput";
    case 'w':
      return "Windows.Gaming.Input";
    case 'g':
      return "GameInput";
    case 'm':
      return "MFi";
    default:
#if defined(_WIN32)
      return "DirectInput";
#elif defined(__linux__)
      return "evdev";
#else
      return "system";
#endif
  }
}

static inline const char *gdb_xinput_name(Uint16 bit) {
  switch (bit) {
    case GDB_X_A:
      return "A";
    case GDB_X_B:
      return "B";
    case GDB_X_X:
      return "X";
    case GDB_X_Y:
      return "Y";
    default:
      return "-";
  }
}

/* One line about a gamepad SDL opened: its name, kind, IDs, driver, and the letter on each face button with the
 * Xbox button it presses. */
static inline void gdb_describe(SDL_Gamepad *pad, const Uint16 face[4], char *out, size_t size) {
  SDL_Joystick *joy = SDL_GetGamepadJoystick(pad);
  SDL_GUID guid = SDL_GetJoystickGUID(joy);
  char guid_text[33];
  const char *name = SDL_GetGamepadName(pad);
  SDL_GUIDToString(guid, guid_text, (int)sizeof(guid_text));
  SDL_snprintf(out, size,
               "\"%s\" (%s, %04x:%04x, %s, GUID %s): bottom %s->%s, right %s->%s, left %s->%s, top %s->%s",
               name ? name : "", gdb_type_name(SDL_GetGamepadType(pad)), SDL_GetGamepadVendor(pad),
               SDL_GetGamepadProduct(pad), gdb_backend_name(guid), guid_text,
               gdb_label_name(SDL_GetGamepadButtonLabel(pad, SDL_GAMEPAD_BUTTON_SOUTH)), gdb_xinput_name(face[0]),
               gdb_label_name(SDL_GetGamepadButtonLabel(pad, SDL_GAMEPAD_BUTTON_EAST)), gdb_xinput_name(face[1]),
               gdb_label_name(SDL_GetGamepadButtonLabel(pad, SDL_GAMEPAD_BUTTON_WEST)), gdb_xinput_name(face[2]),
               gdb_label_name(SDL_GetGamepadButtonLabel(pad, SDL_GAMEPAD_BUTTON_NORTH)), gdb_xinput_name(face[3]));
}

/* A joystick SDL has no gamepad mapping for can't be used (the game reads gamepads only): say so once, with what
 * would make it work. `user_file` is where the player's own mappings go (may be NULL). Returns 1 when it warned. */
static inline int gdb_note_joystick(SDL_JoystickID id, const char *user_file, gdb_log_fn log, void *ctx) {
  char guid_text[33];
  const char *name;
  if (SDL_IsGamepad(id)) {
    return 0;
  }
  name = SDL_GetJoystickNameForID(id);
  if (!name) {
    return 0; /* gone again */
  }
  SDL_GUIDToString(SDL_GetJoystickGUIDForID(id), guid_text, (int)sizeof(guid_text));
  gdb_logf(log, ctx, GDB_WARN,
           "Controller \"%s\" (%04x:%04x, %s, GUID %s) has no gamepad mapping, so it isn't used. A line for GUID %s "
           "in %s (SDL GameControllerDB format: https://github.com/mdqinc/SDL_GameControllerDB), or in the "
           "SDL_GAMECONTROLLERCONFIG environment variable, makes it a gamepad.",
           name, SDL_GetJoystickVendorForID(id), SDL_GetJoystickProductForID(id),
           gdb_backend_name(SDL_GetJoystickGUIDForID(id)), guid_text, guid_text,
           (user_file && user_file[0]) ? user_file : "a gamecontrollerdb.txt next to the program");
  return 1;
}

/* ---- Linux: device access --------------------------------------------------------------------------------------- */

#if defined(__linux__)
/* Controllers SDL reads through HIDAPI (/dev/hidraw*) when it may, by USB vendor. */
static inline const char *gdb_hid_vendor_name(unsigned vendor) {
  switch (vendor) {
    case 0x057e:
      return "Nintendo";
    case 0x054c:
      return "Sony";
    case 0x28de:
      return "Valve";
    case 0x0f0d:
      return "HORI";
    case 0x20d6:
    case 0x24c6:
      return "PowerA";
    case 0x0e6f:
      return "PDP";
    case 0x2dc8:
      return "8BitDo";
    case 0x0079:
      return "DragonRise / Mayflash";
    default:
      return NULL;
  }
}

/* SDL reads these only through libusb (SDL's own list, src/hidapi/SDL_hidapi.c). */
static inline const char *gdb_libusb_device_name(unsigned vendor, unsigned product) {
  if (vendor != 0x057e) {
    return NULL;
  }
  switch (product) {
    case 0x0337:
      return "Wii U GameCube adapter (also a Mayflash adapter in Wii U mode)";
    case 0x2073:
      return "Switch 2 GameCube controller";
    case 0x2066:
    case 0x2067:
      return "Switch 2 Joy-Con";
    case 0x2069:
      return "Switch 2 Pro controller";
    default:
      return NULL;
  }
}

static inline int gdb_read_small_file(const char *path, char *buf, size_t size) {
  FILE *f = fopen(path, "r");
  size_t n;
  if (!f) {
    return 0;
  }
  n = fread(buf, 1, size - 1, f);
  fclose(f);
  buf[n] = '\0';
  return 1;
}

/* Logs why a connected controller can't be read directly: a /dev/hidrawN this user may not read and write (SDL then
 * falls back to evdev, where only a mapping by GUID names the buttons and Joy-Cons stay apart), and the adapters
 * SDL reads only through libusb. Reads /sys and checks access(2); opens nothing. Returns the number of warnings. */
static inline int gdb_linux_access_hints(gdb_log_fn log, void *ctx) {
  int warned = 0;
  DIR *dir = opendir("/sys/class/hidraw");
  struct dirent *e;
  if (dir) {
    while ((e = readdir(dir)) != NULL) {
      char path[512], text[2048], node[300];
      const char *id;
      unsigned bus = 0, vendor = 0, product = 0;
      const char *who;
      if (SDL_strncmp(e->d_name, "hidraw", 6) != 0) {
        continue;
      }
      SDL_snprintf(path, sizeof(path), "/sys/class/hidraw/%s/device/uevent", e->d_name);
      if (!gdb_read_small_file(path, text, sizeof(text))) {
        continue;
      }
      id = SDL_strstr(text, "HID_ID=");
      if (!id || SDL_sscanf(id + 7, "%x:%x:%x", &bus, &vendor, &product) != 3) {
        continue;
      }
      who = gdb_hid_vendor_name(vendor);
      if (!who) {
        continue;
      }
      SDL_snprintf(node, sizeof(node), "/dev/%s", e->d_name);
      if (access(node, R_OK | W_OK) == 0) {
        continue;
      }
      if (errno == ENOENT) {
        gdb_logf(log, ctx, GDB_INFO, "Controller access: %s (%s %04x:%04x) has no device node here (a container?)",
                 node, who, vendor, product);
        continue;
      }
      gdb_logf(log, ctx, GDB_WARN,
               "Controller access: %s (%s %04x:%04x, %s) can't be opened for reading and writing by this user (%s). "
               "SDL then reads it through the kernel's joystick driver (evdev) instead, where only a mapping for its "
               "GUID (gamecontrollerdb.txt) names its buttons and two Joy-Cons stay two controllers. A udev rule "
               "giving your user access to it lets SDL read it directly: your distribution's steam-devices (or "
               "game-devices-udev) package has rules for many pads, or a rule of your own such as "
               "KERNEL==\"hidraw*\", ATTRS{idVendor}==\"%04x\", MODE=\"0660\", TAG+=\"uaccess\" (USB; for "
               "Bluetooth: KERNEL==\"hidraw*\", KERNELS==\"*%04X:%04X*\", ...).",
               node, who, vendor, product, bus == 5 ? "Bluetooth" : "USB", strerror(errno), vendor, vendor, product);
      ++warned;
    }
    closedir(dir);
  }
  dir = opendir("/sys/bus/usb/devices");
  if (dir) {
    while ((e = readdir(dir)) != NULL) {
      char path[512], text[64], node[64];
      unsigned vendor = 0, product = 0, busnum = 0, devnum = 0;
      const char *what;
      SDL_hid_device_info *found;
      if (e->d_name[0] == '.') {
        continue;
      }
      SDL_snprintf(path, sizeof(path), "/sys/bus/usb/devices/%s/idVendor", e->d_name);
      if (!gdb_read_small_file(path, text, sizeof(text)) || SDL_sscanf(text, "%x", &vendor) != 1) {
        continue;
      }
      SDL_snprintf(path, sizeof(path), "/sys/bus/usb/devices/%s/idProduct", e->d_name);
      if (!gdb_read_small_file(path, text, sizeof(text)) || SDL_sscanf(text, "%x", &product) != 1) {
        continue;
      }
      what = gdb_libusb_device_name(vendor, product);
      if (!what) {
        continue;
      }
      found = SDL_hid_enumerate((unsigned short)vendor, (unsigned short)product);
      if (!found) {
        gdb_logf(log, ctx, GDB_WARN,
                 "Controller access: a %s (%04x:%04x) is plugged in, but SDL reads it only through libusb, and this "
                 "program's SDL can't (built without libusb, or libusb-1.0 isn't installed): its controllers won't "
                 "work. A Mayflash adapter switched to PC mode works without libusb.",
                 what, vendor, product);
        ++warned;
        continue;
      }
      SDL_hid_free_enumeration(found);
      SDL_snprintf(path, sizeof(path), "/sys/bus/usb/devices/%s/busnum", e->d_name);
      if (gdb_read_small_file(path, text, sizeof(text))) {
        SDL_sscanf(text, "%u", &busnum);
      }
      SDL_snprintf(path, sizeof(path), "/sys/bus/usb/devices/%s/devnum", e->d_name);
      if (gdb_read_small_file(path, text, sizeof(text))) {
        SDL_sscanf(text, "%u", &devnum);
      }
      SDL_snprintf(node, sizeof(node), "/dev/bus/usb/%03u/%03u", busnum, devnum);
      if (busnum && devnum && access(node, R_OK | W_OK) != 0 && errno != ENOENT) {
        gdb_logf(log, ctx, GDB_WARN,
                 "Controller access: a %s (%04x:%04x) is plugged in, but this user can't open %s for reading and "
                 "writing (%s), which libusb needs. A udev rule such as SUBSYSTEM==\"usb\", ATTRS{idVendor}==\"%04x\", "
                 "ATTRS{idProduct}==\"%04x\", MODE=\"0660\", TAG+=\"uaccess\" gives it.",
                 what, vendor, product, node, strerror(errno), vendor, product);
        ++warned;
      }
    }
    closedir(dir);
  }
  return warned;
}
#endif /* __linux__ */

#endif /* REX_INPUT_SDL_GAMEPAD_DB_H */
