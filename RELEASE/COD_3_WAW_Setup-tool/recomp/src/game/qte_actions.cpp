// cod3 - stick battle actions (QTEs) with any input (see qte_actions.h).
//
// Game side. Where things are in default.xex (all found by disassembly; the
// numbers are the game's):
//   * One InteractInputRcvr per input type (a singleton each, table
//     0x829E9278): +0 vtable, +4 inputType, +12 flags, +16 the action's
//     definition, +20 time since Start, +24 when input is taken
//     (acceptInputMinTime), +28 the output ("rate"), +32 the rowboat phase,
//     +36 time since the last flip; toggles keep the last side at +92, the
//     swirl its smoothed turning at +100.
//   * The definitions (INTERACTSTATEFILE records, 1592 bytes): +0 name,
//     +0x20 interactType, +0x24 inputType, +0xCC successOnInput, +0x380 /
//     +0x384 acceptInputMinTime / MaxTime, +0x388 inputRateFactor, +0x3FC
//     buttonHelpStr, +0x440 swirlClockwise, +0x444 leftStick, +0x44C
//     difficulty, +0x49C animSpeedInputRateFactor, +0x4A4 animSpeedMax.
//   * The running InteractState of a local client: [0x82A59DA0 + 4 * client]
//     + 4; its +28 is the definition, +32 the receiver; a LeverPush keeps its
//     score at +432.
//   * The stick the receivers read: 0x829C9140 + 72 * client, ints on the
//     game's scale after the dead zone: [0] RX, [1] -RY, [2] LX, [3] -LY.

#include "game/qte_actions.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <string_view>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>

#include "game/addresses.h"
#include "game/guest.h"
#include "game/qte_math.h"

REXCVAR_DEFINE_STRING(cod3_qte_style, "spinner", "cod3/Battle actions",
                      "Stick battle actions (rotate, toggle, row): spinner (a guide circle for "
                      "the mouse or the stick) or original (the game's own arrow). The mouse "
                      "works in both")
    .allowed({"spinner", "original"});
REXCVAR_DEFINE_BOOL(cod3_qte_spinner_with_pad, true, "cod3/Battle actions",
                    "Show the spinner with a controller too (off: the game's arrow with a "
                    "controller, the spinner with the mouse)");
REXCVAR_DEFINE_DOUBLE(cod3_qte_spinner_scale, 1.0, "cod3/Battle actions", "Size of the spinner")
    .range(0.5, 2.0);
REXCVAR_DEFINE_BOOL(cod3_qte_fps_fix, true, "cod3/Battle actions",
                    "Stick swirls (valves, cranes) progress as fast as on the console above 60 "
                    "frames a second (the game's own math slows them down)");
REXCVAR_DEFINE_STRING(cod3_qte_demo, "off", "cod3/Battle actions",
                      "Test: fake a stick battle action anywhere (not saved): off, swirl_cw, "
                      "swirl_ccw, toggle_h, toggle_v, row")
    .allowed({"off", "swirl_cw", "swirl_ccw", "toggle_h", "toggle_v", "row"})
    .debug_only();

REXCVAR_DECLARE(bool, mnk_mode);
REXCVAR_DECLARE(bool, cod3_key_prompts);
REXCVAR_DECLARE(double, cod3_sensitivity);
REXCVAR_DECLARE(double, cod3_m_yaw);

namespace cod3::qte {
namespace {

namespace m = cod3::qte::math;
using guest::ReadF32;
using guest::ReadS32;
using guest::ReadU16;
using guest::ReadU32;
using guest::WriteF32;
using guest::WriteU16;
using guest::WriteU32;

// ---------------------------------------------------------------------------
// The game's addresses (default.xex)
// ---------------------------------------------------------------------------
constexpr uint32_t kActiveClient = addr::kActiveLocalClient;          // 0x82A2A1C8
constexpr uint32_t kInteractions = addr::interact::kPerClient;       // Interaction*[4]
constexpr uint32_t kInteractionState = addr::interact::kState;       // +4: InteractState*
constexpr uint32_t kStickAxes = 0x829C9140;      // int[18] a client, game scale
constexpr uint32_t kStickAxesStride = 72;
constexpr uint32_t kPadManager = 0x8290A910;     // +24 the pad the game reads, +28 set
constexpr uint32_t kGameMYaw = 0x829C3E6C;       // dvar_t* m_yaw (value at +28)
constexpr uint32_t kPlayerEntities = 0x82A2AB40; // gentity_t** (a client's at +4 * (client + 1))
constexpr uint32_t kSwirlNotifyName = 0x82A51084;  // the name the swirl step notifies
constexpr uint32_t kNotify = 0x824927C0;           // (entity, name)
// The stick arrow is [[g_FEManager 0x82A8DF2C] + 20] + 76; its Show passes it.
constexpr uint32_t kArrowShow = 0x824C0EB0;        // (arrow, type, 1, f1); hooked
constexpr uint32_t kArrowHide = 0x824C0E08;        // (arrow)
constexpr uint32_t kArrowShownType = 48;           // the type it shows, 0 = none
// The swirl turret (sub_8250F648, called by CL_MouseMove for turrets of
// type 5): stick 0 turns the yaw (cl + 5736), stick 1 the pitch (cl + 5732).
constexpr uint32_t kClientActive = addr::kClientActive;               // 0x82A57900
constexpr uint32_t kClientActiveStride = addr::kClientActiveStride;   // 7280
constexpr uint32_t kTurretYaw = addr::kViewAnglesOffset + 4;          // 5736
constexpr uint32_t kTurretFactors = 0x829C9194;  // float[2], set by the turret code
constexpr float kTurretStepLimit = 0.08f;        // its clamp (0x825D19EC)
constexpr uint32_t kFrameMsec = 0x829C41E4;      // this frame's msec (int)

// Receiver fields
constexpr uint32_t kRcvrType = 4;
constexpr uint32_t kRcvrFlags = 12;  // (+16: the definition)
constexpr uint32_t kRcvrElapsed = 20;
constexpr uint32_t kRcvrAcceptAfter = 24;
constexpr uint32_t kRcvrRate = 28;
constexpr uint32_t kRcvrPhase = 32;
constexpr uint32_t kToggleLastSide = 92;
constexpr uint32_t kFlagSlipped = 4;
constexpr uint32_t kFlagStrokeDone = 8;
constexpr uint32_t kFlagSwirlNotified = 0x10;
constexpr float kSwirlNotifyLevel = 3.0f;
// Vtables by inputType: ButtonMash, ButtonPress, StickSwirl,
// StickToggleHoriz, StickToggleVert, Rowboat.
constexpr uint32_t kReceiverVtables[6] = {0x8206C9B8, 0x8206C9D4, 0x8206C9F0,
                                          0x8206CA28, 0x8206CA0C, 0x8206CA44};
constexpr const char* kInputTypeNames[6] = {"ButtonMash",       "ButtonPress",     "StickSwirl",
                                            "StickToggleHoriz", "StickToggleVert", "Rowboat"};
// interactType names (the game's table at 0x825D1190).
constexpr const char* kInteractTypeNames[] = {
    "PlayAnims",          "PlayPlayerAnim",     "PlaceItem",        "Push",
    "StrengthTest",       "ScaleAnimSpeed",     "LeverPush",        "Melee",
    "MeleeInitiate",      "MeleeStart",         "MeleeSuccessSetup", "MeleeSuccess",
    "MeleeFailure",       "MeleeStagedInitiate", "MeleeStagedSuccess", "MeleeDropWeapon",
    "MortarIdle",         "MortarStart",        "MortarLoad",       "VehicleBase",
    "VehicleIdle",        "VehicleTurn",        "VehicleRelease",   "VehicleLink",
    "PickLiveGrenade",    "RowboatInit",        "Rowboat"};
constexpr int kScaleAnimSpeed = 5;
constexpr int kLeverPush = 6;

// Definition fields
constexpr uint32_t kDefInteractType = 0x20;
constexpr uint32_t kDefInputType = 0x24;
constexpr uint32_t kDefSuccessOnInput = 0xCC;
constexpr uint32_t kDefAcceptMin = 0x380;
constexpr uint32_t kDefAcceptMax = 0x384;
constexpr uint32_t kDefRateFactor = 0x388;
constexpr uint32_t kDefHelp = 0x3FC;
constexpr uint32_t kDefClockwise = 0x440;
constexpr uint32_t kDefLeftStick = 0x444;
constexpr uint32_t kDefDifficulty = 0x44C;
constexpr uint32_t kDefAnimFactor = 0x49C;
constexpr uint32_t kDefAnimMax = 0x4A4;
// InteractState fields
constexpr uint32_t kStateReceiver = 32;
constexpr uint32_t kLeverScore = 432;

bool Valid(uint32_t p) { return p >= 0x10000 && p < 0xC0000000u && (p & 3) == 0; }
bool ValidText(uint32_t p) { return p >= 0x10000 && p < 0xC0000000u; }

int64_t NowNs() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}
constexpr int64_t kMs = 1'000'000;

float Finite(float v, float fallback = 0.0f) { return std::isfinite(v) ? v : fallback; }

// ---------------------------------------------------------------------------
// Settings (read on the game thread at most four times a second; the string
// settings are copied then, never every frame)
// ---------------------------------------------------------------------------
std::atomic<Style> g_style{Style::kSpinner};
std::atomic<bool> g_spinner_with_pad{true};
std::atomic<bool> g_fps_fix{true};
std::atomic<float> g_counts_per_radius{220.0f};
// The mouse distance that moves the cursor one spinner radius turns the view
// this far (degrees): the spinner feels the same at any mouse speed setting
// (about 220 counts at the default sensitivity, 1 px a count at 1080p).
constexpr double kCursorDegreesPerRadius = 24.0;

Kind DemoKindFor(std::string_view v, bool* clockwise) {
  *clockwise = v == "swirl_cw";
  if (v == "swirl_cw" || v == "swirl_ccw") return Kind::kSwirl;
  if (v == "toggle_h") return Kind::kToggleHorizontal;
  if (v == "toggle_v") return Kind::kToggleVertical;
  if (v == "row") return Kind::kRow;
  return Kind::kNone;
}

// ---------------------------------------------------------------------------
// Which input drives: the one that moved last
// ---------------------------------------------------------------------------
std::atomic<int64_t> g_mouse_moved_ns{0};
std::atomic<int64_t> g_pad_moved_ns{0};
// The real stick the action reads (before the mouse is written), -1..1, y up.
std::atomic<float> g_pad_x{0.0f}, g_pad_y{0.0f};

Device Driving() {
  const int64_t mouse = g_mouse_moved_ns.load(std::memory_order_relaxed);
  const int64_t pad = g_pad_moved_ns.load(std::memory_order_relaxed);
  if (mouse == 0 && pad == 0) return REXCVAR_GET(mnk_mode) ? Device::kMouse : Device::kPad;
  return mouse > pad ? Device::kMouse : Device::kPad;
}

// ---------------------------------------------------------------------------
// The action
// ---------------------------------------------------------------------------
struct Definition {
  char name[32] = {};
  char help[64] = {};
  int interact_type = -1;
  int input_type = -1;
  bool success_on_input = false;
  bool clockwise = false;
  bool left_stick = false;
  float accept_min = 0.0f;
  float accept_max = 0.0f;
  float rate_factor = 1.0f;
  float difficulty = 0.0f;
  float anim_factor = 0.0f;
  float anim_max = 0.0f;
};

struct Action {
  uint32_t receiver = 0;  // 0 for the demo
  Definition def;
  Kind kind = Kind::kNone;
  bool live = false;     // between Start and Stop
  bool demo = false;
  uint32_t serial = 0;
  int64_t last_update_ns = 0;
  float update_interval = 0.016f;  // wall seconds between two updates (smoothed)
  // What the game measured this frame.
  float rate = 0.0f;
  float phase = 0.0f;
  float score = 0.0f;
  int last_side = 0;
  bool have_side = false;
  bool slipped = false;
  bool stroke_done = false;
  float stick_x = 0.0f, stick_y_up = 0.0f;
  bool ready = false;
  float guide_speed = 0.0f;
  // The judge.
  Verdict verdict = Verdict::kNeutral;
  const char* reason = "";
  float quality = 0.0f;
  bool started = false;      // progress seen in this attempt
  float since_progress = 0.0f;
  float wrong_way_time = 0.0f;
  float turn_speed = 0.0f;   // radians a second, positive = the way to go
  float last_angle = 0.0f;
  bool have_angle = false;
  float since_flip = 0.0f;
  float last_phase = 0.0f;
  float since_rise = 10.0f;
  float good_hold = 0.0f;
  float last_score = 0.0f;
  float score_trend = 0.0f;  // a second
  uint32_t events = 0;
  Verdict logged_verdict = Verdict::kNeutral;
  Device logged_device = Device::kPad;
  bool logged_device_once = false;
};

std::mutex g_mutex;  // guards g_action, g_serial
Action g_action;
uint32_t g_serial = 0;

// The virtual cursor (UI thread writes, the game thread reads), in spinner
// radii, y down.
std::mutex g_cursor_mutex;
float g_cursor_x = 0.0f, g_cursor_y = 0.0f;
std::atomic<bool> g_cursor_on{false};
std::atomic<bool> g_demo_on{false};
std::atomic<Kind> g_cursor_kind{Kind::kNone};
std::atomic<int64_t> g_captured_motion_ns{0};
std::atomic<float> g_drawn_radius_px{200.0f};

bool Alive(const Action& a, int64_t now) {
  if (a.demo) return a.kind != Kind::kNone;
  if (!a.live || a.kind == Kind::kNone) return false;
  // A paused game stops updating its actions: the spinner goes away and the
  // mouse goes back to the view until the action runs again. (Stop ends an
  // action at once; this only covers updates that stop without it.)
  const int64_t timeout = std::max<int64_t>(400 * kMs, int64_t(3.0f * a.update_interval * 1e9f));
  return now - a.last_update_ns < timeout;
}

void SetCursor(float x, float y) {
  const float length = std::sqrt(x * x + y * y);
  if (length > m::kCursorLimit) {
    x *= m::kCursorLimit / length;
    y *= m::kCursorLimit / length;
  }
  std::lock_guard lock(g_cursor_mutex);
  g_cursor_x = Finite(x);
  g_cursor_y = Finite(y);
}

// When the mouse takes over, the cursor starts where the stick was, so the
// action carries on from there.
void SeedCursorFromStick() {
  const float x = g_pad_x.load(std::memory_order_relaxed);
  const float y_up = g_pad_y.load(std::memory_order_relaxed);
  switch (g_cursor_kind.load(std::memory_order_relaxed)) {
    case Kind::kToggleHorizontal:
      SetCursor(x, 0.0f);
      break;
    case Kind::kToggleVertical:
      SetCursor(0.0f, -y_up);
      break;
    case Kind::kSwirl:
    case Kind::kRow: {
      const float length = std::sqrt(x * x + y_up * y_up);
      if (length > 0.3f) SetCursor(x / length, -y_up / length);
      break;
    }
    case Kind::kNone:
      break;
  }
}

// UI thread: mouse motion counts as the mouse being used once it adds up to
// a few counts within 0.1 s (a resting mouse or a bump doesn't).
void NoteMouseMotion(float dx, float dy) {
  static int64_t window_start = 0;
  static float window_counts = 0.0f;
  const int64_t now = NowNs();
  if (now - window_start > 100 * kMs) {
    window_start = now;
    window_counts = 0.0f;
  }
  window_counts += std::fabs(dx) + std::fabs(dy);
  if (window_counts < 4.0f) return;
  window_counts = 0.0f;
  if (g_cursor_on.load(std::memory_order_relaxed) && Driving() == Device::kPad) SeedCursorFromStick();
  g_mouse_moved_ns.store(now, std::memory_order_relaxed);
}

void MoveCursor(float dx, float dy) {
  float x, y;
  {
    std::lock_guard lock(g_cursor_mutex);
    x = g_cursor_x + dx;
    y = g_cursor_y + dy;
  }
  SetCursor(x, y);
}

// ---------------------------------------------------------------------------
// The judge: neutral, green or red, from what the game measured
// ---------------------------------------------------------------------------
constexpr float kGoodQuality = 0.5f;       // green from half the action's full speed
constexpr float kKeepGoodQuality = 0.35f;  // and stays green down to this
constexpr float kMinTurn = 1.5f;           // radians a second that count as turning
constexpr float kWrongWayTime = 0.3f;
constexpr float kStoppedTime = 0.4f;
constexpr float kToggleIdleTime = 0.5f;
constexpr float kRowGoodHold = 0.6f;
constexpr float kLeverLosing = 0.1f;
constexpr float kLeverWinning = 0.9f;

void Set(Action& a, Verdict v, const char* reason = "") {
  a.verdict = v;
  a.reason = reason;
}

// A new attempt after a failure: progress has to be seen again.
void NextAttempt(Action& a) {
  Set(a, Verdict::kNeutral);
  a.started = false;
  a.since_progress = 0.0f;
  a.wrong_way_time = 0.0f;
}

// How well the action goes, 0..1, the way its kind of action measures it.
float Quality(const Action& a) {
  if (a.def.interact_type == kLeverPush) return std::clamp(a.score, 0.0f, 1.0f);
  if (a.kind == Kind::kRow)
    return a.phase >= m::kRowStrokeStart ? (a.phase - m::kRowStrokeStart) / (1.0f - m::kRowStrokeStart) : 0.0f;
  if (a.def.interact_type == kScaleAnimSpeed && a.def.anim_factor > 0.0f && a.def.anim_max > 0.0f)
    return std::clamp(a.def.anim_factor * a.rate / a.def.anim_max, 0.0f, 1.0f);
  // Other actions finish on any input (successOnInput) or use the rate as is.
  const float full = a.kind == Kind::kSwirl ? kSwirlNotifyLevel : m::kToggleGuideFlips * a.def.rate_factor;
  return full > 0.0f ? std::clamp(a.rate / full, 0.0f, 1.0f) : 0.0f;
}

// LeverPush (a tug of war): green while the score climbs or is nearly won,
// red while it is nearly lost.
bool LeverGood(const Action& a) { return a.score >= kLeverWinning || a.score_trend > 0.02f; }
bool LeverLost(const Action& a) { return a.score < kLeverLosing; }

void TrackTurning(Action& a, float dt, bool clockwise) {
  const float length = std::sqrt(a.stick_x * a.stick_x + a.stick_y_up * a.stick_y_up);
  const float blend = std::min(1.0f, dt / 0.15f);
  if (length < 0.3f || dt <= 0.0f) {
    a.have_angle = false;
    a.turn_speed -= a.turn_speed * blend;
    return;
  }
  const float angle = std::atan2(a.stick_y_up, a.stick_x);  // counterclockwise positive
  if (a.have_angle) {
    float turned = m::WrapAngle(angle - a.last_angle);
    if (clockwise) turned = -turned;
    a.turn_speed += (turned / dt - a.turn_speed) * blend;
  }
  a.last_angle = angle;
  a.have_angle = true;
}

void JudgeSwirl(Action& a, float dt) {
  TrackTurning(a, dt, a.def.clockwise);
  a.quality = Quality(a);
  const bool lever = a.def.interact_type == kLeverPush;
  const bool progress = lever ? LeverGood(a) : a.quality > 0.05f;
  if (progress) {
    a.started = true;
    a.since_progress = 0.0f;
  } else {
    a.since_progress += dt;
  }
  a.wrong_way_time = a.turn_speed < -kMinTurn ? a.wrong_way_time + dt : 0.0f;
  const bool turning = a.turn_speed > kMinTurn;
  // At a low frame rate every verdict needs two frames of evidence.
  if (a.wrong_way_time > std::max(kWrongWayTime, 1.5f * dt)) {
    Set(a, Verdict::kFail, "Wrong way");
  } else if (a.verdict == Verdict::kFail) {
    if (turning) NextAttempt(a);  // the next attempt starts as soon as it turns the right way
  } else if (lever ? (a.started && LeverLost(a))
                   : (a.started && a.since_progress > std::max(kStoppedTime, 1.5f * dt))) {
    Set(a, Verdict::kFail, "Keep turning");
  } else if (lever ? LeverGood(a)
                   : (a.quality >= kGoodQuality ||
                      (a.verdict == Verdict::kGood && a.quality >= kKeepGoodQuality))) {
    Set(a, Verdict::kGood);
  } else {
    Set(a, Verdict::kNeutral);
  }
}

void JudgeToggle(Action& a, float dt, int side_before) {
  a.quality = Quality(a);
  const bool flipped = a.last_side != side_before && a.last_side != 0;
  if (flipped) {
    ++a.events;
    a.since_flip = 0.0f;
    if (a.verdict == Verdict::kFail) NextAttempt(a);
    a.started = true;
  } else {
    a.since_flip += dt;
  }
  const bool lever = a.def.interact_type == kLeverPush;
  if (a.verdict == Verdict::kFail) return;  // until the next flip
  // Half a second without a flip at the usual pace (slower guides, like the
  // demo's at a low frame rate, wait longer; and always two frames).
  const float idle = std::max({kToggleIdleTime, 1.25f / std::max(a.guide_speed, 0.1f), 2.5f * dt});
  if (a.started && (a.since_flip > idle || (lever && LeverLost(a)))) {
    Set(a, Verdict::kFail, "Keep going");
  } else if (a.started && (lever ? LeverGood(a) : a.since_flip <= idle)) {
    Set(a, Verdict::kGood);
  } else {
    Set(a, Verdict::kNeutral);
  }
}

void JudgeRow(Action& a, float dt) {
  const bool caught = a.last_phase < m::kRowStrokeStart && a.phase >= m::kRowStrokeStart;
  const bool released = a.last_phase >= m::kRowStrokeStart && a.phase < m::kRowStrokeStart && !a.stroke_done;
  if (a.stroke_done) {
    ++a.events;
    a.good_hold = std::max(kRowGoodHold, 3.0f * dt);
    a.started = true;
  }
  if (a.phase >= m::kRowStrokeStart && a.phase > a.last_phase + 1e-4f) {
    a.since_rise = 0.0f;
  } else {
    a.since_rise += dt;
  }
  const bool going_back = a.phase >= m::kRowStrokeStart && a.phase < a.last_phase - 1e-4f;
  a.wrong_way_time = going_back ? a.wrong_way_time + dt : (a.phase > a.last_phase ? 0.0f : a.wrong_way_time);
  a.good_hold = std::max(0.0f, a.good_hold - dt);
  a.quality = Quality(a);
  if (a.slipped) {
    Set(a, Verdict::kFail, "You slipped");
  } else if (released) {
    Set(a, Verdict::kFail, "Finish the stroke");
  } else if (a.wrong_way_time > std::max(kWrongWayTime, 1.5f * dt)) {
    Set(a, Verdict::kFail, "Wrong way");
  } else if (a.verdict == Verdict::kFail) {
    if (caught || a.stroke_done) NextAttempt(a);  // the next catch is the next attempt
  }
  if (a.verdict != Verdict::kFail)
    Set(a, a.since_rise < std::max(0.25f, 2.0f * dt) || a.good_hold > 0.0f ? Verdict::kGood : Verdict::kNeutral);
  a.last_phase = a.phase;
}

void Judge(Action& a, float dt, int side_before) {
  if (a.def.interact_type == kLeverPush && dt > 0.0f) {
    const float trend = (a.score - a.last_score) / dt;
    a.score_trend += (trend - a.score_trend) * std::min(1.0f, dt / 0.2f);
    a.last_score = a.score;
  }
  if (!a.ready) {
    Set(a, Verdict::kNeutral);
    a.quality = 0.0f;
    a.last_phase = a.phase;
    return;
  }
  switch (a.kind) {
    case Kind::kSwirl:
      JudgeSwirl(a, dt);
      break;
    case Kind::kToggleHorizontal:
    case Kind::kToggleVertical:
      JudgeToggle(a, dt, side_before);
      break;
    case Kind::kRow:
      JudgeRow(a, dt);
      break;
    case Kind::kNone:
      break;
  }
}

// Verdict and device changes go to the log (the demo's at info level, so a
// test can follow them; a real action's at debug level, they come often).
void LogChanges(Action& a) {
  const Device device = Driving();
  const char* name = a.demo ? "demo" : a.def.name;
  if (!a.logged_device_once || device != a.logged_device) {
    a.logged_device_once = true;
    a.logged_device = device;
    REXLOG_INFO("cod3: battle action '{}': the {} drives", name, device == Device::kMouse ? "mouse" : "controller");
  }
  if (a.verdict == a.logged_verdict) return;
  a.logged_verdict = a.verdict;
  const char* text = a.verdict == Verdict::kGood ? "good" : a.verdict == Verdict::kFail ? "failed" : "neutral";
  if (a.demo)
    REXLOG_INFO("cod3: battle action demo: {}{}{}", text, *a.reason ? ": " : "", a.reason);
  else
    REXLOG_DEBUG("cod3: battle action '{}': {}{}{}", name, text, *a.reason ? ": " : "", a.reason);
}

float GuideSpeed(const Action& a, float m_yaw) {
  switch (a.kind) {
    case Kind::kSwirl:
      if (a.def.interact_type == kScaleAnimSpeed)
        return m::SwirlGuideRadiansPerSecond(a.def.anim_factor, a.def.anim_max, m_yaw);
      return m::kGuideFallbackTurns * m::kTwoPi;
    case Kind::kToggleHorizontal:
    case Kind::kToggleVertical:
      return m::kToggleGuideFlips;
    case Kind::kRow:
      return 1.0f;  // the guide's stroke at its normal pace
    default:
      return 0.0f;
  }
}

Kind KindFor(int input_type) {
  switch (input_type) {
    case 2: return Kind::kSwirl;
    case 3: return Kind::kToggleHorizontal;
    case 4: return Kind::kToggleVertical;
    case 5: return Kind::kRow;
    default: return Kind::kNone;
  }
}

// Starts a new action (the lock held).
void Begin(Action& a, uint32_t receiver, const Definition& def, Kind kind, bool demo) {
  a = Action{};
  a.receiver = receiver;
  a.def = def;
  a.kind = kind;
  a.live = true;
  a.demo = demo;
  a.serial = ++g_serial;
  a.last_update_ns = NowNs();
  g_cursor_kind.store(kind, std::memory_order_relaxed);
  g_cursor_on.store(kind != Kind::kNone, std::memory_order_relaxed);
  g_demo_on.store(demo && kind != Kind::kNone, std::memory_order_relaxed);
  SetCursor(0.0f, 0.0f);
}

void End(Action& a) {
  a.live = false;
  a.kind = Kind::kNone;
  g_cursor_on.store(false, std::memory_order_relaxed);
  g_demo_on.store(false, std::memory_order_relaxed);
}

// ---------------------------------------------------------------------------
// Reading the game
// ---------------------------------------------------------------------------
void CopyText(uint8_t* base, uint32_t addr, char* out, size_t size) {
  size_t n = 0;
  for (; n + 1 < size; ++n) {
    const char c = char(base[addr + n]);
    if (c == '\0') break;
    out[n] = (c >= 32 && c < 127) ? c : '?';  // guest data: printable only
  }
  out[n] = '\0';
}

bool ReadDefinition(uint8_t* base, uint32_t def, Definition* out) {
  if (!Valid(def)) return false;
  Definition d;
  CopyText(base, def, d.name, sizeof(d.name));
  CopyText(base, def + kDefHelp, d.help, sizeof(d.help));
  d.interact_type = ReadS32(base, def + kDefInteractType);
  d.input_type = ReadS32(base, def + kDefInputType);
  d.success_on_input = ReadU32(base, def + kDefSuccessOnInput) != 0;
  d.clockwise = ReadU32(base, def + kDefClockwise) != 0;
  d.left_stick = ReadU32(base, def + kDefLeftStick) != 0;
  d.accept_min = Finite(ReadF32(base, def + kDefAcceptMin));
  d.accept_max = Finite(ReadF32(base, def + kDefAcceptMax));
  d.rate_factor = Finite(ReadF32(base, def + kDefRateFactor), 1.0f);
  d.difficulty = Finite(ReadF32(base, def + kDefDifficulty));
  d.anim_factor = Finite(ReadF32(base, def + kDefAnimFactor));
  d.anim_max = Finite(ReadF32(base, def + kDefAnimMax));
  *out = d;
  return true;
}

// One log line per action name (the owner's play log tells what each
// level's crane, valve, bomb and boat use).
void LogDefinition(const Definition& d) {
  static std::array<std::array<char, 32>, 64> logged{};
  static size_t count = 0;
  for (size_t i = 0; i < count; ++i)
    if (std::strncmp(logged[i].data(), d.name, 32) == 0) return;
  if (count < logged.size()) std::memcpy(logged[count++].data(), d.name, 32);
  const char* interact = d.interact_type >= 0 && d.interact_type < int(std::size(kInteractTypeNames))
                             ? kInteractTypeNames[d.interact_type]
                             : "?";
  const char* input = d.input_type >= 0 && d.input_type < 6 ? kInputTypeNames[d.input_type] : "?";
  REXLOG_INFO(
      "cod3: battle action '{}': {}, {}, {} stick{}, input after {:.2f} s (fails after {:.2f} s), "
      "rate x{:.2f}, anim speed x{:.3f} up to {:.2f}, difficulty {:.2f}{}, help '{}'",
      d.name, interact, input, d.left_stick ? "left" : "right",
      d.input_type == 2 ? (d.clockwise ? ", clockwise" : ", counterclockwise") : "", d.accept_min,
      d.accept_max, d.rate_factor, d.anim_factor, d.anim_max, d.difficulty,
      d.success_on_input ? ", done on input" : "", d.help);
}

// The running InteractState that uses `receiver` (any local client), or 0.
uint32_t StateOf(uint8_t* base, uint32_t receiver) {
  for (uint32_t client = 0; client < 4; ++client) {
    const uint32_t interaction = ReadU32(base, kInteractions + 4 * client);
    if (!Valid(interaction)) continue;
    const uint32_t state = ReadU32(base, interaction + kInteractionState);
    if (Valid(state) && ReadU32(base, state + kStateReceiver) == receiver) return state;
  }
  return 0;
}

float GameMYaw(uint8_t* base) {
  const uint32_t dvar = ReadU32(base, kGameMYaw);
  const float v = Valid(dvar) ? ReadF32(base, dvar + 28) : 0.0f;
  return std::isfinite(v) && v > 0.0f && v < 10.0f ? v : 0.7f;
}

// The stick an action reads, as the game sees it (-1..1, up positive).
void GameStick(uint8_t* base, bool left, float* x, float* y_up) {
  const uint32_t client = std::min<uint32_t>(ReadU32(base, kActiveClient), 3);
  const uint32_t axes = kStickAxes + kStickAxesStride * client + (left ? 8 : 0);
  *x = std::clamp(float(ReadS32(base, axes)) / m::kGameAxisMax, -1.0f, 1.0f);
  *y_up = std::clamp(-float(ReadS32(base, axes + 4)) / m::kGameAxisMax, -1.0f, 1.0f);
}

// The pad the game reads: the one START was pressed on, else user 0.
uint32_t ActivePadUser(uint8_t* base) {
  const uint32_t manager = ReadU32(base, kPadManager);
  if (!Valid(manager) || !guest::ReadU8(base, manager + 28)) return 0;
  const uint32_t user = ReadU32(base, manager + 24);
  return user < 4 ? user : 0;
}

// Frames a second of the game (frame times the receivers get).
m::FrameRateMeter g_game_fps;    // game thread
m::FrameRateMeter g_turret_fps;  // game thread

// ---------------------------------------------------------------------------
// The game's stick arrow: hidden while the spinner shows, back when the
// controller takes over with the spinner off for controllers.
// ---------------------------------------------------------------------------
struct Arrow {
  uint32_t widget = 0;
  int type = 0;  // 1 swirl clockwise, 2 counterclockwise, 3 row, 4 vertical, 5 horizontal
  uint32_t arg = 1;
  double f1 = 0.0;
  bool held_back = false;  // the game asked for it while the spinner replaced it
};
Arrow g_arrow;  // game thread

void SyncArrow(PPCContext& ctx, uint8_t* base) {
  if (!Valid(g_arrow.widget)) return;
  const bool spinner = SpinnerShown();
  const int shown = ReadS32(base, g_arrow.widget + kArrowShownType);
  if (spinner && shown == g_arrow.type) {
    guest::Call c(ctx, base);
    c.Invoke(kArrowHide, {g_arrow.widget});
    g_arrow.held_back = true;
  } else if (!spinner && g_arrow.held_back && shown == 0) {
    g_arrow.held_back = false;
    guest::Call c(ctx, base);
    c.Invoke(kArrowShow, {g_arrow.widget, uint64_t(g_arrow.type), g_arrow.arg}, {g_arrow.f1});
  }
}

// ---------------------------------------------------------------------------
// The demo (cod3_qte_demo): the game's math on the stick or the mouse, no
// level needed. Ticked by the pad reads; never writes the pad.
// ---------------------------------------------------------------------------
struct Demo {
  Kind kind = Kind::kNone;
  bool clockwise = false;
  m::ToggleMeter toggle;
  m::RowboatModel row;
  m::FrameRateMeter fps;
  float elapsed = 0.0f;
  int64_t last_tick_ns = 0;
  float last_angle = 0.0f;
  bool have_angle = false;
  float turn = 0.0f;  // radians a second the way to go, over about half a second
};
Demo g_demo;  // game thread (under g_mutex)

constexpr float kDemoAcceptAfter = 1.0f;  // "get ready" for the first second

void StartDemo(Kind kind, bool clockwise) {
  std::lock_guard lock(g_mutex);
  if (g_action.live && !g_action.demo) return;  // a real action runs
  g_demo = Demo{};
  g_demo.kind = kind;
  g_demo.clockwise = clockwise;
  if (kind == Kind::kNone) {
    if (g_action.demo) End(g_action);
    g_action.demo = false;
    return;
  }
  Definition def;
  std::snprintf(def.name, sizeof(def.name), "demo");
  def.interact_type = kScaleAnimSpeed;
  def.input_type = kind == Kind::kSwirl ? 2 : kind == Kind::kToggleHorizontal ? 3
                                            : kind == Kind::kToggleVertical ? 4 : 5;
  def.clockwise = clockwise;
  def.accept_min = kDemoAcceptAfter;
  def.rate_factor = 1.0f;
  // Typical numbers: full speed at 1.6 turns a second (swirl) or 2.5 flips.
  def.anim_factor = kind == Kind::kSwirl ? 0.1f : 0.4f;
  def.anim_max = 1.0f;
  Begin(g_action, 0, def, kind, /*demo=*/true);
  g_action.guide_speed = GuideSpeed(g_action, 0.7f);
  REXLOG_INFO("cod3: battle action demo: {}", def.input_type < 6 ? kInputTypeNames[def.input_type] : "?");
}

void TickDemo(int64_t now, float stick_x, float stick_y_up) {
  std::lock_guard lock(g_mutex);
  if (!g_action.demo || g_demo.kind == Kind::kNone) return;
  if (g_demo.last_tick_ns == 0) g_demo.last_tick_ns = now;
  const float dt = float(now - g_demo.last_tick_ns) * 1e-9f;
  // Several pad reads a frame: one tick a frame (a few frames at most).
  if (dt < 0.03f) return;
  g_demo.last_tick_ns = now;
  const float frame = std::min(dt, 1.0f);
  g_demo.fps.Add(frame);
  g_demo.elapsed += frame;
  // The demo is meant to be followed at any frame rate, a test machine's few
  // frames a second too: its guide never asks for more than an eighth of a
  // turn (or half a flip, a sixth of a stroke) a frame. A real action uses
  // the game's own numbers.
  const float fps = g_demo.fps.FramesPerSecond();
  Action& a = g_action;
  a.last_update_ns = now;
  a.ready = g_demo.elapsed > kDemoAcceptAfter;
  // The stick: the mouse's or the pad's, through the game's dead zone.
  float x = stick_x, y_up = stick_y_up;
  if (Driving() == Device::kMouse) {
    float cx, cy;
    {
      std::lock_guard cursor(g_cursor_mutex);
      cx = g_cursor_x;
      cy = g_cursor_y;
    }
    m::Stick s = a.kind == Kind::kSwirl ? m::SwirlStick(cx, cy)
                 : a.kind == Kind::kRow ? m::RowStick(cx, cy)
                                        : m::ToggleStick(cx, cy, a.kind == Kind::kToggleVertical);
    x = s.x;
    y_up = s.y_up;
  }
  const float gx = m::GameAxis(x), gy = m::GameAxis(y_up);
  a.stick_x = gx / m::kGameAxisMax;
  a.stick_y_up = gy / m::kGameAxisMax;
  const int side_before = a.last_side;
  a.slipped = a.stroke_done = false;
  if (a.ready) {
    switch (a.kind) {
      case Kind::kSwirl: {
        // The console's progress grows with the square of the turning speed
        // (qte_math.h); here it is full at the guide's speed.
        a.guide_speed = std::min(m::SwirlGuideRadiansPerSecond(0.1f, 1.0f, 0.7f), fps * m::kPi * 0.25f);
        if (std::hypot(gx, gy) > 30.0f) {
          const float angle = std::atan2(gy, gx);
          if (g_demo.have_angle) {
            float turned = m::WrapAngle(angle - g_demo.last_angle);
            if (g_demo.clockwise) turned = -turned;
            g_demo.turn += (turned / frame - g_demo.turn) * std::min(1.0f, frame / 0.5f);
          }
          g_demo.last_angle = angle;
          g_demo.have_angle = true;
        } else {
          g_demo.have_angle = false;
          g_demo.turn -= g_demo.turn * std::min(1.0f, frame / 0.5f);
        }
        const float share = std::max(0.0f, g_demo.turn) / std::max(a.guide_speed, 0.1f);
        a.rate = std::min(share * share, 1.2f) * a.def.anim_max / a.def.anim_factor;
        break;
      }
      case Kind::kToggleHorizontal:
      case Kind::kToggleVertical:
        a.guide_speed = std::min(m::kToggleGuideFlips, std::max(0.3f, fps * 0.5f));
        a.rate = g_demo.toggle.Step(a.kind == Kind::kToggleVertical ? gy : gx, frame, 1.0f) *
                 (m::kToggleGuideFlips / a.guide_speed);
        a.last_side = g_demo.toggle.side();
        break;
      case Kind::kRow:
        a.guide_speed = std::min(1.0f, std::max(0.1f, fps / 6.0f));
        g_demo.row.Step(gx, gy);
        a.phase = g_demo.row.phase;
        a.slipped = g_demo.row.slipped;
        a.stroke_done = g_demo.row.stroke_done;
        break;
      case Kind::kNone:
        break;
    }
  }
  Judge(a, frame, side_before);
  LogChanges(a);
}

// ---------------------------------------------------------------------------
// Settings refresh (game thread)
// ---------------------------------------------------------------------------
void RefreshSettings(int64_t now) {
  static int64_t next = 0;
  static bool first = true;
  static std::string demo_seen = "off";
  if (now < next) return;
  next = now + 250 * kMs;
  g_style.store(rex::cvar::GetFlagByName("cod3_qte_style") == "original" ? Style::kOriginal : Style::kSpinner,
                std::memory_order_relaxed);
  g_spinner_with_pad.store(REXCVAR_GET(cod3_qte_spinner_with_pad), std::memory_order_relaxed);
  g_fps_fix.store(REXCVAR_GET(cod3_qte_fps_fix), std::memory_order_relaxed);
  const double degrees_per_count = REXCVAR_GET(cod3_sensitivity) * std::fabs(REXCVAR_GET(cod3_m_yaw));
  const double counts = degrees_per_count > 1e-6 ? kCursorDegreesPerRadius / degrees_per_count : 220.0;
  g_counts_per_radius.store(float(std::clamp(counts, 40.0, 4000.0)), std::memory_order_relaxed);
  // The demo is a test switch: never kept from the settings file.
  if (first) {
    first = false;
    if (rex::cvar::GetFlagSource("cod3_qte_demo") == rex::cvar::Source::kConfig)
      rex::cvar::SetFlagByName("cod3_qte_demo", "off");
  }
  std::string demo = rex::cvar::GetFlagByName("cod3_qte_demo");
  if (demo != demo_seen) {
    demo_seen = demo;
    bool clockwise = false;
    const Kind kind = DemoKindFor(demo, &clockwise);
    StartDemo(kind, clockwise);
  }
}

// ---------------------------------------------------------------------------
// Hint texts for the mouse (English): the game's text -> ours.
// ---------------------------------------------------------------------------
struct Hint {
  const char* game;
  const char* mouse;
};
constexpr Hint kHints[] = {
    {"Rotate right thumbstick clockwise.", "Circle the mouse clockwise."},
    {"Rotate right stick clockwise.", "Circle the mouse clockwise."},
    {"Rotate right analog stick clockwise.", "Circle the mouse clockwise."},
    {"Rotate right thumbstick counterclockwise.", "Circle the mouse counterclockwise."},
    {"Rotate right stick counterclockwise.", "Circle the mouse counterclockwise."},
    {"Rotate right analog stick counterclockwise.", "Circle the mouse counterclockwise."},
    {"Move right thumbstick up and down to move crowbar.", "Move the mouse up and down to move crowbar."},
    {"Move right stick up and down to move crowbar.", "Move the mouse up and down to move crowbar."},
    {"Move right analog stick up and down to move crowbar.", "Move the mouse up and down to move crowbar."},
    {"Toggle right thumbstick left and right.", "Move the mouse left and right."},
    {"Toggle right stick left and right.", "Move the mouse left and right."},
    {"Toggle right analog stick left and right.", "Move the mouse left and right."},
    {"Rotate right thumbstick \nin a half circle to row.", "Circle the mouse clockwise to row."},
    {"Rotate right stick \nin a half circle to row.", "Circle the mouse clockwise to row."},
    {"Rotate right analog stick \nin a half circle to row.", "Circle the mouse clockwise to row."},
};
constexpr size_t kHintCount = std::size(kHints);
std::mutex g_hint_mutex;
uint32_t g_hint_guest[kHintCount] = {};  // guest copies, made once

}  // namespace

// ===========================================================================
// Public
// ===========================================================================
Style CurrentStyle() { return g_style.load(std::memory_order_relaxed); }

bool SpinnerShown() {
  return CurrentStyle() == Style::kSpinner &&
         (Driving() == Device::kMouse || g_spinner_with_pad.load(std::memory_order_relaxed));
}

Snapshot CurrentSnapshot() {
  Snapshot s;
  const int64_t now = NowNs();
  std::lock_guard lock(g_mutex);
  const Action& a = g_action;
  if (!Alive(a, now)) return s;
  s.active = true;
  s.demo = a.demo;
  s.kind = a.kind;
  s.clockwise = a.kind == Kind::kRow ? true : a.def.clockwise;
  s.left_stick = a.def.left_stick;
  s.ready = a.ready;
  s.device = Driving();
  s.verdict = a.verdict;
  s.reason = a.reason;
  s.quality = a.quality;
  s.stick_x = a.stick_x;
  s.stick_y_up = a.stick_y_up;
  s.pad_x = g_pad_x.load(std::memory_order_relaxed);
  s.pad_y_up = g_pad_y.load(std::memory_order_relaxed);
  s.guide_speed = a.guide_speed;
  s.row_phase = a.phase;
  s.serial = a.serial;
  s.events = a.events;
  return s;
}

void AddMouse(float dx, float dy) {
  g_captured_motion_ns.store(NowNs(), std::memory_order_relaxed);
  NoteMouseMotion(dx, dy);
  if (!g_cursor_on.load(std::memory_order_relaxed)) return;
  const float scale = 1.0f / g_counts_per_radius.load(std::memory_order_relaxed);
  MoveCursor(dx * scale, dy * scale);
}

void AddPointerMotion(float dx, float dy) {
  // Only for the demo while the game doesn't hold the mouse (the menus):
  // window pixels, one to one with the drawn spinner.
  if (!g_demo_on.load(std::memory_order_relaxed)) return;
  if (NowNs() - g_captured_motion_ns.load(std::memory_order_relaxed) < 200 * kMs) return;
  NoteMouseMotion(dx, dy);
  const float radius = std::max(20.0f, g_drawn_radius_px.load(std::memory_order_relaxed));
  MoveCursor(dx / radius, dy / radius);
}

bool CursorPosition(float* x, float* y) {
  if (!g_cursor_on.load(std::memory_order_relaxed)) return false;
  std::lock_guard lock(g_cursor_mutex);
  *x = g_cursor_x;
  *y = g_cursor_y;
  return true;
}

// ui/qte_spinner.cpp: the radius it drew, for pointer motion in pixels.
void NoteDrawnRadius(float pixels) { g_drawn_radius_px.store(pixels, std::memory_order_relaxed); }

void ApplyToPad(uint8_t* base, uint32_t user, uint32_t state) {
  if (!Valid(state) || user != ActivePadUser(base)) return;
  const int64_t now = NowNs();
  RefreshSettings(now);

  Kind kind;
  bool left, alive, demo;
  {
    std::lock_guard lock(g_mutex);
    kind = g_action.kind;
    left = g_action.def.left_stick;
    alive = Alive(g_action, now);
    demo = g_action.demo;
  }
  // X_INPUT_STATE: s16 LX +8, LY +10, RX +12, RY +14 (up positive).
  const uint32_t axes = state + (left ? 8 : 12);
  const float x = float(int16_t(ReadU16(base, axes))) / 32767.0f;
  const float y_up = float(int16_t(ReadU16(base, axes + 2))) / 32767.0f;

  // The real stick moved: a push away from the middle, or 20% of travel
  // since the last time (a thumb resting on the stick doesn't count).
  static float ref_x = 0.0f, ref_y = 0.0f;  // game thread
  const float length = std::sqrt(x * x + y_up * y_up);
  if (length <= 0.3f) {
    ref_x = ref_y = 0.0f;
  } else if (std::hypot(x - ref_x, y_up - ref_y) > 0.2f) {
    ref_x = x, ref_y = y_up;
    g_pad_moved_ns.store(now, std::memory_order_relaxed);
  }
  g_pad_x.store(x, std::memory_order_relaxed);
  g_pad_y.store(y_up, std::memory_order_relaxed);

  if (demo) {
    TickDemo(now, x, y_up);
    return;
  }
  if (!alive || kind == Kind::kNone || Driving() != Device::kMouse) return;
  float cx, cy;
  {
    std::lock_guard lock(g_cursor_mutex);
    cx = g_cursor_x;
    cy = g_cursor_y;
  }
  const m::Stick s = kind == Kind::kSwirl ? m::SwirlStick(cx, cy)
                     : kind == Kind::kRow ? m::RowStick(cx, cy)
                                          : m::ToggleStick(cx, cy, kind == Kind::kToggleVertical);
  auto to_axis = [](float v) { return uint16_t(int16_t(std::lround(std::clamp(v, -1.0f, 1.0f) * 32767.0f))); };
  WriteU16(base, axes, to_axis(s.x));
  WriteU16(base, axes + 2, to_axis(s.y_up));
}

bool MouseOwnsView() {
  const int64_t now = NowNs();
  std::lock_guard lock(g_mutex);
  return !g_action.demo && Alive(g_action, now);
}

uint32_t LocalizeHint(uint8_t* base, uint32_t text) {
  if (!ValidText(text) || !REXCVAR_GET(mnk_mode) || !REXCVAR_GET(cod3_key_prompts)) return 0;
  if (Driving() != Device::kMouse) return 0;
  const char* t = reinterpret_cast<const char*>(base + text);
  if (t[0] != 'R' && t[0] != 'M' && t[0] != 'T') return 0;  // every hint starts so
  for (size_t i = 0; i < kHintCount; ++i) {
    if (std::strncmp(t, kHints[i].game, 96) != 0) continue;
    std::lock_guard lock(g_hint_mutex);
    if (!g_hint_guest[i]) {
      const size_t size = std::strlen(kHints[i].mouse) + 1;
      g_hint_guest[i] = rex::system::kernel_state()->memory()->SystemHeapAlloc(uint32_t(size));
      if (g_hint_guest[i]) std::memcpy(base + g_hint_guest[i], kHints[i].mouse, size);
    }
    return g_hint_guest[i];
  }
  return 0;
}

std::string MenuValue() { return CurrentStyle() == Style::kOriginal ? "ORIGINAL" : "SPINNER"; }

void MenuStep() {
  const Style next = CurrentStyle() == Style::kOriginal ? Style::kSpinner : Style::kOriginal;
  rex::cvar::SetFlagByName("cod3_qte_style", next == Style::kOriginal ? "original" : "spinner");
  g_style.store(next, std::memory_order_relaxed);
  REXLOG_INFO("cod3: battle actions: {}", next == Style::kOriginal ? "original" : "spinner");
}

std::string MenuHelp() {
  return "Stick battle actions (rotate, toggle, row). Spinner: a circle shows the way, green while "
         "it goes well, red when it fails. Original: the game's arrow. The mouse works with both.";
}

// ===========================================================================
// Game thread entry points from the hooks below
// ===========================================================================
namespace {

void OnReceiverStart(uint8_t* base, uint32_t receiver, uint32_t def_addr) {
  Definition def;
  if (!Valid(receiver) || !ReadDefinition(base, def_addr, &def)) return;
  const int type = ReadS32(base, receiver + kRcvrType);
  if (type < 0 || type > 5 || ReadU32(base, receiver) != kReceiverVtables[type]) return;
  LogDefinition(def);
  const Kind kind = KindFor(type);
  std::lock_guard lock(g_mutex);
  if (g_action.demo) g_demo = Demo{};  // a real action ends the demo
  Begin(g_action, receiver, def, kind, /*demo=*/false);
  g_action.guide_speed = GuideSpeed(g_action, GameMYaw(base));
  g_arrow = Arrow{};
  REXLOG_DEBUG("cod3: battle action '{}' started ({})", def.name,
               Driving() == Device::kMouse ? "mouse" : "controller");
}

void OnReceiverUpdate(uint8_t* base, uint32_t receiver, float dt) {
  const int64_t now = NowNs();
  {
    std::lock_guard lock(g_mutex);
    Action& a = g_action;
    if (a.demo || a.receiver != receiver || !a.live || a.kind == Kind::kNone) return;
    if (a.last_update_ns) {
      const float interval = std::clamp(float(now - a.last_update_ns) * 1e-9f, 0.001f, 1.0f);
      a.update_interval += (interval - a.update_interval) * 0.2f;
    }
    a.last_update_ns = now;
    a.ready = ReadF32(base, receiver + kRcvrElapsed) > ReadF32(base, receiver + kRcvrAcceptAfter);
    a.rate = Finite(ReadF32(base, receiver + kRcvrRate));
    a.phase = Finite(ReadF32(base, receiver + kRcvrPhase));
    const uint32_t flags = ReadU32(base, receiver + kRcvrFlags);
    a.slipped = (flags & kFlagSlipped) != 0;
    a.stroke_done = (flags & kFlagStrokeDone) != 0;
    int side_before = a.last_side;
    if (a.kind == Kind::kToggleHorizontal || a.kind == Kind::kToggleVertical) {
      a.last_side = ReadS32(base, receiver + kToggleLastSide);
      // The receiver is reused: its side from an earlier action isn't a flip.
      if (!a.have_side) side_before = a.last_side;
      a.have_side = true;
    }
    if (a.def.interact_type == kLeverPush) {
      const uint32_t state = StateOf(base, receiver);
      if (state) a.score = Finite(ReadF32(base, state + kLeverScore));
    }
    GameStick(base, a.def.left_stick, &a.stick_x, &a.stick_y_up);
    Judge(a, std::clamp(dt, 0.0f, 0.25f), side_before);
    LogChanges(a);
  }
}

void OnReceiverStop(uint32_t receiver) {
  std::lock_guard lock(g_mutex);
  if (!g_action.demo && g_action.receiver == receiver) End(g_action);
  g_arrow = Arrow{};
}

}  // namespace
}  // namespace cod3::qte

// ===========================================================================
// Hooks
// ===========================================================================

// InteractInputRcvr base Start(this, definition): every receiver type.
REX_EXTERN(__imp__sub_824F2D88);
REX_HOOK_RAW(sub_824F2D88) {
  const uint32_t receiver = ctx.r3.u32;
  const uint32_t def = ctx.r4.u32;
  __imp__sub_824F2D88(ctx, base);
  cod3::qte::OnReceiverStart(base, receiver, def);
}

// InteractInputRcvr::Update(this, f1 = frame seconds): every frame for the
// live receiver; returns the output in f1 (left as the game made it).
REX_EXTERN(__imp__sub_824F16D8);
REX_HOOK_RAW(sub_824F16D8) {
  const uint32_t receiver = ctx.r3.u32;
  const float dt = float(ctx.f1.f64);
  cod3::qte::g_game_fps.Add(dt);  // before the step, which may use it (swirl)
  __imp__sub_824F16D8(ctx, base);
  cod3::qte::OnReceiverUpdate(base, receiver, dt);
  cod3::qte::SyncArrow(ctx, base);
}

// The stick receivers' Stop.
REX_EXTERN(__imp__InteractInputRcvrStickSwirl__vfunc_1);
REX_HOOK_RAW(InteractInputRcvrStickSwirl__vfunc_1) {
  cod3::qte::OnReceiverStop(ctx.r3.u32);
  __imp__InteractInputRcvrStickSwirl__vfunc_1(ctx, base);
}
REX_EXTERN(__imp__InteractInputRcvrStickToggleHoriz__vfunc_1);
REX_HOOK_RAW(InteractInputRcvrStickToggleHoriz__vfunc_1) {
  cod3::qte::OnReceiverStop(ctx.r3.u32);
  __imp__InteractInputRcvrStickToggleHoriz__vfunc_1(ctx, base);
}
REX_EXTERN(__imp__InteractInputRcvrStickToggleVert__vfunc_1);
REX_HOOK_RAW(InteractInputRcvrStickToggleVert__vfunc_1) {
  cod3::qte::OnReceiverStop(ctx.r3.u32);
  __imp__InteractInputRcvrStickToggleVert__vfunc_1(ctx, base);
}
REX_EXTERN(__imp__InteractInputRcvrRowboat__vfunc_1);
REX_HOOK_RAW(InteractInputRcvrRowboat__vfunc_1) {
  cod3::qte::OnReceiverStop(ctx.r3.u32);
  __imp__InteractInputRcvrRowboat__vfunc_1(ctx, base);
}

// StickSwirl's step (this, float* output, float* phase, f1 = frame seconds):
// the output times the frame-rate factor (qte_math.h). The game sends its
// "started turning" notify the first time the output passes 3; when only the
// corrected output passes, it is sent here the same way.
REX_EXTERN(__imp__InteractInputRcvrStickSwirl__vfunc_5);
REX_HOOK_RAW(InteractInputRcvrStickSwirl__vfunc_5) {
  namespace q = cod3::qte;
  const uint32_t receiver = ctx.r3.u32;
  const uint32_t out = ctx.r4.u32;
  __imp__InteractInputRcvrStickSwirl__vfunc_5(ctx, base);
  if (!q::g_fps_fix.load(std::memory_order_relaxed) || !q::Valid(out)) return;
  const float factor = q::m::SwirlFrameRateFactor(q::g_game_fps.FramesPerSecond());
  const float value = cod3::guest::ReadF32(base, out);
  if (!(factor > 1.0f) || !(value > 0.0f)) return;
  const float fixed = value * factor;
  cod3::guest::WriteF32(base, out, fixed);
  const uint32_t flags = cod3::guest::ReadU32(base, receiver + q::kRcvrFlags);
  if (fixed > q::kSwirlNotifyLevel && !(flags & q::kFlagSwirlNotified)) {
    const uint32_t client = cod3::guest::ReadU32(base, q::kActiveClient);
    const uint32_t entities = cod3::guest::ReadU32(base, q::kPlayerEntities);
    if (client < 4 && q::Valid(entities)) {
      const uint32_t player = cod3::guest::ReadU32(base, entities + 4 * (client + 1));
      if (q::Valid(player)) {
        cod3::guest::Call c(ctx, base);
        c.Invoke(q::kNotify, {player, cod3::guest::ReadU32(base, q::kSwirlNotifyName)});
      }
    }
    cod3::guest::WriteU32(base, receiver + q::kRcvrFlags,
                          cod3::guest::ReadU32(base, receiver + q::kRcvrFlags) | q::kFlagSwirlNotified);
  }
}

// The swirl turret's turn (stick: 0 yaw, 1 pitch), once a frame for each
// stick while such a turret is used: put the console's turn a second back.
REX_EXTERN(__imp__sub_8250F648);
REX_HOOK_RAW(sub_8250F648) {
  namespace q = cod3::qte;
  const uint32_t stick = ctx.r3.u32;
  const uint32_t client = cod3::guest::ReadU32(base, q::kActiveClient);
  if (stick > 1 || client >= 4 || !q::g_fps_fix.load(std::memory_order_relaxed)) {
    __imp__sub_8250F648(ctx, base);
    return;
  }
  const uint32_t angle = q::kClientActive + client * q::kClientActiveStride + q::kTurretYaw - 4 * stick;
  const float before = cod3::guest::ReadF32(base, angle);
  __imp__sub_8250F648(ctx, base);
  if (stick == 0) q::g_turret_fps.Add(float(cod3::guest::ReadS32(base, q::kFrameMsec)) * 0.001f);
  const float after = cod3::guest::ReadF32(base, angle);
  const float step = after - before;
  const float fixed = q::m::TurretStep(step, q::g_turret_fps.FramesPerSecond(),
                                       cod3::guest::ReadF32(base, q::kTurretFactors + 4 * stick),
                                       q::kTurretStepLimit);
  if (fixed != step && std::isfinite(fixed) && std::isfinite(before))
    cod3::guest::WriteF32(base, angle, before + fixed);
}

// The stick arrow's Show(arrow, type, 1, f1): types 1-5 are the stick
// actions'. While the spinner shows, it replaces the arrow (Update brings
// the arrow back if a controller takes over with the spinner off for
// controllers; the game's own Stop hides it as always).
REX_EXTERN(__imp__sub_824C0EB0);
REX_HOOK_RAW(sub_824C0EB0) {
  namespace q = cod3::qte;
  const int type = int(ctx.r4.s32);
  if (type >= 1 && type <= 5) {
    q::g_arrow.widget = ctx.r3.u32;
    q::g_arrow.type = type;
    q::g_arrow.arg = ctx.r5.u32;
    q::g_arrow.f1 = ctx.f1.f64;
    q::g_arrow.held_back = false;
    if (q::SpinnerShown()) {
      q::g_arrow.held_back = true;
      return;
    }
  }
  __imp__sub_824C0EB0(ctx, base);
}

// ===========================================================================
// [claude-9b-think] 2026-10-29 UNTESTED cod3_fix_interact_events: the falaise crane's real fix, generic for every
// interaction (owner: "we need this game to function at uncapped fps").
//
// The good run (ob11, guard v2, 12:17) shows where the drop comes from. After lever_wait_final's press (the
// ButtonPress step's notify E6DD54A6 at level time 580033), the notifies 5A0A8232 and B613E4A8 (the drop, which the
// drop audio waits for), then 126EBF27 (+1 s) and 73923DB8 (the minigame's end, +3 s; Battle_Drone stops on it) all
// come from one place: 0x8250729C, in 0x82507150. InteractState::vfunc_3 calls it with f1 = dt. It fires the current
// battle action's timed events (up to 5: the definition's +1212 + 76 k names, +1248 times, each once: bit k of
// state +96) when the event time t lies in [elapsed - dt, elapsed), where elapsed is state +56.
//
// Elapsed is advanced elsewhere, by InteractState::vfunc_2 (elapsed += its own f1). The window only tiles when the
// two run in step with the same dt: at 60 fps, one frame for one game update, they do. Above 60, with the game in
// 1/60 s updates, their dt and cadence differ (one per drawn frame, the other per update). Then each window covers
// only part of the time elapsed advanced, and an event in the gap never fires. Its bit stays clear, the level's
// script waits for it forever, and the crane never drops. It is a matter of luck per event, and the drop chain needs
// several events in a row.
//
// Now each state's window starts at the elapsed time of its previous check: [previous elapsed, elapsed). Every
// stretch of the state's time is checked exactly once, at any frame rate and any split between frames and updates.
// f1 for 0x82507150 becomes elapsed - previous. At 60 fps that equals dt (no change), and in a check where elapsed
// did not move it is 0 (nothing fires twice: the bits stay as they were). A state's first check, a restarted state
// (elapsed went down) or a gap over 1 s keeps the game's own window.
// ===========================================================================
REXCVAR_DEFINE_BOOL(cod3_fix_interact_events, true, "cod3/Game",
                    "Battle actions and other interactions fire their timed events (the crane's drop, levers, "
                    "valves ...) at any frame rate: each check covers the time since the last one; off: the game's "
                    "own window, which can skip an event above 60 fps")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
#include <limits>
#include <unordered_map>

namespace cod3::qte {
namespace {
constexpr uint32_t kStateElapsed = 56;  // InteractState: its time (s), advanced by vfunc_2
std::mutex g_event_mutex;
std::unordered_map<uint32_t, float> g_event_last;  // state -> its elapsed time at its last event check
std::atomic<uint32_t> g_event_changed{0}, g_event_checks{0};
std::atomic<int64_t> g_event_log_ns{0};
}  // namespace
}  // namespace cod3::qte

// 0x82507150(state, f1 = dt): the state's timed events in [elapsed - dt, elapsed), from InteractState::vfunc_3.
REX_EXTERN(__imp__sub_82507150);
REX_HOOK_RAW(sub_82507150) {
  namespace q = cod3::qte;
  const uint32_t state = ctx.r3.u32;
  if (!REXCVAR_GET(cod3_fix_interact_events) || !q::Valid(state)) {
    __imp__sub_82507150(ctx, base);
    return;
  }
  const float now = cod3::guest::ReadF32(base, state + q::kStateElapsed);
  float prev = std::numeric_limits<float>::quiet_NaN();
  {
    std::lock_guard lock(q::g_event_mutex);
    if (auto it = q::g_event_last.find(state); it != q::g_event_last.end()) prev = it->second;
  }
  q::g_event_checks.fetch_add(1, std::memory_order_relaxed);
  if (std::isfinite(now) && std::isfinite(prev) && prev <= now && now - prev < 1.0f) {
    const double window = double(now) - double(prev);
    if (std::fabs(window - ctx.f1.f64) > 1e-5) q::g_event_changed.fetch_add(1, std::memory_order_relaxed);
    ctx.f1.f64 = window;
  }
  __imp__sub_82507150(ctx, base);
  {
    std::lock_guard lock(q::g_event_mutex);
    if (q::g_event_last.size() > 256) q::g_event_last.clear();  // states that went away
    q::g_event_last[state] = now;
  }
  // Once every 10 s while windows were changed: how many (they differ from the game's above 60 fps).
  const int64_t now_ns = q::NowNs();
  if (now_ns - q::g_event_log_ns.load(std::memory_order_relaxed) > 10000 * q::kMs) {
    q::g_event_log_ns.store(now_ns, std::memory_order_relaxed);
    const uint32_t changed = q::g_event_changed.exchange(0, std::memory_order_relaxed);
    const uint32_t checks = q::g_event_checks.exchange(0, std::memory_order_relaxed);
    if (changed)
      REXLOG_INFO("cod3: interaction events: {} of {} checks in the last 10 s took the time since the last check "
                  "(cod3_fix_interact_events)",
                  changed, checks);
  }
}
