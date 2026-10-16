// cod3 - the picture's colors (see color_output.h).

#include "game/color_output.h"

#include <atomic>
#include <string>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>

#include "game/guest.h"

REXCVAR_DEFINE_STRING(cod3_color_space, "display", "cod3/Display",
                      "Picture colors: display (matched to your display: sRGB, which your system maps to "
                      "the screen) or bt709 (the console on an HDTV: BT.709 shown with a TV's gamma 2.4)")
    .allowed({"display", "bt709"})
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace cod3 {

namespace {

// The game's "gamma ramp changed" flag (u8): at its next present the game
// sets its gamma ramp again (D3DDevice_SetGammaRamp, which asks for the
// display's gamma again), then clears it.
//   campaign:    0x8290F240, read at 0x821305DC (SetGammaRamp 0x822E94D0)
//   multiplayer: 0x82A8C0F0, read at 0x8214360C (SetGammaRamp 0x822B8218)
#if defined(COD3_MP)
constexpr uint32_t kGammaRampChanged = 0x82A8C0F0;
#else
constexpr uint32_t kGammaRampChanged = 0x8290F240;
#endif

std::atomic<bool> g_callback_registered{false};

void MarkGammaRampChanged() {
  auto* kernel_state = rex::system::kernel_state();
  uint8_t* base = kernel_state && kernel_state->memory() ? kernel_state->memory()->virtual_membase() : nullptr;
  if (base) cod3::guest::WriteU8(base, kGammaRampChanged, 1);
}

}  // namespace

void ApplyColorSpace() {
  const bool bt709 = REXCVAR_GET(cod3_color_space) == "bt709";
  // The display the game draws for: sRGB (1) or BT.709 (2) (SDK patch 0040).
  rex::cvar::SetFlagByName("kernel_display_gamma_type", bt709 ? "2" : "1");
  // How the GPU shows the result: as it is, or as an HDTV shows it. The
  // setting is the GPU plugin's, so it exists only once the plugin is loaded.
  if (rex::cvar::GetFlagInfo("display_output_transfer"))
    rex::cvar::SetFlagByName("display_output_transfer", bt709 ? "bt1886" : "none");
  else  // before the plugin loads (applied again after), or an SDK without patch 0040
    REXLOG_DEBUG("cod3: display_output_transfer isn't there yet");
  static int logged = -1;  // the last choice logged (Cod3App applies it twice at start)
  if (logged != int(bt709)) {
    logged = int(bt709);
    REXLOG_INFO("cod3: picture colors: {}",
                bt709 ? "BT.709 as on an HDTV (the game draws for BT.709, shown with a TV's gamma 2.4)"
                      : "matched to the display (the game draws for an sRGB display)");
  }
  if (!g_callback_registered.exchange(true)) {
    rex::cvar::RegisterChangeCallback("cod3_color_space", [](std::string_view, std::string_view) {
      ApplyColorSpace();
      MarkGammaRampChanged();  // the game builds its ramp for the new display at the next frame
    });
  }
}

}  // namespace cod3
