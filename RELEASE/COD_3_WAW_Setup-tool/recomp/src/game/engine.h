// cod3 - bridge between host code (UI thread) and the game's Q3 engine
// (guest main thread).
//
// Host code never calls into the guest directly from the UI thread. Instead
// it queues work here; the queue is drained on the guest main thread from our
// Cbuf_Execute hook, once per game frame. The same hook takes a snapshot of
// the game's cvars and commands that the console uses for autocompletion.

#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <vector>

struct PPCContext;

namespace cod3 {

struct CvarInfo {
  std::string name;
  std::string value;
  std::string reset;
  uint32_t flags = 0;
};

struct EngineSnapshot {
  std::vector<CvarInfo> cvars;          // sorted by name
  std::vector<std::string> commands;    // sorted, unique
  uint64_t generation = 0;
};

class Engine {
 public:
  static Engine& Get();

  // --- called from any thread -------------------------------------------
  // Queue a console line ("map island; god") for the game's command buffer.
  void QueueCommand(std::string line);
  // Queue arbitrary work to run on the guest main thread.
  void QueueGuestTask(std::function<void(PPCContext&, uint8_t*)> task);
  // Latest cvar/command snapshot (copied) and its generation counter.
  EngineSnapshot Snapshot();
  uint64_t SnapshotGeneration();
  // Ask for a fresh snapshot on the next game frame.
  void RequestSnapshot() { snapshot_requested_ = true; }
  bool GameReady() const { return game_ready_; }

  // Persistent settings (written to <user_data>/cod3_config.cfg as `seta` lines).
  void SetConfigPath(std::filesystem::path path);
  void SetArchived(const std::string& cvar, const std::string& value);
  // Removes a cvar from the config (a cheat-protected one found there).
  void DropArchived(const std::string& cvar);
  std::map<std::string, std::string> ArchivedCvars();
  void SaveConfig();

  // The player's own cg_fov / cg_fovScale, kept on the host so the UI never
  // touches guest memory, and always within the effective range (game/fov.h;
  // defined in fov.cpp). A game-made change (scripted view, vehicle) is not
  // the player's and isn't shown or saved here.
  float fov() const;
  float fov_scale() const;
  void SetFov(float fov);            // sets cg_fov  (limited, queued, persisted)
  void SetFovScale(float scale);     // sets cg_fovScale (limited, queued, persisted)
  void SetFovSettings(float fov, float scale);  // both at once (a Reset)
  // Guest-thread variant used by the native options menu (applies immediately).
  void ApplyFovFromGuest(PPCContext& ctx, uint8_t* base, float fov);

  // Frame rate limit and counter, World at War style (see game/frame_rate.h).
  // Both are real game cvars, so the console, `seta`, `toggle` and exec'd
  // configs work on them; these setters are for the settings window.
  void SetMaxFps(int fps);            // com_maxfps, 0 = no limit (queued, persisted)
  void SetDrawFps(int mode);          // cg_drawFPS 0..3 (queued, persisted)
  // Console settings (everything saved with seta / in cod3_config.cfg) back
  // to the game's defaults; the config file is emptied. Queued.
  void ResetArchivedCvars();
  int draw_fps() const { return draw_fps_; }  // cg_drawFPS as a number

  // --- guest main thread only ----------------------------------------------
  void OnGuestFrame(PPCContext& ctx, uint8_t* base);
  // cvar_t* for cg_fovScale, 0 until registered.
  uint32_t fov_scale_cvar() const { return fov_scale_cvar_; }
  uint32_t developer_cvar() const { return developer_cvar_; }

  // Helpers usable from guest-thread hooks.
  static uint32_t CvarFind(PPCContext& ctx, uint8_t* base, const char* name);
  static uint32_t CvarGet(PPCContext& ctx, uint8_t* base, const char* name, const char* value,
                          uint32_t flags);
  static void CvarSet(PPCContext& ctx, uint8_t* base, const char* name, const char* value,
                      bool force);
  static void CbufAddText(PPCContext& ctx, uint8_t* base, const std::string& text);

 private:
  Engine() = default;
  void LoadConfig();
  void TakeSnapshot(uint8_t* base);
  void FirstFrame(PPCContext& ctx, uint8_t* base);
  void RunAutoexec(PPCContext& ctx, uint8_t* base);
  void SyncWatchedCvars(PPCContext& ctx, uint8_t* base);

  std::mutex mutex_;
  std::vector<std::string> pending_commands_;
  std::vector<std::function<void(PPCContext&, uint8_t*)>> pending_tasks_;
  EngineSnapshot snapshot_;
  std::atomic<bool> snapshot_requested_{true};
  bool snapshot_next_frame_ = false;
  std::atomic<bool> game_ready_{false};
  uint64_t frame_ = 0;
  bool autoexec_done_ = false;
  // cod3_autoexec's steps (RunAutoexec): how long after the previous step
  // (frames, or seconds for "@30s"), and what.
  struct AutoexecStep {
    uint64_t frames = 0;
    double seconds = 0.0;
    std::string commands;
  };
  std::vector<AutoexecStep> autoexec_steps_;
  size_t autoexec_next_ = 0;
  bool autoexec_parsed_ = false;
  uint64_t autoexec_last_frame_ = 0;  // when the previous step ran (or the game came up)
  std::chrono::steady_clock::time_point autoexec_last_time_;

  std::filesystem::path config_path_;
  std::map<std::string, std::string> archived_;
  bool config_loaded_ = false;

  uint32_t fov_scale_cvar_ = 0;
  uint32_t developer_cvar_ = 0;

  // Game cvars whose console changes are saved to the config like `seta`.
  struct WatchedCvar {
    const char* name;
    uint32_t cvar = 0;     // cvar_t*, 0 until found
    std::string value;     // last value seen
    bool seen = false;     // the first value seen is the starting value, not a change
  };
  WatchedCvar watched_[2] = {{"com_maxfps"}, {"cg_drawFPS"}};
  std::atomic<int> draw_fps_{0};
};

}  // namespace cod3
