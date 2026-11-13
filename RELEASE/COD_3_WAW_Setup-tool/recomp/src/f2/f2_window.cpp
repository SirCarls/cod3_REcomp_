// f2 - the F2 settings window shared by every game's port (see f2_window.h). [f2-menus]

#include "f2/f2_window.h"

#include <algorithm>

#include <imgui.h>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ui/imgui_drawer.h>
#include <rex/ui/ui_event.h>
#include <rex/ui/virtual_key.h>
#include <rex/ui/window.h>

#include "f2/f2_common.h"
#include "f2/f2_rows.h"
#include "f2/f2_theme.h"

namespace f2 {

// ---------------------------------------------------------------------------------------------------------------
// The window
// ---------------------------------------------------------------------------------------------------------------

SettingsWindow::SettingsWindow(rex::ui::ImGuiDrawer* drawer, WindowSpec spec, PadShortcut* pad,
                               std::function<void()> on_close)
    : ImGuiDialog(drawer), spec_(std::move(spec)), pad_(pad), on_close_(std::move(on_close)) {}

void SettingsWindow::Save() {
  try {
    rex::cvar::SaveConfig(spec_.config_path);
    status_ = "Saved to " + spec_.config_path.filename().string();
    context_.dirty = false;
    REXLOG_INFO("f2: settings saved to {}", spec_.config_path.filename().string());
  } catch (const std::exception& e) {
    status_ = std::string("Save failed: ") + e.what();
    REXLOG_WARN("f2: saving the settings failed: {}", e.what());
  }
}

void SettingsWindow::SaveIfDirty() {
  if (context_.dirty) Save();
}

bool SettingsWindow::ShowPage(const std::string& label) {
  for (int i = 0; i < static_cast<int>(spec_.pages.size()); ++i) {
    if (label == spec_.pages[i].label) {
      wanted_page_ = i;
      return true;
    }
  }
  return false;
}

void SettingsWindow::OnDraw(ImGuiIO& io) {
  if (!open_) return;
  ApplyThemeIfChanged();
  const float fs = ImGui::GetStyle().FontSizeBase;
  ImGui::SetNextWindowSize(ImVec2(std::min(fs * 48.0f, io.DisplaySize.x * 0.9f),
                                  std::min(fs * 40.0f, io.DisplaySize.y * 0.85f)),
                           ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_FirstUseEver,
                          ImVec2(0.5f, 0.5f));
  // The window takes the keyboard and the controller when it opens.
  if (first_draw_) ImGui::SetNextWindowFocus();
  first_draw_ = false;

  // LB / RB: the page before or after (f2_pad.h).
  if (const int step = pad_ ? pad_->TakePageStep() : 0; step && !spec_.pages.empty()) {
    const int n = static_cast<int>(spec_.pages.size());
    wanted_page_ = ((std::max(current_page_, 0) + step) % n + n) % n;
  }

  // The title (bold, capitals like the games' menus) is drawn by Begin, so the bold font is pushed around it only.
  const std::string title = spec_.title + "  \xC2\xB7  SETTINGS###" + spec_.id;
  ImGui::PushFont(BoldFont(), 0.0f);
  const bool shown = ImGui::Begin(title.c_str(), &open_, ImGuiWindowFlags_NoCollapse);
  ImGui::PopFont();
  if (shown) {
    // Tab labels in the bold font; their pages in the regular one.
    ImGui::PushFont(BoldFont(), 0.0f);
    if (ImGui::BeginTabBar("##tabs", ImGuiTabBarFlags_FittingPolicyScroll)) {
      for (int i = 0; i < static_cast<int>(spec_.pages.size()); ++i) {
        const Page& page = spec_.pages[i];
        const ImGuiTabItemFlags flags = i == wanted_page_ ? ImGuiTabItemFlags_SetSelected : 0;
        if (!ImGui::BeginTabItem(page.label, nullptr, flags)) continue;
        if (current_page_ != i) REXLOG_INFO("f2: page {}", page.label);  // for test runs
        current_page_ = i;
        ImGui::PopFont();
        // The page scrolls under the tabs; the Save row stays. Text wraps to the page.
        // [f2-menus] 2026-11-11 the Save row's real height (the spacing, the separator, the spacing, the button), as
        // in Call of Duty 3's window (rexglue-native-f2's review, item 1): the old reserve let the window overflow.
        const float save_row = ImGui::GetFrameHeight() + 2.0f * ImGui::GetStyle().ItemSpacing.y + 1.0f;
        ImGui::BeginChild("##page", ImVec2(0, -save_row));
        page.draw(context_);
        ImGui::EndChild();
        ImGui::PushFont(BoldFont(), 0.0f);
        ImGui::EndTabItem();
      }
      ImGui::EndTabBar();
    }
    wanted_page_ = -1;
    ImGui::PopFont();
    ImGui::Separator();
    if (ImGui::Button(context_.dirty ? "Save *" : "Save")) Save();
    ImGui::SameLine();
    if (context_.restart) ImGui::TextColored(CurrentColors().warn, "Restart the game to apply some changes.");
    if (!status_.empty()) {
      ImGui::SameLine();
      ImGui::TextDisabled("%s", status_.c_str());
    }
    ImGui::SameLine();
    ImGui::TextDisabled("  F2 or Back + Start (held) to close");
  }
  ImGui::End();
  if (context_.save_now) {
    context_.save_now = false;
    Save();
  }
  if (!open_) {
    SaveIfDirty();
    if (on_close_) on_close_();
  }
}

// ---------------------------------------------------------------------------------------------------------------
// The always-on part
// ---------------------------------------------------------------------------------------------------------------

Overlay::Overlay(rex::ui::ImGuiDrawer* drawer, rex::ui::Window* window, Options options)
    : ImGuiDialog(drawer), drawer_(drawer), game_window_(window), options_(std::move(options)) {
  // Above the ImGui drawer's own listener (z 64), as Call of Duty 3's F2 key (z 200), so the key never reaches a
  // text box or the game.
  if (game_window_) game_window_->AddInputListener(this, 200);
  display::SetGameWindow(game_window_);  // [f2-menus] 2026-11-11 the Display rows and "Match the screen"
}

Overlay::~Overlay() {
  display::SetGameWindow(nullptr);
  if (game_window_) game_window_->RemoveInputListener(this);
  SaveIfDirty();
  window_.reset();
}

void Overlay::SaveIfDirty() {
  if (window_) window_->SaveIfDirty();
}

void Overlay::Toggle(const char* why) {
  // Made and destroyed after the current frame, outside the drawer's loop over its dialogs.
  if (toggle_pending_) return;
  toggle_pending_ = true;
  auto toggle = [this, why, alive = std::weak_ptr<int>(alive_)]() {
    if (alive.expired()) return;
    toggle_pending_ = false;
    if (window_) {
      window_->SaveIfDirty();
      window_.reset();
      REXLOG_INFO("f2: settings window closed ({})", why);
      return;
    }
    window_ = std::make_unique<SettingsWindow>(drawer_, options_.spec, &pad_, [this]() {
      // Closed with its own [x] (it has saved already): destroyed after this frame.
      auto close = [this, alive = std::weak_ptr<int>(alive_)]() {
        if (alive.expired()) return;
        window_.reset();
        REXLOG_INFO("f2: settings window closed (its close button)");
      };
      if (options_.defer)
        options_.defer(close);
      else
        close();
    });
    REXLOG_INFO("f2: settings window opened ({})", why);
  };
  if (options_.defer)
    options_.defer(toggle);
  else
    toggle();
}

bool Overlay::GameGetsInput() const {
  if (window_) return false;
  if (!drawer_ || !drawer_->HasDialogs()) return true;
  // Typing into another Dear ImGui window (the SDK's own overlays) never reaches the game either.
  ImGuiIO& io = drawer_->GetIO();
  return !io.WantCaptureMouse && !io.WantTextInput;
}

void Overlay::OnKeyDown(rex::ui::KeyEvent& e) {
  if (e.virtual_key() != rex::ui::VirtualKey::kF2) return;
  e.set_handled(true);
  if (!e.prev_state()) Toggle("F2 key");  // a held key's repeats don't open and close it again
}

void Overlay::OnDraw(ImGuiIO& io) {
  if (options_.on_frame) options_.on_frame(*this);
  frame_cost::Tick();  // [f2-menus] 2026-11-11 the frame cost lines and log (f2_common.h), window open or not
  accel::Tick();       // [f2-menus] 2026-11-11 the sensitivity randomizer (after the frame cost lines: its line is under them)
  if (display::Tick()) {  // [f2-menus] 2026-11-11 "Match the screen" picked another scale for the next start
    try {
      rex::cvar::SaveConfig(options_.spec.config_path);
      REXLOG_INFO("f2: settings saved (the render scale for this screen changed)");
    } catch (const std::exception& e) {
      REXLOG_WARN("f2: saving the settings failed: {}", e.what());
    }
  }
  const char* setting = options_.pad_shortcut_setting;
  const bool enabled = !setting || !*setting || GetBool(setting);
  if (pad_.Poll(enabled)) Toggle("controller: Back + Start held");
  pad_.FeedNavigation(io, window_ != nullptr);

  // While Back + Start fill (past a quick press, which only pauses the game), a small hint at the top of the window.
  const float progress = pad_.Progress();
  if (progress > 0.2f) {
    ApplyThemeIfChanged();
    const ThemeColors& c = CurrentColors();
    const char* text = window_ ? "Keep holding to close the settings" : "Keep holding to open the settings";
    const ImVec2 size = ImGui::CalcTextSize(text);
    const ImVec2 pad(12.0f, 8.0f);
    const ImVec2 p0((io.DisplaySize.x - size.x) * 0.5f - pad.x, 24.0f);
    const ImVec2 p1(p0.x + size.x + pad.x * 2.0f, p0.y + size.y + pad.y * 2.0f + 4.0f);
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    dl->AddRectFilled(p0, p1, ImGui::GetColorU32(ImVec4(c.window.x, c.window.y, c.window.z, 0.9f)), 3.0f);
    dl->AddText(ImVec2(p0.x + pad.x, p0.y + pad.y), ImGui::GetColorU32(c.text), text);
    dl->AddRectFilled(ImVec2(p0.x, p1.y - 4.0f), ImVec2(p0.x + (p1.x - p0.x) * progress, p1.y),
                      ImGui::GetColorU32(c.accent));
  }
}

}  // namespace f2
