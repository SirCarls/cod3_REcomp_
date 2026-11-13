// f2 - mouse acceleration, the sensitivity randomizer and the sensitivity scales, inside the game only. [f2-menus]
//
// Raw Accel's curves (owner 2026-11-11 02:14: "ONLY in-game"), a sensitivity randomizer (03:02) and other games'
// sensitivity numbers (03:09), on the counts a port has already read for its own mouse look: no driver, nothing
// outside the game's process, all off by default. The frame rate has no part in any of it (owner 03:24: "FPS cannot
// affect mouse input."): Apply works on each mouse event with the event's own timing, and the randomizer runs on
// real time in fixed steps. No code of the F2 window is used, so every port builds this file, Call of Duty 3 too.

#pragma once

namespace f2::accel {

// Once, by a port that calls Apply: makes DrawRows draw.
void SetSupported(bool supported);
// [f2-menus] 2026-11-11 03:20 (owner 03:09, "different sensitivity equations"): the port's own mouse sensitivity
// setting, the switch that makes it count (may be null), how many degrees it turns a count at 1 (0.022 in these
// games) and its limits. With it CONTROLS shows "Sensitivity as another game counts it" (f2_sens_scale,
// f2_sens_value, f2_sens_dpi), which sets that setting.
void SetGameSensitivity(const char* setting, const char* own_switch, double yaw, double low, double high);
// (dx, dy): the counts the mouse moved in `ms` milliseconds (0 or less = not known), before the game's own
// sensitivity. They are changed in place; fractions of a count stay in them for the caller's own carry. Any thread.
void Apply(float* dx, float* dy, double ms);
// [f2-menus] 2026-11-11 The same for counts that are `age_ms` milliseconds old by now: a port that takes its mouse
// events once a frame (Big Red One) gives each event's own age, so the randomizer's multiple is the one of the
// event's moment at any frame rate.
void ApplyAged(float* dx, float* dy, double ms, double age_ms);
// The sensitivity multiple at `speed` counts a millisecond with the current settings (the graph, tests).
double Multiplier(double speed);
// [f2-menus] 2026-11-11 03:10 Once a drawn UI frame (Overlay::OnDraw): moves the sensitivity randomizer's multiple
// (f2_sens_random; owner 03:02, as github.com/Whisperrr/SensitivityRandomizer but inside the game only), which Apply
// multiplies the counts by, and draws its readout.
void Tick();

// The settings of this file a player sets (not the debug ones), by name, for a window's "Reset to defaults" button.
// Returns how many; *names points at them.
int Settings(const char* const** names);

// Where the corner line (the randomizer's multiple) may start: under other corner lines drawn this frame (0 = none).
void SetCornerTop(float y);

// The rows, drawn with the caller's own widgets: each widget edits the setting `name` (as the F2 rows do) and
// returns true when the player changed it; `restart` is the caller's "a restart is needed" flag.
struct Widgets {
  void (*section)(const char* title);
  bool (*check)(const char* label, const char* name, bool* restart);
  bool (*choice)(const char* label, const char* name, const char* const* values, const char* const* names, int count,
                 bool* restart);
  bool (*slider_int)(const char* label, const char* name, int min, int max, const char* format, bool* restart);
  bool (*slider_double)(const char* label, const char* name, double min, double max, const char* format,
                        bool* restart);
  void (*help)(const char* text);
};
// Inside an ImGui window. True when a setting changed (the caller marks its window unsaved).
bool DrawRows(const Widgets& w, bool* restart);

}  // namespace f2::accel
