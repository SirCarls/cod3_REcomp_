// cod3 - decomp trace build: the parts of game/decomp_trace.cpp that the
// evidence collectors (trace/evidence.cpp) use. Trace build only.

#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "game/trace/trace_abi.h"

namespace cod3::trace {

// What a known game function was asked (evidence.tsv "kind").
enum class Evidence : uint8_t {
  kPrint,           // a print format the caller passed (Com_Printf, Com_DPrintf, Com_Error)
  kDvar,            // a dvar name looked up (Cvar_FindVar)
  kDvarRegister,    // a dvar registered with its default (Cvar_Get)
  kDvarSet,         // a dvar name set (Cvar_Set2; the value is not kept)
  kFile,            // a game file opened or read (FS_*)
  kCommandRun,      // a console command run (Cmd_ExecuteString; its name only)
  kCommandHandler,  // the function that handles a console command (Cmd_Add*Command)
  kNameHash,        // a name hashed for a lookup (NameHash_Init: assets, banks, tags ...)
  kLevelSlot,       // the function behind a level interface slot (what level scripts call)
};
const char* EvidenceKindName(Evidence kind);

// Milliseconds since the trace started.
uint32_t NowMs();

// A guest return address as the slots keep it: with the running level
// library's id when it is outside the executable's code.
uint64_t CallSite(uint32_t return_address);

// Notes one piece of evidence. `site` is the caller (CallSite), `target` the
// function the evidence is about when it isn't the caller's (a command's
// handler, a level slot's function; 0 = the caller).
void NoteEvidence(Evidence kind, uint64_t site, uint32_t target, std::string_view value);

// What the game is doing, as functions first seen from now on are labeled.
// `level` empty = keep the current one.
void SetContext(std::string_view level, std::string_view state);
std::string CurrentLevel();

// The level library whose code runs now (Registry::current_level): set by
// name when a level load starts ("saint_lo"), and by the library's own code
// as it runs. 0 = none known.
void SetCurrentLevelModule(std::string_view name);
uint32_t CurrentLevelModule();

// Whether `address` starts a function of the executable's code.
bool IsMainFunction(uint32_t address);

// The collector for a function of the executable's code, or nullptr
// (trace/evidence.cpp).
EvidenceFn EvidenceFor(uint32_t address);

}  // namespace cod3::trace
