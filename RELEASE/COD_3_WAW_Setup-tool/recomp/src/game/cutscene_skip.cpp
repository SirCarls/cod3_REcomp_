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
#include "game/button_prompts.h"  // [acct2-decomp] the device the prompt shows

REXCVAR_DEFINE_BOOL(cod3_skip_movies, false, "cod3/Game",
                    "Skip cutscenes (logos, level briefings, level intros) as soon as the game "
                    "allows it");
REXCVAR_DEFINE_BOOL(cod3_skip_prompt, true, "cod3/Game",
                    "Show \"SPACE Skip\" (or the controller's button) while a level's briefing "
                    "can be skipped");
// [claude-9b-think] 2026-10-27 UNTESTED (see cutscene_skip.h).
REXCVAR_DEFINE_STRING(cod3_skip_prompt_position, "corner", "cod3/Game",
                      "Where the skip prompt is drawn: corner (the bottom-right corner of the HUD's safe area, "
                      "small, out of the subtitles' way) or bottom_center (as before, where the loading line is)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_INT32(cod3_skip_hold_ms, 1000, "cod3/Game",
                     "How long the skip key (Space, Enter, Escape, a left click) or the controller's A / START is "
                     "held to skip a level's briefing, in milliseconds; a fill on the prompt shows the hold. 0: a "
                     "press skips at once (as before)")
    .range(0, 5000)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
// [claude-9b-think] 2026-10-28 UNTESTED (s153: a start-up hang after two logos were stopped 32 ms apart; see the
// automatic skip in OnMovieFrame).
REXCVAR_DEFINE_INT32(cod3_skip_movies_after_ms, 250, "cod3/Game",
                     "With cod3_skip_movies: how much of a start-up logo, the attract film or a bonus film plays "
                     "before it is skipped, in milliseconds of its own playback (and at least 3 frames), so its "
                     "decoder is running when it is stopped. 0: skipped at the first chance (as before). Briefings "
                     "and the player's own skips are not affected")
    .range(0, 5000)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
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
// [acct2-decomp] Owner 2026-10-26 00:00: "Please make it fade in and out as well."
constexpr int64_t kPromptFadeOut = Ticks(400ms);
// [claude-9b-think] How long the prompt shows at the film's start and after a press, and its fade-in after a press.
constexpr int64_t kPromptShown = Ticks(4s);
constexpr int64_t kPromptReveal = Ticks(150ms);
constexpr int64_t kCoverFadeOut = Ticks(250ms);
constexpr int64_t kCoverFadeIn = Ticks(600ms);
// Safety net only: if a skipped intro somehow never reports its end, give
// the player the game back after this many game frames (at least two
// minutes of game time even when the game caps each frame's time step:
// many times longer than any intro). Counted in frames, not seconds, so a
// slow PC doesn't run out of time.
constexpr uint32_t kSkipFrameLimit = 2400;
// [claude-9b] 2026-10-27 ...and at least this much real time, so a fast PC doesn't run out of frames: uncapped
// (~600 fps) 2400 frames passed in 3.8 s and Saint-Lo's intro was given up before it ended (claude-9b-test s149r;
// at 60 fps it ends at 6.9-7.0 s). A minute of real time is 20 minutes of intro at the skip's timescale.
constexpr int64_t kSkipMinTime = Ticks(std::chrono::seconds(60));
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
// [acct2-decomp] The read also tells the prompts a controller is in use
// (button_prompts.h NotePadState): the game may not read its pads while a
// briefing is the loading screen, so "SPACE Skip" would never switch.
uint16_t PadButtons() {
  using rex::X_RESULT;  // for X_ERROR_SUCCESS
  auto* kernel = REX_KERNEL_STATE();
  auto* runtime = kernel ? kernel->emulator() : nullptr;
  auto* input = runtime ? static_cast<rex::input::InputSystem*>(runtime->input_system()) : nullptr;
  rex::input::X_INPUT_STATE state{};
  if (!input || input->GetState(0, &state) != X_ERROR_SUCCESS) return 0;
  const uint16_t buttons = state.gamepad.buttons;
  const int16_t axes[4] = {static_cast<int16_t>(state.gamepad.thumb_lx), static_cast<int16_t>(state.gamepad.thumb_ly),
                           static_cast<int16_t>(state.gamepad.thumb_rx), static_cast<int16_t>(state.gamepad.thumb_ry)};
  prompts::NotePadState(buttons, state.gamepad.left_trigger, state.gamepad.right_trigger, axes);
  return buttons;
}

// [claude-9b-think] 2026-10-28 UNTESTED cod3_skip_movies_after_ms: the movie an automatic skip waits on (guest main
// thread only: OnMovieLoop and OnMovieFrame). A new movie object starts a new count.
constexpr uint32_t kAutoSkipMinFrames = 3;
// If the movie's own clock stands still, real time since its first frame decides, with this margin.
constexpr int64_t kAutoSkipClockMargin = Ticks(2s);
struct AutoSkipMovie {
  uint32_t handle = 0;
  uint32_t frames = 0;
  int64_t since = 0;  // steady_clock ticks at its first frame; 0 = none yet
};
AutoSkipMovie g_auto_skip;

int64_t NowTicks() { return std::chrono::steady_clock::now().time_since_epoch().count(); }

// True when an automatic skip (cod3_skip_movies) may stop the current movie, `elapsed_ms` its own clock.
bool AutoSkipDue(float elapsed_ms) {
  const int32_t after_ms = REXCVAR_GET(cod3_skip_movies_after_ms);
  if (after_ms <= 0) return true;  // as before: the first chance
  if (g_auto_skip.frames < kAutoSkipMinFrames) return false;
  if (elapsed_ms >= float(after_ms)) return true;
  return NowTicks() - g_auto_skip.since >= Ticks(std::chrono::milliseconds(after_ms)) + kAutoSkipClockMargin;
}

// For the "movie skipped" line: the frames and the real time since the current movie's first frame.
std::string AutoSkipPlayed() {
  if (!g_auto_skip.since) return {};
  const int64_t ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                         std::chrono::steady_clock::duration(NowTicks() - g_auto_skip.since))
                         .count();
  return ", after " + std::to_string(g_auto_skip.frames) + " frames, " + std::to_string(ms) + " ms";
}
}  // namespace

CutsceneSkip& CutsceneSkip::Get() {
  static CutsceneSkip instance;
  return instance;
}

// [acct2-decomp] Owner 2026-10-25 23:44: "cutscene skipping should only be
// available when it's an in-game movie for an intro to a level", and asked
// which: "Only the briefing movie". So the skip keys, the controller's A /
// START and the prompt work only for a level's briefing (the loading screen
// movie and its post-load loop). The start-up logos, attract film, bonus
// movies and Finale keep only the game's own skip (any controller button
// after their minimum time, as on the console), and the level intros play in
// full. cod3_skip_movies (off by default; F2 > Game, and the test runs) still
// skips them all.
bool CutsceneSkip::Skippable() const {
  return (movie_loop_.load(std::memory_order_relaxed) && briefing_loop_.load(std::memory_order_relaxed) &&
          movie_skippable_.load(std::memory_order_relaxed)) ||
         loading_skippable_.load(std::memory_order_relaxed) ||
         intro_skippable_.load(std::memory_order_relaxed);
}

void CutsceneSkip::RequestSkip() {
  // Only while something can be skipped right now: a key pressed in a menu
  // or in game must not skip the next cutscene.
  if (Skippable()) skip_requested_.store(true, std::memory_order_relaxed);
}

// [claude-9b-think] 2026-10-27 UNTESTED the hold (see cutscene_skip.h).
void CutsceneSkip::SkipKeyDown(uint32_t key) {
  const uint32_t before = skip_keys_down_;
  skip_keys_down_ |= key;
  if (before) return;  // a held key's repeat, or another skip key already down
  if (!Skippable()) return;           // pressed outside a film: it never starts a hold, even if held into one
  const int64_t now = Now();
  const bool shown = PromptShown(now);  // before this press shows it
  input_at_.store(now, std::memory_order_relaxed);
  if (REXCVAR_GET(cod3_skip_hold_ms) <= 0) {
    RequestSkip();  // a press skips, as before
    return;
  }
  if (!shown) return;  // the prompt was hidden: this press only shows it
  if (!hold_since_.load(std::memory_order_relaxed)) {
    hold_source_.store(1, std::memory_order_relaxed);
    hold_since_.store(Now(), std::memory_order_relaxed);
  }
}

void CutsceneSkip::SkipKeyUp(uint32_t key) {
  skip_keys_down_ &= ~key;
  if (skip_keys_down_ == 0 && hold_source_.load(std::memory_order_relaxed) == 1) {
    hold_since_.store(0, std::memory_order_relaxed);  // let go early: cancelled
    hold_source_.store(0, std::memory_order_relaxed);
  }
}

// With the prompt hidden (its 4 s are over), the first press only shows it; a press while it shows starts the hold
// (owner 13:21). With the prompt switched off, every press counts.
bool CutsceneSkip::PromptShown(int64_t t) const {
  return !REXCVAR_GET(cod3_skip_prompt) || Visible(t) > 0.0;
}

void CutsceneSkip::AnyInput() {
  if (Skippable()) input_at_.store(Now(), std::memory_order_relaxed);
}

float CutsceneSkip::HoldProgress() const {
  const int64_t since = hold_since_.load(std::memory_order_relaxed);
  const int hold_ms = REXCVAR_GET(cod3_skip_hold_ms);
  if (!since || hold_ms <= 0 || !Skippable()) return -1.0f;
  return float(std::clamp(double(Now() - since) / double(Ticks(std::chrono::milliseconds(hold_ms))), 0.0, 1.0));
}

bool CutsceneSkip::PadHold(bool first) {
  const uint16_t now = PadButtons();
  const uint16_t went_down = now & ~pad_buttons_;
  pad_buttons_ = now;
  if (first) return false;  // held from before the film: doesn't count
  const int64_t t = Now();
  const bool shown = PromptShown(t);  // before this press shows it
  if (went_down) input_at_.store(t, std::memory_order_relaxed);
  if ((went_down & kPadSkipButtons) && shown && !hold_since_.load(std::memory_order_relaxed)) {
    hold_source_.store(2, std::memory_order_relaxed);
    hold_since_.store(Now(), std::memory_order_relaxed);
  }
  if (hold_source_.load(std::memory_order_relaxed) == 2 && !(now & kPadSkipButtons)) {
    hold_since_.store(0, std::memory_order_relaxed);  // let go early: cancelled
    hold_source_.store(0, std::memory_order_relaxed);
  }
  return HoldDone();
}

bool CutsceneSkip::HoldDone() {
  const int hold_ms = REXCVAR_GET(cod3_skip_hold_ms);
  const int64_t since = hold_since_.load(std::memory_order_relaxed);
  if (hold_ms <= 0 || !since || Now() - since < Ticks(std::chrono::milliseconds(hold_ms))) return false;
  hold_since_.store(0, std::memory_order_relaxed);
  hold_source_.store(0, std::memory_order_relaxed);
  return true;
}

bool CutsceneSkip::TakeRequest() {
  const bool asked = skip_requested_.exchange(false, std::memory_order_relaxed);
  return asked || HoldDone() || REXCVAR_GET(cod3_skip_movies);  // [claude-9b-think] HoldDone
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
  if (was && !Skippable()) hidden_since_.store(Now(), std::memory_order_relaxed);  // [acct2-decomp] fade out
  if (!Skippable()) {
    skip_requested_.store(false, std::memory_order_relaxed);
    hold_since_.store(0, std::memory_order_relaxed);  // [claude-9b-think] a hold ends with the film
    hold_source_.store(0, std::memory_order_relaxed);
  }
}

void CutsceneSkip::OnMovieLoop(bool running, bool briefing) {
  g_auto_skip = AutoSkipMovie();  // [claude-9b-think] a movie loop starts or ends: a new count
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
  // [claude-9b-think] 2026-10-28 UNTESTED this movie's frames for an automatic skip (AutoSkipDue); another movie
  // object starts a new count.
  const uint32_t handle = guest::ReadU32(base, m::kHandle);
  if (handle != g_auto_skip.handle || !g_auto_skip.since) {
    g_auto_skip.handle = handle;
    g_auto_skip.frames = 0;
    g_auto_skip.since = NowTicks();
  }
  ++g_auto_skip.frames;
  const float elapsed = guest::ReadF32(base, m::kElapsedMs);
  float minimum = guest::ReadF32(base, m::kMinimumMs);
  // "Never": what the level loader gives a briefing (a movie played after a
  // level without one would inherit it too). The briefing's loop only runs
  // once the level is ready, so lower it to 0: from the next frame on, the
  // game's own check stops the movie on any controller button, like the
  // logos. Other movies keep the game's own minimum (the legal screen's 5 s).
  // [claude-9b-think] With cod3_skip_hold_ms, a briefing keeps "never": the game's own check then never skips it on
  // a press, and this hook (called every frame from the check's "keep playing" path) skips it when a hold is done.
  const bool briefing = briefing_loop_.load(std::memory_order_relaxed);
  const bool hold = briefing && REXCVAR_GET(cod3_skip_hold_ms) > 0 && !REXCVAR_GET(cod3_skip_movies);
  if (minimum >= kNeverMs && !hold) {
    guest::WriteF32(base, m::kMinimumMs, 0.0f);
    minimum = 0.0f;
  }
  const bool skippable = hold || elapsed >= minimum;
  const bool was_skippable = movie_skippable_.load(std::memory_order_relaxed);
  if (skippable != was_skippable)
    SetSkippable(movie_skippable_, skippable);
  if (!skippable) return false;
  if (hold) {  // [claude-9b-think] the controller's A / START held (the keys and the mouse in the UI thread)
    if (PadHold(/*first=*/!was_skippable)) {
      SetSkippable(movie_skippable_, false);
      skip_state = m::kSkipState;
      return true;
    }
  }
  // [acct2-decomp] Only a briefing takes the player's skip (Skippable); any
  // other movie only cod3_skip_movies (the game's own button check still
  // stops it, as on the console).
  if (!briefing_loop_.load(std::memory_order_relaxed)) {
    if (!REXCVAR_GET(cod3_skip_movies)) return false;
    // [claude-9b-think] 2026-10-28 UNTESTED not on its first frame: the movie's decoder threads are stopped
    // through the game's own skip path once it has played cod3_skip_movies_after_ms (s153's start-up hang came
    // from a logo stopped about 30 ms after it started).
    if (!AutoSkipDue(elapsed)) return false;
  } else if (!TakeRequest()) {
    return false;
  }
  SetSkippable(movie_skippable_, false);
  // The skip path stores "skipped" through r31, which the game sets up only
  // on its no-button path; set it here so the minimum-time path (taken by a
  // briefing's first frame) works the same.
  skip_state = m::kSkipState;
  return true;
}

void CutsceneSkip::OnMovieSkipped() {
  REXLOG_INFO("cod3: {} skipped{}{}",
              briefing_loop_.load(std::memory_order_relaxed) ? "briefing" : "movie",
              REXCVAR_GET(cod3_skip_movies) ? " (cod3_skip_movies)" : "",
              AutoSkipPlayed());  // [claude-9b-think] ", after N frames, X ms"
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
  // [claude-9b-think] With cod3_skip_hold_ms the controller's A / START is held too (PadHold), else pressed.
  const bool pad = REXCVAR_GET(cod3_skip_hold_ms) > 0 ? PadHold(/*first=*/!was) : PadPressed(/*first=*/!was);
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
    } else if (++skip_frames_ > kSkipFrameLimit &&
               Now() - skip_started_.load(std::memory_order_relaxed) >= kSkipMinTime) {  // [claude-9b] both
      EndIntroSkip(ctx, base, "the intro didn't end; gave up");
      gave_up_ = true;  // don't start over on this intro
    }
    return;
  }
  if (!intro) gave_up_ = false;
  // [acct2-decomp] Level intros play in full (owner 2026-10-25: only the
  // briefing movie is skippable); cod3_skip_movies still runs them fast.
  const bool skippable = intro && !gave_up_ && REXCVAR_GET(cod3_skip_movies);
  const bool was = intro_skippable_.load(std::memory_order_relaxed);
  if (skippable != was) SetSkippable(intro_skippable_, skippable);
  if (!skippable) return;

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

// [claude-9b-think] 2026-10-27 UNTESTED The prompt shows for kPromptShown at the film's start and after each press
// during it, and the whole time a skip is held; it fades in at the start (quicker after a press) and out after.
double CutsceneSkip::Visible(int64_t t) const {
  if (hold_since_.load(std::memory_order_relaxed)) return 1.0;
  const int64_t since = skippable_since_.load(std::memory_order_relaxed);
  const int64_t input = input_at_.load(std::memory_order_relaxed);
  auto window = [t](int64_t from, int64_t fade_in) {
    const int64_t end = from + kPromptShown;
    if (t < end) return std::clamp(double(t - from) / double(fade_in), 0.0, 1.0);
    return std::clamp(1.0 - double(t - end) / double(kPromptFadeOut), 0.0, 1.0);
  };
  double a = window(since, kPromptFadeIn);
  if (input >= since) a = std::max(a, window(input, kPromptReveal));
  return a;
}

float CutsceneSkip::PromptAlpha() const {
  if (!REXCVAR_GET(cod3_skip_prompt) || REXCVAR_GET(cod3_skip_movies)) return 0.0f;
  const int64_t now = Now();
  const int64_t since = skippable_since_.load(std::memory_order_relaxed);
  if (Skippable()) return float(Visible(now));  // [claude-9b-think] was: fully shown the whole time
  // [acct2-decomp] Fading out from where it had got to (a prompt still fading
  // in when the briefing ended starts lower).
  const int64_t hidden = hidden_since_.load(std::memory_order_relaxed);
  if (!hidden || hidden < since) return 0.0f;
  const double shown = Visible(hidden);  // [claude-9b-think]
  const double t = double(now - hidden) / double(kPromptFadeOut);
  return float(std::clamp(shown * (1.0 - t), 0.0, 1.0));
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
