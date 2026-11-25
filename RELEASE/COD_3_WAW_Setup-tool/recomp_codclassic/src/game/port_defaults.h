// codclassic - the port's defaults for the SDK's own settings. [acct2-uo]

#pragma once

namespace codclassic::port_defaults {

// The keyboard as a controller (the SDK's keyboard pad), so a PC keyboard reaches the game's menus.
// Called from OnPreSetup, before the input drivers start.
void ApplyInputDefaults();

// The guest vblank at 1000 Hz, as the other games on this SDK run. Called from OnPostSetup, once the
// GPU plugin's settings exist.
void ApplyGpuDefaults();

}  // namespace codclassic::port_defaults
