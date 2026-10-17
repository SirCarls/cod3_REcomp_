// cod3 - decomp trace build: the prologue of every recompiled function.
//
// The trace build (CMake option COD3_DECOMP_TRACE, src/game/trace/
// decomp_trace.cmake) adds this header to the precompiled header of every
// generated-code target, after the codegen's cod3_pch.h. So
// REX_FUNC_PROLOGUE(), the first statement of every recompiled function,
// becomes the one below. Normal builds never include this file: their
// generated code and cod3_pch.h are byte for byte what they are without it.
//
// Per call the prologue adds a plain load and store of the function's call
// count and one test; the first calls and then one in 1024 go to the
// module's slow path (trace_module.cpp).

#pragma once

#include <cstddef>
#include <cstdint>

#include "trace_abi.h"

#if !defined(__clang__) && !defined(__GNUC__)
#error "The decomp trace build needs clang or GCC (it uses __builtin_assume / attributes)"
#endif

#if defined(_WIN32)
#define COD3_TRACE_MODULE_LOCAL
#else
#define COD3_TRACE_MODULE_LOCAL __attribute__((visibility("hidden")))
#endif

namespace cod3::trace {

// The module's slow path (trace_module.cpp, one copy in each module).
COD3_TRACE_MODULE_LOCAL __attribute__((cold, noinline)) void OnSlowCall(Slot& slot, PPCContext& ctx,
                                                                       uint8_t* base, uint64_t calls);

// The register save and restore helpers (__savegprlr_14 ...) run inside
// other functions' own prologues and are not traced.
consteval bool Traced(const char* function) {
  constexpr char kHelperPrefix[] = "__imp____";
  for (int i = 0; kHelperPrefix[i]; ++i)
    if (function[i] != kHelperPrefix[i]) return true;
  return false;
}

__attribute__((always_inline)) inline void Hit(Slot& slot, PPCContext& ctx, uint8_t* base) {
  const uint64_t calls = slot.calls.load(std::memory_order_relaxed);
  slot.calls.store(calls + 1, std::memory_order_relaxed);
  if ((calls & slot.mask.load(std::memory_order_relaxed)) == 0) [[unlikely]]
    OnSlowCall(slot, ctx, base, calls);
}

}  // namespace cod3::trace

// The codegen's own prologue only tells the compiler that guest memory is
// 32-byte aligned; that stays.
#if defined(__clang__)
#define COD3_TRACE_ASSUME_ALIGNED_BASE() __builtin_assume(((size_t)base & 0x1F) == 0)
#else
#define COD3_TRACE_ASSUME_ALIGNED_BASE() \
  do {                                   \
    if (((size_t)base & 0x1F) != 0)      \
      __builtin_unreachable();           \
  } while (0)
#endif

#undef REX_FUNC_PROLOGUE
#define REX_FUNC_PROLOGUE()                                             \
  COD3_TRACE_ASSUME_ALIGNED_BASE();                                     \
  if constexpr (::cod3::trace::Traced(__func__)) {                      \
    static constinit ::cod3::trace::Slot cod3_trace_slot{__func__};     \
    ::cod3::trace::Hit(cod3_trace_slot, ctx, base);                     \
  }                                                                     \
  static_assert(true)
