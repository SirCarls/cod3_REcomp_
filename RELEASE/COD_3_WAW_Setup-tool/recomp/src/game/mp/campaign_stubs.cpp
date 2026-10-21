// cod3mp: the parts of campaign-only modules that shared files call, here
// doing nothing. The modules themselves aren't in this program:
//  - game/cutscene_skip.cpp: multiplayer plays no films and has no level
//    intros (its film code has no callers);
//  - game/qte_actions.cpp, ui/qte_spinner.cpp: multiplayer has no battle
//    actions (the receiver classes exist, nothing starts one); the button
//    prompts' pointer and pad hooks call into it;
//  - game/aim_assist.cpp: the setting lives in each controller profile's
//    block in multiplayer, read by other code (later);
//  - game/voice_language.cpp: multiplayer registers its own sound banks
//    (later: its banks and region index differ).

#include <cstdint>
#include <optional>

#include "game/aim_assist.h"
#include "game/cutscene_skip.h"
#include "game/qte_actions.h"
#include "game/voice_language.h"
#include "ui/qte_spinner.h"

namespace cod3 {

CutsceneSkip& CutsceneSkip::Get() {
  static CutsceneSkip instance;
  return instance;
}
void CutsceneSkip::RequestSkip() {}
void CutsceneSkip::OnGuestFrame(PPCContext&, uint8_t*) {}
float CutsceneSkip::PromptAlpha() const { return 0.0f; }
float CutsceneSkip::CoverAlpha() const { return 0.0f; }

namespace qte {
void AddMouse(float, float) {}
void AddPointerMotion(float, float) {}  // [mp-menus] game/button_prompts.cpp's pointer watch
void ApplyToPad(uint8_t*, uint32_t, uint32_t) {}  // [mp-menus] and its pad state hook
bool MouseOwnsView() { return false; }
}  // namespace qte

std::optional<bool> AimAssistEnabled() { return std::nullopt; }
void SetAimAssist(bool) {}

namespace voice {
bool DrawSettingsRow() { return false; }
}  // namespace voice

namespace ui {
void DrawQteSpinner(ImGuiIO&) {}
bool DrawQteSettings() { return false; }
}  // namespace ui

}  // namespace cod3
