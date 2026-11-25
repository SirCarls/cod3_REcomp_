// inputkit - the C interface (see inputkit.h).

#include "inputkit/inputkit.h"

#include <memory>
#include <string>
#include <vector>

#include "inputkit/binds.h"
#include "inputkit/keys.h"
#include "inputkit/mouse.h"
#include "inputkit/sens_scale.h"

using namespace inputkit;

struct ik_binds {
  std::vector<binds::ActionSpec> specs;
  binds::Table table{};
  std::unique_ptr<binds::KeyRow[]> keys;
  std::vector<std::string> notes;
  std::string last_note;
  std::string text;  // the last string handed out
};

struct ik_motion {
  mouse::MotionQueue queue;
};

extern "C" {

uint16_t ik_key_parse(const char* name) { return name ? ParseKey(name) : kNoKey; }

const char* ik_key_name(uint16_t code) {
  thread_local std::string name;
  name = KeyName(code);
  return name.c_str();
}

int ik_key_is_combination(const char* name) { return name && IsCombination(name); }
int ik_key_always_reserved(uint16_t code) { return IsAlwaysReserved(code); }
uint16_t ik_key_from_sdl_scancode(int scancode) { return KeyFromSdlScancode(scancode); }
uint16_t ik_key_from_sdl_mouse_button(int button) { return KeyFromSdlMouseButton(button); }

ik_binds* ik_binds_new(const ik_action* actions, int count, const char* setting_name) {
  if (!actions || count <= 0) return nullptr;
  auto* b = new ik_binds;
  b->specs.resize(size_t(count));
  for (int i = 0; i < count; ++i) {
    binds::ActionSpec& s = b->specs[size_t(i)];
    s.id = actions[i].id;
    s.label = actions[i].label ? actions[i].label : actions[i].id;
    s.kind = actions[i].kind;
    s.defaults[0] = s.defaults[1] = kNoKey;
    int slot = 0;
    for (std::string_view name : binds::Split(actions[i].defaults ? actions[i].defaults : "", ','))
      if (slot < binds::kSlots)
        if (const KeyCode k = ParseKey(name); k != kNoKey) s.defaults[slot++] = k;
  }
  b->table = {b->specs.data(), count, setting_name ? setting_name : "the key binds"};
  b->keys.reset(new binds::KeyRow[size_t(count)]);
  binds::Defaults(b->table, b->keys.get());
  return b;
}

void ik_binds_free(ik_binds* b) { delete b; }

int ik_binds_parse(ik_binds* b, const char* text) {
  if (!b) return 0;
  b->notes.clear();
  binds::Parse(b->table, text ? text : "", b->keys.get(), &b->notes);
  return int(b->notes.size());
}

const char* ik_binds_note(const ik_binds* b, int i) {
  return b && i >= 0 && size_t(i) < b->notes.size() ? b->notes[size_t(i)].c_str() : "";
}

int ik_binds_assign_text(ik_binds* b, int action, const char* text) {
  if (!b) return 0;
  b->last_note.clear();
  return binds::AssignText(b->table, b->keys.get(), action, text ? text : "", &b->last_note);
}

const char* ik_binds_last_note(const ik_binds* b) { return b ? b->last_note.c_str() : ""; }

int ik_binds_action_by_id(const ik_binds* b, const char* id) {
  return b && id ? binds::ActionById(b->table, id) : -1;
}

uint16_t ik_binds_key(const ik_binds* b, int action, int slot) {
  if (!b || action < 0 || action >= b->table.count || slot < 0 || slot >= binds::kSlots) return kNoKey;
  return b->keys[size_t(action)][slot];
}

const char* ik_binds_keys_text(ik_binds* b, int action) {
  if (!b || action < 0 || action >= b->table.count) return "";
  b->text = binds::KeysText(b->keys.get(), action);
  return b->text.c_str();
}

int ik_binds_actions_of_key(const ik_binds* b, uint16_t key, int* out, int max) {
  if (!b || !out || max <= 0) return 0;
  binds::KeyUse uses[binds::kMaxActionsPerKey];
  const int n = binds::FindAll(b->table, b->keys.get(), key, uses, max < binds::kMaxActionsPerKey ? max : binds::kMaxActionsPerKey);
  for (int i = 0; i < n; ++i) out[i] = uses[i].action;
  return n;
}

const char* ik_binds_serialize(ik_binds* b) {
  if (!b) return "";
  b->text = binds::Serialize(b->table, b->keys.get());
  return b->text.c_str();
}

ik_motion* ik_motion_new(void) { return new ik_motion; }
void ik_motion_free(ik_motion* m) { delete m; }

void ik_motion_add(ik_motion* m, float dx, float dy, int64_t now_ns, int resample) {
  if (m) m->queue.Add(dx, dy, now_ns, resample != 0);
}

void ik_motion_take(ik_motion* m, int64_t now_ns, float* dx, float* dy) {
  float x = 0.0f, y = 0.0f;
  if (m) m->queue.Take(now_ns, &x, &y);
  if (dx) *dx = x;
  if (dy) *dy = y;
}

void ik_motion_clear(ik_motion* m) {
  if (m) m->queue.Clear();
}

void ik_turn_defaults(ik_turn* t) {
  if (!t) return;
  const mouse::Turn d;
  *t = {d.sensitivity, d.m_yaw, d.m_pitch, d.zoom_ratio, d.zoom_sensitivity, d.multiplier, d.scale_x, d.scale_y, 0};
}

void ik_turn_from_counts(const ik_turn* t, float dx, float dy, float* yaw_delta, float* pitch_delta) {
  mouse::Turn turn;
  if (t) {
    turn.sensitivity = t->sensitivity;
    turn.m_yaw = t->m_yaw;
    turn.m_pitch = t->m_pitch;
    turn.zoom_ratio = t->zoom_ratio;
    turn.zoom_sensitivity = t->zoom_sensitivity;
    turn.multiplier = t->multiplier;
    turn.scale_x = t->scale_x;
    turn.scale_y = t->scale_y;
    turn.invert = t->invert != 0;
  }
  float y = 0.0f, p = 0.0f;
  mouse::TurnFromCounts(turn, dx, dy, &y, &p);
  if (yaw_delta) *yaw_delta = y;
  if (pitch_delta) *pitch_delta = p;
}

double ik_zoom_ratio(double fov, double base_fov) { return mouse::ZoomRatio(fov, base_fov); }

// [input-port] 2026-11-24 other games' sensitivity scales
const char* ik_sens_scale_id(int scale) { return sensscale::Describe(sensscale::FromIndex(scale)).id; }
const char* ik_sens_scale_label(int scale) { return sensscale::Describe(sensscale::FromIndex(scale)).label; }
const char* ik_sens_scale_games(int scale) { return sensscale::Describe(sensscale::FromIndex(scale)).games; }
void ik_sens_scale_range(int scale, double* min, double* max, double* step) {
  const auto& i = sensscale::Describe(sensscale::FromIndex(scale));
  if (min) *min = i.min;
  if (max) *max = i.max;
  if (step) *step = i.step;
}
int ik_sens_scale_zoom(int scale, double* coefficient) {
  const auto& i = sensscale::Describe(sensscale::FromIndex(scale));
  if (coefficient) *coefficient = i.coefficient;
  return static_cast<int>(i.zoom);
}
int ik_sens_scale_from_id(const char* id) { return id ? static_cast<int>(sensscale::FromId(id)) : 0; }
double ik_sens_degrees_per_count(int scale, double value) {
  return sensscale::DegreesPerCount(sensscale::FromIndex(scale), value);
}
double ik_sens_to_source(int scale, double value) { return sensscale::ToSource(sensscale::FromIndex(scale), value); }
double ik_sens_from_source(int scale, double source) {
  return sensscale::FromSource(sensscale::FromIndex(scale), source);
}
double ik_sens_convert(int from_scale, int to_scale, double value) {
  return sensscale::Convert(sensscale::FromIndex(from_scale), sensscale::FromIndex(to_scale), value);
}
double ik_monitor_distance_ratio(double hip_vfov_deg, double zoom_vfov_deg, double coefficient) {
  return sensscale::MonitorDistanceRatio(hip_vfov_deg, zoom_vfov_deg, coefficient);
}
double ik_vertical_fov(double hfov_deg, double aspect) { return sensscale::VerticalFov(hfov_deg, aspect); }

}  // extern "C"
