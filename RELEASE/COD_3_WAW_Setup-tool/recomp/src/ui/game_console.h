// cod3 - World at War style drop-down console.
//
//   `  / ~         open / close the compact console (input line + suggestions)
//   Shift + ` / ~  open the full console (scroll-back output above the input)
//   Tab            complete the longest common prefix / cycle suggestions
//   Up / Down      history (or move through suggestions while they are shown)
//   PgUp / PgDn    scroll the output, Ctrl+L clears it, Esc closes
//
// Everything typed is executed by the game's own command buffer (Q3 Cbuf), so
// engine commands (map, devmap, god, noclip, give, set/seta/toggle, exec, vstr,
// bind, cmdlist ...) and "<cvar> <value>" all work. A few console-side
// commands are handled locally: clear, condump, cvarlist, find, fov,
// sv_cheats (= cheats), quit. Cheat commands and cheat-protected dvars pass
// the cheat guard first (game/cheat_guard.h).

#pragma once

#include <atomic>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include <rex/ui/imgui_dialog.h>
#include <rex/ui/window_listener.h>

#include "game/cheat_guard.h"
#include "game/engine.h"

struct ImGuiInputTextCallbackData;

namespace cod3::ui {

enum class ConsoleChannel : uint8_t { kGame, kDeveloper, kError, kEcho, kSystem };

struct ConsoleLine {
  ConsoleChannel channel;
  std::string text;
};

// Thread-safe console model: output buffer + command history.
class GameConsole {
 public:
  static GameConsole& Get();

  void Print(ConsoleChannel channel, std::string_view text);
  void Clear();
  uint64_t generation() const { return generation_.load(); }
  void CopyLines(std::vector<ConsoleLine>& out);
  bool Dump(const std::filesystem::path& path);

  // Executes a full input line (may contain ';'-separated commands). Every
  // command passes the cheat guard (game/cheat_guard.h) for its source: the
  // console itself, or cod3_autoexec.
  void Submit(std::string_view line, cheats::Source source = cheats::Source::kConsole);

  std::deque<std::string>& history() { return history_; }  // UI thread only
  void SetUserDir(std::filesystem::path dir) { user_dir_ = std::move(dir); }
  // quit / exit: set by the app so the game closes the same way as the
  // window's close button (settings saved, process always ends).
  void SetQuitHandler(std::function<void()> fn) { on_quit_ = std::move(fn); }
  // A World at War name changed a port setting (sensitivity, r_mode ...):
  // the app saves the settings file.
  void SetSettingsChangedHandler(std::function<void()> fn) { on_settings_changed_ = std::move(fn); }

  static bool IsOpen() { return open_.load(); }
  static void SetOpen(bool open) { open_ = open; }

  // The commands the console itself handles (clear, condump, problems ...),
  // for completion.
  static const std::vector<const char*>& LocalCommands();

 private:
  bool HandleLocal(const std::string& cmd, const std::vector<std::string>& args, cheats::Source source);
  // Whether the game knows `name` as a cheat-protected dvar (from the snapshot).
  bool IsCheatDvar(const std::string& name);
  // World at War dvar names (waw_dvars.h). Returns true when fully handled;
  // may rewrite `cmd` / `args` to the CoD3 dvar.
  bool HandleWaw(std::string& cmd, std::vector<std::string>& args);

  static constexpr size_t kMaxLines = 4000;
  static constexpr size_t kMaxHistory = 64;

  std::mutex mutex_;
  std::deque<ConsoleLine> lines_;
  bool last_line_open_ = false;  // previous Print did not end with '\n'
  std::atomic<uint64_t> generation_{0};
  std::deque<std::string> history_;
  std::filesystem::path user_dir_;
  std::function<void()> on_quit_;
  std::function<void()> on_settings_changed_;
  static inline std::atomic<bool> open_{false};
};

class ConsoleView;

// The drop-down window. Created when the console opens, destroyed when it
// closes. What is inside (output, input line, completion, history) is the
// ConsoleView that F2 > Console shows too (ui/console_view.h).
class GameConsoleDialog : public rex::ui::ImGuiDialog {
 public:
  GameConsoleDialog(rex::ui::ImGuiDrawer* drawer, bool full);
  ~GameConsoleDialog() override;

  void SetFull(bool full) { full_ = full; }
  bool full() const { return full_; }

 protected:
  void OnDraw(ImGuiIO& io) override;

 private:
  void DrawConsole(ImGuiIO& io);

  std::unique_ptr<ConsoleView> view_;
  bool full_;
  float open_anim_ = 0.0f;
};

}  // namespace cod3::ui
