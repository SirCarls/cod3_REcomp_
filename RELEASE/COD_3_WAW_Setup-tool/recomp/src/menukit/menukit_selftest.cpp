// menukit - a self-test with a pretend game: reads a menu file, walks it with every input, checks the layout.
// [waw-decomp/menus1] 2026-11-23. Built and run by hand on Linux and Windows (no game, no SDK):
//   menukit_selftest <path to recomp/menus/cod3/pause_options.toml>
// Prints one line per failed check and a summary; exit code 0 when every check passed.

#include <cstdio>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>

#include "menukit.h"

namespace {

int g_checks = 0, g_failed = 0;

void Check(bool ok, const char* what, int line) {
  ++g_checks;
  if (!ok) {
    ++g_failed;
    std::printf("FAILED line %d: %s\n", line, what);
  }
}
#define CHECK(x) Check((x), #x, __LINE__)

// Every row name the CoD3 menu file uses, by provider, as the CoD3 adapter will have them.
const std::map<std::string, std::set<std::string>> kRows = {
    {"fov", {"field_of_view", "fov_scale"}},
    {"video", {"battle_actions", "resolution", "frame_rate_limit", "vsync", "anti_aliasing", "shadows", "hd_textures",
               "depth_of_field", "motion_blur"}},
    {"port", {"skip_cutscenes", "hold_to_skip", "cutscene_bars", "use_hold_time", "left_handed_weapon",
              "vehicle_fov_scale", "keys_in_prompts", "keys_as_key_caps", "button_pictures", "button_colors",
              "raw_mouse", "invert_mouse", "mouse_sensitivity", "mouse_ads_scale", "even_mouse_turning",
              "master_volume", "voices", "text_language", "colors_in_names", "show_version", "split_screen_hud"}},
    {"controller", {"sensitivity_limit", "stick_dead_zone", "vibrate_with_keyboard"}},
    {"aim", {"crosshair", "crosshair_style", "crosshair_length", "crosshair_gap", "crosshair_thickness",
             "crosshair_color", "mouse_sensitivity", "sensitivity_for", "from_the_hip", "aiming_down_sights"}},
    {"movement", {"sprint", "sprint_button", "movement_style", "cod4_sprint", "cod4_speeds", "cod4_jump_tricks"}},
    {"test", {"a", "b", "note", "locked", "hidden", "go"}},
};

class FakeProvider : public menukit::RowProvider {
 public:
  explicit FakeProvider(std::string prefix) : prefix_(std::move(prefix)) {}
  std::string_view Prefix() const override { return prefix_; }
  bool View(std::string_view name, const menukit::Context& c, menukit::RowView* out) const override {
    const auto& names = kRows.at(prefix_);
    if (!names.count(std::string(name))) return false;
    const std::string key = prefix_ + ":" + std::string(name) + "#" + std::to_string(c.player);
    const auto it = values_.find(key);
    const int v = it == values_.end() ? 0 : it->second;
    out->label = std::string(name);
    out->value = std::to_string(v);
    out->note = "note of " + std::string(name);
    out->kind = name == "note" ? menukit::Kind::kNote
                : name == "go" ? menukit::Kind::kAction
                : (name.find("volume") != std::string_view::npos || name == "field_of_view") ? menukit::Kind::kSlider
                                                                                             : menukit::Kind::kChoice;
    out->locked = name == "locked";
    out->number = {0, 100, 5};
    out->number_value = v;
    return true;
  }
  bool Step(std::string_view name, const menukit::Context& c, int direction) override {
    values_[prefix_ + ":" + std::string(name) + "#" + std::to_string(c.player)] += direction;
    return true;
  }
  bool SetNumber(std::string_view name, const menukit::Context& c, double v) override {
    values_[prefix_ + ":" + std::string(name) + "#" + std::to_string(c.player)] = static_cast<int>(v);
    return true;
  }
  bool Activate(std::string_view, const menukit::Context&) override { return ++activated, true; }
  int Value(const std::string& key) const {
    const auto it = values_.find(key);
    return it == values_.end() ? 0 : it->second;
  }
  int activated = 0;

 private:
  std::string prefix_;
  std::map<std::string, int> values_;
};

class FakeAdapter : public menukit::Adapter {
 public:
  explicit FakeAdapter(std::string tag) : tag_(std::move(tag)) {
    for (const auto& p : kRows) providers_.emplace(p.first, FakeProvider(p.first));
  }
  std::string_view Game() const override { return "test"; }
  bool HasTag(std::string_view tag) const override { return tag == tag_; }
  std::string Text(std::string_view key) const override { return language + ":" + std::string(key); }
  menukit::RowProvider* Provider(std::string_view prefix) override {
    const auto it = providers_.find(std::string(prefix));
    return it == providers_.end() ? nullptr : &it->second;
  }
  bool Condition(std::string_view when, const menukit::Context&) const override { return when != "never"; }
  void Play(menukit::Sound s, const menukit::Context&) override { last_sound = s; }
  FakeProvider& Fake(const std::string& prefix) { return providers_.at(prefix); }
  std::string language = "en";
  menukit::Sound last_sound = menukit::Sound::kMove;

 private:
  std::string tag_;
  std::map<std::string, FakeProvider> providers_;
};

bool Overlap(const menukit::Box& a, const menukit::Box& b) {
  return a.x0 < b.x1 && b.x0 < a.x1 && a.y0 < b.y1 && b.y0 < a.y1;
}

void CheckLayout(const std::vector<menukit::DrawItem>& items, const menukit::Box& area) {
  for (size_t i = 0; i < items.size(); ++i) {
    const menukit::Box& b = items[i].box;
    CHECK(b.x0 >= area.x0 - 0.5f && b.x1 <= area.x1 + 0.5f && b.y0 >= area.y0 - 0.5f && b.y1 <= area.y1 + 0.5f);
    for (size_t j = i + 1; j < items.size(); ++j) {
      if (items[i].what == menukit::What::kLabel && items[j].what == menukit::What::kLabel)
        CHECK(!Overlap(b, items[j].box));
    }
  }
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::printf("usage: menukit_selftest <pause_options.toml>\n");
    return 2;
  }
  std::ifstream file(argv[1], std::ios::binary);
  std::stringstream text;
  text << file.rdbuf();
  CHECK(file.good() || file.eof());

  // 1. The CoD3 file in both programs: every row known, no problems.
  for (const char* tag : {"cod3", "cod3mp"}) {
    FakeAdapter adapter(tag);
    menukit::Menu menu;
    std::vector<std::string> problems;
    CHECK(menukit::ParseMenu(text.str(), "pause_options.toml", adapter, &menu, &problems));
    for (const std::string& p : problems) std::printf("problem (%s): %s\n", tag, p.c_str());
    CHECK(problems.empty());
    CHECK(menu.id == "pause_options");
    CHECK(menu.pages.size() == (std::string(tag) == "cod3" ? 8u : 6u));
  }

  // 2. Problems are reported with their line, and the rest of the file still reads.
  const char* broken =
      "[menu]\nid = \"t\"\ntitle = \"T\"\n"
      "[[page]]\nid = \"p\"\nlabel = \"P\"\nrows = [ \"test:a\", \"test:nope\", \"nope:a\", { row = \"test:b\" },\n"
      "  { row = \"test:hidden\", when = \"never\" }, { row = \"test:locked\" }, \"test:note\", \"test:go\",\n"
      "  { row = \"test:a\", only = \"other\" } ]\n"
      "oops\n"
      "[[page]]\nid = \"q\"\nlabel = \"Q\"\nrows = [ \"test:a\" ]\n";
  {
    FakeAdapter adapter("cod3");
    menukit::Menu menu;
    std::vector<std::string> problems;
    CHECK(menukit::ParseMenu(broken, "broken.toml", adapter, &menu, &problems));
    CHECK(problems.size() == 3);  // test:nope, nope:a, the line "oops"
    CHECK(menu.pages.size() == 2 && menu.pages[0].rows.size() == 6);
    bool line_named = false;
    for (const std::string& p : problems) line_named = line_named || p.find("broken.toml:10:") == 0;
    CHECK(line_named);

    // 3. Original look: one page at a time; the hidden row and the note are skipped; back leaves.
    menukit::Navigator nav;
    menukit::Context c;
    nav.Open(&menu, menukit::Look::kOriginal, adapter, c);
    CHECK(nav.row() == 0 && !nav.in_list());
    CHECK(nav.VisibleRows(adapter, c).size() == 5);
    CHECK(nav.Press(menukit::Input::kRight, adapter, c) == menukit::Outcome::kHandled);
    CHECK(adapter.Fake("test").Value("test:a#0") == 1);
    nav.Press(menukit::Input::kUp, adapter, c);  // wraps to the last focusable row: go
    CHECK(nav.row() == 5);
    nav.Press(menukit::Input::kAccept, adapter, c);
    CHECK(adapter.Fake("test").activated == 1);
    nav.Press(menukit::Input::kUp, adapter, c);  // skips the note
    CHECK(nav.row() == 3);
    nav.Press(menukit::Input::kRight, adapter, c);  // locked: refused
    CHECK(adapter.last_sound == menukit::Sound::kRefuse);
    nav.Press(menukit::Input::kPageNext, adapter, c);
    CHECK(nav.page() == 1 && nav.row() == 0);
    CHECK(nav.Press(menukit::Input::kBack, adapter, c) == menukit::Outcome::kLeave);

    // 4. Modern look: the list, then the rows; back twice leaves. Each player keeps their own values.
    menukit::Navigator p2;
    menukit::Context c2{1, nullptr};
    p2.Open(&menu, menukit::Look::kModern, adapter, c2);
    CHECK(p2.in_list());
    p2.Press(menukit::Input::kAccept, adapter, c2);
    CHECK(!p2.in_list() && p2.row() == 0);
    p2.Press(menukit::Input::kLeft, adapter, c2);
    CHECK(adapter.Fake("test").Value("test:a#1") == -1 && adapter.Fake("test").Value("test:a#0") == 1);
    CHECK(p2.Press(menukit::Input::kBack, adapter, c2) == menukit::Outcome::kHandled && p2.in_list());
    CHECK(p2.Press(menukit::Input::kBack, adapter, c2) == menukit::Outcome::kLeave);

    // 5. Layout: inside the box, labels apart, words asked every frame (a language change shows at once).
    const menukit::Box area{100, 50, 1380, 1010};
    p2.Press(menukit::Input::kAccept, adapter, c2);
    std::vector<menukit::DrawItem> items = menukit::LayOut(p2, adapter, c2, area);
    CheckLayout(items, area);
    CHECK(!items.empty() && items[0].what == menukit::What::kTitle && items[0].text == "en:T");
    adapter.language = "fr";
    items = menukit::LayOut(p2, adapter, c2, area);
    CHECK(items[0].text == "fr:T");
    // The pointer: the first row's label is hit at its middle.
    const menukit::DrawItem* label = nullptr;
    for (const auto& item : items)
      if (item.what == menukit::What::kLabel && !label) label = &item;
    CHECK(label != nullptr);
    if (label) {
      const menukit::DrawItem* hit =
          menukit::HitTest(items, (label->box.x0 + label->box.x1) / 2, (label->box.y0 + label->box.y1) / 2);
      CHECK(hit && hit->index == label->index && !hit->category);
    }
  }

  // 6. A long page scrolls and keeps the focused row in view; a slider click sets the value where clicked.
  {
    std::string longer = "[menu]\nid = \"l\"\n[[page]]\nid = \"p\"\nrows = [";
    for (int i = 0; i < 30; ++i) longer += "\"port:master_volume\", \"test:a\",\n";
    longer += "]\n";
    FakeAdapter adapter("cod3");
    menukit::Menu menu;
    std::vector<std::string> problems;
    CHECK(menukit::ParseMenu(longer, "long.toml", adapter, &menu, &problems) && problems.empty());
    menukit::Navigator nav;
    menukit::Context c;
    nav.Open(&menu, menukit::Look::kOriginal, adapter, c);
    const menukit::Box area{0, 0, 640, 480};
    for (int i = 0; i < 45; ++i) {
      nav.Press(menukit::Input::kDown, adapter, c);
      const auto items = menukit::LayOut(nav, adapter, c, area);
      CheckLayout(items, area);
      bool focus_shown = false;
      for (const auto& item : items) focus_shown = focus_shown || (item.what == menukit::What::kLabel && item.focused);
      CHECK(focus_shown);
    }
    nav.Press(menukit::Input::kDown, adapter, c);  // row 46: master volume (even rows)
    const auto items = menukit::LayOut(nav, adapter, c, area);
    const menukit::DrawItem* gauge = nullptr;
    for (const auto& item : items)
      if (item.what == menukit::What::kGauge && item.focused) gauge = &item;
    CHECK(gauge != nullptr);
    if (gauge) {
      nav.Click(gauge->index, false, 0, 0.5f, adapter, c);
      CHECK(adapter.Fake("port").Value("port:master_volume#0") == 50);
    }
  }

  std::printf("menukit selftest: %d checks, %d failed\n", g_checks, g_failed);
  return g_failed == 0 ? 0 : 1;
}
