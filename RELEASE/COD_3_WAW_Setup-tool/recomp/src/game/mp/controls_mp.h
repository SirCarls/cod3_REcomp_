// cod3mp: the player's keys, controls and crosshair as the campaign has them
// (controls_mp.cpp has the details).
//
//  - Grenade keys: the keyboard's Grenade and Smoke grenade rows (F2 >
//    Controls, G and 4) throw the frag and the special grenade also when
//    multiplayer's controller layout swaps them between the shoulders.
//  - Controls: the campaign's control settings, handed over when its
//    Multiplayer item starts this program, are kept when the profile's own
//    saved settings load afterwards.
//  - Crosshair: the campaign's choice (cg_drawCrosshair in its console
//    config) is multiplayer's too, and a change made here goes back to it;
//    with no choice yet, the first value the game has becomes it.

#pragma once

#include <filesystem>

namespace cod3::mpcontrols {

// Before the game starts (Cod3MpApp::OnPostSetup, after this program's own
// console config is loaded): the campaign's crosshair choice from its
// console config `campaign_config` (userdata/cod3_config.cfg), if it has one.
void TakeCampaignCrosshair(const std::filesystem::path& campaign_config);

// UI thread, every frame: a crosshair choice made in multiplayer is written
// back to the campaign's console config (a few times a second at most).
void Tick();
// UI thread, before the program ends or hands over: the same, at once.
void Flush();

}  // namespace cod3::mpcontrols
