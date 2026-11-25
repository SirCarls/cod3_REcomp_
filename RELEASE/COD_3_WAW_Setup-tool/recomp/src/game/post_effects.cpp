// cod3 - depth of field and motion blur on or off (see post_effects.h).
// Shared by both programs: the hooks name the campaign's functions, and
// cod3mp binds them to its own (mp/config/campaign_map.tsv); the guest
// addresses go through COD3_ADDR or addresses.h.

#include "game/post_effects.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <mutex>
#include <string_view>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc.h>
#include <rex/system/kernel_state.h>

#include "game/addresses.h"
#include "game/guest.h"
#include "game/program.h"
#include "game/build_address.h"  // [waw-decomp] 2026-11-23: A(), this build's address of a USA address (discmap)

REXCVAR_DEFINE_BOOL(cod3_depth_of_field, true, "cod3/Display",
                    "Depth of field: the background blurs while you aim down the sights, where a "
                    "mission or map uses it (the game's own effect; off skips it)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(cod3_motion_blur, true, "cod3/Display",
                    "Motion blur: the screen trails the missions show at some moments (the game's own "
                    "effect; off skips it)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(cod3_test_post_effects, false, "cod3",
                    "Testing, from the command line only: depth of field drawn with fixed distances "
                    "wherever the game draws its pass (the campaign: while aiming) and a steady motion "
                    "blur, so both switches can be seen without a level that uses them");
// [claude-9b-think] 2026-10-27 UNTESTED: the old film look at the same speed at any frame rate (see OldFilmBefore below).
REXCVAR_DEFINE_BOOL(cod3_fix_old_film, true, "cod3/Video",
                    "The old film look (where a level uses it): its fade-in and its flicker step 60 times a "
                    "second at any frame rate, as at 60 fps; off: once per drawn frame, as the game does (faster "
                    "above 60 fps)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(cod3_test_old_film, false, "cod3",
                    "Testing, from the command line only: the old film look switched on in every view, so "
                    "cod3_fix_old_film can be seen without a level that uses it");

namespace {

using cod3::guest::Call;
using cod3::guest::ReadF32;
using cod3::guest::ReadU32;
using cod3::guest::WriteF32;
using cod3::guest::WriteU32;

// The game's depth of field state (written by its setter, the campaign's
// 0x82147098 / cod3mp's 0x8215EEA0, from an effect's settings or a level
// script): on, then the distances the pass turns into depth values (the
// pass reads the first two). The pass itself does nothing while it is off.
constexpr uint32_t kDofOn = COD3_ADDR(0x829128C4, 0x82A8F7D4);
constexpr uint32_t kDofNear = COD3_ADDR(0x825B3070, 0x825C3410);
constexpr uint32_t kDofFar = COD3_ADDR(0x825B3074, 0x825C3414);

// The motion blur amount the end of the scene reads (post effect type 4 runs
// while it isn't 0): one in the campaign, one per local player in cod3mp.
constexpr uint32_t kBlurAmount = COD3_ADDR(0x82A2ABF8, 0x82BCC928);
constexpr int kBlurAmounts = cod3::kMultiplayer ? 4 : 1;

// Direct3D's GetViewport / SetViewport (device, D3DVIEWPORT9*: x, y, width,
// height, min z, max z; 24 bytes), as the motion blur pass calls them.
constexpr uint32_t kGetViewport = COD3_ADDR(0x822E91F0, 0x822B7FC0);
constexpr uint32_t kSetViewport = COD3_ADDR(0x822E8FC8, 0x822B7D98);
constexpr uint32_t kViewportHeight = 12;
// The console's 4:3 video mode, for which the motion blur pass draws a
// 736-line picture as 720 lines (see post_effects.h).
constexpr uint32_t kConsoleWidth = 880;
constexpr uint32_t kConsoleHeight = 736;
constexpr uint32_t kCutHeight = 736;

// cod3_test_post_effects: the distances the test gives the pass (game units)
// and the motion blur amount (Saint-Lo's own).
constexpr float kTestDofNear = 300.0f;
constexpr float kTestDofFar = 1500.0f;
constexpr float kTestBlur = 0.6f;

// cod3_test_post_effects in the campaign: the game draws depth of field only
// while the player is at least 95% aimed down the sights (0x8253D290: the
// player's entity in the list *0x82A2AB40 at [player + 1], its client data at
// +0x210, how far the weapon is up to the sights at +0x8C), which a test run
// can't count on. Otherwise the test draws the pass itself, where the game
// would (before the scene's other post effects).
constexpr uint32_t kDofPass = COD3_ADDR(0x82149C80, 0x82161B28);
constexpr uint32_t kEntityList = COD3_ADDR(0x82A2AB40, 0x82BCC7FC);
constexpr uint32_t kEntityClient = 0x210;
constexpr uint32_t kSightsFraction = 0x8C;
constexpr float kAimedFraction = 0.95f;

// Passes drawn and skipped (the test switch's log line).
std::atomic<uint32_t> g_dof_drawn{0}, g_dof_skipped{0}, g_blur_drawn{0}, g_blur_skipped{0};
std::atomic<uint32_t> g_viewport_fixed{0}, g_dof_by_test{0};
float g_sights_max = -1.0f;  // the render thread's only (the end of the scene)

bool TestMode() {
  // A test switch only counts when given on the command line, never from
  // the settings file (SDK patch 0042 doesn't save it either).
  static const bool on = REXCVAR_GET(cod3_test_post_effects) &&
                         rex::cvar::GetFlagSource("cod3_test_post_effects") == rex::cvar::Source::kCommandLine;
  return on;
}

void LogSetting(const char* what, bool on) {
  REXLOG_INFO("cod3: {} {} ({})", what, on ? "on" : "off",
              on ? "the game's own effect" : "the game's pass is skipped");
}

// Once, at the first frame's post effects: the settings' state in the log,
// and a line whenever one changes (F2, the menus, the console).
std::once_flag g_start_once;

void Start() {
  LogSetting("depth of field", REXCVAR_GET(cod3_depth_of_field));
  LogSetting("motion blur", REXCVAR_GET(cod3_motion_blur));
  if (TestMode())
    REXLOG_INFO("cod3: post effects test: depth of field at {:g}-{:g} units wherever its pass runs, motion "
                "blur {:g} (cod3_test_post_effects)", kTestDofNear, kTestDofFar, kTestBlur);
  rex::cvar::RegisterChangeCallback(cod3::post_effects::kDepthOfField, [](std::string_view, std::string_view) {
    LogSetting("depth of field", REXCVAR_GET(cod3_depth_of_field));
  });
  rex::cvar::RegisterChangeCallback(cod3::post_effects::kMotionBlur, [](std::string_view, std::string_view) {
    LogSetting("motion blur", REXCVAR_GET(cod3_motion_blur));
  });
}

void LogCountsNow() {
  REXLOG_INFO("cod3: post effects test: depth of field pass drawn {} skipped {} (by the test {}; up to the "
              "sights at most {:.2f} in 10 s), motion blur pass drawn {} skipped {}, viewport given back {}",
              g_dof_drawn.load(), g_dof_skipped.load(), g_dof_by_test.load(), g_sights_max, g_blur_drawn.load(),
              g_blur_skipped.load(), g_viewport_fixed.load());
  g_sights_max = -1.0f;
}

// A guest pointer that can be read: not null and not the GPU register window.
bool Readable(uint32_t address) {
  return address >= 0x10000 && (address < 0x7F000000 || address >= 0x80000000) && address < 0xFFFF0000;
}

// How far the campaign player's weapon is up to the sights (0 to 1), as
// 0x8253D290 reads it; -1 while there is no player.
float SightsFraction(uint8_t* base) {
  const uint32_t player = ReadU32(base, cod3::addr::kActiveLocalClient);
  const uint32_t list = ReadU32(base, kEntityList);
  if (player > 3 || !Readable(list)) return -1.0f;
  const uint32_t entity = ReadU32(base, list + 4 * (player + 1));
  if (!Readable(entity)) return -1.0f;
  const uint32_t client = ReadU32(base, entity + kEntityClient);
  if (!Readable(client)) return -1.0f;
  const float f = ReadF32(base, client + kSightsFraction);
  return std::isfinite(f) ? f : -1.0f;
}

void LogCounts() {
  static auto next = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  const auto now = std::chrono::steady_clock::now();
  if (now < next) return;
  next = now + std::chrono::seconds(10);
  LogCountsNow();
}

// A guest buffer for one D3DVIEWPORT9 (the passes run on the game's render
// thread, one after another).
uint32_t ViewportBuffer() {
  static const uint32_t buffer = [] {
    auto* state = rex::system::kernel_state();
    auto* memory = state ? state->memory() : nullptr;
    return memory ? memory->SystemHeapAlloc(32) : 0u;
  }();
  return buffer;
}

// ---------------------------------------------------------------------------
// [claude-9b-think] 2026-10-27 UNTESTED cod3_fix_old_film (FINDINGS-FRAME-CALLS, side finding).
//
// The end of the scene (campaign 0x82426500, cod3mp 0x824311E0) draws the old film look (post effect type 7)
// while its flag is on: campaign 0x82A2AC00; cod3mp 0x82BCC8E8 + 4 per local player, the view's player being the
// scene's r3 + 40. Each call adds 1 to its step (campaign 0x82A2AC04; cod3mp 0x82BCC938 + 4 per player), held at
// 149, and draws with the step's entry of five 150-entry tables (the fade-in) and two random numbers (campaign
// 0x824172D0 called from 0x8242669C / 0x824266B4; cod3mp 0x8242F338 from 0x8243142C / 0x82431444) times the
// step's amplitude: the flicker. While the flag is off the step is set to 0. So the fade-in took 149 drawn
// frames, 2.5 s at 60 fps but 0.5 s at 300, and the flicker changed every drawn frame. (Each cod3mp view has its
// own flag and step: one step per drawn frame per view, not more.)
//
// With cod3_fix_old_film, a frame that is not due a step (60 a second of real time since the look came on, at
// most one a frame, so 60 fps and below stay exactly as the game has it) gets its step lowered by one before the
// game adds it back, and the two random numbers of the last step. The game's first frame of the look always
// steps (to 1, as the game does). The real time between frames stands in for game time as the corona fade does:
// it is what the console did at 60 fps, paused or not. The addresses are each program's own, in
// `#if defined(COD3_MP)` blocks as in coronas.cpp (tools/mp_aliases.py leaves those to the file).
#if defined(COD3_MP)
constexpr uint32_t kFilmOn = 0x82BCC8E8;
constexpr uint32_t kFilmStep = 0x82BCC938;
// [waw-decomp] 2026-11-23: A() gives the build's own address (tools/discmap); USA: the same.
constexpr uint32_t kFilmRandomReturn[2] = {A(0x82431430), A(0x82431448)};
#else
constexpr uint32_t kFilmOn = 0x82A2AC00;
constexpr uint32_t kFilmStep = 0x82A2AC04;
constexpr uint32_t kFilmRandomReturn[2] = {0x824266A0, 0x824266B8};
#endif
constexpr uint32_t kSceneViewPlayer = 40;  // cod3mp: the end of the scene's r3 + 40
constexpr int kFilmViews = cod3::kMultiplayer ? 4 : 1;
constexpr int32_t kFilmLastStep = 149;
constexpr double kFilmStepSeconds = 1.0 / 60.0;
constexpr double kFilmMaxSteps = 6.0;  // [rexglue-native thread2] 2026-11-24: the most steps one frame takes (10 fps)

struct FilmView {
  bool running = false;                            // the look was on at this view's last frame
  std::chrono::steady_clock::time_point started{};  // its first frame (the log line)
  std::chrono::steady_clock::time_point last{};     // this view's last frame
  double owed = 0.0;                                // real time not stepped yet (under one step)
  float random[2] = {0.0f, 0.0f};                   // the last step's two random numbers
  bool have_random = false;
  bool logged_end = false;
};
FilmView g_film[4];  // the render thread's only (the end of the scene)

// Around one end of the scene on this thread: the view whose random numbers are given back (a frame not due a
// step) or recorded (a step).
thread_local FilmView* t_film_hold = nullptr;
thread_local FilmView* t_film_record = nullptr;

bool TestOldFilm() {
  static const bool on = REXCVAR_GET(cod3_test_old_film) &&
                         rex::cvar::GetFlagSource("cod3_test_old_film") == rex::cvar::Source::kCommandLine;
  return on;
}

// Before the end of the scene: whether this frame steps the view's old film look (sets t_film_*).
void OldFilmBefore(PPCContext& ctx, uint8_t* base) {
  t_film_hold = t_film_record = nullptr;
  int view = 0;
  if (cod3::kMultiplayer) {
    const uint32_t scene = ctx.r3.u32;
    view = Readable(scene) ? static_cast<int>(ReadU32(base, scene + kSceneViewPlayer)) : 0;
    if (view < 0 || view >= kFilmViews) return;
  }
  const uint32_t on_at = kFilmOn + 4u * uint32_t(view);
  const uint32_t step_at = kFilmStep + 4u * uint32_t(view);
  if (TestOldFilm()) WriteU32(base, on_at, 1);
  FilmView& f = g_film[view];
  if (!ReadU32(base, on_at)) {
    f.running = false;  // the game sets the step to 0 itself
    return;
  }
  const auto now = std::chrono::steady_clock::now();
  const int32_t step = cod3::guest::ReadS32(base, step_at);
  const bool fix = REXCVAR_GET(cod3_fix_old_film);
  if (!f.running || step <= 0) {
    // The look's first frame: the game's own step (0 -> 1).
    f = FilmView{};
    f.running = true;
    f.started = f.last = now;
    REXLOG_INFO("cod3: old film look on (view {}): its fade-in steps {}", view + 1,
                fix ? "60 times a second (cod3_fix_old_film)" : "once per drawn frame (as the game)");
    t_film_record = &f;
    return;
  }
  if (step >= kFilmLastStep && !f.logged_end) {
    // The last frame reached the last step: how long the fade-in took (about 2.48 s at 60 fps; without the
    // fix 148 drawn frames: 0.49 s at 300 fps).
    f.logged_end = true;
    REXLOG_INFO("cod3: old film look (view {}): fade-in done in {:.2f} s", view + 1,
                std::chrono::duration<double>(now - f.started).count());
  }
  const double dt = std::chrono::duration<double>(now - f.last).count();
  f.last = now;
  if (!fix) {
    f.owed = 0.0;
    return;
  }
  // [rexglue-native thread2] 2026-11-24 rule 14 (FPS-RULE14.md A3): the frame's whole time is owed (it was at most one
  // step a frame, so below 60 fps the fade-in took 60/fps as long: 3x at 20); a frame due several steps (at most 6:
  // 10 fps) adds the extra ones before the game's own +1.
  f.owed += std::min(dt, kFilmMaxSteps * kFilmStepSeconds);
  if (f.owed >= kFilmStepSeconds - 1e-9) {
    const int steps = static_cast<int>((f.owed + 1e-9) / kFilmStepSeconds);
    f.owed = std::max(0.0, f.owed - steps * kFilmStepSeconds);
    if (steps > 1 && step < kFilmLastStep)
      WriteU32(base, step_at, static_cast<uint32_t>(std::min(step + steps - 1, kFilmLastStep - 1)));
    t_film_record = &f;
    return;
  }
  // Not due a step: the game's +1 gives this frame's step back, and the last step's random numbers.
  WriteU32(base, step_at, static_cast<uint32_t>(step - 1));
  if (f.have_random) t_film_hold = &f;
}

// Clears the thread's marks when the end of the scene returns.
struct OldFilmScope {
  ~OldFilmScope() { t_film_hold = t_film_record = nullptr; }
};

}  // namespace

// [claude-9b-think] UNTESTED: the old film look's random numbers (see OldFilmBefore): given back on a frame not due
// a step, recorded on a step. The function (rand() scaled to a fixed range) is called from elsewhere too; only the
// two calls of the end of the scene, by their return address, are touched.
#if defined(COD3_MP)
REX_EXTERN(__imp__sub_8242F338);
REX_HOOK_RAW(sub_8242F338) {
#else
REX_EXTERN(__imp__sub_824172D0);
REX_HOOK_RAW(sub_824172D0) {
#endif
  const uint32_t lr = static_cast<uint32_t>(ctx.lr);
  const int which = lr == kFilmRandomReturn[0] ? 0 : lr == kFilmRandomReturn[1] ? 1 : -1;
  if (which >= 0 && t_film_hold) {
    ctx.f1.f64 = double(t_film_hold->random[which]);
    return;
  }
#if defined(COD3_MP)
  __imp__sub_8242F338(ctx, base);
#else
  __imp__sub_824172D0(ctx, base);
#endif
  if (which >= 0 && t_film_record) {
    t_film_record->random[which] = static_cast<float>(ctx.f1.f64);
    if (which == 1) t_film_record->have_random = true;
  }
}

namespace cod3::post_effects {

bool DepthOfFieldOn() { return REXCVAR_GET(cod3_depth_of_field); }
bool MotionBlurOn() { return REXCVAR_GET(cod3_motion_blur); }

}  // namespace cod3::post_effects

// The depth of field pass (campaign: from the end of the scene, 0x82426500,
// while 0x8253D290 says the player is at least 95% aimed down the sights;
// cod3mp: from 0x824311E0 while fewer than 2 players play on this PC).
// Skipping it gives the frame the game draws whenever the pass doesn't run
// (in the campaign: while you aren't aiming); the game's own depth of field
// state stays as it is, so whatever else it holds back while depth of field
// is on (the glow pass) is held back as in that frame.
REX_EXTERN(__imp__sub_82149C80);
REX_HOOK_RAW(sub_82149C80) {
  if (!REXCVAR_GET(cod3_depth_of_field)) {
    g_dof_skipped.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  if (!TestMode()) {
    if (ReadU32(base, kDofOn)) g_dof_drawn.fetch_add(1, std::memory_order_relaxed);
    __imp__sub_82149C80(ctx, base);
    return;
  }
  // The test: drawn with fixed distances; the game's values back afterwards.
  const uint32_t on = ReadU32(base, kDofOn);
  const float near_d = ReadF32(base, kDofNear), far_d = ReadF32(base, kDofFar);
  WriteU32(base, kDofOn, 1);
  WriteF32(base, kDofNear, kTestDofNear);
  WriteF32(base, kDofFar, kTestDofFar);
  g_dof_drawn.fetch_add(1, std::memory_order_relaxed);
  __imp__sub_82149C80(ctx, base);
  // Only what still holds the test's values: a change the game made meanwhile
  // (its setter, from an effect or a level script) stays.
  if (ReadU32(base, kDofOn) == 1) WriteU32(base, kDofOn, on);
  if (ReadF32(base, kDofNear) == kTestDofNear) WriteF32(base, kDofNear, near_d);
  if (ReadF32(base, kDofFar) == kTestDofFar) WriteF32(base, kDofFar, far_d);
}

// The motion blur pass, post effect type 4 of the dispatcher (0x8214B0B0;
// cod3mp 0x82162F58), which keeps the depth-stencil surface around it but not
// the viewport (so the pass's 720-line cut stays). r3 is the history (a
// quarter-size render target record), r4 the shader, r5/r6 its constants.
// Skipping it leaves the frame as drawn.
REX_EXTERN(__imp__sub_82149950);
REX_HOOK_RAW(sub_82149950) {
  if (!REXCVAR_GET(cod3_motion_blur)) {
    g_blur_skipped.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  g_blur_drawn.fetch_add(1, std::memory_order_relaxed);
  // The 736-line case (post_effects.h): the viewport as it was, to give back
  // when the pass cuts it to 720 lines outside the console's own 4:3 mode.
  const uint32_t device = ReadU32(base, cod3::addr::live_video::kDevice);
  const uint32_t viewport = ViewportBuffer();
  bool give_back = false;
  if (device && viewport) {
    const uint32_t mode_w = ReadU32(base, cod3::addr::video::kVideoMode);
    const uint32_t mode_h = ReadU32(base, cod3::addr::video::kVideoMode + 4);
    if (mode_w != kConsoleWidth || mode_h != kConsoleHeight) {
      Call c(ctx, base);
      c.Invoke(kGetViewport, {device, viewport});
      give_back = ReadU32(base, viewport + kViewportHeight) == kCutHeight;
    }
  }
  __imp__sub_82149950(ctx, base);
  if (give_back) {
    Call c(ctx, base);
    c.Invoke(kSetViewport, {device, viewport});
    if (g_viewport_fixed.fetch_add(1, std::memory_order_relaxed) == 0)
      REXLOG_INFO("cod3: motion blur: the viewport's 736 lines given back after the pass (the game cuts them "
                  "to 720 for the console's 880x736 mode; this picture is {}x{})",
                  ReadU32(base, cod3::addr::video::kVideoMode), ReadU32(base, cod3::addr::video::kVideoMode + 4));
  }
}

// [rexglue-native thread2] 2026-11-24 rule 14 (FPS-RULE14.md A5): the motion blur (post effect 4, the afterimage of
// a hit or a close shell burst) blends once per drawn frame: kept = s x kept + (1 - s) x picture, the kept picture read
// d in from every edge, s = the amount (0x82A2ABF8) and d the second word (0x825CDAB0), both set by the game in real
// time (0x824167D8, its fade, adds the frame's seconds). s is the history's weight: the dispatcher (0x8214B0B0, type 4)
// makes c1 = s and c0 = 1 - s, and the shader draws c0 x texture 0 (the screen copy) + c1 x texture 1 (the effect's kept
// picture) (ngl_side.cpp rn47g, from the shader's microcode), so s^(60 dt) (not 1 - (1 - s)^(60 dt)). So a trail's half-life was ln 0.5 / ln s / fps: 60/fps times
// the console's 60 fps one (3x longer at 20, 0.42x at 144), and the picture streamed outward fps/60 as fast. Here, for
// the end of the scene's call (it queues the effect with both words: both renderers draw from them), the per-frame
// weight is s^(60 dt) and the inset d x 60 dt, dt the real time since this view's last end of the scene (at most
// 0.1 s): every rate has the 60 fps half-life and spread. Off (cod3_fix_motion_blur_rate false): as the game. cod3mp:
// one amount per view (0x82BCC928 + 4 x view, the scene's view at r3 + 40) and one second word for all (0x825D68F4,
// its end of the scene 0x824311E0 queues lfs 26868(0x825D)), each view with its own clock.
REXCVAR_DEFINE_BOOL(cod3_fix_motion_blur_rate, true, "cod3/Video",
                    "The motion blur's trails (a hit, a close shell burst) last the same time at any frame rate, as "
                    "at the console's 60; off: per drawn frame, as the game (shorter above 60, longer below)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(cod3_debug_post_effects, false, "cod3",
                    "Log every 2 s how many times a second each view ended its scene (the motion blur's per-view "
                    "clock; a test of the frame-rate rule)");
namespace {
#if defined(COD3_MP)
constexpr uint32_t kBlurInset = 0x825D68F4;  // the effect's second word (d), one for every view
#else
constexpr uint32_t kBlurInset = 0x825CDAB0;  // the effect's second word (d), as the end of the scene queues it
#endif
struct BlurRate {
  bool scaled = false;
  uint32_t amount_at = 0;
  float amount = 0.0f, inset = 0.0f, amount_set = 0.0f, inset_set = 0.0f;
};
std::chrono::steady_clock::time_point g_blur_last[4]{};  // each view's last end of the scene (the render thread's)
BlurRate ScaleBlur(PPCContext& ctx, uint8_t* base) {
  BlurRate b;
  int view = 0;
  if (cod3::kMultiplayer) {
    const uint32_t scene = ctx.r3.u32;
    view = Readable(scene) ? static_cast<int>(ReadU32(base, scene + kSceneViewPlayer)) : 0;
    if (view < 0 || view >= kBlurAmounts) return b;
  }
  const auto now = std::chrono::steady_clock::now();
  auto& last = g_blur_last[view];
  const double dt = last.time_since_epoch().count() == 0
                        ? 1.0 / 60.0
                        : std::clamp(std::chrono::duration<double>(now - last).count(), 0.0, 0.1);
  last = now;
  // cod3_debug_post_effects: each view's ends of the scene a second (both split-screen views end every drawn frame)
  if (REXCVAR_GET(cod3_debug_post_effects)) {
    static std::chrono::steady_clock::time_point since[4]{};
    static int count[4]{};
    ++count[view];
    if (since[view].time_since_epoch().count() == 0) since[view] = now;
    const double span = std::chrono::duration<double>(now - since[view]).count();
    if (span >= 2.0) {
      REXLOG_INFO("cod3: motion blur: view {} ended its scene {:.1f} times a second (cod3_debug_post_effects)",
                  view + 1, count[view] / span);
      count[view] = 0;
      since[view] = now;
    }
  }
  if (!REXCVAR_GET(cod3_fix_motion_blur_rate)) return b;
  b.amount_at = kBlurAmount + 4u * uint32_t(view);
  b.amount = ReadF32(base, b.amount_at);
  if (!(b.amount > 0.0f && b.amount < 1.0f)) return b;
  b.inset = ReadF32(base, kBlurInset);
  const double rate = 60.0 * dt;
  b.amount_set = static_cast<float>(std::pow(double(b.amount), rate));
  // the kept picture grows by 1 / (1 - 2d) each frame (drawn into itself with its corners d in): rate frames' worth of
  // that growth is 1 / (1 - 2d)^rate, i.e. an inset of (1 - (1 - 2d)^rate) / 2 (d x rate to first order)
  b.inset_set = std::isfinite(b.inset) && b.inset > 0.0f && b.inset < 0.5f
                    ? static_cast<float>(0.5 * (1.0 - std::pow(1.0 - 2.0 * double(b.inset), rate)))
                    : b.inset;
  WriteF32(base, b.amount_at, b.amount_set);
  WriteF32(base, kBlurInset, b.inset_set);
  b.scaled = true;
  static std::atomic<int> said[4]{};  // the first 3 of each view
  if (said[view].fetch_add(1, std::memory_order_relaxed) < 3)
    REXLOG_INFO("cod3: motion blur: {:.3f} kept a frame at {:.0f} fps (the game's {:.3f} at 60), inset {:.4f} "
                "({:.4f}), view {} (cod3_fix_motion_blur_rate)",
                b.amount_set, dt > 0.0 ? 1.0 / dt : 0.0, b.amount, b.inset_set, b.inset, view + 1);
  return b;
}
// (the inset is one word for every view in cod3mp: each view's scaling writes it for its own call and puts it back)
void RestoreBlur(uint8_t* base, const BlurRate& b) {
  if (!b.scaled) return;
  // put back only what the end of the scene left as it was set (it doesn't write them; the game's fade might)
  if (ReadF32(base, b.amount_at) == b.amount_set) WriteF32(base, b.amount_at, b.amount);
  if (ReadF32(base, kBlurInset) == b.inset_set) WriteF32(base, kBlurInset, b.inset);
}
}  // namespace

// The end of the scene's post effects (tint, color curves, motion blur, the
// death fade, old film, and depth of field before them; cod3mp 0x824311E0),
// a layer callback of the scene (RE_RenderScene). Here for the settings' log
// lines and the test switch: a steady motion blur while no mission's runs.
REX_EXTERN(__imp__sub_82426500);
REX_HOOK_RAW(sub_82426500) {
  std::call_once(g_start_once, Start);
  OldFilmScope film_scope;     // [claude-9b-think] cod3_fix_old_film
  OldFilmBefore(ctx, base);
  if (!TestMode()) {
    const BlurRate blur = ScaleBlur(ctx, base);  // [rexglue-native thread2] rule 14 A5
    __imp__sub_82426500(ctx, base);
    RestoreBlur(base, blur);
    return;
  }
  std::array<float, kBlurAmounts> saved{};
  for (int i = 0; i < kBlurAmounts; ++i) {
    const uint32_t at = kBlurAmount + 4u * uint32_t(i);
    saved[i] = ReadF32(base, at);
    if (saved[i] == 0.0f) WriteF32(base, at, kTestBlur);
  }
  if (!cod3::kMultiplayer) {
    // Depth of field where the game wouldn't draw it this frame (cod3mp's
    // end of the scene draws it without an aiming test).
    const float sights = SightsFraction(base);
    if (sights > g_sights_max) g_sights_max = sights;
    if (sights >= 0.0f && sights < kAimedFraction && REXCVAR_GET(cod3_depth_of_field)) {
      Call c(ctx, base);
      c.Invoke(kDofPass);
      g_dof_by_test.fetch_add(1, std::memory_order_relaxed);
    }
  }
  const BlurRate blur = ScaleBlur(ctx, base);  // [rexglue-native thread2] rule 14 A5 (after the test's amount)
  __imp__sub_82426500(ctx, base);
  RestoreBlur(base, blur);
  for (int i = 0; i < kBlurAmounts; ++i) {
    const uint32_t at = kBlurAmount + 4u * uint32_t(i);
    if (saved[i] == 0.0f && ReadF32(base, at) == kTestBlur) WriteF32(base, at, 0.0f);
  }
  LogCounts();
}
