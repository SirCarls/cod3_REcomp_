// cod3 - skipping cutscenes (see cutscene_skip.h).

#include "game/cutscene_skip.h"

#include <algorithm>

#include <rex/audio/downmix.h>
#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/input/input.h>
#include <rex/input/input_system.h>
#include <rex/logging.h>
#include <rex/runtime.h>
#include <rex/system/kernel_state.h>

#include "game/addresses.h"
#include "game/diagnostics.h"
#include "game/engine.h"
#include "game/guest.h"

REXCVAR_DEFINE_BOOL(cod3_skip_movies, false, "cod3/Game",
                    "Skip cutscenes (logos, level briefings, level intros) as soon as the game "
                    "allows it");
REXCVAR_DEFINE_BOOL(cod3_skip_prompt, true, "cod3/Game",
                    "Show \"SPACE Skip\" while a cutscene can be skipped");
REXCVAR_DECLARE(double, cod3_volume);

namespace cod3 {

namespace {
using namespace std::chrono_literals;
template <typename D>
constexpr int64_t Ticks(D d) {
  return std::chrono::duration_cast<std::chrono::steady_clock::duration>(d).count();
}
// Looks only (the prompt's fade-in, the cover's fades). What can be skipped
// and when the skip ends come from the game's own state, never from these.
constexpr int64_t kPromptFadeIn = Ticks(400ms);
constexpr int64_t kCoverFadeOut = Ticks(250ms);
constexpr int64_t kCoverFadeIn = Ticks(600ms);
// Safety net only: if a skipped intro somehow never reports its end, give
// the player the game back after this many game frames (at least two
// minutes of game time even when the game caps each frame's time step:
// many times longer than any intro). Counted in frames, not seconds, so a
// slow PC doesn't run out of time.
constexpr uint32_t kSkipFrameLimit = 2400;
// Game time per real time while an intro is skipped: a minute of intro in
// about 3 seconds, in steps of ~330 ms at 60 fps.
constexpr const char* kSkipTimescale = "20";
// A movie minimum at or above this is the level loader's "never" (FLT_MAX).
constexpr float kNeverMs = 1.0e30f;
// The controller buttons that skip (Space and Enter/Escape in the default
// keyboard binds).
constexpr uint16_t kPadSkipButtons = rex::input::X_INPUT_GAMEPAD_A | rex::input::X_INPUT_GAMEPAD_START;

constexpr uint32_t kCvarString = 4;    // cvar_t: char* string
constexpr uint32_t kCvarInteger = 32;  // cvar_t: int integer

// Player one's controller buttons (every pad and the keyboard binds merged).
uint16_t PadButtons() {
  using rex::X_RESULT;  // for X_ERROR_SUCCESS
  auto* kernel = REX_KERNEL_STATE();
  auto* runtime = kernel ? kernel->emulator() : nullptr;
  auto* input = runtime ? static_cast<rex::input::InputSystem*>(runtime->input_system()) : nullptr;
  rex::input::X_INPUT_STATE state{};
  if (!input || input->GetState(0, &state) != X_ERROR_SUCCESS) return 0;
  return state.gamepad.buttons;
}
}  // namespace

CutsceneSkip& CutsceneSkip::Get() {
  static CutsceneSkip instance;
  return instance;
}

bool CutsceneSkip::Skippable() const {
  return (movie_loop_.load(std::memory_order_relaxed) &&
          movie_skippable_.load(std::memory_order_relaxed)) ||
         loading_skippable_.load(std::memory_order_relaxed) ||
         intro_skippable_.load(std::memory_order_relaxed);
}

void CutsceneSkip::RequestSkip() {
  // Only while something can be skipped right now: a key pressed in a menu
  // or in game must not skip the next cutscene.
  if (Skippable()) skip_requested_.store(true, std::memory_order_relaxed);
}

bool CutsceneSkip::TakeRequest() {
  const bool asked = skip_requested_.exchange(false, std::memory_order_relaxed);
  return asked || REXCVAR_GET(cod3_skip_movies);
}

bool CutsceneSkip::PadPressed(bool first) {
  const uint16_t now = PadButtons();
  const uint16_t went_down = now & ~pad_buttons_;
  pad_buttons_ = now;
  return !first && (went_down & kPadSkipButtons) != 0;
}

void CutsceneSkip::SetSkippable(std::atomic<bool>& flag, bool on) {
  const bool was = Skippable();
  flag.store(on, std::memory_order_relaxed);
  if (!was && Skippable()) skippable_since_.store(Now(), std::memory_order_relaxed);
  if (!Skippable()) skip_requested_.store(false, std::memory_order_relaxed);
}

void CutsceneSkip::OnMovieLoop(bool running, bool briefing) {
  if (running) {
    briefing_loop_.store(briefing, std::memory_order_relaxed);
    movie_loop_.store(true, std::memory_order_relaxed);
    // A briefing's loop only starts once the level is ready: skippable from
    // its first frame (OnMovieFrame lowers the game's "never" minimum).
    if (briefing) {
      SetSkippable(movie_skippable_, true);
      REXLOG_INFO("cod3: the level is ready; its briefing plays on until skipped or over");
    }
    // Its loading-screen part is over (after the line above, so the prompt
    // stays up without fading in again).
    SetSkippable(loading_skippable_, false);
    return;
  }
  SetSkippable(movie_skippable_, false);
  briefing_loop_.store(false, std::memory_order_relaxed);
  movie_loop_.store(false, std::memory_order_relaxed);
}

bool CutsceneSkip::OnMovieFrame(uint8_t* base, uint32_t& skip_state) {
  namespace m = addr::movie;
  const float elapsed = guest::ReadF32(base, m::kElapsedMs);
  float minimum = guest::ReadF32(base, m::kMinimumMs);
  // "Never": what the level loader gives a briefing (a movie played after a
  // level without one would inherit it too). The briefing's loop only runs
  // once the level is ready, so lower it to 0: from the next frame on, the
  // game's own check stops the movie on any controller button, like the
  // logos. Other movies keep the game's own minimum (the legal screen's 5 s).
  if (minimum >= kNeverMs) {
    guest::WriteF32(base, m::kMinimumMs, 0.0f);
    minimum = 0.0f;
  }
  const bool skippable = elapsed >= minimum;
  if (skippable != movie_skippable_.load(std::memory_order_relaxed))
    SetSkippable(movie_skippable_, skippable);
  if (!skippable || !TakeRequest()) return false;
  SetSkippable(movie_skippable_, false);
  // The skip path stores "skipped" through r31, which the game sets up only
  // on its no-button path; set it here so the minimum-time path (taken by a
  // briefing's first frame) works the same.
  skip_state = m::kSkipState;
  return true;
}

void CutsceneSkip::OnMovieSkipped() {
  REXLOG_INFO("cod3: {} skipped{}",
              briefing_loop_.load(std::memory_order_relaxed) ? "briefing" : "movie",
              REXCVAR_GET(cod3_skip_movies) ? " (cod3_skip_movies)" : "");
}

bool CutsceneSkip::OnLoadingMovie(PPCContext& ctx, uint8_t* base) {
  namespace m = addr::movie;
  // The briefing is the loading screen while the level loads. (If the film
  // ends before the level is ready it holds its last frame; a skip then just
  // brings the plain loading screen back sooner.)
  const bool playing = guest::ReadU32(base, m::kHandle) != 0;
  const bool was = loading_skippable_.load(std::memory_order_relaxed);
  if (playing != was) SetSkippable(loading_skippable_, playing);
  if (!playing) return false;
  const bool pad = PadPressed(/*first=*/!was);
  if (!pad && !TakeRequest()) return false;

  // Stop it the game's own way (frees the movie and its sound, draws the
  // loading screen), then let the loader draw the plain loading screen
  // until the level is ready, as for a level without a briefing. The
  // briefing's post-load loop then finds no movie and the level starts.
  SetSkippable(loading_skippable_, false);
  guest::Call(ctx, base).Invoke(m::kStop, {1});
  const uint32_t loader = guest::ReadU32(base, m::kLoader);
  if (loader && guest::ReadU32(base, loader + m::kLoaderScreen) == m::kLoadingMovie)
    guest::WriteU32(base, loader + m::kLoaderScreen, m::kLoadingScreen);
  REXLOG_INFO("cod3: briefing skipped while the level loads{}",
              REXCVAR_GET(cod3_skip_movies) ? " (cod3_skip_movies)" : "");
  return true;
}

void CutsceneSkip::OnGuestFrame(PPCContext& ctx, uint8_t* base) {
  // A game frame runs only when no level is loading, so a briefing isn't the
  // loading screen any more (normally its post-load loop said so already).
  if (loading_skippable_.load(std::memory_order_relaxed)) SetSkippable(loading_skippable_, false);

  if (!intro_cvar_) intro_cvar_ = Engine::CvarFind(ctx, base, "introScreen");
  if (!intro_cvar_) return;
  // The level script's intro: the game keeps introScreen at 1 until it ends.
  const bool intro = guest::ReadS32(base, intro_cvar_ + kCvarInteger) != 0;

  if (skipping_.load(std::memory_order_relaxed)) {
    if (!intro) {
      EndIntroSkip(ctx, base, "the intro ended");
    } else if (++skip_frames_ > kSkipFrameLimit) {
      EndIntroSkip(ctx, base, "the intro didn't end; gave up");
      gave_up_ = true;  // don't start over on this intro
    }
    return;
  }
  if (!intro) gave_up_ = false;
  const bool skippable = intro && !gave_up_;
  const bool was = intro_skippable_.load(std::memory_order_relaxed);
  if (skippable != was) SetSkippable(intro_skippable_, skippable);
  if (!skippable) return;
  const bool pad = PadPressed(/*first=*/!was);
  if (!pad && !TakeRequest()) return;

  // Fade out, mute, and run the rest of the intro on fast game time, so
  // everything it sets up (squad, objectives, music) happens as usual.
  const uint32_t timescale = Engine::CvarFind(ctx, base, "timescale");
  saved_timescale_ =
      timescale ? guest::ReadString(base, guest::ReadU32(base, timescale + kCvarString), 32)
                : std::string("1");
  if (saved_timescale_.empty()) saved_timescale_ = "1";
  Engine::CvarSet(ctx, base, "timescale", kSkipTimescale, true);  // forced, like the game's code
  rex::audio::SetOutputGain(0.0f);
  SetSkippable(intro_skippable_, false);
  skip_started_.store(Now(), std::memory_order_relaxed);
  skip_frames_ = 0;
  skipping_.store(true, std::memory_order_relaxed);
  REXLOG_INFO("cod3: skipping the level intro{}",
              REXCVAR_GET(cod3_skip_movies) ? " (cod3_skip_movies)" : "");
}

void CutsceneSkip::EndIntroSkip(PPCContext& ctx, uint8_t* base, const char* why) {
  Engine::CvarSet(ctx, base, "timescale", saved_timescale_.c_str(), true);
  rex::audio::SetOutputGain(float(REXCVAR_GET(cod3_volume)));
  skipping_.store(false, std::memory_order_relaxed);
  skip_ended_.store(Now(), std::memory_order_relaxed);
  REXLOG_INFO("cod3: level intro skipped ({}; {:.1f} s)", why,
              double(Now() - skip_started_.load(std::memory_order_relaxed)) / double(Ticks(1s)));
}

float CutsceneSkip::PromptAlpha() const {
  if (!REXCVAR_GET(cod3_skip_prompt) || REXCVAR_GET(cod3_skip_movies) || !Skippable()) return 0.0f;
  const double t =
      double(Now() - skippable_since_.load(std::memory_order_relaxed)) / double(kPromptFadeIn);
  return float(std::clamp(t, 0.0, 1.0));
}

float CutsceneSkip::CoverAlpha() const {
  const int64_t now = Now();
  if (skipping_.load(std::memory_order_relaxed)) {
    const double t =
        double(now - skip_started_.load(std::memory_order_relaxed)) / double(kCoverFadeOut);
    return float(std::clamp(t, 0.0, 1.0));
  }
  const int64_t ended = skip_ended_.load(std::memory_order_relaxed);
  if (!ended) return 0.0f;
  const double t = double(now - ended) / double(kCoverFadeIn);
  return float(std::clamp(1.0 - t, 0.0, 1.0));
}

}  // namespace cod3

// ---------------------------------------------------------------------------
// Mid-asm hook in the movie skip check 0x824D0470 (config/default.toml), at
// 0x824D050C, where both "keep playing" paths meet (minimum time not reached,
// or no button pressed). Returning true jumps to 0x824D04E4, the game's own
// skip path (clears the pressed buttons, stores "skipped" at r31 + 54,
// returns 1). The game loads r31 (0x82A8DF2C) only on the no-button path, so
// the hook sets it before taking the skip path from the other one.
// ---------------------------------------------------------------------------
bool Cod3MovieSkipHook(PPCRegister& r31, uint8_t* base) {
  uint32_t state = r31.u32;
  if (!cod3::CutsceneSkip::Get().OnMovieFrame(base, state)) return false;
  r31.u64 = state;
  return true;
}

// Mid-asm hook on the game's skip path 0x824D04E4: a movie is being skipped,
// by the game's own button check or by the hook above.
void Cod3MovieSkipped() { cod3::CutsceneSkip::Get().OnMovieSkipped(); }

// Mid-asm hook on the first instruction of 0x824EECF8, the briefing as the
// loading screen (config/default.toml): true returns at once (the movie was
// stopped; the plain loading screen takes over from the next call).
bool Cod3LoadingMovieHook(PPCContext& ctx, uint8_t* base) {
  return cod3::CutsceneSkip::Get().OnLoadingMovie(ctx, base);
}

// Mid-asm hooks where each movie loop starts and ends (config/default.toml):
// 0x824EF308 (logos at start-up, attract, bonus movies, the finale) and
// 0x824EEBA0 (a level's briefing, once the level has loaded).
void Cod3MovieLoopBegin() { cod3::CutsceneSkip::Get().OnMovieLoop(true); }
void Cod3BriefingLoopBegin() {
  cod3::CutsceneSkip::Get().OnMovieLoop(true, /*briefing=*/true);
  cod3::diag::LevelLoadMovie();  // the briefing playing on after the load (load time log)
}
void Cod3MovieLoopEnd() { cod3::CutsceneSkip::Get().OnMovieLoop(false); }
