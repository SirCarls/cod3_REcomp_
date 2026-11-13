// cod3 - a test-only tour of the F2 settings window: see ui/f2_tour.h. [rexglue-native-f2] 2026-11-11 f2t1
//
// How a page is chosen: Dear ImGui keeps each tab bar's state (ImGuiContext::TabBars); setting a bar's
// NextSelectedTabId selects that tab the next time the bar is drawn, exactly as a click on it would. The F2 window's
// page scrolls in a child window called "##page" (SettingsWindow::OnDraw); SetScrollY moves it. Nothing in
// ui/hud_overlay.cpp is changed for this. Dear ImGui's internal header is the SDK's (1.92.5). Its context is
// reached through ImGui::GetCurrentContext() only, never the global GImGui, which a native Windows (MSVC ABI)
// link can't import from the runtime DLL (the same rule as game/keybinds/binds.cpp).

#include "ui/f2_tour.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

#if defined(_WIN32)
#include <process.h>  // _getpid
#else
#include <unistd.h>  // getpid
#endif

#include <imgui.h>
#include <imgui_internal.h>  // the tab bars' state and the windows' scroll

#include <rex/cvar.h>
#include <rex/logging.h>

REXCVAR_DEFINE_STRING(cod3_debug_f2_tour, "", "cod3/Debug",
                      "Test only: the F2 window opens by itself and shows its pages in turn, with one log line a view, "
                      "for pictures (ui/f2_tour.h). all, or pages such as VIDEO/Resolution,CROSSHAIR; empty: off");
REXCVAR_DEFINE_INT32(cod3_debug_f2_tour_start_s, 30, "cod3/Debug",
                     "Test only: with cod3_debug_f2_tour, the seconds after the game's window appears before the tour "
                     "starts");
REXCVAR_DEFINE_INT32(cod3_debug_f2_tour_ms, 1500, "cod3/Debug",
                     "Test only: with cod3_debug_f2_tour, how long each view stays on screen (milliseconds)");

namespace cod3::ui::f2tour {
namespace {

using Clock = std::chrono::steady_clock;

// Every page of the F2 window in its own order: the top tab, then the subtab. A page this program doesn't have
// (cod3mp has no Cutscenes, Aim or Movement; Server shows only on a host) is skipped after a few frames.
constexpr const char* kAllPages[] = {
    "VIDEO/Display",       "VIDEO/Resolution",    "VIDEO/Frame rate",      "VIDEO/Colors",
    "VIDEO/Quality",       "SOUND",               "GAME/View",             "GAME/Weapon",
    "GAME/Prompts",        "GAME/Cutscenes",      "GAME/Aim",              "GAME/Movement",
    "GAME/Server",         "GAME/Player",         "CROSSHAIR",             "CONTROLS/Mouse",
    "CONTROLS/Controller", "CONTROLS/Per weapon", "CONTROLS/Key bindings", "INTERFACE",
    "PERFORMANCE",         "EMULATION/GPU",       "EMULATION/GPU speed-ups", "EMULATION/Native code",
    "EMULATION/Audio engine", "CONSOLE/Console",  "CONSOLE/Problems",      "CONSOLE/Saved settings",
};

constexpr int kSettleFrames = 3;     // frames for a new tab or scroll to be drawn and laid out
constexpr int kGiveUpFrames = 30;    // a tab not found in this many frames: that page is skipped
constexpr int kMaxParts = 12;        // views of one page at most
constexpr float kPartStep = 0.85f;   // each view scrolls this share of the page's height (the rest overlaps)

enum class Step {
  kWaitStart,  // the start delay
  kOpen,       // the window was asked to open
  kTop,        // selecting the page's top tab
  kSub,        // selecting its subtab
  kMeasure,    // the page at its top: how many views it needs
  kShow,       // scrolling to the next view
  kHold,       // a view on screen, its log line written
  kRestore,    // the tabs selected before and every page at its top again
  kClose,      // closing the window
  kDone,
};

struct Tour {
  Step step = Step::kWaitStart;
  Clock::time_point since;          // when the current step started
  int frames = 0;                   // frames in the current step
  std::vector<std::string> pages;   // "TOP" or "TOP/Sub"
  size_t page = 0;
  int part = 0;
  int parts = 1;
  long pid = 0;
  std::unordered_map<ImGuiID, ImGuiID> selected_before;  // tab bar -> the tab it had when the tour first saw it
  std::vector<ImGuiID> pages_scrolled;                   // the page windows the tour scrolled
};

Tour* g_tour = nullptr;     // made on the first Tick when the setting is on
bool g_checked = false;     // the setting was read (once: the tour runs once a start)

void Go(Step step) {
  g_tour->step = step;
  g_tour->since = Clock::now();
  g_tour->frames = 0;
}

double MsInStep() {
  return std::chrono::duration<double, std::milli>(Clock::now() - g_tour->since).count();
}

// The visible part of a tab's label ("Problems (2)###problems" -> "Problems (2)").
std::string TabName(ImGuiTabBar* bar, const ImGuiTabItem& tab) {
  if (tab.NameOffset < 0) return {};
  const char* name = bar->TabsNames.c_str() + tab.NameOffset;
  const char* hash = std::strstr(name, "##");
  return hash ? std::string(name, hash) : std::string(name);
}

// Whether a tab bar was drawn in the last frame (one of its tabs was).
bool ShownLastFrame(ImGuiTabBar* bar) {
  const int last = ImGui::GetCurrentContext()->FrameCount - 1;
  for (const ImGuiTabItem& tab : bar->Tabs)
    if (tab.LastFrameVisible >= last) return true;
  return false;
}

// Each shown tab bar's selected tab, kept the first time the tour sees the bar (before it changes anything there).
void NoteSelections() {
  ImGuiContext& g = *ImGui::GetCurrentContext();
  for (int n = 0; n < g.TabBars.GetMapSize(); ++n) {
    ImGuiTabBar* bar = g.TabBars.TryGetMapData(n);
    if (!bar || !ShownLastFrame(bar) || g_tour->selected_before.count(bar->ID)) continue;
    g_tour->selected_before[bar->ID] = bar->SelectedTabId;
  }
}

// Selects the tab called `name` in the tab bar drawn last frame that has one: the exact name first, else the first
// whose name starts with it ("Problems" for "Problems (2)"). False when no shown bar has it.
bool SelectTab(const std::string& name) {
  ImGuiContext& g = *ImGui::GetCurrentContext();
  ImGuiTabBar* found_bar = nullptr;
  ImGuiID found_tab = 0;
  for (int pass = 0; pass < 2 && !found_bar; ++pass) {
    for (int n = 0; n < g.TabBars.GetMapSize() && !found_bar; ++n) {
      ImGuiTabBar* bar = g.TabBars.TryGetMapData(n);
      if (!bar || !ShownLastFrame(bar)) continue;
      for (const ImGuiTabItem& tab : bar->Tabs) {
        const std::string label = TabName(bar, tab);
        const bool match = pass == 0 ? label == name : label.rfind(name, 0) == 0;
        if (match) {
          found_bar = bar;
          found_tab = tab.ID;
          break;
        }
      }
    }
  }
  if (!found_bar) return false;
  found_bar->NextSelectedTabId = found_tab;
  return true;
}

// The F2 window (its id is "###cod3_settings" in SettingsWindow::OnDraw, whatever its title).
ImGuiWindow* SettingsImGuiWindow() { return ImGui::FindWindowByName("###cod3_settings"); }

// The F2 window's scrolling page drawn last frame (BeginChild("##page"); one such child per top tab).
ImGuiWindow* PageWindow() {
  ImGuiWindow* settings = SettingsImGuiWindow();
  if (!settings) return nullptr;
  for (ImGuiWindow* w : ImGui::GetCurrentContext()->Windows)
    if (w != settings && w->WasActive && w->RootWindow == settings && std::strstr(w->Name, "/##page_")) return w;
  return nullptr;
}

// The window that scrolls on the current page: the page itself, or, when the page fits and a column inside it scrolls
// by itself (CROSSHAIR's settings beside its preview, "##crosshair_settings"), that column, the one with the most to
// scroll. [rexglue-native-f2] 2026-11-11 f2t3
ImGuiWindow* ScrollWindow() {
  ImGuiWindow* page = PageWindow();
  if (!page || page->ScrollMax.y > 1.0f) return page;
  ImGuiWindow* best = page;
  for (ImGuiWindow* w : ImGui::GetCurrentContext()->Windows)
    if (w->WasActive && w->ParentWindow == page && w->ScrollMax.y > best->ScrollMax.y) best = w;
  return best;
}

void ScrollPage(ImGuiWindow* page, float y) {
  ImGui::SetScrollY(page, y);
  if (std::find(g_tour->pages_scrolled.begin(), g_tour->pages_scrolled.end(), page->ID) ==
      g_tour->pages_scrolled.end())
    g_tour->pages_scrolled.push_back(page->ID);
}

std::string TopOf(const std::string& page) { return page.substr(0, page.find('/')); }
std::string SubOf(const std::string& page) {
  const size_t slash = page.find('/');
  return slash == std::string::npos ? std::string() : page.substr(slash + 1);
}

void NextPage() {
  if (ImGuiWindow* page = PageWindow()) ScrollPage(page, 0.0f);  // each page left at its top again
  if (ImGuiWindow* inner = ScrollWindow()) ScrollPage(inner, 0.0f);  // and its scrolling column (f2t3)
  ++g_tour->page;
  Go(g_tour->page < g_tour->pages.size() ? Step::kTop : Step::kRestore);
}

// The page list from the setting: "all", or names separated by commas.
std::vector<std::string> ParsePages(const std::string& text) {
  std::vector<std::string> pages;
  if (text == "all") {
    for (const char* p : kAllPages) pages.emplace_back(p);
    return pages;
  }
  size_t start = 0;
  while (start <= text.size()) {
    size_t comma = text.find(',', start);
    if (comma == std::string::npos) comma = text.size();
    std::string page = text.substr(start, comma - start);
    while (!page.empty() && page.front() == ' ') page.erase(page.begin());
    while (!page.empty() && page.back() == ' ') page.pop_back();
    if (!page.empty()) pages.push_back(page);
    start = comma + 1;
  }
  return pages;
}

}  // namespace

void Tick(const std::function<void()>& toggle, bool open) {
  if (!g_checked) {
    g_checked = true;
    const std::string text = REXCVAR_GET(cod3_debug_f2_tour);
    if (text.empty()) return;  // off: never anything more
    g_tour = new Tour();       // lives until the program ends (a test setting, a few bytes)
    g_tour->pages = ParsePages(text);
#if defined(_WIN32)
    g_tour->pid = static_cast<long>(_getpid());
#else
    g_tour->pid = static_cast<long>(getpid());
#endif
    Go(Step::kWaitStart);
    REXLOG_INFO("cod3: f2 tour: {} pages, starting in {} s (pid {})", g_tour->pages.size(),
                REXCVAR_GET(cod3_debug_f2_tour_start_s), g_tour->pid);
  }
  if (!g_tour || g_tour->step == Step::kDone || !ImGui::GetCurrentContext()) return;
  ++g_tour->frames;
  Tour& t = *g_tour;
  // Closed by someone else while touring: stop.
  if (!open && t.step != Step::kWaitStart && t.step != Step::kOpen && t.step != Step::kClose) {
    REXLOG_INFO("cod3: f2 tour: the window was closed, the tour stops (pid {})", t.pid);
    Go(Step::kDone);
    return;
  }
  if (open && t.step != Step::kWaitStart) NoteSelections();
  switch (t.step) {
    case Step::kWaitStart:
      if (MsInStep() < 1000.0 * std::max(0, static_cast<int>(REXCVAR_GET(cod3_debug_f2_tour_start_s)))) return;
      if (!open) toggle();
      Go(Step::kOpen);
      return;
    case Step::kOpen:
      if (open && SettingsImGuiWindow() && SettingsImGuiWindow()->WasActive && t.frames > kSettleFrames) {
        const ImVec2 size = SettingsImGuiWindow()->Size;
        const ImVec2 screen = ImGui::GetIO().DisplaySize;
        REXLOG_INFO("cod3: f2 tour: start, window {:.0f}x{:.0f} on {:.0f}x{:.0f} (pid {})", size.x, size.y, screen.x,
                    screen.y, t.pid);
        Go(Step::kTop);
      } else if (MsInStep() > 10000.0) {
        REXLOG_INFO("cod3: f2 tour: the window didn't open, the tour stops (pid {})", t.pid);
        Go(Step::kDone);
      }
      return;
    case Step::kTop:
    case Step::kSub: {
      const std::string& page = t.pages[t.page];
      const std::string name = t.step == Step::kTop ? TopOf(page) : SubOf(page);
      if (t.step == Step::kSub && name.empty()) {
        Go(Step::kMeasure);
        return;
      }
      if (t.frames == 1 || t.frames % 5 == 0) {  // asked again now and then: a tab bar may appear a frame later
        if (SelectTab(name) && t.frames >= kSettleFrames) {
          Go(t.step == Step::kTop ? Step::kSub : Step::kMeasure);
          return;
        }
      }
      if (t.frames > kGiveUpFrames) {
        REXLOG_INFO("cod3: f2 tour: {} not found, skipped (pid {})", page, t.pid);
        NextPage();
      }
      return;
    }
    case Step::kMeasure: {
      ImGuiWindow* page = ScrollWindow();  // the page, or the column on it that scrolls (f2t3)
      if (t.frames == 1) {
        if (ImGuiWindow* settings = SettingsImGuiWindow()) ImGui::SetScrollY(settings, 0.0f);
        if (ImGuiWindow* whole = PageWindow()) ScrollPage(whole, 0.0f);
        if (page) ScrollPage(page, 0.0f);
        return;
      }
      if (t.frames < kSettleFrames + 2) return;
      t.parts = 1;
      if (page) {
        const float view = page->InnerRect.GetHeight();
        const float max = page->ScrollMax.y;
        if (view > 1.0f && max > 1.0f)
          t.parts = std::min(kMaxParts, 1 + static_cast<int>(std::ceil(max / (view * kPartStep))));
      }
      t.part = 0;
      Go(Step::kShow);
      return;
    }
    case Step::kShow: {
      ImGuiWindow* page = ScrollWindow();  // (f2t3)
      if (t.frames == 1 && page && t.part > 0) {
        const float view = page->InnerRect.GetHeight();
        ScrollPage(page, std::min(page->ScrollMax.y, t.part * view * kPartStep));
      }
      if (t.frames < kSettleFrames) return;
      REXLOG_INFO("cod3: f2 tour: {} part {} of {} (pid {})", t.pages[t.page], t.part + 1, t.parts, t.pid);
      Go(Step::kHold);
      return;
    }
    case Step::kHold:
      if (MsInStep() < std::max(200, static_cast<int>(REXCVAR_GET(cod3_debug_f2_tour_ms)))) return;
      if (++t.part < t.parts) {
        Go(Step::kShow);
      } else {
        NextPage();
      }
      return;
    case Step::kRestore: {
      if (t.frames == 1) {
        // The tabs selected before the tour, each applied the next time its bar is drawn (a subtab bar of a page
        // that isn't shown keeps it until that page is opened), and every page the tour scrolled at its top.
        ImGuiContext& g = *ImGui::GetCurrentContext();
        for (const auto& [bar_id, tab_id] : t.selected_before)
          if (ImGuiTabBar* bar = g.TabBars.GetByKey(bar_id)) bar->NextSelectedTabId = tab_id;
        for (ImGuiID id : t.pages_scrolled)
          if (ImGuiWindow* w = ImGui::FindWindowByID(id)) ImGui::SetScrollY(w, 0.0f);
        if (ImGuiWindow* settings = SettingsImGuiWindow()) ImGui::SetScrollY(settings, 0.0f);
      }
      if (t.frames < kSettleFrames + 2) return;
      Go(Step::kClose);
      if (open) toggle();
      return;
    }
    case Step::kClose:
      if (!open || MsInStep() > 5000.0) {
        REXLOG_INFO("cod3: f2 tour: done (pid {})", t.pid);
        Go(Step::kDone);
      }
      return;
    case Step::kDone:
      return;
  }
}

}  // namespace cod3::ui::f2tour
