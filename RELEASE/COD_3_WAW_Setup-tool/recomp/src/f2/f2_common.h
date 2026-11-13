// f2 - pages and helpers every game's F2 window shares. [f2-menus]
//
// World at War's window (recomp_waw/src/waw_f2.cpp, the first game on the shell) wrote these for itself; from Call of
// Duty 4 on, each game takes them from here and adds only its own rows: the SOUND and INTERFACE pages, the keyboard /
// controller half of CONTROLS, the queue of console commands for the game's frame hook, and the test-run script.

#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include "f2/f2_accel.h"  // [f2-menus] f2::accel: Apply, Tick, SetSupported, SetGameSensitivity

namespace rex::ui {
class Window;
}  // namespace rex::ui

namespace f2 {

class Overlay;
struct PageContext;

// Console commands from the UI thread (the CONSOLE page, a row that sets one of the game's dvars) to the game's
// frame hook, which runs them through the game's Cbuf_AddText. Has() is cheap enough for every frame.
class CommandQueue {
 public:
  void Push(std::string command);
  bool Has() const { return has_.load(std::memory_order_acquire); }
  void Take(std::vector<std::string>& out);  // out is swapped with the queue, which is then empty

 private:
  std::mutex mutex_;
  std::vector<std::string> commands_;
  std::atomic<bool> has_{false};
};

// VIDEO's "Resolution" section, the same in every game on the rexglue SDK (dev/agents/f2-menus/RESOLUTION-PLAN.md,
// phase 1): the render scale (the SDK's resolution_scale: every picture the game draws, N times larger), the picture's
// shape in the window (present_letterbox: black bars, or stretched to fill it) and the window's size at start
// (window_width / window_height). Its settings, for the page's Reset: kResolutionSettings.
void DrawResolutionSection(PageContext& c);
extern const char* const kResolutionSettings[8];

// VIDEO's "Picture" section, the same in every game on the rexglue SDK, as Call of Duty 3's "Anti-aliasing & scaling"
// (2026-11-10, owner: "keep adding settings from the other f2 menu to the other titles"): post-process anti-aliasing
// (swap_post_effect), how the finished picture is fitted to the window (present_effect) with its sharpness as a
// slider (present_fsr_sharpness_reduction / present_cas_additional_sharpness), and anisotropic filtering. A row whose
// setting this build's SDK doesn't have is left out. Its settings, for the page's Reset: kPictureSettings.
void DrawPictureSection(PageContext& c);
extern const char* const kPictureSettings[5];

// EMULATION: Call of Duty 3's EMULATION rows that belong to the SDK, not to one game, so every game has them: how the
// Xbox 360's video memory is emulated, resolve readback, the Vulkan speed-ups, shader building, and the sound
// chip's emulation. `game_rows` (may be empty) come first, under the game's own heading; `game_settings` are their
// settings, for the page's Reset. Rows whose setting this build's SDK doesn't have are left out.
void DrawEmulationPage(PageContext& c, const std::function<void(PageContext&)>& game_rows,
                       const std::vector<const char*>& game_settings);

// [f2-menus] 2026-11-11 02:55 Mouse acceleration inside the game only (owner 02:14: Raw Accel's, "but ONLY in-game"):
// Raw Accel's curves on the counts a port has already read for its own mouse look. No driver, nothing outside the
// game's process; off by default (f2_accel). A port calls Apply where its counts become a turn and SetSupported(true)
// once, which makes the CONTROLS page show the "Mouse acceleration" section.
// [f2-menus] 2026-11-11 03:45 they are declared in f2_accel.h (a file Call of Duty 3 builds too); here only the rows
// drawn with this window's widgets, for a port's CONTROLS page.
namespace accel {
void DrawRows(PageContext& c);
}  // namespace accel

// [f2-menus] 2026-11-11 02:40 VIDEO > Display rows of Call of Duty 3's window: "Display" (which monitor; the SDK's
// monitor / monitor_name) and "Refresh rate (fullscreen)" (f2_refresh_rate). A port draws them after Display mode.
// The Render resolution row by name (f2_render_resolution) is part of DrawResolutionSection.
void DrawDisplayRows(PageContext& c);
namespace display {
// The game's window, for those rows and for "Match the screen" (the Overlay gives it; it also applies
// f2_refresh_rate once).
void SetGameWindow(rex::ui::Window* window);
// Once a drawn UI frame: applies f2_render_resolution when it changed (the window, a test script, the command line).
// True when the settings should be saved now ("Match the screen" found another scale at the first frame).
bool Tick();
}  // namespace display

// [f2-menus] 2026-11-11 02:10 PERFORMANCE rows every port shares (Call of Duty 3's window has the page): "Show what
// each frame costs" (f2_frame_cost), whose lines frame_cost::Tick draws. A port puts them on its PERFORMANCE page.
void DrawPerformanceRows(PageContext& c);
namespace frame_cost {
// Once a drawn UI frame (Overlay::OnDraw calls it): refreshes the numbers every second from the SDK's GPU totals,
// draws the corner lines while f2_frame_cost is on and writes the log line every f2_frame_cost_log seconds.
void Tick();
}  // namespace frame_cost

// SOUND: the output device (saved at once, as Call of Duty 3's), what the 5.1 sound is mixed for, and mute.
void DrawSoundPage(PageContext& c);

// SOUND's "Speakers" row, as Call of Duty 3's (the SDK's audio_output_layout: automatic, stereo, headphones with
// virtual surround, 5.1, 7.1), saved at once. Nothing when this build's SDK doesn't have the setting. DrawSoundPage
// draws it; a game with a SOUND page of its own calls it after its output row.
void DrawSpeakersRow(PageContext& c);

// GAME's "Player" rows, as Call of Duty 3's Player tab (2026-11-10): the player's name (user_name, read by the game
// when it starts), split-screen players 2 to 4 (user_name_2..4) and which player each controller plays as
// (input_player_order, at once). Rows whose setting this build's SDK doesn't have are left out; nothing at all (not
// even the heading) when it has none. Its settings, for the page's Reset: kPlayerSettings.
void DrawPlayerSection(PageContext& c);
extern const char* const kPlayerSettings[5];

// INTERFACE: theme, text size and opacity, the settings the game named in SetLook (f2_theme.h).
void DrawInterfacePage(PageContext& c);

// CONTROLS: keyboard and mouse (the SDK's keyboard pad), then `game_rows` (the game's own, may be empty), the
// controller shortcut's switch (`pad_shortcut_setting`), and the keys when the keyboard plays. `game_settings` are
// the game rows' settings, for the page's Reset.
void DrawControlsPage(PageContext& c, const char* pad_shortcut_setting,
                      const std::function<void(PageContext&)>& game_rows,
                      const std::vector<const char*>& game_settings);

// A test run's steps for the window: "<seconds>:<step>|...", the seconds counted from the first UI frame. A step is
// open, close, page <LABEL>, set <setting> <value> (as a row would, then unsaved), save, or console <command> (as
// typed on the CONSOLE page). Each step runs once, in order.
class TestScript {
 public:
  explicit TestScript(const std::string& script);
  void Run(Overlay& overlay, CommandQueue& commands);  // once a UI frame (Overlay::Options::on_frame)

 private:
  struct Step {
    double seconds;
    std::string action;
  };
  std::vector<Step> steps_;
  std::chrono::steady_clock::time_point first_{};
  bool started_ = false;
  size_t next_ = 0;
};

// Rows for the game's own options that the console can set (2026-11-10, owner: "Please keep going on these
// settings"; found by asking each dvar on the game's console in a level, changing it and asking again: run dv1).
// Each row is a setting of ours with three choices: "" leaves the game's own value alone, "1" and "0" force it on
// or off. `kind` picks which of the known rows a game shows (a game leaves out a row whose dvar it doesn't have).
struct GameOption {
  const char* setting;  // ours, e.g. "f2_game_subtitles"
  const char* dvar;     // the game's, e.g. "cg_subtitles"
  const char* label;
  const char* help;
};
// Every known row; a game passes the ones it has (by index into this table, kGameOptionCount in all).
extern const GameOption kGameOptions[];
extern const int kGameOptionCount;
// The rows of `indices` (into kGameOptions) under the current section. [f2-menus] 2026-11-10 20:30: `first` and
// `last` keep it to the rows whose number in kGameOptions is in that range, so a page can draw its rows in groups
// (0 to 10 the game's options, 11 and 12 the controller, 13 to 16 hints and small effects, 17 to 21 the picture).
void DrawGameOptionRows(PageContext& c, const int* indices, int count, int first = 0, int last = 1 << 30);
// Once a UI frame: a row whose setting changed since the last call goes to the game as a console line through
// `push` ("set <dvar> <value>", or "reset <dvar>" when it went back to the game's own). At the first call every row
// that isn't "" is sent, so a saved choice applies at the start; a forced row is sent again every 3 seconds, because
// the game puts its own value back when a profile or a level loads.
void SendGameOptions(const int* indices, int count, const std::function<void(std::string)>& push);
// [f2-menus] 2026-11-11 00:15 A port that knows whether a level runs calls SetLevelRuns once a UI frame, before
// SendGameOptions. A row given back to the game while no level runs has its "reset" sent once more two seconds
// into the next level (the game may register the dvar only then and would keep the row's value otherwise).
// ResetAgainAtLevelStart is for a port's own row that resets a dvar. A port that never calls SetLevelRuns keeps the
// old behaviour: one reset.
void SetLevelRuns(bool runs, const std::function<void(std::string)>& push);
void ResetAgainAtLevelStart(const char* dvar);

// [f2-menus] 2026-11-10 The game's own numbers as rows (under the current section): the weapon's place on screen
// (f2_gun_own with f2_gun_x / _y / _z -> cg_gun_x / _y / _z) and the game crosshair's opacity
// (f2_crosshair_alpha_own with f2_crosshair_alpha -> cg_crosshairAlpha). kGameNumberSettings are their settings,
// for a page's Reset.
extern const char* const kGameNumberSettings[6];
void DrawGameNumberRows(PageContext& c);
// [f2-menus] 2026-11-10 21:00 The game's own look numbers as rows: the right stick's look speed (f2_stick_sens_own
// with f2_stick_sens -> input_viewSensitivity) and, with `mouse`, the sensitivity of the game's own mouse look
// (f2_mouse_sens_own with f2_mouse_sens -> sensitivity). SendGameNumbers sends them too. kGameInputSettings are
// their settings, for a page's Reset.
extern const char* const kGameInputSettings[4];
void DrawGameInputRows(PageContext& c, bool mouse);
// [f2-menus] 2026-11-10 22:30 The game's own gamma as rows (f2_gamma_own with f2_gamma -> r_gamma) and, with
// `brightness`, its brightness (f2_brightness_own with f2_brightness -> r_brightness; cheat protected in Call of
// Duty 4's multiplayer program, so not there). SendGameNumbers sends them too. kGamePictureSettings are their
// settings, for a page's Reset. [f2-menus] 2026-11-11 07:35 Call of Duty 2's campaign has r_gamma after all (asked of
// the running game; not in the text the dvar lists were made from) and takes it; it has no r_brightness.
extern const char* const kGamePictureSettings[4];
void DrawGamePictureRows(PageContext& c, bool brightness);
// [f2-menus] 2026-11-11 07:35 Call of Duty 2's dynamic lights as rows (f2_dlights_own with f2_dlights ->
// r_dlightLimit, 0 to 4). SendGameNumbers sends them too. kGameLightSettings are their settings, for a page's Reset.
extern const char* const kGameLightSettings[2];
void DrawGameLightRows(PageContext& c);
// [f2-menus] 2026-11-11 07:52 Call of Duty 2's campaign: bodies left lying (f2_corpses_own with f2_corpses ->
// ai_corpseCount, 0 to 64). SendGameNumbers sends them too. kGameCorpseSettings are their settings, for a Reset.
extern const char* const kGameCorpseSettings[2];
void DrawGameCorpseRows(PageContext& c);
// Once a UI frame, as SendGameOptions: "set <dvar> <value>" on a change and again every 3 seconds while a group's
// switch is on, "reset <dvar>" once when it goes off.
void SendGameNumbers(const std::function<void(std::string)>& push);

// The custom crosshair, as Call of Duty 3's (owner 2026-11-10: "keep going with any missing feature like the
// crosshair"): the port's own crosshair drawn over the game's picture where the game would draw its hip-fire
// crosshair, in the game's own color for that moment (white, red over an enemy, green over a friendly) or one
// constant color. The settings are the same in every game (f2_crosshair, f2_crosshair_style, _size, _thickness,
// _gap, _outline, _outline_thickness, _dot, _constant_color, _color).
namespace crosshair {

// The setting: the custom crosshair replaces the game's own.
bool Enabled();

// From the game's hook on its crosshair drawing (the game's thread): the game is about to draw local player `view`'s
// hip-fire crosshair in `rgba` (0-1 each; the alpha already carries the game's fades), its four side pieces pushed
// out from the middle by `spread` (in the game's 640 x 480 HUD units: what its own crosshair shows of the weapon's
// spread when you move, fire or crouch; 0 when not known). The hook skips the game's own drawing. Only view 0 is
// drawn for now (the whole picture's middle).
void Notify(int view, const float rgba[4], float spread);

// The game's spread, measured from the game's own drawing (owner 2026-11-10: "we also need to mimick the dynamic
// behaviour of both the actual game and cs2"). The game's hook on its crosshair's sides calls CaptureBegin, then the
// game's own function, then CaptureEnd. While capturing, the game's hook on ScrPlace_ApplyRect hands each rectangle
// to CaptureRect (the guest addresses of its x and width, r4 and r6), and the hook on the picture drawing skips
// the picture (Capturing()). The four side pieces come in the order top, right, bottom, left, so
// spread = (x of the 2nd - x of the 4th - 1.125 x width) / 2. CaptureEnd returns how many rectangles came (4 when
// the game drew its crosshair, 0 when it chose not to) and gives the spread.
void CaptureBegin();
bool Capturing();
void CaptureRect(const uint8_t* base, uint32_t x_address, uint32_t width_address);
int CaptureEnd(float* spread);

// Once a UI frame, window open or not (Overlay::Options::on_frame): draws the crosshair while the game asks for it.
void Draw();

// The CROSSHAIR page: the switch, the shape's rows, a preview, the color.
void DrawPage(PageContext& c);

// Reads four big-endian floats (a color in the game's memory) at guest address `address`; false, and `rgba` left
// alone, when the address is 0 or a value isn't a number between 0 and 1 (so a wrong guess at the register that
// carries the color shows as "no color", not as noise).
bool ReadGuestColor(const uint8_t* base, uint32_t address, float rgba[4]);

}  // namespace crosshair

}  // namespace f2
