// cod3 - depth of field and motion blur on or off (see post_effects.h).
// Shared by both programs: the hooks name the campaign's functions, and
// cod3mp binds them to its own (mp/config/campaign_map.tsv); the guest
// addresses go through COD3_ADDR or addresses.h.

#include "game/post_effects.h"

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
#include <rex/system/kernel_state.h>

#include "game/addresses.h"
#include "game/guest.h"
#include "game/program.h"

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

}  // namespace

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

// The end of the scene's post effects (tint, color curves, motion blur, the
// death fade, old film, and depth of field before them; cod3mp 0x824311E0),
// a layer callback of the scene (RE_RenderScene). Here for the settings' log
// lines and the test switch: a steady motion blur while no mission's runs.
REX_EXTERN(__imp__sub_82426500);
REX_HOOK_RAW(sub_82426500) {
  std::call_once(g_start_once, Start);
  if (!TestMode()) {
    __imp__sub_82426500(ctx, base);
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
  __imp__sub_82426500(ctx, base);
  for (int i = 0; i < kBlurAmounts; ++i) {
    const uint32_t at = kBlurAmount + 4u * uint32_t(i);
    if (saved[i] == 0.0f && ReadF32(base, at) == kTestBlur) WriteF32(base, at, 0.0f);
  }
  LogCounts();
}
