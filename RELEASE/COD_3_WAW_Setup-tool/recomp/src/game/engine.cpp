// cod3 - host <-> guest engine bridge. See engine.h.

#include "util.h"
#include "game/engine.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string_view>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>

#include "game/addresses.h"
#include "game/cheat_guard.h"
#include "game/cheat_watch.h"
#include "game/cutscene_skip.h"
#include "game/fov.h"
#include "game/game_api.h"
#include "game/settings_sync.h"
#include "game/hd_textures.h"
#include "game/guest.h"
#include "game/script_threads.h"
#include "game/thread_profiler.h"  // [hw-scaling]
#include "ui/game_console.h"

REXCVAR_DEFINE_STRING(cod3_autoexec, "", "cod3",
                      "Console commands to run once the game is up (e.g. \"spmap saint_lo\")");
REXCVAR_DEFINE_BOOL(cod3_debug_frames, false, "cod3",
                    "Log the guest frame counter every 300 frames (diagnostics)");
REXCVAR_DEFINE_INT32(cod3_autoexec_delay, 300, "cod3",
                     "Frames to wait before running cod3_autoexec")
    .range(0, 100000);

namespace cod3 {

namespace a = cod3::addr;
using guest::ReadString;
using guest::ReadU32;

// Limits on what reaches the game from the port (console lines, config
// values): the Q3 engine's own sizes (MAX_STRING_CHARS 1024 for a command,
// MAX_CVAR_VALUE_STRING 256), so nothing can overrun its buffers or the guest
// stack the strings are pushed on (game/guest.h).
constexpr size_t kMaxCommandChars = 1024;
constexpr size_t kMaxCvarNameChars = 63;
constexpr size_t kMaxCvarValueChars = 255;
constexpr size_t kMaxArchivedCvars = 512;
constexpr uintmax_t kMaxConfigBytes = 256 * 1024;

// A dvar name the game would accept: letters, digits and '_' (Cvar_ValidateString
// also refuses '\\', '"' and ';'), short enough for its buffers.
static bool ValidCvarName(const std::string& name) {
  if (name.empty() || name.size() > kMaxCvarNameChars) return false;
  for (char c : name)
    if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_') return false;
  return true;
}

// A value as the Q3 tokenizer can read it back: no quotes, no line breaks,
// no other control characters, bounded.
static bool ValidCvarValue(const std::string& value) {
  if (value.size() > kMaxCvarValueChars) return false;
  for (char c : value)
    if (static_cast<unsigned char>(c) < 0x20 || c == '"') return false;
  return true;
}

Engine& Engine::Get() {
  static Engine engine;
  return engine;
}

// ---------------------------------------------------------------------------
// Queues (any thread)
// ---------------------------------------------------------------------------
void Engine::QueueCommand(std::string line) {
  if (line.size() > kMaxCommandChars) {
    REXLOG_WARN("cod3: a console line of {} characters was dropped (the limit is {})", line.size(),
                kMaxCommandChars);
    ui::GameConsole::Get().Print(ui::ConsoleChannel::kError, "^1Command line too long; ignored.\n");
    return;
  }
  std::lock_guard lock(mutex_);
  pending_commands_.push_back(std::move(line));
}

void Engine::QueueGuestTask(std::function<void(PPCContext&, uint8_t*)> task) {
  std::lock_guard lock(mutex_);
  pending_tasks_.push_back(std::move(task));
}

EngineSnapshot Engine::Snapshot() {
  std::lock_guard lock(mutex_);
  return snapshot_;
}

uint64_t Engine::SnapshotGeneration() {
  std::lock_guard lock(mutex_);
  return snapshot_.generation;
}

// ---------------------------------------------------------------------------
// Config persistence
// ---------------------------------------------------------------------------
static std::string QuoteValue(const std::string& v) {
  std::string out = "\"";
  for (char c : v) {
    if (c == '"' || c == '\n' || c == '\r') continue;  // the Q3 tokenizer has no escapes
    out += c;
  }
  return out + "\"";
}

void Engine::SetConfigPath(std::filesystem::path path) {
  {
    std::lock_guard lock(mutex_);
    config_path_ = std::move(path);
  }
  LoadConfig();
}

// The console config is a plain text file the player may edit, so every line
// is checked: only `seta <name> "<value>"` / `set ...` lines with a dvar name
// the game accepts and a bounded, printable value are kept (and the file as a
// whole is bounded). sv_cheats is never read from it. Whether a kept dvar is
// cheat-protected is only known once the game runs: FirstFrame checks that.
void Engine::LoadConfig() {
  std::lock_guard lock(mutex_);
  config_loaded_ = true;
  std::error_code ec;
  const uintmax_t bytes = std::filesystem::file_size(config_path_, ec);
  if (!ec && bytes > kMaxConfigBytes) {
    REXLOG_WARN("cod3: {} is {} bytes, more than the {} KB a console config can be; not read",
                cod3::PathToUtf8(config_path_), bytes, kMaxConfigBytes / 1024);
    return;
  }
  std::ifstream in(config_path_);
  if (!in) {
    REXLOG_INFO("cod3: no config at {}, using defaults", cod3::PathToUtf8(config_path_));
    return;
  }
  std::string line;
  size_t dropped = 0;
  while (std::getline(in, line)) {
    // seta <name> "<value>"
    std::istringstream ss(line);
    std::string cmd, name;
    if (!(ss >> cmd >> name) || (cmd != "seta" && cmd != "set")) continue;
    std::string rest;
    std::getline(ss, rest);
    auto first = rest.find_first_not_of(" \t");
    rest = first == std::string::npos ? "" : rest.substr(first);
    if (rest.size() >= 2 && rest.front() == '"' && rest.back() == '"')
      rest = rest.substr(1, rest.size() - 2);
    if (!ValidCvarName(name) || !ValidCvarValue(rest) || cheats::IsSvCheatsName(name) ||
        archived_.size() >= kMaxArchivedCvars) {
      ++dropped;
      REXLOG_WARN("cod3: {}: line dropped: seta {} ... ({})", cod3::PathToUtf8(config_path_),
                  name.substr(0, 40),
                  cheats::IsSvCheatsName(name) ? "sv_cheats is set in the console only"
                                               : "not a valid dvar name or value");
      continue;
    }
    archived_[name] = rest;
  }
  fov::LoadPlayerSettings(archived_);  // the player's cg_fov / cg_fovScale, limited (game/fov.h)
  REXLOG_INFO("cod3: loaded {} archived cvars from {}{}", archived_.size(), cod3::PathToUtf8(config_path_),
              dropped ? " (" + std::to_string(dropped) + " line(s) dropped)" : std::string());
}

void Engine::SetArchived(const std::string& cvar, const std::string& value) {
  // Only what LoadConfig would read back (and never sv_cheats).
  if (!ValidCvarName(cvar) || !ValidCvarValue(value) || cheats::IsSvCheatsName(cvar)) {
    REXLOG_WARN("cod3: not saving '{}' to the console config (name or value not allowed)",
                cvar.substr(0, 40));
    return;
  }
  {
    std::lock_guard lock(mutex_);
    if (archived_.find(cvar) == archived_.end() && archived_.size() >= kMaxArchivedCvars) return;
    archived_[cvar] = value;
  }
  SaveConfig();
}

void Engine::DropArchived(const std::string& cvar) {
  {
    std::lock_guard lock(mutex_);
    if (archived_.erase(cvar) == 0) return;
  }
  SaveConfig();
}

std::map<std::string, std::string> Engine::ArchivedCvars() {
  std::lock_guard lock(mutex_);
  return archived_;
}

void Engine::SaveConfig() {
  std::lock_guard lock(mutex_);
  if (config_path_.empty()) return;
  std::error_code ec;
  std::filesystem::create_directories(config_path_.parent_path(), ec);
  std::ofstream out(config_path_, std::ios::trunc);
  if (!out) {
    REXLOG_WARN("cod3: could not write {}", cod3::PathToUtf8(config_path_));
    return;
  }
  out << "// Call of Duty 3 static recomp - archived cvars (seta). Edit freely.\n";
  for (auto& [k, v] : archived_) out << "seta " << k << " " << QuoteValue(v) << "\n";
}

// FOV: Engine::fov() / SetFov() / SetFovScale() ... are in game/fov.cpp.

// ---------------------------------------------------------------------------
// Frame rate limit / counter
// ---------------------------------------------------------------------------
void Engine::SetMaxFps(int fps) {
  std::string v = std::to_string(std::clamp(fps, 0, 1000));
  QueueGuestTask(
      [v](PPCContext& ctx, uint8_t* base) { CvarSet(ctx, base, "com_maxfps", v.c_str(), true); });
  SetArchived("com_maxfps", v);
}

void Engine::SetDrawFps(int mode) {
  mode = std::clamp(mode, 0, 3);
  draw_fps_ = mode;
  std::string v = std::to_string(mode);
  QueueGuestTask(
      [v](PPCContext& ctx, uint8_t* base) { CvarSet(ctx, base, "cg_drawFPS", v.c_str(), false); });
  SetArchived("cg_drawFPS", v);
}

void Engine::ResetArchivedCvars() {
  std::vector<std::string> names;
  {
    std::lock_guard lock(mutex_);
    for (auto& [name, value] : archived_) names.push_back(name);
    archived_.clear();
  }
  SaveConfig();
  const std::string max_fps = rex::cvar::GetFlagByName("cod3_max_fps");
  QueueGuestTask([this, names, max_fps](PPCContext& ctx, uint8_t* base) {
    // Each one back to its reset value (the default it was registered with).
    for (const auto& name : names) {
      const uint32_t var = CvarFind(ctx, base, name.c_str());
      if (!var) continue;
      const std::string reset = game::CvarResetString(base, var);
      CvarSet(ctx, base, name.c_str(), reset.c_str(), /*force=*/true);
    }
    // CoD3's own com_maxfps default is 0 (the Xbox 360's vsync held it to
    // 60); ours is the starting limit from the settings.
    if (!max_fps.empty()) CvarSet(ctx, base, "com_maxfps", max_fps.c_str(), /*force=*/true);
    // Take the new values as the baseline, so they aren't saved again as
    // if the player had changed them.
    fov::AdoptFromGuest(ctx, base);
    for (auto& w : watched_) {
      if (w.cvar) w.value = game::CvarString(base, w.cvar, 64);
    }
    RequestSnapshot();
  });
}

// cg_drawFPS takes World at War's names as well as numbers.
static int ParseDrawFps(const std::string& value) {
  std::string v;
  for (char c : value) v += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  if (v == "off") return 0;
  if (v == "simple") return 1;
  if (v == "simpleranges") return 2;
  if (v == "verbose") return 3;
  return std::clamp(std::atoi(v.c_str()), 0, 3);
}

// Runs every frame: mirrors cg_drawFPS for the HUD and, a few times a
// second, saves a console change of a watched cvar to the config.
void Engine::SyncWatchedCvars(PPCContext& ctx, uint8_t* base) {
  const bool check = (frame_ % 30) == 0;
  for (auto& w : watched_) {
    if (!w.cvar) {
      if (!check) continue;
      w.cvar = CvarFind(ctx, base, w.name);
      if (!w.cvar) continue;
    }
    std::string value = game::CvarString(base, w.cvar, 64);
    if (&w == &watched_[1]) draw_fps_ = ParseDrawFps(value);
    if (!check || value == w.value) continue;
    w.value = value;
    if (!w.seen) {
      w.seen = true;
      continue;
    }
    bool changed = false;
    {
      std::lock_guard lock(mutex_);
      auto it = archived_.find(w.name);
      if (it == archived_.end() || it->second != value) {
        archived_[w.name] = value;
        changed = true;
      }
    }
    if (changed) SaveConfig();
    // The console's com_maxfps is the frame rate limit too: cod3_max_fps,
    // its one home (FirstFrame), follows, so the next start keeps it.
    if (changed && &w == &watched_[0]) {
      char* end = nullptr;
      const long fps = std::strtol(value.c_str(), &end, 10);
      const std::string limit = std::to_string(std::clamp(fps, 0L, 1000L));
      if (end != value.c_str() && limit != rex::cvar::GetFlagByName("cod3_max_fps") &&
          rex::cvar::SetFlagByName("cod3_max_fps", limit))
        settings::RequestSave();
    }
  }
}

// ---------------------------------------------------------------------------
// Guest helpers (guest main thread); the calls themselves are in game_api.h.
// ---------------------------------------------------------------------------
uint32_t Engine::CvarFind(PPCContext& ctx, uint8_t* base, const char* name) {
  return game::CvarFind(ctx, base, name);
}

uint32_t Engine::CvarGet(PPCContext& ctx, uint8_t* base, const char* name, const char* value,
                         uint32_t flags) {
  return game::CvarGet(ctx, base, name, value, flags);
}

// Every dvar write the port makes goes through here. `force` skips the game's
// own checks (read-only, write-protected, cheat-protected), so it is allowed
// only for the dvars in cheats::ForcedDvars() or for a value equal to the
// dvar's default (game/cheat_guard.h); anything else becomes a plain write,
// which the game checks as it would a console line.
void Engine::CvarSet(PPCContext& ctx, uint8_t* base, const char* name, const char* value,
                     bool force) {
  const std::string_view name_view(name), value_view(value);
  if (name_view.empty() || name_view.size() > kMaxCvarNameChars || value_view.size() > kMaxCvarValueChars) {
    REXLOG_WARN("cod3: dvar write dropped: name or value too long ({} / {} characters)", name_view.size(),
                value_view.size());
    return;
  }
  if (force) {
    const uint32_t var = CvarFind(ctx, base, name);
    if (!cheats::MayForce(base, var, name_view, value_view)) {
      REXLOG_WARN("cod3: {} = '{}' asked for with force; not on the allow-list, written as a plain "
                  "(checked) write instead",
                  name, value);
      force = false;
    }
  }
  game::CvarSet2(ctx, base, name_view, value_view, force);
}

void Engine::CbufAddText(PPCContext& ctx, uint8_t* base, const std::string& text) {
  game::CbufAddText(ctx, base, text);
}

// ---------------------------------------------------------------------------
// Per-frame pump (guest main thread, called from the Cbuf_Execute hook)
// ---------------------------------------------------------------------------
void Engine::FirstFrame(PPCContext& ctx, uint8_t* base) {
  // cg_fovScale does not exist in CoD3; register it the way World at War does.
  fov_scale_cvar_ = CvarGet(ctx, base, "cg_fovScale", "1", a::cvar_flag::kArchive);
  // cg_drawFPS doesn't exist in CoD3 either; the HUD draws the counter.
  CvarGet(ctx, base, "cg_drawFPS", "0", a::cvar_flag::kArchive);
  developer_cvar_ = CvarFind(ctx, base, "developer");

  // Frame rate limit: one home, cod3_max_fps in cod3.toml. F2, the game's
  // Options, the setup tool and the console's com_maxfps (mirrored by
  // SyncWatchedCvars) all change it; the console config's com_maxfps is kept
  // equal to it. Before, the console config's com_maxfps won at every start,
  // so a limit chosen in the setup tool did nothing once the console config
  // had one. An install from before with only the console config's value
  // takes it over once. (CoD3's own default is 0: the Xbox 360's vsync held
  // it at 60.)
  std::string max_fps = rex::cvar::GetFlagByName("cod3_max_fps");
  if (const auto archived = ArchivedCvars(); archived.count("com_maxfps")) {
    const std::string& old = archived.at("com_maxfps");
    char* end = nullptr;
    const long fps = std::strtol(old.c_str(), &end, 10);
    if (old != max_fps && end != old.c_str() && *end == '\0' &&
        rex::cvar::GetFlagSource("cod3_max_fps") == rex::cvar::Source::kDefault) {
      const std::string taken = std::to_string(std::clamp(fps, 0L, 1000L));
      if (rex::cvar::SetFlagByName("cod3_max_fps", taken)) {
        REXLOG_INFO("cod3: frame rate limit {} taken over from the console config (com_maxfps)", taken);
        max_fps = taken;
        settings::RequestSave();
      }
    }
  }
  if (!max_fps.empty()) CvarSet(ctx, base, "com_maxfps", max_fps.c_str(), /*force=*/true);

  // Re-apply archived cvars. Cvar_Set2 creates not-yet-registered cvars as
  // user-created; when cgame later registers them Cvar_Get keeps our value
  // (a cheat-protected one is then put back by the Cvar_Get hook,
  // game/cheat_guard.cpp). A dvar the game already knows as cheat-protected is
  // refused here, with a log line, and dropped from the file: cheats are
  // turned on in the console only (owner).
  cheats::Sync(base);
  std::vector<std::string> refused;
  for (auto& [name, value] : ArchivedCvars()) {
    if (name == "com_maxfps") continue;  // cod3_max_fps is its home (above)
    const uint32_t var = CvarFind(ctx, base, name.c_str());
    if (var && (game::CvarFlags(base, var) & a::cvar_flag::kCheat) && !cheats::Enabled()) {
      REXLOG_WARN("cod3: {}: '{}' is cheat protected; not applied and dropped (cheats are turned on "
                  "in the console)",
                  cod3::PathToUtf8(config_path_), name);
      ui::GameConsole::Get().Print(ui::ConsoleChannel::kError,
                                   "^1" + name + " from the console config is cheat protected; dropped.\n");
      refused.push_back(name);
      continue;
    }
    CvarSet(ctx, base, name.c_str(), value.c_str(), /*force=*/false);
  }
  if (!refused.empty()) {
    {
      std::lock_guard lock(mutex_);
      for (const auto& name : refused) archived_.erase(name);
    }
    SaveConfig();
  }
  // The console config's com_maxfps follows the limit's home.
  if (const auto archived = ArchivedCvars();
      !max_fps.empty() && archived.count("com_maxfps") && archived.at("com_maxfps") != max_fps)
    SetArchived("com_maxfps", max_fps);
  // Starting values of the watched cvars (anything after this is a change
  // made in the game and gets saved).
  for (auto& w : watched_) {
    w.cvar = CvarFind(ctx, base, w.name);
    if (!w.cvar) continue;
    w.value = game::CvarString(base, w.cvar, 64);
    w.seen = true;
  }
  game_ready_ = true;
  REXLOG_INFO("cod3: engine hooks live (cg_fovScale cvar @ {:08X})", fov_scale_cvar_);
}

void Engine::TakeSnapshot(uint8_t* base) {
  EngineSnapshot snap;
  uint32_t var = ReadU32(base, a::kCvarVars);
  for (int guard = 0; var && guard < 4096; ++guard) {
    CvarInfo info;
    info.name = game::CvarName(base, var);
    info.value = game::CvarString(base, var);
    info.reset = game::CvarResetString(base, var);
    info.flags = game::CvarFlags(base, var);
    if (!info.name.empty()) snap.cvars.push_back(std::move(info));
    var = ReadU32(base, var + a::cvar::kNext);
  }
  uint32_t cmd = ReadU32(base, a::kCmdFunctions);
  for (int guard = 0; cmd && guard < 4096; ++guard) {
    std::string name = ReadString(base, ReadU32(base, cmd + 4), 128);
    if (!name.empty()) snap.commands.push_back(std::move(name));
    cmd = ReadU32(base, cmd);
  }
  auto ci_less = [](const std::string& x, const std::string& y) {
    return std::lexicographical_compare(x.begin(), x.end(), y.begin(), y.end(), [](char p, char q) {
      return std::tolower(static_cast<unsigned char>(p)) < std::tolower(static_cast<unsigned char>(q));
    });
  };
  std::sort(snap.cvars.begin(), snap.cvars.end(),
            [&](const CvarInfo& x, const CvarInfo& y) { return ci_less(x.name, y.name); });
  std::sort(snap.commands.begin(), snap.commands.end(), ci_less);
  snap.commands.erase(std::unique(snap.commands.begin(), snap.commands.end()), snap.commands.end());

  std::lock_guard lock(mutex_);
  snap.generation = snapshot_.generation + 1;
  snapshot_ = std::move(snap);
}

// --cod3_autoexec="spmap saint_lo" runs console commands once the game is up
// (after cod3_autoexec_delay frames), e.g. to jump straight into a mission or
// for automated testing. More steps follow after '|', each optionally
// starting with "@N" = N frames after the previous step, or "@Ns" = N
// seconds after it (frames can be very slow, or stop while a level loads):
//   --cod3_autoexec="spmap saint_lo|@3000 reboot"
//   --cod3_autoexec="spmap saint_lo|@240s r_mode 1920x1080"
void Engine::RunAutoexec(PPCContext& ctx, uint8_t* base) {
  (void)ctx;
  (void)base;
  if (autoexec_done_) return;
  const auto now = std::chrono::steady_clock::now();
  if (!autoexec_parsed_) {
    autoexec_parsed_ = true;
    autoexec_last_frame_ = frame_;
    autoexec_last_time_ = now;
    const std::string& all = REXCVAR_GET(cod3_autoexec);
    size_t start = 0;
    bool first = true;
    while (start <= all.size()) {
      size_t end = all.find('|', start);
      if (end == std::string::npos) end = all.size();
      std::string step = all.substr(start, end - start);
      start = end + 1;
      while (!step.empty() && step.front() == ' ') step.erase(step.begin());
      AutoexecStep parsed;
      if (first) parsed.frames = uint64_t(std::max(0, REXCVAR_GET(cod3_autoexec_delay)));
      if (!first && !step.empty() && step.front() == '@') {
        size_t digits = 1;
        while (digits < step.size() && std::isdigit(static_cast<unsigned char>(step[digits]))) ++digits;
        const uint64_t amount = std::strtoull(step.substr(1, digits - 1).c_str(), nullptr, 10);
        if (digits < step.size() && step[digits] == 's') {
          parsed.seconds = double(amount);
          ++digits;
        } else {
          parsed.frames = amount;
        }
        step.erase(0, digits);
        while (!step.empty() && step.front() == ' ') step.erase(step.begin());
      }
      first = false;
      // A step with only a wait ("@30s|") just waits before the next one.
      if (step.empty() && !parsed.frames && parsed.seconds <= 0.0) continue;
      parsed.commands = step;
      autoexec_steps_.push_back(parsed);
    }
  }
  while (autoexec_next_ < autoexec_steps_.size()) {
    const AutoexecStep& step = autoexec_steps_[autoexec_next_];
    const bool due = step.seconds > 0.0
                         ? std::chrono::duration<double>(now - autoexec_last_time_).count() >= step.seconds
                         : frame_ - autoexec_last_frame_ >= step.frames;
    if (!due) break;
    ++autoexec_next_;
    autoexec_last_frame_ = frame_;
    autoexec_last_time_ = now;
    if (step.commands.empty()) continue;
    REXLOG_INFO("cod3: autoexec '{}'", step.commands);
    // Through the console, so its own commands (sv_cheats, fov, quit ...) work
    // too and the cheat guard applies; game commands still go to the game's
    // command buffer in order.
    ui::GameConsole::Get().Submit(step.commands, cheats::Source::kAutoexec);
  }
  if (autoexec_next_ >= autoexec_steps_.size()) autoexec_done_ = true;
}

void Engine::OnGuestFrame(PPCContext& ctx, uint8_t* base) {
  ++frame_;
  // [hw-scaling] this is the game's main thread (game/thread_profiler.h).
  ThreadProfiler::Get().NoteGameMainThread();
  if (REXCVAR_GET(cod3_debug_frames)) {
    // Every 10 seconds of real time, however slow the frames are.
    static auto last = std::chrono::steady_clock::now();
    const auto now = std::chrono::steady_clock::now();
    if (now - last >= std::chrono::seconds(10)) {
      last = now;
      REXLOG_INFO("cod3: frame {}; {}", frame_, ScriptThreadSummary());
    }
  }
  if (!game_ready_) {
    // Cvar_Init has run once the sorted cvar list has a head.
    if (ReadU32(base, a::kCvarVars) == 0) return;
    FirstFrame(ctx, base);
  }

  std::vector<std::string> commands;
  std::vector<std::function<void(PPCContext&, uint8_t*)>> tasks;
  {
    std::lock_guard lock(mutex_);
    commands.swap(pending_commands_);
    tasks.swap(pending_tasks_);
  }
  for (auto& task : tasks) task(ctx, base);
  for (auto& line : commands) CbufAddText(ctx, base, line + "\n");

  cheats::Sync(base);  // sv_cheats for the console and the HUD
  hd::SyncMultiplayerMode();  // texture replacement follows the match state (game/hd_textures.h)
  fov::Sync(ctx, base);  // the player's cg_fov / cg_fovScale (game/fov.h)
  SyncWatchedCvars(ctx, base);
  settings::SyncCrosshair(ctx, base);  // cg_drawCrosshair = Options > Gameplay > Crosshair
  if ((frame_ % 10) == 0) CheatWatch::Get().Update(base);
  CutsceneSkip::Get().OnGuestFrame(ctx, base);
  RunAutoexec(ctx, base);

  // Commands queued this frame run inside the original Cbuf_Execute right
  // after we return, so refresh the snapshot on the following frame.
  bool snap = snapshot_requested_.exchange(false) || snapshot_next_frame_;
  snapshot_next_frame_ = !commands.empty();
  if (snap) TakeSnapshot(base);
}

}  // namespace cod3

// ---------------------------------------------------------------------------
// Hook: Cbuf_Execute - runs on the guest main thread every frame.
// ---------------------------------------------------------------------------
REX_EXTERN(__imp__Cbuf_Execute);
REX_HOOK_RAW(Cbuf_Execute) {
  cod3::Engine::Get().OnGuestFrame(ctx, base);
  __imp__Cbuf_Execute(ctx, base);
}
