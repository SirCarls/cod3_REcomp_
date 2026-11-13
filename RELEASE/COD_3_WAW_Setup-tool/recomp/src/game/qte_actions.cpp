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
#include <cstdlib>  // [rexglue-native qte] std::strtof (cod3_test_qte_input)
#include <cstring>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>  // [rexglue-native qte] cod3_test_qte_bench's hitch

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>

#include <vector>  // [rexglue-native qte] cod3_test_qte_input

#include "game/addresses.h"
#include "game/guest.h"
#include "game/mouse_look.h"  // [rexglue-native qte] cod3_test_qte_input hands its motion to the mouse aim
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
// [rexglue-native qte] 2026-11-09 the swirl part now holds below 60 drawn frames too (qte_math.h SwirlRateFactor);
// the cranes' turn (TurretStep) as before, above 60.
REXCVAR_DEFINE_BOOL(cod3_qte_fps_fix, true, "cod3/Battle actions",
                    "Stick swirls (valves, cranes) progress as fast as on the console at 60 frames a second, at "
                    "any frame rate (the game's own math depends on it)");
// [rexglue-native qte] 2026-11-09 (owner 14:36: "frame rate shouldn't softlock the user within QTEs"): the mash
// battle actions' presses wait for the game's updates instead of merging ("Mash presses", below the receiver hooks).
REXCVAR_DEFINE_BOOL(cod3_qte_mash_queue, true, "cod3/Battle actions",
                    "Mash battle actions (melee struggles, \"press repeatedly\") count every press at any frame rate: "
                    "presses that come together wait for the next game update, one an update as on the console, "
                    "instead of counting as one");
REXCVAR_DEFINE_STRING(cod3_qte_demo, "off", "cod3/Battle actions",
                      "Test: fake a stick battle action anywhere (not saved): off, swirl_cw, "
                      "swirl_ccw, toggle_h, toggle_v, row")
    .allowed({"off", "swirl_cw", "swirl_ccw", "toggle_h", "toggle_v", "row"})
    .debug_only();
// [rexglue-native qte] 2026-11-09 (owner 13:19, priority 1: "check for ANY potential issues" with the battle
// actions): what a running battle action measures, for test runs and players' bug reports. Off by default.
REXCVAR_DEFINE_BOOL(cod3_debug_qte, false, "cod3/Battle actions",
                    "Log a running battle action twice a second (the rate or phase the game measures, flips or "
                    "strokes, the stick it reads, the mouse cursor, the frame rate) and a summary when it ends")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
// [rexglue-native qte] 2026-11-09 TEST ONLY (off by default, never kept from the settings file): mouse and
// controller input made up by a script, from the start of each battle action (or the demo), so the battle actions
// can be played in automated runs, where the game's window has no focus and holds no mouse. Mouse strokes go to
// the mouse aim the way the window's relative mouse motion does (MouseLook::AddDelta); controller strokes are
// written into the pad state the game reads, as a controller would set it. Entries "<start s>:<shape>:<seconds>"
// joined by ';'. Mouse shapes, in mouse counts: updown=A,HZ[,DRIFT] (up and down A counts, HZ times a second; DRIFT:
// sideways counts per count of travel, a slanted stroke), leftright=A,HZ[,DRIFT], circle=R,TURNS[,cw|ccw] (TURNS a
// second, as seen on the screen). Controller shapes, in stick deflection 0..1, on the stick the action reads:
// pad_updown=A,HZ, pad_leftright=A,HZ, pad_circle=A,TURNS[,cw|ccw]; and pad_press=RT/LT,HZ (one or two buttons
// in turn, HZ presses a second; A B X Y LB RB LT RT LS RS START BACK UP DOWN LEFT RIGHT).
// Example: "0.5:updown=180,3,0.1:15" (the crowbar with a slanted mouse stroke).
REXCVAR_DEFINE_STRING(cod3_test_qte_input, "", "cod3/Battle actions",
                      "Test only (not saved): scripted mouse and controller input from the start of each battle "
                      "action: \"<start s>:<shape>:<seconds>;...\" (game/qte_actions.cpp)")
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
// [rexglue-native qte] 2026-11-09 ButtonMash (inputType 0): its keys (+88; +92 the second key, -1 for one key; +96 the
// last key it took, for the alternation), set by its Start from the definition's +1004 and +1008 (sub_824F65F8): the
// definition's button 0-15 plus 207, the game's pad key numbers (5 -> 212 A, 8 -> 215 RT, 9 -> 216 LT; ButtonPress's
// Start does the same), and the press flag its key slot sets and its step (sub_824F2BD0) counts once and clears.
constexpr uint32_t kMashKey1 = 88;
constexpr uint32_t kMashKey2 = 92;
constexpr uint32_t kMashLastKey = 96;
constexpr uint32_t kFlagMashPress = 1;
constexpr uint32_t kDefMashKey1 = 1004;
constexpr uint32_t kDefMashKey2 = 1008;  // above 15: one key
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
  // [rexglue-native qte] 2026-11-09 cod3_debug_qte: when it started and when it is logged next.
  int64_t started_ns = 0;
  int64_t next_debug_ns = 0;
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
  // [rexglue-native qte] 2026-11-09: a toggle (the crowbar's up and down, the left and right ones) reads one
  // axis of the cursor only (ToggleStick). Its other axis used to keep every bit of sideways motion, and the
  // cursor's limit is a circle (kCursorLimit, 1.3 radii), so after a few slanted strokes the cursor sat near the
  // circle's side and its own axis could no longer reach the flip line (0.635 radii, the game's +-70): the crowbar
  // stopped with the mouse (owner 2026-11-09 13:19, players: "issue with mouse and the qtes"). The unused axis now
  // stays 0, so the whole range is always there. The stick the game reads is the same function of the used axis
  // as before, the thresholds and timing are the game's, and a controller is not touched by this at all.
  switch (g_cursor_kind.load(std::memory_order_relaxed)) {
    case Kind::kToggleHorizontal:
      y = 0.0f;
      break;
    case Kind::kToggleVertical:
      x = 0.0f;
      break;
    default:
      break;
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
  a.started_ns = a.last_update_ns;  // [rexglue-native qte] cod3_debug_qte
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
// [rexglue-native qte] 2026-11-09: in the campaign these are the game's fixed updates (cod3_server_hz, 60 a second,
// whatever the drawn frame rate; server_step.cpp), not drawn frames: G_RunFrame updates the battle action.
m::FrameRateMeter g_game_fps;    // game thread
m::FrameRateMeter g_turret_fps;  // game thread
// [rexglue-native qte] 2026-11-09: drawn frames a second, from the game's frame milliseconds (kFrameMsec, set once
// a drawn frame), read at each receiver update. The stick the receivers read changes once a drawn frame, so below
// 60 drawn frames two or more updates read the same stick (qte_math.h SwirlRateFactor).
m::FrameRateMeter g_drawn_fps;   // game thread

// [rexglue-native qte] 2026-11-09 cod3_debug_qte: one line twice a second while an action runs (g_mutex held).
// For button actions (kind kNone) the rate is the game's presses a second times the action's factor.
void DebugLog(Action& a, int64_t now) {
  if (!REXCVAR_GET(cod3_debug_qte) || now < a.next_debug_ns) return;
  a.next_debug_ns = now + 500 * kMs;
  float cx = 0.0f, cy = 0.0f;
  {
    std::lock_guard cursor(g_cursor_mutex);
    cx = g_cursor_x;
    cy = g_cursor_y;
  }
  static constexpr const char* kKinds[] = {"button", "swirl", "toggle_h", "toggle_v", "row"};
  REXLOG_INFO(
      "cod3: qte '{}' {} t={:.1f}s {} ready={} rate={:.2f} phase={:.2f} quality={:.2f} events={} {}{}{} "
      "stick=({:+.2f},{:+.2f}) cursor=({:+.2f},{:+.2f}) fps={:.0f} updates={:.0f}",
      a.demo ? "demo" : a.def.name, kKinds[size_t(a.kind) < std::size(kKinds) ? size_t(a.kind) : 0],
      float(now - a.started_ns) * 1e-9f, Driving() == Device::kMouse ? "mouse" : "pad", a.ready ? 1 : 0, a.rate,
      a.phase, a.quality, a.events,
      a.verdict == Verdict::kGood ? "good" : a.verdict == Verdict::kFail ? "failed" : "neutral",
      *a.reason ? ": " : "", a.reason, a.stick_x, a.stick_y_up, cx, cy, g_drawn_fps.FramesPerSecond(),
      g_game_fps.FramesPerSecond());  // [rexglue-native qte] drawn frames and the game's updates a second
}

// [rexglue-native qte] 2026-11-09 TEST ONLY: cod3_test_qte_input (see its definition at the top). The script's
// strokes, read on the game thread when the setting changes; each battle action (or the demo) runs them from its
// start: the mouse ones as raw mouse motion, the controller ones written into the pad state the game reads, the
// way a controller would set it. Nothing happens while the setting is empty, which it is unless a test run sets it.
struct TestStroke {
  enum class Shape : uint8_t { kUpDown, kLeftRight, kCircle, kPress };
  Shape shape = Shape::kUpDown;
  bool pad = false;     // a controller stroke ("pad_..."), else the mouse
  float start = 0.0f;   // seconds after the action started
  float length = 0.0f;  // seconds
  float size = 0.0f;    // amplitude or radius: mouse counts, or stick deflection (0..1)
  float rate = 0.0f;    // cycles, turns or presses a second
  float drift = 0.0f;   // mouse toggles: sideways counts per count of travel
  bool clockwise = true;
  uint16_t buttons[2] = {};   // pad_press: XInput button bits, taken in turn
  uint8_t triggers[2] = {};   // pad_press: 1 left trigger, 2 right trigger, taken in turn
  int count = 0;              // pad_press: how many buttons take turns (1 or 2)
};
struct TestInput {
  std::vector<TestStroke> strokes;
  uint32_t serial = 0;  // the action the script runs for now
  int64_t start_ns = 0;
  float last_x = 0.0f, last_y = 0.0f;  // where the script's mouse was at the last read (counts, y down)
};
TestInput g_test_input;  // game thread

// "A", "RT" ... -> the button bit or the trigger (pad_press).
bool TestButton(const std::string& name, uint16_t* button, uint8_t* trigger) {
  struct Name {
    const char* name;
    uint16_t button;
    uint8_t trigger;
  };
  static constexpr Name kNames[] = {
      {"UP", 0x0001, 0},   {"DOWN", 0x0002, 0}, {"LEFT", 0x0004, 0}, {"RIGHT", 0x0008, 0}, {"START", 0x0010, 0},
      {"BACK", 0x0020, 0}, {"LS", 0x0040, 0},   {"RS", 0x0080, 0},   {"LB", 0x0100, 0},    {"RB", 0x0200, 0},
      {"A", 0x1000, 0},    {"B", 0x2000, 0},    {"X", 0x4000, 0},    {"Y", 0x8000, 0},     {"LT", 0, 1},
      {"RT", 0, 2},
  };
  for (const Name& n : kNames) {
    if (name != n.name) continue;
    *button = n.button;
    *trigger = n.trigger;
    return true;
  }
  return false;
}

std::vector<TestStroke> ParseTestInput(const std::string& text) {
  std::vector<TestStroke> out;
  size_t at = 0;
  while (at < text.size() && out.size() < 64) {
    size_t end = text.find(';', at);
    if (end == std::string::npos) end = text.size();
    const std::string entry = text.substr(at, end - at);
    at = end + 1;
    // "<start>:<shape>=<args>:<seconds>"
    const size_t c1 = entry.find(':'), c2 = entry.rfind(':');
    if (c1 == std::string::npos || c2 == c1) continue;
    const std::string shape = entry.substr(c1 + 1, c2 - c1 - 1);
    const size_t eq = shape.find('=');
    if (eq == std::string::npos) continue;
    TestStroke s;
    s.start = std::strtof(entry.c_str(), nullptr);
    s.length = std::strtof(entry.c_str() + c2 + 1, nullptr);
    std::string name = shape.substr(0, eq);
    const std::string args = shape.substr(eq + 1);
    if (name.rfind("pad_", 0) == 0) {
      s.pad = true;
      name = name.substr(4);
    }
    float a = 0.0f, b = 0.0f, c = 0.0f;
    int n = 0;
    if (name == "press" && s.pad) {
      // pad_press=RT/LT,HZ: the buttons in turn, HZ presses a second, each held half its time.
      const size_t comma = args.find(',');
      const std::string buttons = args.substr(0, comma);
      s.shape = TestStroke::Shape::kPress;
      s.rate = comma == std::string::npos ? 0.0f : std::strtof(args.c_str() + comma + 1, nullptr);
      const size_t slash = buttons.find('/');
      const std::string first = buttons.substr(0, slash);
      const std::string second = slash == std::string::npos ? std::string() : buttons.substr(slash + 1);
      if (TestButton(first, &s.buttons[s.count], &s.triggers[s.count])) ++s.count;
      if (!second.empty() && TestButton(second, &s.buttons[s.count], &s.triggers[s.count])) ++s.count;
      if (!s.count || !(s.rate > 0.0f) || !std::isfinite(s.start + s.length + s.rate) || s.length <= 0.0f) continue;
      out.push_back(s);
      continue;
    }
    n = std::sscanf(args.c_str(), "%f,%f,%f", &a, &b, &c);
    if (name == "updown" || name == "leftright") {
      s.shape = name == "updown" ? TestStroke::Shape::kUpDown : TestStroke::Shape::kLeftRight;
      s.drift = n >= 3 ? c : 0.0f;
    } else if (name == "circle") {
      s.shape = TestStroke::Shape::kCircle;
      s.clockwise = args.find("ccw") == std::string::npos;
    } else {
      continue;
    }
    s.size = a;
    s.rate = b;
    if (n < 2 || !std::isfinite(s.start + s.length + s.size + s.rate + s.drift) || s.length <= 0.0f) continue;
    out.push_back(s);
  }
  return out;
}

// The script's mouse position (counts, y down, 0 at the start) `t` seconds into the action: each mouse stroke adds
// its shape over its own time and keeps its last position after it.
void TestMousePosition(float t, float* x, float* y) {
  *x = *y = 0.0f;
  for (const TestStroke& s : g_test_input.strokes) {
    if (s.pad || t < s.start) continue;
    const float local = std::min(t - s.start, s.length);
    const float phase = m::kTwoPi * s.rate * local;
    switch (s.shape) {
      case TestStroke::Shape::kUpDown:
        *y += s.size * std::sin(phase);
        *x += s.drift * 4.0f * s.size * s.rate * local;  // 4 * size of travel a cycle
        break;
      case TestStroke::Shape::kLeftRight:
        *x += s.size * std::sin(phase);
        *y += s.drift * 4.0f * s.size * s.rate * local;
        break;
      case TestStroke::Shape::kCircle:
        // On the screen (y down) (cos, sin) turns clockwise.
        *x += s.size * (std::cos(phase) - 1.0f);
        *y += (s.clockwise ? 1.0f : -1.0f) * s.size * std::sin(phase);
        break;
      case TestStroke::Shape::kPress:
        break;
    }
  }
}

// The script's controller `t` seconds into the action: true when a stick stroke runs (its deflection in *x, *y_up,
// -1..1), and the buttons and triggers its press strokes hold.
bool TestPadAt(float t, float* x, float* y_up, uint16_t* buttons, uint8_t* triggers) {
  bool stick = false;
  *x = *y_up = 0.0f;
  *buttons = 0;
  *triggers = 0;
  for (const TestStroke& s : g_test_input.strokes) {
    if (!s.pad || t < s.start || t >= s.start + s.length) continue;
    const float local = t - s.start;
    const float phase = m::kTwoPi * s.rate * local;
    switch (s.shape) {
      case TestStroke::Shape::kUpDown:
        *y_up = s.size * std::sin(phase), stick = true;
        break;
      case TestStroke::Shape::kLeftRight:
        *x = s.size * std::sin(phase), stick = true;
        break;
      case TestStroke::Shape::kCircle:
        // Clockwise as seen by the player: on the stick (y up) from 12 o'clock toward 3 o'clock.
        *x = (s.clockwise ? 1.0f : -1.0f) * s.size * std::sin(phase);
        *y_up = s.size * std::cos(phase);
        stick = true;
        break;
      case TestStroke::Shape::kPress: {
        const float presses = local * s.rate;
        const int k = int(presses);
        if (presses - float(k) < 0.5f) {
          *buttons |= s.buttons[k % s.count];
          *triggers |= s.triggers[k % s.count];
        }
        break;
      }
    }
  }
  return stick;
}

// Game thread, each read of the active pad while a battle action runs (or the demo): the script's mouse motion
// since the last read, handed over as the window would; and its controller, written into `state` (the guest
// X_INPUT_STATE) at the stick the action reads (`axes`: its x, then y). Returns true when it wrote the stick.
bool TickTestInput(uint8_t* base, uint32_t state, uint32_t axes, int64_t now, uint32_t serial, bool running) {
  if (g_test_input.strokes.empty() || !running) return false;
  if (serial != g_test_input.serial) {
    g_test_input.serial = serial;
    g_test_input.start_ns = now;
    g_test_input.last_x = g_test_input.last_y = 0.0f;
    REXLOG_INFO("cod3: qte test input: {} strokes from now (cod3_test_qte_input)", g_test_input.strokes.size());
  }
  const float t = float(now - g_test_input.start_ns) * 1e-9f;
  float x, y;
  TestMousePosition(t, &x, &y);
  const float dx = x - g_test_input.last_x, dy = y - g_test_input.last_y;
  g_test_input.last_x = x;
  g_test_input.last_y = y;
  if (dx != 0.0f || dy != 0.0f) cod3::MouseLook::Get().AddDelta(dx, dy);
  float px, py;
  uint16_t buttons;
  uint8_t triggers;
  const bool stick = TestPadAt(t, &px, &py, &buttons, &triggers);
  // X_INPUT_STATE (big-endian): u16 buttons +4, u8 LT +6, u8 RT +7.
  if (buttons) WriteU16(base, state + 4, uint16_t(ReadU16(base, state + 4) | buttons));
  if (triggers & 1) guest::WriteU8(base, state + 6, 255);
  if (triggers & 2) guest::WriteU8(base, state + 7, 255);
  if (!stick) return false;
  auto to_axis = [](float v) { return uint16_t(int16_t(std::lround(std::clamp(v, -1.0f, 1.0f) * 32767.0f))); };
  WriteU16(base, axes, to_axis(px));
  WriteU16(base, axes + 2, to_axis(py));
  return true;
}

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
  DebugLog(a, now);  // [rexglue-native qte] cod3_debug_qte
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
    // [rexglue-native qte] 2026-11-09 the test input too (cod3_test_qte_input).
    if (rex::cvar::GetFlagSource("cod3_test_qte_input") == rex::cvar::Source::kConfig)
      rex::cvar::SetFlagByName("cod3_test_qte_input", "");
  }
  // [rexglue-native qte] 2026-11-09 TEST ONLY: the scripted input's strokes, read again when the setting changes.
  {
    static std::string test_seen;
    std::string test = rex::cvar::GetFlagByName("cod3_test_qte_input");
    if (test != test_seen) {
      test_seen = test;
      g_test_input = TestInput{};
      g_test_input.strokes = ParseTestInput(test);
      if (!test.empty())
        REXLOG_INFO("cod3: qte test input: {} strokes (cod3_test_qte_input, tests only)", g_test_input.strokes.size());
    }
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

namespace {
// [rexglue-native qte] 2026-11-09 TEST ONLY: cod3_test_qte_bench's mash presses, a poll of them (end of this file).
void BenchPadPoll(uint8_t* base, int64_t now);
}  // namespace

void ApplyToPad(uint8_t* base, uint32_t user, uint32_t state) {
  if (!Valid(state) || user != ActivePadUser(base)) return;
  const int64_t now = NowNs();
  RefreshSettings(now);
  BenchPadPoll(base, now);  // [rexglue-native qte] 2026-11-09 TEST ONLY (nothing unless a test's bench runs a mash)

  Kind kind;
  bool left, alive, demo, button_action;
  uint32_t serial;
  {
    std::lock_guard lock(g_mutex);
    kind = g_action.kind;
    left = g_action.def.left_stick;
    alive = Alive(g_action, now);
    demo = g_action.demo;
    serial = g_action.serial;
    // [rexglue-native qte] a button action the game still updates (OnReceiverUpdate notes each update).
    button_action = g_action.live && !g_action.demo && kind == Kind::kNone &&
                    now - g_action.last_update_ns < 400 * kMs;
  }
  // X_INPUT_STATE: s16 LX +8, LY +10, RX +12, RY +14 (up positive).
  const uint32_t axes = state + (left ? 8 : 12);
  // [rexglue-native qte] 2026-11-09 TEST ONLY: the scripted input (cod3_test_qte_input; nothing when it's empty),
  // before the stick is read below, so a scripted controller counts as a controller.
  TickTestInput(base, state, axes, now, serial, ((alive || demo) && kind != Kind::kNone) || button_action);
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
    if (a.demo || a.receiver != receiver || !a.live) return;
    if (a.kind == Kind::kNone) {
      // [rexglue-native qte] 2026-11-09 cod3_debug_qte: button actions (melee struggles, the crowbar's swing, the
      // charges) are logged too: their rate is the game's presses a second times the action's factor. Their last
      // update is noted for the test input (cod3_test_qte_input). Nothing else is done for them here, as before
      // (Alive() is false for them: no spinner, the mouse keeps the view).
      a.last_update_ns = now;
      if (REXCVAR_GET(cod3_debug_qte)) {
        a.ready = ReadF32(base, receiver + kRcvrElapsed) > ReadF32(base, receiver + kRcvrAcceptAfter);
        a.rate = Finite(ReadF32(base, receiver + kRcvrRate));
        DebugLog(a, now);
      }
      return;
    }
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
    DebugLog(a, now);  // [rexglue-native qte] cod3_debug_qte
  }
}

void OnReceiverStop(uint32_t receiver) {
  std::lock_guard lock(g_mutex);
  // [rexglue-native qte] 2026-11-09 cod3_debug_qte: how the action went, in one line.
  if (REXCVAR_GET(cod3_debug_qte) && !g_action.demo && g_action.receiver == receiver && g_action.live)
    REXLOG_INFO("cod3: qte '{}' ended after {:.1f} s: {} flips or strokes, last rate {:.2f}, the {} drove, fps {:.0f}, "
                "updates {:.0f}",
                g_action.def.name, float(NowNs() - g_action.started_ns) * 1e-9f, g_action.events, g_action.rate,
                Driving() == Device::kMouse ? "mouse" : "controller", g_drawn_fps.FramesPerSecond(),
                g_game_fps.FramesPerSecond());
  if (!g_action.demo && g_action.receiver == receiver) End(g_action);
  g_arrow = Arrow{};
}

}  // namespace
}  // namespace cod3::qte

// ===========================================================================
// [rexglue-native qte] 2026-11-09 Mash presses (owner 14:36: "frame rate shouldn't softlock the user within QTEs")
// ===========================================================================
// ButtonMash (the melee struggles' "Pull ~r1 and ~l1 alternately", "Press ~cross repeatedly") takes the player's key
// events through its vtable slot 3 (0x824F2B48): a key of the action sets the press flag (+12 bit 0); with two keys
// only a key other than the last one taken (+96) does. Its step (sub_824F2BD0, every game update once input is
// taken) counts the flag once and clears it, so the game counts at most one press an update, and two presses that
// come before the next update count as one. The rate is the presses counted in the last 1 to 5 s (at least 1 s) times
// the action's inputRateFactor; the melee struggle (sub_824F2520) pits it against the enemy's strength every update,
// so a press lost is strength lost.
// The port runs the game's updates at a fixed 60 a second (server_step.cpp). At 60 drawn frames a second each frame
// has one update and presses rarely meet; below 60 a drawn frame's key events all come before its updates (two in
// one 50 ms frame at 20 fps count once), above 60 some frames have no update (presses in two such frames count once).
// With cod3_qte_mash_queue a key event of the action that would merge waits in a small queue, in order, and each
// update hands the next one to the game's own slot 3 before the update runs, when the flag is clear. The game still
// decides what counts (its alternation rule) and counts one an update, as on the console; nothing is made up or
// smoothed. A press that would count today goes straight through (nothing waits and the flag is clear), so it reaches
// the game in the same update as before; before the action takes input, presses go straight through too (they merge
// into the flag the game counts first, as before).
// The keyboard's and mouse buttons' presses reach the game as controller buttons inside an interaction
// (keybinds/binds.cpp); a press that came while its button was still down is pressed again after one poll up there,
// so it isn't dropped before it gets here.
REX_EXTERN(__imp__InteractInputRcvrButtonMash__vfunc_3);

namespace cod3::qte {
namespace {

constexpr size_t kMashQueueSize = 16;  // more than a second of fast mashing at 20 fps; more are dropped (counted)

struct MashQueue {
  uint32_t receiver = 0;  // the ButtonMash receiver the keys are for
  std::array<int32_t, kMashQueueSize> keys{};
  size_t first = 0, count = 0;
  // Since the action started (cod3_debug_qte's summary, the test bench).
  uint32_t waited = 0;   // key events that waited for an update
  uint32_t counted = 0;  // presses the game counted (its flag cleared by its step)
  uint32_t dropped = 0;  // key events dropped because the queue was full
};
MashQueue g_mash;  // game thread

bool IsMashReceiver(uint8_t* base, uint32_t receiver) {
  return Valid(receiver) && ReadU32(base, receiver) == kReceiverVtables[0];
}

// A ButtonMash action starts (or another receiver shows up): nothing waits.
void ResetMash(uint32_t receiver) {
  MashQueue& q = g_mash;
  if (REXCVAR_GET(cod3_debug_qte) && q.receiver && (q.counted || q.waited || q.dropped))
    REXLOG_INFO("cod3: qte mash: {} presses counted, {} of them waited for an update, {} dropped "
                "(cod3_qte_mash_queue)",
                q.counted, q.waited, q.dropped);
  q = MashQueue{};
  q.receiver = receiver;
}

// The game's key event for a ButtonMash receiver (its slot 3, hooked below). True: it waits in the queue, and the
// slot answers what it always answers for a key other than START and BACK (1, taken).
bool QueueMashKey(uint8_t* base, uint32_t receiver, int32_t key) {
  if (!REXCVAR_GET(cod3_qte_mash_queue) || !IsMashReceiver(base, receiver)) return false;
  if (key == 27 || key == 9) return false;  // START, BACK: the game's own answer
  const int32_t first = ReadS32(base, receiver + kMashKey1), second = ReadS32(base, receiver + kMashKey2);
  if (key != first && (second == -1 || key != second)) return false;  // not the action's key: as before
  // Not taking input yet: the game merges these into the flag it counts first, as before.
  if (!(ReadF32(base, receiver + kRcvrElapsed) > ReadF32(base, receiver + kRcvrAcceptAfter))) return false;
  MashQueue& q = g_mash;
  if (q.receiver != receiver) ResetMash(receiver);
  // Nothing waits and no press waits to be counted: this one counts at the next update, as before.
  if (q.count == 0 && !(ReadU32(base, receiver + kRcvrFlags) & kFlagMashPress)) return false;
  if (q.count == kMashQueueSize) {
    ++q.dropped;
    return true;
  }
  q.keys[(q.first + q.count) % kMashQueueSize] = key;
  ++q.count;
  ++q.waited;
  return true;
}

// Before a ButtonMash receiver's update: the waiting keys, in order, through the game's own slot 3, until one sets
// the flag (a key the game turns down, the same key twice in an alternating action, leaves it clear: the next one
// is tried). The rest wait for the next update.
void FeedMash(PPCContext& ctx, uint8_t* base, uint32_t receiver) {
  MashQueue& q = g_mash;
  if (q.receiver != receiver) return;
  while (q.count > 0 && !(ReadU32(base, receiver + kRcvrFlags) & kFlagMashPress)) {
    const int32_t key = q.keys[q.first];
    q.first = (q.first + 1) % kMashQueueSize;
    --q.count;
    guest::Call call(ctx, base);
    PPCContext& c = call.ctx();
    c.r3.u64 = receiver;
    c.r4.u64 = uint64_t(int64_t(key));
    __imp__InteractInputRcvrButtonMash__vfunc_3(c, base);
  }
}

// After its update: a press the game counted (its step cleared the flag).
void NoteMashCounted(uint8_t* base, uint32_t receiver, bool pressed_before) {
  if (!pressed_before || (ReadU32(base, receiver + kRcvrFlags) & kFlagMashPress)) return;
  if (g_mash.receiver != receiver) ResetMash(receiver);
  ++g_mash.counted;
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
  // [rexglue-native qte] 2026-11-09 a mash action starts with nothing waiting (Mash presses, above).
  if (cod3::qte::IsMashReceiver(base, receiver)) cod3::qte::ResetMash(receiver);
  cod3::qte::OnReceiverStart(base, receiver, def);
}

// [rexglue-native qte] 2026-11-09 ButtonMash's key slot (receiver, key): a key that would merge waits for an update
// (Mash presses, above); every other key goes to the game as before.
REX_HOOK_RAW(InteractInputRcvrButtonMash__vfunc_3) {
  if (cod3::qte::QueueMashKey(base, ctx.r3.u32, int32_t(ctx.r4.u32))) {
    ctx.r3.u64 = 1;
    return;
  }
  __imp__InteractInputRcvrButtonMash__vfunc_3(ctx, base);
}

// InteractInputRcvr::Update(this, f1 = frame seconds): every frame for the
// live receiver; returns the output in f1 (left as the game made it).
// [rexglue-native qte] 2026-11-09: "every frame" is every game update in the port (G_RunFrame -> sub_825012F8 ->
// InteractState vfunc_2 -> here, f1 = the update's seconds), 60 a second at any drawn frame rate.
REX_EXTERN(__imp__sub_824F16D8);
REX_HOOK_RAW(sub_824F16D8) {
  const uint32_t receiver = ctx.r3.u32;
  const float dt = float(ctx.f1.f64);
  cod3::qte::g_game_fps.Add(dt);  // before the step, which may use it (swirl)
  // [rexglue-native qte] 2026-11-09 the drawn frame's milliseconds (the same in every update of one drawn frame).
  cod3::qte::g_drawn_fps.Add(float(cod3::guest::ReadS32(base, cod3::qte::kFrameMsec)) * 0.001f);
  // [rexglue-native qte] 2026-11-09 a mash action: the next waiting press first (Mash presses, above).
  const bool mash = cod3::qte::IsMashReceiver(base, receiver);
  if (mash) cod3::qte::FeedMash(ctx, base, receiver);
  const bool mash_pressed =
      mash && (cod3::guest::ReadU32(base, receiver + cod3::qte::kRcvrFlags) & cod3::qte::kFlagMashPress) != 0;
  __imp__sub_824F16D8(ctx, base);
  if (mash) cod3::qte::NoteMashCounted(base, receiver, mash_pressed);
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
// [rexglue-native qte] 2026-11-09: frame seconds = the game update's (see sub_824F16D8 above).
// [rexglue-native qte] 2026-11-09 (owner 14:36: "frame rate shouldn't softlock the user within QTEs"): the factor
// is SwirlRateFactor's, from the game's updates and the drawn frames a second (qte_math.h), so the output is the
// console's at 60 at any frame rate: unchanged at 60 and above in the campaign's fixed updates, lower below 60 (where
// two or more updates read the same stick and the game's number came out up to 60/D times the console's).
// When the factor is below 1 the game's own notify would come from its larger number, early: it is held back for
// that step (the flag it checks set for the call) and sent below from the corrected output, as above 1.
REX_EXTERN(__imp__InteractInputRcvrStickSwirl__vfunc_5);
REX_HOOK_RAW(InteractInputRcvrStickSwirl__vfunc_5) {
  namespace q = cod3::qte;
  const uint32_t receiver = ctx.r3.u32;
  const uint32_t out = ctx.r4.u32;
  if (!q::g_fps_fix.load(std::memory_order_relaxed) || !q::Valid(out) || !q::Valid(receiver)) {
    __imp__InteractInputRcvrStickSwirl__vfunc_5(ctx, base);
    return;
  }
  const float factor = q::m::SwirlRateFactor(q::g_game_fps.FramesPerSecond(), q::g_drawn_fps.FramesPerSecond());
  const uint32_t flags_before = cod3::guest::ReadU32(base, receiver + q::kRcvrFlags);
  const bool hold_notify = factor < 1.0f && !(flags_before & q::kFlagSwirlNotified);
  if (hold_notify) cod3::guest::WriteU32(base, receiver + q::kRcvrFlags, flags_before | q::kFlagSwirlNotified);
  __imp__InteractInputRcvrStickSwirl__vfunc_5(ctx, base);
  if (hold_notify)
    cod3::guest::WriteU32(base, receiver + q::kRcvrFlags,
                          cod3::guest::ReadU32(base, receiver + q::kRcvrFlags) & ~q::kFlagSwirlNotified);
  const float value = cod3::guest::ReadF32(base, out);
  if (factor == 1.0f || !(value > 0.0f)) return;
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

// ---------------------------------------------------------------------------
// [acct2-main] 2026-11-05 the mortar's turning hints with any input (owner
// 17:40 Forest's mortar; 19:01 "It has to be the way it's intended").
// Mounting a swirl turret (type 5; 0x82508110) sets a byte per stick at
// 0x829C91A0 (yaw) / 0x829C91A1 (pitch). The game's turn step below
// (0x8250F648) sends the player's entity the notify named at 0x8206A76C
// ("stick_rot_left", yaw) or 0x8206A77C (pitch) the first time its stick
// moves (average over 0.005), then clears the byte (the pitch one only once
// the yaw one is clear). Level tutorials wait for it: Forest shows "Rotate
// [stick] to move barrel left and right" until stick_rot_left (forest.dll
// 0x89031748). The mouse and the turn keys turn the view in CL_AdjustAngles,
// never through that stick, so with them the hint stayed for good. Here a
// turn of the view between two steps of a stick (made outside the step: keys,
// the mouse) counts as that stick moving: at 0.5 degrees in all the game's
// notify is sent the game's way and the byte cleared by its rule. Only the
// player's input moves these angles (SetClientViewAngle moves delta_angles),
// and once the stick has sent it nothing more is sent. The turn is unchanged.
REXCVAR_DEFINE_BOOL(cod3_turret_hints_any_input, true, "cod3/Battle actions",
                    "Turret hints (the mortar's \"Rotate ... to move barrel\") go on when you turn "
                    "it with the mouse or keys too, as they do with the stick")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace cod3::qte {
namespace {

constexpr uint32_t kTurretHintFlags = 0x829C91A0;                   // a byte per stick
constexpr uint32_t kTurretHintNames[2] = {0x8206A76C, 0x8206A77C};  // the notify names (strings)
constexpr uint32_t kNameHash = 0x820CEBB8;                          // (string) -> the game's name
constexpr float kTurretHintTurn = 0.5f;                             // degrees
struct TurretHint {
  float last = 0.0f;  // this stick's angle after its last step
  float turned = 0.0f;
  int64_t ns = 0;
  bool valid = false;
};
TurretHint g_turret_hint[4][2];  // [client][stick]; CL_MouseMove's thread only

uint32_t TurretAngle(uint32_t client, uint32_t stick) {
  return kClientActive + client * kClientActiveStride + kTurretYaw - 4 * stick;
}

// Before the game's step: the turn since this stick's last step.
void TurretHintBefore(PPCContext& ctx, uint8_t* base, uint32_t stick, uint32_t client) {
  if (stick > 1 || client >= 4 || !REXCVAR_GET(cod3_turret_hints_any_input)) return;
  TurretHint& t = g_turret_hint[client][stick];
  const float now = ReadF32(base, TurretAngle(client, stick));
  const bool fresh = t.valid && NowNs() - t.ns < 500 * kMs && std::isfinite(now);
  if (!fresh || guest::ReadU8(base, kTurretHintFlags + stick) == 0) {
    t.turned = 0.0f;
    return;
  }
  const float d = std::remainder(now - t.last, 360.0f);
  if (std::isfinite(d)) t.turned += std::fabs(d);
  if (t.turned < kTurretHintTurn) return;
  const float turned = t.turned;
  t.turned = 0.0f;
  const uint32_t entities = ReadU32(base, kPlayerEntities);
  const uint32_t player = Valid(entities) ? ReadU32(base, entities + 4 * (client + 1)) : 0;
  if (!Valid(player)) return;
  guest::Call c(ctx, base);
  const uint32_t name = uint32_t(c.Invoke(kNameHash, {kTurretHintNames[stick]}));
  c.Invoke(kNotify, {player, name});
  if (stick == 0 || guest::ReadU8(base, kTurretHintFlags) == 0) guest::WriteU8(base, kTurretHintFlags + stick, 0);
  REXLOG_INFO("cod3: turret hints: the {} turned {:.1f} deg without the stick: sent the game's notify {:08X}",
              stick == 0 ? "yaw" : "pitch", turned, name);
}

// After it: where this stick's angle ended.
void TurretHintAfter(uint8_t* base, uint32_t stick, uint32_t client) {
  if (stick > 1 || client >= 4) return;
  TurretHint& t = g_turret_hint[client][stick];
  t.last = ReadF32(base, TurretAngle(client, stick));
  t.ns = NowNs();
  t.valid = std::isfinite(t.last);
}

}  // namespace
}  // namespace cod3::qte

// The swirl turret's turn (stick: 0 yaw, 1 pitch), once a frame for each
// stick while such a turret is used: put the console's turn a second back.
REX_EXTERN(__imp__sub_8250F648);
REX_HOOK_RAW(sub_8250F648) {
  namespace q = cod3::qte;
  const uint32_t stick = ctx.r3.u32;
  const uint32_t client = cod3::guest::ReadU32(base, q::kActiveClient);
  q::TurretHintBefore(ctx, base, stick, client);  // [acct2-main]
  if (stick > 1 || client >= 4 || !q::g_fps_fix.load(std::memory_order_relaxed)) {
    __imp__sub_8250F648(ctx, base);
    q::TurretHintAfter(base, stick, client);  // [acct2-main]
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
  q::TurretHintAfter(base, stick, client);  // [acct2-main]
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

// ===========================================================================
// [rexglue-native qte] 2026-11-09 TEST ONLY: cod3_test_qte_bench, one of the game's own battle action receivers run
// in the loaded level (owner 14:36: "frame rate shouldn't softlock the user within QTEs").
//
// No automated run reaches the levels' battle actions, so this starts one of the game's own input receivers (the
// singletons at kBenchReceivers: the swirl, a toggle, the rowboat) with a made-up definition, and updates it where
// and when the game updates a real one: right after the player's Interaction update (sub_825012F8), which G_RunFrame
// calls once per game update with the update's seconds (a real receiver is updated inside it, by InteractState
// vfunc_2). The rest is the real path: the receiver's Start and Update go through the port's hooks above (the spinner,
// the mouse cursor, cod3_test_qte_input's strokes, cod3_debug_qte's lines) and the stick comes from the pad through
// the game's IN_JoyMove. Only the InteractState that would own the receiver is missing, so its output drives
// nothing; the bench adds it up the way ScaleAnimSpeed turns it into the animation's speed (0x825067B0: clamp(
// animSpeedInputRateFactor * output, 0, animSpeedMax)).
// Every 5 s of game time one line: the game's updates and the drawn frames a second, the output, the animation's
// speed, and the flips or strokes a second. "hitch=MS:PERIOD" also stalls the game thread MS milliseconds every
// PERIOD seconds of game time (a slow frame, as a hitch is). The setting:
// "<kind>[,<kind>...][;after=S][;each=E][;hitch=MS:PERIOD]" with kinds swirl_cw, swirl_ccw, toggle_h, toggle_v or
// row, run one after the other for E seconds each (default 40; each is stopped through its vtable's Stop and the next
// started, so cod3_test_qte_input's strokes start again with each); the first starts S seconds (default 20) of game
// time after the level's first update. At each kind's end one summary line: its two phases, kBenchPhase1 and
// kBenchPhase2 seconds into it (a test's input puts its two kinds of strokes there). Read once, at the first update;
// never kept from the settings file. Nothing happens while it is empty, which it is unless a test run sets it.
// [rexglue-native qte] 2026-11-09 Also the kinds mash (one key) and mash2 (two keys in turn): the game's ButtonMash
// receiver with scripted presses (";rate=N" a second, default 8; ";hold=MS", default 40; ";jitter=MS", default 0),
// a controller's in phase 1 and a keyboard's in phase 2 (kMashPad); their lines give the game's rate and the presses
// it counted against the presses made. A mash kind also logs, once, the constants of the rate and the melee struggle.
// ===========================================================================
REXCVAR_DEFINE_STRING(cod3_test_qte_bench, "", "cod3/Battle actions",
                      "Test only (not saved): run the game's own battle action receivers in the loaded level and log "
                      "what they measure: \"<kind>[,<kind>...][;after=S][;each=E][;hitch=MS:PERIOD][;rate=N][;hold=MS]"
                      "[;jitter=MS]\" (game/qte_actions.cpp)")
    .debug_only();

namespace cod3::qte {
namespace {

constexpr uint32_t kBenchReceivers = 0x829E9278;  // InteractInputRcvr*[6] by inputType (this file's header)
constexpr uint32_t kReceiverUpdate = 0x824F16D8;  // (receiver, f1 = seconds), hooked above
constexpr uint32_t kDefinitionSize = 1592;        // an INTERACTSTATEFILE record
constexpr float kBenchAnimFactor = 0.1f;          // the made-up definition's animSpeedInputRateFactor
constexpr float kBenchAnimMax = 1.0f;             // and animSpeedMax: full speed at about 1.6 turns a second
// The two phases of each kind summed up at its end (seconds into the kind): the strokes a test makes from 3 to 18
// and from 21 to 36 s, without their first two seconds (the rates' 1 s windows filling up).
constexpr double kBenchPhase1[2] = {5.0, 18.0};
constexpr double kBenchPhase2[2] = {23.0, 36.0};

struct BenchKind {
  const char* name;
  int input_type;
  bool clockwise;
  int mash_keys = 0;  // [rexglue-native qte] 2026-11-09 ButtonMash: one key, or two in turn
};
constexpr BenchKind kBenchKinds[] = {
    {"swirl_cw", 2, true}, {"swirl_ccw", 2, false}, {"toggle_h", 3, false}, {"toggle_v", 4, false}, {"row", 5, false},
    {"mash", 0, false, 1}, {"mash2", 0, false, 2}};

// What one phase of a kind measured.
struct BenchPhase {
  double time = 0.0, output = 0.0, speed = 0.0, frame_ms = 0.0, max_dt = 0.0;
  int updates = 0, events = 0;
  int made = 0;  // [rexglue-native qte] 2026-11-09 mash: the scripted presses made in it
  void Add(double dt, float value, double speed_now, int events_now, int frame_ms_now, int made_now = 0) {
    time += dt;
    updates += 1;
    output += value;
    speed += speed_now * dt;
    events += events_now;
    frame_ms += frame_ms_now;
    max_dt = std::max(max_dt, dt);
    made += made_now;
  }
};

// [rexglue-native qte] 2026-11-09 The mash kinds' presses: a player pressing the action's key(s) `rate` times a
// second (in turn for two keys), each held `hold` ms, each moved by up to +-`jitter` ms (a fixed pseudo-random
// series), from kMashPad[0] to kMashPad[1] s and again from kMashKeys[0] to kMashKeys[1] s into the kind (wall time).
// The first series is a controller: the game's pad poll (each read of the active pad, ApplyToPad) sees a key down
// when a press holds it at that moment, and a key event when it went down since the last poll. The second is a key
// of the keyboard: its key-downs and key-ups between two polls are handed over at the next poll as keybinds/binds.cpp
// does inside an interaction (the controller button it presses: one poll at least, then one poll up before the slot
// is free; a press while it is down or up for that poll goes down after it; with cod3_qte_mash_queue off, as
// binds.cpp did before: free at once, a press while it is down dropped), and the game sees
// a key event when that button went down since the last poll. Nothing is written into the pad (the player doesn't
// fire): the key events go to the receiver's slot 3 (hooked: Mash presses) at the next update, before it runs, the
// order the game's own key events and updates come in a drawn frame.
constexpr double kMashPad[2] = {3.0, 18.0};
constexpr double kMashKeys[2] = {21.0, 36.0};
// The made-up definition's two buttons (+1004, +1008): RT and LT, as the melee struggles (the receiver's keys 215, 216).
constexpr int kMashBenchKeys[2] = {8, 9};
constexpr uint8_t kBenchPadQueueMax = 4;   // as binds.cpp's kPadQueueMax

struct BenchMashSlot {  // binds.cpp's Slot for one key, the parts a mash uses
  bool down = false, release_pending = false, gap = false, held = false;
  uint8_t queued = 0;
  uint32_t since_poll = 0;
};

// The press k of a series: its time (s into the kind) and key index.
double MashPressTime(double start, int k, float rate, float jitter_ms) {
  uint32_t h = uint32_t(k) * 2654435761u + 12345u;  // a fixed pseudo-random number for press k
  h ^= h >> 13;
  h *= 0x5bd1e995u;
  h ^= h >> 15;
  const double u = double(h & 0xFFFF) / 65535.0;  // 0..1
  return start + double(k) / rate + (2.0 * u - 1.0) * jitter_ms * 0.001;
}

struct Bench {
  bool read = false;
  std::vector<BenchKind> kinds;  // empty: off
  size_t index = 0;              // the kind running
  float after = 20.0f, each = 40.0f;
  int hitch_ms = 0;
  float hitch_period = 0.0f;
  bool started = false, failed = false, finished = false;
  uint32_t receiver = 0, def = 0;
  double game_time = 0.0, next_hitch = 0.0, started_at = 0.0;
  int64_t started_ns = 0, last_update_ns = 0;
  int last_side = 0;
  bool have_side = false;
  uint32_t last_flags = 0;
  // This 5 s window, and the kind's two phases.
  BenchPhase window, phase1, phase2;
  // [rexglue-native qte] 2026-11-09 the mash kinds (see kMashPad): the presses, and the polls' state.
  float mash_rate = 8.0f, mash_hold_ms = 40.0f, mash_jitter_ms = 0.0f;
  std::vector<int32_t> mash_events;  // key events of the polls since the last update
  bool mash_down[2] = {};            // the controller series: each key at the last poll
  bool mash_seen[2] = {};            // the keyboard series: the button the game saw at the last poll
  BenchMashSlot mash_slots[2];
  uint32_t mash_poll = 0;
  double mash_last_t = 0.0;      // the last poll, s into the kind
  int mash_made = 0;             // presses made so far (both series)
  int mash_made_measured = 0;    // ... of them in a phase already
  uint32_t mash_counted_measured = 0;
  int32_t mash_key[2] = {-1, -1};  // the receiver's keys (+88, +92) as its Start set them: the key events' numbers
  int64_t last_poll_ns = 0;
  const BenchKind& kind() const { return kinds[index]; }
};
Bench g_bench;  // game thread

void BenchRead() {
  Bench& b = g_bench;
  b.read = true;
  if (rex::cvar::GetFlagSource("cod3_test_qte_bench") == rex::cvar::Source::kConfig) {
    rex::cvar::SetFlagByName("cod3_test_qte_bench", "");
    return;
  }
  const std::string spec = rex::cvar::GetFlagByName("cod3_test_qte_bench");
  if (spec.empty()) return;
  size_t pos = 0;
  bool first = true;
  while (pos <= spec.size()) {
    size_t end = spec.find(';', pos);
    if (end == std::string::npos) end = spec.size();
    const std::string part = spec.substr(pos, end - pos);
    pos = end + 1;
    if (first) {  // the kinds, joined by ','
      first = false;
      size_t k0 = 0;
      while (k0 <= part.size()) {
        size_t k1 = part.find(',', k0);
        if (k1 == std::string::npos) k1 = part.size();
        const std::string name = part.substr(k0, k1 - k0);
        k0 = k1 + 1;
        for (const BenchKind& k : kBenchKinds)
          if (name == k.name) b.kinds.push_back(k);
      }
    } else if (part.rfind("after=", 0) == 0) {
      b.after = std::strtof(part.c_str() + 6, nullptr);
    } else if (part.rfind("each=", 0) == 0) {
      b.each = std::strtof(part.c_str() + 5, nullptr);
    } else if (part.rfind("hitch=", 0) == 0) {
      char* rest = nullptr;
      b.hitch_ms = int(std::strtol(part.c_str() + 6, &rest, 10));
      if (rest && *rest == ':') b.hitch_period = std::strtof(rest + 1, nullptr);
    } else if (part.rfind("rate=", 0) == 0) {  // [rexglue-native qte] 2026-11-09 the mash kinds' presses
      b.mash_rate = std::strtof(part.c_str() + 5, nullptr);
    } else if (part.rfind("hold=", 0) == 0) {
      b.mash_hold_ms = std::strtof(part.c_str() + 5, nullptr);
    } else if (part.rfind("jitter=", 0) == 0) {
      b.mash_jitter_ms = std::strtof(part.c_str() + 7, nullptr);
    }
  }
  if (!std::isfinite(b.after) || b.after < 0.0f) b.after = 20.0f;
  if (!std::isfinite(b.each) || b.each < 5.0f) b.each = 40.0f;
  if (b.hitch_ms < 0 || b.hitch_ms > 2000 || !(b.hitch_period >= 0.5f)) b.hitch_ms = 0;
  // [rexglue-native qte] 2026-11-09 the mash presses: 1-30 a second; each press's own time inside its period.
  if (!std::isfinite(b.mash_rate) || b.mash_rate < 1.0f || b.mash_rate > 30.0f) b.mash_rate = 8.0f;
  const float period_ms = 1000.0f / b.mash_rate;
  if (!std::isfinite(b.mash_jitter_ms) || b.mash_jitter_ms < 0.0f) b.mash_jitter_ms = 0.0f;
  b.mash_jitter_ms = std::min(b.mash_jitter_ms, period_ms * 0.25f);
  if (!std::isfinite(b.mash_hold_ms)) b.mash_hold_ms = 40.0f;
  b.mash_hold_ms = std::clamp(b.mash_hold_ms, 5.0f, std::max(5.0f, period_ms - 2.0f * b.mash_jitter_ms - 5.0f));
  REXLOG_INFO("cod3: qte bench: '{}': {} kinds, {:.0f} s each, from {:.0f} s of game time, hitch {} ms every {:.1f} s, "
              "mash {:.1f} presses a second held {:.0f} ms +-{:.0f} ms (cod3_test_qte_bench, tests only)",
              spec, b.kinds.size(), b.each, b.after, b.hitch_ms, b.hitch_ms ? b.hitch_period : 0.0f, b.mash_rate,
              b.mash_hold_ms, b.mash_jitter_ms);
}

// [rexglue-native qte] 2026-11-09 The constants the mash rate and the melee struggle use (scan 20, 23: the xex's
// data, unreadable without running it), logged once by a mash kind for the analysis of the presses a second a
// struggle needs: sub_824F2BD0 (the rate) and sub_824F2520 (the struggle, every update).
struct BenchConstant {
  uint32_t address;
  const char* what;
};
constexpr BenchConstant kMashConstants[] = {
    {0x82062C54, "zero / the difficulty that gets the factor twice (struggle f28)"},
    {0x82064200, "1 s: the rate's window, the struggle's position limit"},
    {0x82062720, "the rate's fade starts after this long without a press; the difficulty split"},
    {0x8207C864, "the rate's fade a second after that"},
    {0x826A250C, "the struggle: rate x this (twice at difficulty zero)"},
    {0x8207C85C, "the struggle: strength factor below the split"},
    {0x8207C860, "the struggle: strength factor at the split"},
    {0x826A13CC, "the struggle: strength factor above the split"},
    {0x826A13C4, "the struggle: the enemy's base"},
    {0x8207C858, "the struggle: the enemy's base factor"},
    {0x8207C800, "the struggle: the enemy's wave speed"},
    {0x8207C13C, "the struggle: the enemy's wave size"},
    {0x826A24FC, "the struggle: a pause this long (x strength factor) makes the enemy grow"},
    {0x82062C18, "the struggle: the enemy's growth factor then"},
    {0x826A13C0, "the struggle: the enemy's catch-up a second"},
    {0x82064214, "the struggle: the enemy's fall a second while the player leads"},
    {0x82062F3C, "the struggle: the enemy's floor"},
    {0x820632C8, "StrengthTest: c0"},
    {0x820635D0, "StrengthTest: c776"},
};

void BenchMashStarted(uint8_t* base, uint32_t receiver) {
  Bench& b = g_bench;
  static bool constants_logged = false;
  if (!constants_logged) {
    constants_logged = true;
    for (const BenchConstant& c : kMashConstants)
      REXLOG_INFO("cod3: qte bench: constant {:08X} = {:.6g} ({})", c.address, ReadF32(base, c.address), c.what);
  }
  REXLOG_INFO("cod3: qte bench: mash keys {} and {} (last {}), {:.1f} presses a second held {:.0f} ms +-{:.0f} ms, "
              "queue {}",
              ReadS32(base, receiver + kMashKey1), ReadS32(base, receiver + kMashKey2),
              ReadS32(base, receiver + kMashLastKey), b.mash_rate, b.mash_hold_ms, b.mash_jitter_ms,
              REXCVAR_GET(cod3_qte_mash_queue) ? "on" : "off");
  b.mash_events.clear();
  b.mash_down[0] = b.mash_down[1] = false;
  b.mash_seen[0] = b.mash_seen[1] = false;
  b.mash_slots[0] = b.mash_slots[1] = BenchMashSlot{};
  b.mash_poll = 0;
  b.mash_last_t = 0.0;
  b.mash_made = b.mash_made_measured = 0;
  b.mash_counted_measured = g_mash.counted;
  b.mash_key[0] = ReadS32(base, receiver + kMashKey1);
  b.mash_key[1] = ReadS32(base, receiver + kMashKey2);
}

// Each read of the active pad (ApplyToPad): a poll of the mash kinds' presses (see kMashPad).
void BenchPadPoll(uint8_t* base, int64_t now) {
  Bench& b = g_bench;
  if (!b.started || b.failed || b.finished || b.kinds.empty() || b.kind().input_type != 0) return;
  if (now - b.last_poll_ns < kMs) return;  // the same poll read twice
  b.last_poll_ns = now;
  const double t = double(now - b.started_ns) * 1e-9;  // s into the kind
  const double last = b.mash_last_t;
  b.mash_last_t = t;
  const uint32_t poll = ++b.mash_poll;
  const int keys = b.kind().mash_keys;
  const float rate = b.mash_rate, jitter = b.mash_jitter_ms;
  const double hold = b.mash_hold_ms * 0.001;
  // The presses made since the last poll (both series), for the phases.
  auto series_count = [&](const double* range, double until) {
    if (until <= range[0]) return 0;
    int n = 0;
    for (int k = 0;; ++k) {
      const double at = MashPressTime(range[0], k, rate, jitter);
      if (at >= range[1] || at > until) break;
      ++n;
    }
    return n;
  };
  b.mash_made = series_count(kMashPad, t) + series_count(kMashKeys, t);
  // The controller: each key down now? A key event when it went down since the last poll.
  if (t >= kMashPad[0] - 0.5 && t < kMashPad[1] + 0.5) {
    bool down[2] = {};
    const int k0 = std::max(0, int((t - kMashPad[0]) * rate) - 2);
    for (int k = k0; k < k0 + 5; ++k) {
      const double at = MashPressTime(kMashPad[0], k, rate, jitter);
      if (at >= kMashPad[1]) break;
      if (t >= at && t < at + hold) down[k % keys] = true;
    }
    for (int j = 0; j < keys; ++j) {
      if (down[j] && !b.mash_down[j]) b.mash_events.push_back(b.mash_key[j]);
      b.mash_down[j] = down[j];
    }
  }
  // The keyboard: key-downs and key-ups since the last poll, in order, as binds.cpp takes them.
  if (t >= kMashKeys[0] && last < kMashKeys[1] + 1.0) {
    const bool queue = REXCVAR_GET(cod3_qte_mash_queue);
    for (int j = 0; j < keys; ++j) {  // BeforePadPoll: the buttons seen once let go, a waiting press after a poll up
      BenchMashSlot& s = b.mash_slots[j];
      if (s.gap && s.since_poll < poll) {
        if (s.queued == 0) {
          s = BenchMashSlot{};  // nothing came during the gap: free
        } else {
          --s.queued;
          s.gap = false;
          s.down = true;
          s.since_poll = poll;
          s.release_pending = s.queued > 0 || !s.held;
        }
      } else if (s.down && s.release_pending && s.since_poll < poll) {
        if (queue) {  // [rexglue-native qte] binds.cpp now: up for one poll (gap), then free or the next press
          s.down = false;
          s.gap = true;
          s.release_pending = false;
          s.since_poll = poll;
        } else {
          s = BenchMashSlot{};  // binds.cpp before: free at once (a press in this poll went down at once)
        }
      }
    }
    struct KeyEvent {
      double at;
      int key;
      bool down;
    };
    std::vector<KeyEvent> events;
    const int k0 = std::max(0, int((last - kMashKeys[0]) * rate) - 2);
    for (int k = k0;; ++k) {
      const double at = MashPressTime(kMashKeys[0], k, rate, jitter);
      if (at >= kMashKeys[1] || at > t) break;
      if (at > last) events.push_back({at, k % keys, true});
      if (at + hold > last && at + hold <= t) events.push_back({at + hold, k % keys, false});
    }
    std::sort(events.begin(), events.end(), [](const KeyEvent& a, const KeyEvent& c) { return a.at < c.at; });
    for (const KeyEvent& e : events) {
      BenchMashSlot& s = b.mash_slots[e.key];
      if (e.down) {  // Press
        if (s.down || s.gap) {
          if (queue && s.queued < kBenchPadQueueMax) ++s.queued;  // before: dropped
          s.held = true;
        } else {
          s = BenchMashSlot{};
          s.down = true;
          s.since_poll = poll;
        }
      } else if (s.down || s.gap) {  // Release
        s.release_pending = true;
        s.held = false;
      }
    }
    for (int j = 0; j < keys; ++j) {  // the game's poll: a key event when the button went down
      const bool seen = b.mash_slots[j].down && !b.mash_slots[j].gap;
      if (seen && !b.mash_seen[j]) b.mash_events.push_back(b.mash_key[j]);
      b.mash_seen[j] = seen;
    }
  }
}

// Before the mash receiver's update: the polls' key events, to its slot 3 (hooked: Mash presses), in order.
void BenchMashDeliver(PPCContext& ctx, uint8_t* base) {
  Bench& b = g_bench;
  if (b.mash_events.empty()) return;
  const uint32_t slot3 = ReadU32(base, ReadU32(base, b.receiver) + 12);
  for (const int32_t key : b.mash_events) {
    guest::Call c(ctx, base);
    c.Invoke(slot3, {b.receiver, uint64_t(int64_t(key))});
  }
  b.mash_events.clear();
}

// The game's own receiver of the bench's kind, started with a made-up definition through its vtable's Start.
// The game makes a kind's receiver when the first InteractState that needs it is made (InteractState's constructor
// calls kMakeReceiver(inputType, its second argument) when the table has none; the receivers are freed when an
// interaction ends, 0x824F2DE8), so the bench makes it the same way, with the player's Interaction as that argument.
constexpr uint32_t kMakeReceiver = 0x824F8878;  // (inputType, owner) -> the receiver, also put in the table
void BenchStart(PPCContext& ctx, uint8_t* base) {
  Bench& b = g_bench;
  const BenchKind& k = b.kind();
  uint32_t receiver = ReadU32(base, kBenchReceivers + 4 * uint32_t(k.input_type));
  if (receiver == 0) {
    const uint32_t client = std::min<uint32_t>(ReadU32(base, kActiveClient), 3);
    const uint32_t interaction = ReadU32(base, kInteractions + 4 * client);
    guest::Call make(ctx, base);
    receiver = uint32_t(make.Invoke(kMakeReceiver, {uint32_t(k.input_type), Valid(interaction) ? interaction : 0u}));
  }
  if (!Valid(receiver) || ReadS32(base, receiver + kRcvrType) != k.input_type ||
      ReadU32(base, receiver) != kReceiverVtables[k.input_type]) {
    REXLOG_INFO("cod3: qte bench: no {} receiver ({:08X}): off", kInputTypeNames[k.input_type], receiver);
    b.failed = true;
    return;
  }
  if (!b.def) b.def = rex::system::kernel_state()->memory()->SystemHeapAlloc(kDefinitionSize);
  const uint32_t def = b.def;
  if (!Valid(def)) {
    b.failed = true;
    return;
  }
  std::memset(base + def, 0, kDefinitionSize);
  const std::string name = std::string("bench_") + k.name;
  std::memcpy(base + def, name.c_str(), std::min<size_t>(name.size(), 31));
  std::memcpy(base + def + kDefHelp, "bench", 5);
  // interactType: ScaleAnimSpeed for the swirl, Rowboat for the boat, StrengthTest for the toggles (the port's
  // hooks only name it; the receivers don't read it).
  // [rexglue-native qte] 2026-11-09 and Melee for a mash (ButtonMash's Start reads it only for MeleeStart, 9).
  WriteU32(base, def + kDefInteractType,
           k.input_type == 2 ? 5u : k.input_type == 5 ? 26u : k.input_type == 0 ? 7u : 4u);
  if (k.input_type == 0) {  // the keys, as ButtonMash's Start takes them (above 15: no second key)
    WriteU32(base, def + kDefMashKey1, uint32_t(kMashBenchKeys[0]));
    WriteU32(base, def + kDefMashKey2, k.mash_keys == 2 ? uint32_t(kMashBenchKeys[1]) : 16u);
  }
  WriteU32(base, def + kDefInputType, uint32_t(k.input_type));
  WriteF32(base, def + kDefAcceptMin, 0.5f);
  WriteF32(base, def + kDefAcceptMax, 0.5f);
  WriteF32(base, def + kDefRateFactor, 1.0f);
  WriteU32(base, def + kDefClockwise, k.clockwise ? 1u : 0u);
  WriteF32(base, def + kDefAnimFactor, kBenchAnimFactor);
  WriteF32(base, def + kDefAnimMax, kBenchAnimMax);
  b.receiver = receiver;
  b.have_side = false;
  b.last_flags = 0;
  b.window = b.phase1 = b.phase2 = BenchPhase{};
  const uint32_t start = ReadU32(base, ReadU32(base, receiver));  // vtable slot 0: Start(receiver, definition)
  guest::Call c(ctx, base);
  c.Invoke(start, {receiver, def});
  b.started = true;
  b.started_at = b.game_time;
  b.started_ns = NowNs();
  REXLOG_INFO("cod3: qte bench: the game's {} receiver ({:08X}) started for {}", kInputTypeNames[k.input_type],
              receiver, k.name);
  if (k.input_type == 0) BenchMashStarted(base, receiver);
}

// One phase of a kind, for its summary line.
std::string BenchPhaseText(const BenchPhase& p, bool strokes, bool mash = false) {
  if (p.updates == 0 || p.time <= 0.0) return "no updates";
  char text[256];
  if (mash) {  // [rexglue-native qte] 2026-11-09 the game's rate (inputRateFactor 1: its presses a second) and counts
    std::snprintf(text, sizeof(text),
                  "rate %.3f, %d presses counted (%.2f a second) of %d made (%.2f a second), %.1f updates a second "
                  "(longest %.0f ms), drawn %.0f fps",
                  p.output / p.updates, p.events, p.events / p.time, p.made, p.made / p.time, p.updates / p.time,
                  p.max_dt * 1000.0, p.frame_ms > 0.0 ? 1000.0 * p.updates / p.frame_ms : 0.0);
    return text;
  }
  std::snprintf(text, sizeof(text),
                "output %.3f, animation speed %.3f, %d %s (%.2f a second), %.1f updates a second (longest %.0f ms), "
                "drawn %.0f fps",
                p.output / p.updates, p.speed / p.time, p.events, strokes ? "strokes" : "flips", p.events / p.time,
                p.updates / p.time, p.max_dt * 1000.0, p.frame_ms > 0.0 ? 1000.0 * p.updates / p.frame_ms : 0.0);
  return text;
}

// The kind's end: its summary, its receiver stopped through its vtable's Stop, the next kind started (if any).
void BenchNextKind(PPCContext& ctx, uint8_t* base) {
  Bench& b = g_bench;
  const BenchKind& k = b.kind();
  const bool strokes = k.input_type == 5;
  const bool mash = k.input_type == 0;  // [rexglue-native qte] 2026-11-09 phase 1: the controller, 2: the keyboard
  char queue_text[96] = "";
  if (mash)
    std::snprintf(queue_text, sizeof(queue_text), "; queue %s: %u waited, %u dropped",
                  REXCVAR_GET(cod3_qte_mash_queue) ? "on" : "off", g_mash.waited, g_mash.dropped);
  REXLOG_INFO("cod3: qte bench summary {}: phase 1 ({:.0f}-{:.0f} s): {}; phase 2 ({:.0f}-{:.0f} s): {}{}", k.name,
              kBenchPhase1[0], kBenchPhase1[1], BenchPhaseText(b.phase1, strokes, mash), kBenchPhase2[0],
              kBenchPhase2[1], BenchPhaseText(b.phase2, strokes, mash), queue_text);
  const uint32_t stop = ReadU32(base, ReadU32(base, b.receiver) + 4);  // vtable slot 1: Stop(receiver)
  {
    guest::Call c(ctx, base);
    c.Invoke(stop, {b.receiver});
  }
  b.started = false;
  if (++b.index >= b.kinds.size()) {
    b.finished = true;
    REXLOG_INFO("cod3: qte bench: done");
    return;
  }
  BenchStart(ctx, base);
}

// After each of the bench receiver's updates: its output, and its flips (toggles) or strokes (the boat).
void BenchMeasure(uint8_t* base, double dt) {
  Bench& b = g_bench;
  const BenchKind& k = b.kind();
  const float out = ReadF32(base, b.receiver + kRcvrRate);
  const uint32_t flags = ReadU32(base, b.receiver + kRcvrFlags);
  int events = 0;
  if (k.input_type == 3 || k.input_type == 4) {
    const int side = ReadS32(base, b.receiver + kToggleLastSide);
    if (b.have_side && side != b.last_side && side != 0) events = 1;
    b.last_side = side;
    b.have_side = true;
  } else if (k.input_type == 5) {
    if ((flags & kFlagStrokeDone) && !(b.last_flags & kFlagStrokeDone)) events = 1;
  }
  // [rexglue-native qte] 2026-11-09 a mash: the presses the game counted in this update, and the presses made.
  int made = 0;
  if (k.input_type == 0) {
    events = int(g_mash.counted - b.mash_counted_measured);
    b.mash_counted_measured = g_mash.counted;
    made = b.mash_made - b.mash_made_measured;
    b.mash_made_measured = b.mash_made;
  }
  b.last_flags = flags;
  const float value = std::isfinite(out) ? out : 0.0f;
  const double speed = std::clamp(kBenchAnimFactor * value, 0.0f, kBenchAnimMax);
  const int frame_ms = ReadS32(base, kFrameMsec);
  const double t = b.game_time - b.started_at;  // seconds into this kind
  b.window.Add(dt, value, speed, events, frame_ms, made);
  if (t >= kBenchPhase1[0] && t < kBenchPhase1[1]) b.phase1.Add(dt, value, speed, events, frame_ms, made);
  if (t >= kBenchPhase2[0] && t < kBenchPhase2[1]) b.phase2.Add(dt, value, speed, events, frame_ms, made);
  if (b.window.time < 5.0) return;
  REXLOG_INFO("cod3: qte bench {}: {:.1f} s in (wall {:.1f} s): {}", k.name, t,
              double(NowNs() - b.started_ns) * 1e-9, BenchPhaseText(b.window, k.input_type == 5, k.input_type == 0));
  b.window = BenchPhase{};
}

void BenchAfterInteractionUpdate(PPCContext& ctx, uint8_t* base, double dt) {
  Bench& b = g_bench;
  if (!b.read) BenchRead();
  if (b.kinds.empty() || b.failed || b.finished) return;
  // A second or more without an update is a level loading: the game time starts again from it (so a test's
  // "after" counts in the level it loaded, not in the title screen's). A level loaded after the start ends the bench.
  const int64_t now = NowNs();
  const bool loaded = b.last_update_ns != 0 && now - b.last_update_ns > 1000 * kMs;
  b.last_update_ns = now;
  if (loaded) {
    if (b.started) {
      REXLOG_INFO("cod3: qte bench: a level loaded: the bench is off");
      b.failed = true;
      return;
    }
    b.game_time = 0.0;
    b.next_hitch = 0.0;
  }
  if (!(dt > 0.0) || dt > 1.0) return;
  b.game_time += dt;
  if (b.hitch_ms > 0 && b.game_time >= b.next_hitch) {
    if (b.next_hitch > 0.0) std::this_thread::sleep_for(std::chrono::milliseconds(b.hitch_ms));
    b.next_hitch = b.game_time + b.hitch_period;
  }
  if (!b.started) {
    if (b.game_time >= b.after) BenchStart(ctx, base);
    return;
  }
  if (b.kind().input_type == 0) BenchMashDeliver(ctx, base);  // [rexglue-native qte] 2026-11-09 the polls' keys first
  {
    guest::Call c(ctx, base);
    c.Invoke(kReceiverUpdate, {b.receiver}, {dt});
  }
  BenchMeasure(base, dt);
  if (b.game_time - b.started_at >= b.each) BenchNextKind(ctx, base);
}

}  // namespace
}  // namespace cod3::qte

// The player's Interaction update (Interaction*, f1 = the update's seconds), from G_RunFrame once per game update
// (0x8256D3E0: msec * 0.001). Only the test bench above uses it; the game's own call is unchanged.
REX_EXTERN(__imp__sub_825012F8);
REX_HOOK_RAW(sub_825012F8) {
  const double dt = ctx.f1.f64;
  __imp__sub_825012F8(ctx, base);
  cod3::qte::BenchAfterInteractionUpdate(ctx, base, dt);
}
