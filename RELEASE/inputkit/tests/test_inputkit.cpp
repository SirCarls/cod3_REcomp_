// inputkit's own tests (no game): key names, the bind model with a small table, the controller binds, the mouse's
// motion queue and turn, the weapon groups' text and the dead zone. Call of Duty 3's own test of its full table,
// dev/agents/keybinds/tests/test_binds.cpp, runs on top of this library too.
//   cmake -S inputkit -B build -DINPUTKIT_BUILD_TESTS=ON && cmake --build build && ctest --test-dir build
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "inputkit/binds.h"
#include "inputkit/keys.h"
#include "inputkit/mouse.h"
#include "inputkit/pad_binds.h"
#include "inputkit/stick.h"
#include "inputkit/weapon_sens.h"

using namespace inputkit;

static int g_fail = 0;
static int g_checks = 0;
#define CHECK(x)                                               \
  do {                                                         \
    ++g_checks;                                                \
    if (!(x)) {                                                \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); \
      ++g_fail;                                                \
    }                                                          \
  } while (0)

static void TestKeys() {
  CHECK(ParseKey("b") == kKeyB && ParseKey("SHIFT") == kKeyShift && ParseKey("Left Shift") == kKeyShift);
  CHECK(ParseKey("Right Ctrl") == kKeyCtrl && ParseKey("Keypad 5") == ParseKey("KP_5"));
  CHECK(ParseKey("Keypad +") == ParseKey("KP_PLUS") && !IsCombination("Keypad +"));
  CHECK(ParseKey("Shift+Up") == kNoKey && IsCombination("Ctrl+W"));
  CHECK(KeyName(kMouse2) == "MOUSE2" && PromptName(kKeyEscape) == "ESC" && PromptName(ParseKey("Numpad5")) == "NUM5");
  for (KeyCode c = 1; c < kKeyCodeCount; ++c) {
    const std::string n = KeyName(c);
    if (!n.empty()) CHECK(ParseKey(n) == c);
  }
  // SDL3's numbering.
  CHECK(KeyFromSdlScancode(4) == 'A' && KeyFromSdlScancode(29) == 'Z');
  CHECK(KeyFromSdlScancode(30) == '1' && KeyFromSdlScancode(39) == '0');
  CHECK(KeyFromSdlScancode(44) == kKeySpace && KeyFromSdlScancode(225) == kKeyShift && KeyFromSdlScancode(229) == kKeyShift);
  CHECK(KeyFromSdlScancode(58) == kKeyF1 && KeyFromSdlScancode(82) == kKeyUp && KeyFromSdlScancode(53) == kKeyBacktick);
  CHECK(KeyFromSdlScancode(98) == ParseKey("KP_0") && KeyFromSdlScancode(91) == ParseKey("KP_3"));
  CHECK(KeyFromSdlScancode(1000) == kNoKey);
  CHECK(KeyFromSdlMouseButton(1) == kMouse1 && KeyFromSdlMouseButton(2) == kMouse3 && KeyFromSdlMouseButton(3) == kMouse2);
}

namespace {
constexpr binds::ActionSpec kActions[] = {
    {"fire", "Fire", 0, {kMouse1, kNoKey}},
    {"aim", "Aim down the sights", 0, {kMouse2, kNoKey}},
    {"melee", "Melee", 0, {'V', kMouse3}},
    {"frag", "Frag grenade", 0, {'G', kNoKey}},
    {"smoke", "Smoke grenade", 0, {'4', kNoKey}},
    {"binoculars", "Binoculars", 0, {kKeyB, kNoKey}},
    {"jump", "Jump", 0, {kKeySpace, kNoKey}},
    {"tank_gun", "Tank: machine gun", 1, {kMouse2, kNoKey}},
    {"veh_gas", "Vehicle: gas", 1, {'W', kNoKey}},
};
constexpr int kCount = int(sizeof(kActions) / sizeof(kActions[0]));
const binds::Table kTable{kActions, kCount, "test_binds"};
}  // namespace

static void TestBinds() {
  using namespace binds;
  KeyRow keys[kCount];
  Defaults(kTable, keys);
  CHECK(IsDefault(kTable, keys) && Serialize(kTable, keys).empty());
  CHECK(ActionById(kTable, " MELEE ") == 2 && ActionById(kTable, "nope") == -1);
  std::vector<std::string> notes;
  Parse(kTable, "", keys, &notes);
  CHECK(IsDefault(kTable, keys) && notes.empty());
  // Unknown ids and keys, reserved keys, combinations: dropped with a note each.
  Parse(kTable, "binoculars=Shift+Up,N;bogus=K;frag=nosuch,ESCAPE,`;smoke=N;exec foo", keys, &notes);
  CHECK(keys[5][0] == 'N' && keys[4][0] == 'N');  // a key may have several actions
  CHECK(keys[3][0] == kNoKey);                     // frag listed with nothing usable: unbound
  CHECK(notes.size() == 6);
  // Defaults of actions the text doesn't list stay, unless an action of their kind has the key.
  notes.clear();
  Parse(kTable, "fire=MOUSE2", keys, &notes);
  CHECK(keys[0][0] == kMouse2 && keys[1][0] == kNoKey);  // aim's MOUSE2 went to fire
  CHECK(keys[7][0] == kMouse2);                          // the vehicle kind keeps its own
  // Round trip.
  const std::string text = Serialize(kTable, keys);
  KeyRow again[kCount];
  Parse(kTable, text, again, nullptr);
  CHECK(Same(kTable, keys, again));
  // Three actions a key at most.
  Defaults(kTable, keys);
  CHECK(Assign(kTable, keys, 3, 0, kKeyB) == "B also does Binoculars");
  CHECK(Assign(kTable, keys, 4, 0, kKeyB) == "B also does Frag grenade, Binoculars");
  bool ok = true;
  const std::string refused = Assign(kTable, keys, 6, 0, kKeyB, &ok);
  CHECK(!ok && refused.find("already does 3 things") != std::string::npos && keys[6][0] == kKeySpace);
  // AssignText.
  Defaults(kTable, keys);
  std::string note;
  CHECK(AssignText(kTable, keys, 5, "N, MOUSE4", &note) && note.empty() && keys[5][0] == 'N' && keys[5][1] == kMouse4);
  CHECK(!AssignText(kTable, keys, 5, "A,B,C", &note) && note == "Two keys at most.");
  CHECK(!AssignText(kTable, keys, 5, "`", &note) && keys[5][0] == 'N');
  CHECK(AssignText(kTable, keys, 5, "", &note) && keys[5][0] == kNoKey && KeysText(keys, 5).empty());
  // The older moving rule, by kind.
  Defaults(kTable, keys);
  CHECK(AssignMoving(kTable, keys, 3, 0, 'V') == "V was Melee; moved here" && keys[2][0] == kMouse3);
  CHECK(AssignMoving(kTable, keys, 8, 0, kMouse2) == "MOUSE2 was Tank: machine gun; moved here" && keys[1][0] == kMouse2);
  // 4 KB at most, control characters dropped.
  notes.clear();
  Parse(kTable, std::string(5000, 'x'), keys, &notes);
  CHECK(!notes.empty() && notes[0].find("test_binds is longer") == 0);
  notes.clear();
  Parse(kTable, std::string("binoculars=N\nquit"), keys, &notes);
  CHECK(keys[5][0] == kKeyB && notes.size() == 1);
}

namespace {
const padbinds::ButtonSpec kButtons[] = {
    {"a", "A", 212, true}, {"b", "B", 213, true}, {"x", "X", 211, true}, {"start", "START", 27, false, "START stays the pause menu's"},
};
bool NotVehicle(int a) { return kActions[a].kind == 0; }
const padbinds::Pad kPad{kButtons, 4, &kTable, NotVehicle};
}  // namespace

static void TestPadBinds() {
  using namespace padbinds;
  std::vector<std::string> notes;
  PadSet set = Parse(kPad, "a=jump,melee;x=;start=fire;b=tank_gun;a=fire", &notes);
  CHECK(set.custom[0] && set.binds[0].act[0] == 6 && set.binds[0].act[1] == 2 && set.binds[0].Count() == 2);
  CHECK(set.custom[2] && set.binds[2].Count() == 0);
  CHECK(!set.custom[3]);
  // START, tank_gun, a twice
  CHECK(notes.size() == 3 && notes[0] == "START stays the pause menu's; its entry is ignored");
  CHECK(Serialize(kPad, set) == "a=jump,melee;b=;x=");
  Layout layout;
  layout.binds[1].act[0] = 0;  // the layout's B fires
  PadSet s2;
  const std::string note = Assign(kPad, s2, layout, 0, 0, 0);  // fire onto A
  CHECK(s2.binds[0].act[0] == 0 && s2.custom[1] && s2.binds[1].Count() == 0);
  CHECK(note == "Fire was on B; B does nothing now");
  Reset(kPad, s2, layout, 1);  // B back to the layout: fire leaves A
  CHECK(!s2.custom[1] && s2.binds[0].Count() == 0);
  CHECK(Assign(kPad, s2, layout, 3, 0, 0).empty() && !s2.custom[3]);  // START never changes
  const std::vector<int> choices = Choices(kPad);
  CHECK(choices.size() == 8 && choices[0] == kNone);
}

static void TestMouse() {
  using namespace mouse;
  MotionQueue q;
  float dx = 0, dy = 0;
  // Resample off: a take gets everything.
  q.Add(10, -4, 1'000'000, false);
  q.Take(1'000'001, &dx, &dy);
  CHECK(dx == 10 && dy == -4);
  // Resample on: batches every 4 ms; a take gets the share up to now less the batch interval, all of it in the end.
  float total = 0;
  int64_t t = 0;
  for (int i = 0; i < 200; ++i) {
    t += 4'000'000;
    q.Add(4, 0, t, true);
    if (i % 3 == 0) {
      q.Take(t, &dx, &dy);
      total += dx;
      CHECK(dx >= 0 && dx <= 16.001f);
    }
  }
  q.Take(t + 20'000'000, &dx, &dy);
  total += dx;
  CHECK(std::fabs(total - 800.0f) < 0.01f);
  CHECK(q.BatchIntervalNs() > 3'900'000 && q.BatchIntervalNs() < 4'100'000);
  q.Add(5, 5, t + 30'000'000, true);
  q.Clear();
  q.Take(t + 90'000'000, &dx, &dy);
  CHECK(dx == 0 && dy == 0);
  // Many batches never taken: the newest 256 stay.
  for (int i = 0; i < 1000; ++i) q.Add(1, 0, t + 100'000'000 + int64_t(i) * 1'000'000, true);
  q.Take(t + 10'000'000'000LL, &dx, &dy);
  CHECK(dx == 256.0f);
  // The turn: World at War's 0.022 degrees a count at sensitivity 1.
  Turn turn;
  float yaw = 0, pitch = 0;
  TurnFromCounts(turn, 10, 10, &yaw, &pitch);
  CHECK(std::fabs(yaw + 1.1f) < 1e-5f && std::fabs(pitch - 1.1f) < 1e-5f);
  turn.invert = true;
  turn.zoom_ratio = 0.5;
  turn.zoom_sensitivity = 2.0;
  TurnFromCounts(turn, 10, 10, &yaw, &pitch);
  CHECK(std::fabs(yaw + 1.1f) < 1e-5f && std::fabs(pitch + 1.1f) < 1e-5f);
  CHECK(ZoomRatio(32.5, 65) == 0.5 && ZoomRatio(65, 0) == 1.0 && ZoomRatio(1, 65) == 0.05 && ZoomRatio(80, 65) == 1.0);
}

static void TestWeaponSens() {
  using namespace weaponsens;
  static const char* const kKeys[] = {"rifle", "smg", "sniper"};
  const Groups groups{kKeys, 3};
  Table t = Parse(groups, "smg=80/60, sniper=300/-5 bogus=50 rifle=x rifle=70");
  CHECK(t.pct[1][0] == 80 && t.pct[1][1] == 60 && t.pct[2][0] == 200 && t.pct[2][1] == 0 && t.pct[0][0] == 70);
  CHECK(Format(groups, t) == "rifle=70 smg=80/60 sniper=200/0");
  CHECK(std::fabs(t.Multiplier(1, 0.5f) - 0.7) < 1e-9 && t.Multiplier(-1, 0) == 1.0);
  CHECK(Table().AllDefault(groups) && Format(groups, Table()).empty());
}

static void TestStick() {
  int x = 3000, y = 0;
  inputkit_radial_dead_zone(&x, &y, 0.2f);
  CHECK(x == 0 && y == 0);
  x = 32767;
  y = 0;
  inputkit_radial_dead_zone(&x, &y, 0.2f);
  CHECK(x >= 32760 && y == 0);
  x = 16000;
  y = 16000;
  inputkit_radial_dead_zone(&x, &y, 0.0f);
  CHECK(x == 16000 && y == 16000);
}

int main() {
  TestKeys();
  TestBinds();
  TestPadBinds();
  TestMouse();
  TestWeaponSens();
  TestStick();
  std::printf("inputkit: %d checks, %d failed\n", g_checks, g_fail);
  return g_fail ? 1 : 0;
}
