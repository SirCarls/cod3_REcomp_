// cod3 - decomp trace build: what every traced module shares with the game.
//
// Only in the trace build (CMake option COD3_DECOMP_TRACE; docs/DECOMP_TRACE.md).
// Every recompiled function gets one Slot, a static in its prologue
// (trace_prologue.h). Its first kFirstCalls calls, and after that one call in
// kSampleMask + 1, take the module's slow path (trace_module.cpp): it
// registers the slot with the game once and notes the caller. The game
// (game/decomp_trace.cpp) keeps the list of seen functions and writes it out.
//
// The executable and every level library use this layout, and every
// generated file includes it: a change means a full rebuild of the trace
// build. Change kAbiVersion with it.

#pragma once

#include <atomic>
#include <cstdint>

struct PPCContext;

namespace cod3::trace {

inline constexpr uint32_t kAbiVersion = 1;

// Distinct callers kept per function.
inline constexpr int kMaxCallers = 8;
// The first calls of a function all take the slow path (caller, context);
// after that one call in kSampleMask + 1 does, so a function called from
// many places still shows several of them.
inline constexpr uint32_t kFirstCalls = 16;
inline constexpr uint32_t kSampleMask = 1023;

// Slot::state
inline constexpr uint32_t kSlotNew = 0;
inline constexpr uint32_t kSlotRegistering = 1;
inline constexpr uint32_t kSlotRegistered = 2;

// One function of a module's function table (generated at build time from
// generated/<module>/cod3_init.cpp; sorted by address).
struct FunctionName {
  uint32_t address;
  const char* name;
};

// One module: the executable's code ("default") or a level library.
struct ModuleInfo {
  uint32_t abi_version;
  const char* name;
  const FunctionName* functions;
  uint32_t function_count;
  uint32_t code_base;
  uint32_t code_size;
  std::atomic<uint32_t> id{0};  // given by the game at the module's first registration
};

struct Slot;

// Run on every call of the few functions the game collects evidence from
// (its dvar, file, command and loading functions); set when the slot registers.
using EvidenceFn = void (*)(Slot& slot, PPCContext& ctx, uint8_t* base);

struct Slot {
  const char* name;                              // __func__: "__imp__<name>"
  std::atomic<uint64_t> calls{0};                // approximate: a plain load and store, no atomic add
  std::atomic<uint32_t> mask{0};                 // the slow path runs when (calls & mask) == 0
  std::atomic<uint32_t> state{kSlotNew};
  // Distinct callers: (level module id << 32) | guest return address (ctx.lr).
  // The id tells the level libraries apart (they all load at one address);
  // it is 0 for a caller in the executable's code.
  std::atomic<uint64_t> callers[kMaxCallers]{};
  std::atomic<uint32_t> more_callers{0};         // 1: more distinct callers than kMaxCallers

  // Set by the game when the slot registers, before state is kSlotRegistered.
  ModuleInfo* module = nullptr;
  uint32_t address = 0;                          // guest address; 0 = not in the function table
  uint32_t first_ms = 0;                         // first call, in ms since the trace started
  uint32_t first_context = 0;                    // what the game was doing then (decomp_trace.cpp)
  EvidenceFn evidence = nullptr;
  Slot* next = nullptr;                          // the game's list of seen functions

  constexpr explicit Slot(const char* function_name) : name(function_name) {}
};

// What the executable hands each module.
struct Registry {
  uint32_t abi_version;
  void (*register_slot)(Slot* slot);             // once per slot, any thread
  std::atomic<uint32_t>* current_level;          // id of the level library that runs now, 0 = unknown
  uint32_t main_code_base;                       // the executable's guest code: callers there
  uint32_t main_code_size;                       // carry level id 0
};

// The executable exports `extern "C" const Registry* cod3_decomp_trace_registry()`.
inline constexpr char kRegistrySymbol[] = "cod3_decomp_trace_registry";
using RegistryFn = const Registry* (*)();

}  // namespace cod3::trace
