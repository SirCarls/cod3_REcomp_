// cod3 - the picture's colors: matched to the player's display, or BT.709 as
// the console showed it on an HDTV (owner, 2026-10-15: "make sure it's
// rendering with bt.709 colorspace or make it match the user's colorspace").
//
// What decides the colors. The game draws in sRGB terms, and the console's
// Direct3D puts the display's tone curve on top of the game's gamma ramp,
// for the display the system reports (VdGetCurrentDisplayGamma): an sRGB
// (PC) display gets the game's ramp as it is, an HDTV gets BT.709's curve,
// which the TV then shows with its own gamma 2.4 (BT.1886). BT.709 and sRGB
// share their primaries and white point, so the tone curve is the only
// difference. The SDK reported an HDTV and nothing decoded it for a PC
// display, so the picture was neither the game's sRGB nor the TV's look.
//
// cod3_color_space (F2 > Video, the console, cod3.toml):
//   display - matched to the player's display: the game is told it draws for
//             a PC display (sRGB) and the picture goes out as sRGB, the color
//             space the system's color management (Windows' and the Linux
//             desktops') expects from an ordinary window and maps to the
//             screen, an HDR or wide-gamut one included. The default.
//   bt709   - the console on an HDTV: the game draws for BT.709 as on the
//             console, and the picture is shown the way an HDTV shows it
//             (BT.1886, gamma 2.4), encoded for the PC display. A little
//             darker and more contrasty than "display".
// Applied through two SDK settings (patch 0040): kernel_display_gamma_type
// (the display the game is told about) and display_output_transfer (BT.1886
// folded into the presented gamma ramp). A change shows at the next frame:
// the game sets its gamma ramp again (its own "ramp changed" flag).
//
// Compiled into both programs (cod3 and cod3mp).

#pragma once

namespace cod3 {

// Sets the two SDK settings from cod3_color_space. Call once the GPU plugin is
// loaded (display_output_transfer is the plugin's) and before the game sets
// its first gamma ramp; later changes of cod3_color_space apply by themselves.
void ApplyColorSpace();

}  // namespace cod3
