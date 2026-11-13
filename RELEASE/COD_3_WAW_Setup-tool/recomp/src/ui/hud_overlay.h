// cod3 - always-on HUD layer (custom crosshair) and the F2 settings window.

#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>

#include <rex/ui/imgui_dialog.h>
#include <rex/ui/window.h>

namespace cod3::ui {

// Valve-style crosshair settings, persisted as host cvars (cod3_crosshair_*).
struct CrosshairStyle {
  bool enabled;
  int style;        // 0 classic cross, 1 cross + dot, 2 dot only, 3 circle, 4 T-shape
  float size;       // arm length in px at 1080p
  float thickness;
  float gap;
  bool outline;
  float outline_thickness;
  bool dot;
  bool constant_color;  // false: the game's own color (red over an enemy, ...)
  float color[4];       // the player's color, used when constant_color is on
};
CrosshairStyle CurrentCrosshair();

// Called from the CG_DrawCrosshair midasm hooks (guest thread): the game
// wanted to draw its normal crosshair this frame, in `rgba` (its own color:
// white, red over an enemy, green over a friendly, black over a dead body).
// aim_fraction: 0 = hip fire, 1 = fully aimed down the sights.
void NotifyGameCrosshair(float aim_fraction, const float rgba[4]);
// [claude-9b] Split screen (owner 2026-10-26: "custom crosshairs per player, placed where the
// original would be", following the number of players and players leaving): the local player
// (0-3) whose view the next NotifyGameCrosshair is for, and that view's place on the game's
// picture as fractions (x, y, width, height). Without it a crosshair is player 1's, whole picture
// (the campaign). cod3mp's CG_DrawCrosshair hooks call it (game/mp/hud_mp.cpp).
void SetGameCrosshairView(int view, float x, float y, float width, float height);
// True while the game is showing (or would show) its crosshair.
bool GameCrosshairVisible();

// [claude-9b] A preview of the custom crosshair (owner 2026-10-20: "the
// crosshair menu needs a preview of the crosshair"): a box in the game's menu
// space (its 640x480 panel space, drawn over the HUD area like the menus, so
// it lines up with the menu at any window size or shape; on the screen it is
// as wide as it is tall, right edge and height from the box), with the
// crosshair drawn in its middle at its in-game size. Shown while it is asked
// for every frame (the AIM page, game/menu_aim.h). Any thread.
void ShowCrosshairPreview(float x0, float y0, float x1, float y1);

// Where the game's HUD is on the window, in pixels: the picture's 16:9 middle
// when a wider picture keeps the HUD 16:9 (game/ultrawide.h), else the
// picture itself (the window, or its letterboxed part). The port's own HUD
// text (watermark, announcements) is placed inside it.
struct HudArea {
  float x, y, width, height;
};
HudArea CurrentHudArea(const ImGuiIO& io);

#if defined(COD3_MP)
// [f2-menus] 2026-11-11 cod3mp: the language settings (Text, Voices, Console language, Text by kind) are the
// campaign's too. cod3mp_app.cpp calls this once, after the campaign's cod3.toml was read (OnPostSetup). From then
// on a change made in this program (F2, the game's Options) is written into cod3.toml as well, as the SDK writes
// its lines, and cod3mp.toml is saved after it. When cod3.toml is the newer file, a language setting it has no line
// for (the campaign's is back at the game's own) is put back here too. Nothing when cod3.toml isn't there.
void ShareLanguageSettings(const std::filesystem::path& campaign_config, const std::filesystem::path& own_config,
                           bool campaign_newer);
#endif

class HudOverlay : public rex::ui::ImGuiDialog {
 public:
  explicit HudOverlay(rex::ui::ImGuiDrawer* drawer) : ImGuiDialog(drawer) {}
  std::function<void()> on_frame;  // UI thread, once per frame

  // Drawn on top of each game frame only: repainting in between (as fast as
  // the GPU could present on Linux) used most of the GPU for nothing.
  bool WantsContinuousRepaint() const override { return false; }

 protected:
  void OnDraw(ImGuiIO& io) override;
};

class ConsoleView;  // ui/console_view.h

class SettingsWindow : public rex::ui::ImGuiDialog {
 public:
  SettingsWindow(rex::ui::ImGuiDrawer* drawer, std::filesystem::path config_path,
                 std::function<void()> on_close);
  bool open() const { return open_; }
  // The game window, for settings that apply to it directly (refresh rate).
  void set_window(rex::ui::Window* window) { window_ = window; }
  // Saves when something changed ([keybinds]: a key row's typed text counts).
  void SaveIfDirty();

 protected:
  void OnDraw(ImGuiIO& io) override;

 private:
  void DrawVideo();
  // [claude-9b] VIDEO's subtabs.
  void DrawVideoDisplay();
  void DrawVideoResolution();
  void DrawVideoFrameRate();
  void DrawVideoColors();
  void DrawVideoQuality();
  void DrawSound();
  void DrawGame();
  void DrawGamePrompts();  // [claude-9b-think] Game > Prompts
  void DrawSoundOutput();
  void DrawSpeakerLayout();  // [acct2-decomp] what the sound is mixed for (SDK patch 0180)
  void DrawCrosshair();
  void DrawControls();
  // [claude-9b-think] CONTROLS' subtabs.
  void DrawControlsMouse();
  void DrawControlsController();
  void DrawControlsPerWeapon();
  void DrawControlsKeys();
  void DrawInterface();
  void DrawPerformance();
  // [claude-9b] 2026-10-27 EMULATION and its subtabs.
  void DrawEmulation();
  void DrawEmulationGpu();
  void DrawEmulationSpeedups();
  void DrawEmulationNative();
  void DrawEmulationSound();
  void DrawConsole();
  void DrawSavedConsoleSettings();
  void Save();

  std::filesystem::path config_path_;
  std::function<void()> on_close_;
  rex::ui::Window* window_ = nullptr;
  bool open_ = true;
  bool dirty_ = false;
  bool restart_needed_ = false;
  std::string status_;
  // F2 > Console's console (ui/console_pages.h), made when first shown.
  std::shared_ptr<ConsoleView> console_view_;
};

}  // namespace cod3::ui
