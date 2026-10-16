// cod3 - scripted controller input (see test_input.h).

#include "test_input.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <sstream>

#include <rex/logging.h>
#include <rex/ui/virtual_key.h>

namespace cod3 {
using rex::X_RESULT;
using rex::X_STATUS;

using namespace rex::input;
using rex::ui::VirtualKey;

namespace {

constexpr DeviceId kScriptDevice = static_cast<DeviceId>(0x54455354);  // 'TEST'

struct ButtonName {
  const char* name;
  uint16_t mask;
  VirtualKey key;
};
constexpr ButtonName kButtons[] = {
    {"A", X_INPUT_GAMEPAD_A, VirtualKey::kXInputPadA},
    {"B", X_INPUT_GAMEPAD_B, VirtualKey::kXInputPadB},
    {"X", X_INPUT_GAMEPAD_X, VirtualKey::kXInputPadX},
    {"Y", X_INPUT_GAMEPAD_Y, VirtualKey::kXInputPadY},
    {"START", X_INPUT_GAMEPAD_START, VirtualKey::kXInputPadStart},
    {"BACK", X_INPUT_GAMEPAD_BACK, VirtualKey::kXInputPadBack},
    {"LB", X_INPUT_GAMEPAD_LEFT_SHOULDER, VirtualKey::kXInputPadLShoulder},
    {"RB", X_INPUT_GAMEPAD_RIGHT_SHOULDER, VirtualKey::kXInputPadRShoulder},
    {"UP", X_INPUT_GAMEPAD_DPAD_UP, VirtualKey::kXInputPadDpadUp},
    {"DOWN", X_INPUT_GAMEPAD_DPAD_DOWN, VirtualKey::kXInputPadDpadDown},
    {"LEFT", X_INPUT_GAMEPAD_DPAD_LEFT, VirtualKey::kXInputPadDpadLeft},
    {"RIGHT", X_INPUT_GAMEPAD_DPAD_RIGHT, VirtualKey::kXInputPadDpadRight},
    {"LS", X_INPUT_GAMEPAD_LEFT_THUMB, VirtualKey::kXInputPadLThumbPress},
    {"RS", X_INPUT_GAMEPAD_RIGHT_THUMB, VirtualKey::kXInputPadRThumbPress},
};

std::vector<std::string> Split(const std::string& s, char sep) {
  std::vector<std::string> out;
  std::string cur;
  std::istringstream in(s);
  while (std::getline(in, cur, sep))
    if (!cur.empty()) out.push_back(cur);
  return out;
}

}  // namespace

ScriptedInputDriver::ScriptedInputDriver(const std::string& script)
    : InputDriver(nullptr, 0), start_(std::chrono::steady_clock::now()) {
  // Bounded: a script is a test's input, not a game file; times must be
  // real numbers (NaN or infinity would never match a frame).
  constexpr size_t kMaxEvents = 4096;
  for (const auto& entry : Split(script, ';')) {
    if (events_.size() >= kMaxEvents) {
      REXLOG_WARN("cod3: scripted test input cut at {} events", kMaxEvents);
      break;
    }
    auto parts = Split(entry, ':');
    if (parts.size() < 2) continue;
    Event ev;
    ev.start = std::atof(parts[0].c_str());
    ev.end = ev.start + (parts.size() > 2 ? std::atof(parts[2].c_str()) : 0.15);
    if (!std::isfinite(ev.start) || !std::isfinite(ev.end) || ev.start < 0.0) continue;
    for (const auto& what : Split(parts[1], '+')) {
      if (what == "LT") {
        ev.lt = 255;
        continue;
      }
      if (what == "RT") {
        ev.rt = 255;
        continue;
      }
      if (auto eq = what.find('='); eq != std::string::npos) {
        static const char* kAxes[] = {"LX", "LY", "RX", "RY"};
        for (int i = 0; i < 4; ++i)
          if (what.compare(0, eq, kAxes[i]) == 0) {
            double v = std::clamp(std::atof(what.c_str() + eq + 1), -1.0, 1.0);
            ev.has_axis[i] = true;
            ev.axis[i] = static_cast<int16_t>(std::lround(v * 32767.0));
          }
        continue;
      }
      for (const auto& b : kButtons)
        if (what == b.name) ev.buttons |= b.mask;
    }
    events_.push_back(ev);
  }
  REXLOG_INFO("cod3: scripted test input with {} events", events_.size());
}

X_STATUS ScriptedInputDriver::Setup() {
  return X_STATUS_SUCCESS;
}

X_RESULT ScriptedInputDriver::SetDeviceVibration(DeviceId, X_INPUT_VIBRATION*) {
  return X_ERROR_SUCCESS;
}

void ScriptedInputDriver::EnumerateDevices(std::vector<DeviceInfo>& out) {
  DeviceInfo info;
  info.id = kScriptDevice;
  info.name = "cod3 test script";
  info.synthetic = true;
  out.push_back(info);
}

void ScriptedInputDriver::Update() {
  double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - start_).count();
  X_INPUT_GAMEPAD pad{};
  uint16_t buttons = 0;
  int16_t axis[4] = {};
  uint8_t lt = 0, rt = 0;
  for (const auto& ev : events_) {
    if (t < ev.start || t >= ev.end) continue;
    buttons |= ev.buttons;
    lt = std::max(lt, ev.lt);
    rt = std::max(rt, ev.rt);
    for (int i = 0; i < 4; ++i)
      if (ev.has_axis[i]) axis[i] = ev.axis[i];
  }
  pad.buttons = buttons;
  pad.left_trigger = lt;
  pad.right_trigger = rt;
  pad.thumb_lx = axis[0];
  pad.thumb_ly = axis[1];
  pad.thumb_rx = axis[2];
  pad.thumb_ry = axis[3];

  // Keystroke events for edges, for titles that navigate menus with them.
  uint16_t changed = buttons ^ last_buttons_;
  for (const auto& b : kButtons) {
    if (!(changed & b.mask)) continue;
    X_INPUT_KEYSTROKE ks{};
    ks.virtual_key = static_cast<uint16_t>(b.key);
    ks.flags = (buttons & b.mask) ? X_INPUT_KEYSTROKE_KEYDOWN : X_INPUT_KEYSTROKE_KEYUP;
    keys_.push(ks);
  }
  if (std::memcmp(&pad, &pad_, sizeof(pad)) != 0) {
    ++packet_;
    if (buttons != last_buttons_)
      REXLOG_INFO("cod3: test input t={:.1f}s buttons={:04X}", t, buttons);
  }
  last_buttons_ = buttons;
  pad_ = pad;
}

X_RESULT ScriptedInputDriver::GetDeviceState(DeviceId id, X_INPUT_STATE* out_state) {
  if (id != kScriptDevice) return X_ERROR_DEVICE_NOT_CONNECTED;
  std::lock_guard lock(mutex_);
  Update();
  if (out_state) {
    out_state->packet_number = packet_;
    out_state->gamepad = pad_;
  }
  return X_ERROR_SUCCESS;
}

X_RESULT ScriptedInputDriver::GetDeviceCapabilities(DeviceId id, uint32_t,
                                                     X_INPUT_CAPABILITIES* out_caps) {
  if (id != kScriptDevice) return X_ERROR_DEVICE_NOT_CONNECTED;
  if (out_caps) {
    std::memset(out_caps, 0, sizeof(*out_caps));
    out_caps->type = 0x01;
    out_caps->sub_type = 0x01;
    out_caps->gamepad.buttons = 0xFFFF;
    out_caps->gamepad.left_trigger = 0xFF;
    out_caps->gamepad.right_trigger = 0xFF;
    out_caps->gamepad.thumb_lx = static_cast<int16_t>(0x7FFF);
    out_caps->gamepad.thumb_ly = static_cast<int16_t>(0x7FFF);
    out_caps->gamepad.thumb_rx = static_cast<int16_t>(0x7FFF);
    out_caps->gamepad.thumb_ry = static_cast<int16_t>(0x7FFF);
  }
  return X_ERROR_SUCCESS;
}

X_RESULT ScriptedInputDriver::GetDeviceKeystroke(DeviceId id, uint32_t,
                                                  X_INPUT_KEYSTROKE* out_keystroke) {
  if (id != kScriptDevice) return X_ERROR_DEVICE_NOT_CONNECTED;
  std::lock_guard lock(mutex_);
  Update();
  if (keys_.empty()) return X_ERROR_EMPTY;
  if (out_keystroke) *out_keystroke = keys_.front();
  keys_.pop();
  return X_ERROR_SUCCESS;
}

}  // namespace cod3
