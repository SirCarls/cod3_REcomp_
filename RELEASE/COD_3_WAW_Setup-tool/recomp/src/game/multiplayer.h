// cod3 - what happens when the player picks Multiplayer.
//
// On the Xbox 360, Call of Duty 3's multiplayer is a separate program,
// d:\codmp_xenonf.xex. The single-player game hands over to it with the
// console command `load_mp_exe` (SV_LoadMpExe_f, 0x825145D8), which three
// places run:
//   - the main menu's Multiplayer item (MainMenu::vfunc_18 at 0x824C40B8,
//     case 1, through Cmd_ExecuteString 0x82468720),
//   - accepting an Xbox Live game invite (0x823FAA80 and 0x823FBBB0),
//   - typing load_mp_exe in the console.
// SV_LoadMpExe_f shuts the client down, stores the player's control settings
// (664 bytes at addr::controls::kSettings) with XamLoaderSetLaunchData and
// launches the
// multiplayer program through XLaunchNewImage (0x82343928) ->
// XamLoaderLaunchTitle, which stops the game for good.
//
// [cod3mp] The multiplayer program is `cod3mp`, codmp_xenonf.xex recompiled
// next to this one (recomp/mp/, docs/MULTIPLAYER.md). When it is installed
// and the disc's multiplayer files are there (MultiplayerAvailable),
// SV_LoadMpExe_f runs as on the console: it hands over the launch data and
// asks for d:\codmp_xenonf.xex, the request arrives in
// Cod3App::OnGuestThreadExit like the game's reboot to its main menu does,
// and game/relaunch.h starts cod3mp with that launch data. Otherwise the
// command is replaced (multiplayer.cpp): the game's own message box says what
// is missing and the menu stays where it was.

#pragma once

#include <filesystem>
#include <string>

namespace cod3::mp {

// The game's data folder (assets/), from Cod3App::OnConfigurePaths: where
// MultiplayerAvailable looks for the multiplayer files.
void SetGameDataRoot(const std::filesystem::path& root);

// True when picking Multiplayer can start cod3mp: its program is next to this
// one and the game data has the multiplayer files (mp\global.cod).
bool MultiplayerAvailable();

// HOOK POINTS for the cheat policy (game/cheat_guard.h). In the single-player
// program they are stubs: no match, this machine is the host (a match never
// runs here). The multiplayer program (cod3mp, docs/MULTIPLAYER.md) is to
// answer them from the game's session: sv_running and the Demonware
// bdSession host flag.
// True while a multiplayer match is running on this machine.
bool IsMatchRunning();
// True when this machine hosts the running match (or there is no match).
bool IsHost();
// Tells every player in the match a short line ("Cheats enabled by the
// host"): it goes into each machine's HUD announcement column
// (ui/announcement_feed.h) and its console. Without a match it shows here
// only. Text from another machine is shown as plain text, never run.
void Announce(const std::string& text);

}  // namespace cod3::mp
