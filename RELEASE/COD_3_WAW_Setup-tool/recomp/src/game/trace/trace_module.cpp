// cod3 - decomp trace build: the slow path of the tracing prologue.
//
// One copy in each module: the executable's generated code (COD3_TRACE_HOST)
// and every level library (decomp_trace.cmake adds it with the module's
// function table and COD3_TRACE_MODULE_NAME). It registers a function's slot
// with the game the first time the function runs and notes distinct callers
// on the sampled calls. Everything else happens in the executable
// (game/decomp_trace.cpp), which every module reaches through the exported
// cod3_decomp_trace_registry().

#include <atomic>
#include <cstdint>

#include <rex/image_info.h>
#include <rex/ppc.h>

#include "src/game/trace/trace_prologue.h"

#if !defined(COD3_TRACE_HOST)
#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#endif
#endif

#ifndef COD3_TRACE_MODULE_NAME
#error "COD3_TRACE_MODULE_NAME (the module's name) is set by src/game/trace/decomp_trace.cmake"
#endif

extern const rex::PPCImageInfo PPCImageConfig;

#if defined(COD3_TRACE_HOST)
extern "C" const cod3::trace::Registry* cod3_decomp_trace_registry();
#endif

namespace cod3::trace {

// This module's function table (trace_names_<module>.cpp, made at build time).
extern const FunctionName kModuleFunctions[];
extern const uint32_t kModuleFunctionCount;

namespace {

ModuleInfo& Module() {
  static ModuleInfo info{kAbiVersion,         COD3_TRACE_MODULE_NAME,   kModuleFunctions,
                         kModuleFunctionCount, PPCImageConfig.code_base, PPCImageConfig.code_size};
  return info;
}

const Registry* FindRegistry() {
  RegistryFn get = nullptr;
#if defined(COD3_TRACE_HOST)
  get = &cod3_decomp_trace_registry;
#elif defined(_WIN32)
  // The executable exports it (__declspec(dllexport) in game/decomp_trace.cpp).
  get = reinterpret_cast<RegistryFn>(
      reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(nullptr), kRegistrySymbol)));
#else
  // The executable exports it through a dynamic list (decomp_trace.cmake).
  get = reinterpret_cast<RegistryFn>(dlsym(RTLD_DEFAULT, kRegistrySymbol));
#endif
  const Registry* registry = get ? get() : nullptr;
  return registry && registry->abi_version == kAbiVersion ? registry : nullptr;
}

const Registry* GetRegistry() {
  static const Registry* const registry = FindRegistry();
  return registry;
}

// Keeps the first kMaxCallers distinct callers; a caller in level code
// carries the id of the level library running now (they share one address range).
void NoteCaller(Slot& slot, const Registry& registry, uint32_t return_address) {
  if (return_address == 0 || (return_address & 3)) return;  // a call from the port (guest::Call)
  uint64_t level = 0;
  if (return_address - registry.main_code_base >= registry.main_code_size)
    level = registry.current_level->load(std::memory_order_relaxed);
  const uint64_t key = (level << 32) | return_address;
  for (auto& caller : slot.callers) {
    uint64_t seen = caller.load(std::memory_order_relaxed);
    if (seen == key) return;
    if (seen == 0) {
      if (caller.compare_exchange_strong(seen, key, std::memory_order_relaxed) || seen == key) return;
    }
  }
  slot.more_callers.store(1, std::memory_order_relaxed);
}

}  // namespace

void OnSlowCall(Slot& slot, PPCContext& ctx, uint8_t* base, uint64_t calls) {
  const Registry* registry = GetRegistry();
  if (!registry) {
    // No game to report to (shouldn't happen): back to the plain count.
    slot.mask.store(~0u, std::memory_order_relaxed);
    return;
  }
  ModuleInfo& module = Module();
  if (slot.state.load(std::memory_order_acquire) != kSlotRegistered) {
    uint32_t state = kSlotNew;
    if (slot.state.compare_exchange_strong(state, kSlotRegistering, std::memory_order_acq_rel)) {
      slot.module = &module;
      registry->register_slot(&slot);
      slot.state.store(kSlotRegistered, std::memory_order_release);
    } else if (state != kSlotRegistered) {
      return;  // another thread is registering it right now
    }
  }
  // Level code that runs says which level library is loaded.
  if (module.code_base != registry->main_code_base) {
    if (const uint32_t id = module.id.load(std::memory_order_relaxed))
      registry->current_level->store(id, std::memory_order_relaxed);
  }
  NoteCaller(slot, *registry, static_cast<uint32_t>(ctx.lr));
  if (slot.evidence) {
    slot.evidence(slot, ctx, base);  // stays on the slow path for every call
  } else if (calls + 1 >= kFirstCalls) {
    slot.mask.store(kSampleMask, std::memory_order_relaxed);
  }
}

}  // namespace cod3::trace
