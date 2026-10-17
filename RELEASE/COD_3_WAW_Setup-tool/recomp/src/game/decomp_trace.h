// cod3 - the decomp trace build: which game functions run, when, who calls
// them, and what they do (docs/DECOMP_TRACE.md).
//
// Only in the trace build (CMake option COD3_DECOMP_TRACE, built in its own
// folder by the setup tool's --trace-build). There every recompiled function
// notes its first call and its callers (src/game/trace/), and this part of the
// game writes it all to userdata/decomp_trace/<session>/ every minute and at
// exit:
//   functions.tsv  every function that ran: module, guest address, name,
//                  first seen (time, level, what the game was doing, the
//                  player's marker), approximate calls, up to 8 callers
//   calls.tsv      the same callers, one line each
//   evidence.tsv   what known game functions were asked by each caller:
//                  print formats, dvar names, files, commands, command
//                  handlers, hashed names, level interface slots
//   markers.tsv    the player's notes ("planted the bomb"): F8 or trace_mark
//   modules.tsv    functions seen per module
// tools/trace/merge_traces.py turns any number of these folders into
// coverage and naming candidates.
//
// In normal builds every call below is an empty inline function.

#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

struct PPCContext;

namespace rex::ui {
class ImGuiDrawer;
class Window;
}  // namespace rex::ui

namespace cod3::trace {

#if defined(COD3_DECOMP_TRACE)

inline constexpr bool kEnabled = true;

// Once at start (Cod3App::ConstructRuntime): makes the session folder under
// userdata/decomp_trace/ and starts the writer.
void Start(const std::filesystem::path& user_data_dir);
// UI thread (Cod3App::OnCreateDialogs): F8 opens a one-line box for a marker.
void AttachMarkerKey(rex::ui::Window* window, rex::ui::ImGuiDrawer* drawer);
// Any thread, before the process ends (Cod3App::SaveAll): writes the files.
void Flush();
// The ~ console (ui/game_console.cpp), lower-cased command and its words:
// trace_mark <words>, trace_status, trace_write. True when it was one of them.
bool HandleConsoleCommand(const std::string& command, const std::vector<std::string>& args);
// The print hooks (game/print_hooks.cpp): the game printed the format at
// guest address `format`; `constant` = it is the game's own text (read-only
// image data), so it may be kept.
void NotePrint(PPCContext& ctx, uint8_t* base, uint32_t format, bool constant);

#else

inline constexpr bool kEnabled = false;

inline void Start(const std::filesystem::path&) {}
inline void AttachMarkerKey(rex::ui::Window*, rex::ui::ImGuiDrawer*) {}
inline void Flush() {}
inline bool HandleConsoleCommand(const std::string&, const std::vector<std::string>&) { return false; }
inline void NotePrint(PPCContext&, uint8_t*, uint32_t, bool) {}

#endif

}  // namespace cod3::trace
