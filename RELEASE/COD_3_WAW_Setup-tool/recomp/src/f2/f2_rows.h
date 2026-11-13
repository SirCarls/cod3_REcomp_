// f2 - the rows of the F2 settings window, each bound to one setting by its name. [f2-menus]
//
// The same rules as Call of Duty 3's window (recomp/src/ui/hud_overlay.cpp): a row only edits a setting the game
// already has (shown greyed out when this build doesn't have it), a combo only offers values the setting accepts,
// every change goes through rex::cvar::SetFlagByName (so it is saved like any other change), and a change that only
// takes effect at the next start says so under the Save button.
//
// Each row returns true when the player changed its value this frame; the window then marks itself unsaved.

#pragma once

#include <initializer_list>
#include <string>

namespace f2 {

// When a row's change takes effect. kAtOnce rows still say "restart" when the setting itself is registered as
// needing one (its lifecycle isn't kHotReload).
enum class Applies { kAtOnce, kAfterRestart };

bool Has(const char* name);
std::string Get(const char* name);
bool GetBool(const char* name);
int GetInt(const char* name, int fallback = 0);
double GetDouble(const char* name, double fallback = 0.0);

// A check box for a true/false setting.
bool Check(const char* label, const char* name, Applies applies, bool* restart);

// A combo over `count` values, each shown by its name.
bool Choice(const char* label, const char* name, const char* const* values, const char* const* names, int count,
            Applies applies, bool* restart);

// A slider with a number box (Call of Duty 3's "sliders + textbox", ui/slider_box.h).
bool SliderInt(const char* label, const char* name, int min, int max, const char* format, Applies applies,
               bool* restart);
bool SliderDouble(const char* label, const char* name, double min, double max, const char* format, Applies applies,
                  bool* restart);

// A text setting, applied when Enter is pressed or the box loses focus (the SDK's key names: "Space", "Shift+Up").
bool TextRow(const char* label, const char* name, bool* restart);

// A short, plain description under a row (dimmed, wrapped to the window).
void Help(const char* text);

// "Reset <what> to defaults" with a confirmation; true once confirmed.
bool ResetButton(const char* id, const char* what);
// Puts the settings back to their defaults the way a change in F2 does. True when one of them needs a restart.
bool ResetSettings(std::initializer_list<const char*> names);

}  // namespace f2
