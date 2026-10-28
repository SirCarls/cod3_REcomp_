// cod3 - the game "rebooting" itself to get back to its main menu.
//
// Call of Duty 3 never returns to the front end inside a running game. Quit
// in the pause menu ("MEM_QUIT_WARNING", Yes), the end of a mission and a few
// other paths run the `reboot` command, SV_Reboot_f (0x82514710): it stores
// the player's control settings with XamLoaderSetLaunchData and launches
// default.xex again (XamLoaderLaunchTitle), and the new instance reads the
// launch data and starts at the main menu. On the console that restarts the
// title; the SDK only stops the game, so the whole program quit (owner,
// 2026-10-10: "quit when paused actually quits the whole application").
//
// Here the program starts itself again: the launch data goes to a file in
// userdata/, the new process gets its path (cod3_launch_data) and puts it
// back where XamLoaderGetLaunchData finds it before the game starts.
//
// [cod3mp] The same hand-over switches between the port's two programs, as
// the console switches between the disc's two executables: the campaign's
// Multiplayer item launches d:\codmp_xenonf.xex (SV_LoadMpExe_f, see
// game/multiplayer.h), which is the `cod3mp` program here, and multiplayer's
// way back (its `reboot` command, SV_Reboot_f 0x82531E20 in
// codmp_xenonf.xex) launches default.xex, the `cod3` program. The 664-byte
// controls block the game passes as launch data goes along both ways.

#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

namespace cod3 {

// The port's two programs, each built from one of the disc's executables.
enum class Program {
  kCampaign,     // default.xex: `cod3`
  kMultiplayer,  // codmp_xenonf.xex: `cod3mp`
};

// The program's executable name ("cod3", "cod3mp"; without .exe).
const char* ProgramName(Program program);

// True when that program's executable sits next to this one (in an AppImage:
// inside it).
bool ProgramInstalled(Program program);

// True when the game has asked to launch default.xex or codmp_xenonf.xex and
// that program is here (call after the game's main thread ended).
bool RelaunchRequested();

// The window as the player has it now (UI thread, every frame; four relaxed
// stores). The new instance starts the same way: fullscreen or windowed at
// this size, whatever the original command line said (the end of every
// mission, quitting to the menu and multiplayer all start a new instance).
// has_focus: with the SDK's start_in_background (patch 0063), the new window
// opens behind the window in use while this one isn't in use (a game left
// running in the background stays there) and in front while it's played.
void NoteWindowState(bool fullscreen, uint32_t logical_width, uint32_t logical_height,
                     bool has_focus);

// [claude-9b] start_in_background (SDK patch 0063) is a launch option for
// testing only (owner 2026-10-20: "it should just be an advanced launch option
// (trying to hide it from users and use for debugging)"): only the command
// line (--start_in_background=true: devrun.py, the setup tool's Extra
// arguments, a relaunch's WindowArgs) or the environment sets it. A value from
// a settings file (saved by F2 or the setup tool before 2.12.17) or, in
// multiplayer, from the campaign's cod3.toml is put back to off here; the
// next settings save then drops it from the file. Call in OnPreSetup, before
// the window opens.
void UseStartInBackgroundAsLaunchOptionOnly();

// Starts the program the game asked for (RelaunchRequested) with the game's
// launch data, keeping the command line. Returns false (and logs why) when
// it couldn't.
bool Relaunch(const std::filesystem::path& userdata_dir);

// In the new instance, before the game starts: hands the launch data from
// cod3_launch_data back to the game, then deletes the file. Only
// <userdata_dir>/launch_data.bin is accepted, and at most the console's 3 KB.
void RestoreLaunchData(const std::filesystem::path& userdata_dir);

// [claude-9b-think] 2026-10-28 v2 UNTESTED: a level command for the next instance ("spmap blkbrn"), which the
// next Relaunch passes on as its --cod3_autoexec, with --cod3_relaunch_by_level_guard=true (the old command line's
// one-off arguments, its --cod3_autoexec among them, are left out as before). The console's level commands, once a
// campaign level has run in this process, use it (ui/game_console.cpp): the game loads one mission per run (it
// restarts between missions: `reboot`), so `spmap blkbrn` there becomes the game's own `reboot` with `spmap blkbrn`
// as the new instance's first command. Any thread; an empty line clears it.
void SetStartupCommand(const std::string& line);

// [claude-9b-think] v2: this instance was started by that (--cod3_relaunch_by_level_guard): the console's guard
// doesn't restart it again in its first minute (no restart loop).
bool StartedByLevelGuard();

}  // namespace cod3
