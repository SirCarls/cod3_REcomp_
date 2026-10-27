// cod3 - skipping cutscenes.
//
// Call of Duty 3 has these (full list with addresses:
// dev/agents/cutscenes/NOTES.md):
//
// Movies played by a blocking loop (0x824EF308): the legal screen, the
// Activision and Treyarch logos and the attract film at start, the attract
// film again when the title screen or main menu sits idle, the bonus movies
// and the "Finale" after the credits. The loop asks 0x824D0470 every frame
// whether to stop: once the movie has run its minimum time (0x82A2A254
// elapsed vs 0x82A2A258: the legal screen's 5 seconds, 0 for the others) any
// controller button stops it. A mid-asm hook in that check
// (config/default.toml) takes the game's own skip path when the player asks,
// so the game sees an ordinary skip.
//
// Level briefings (movies/<level>.wmv with its narration). The level loader
// 0x824636D0 starts it and makes it the loading screen (0x824EECF8, which
// the loader calls while it works); once the level is ready, SV_SpawnServer
// plays the rest in its own loop (0x824EEBA0). The loader sets the minimum
// time to FLT_MAX ("never", from 0x82063FB0), so on the console a briefing
// can't be skipped at all: it plays to its end even after the level is ready
// (owner's run 2026-10-10: Space did nothing for the 31 s it played on after
// the load). The owner wants every mission's loading cutscene skippable, so:
// - while the level loads, a skip stops the movie with the game's own stop
//   function and puts the game's plain loading screen back (what the game
//   shows for a level without a briefing) until the level is ready;
// - once it's ready, the "never" minimum is lowered to 0, so the game's own
//   skip check takes any button, like for the logos.
//
// Intro screens. Each level then opens with the level script's intro: the
// mission's title lines over black with the narration, then the camera
// coming in. The `introScreen` dvar is 1 the whole time (the game itself uses
// it to keep the pause menu and objectives closed). The script runs on game
// time and has no skip of its own, so skipping fades the picture to black,
// mutes the sound and runs game time fast (timescale, set like the game's own
// code does, so it isn't a cheat) until the intro ends, then fades back in.
//
// Not skipped, as on the console: the in-game cinematics inside missions
// (scripted scenes, some letterboxed) and the credits level (START already
// ends it: its script waits for that button).
//
// Every skippable cutscene: "SPACE Skip" at the bottom of the screen; Space,
// Enter, Escape or a left click skips it, whatever the binds, and so does a
// controller's A or START; cod3_skip_movies skips them all as soon as
// possible (F2 > Game), which also makes automated tests minutes shorter.
//
// [acct2-decomp] 2026-10-25 (owner 23:44, asked which: "Only the briefing
// movie"): the player's skip (keys, A / START, the prompt) is for a level's
// briefing only. The logos, attract film, bonus movies and Finale keep the
// game's own skip (any button after their minimum time, as on the console);
// the level intros play in full. cod3_skip_movies still skips everything.
// The prompt shows the key while the keyboard is in use and the
// controller's A once a controller is (game/button_prompts.h
// FirstPlayerLook), and fades in and out.
//
// No timers decide anything (owner, 2026-10-10): a movie is skippable while
// the game's movie loop runs (hooks on its start and end) and past the game's
// own minimum time; a briefing while the game draws it as the loading screen
// and while its post-load loop runs; an intro while introScreen is 1, and its
// skip ends when the game sets it back to 0.

#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>

struct PPCContext;

namespace cod3 {

class CutsceneSkip {
 public:
  static CutsceneSkip& Get();

  // UI thread: the player pressed a skip key or clicked.
  void RequestSkip();

  // Guest main thread, from mid-asm hooks: a movie loop starts and ends
  // (0x824EF308 at start-up; 0x824EEBA0, a briefing, after a level loads).
  void OnMovieLoop(bool running, bool briefing = false);
  // Guest main thread, from the mid-asm hook in the skip check, every frame
  // of a movie loop while the game's own check found no button: true = skip
  // now. `skip_state` is r31, which the game's skip path stores through; it
  // is set when skipping (see the hook).
  bool OnMovieFrame(uint8_t* base, uint32_t& skip_state);
  // Guest main thread, from the mid-asm hook on the game's skip path: a
  // movie is being skipped (by any button or by the hook above). Logs it.
  void OnMovieSkipped();
  // Guest main thread, from the mid-asm hook at the start of the briefing's
  // loading screen (0x824EECF8): true = the movie was stopped, skip this
  // frame of it.
  bool OnLoadingMovie(PPCContext& ctx, uint8_t* base);

  // Guest main thread, every frame (Engine::OnGuestFrame): intro screens.
  void OnGuestFrame(PPCContext& ctx, uint8_t* base);

  // UI thread: the prompt's opacity (0 = hidden) and the black cover's.
  float PromptAlpha() const;
  float CoverAlpha() const;

 private:
  CutsceneSkip() = default;
  using Clock = std::chrono::steady_clock;
  static int64_t Now() { return Clock::now().time_since_epoch().count(); }
  bool Skippable() const;
  bool TakeRequest();
  // Guest main thread: a controller's A or START went down since the last
  // call; `first` = the first call of this cutscene (a button already held,
  // e.g. from skipping the one before, doesn't count).
  bool PadPressed(bool first);
  void SetSkippable(std::atomic<bool>& flag, bool on);
  void EndIntroSkip(PPCContext& ctx, uint8_t* base, const char* why);

  // What's on screen, as the game reports it (no timers): a movie loop is
  // running and past its minimum time, a briefing is the loading screen, or
  // a level intro is running.
  std::atomic<bool> movie_loop_{false};
  std::atomic<bool> briefing_loop_{false};  // the running movie loop is a level's briefing
  std::atomic<bool> movie_skippable_{false};
  std::atomic<bool> loading_skippable_{false};  // the briefing plays as the loading screen
  std::atomic<bool> intro_skippable_{false};
  std::atomic<bool> skip_requested_{false};
  std::atomic<int64_t> skippable_since_{0};  // for the prompt's fade-in only
  std::atomic<int64_t> hidden_since_{0};     // [acct2-decomp] and its fade-out
  uint16_t pad_buttons_ = 0;                 // PadPressed: the buttons seen last time

  // Intro screens (guest main thread, except the atomics read by the UI).
  uint32_t intro_cvar_ = 0;                 // cvar_t* of introScreen
  uint32_t skip_frames_ = 0;                // game frames since the skip started
  bool gave_up_ = false;                    // the safety limit ended this intro's skip
  std::atomic<bool> skipping_{false};
  std::atomic<int64_t> skip_started_{0};
  std::atomic<int64_t> skip_ended_{0};
  std::string saved_timescale_;
};

}  // namespace cod3
