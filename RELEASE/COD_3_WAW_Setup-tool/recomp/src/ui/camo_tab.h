// [acct2-bro] 2026-11-22 cod3 - F2 > Game > Camos (Camo lane). The owner, 08:17: "the camo settings/gloves changes
// should be on a per-player and per gun basis, account for mid-game switches. Can be within f2 within game and a
// sub-tab." The rows only write cod3_camo, cod3_camo_strength and cod3_camo_choices (game/ngl_camo.h); the native
// renderer takes a change at its next frame.
#pragma once

namespace cod3::ui::camo_tab {

// The sub-tab's rows (inside ImGui's BeginTabItem("Camos")). Returns true when a setting changed (to be saved).
bool Draw();

}  // namespace cod3::ui::camo_tab
