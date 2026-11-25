// codclassic - the imports from x360shared.xdl: forwarded to the recompiled module when it is loaded, a
// placeholder until then. [acct2-uo]
//
// Both of CoD Classic's programs import functions by ordinal from x360shared.xdl, an Xbox DLL in the game's
// Xbox Live Arcade package (17 in default.xex, 13 in cod_mp.xex). On the 360 the loader loaded that DLL before
// the game ran and patched each import's thunk with the DLL's function. On this SDK (read 2026-11-07 22:21):
//   - the codegen resolves imports against the kernel's export tables only, so it names each of these thunks
//     sub_<thunk address> and emits no body (the link fails without this file);
//   - a [[modules]] DLL is recompiled into its own library and found by its guest path when it is loaded
//     (KernelState::LoadUserModule, as XexLoadImage does), but nothing loads an imported DLL by itself.
// So each thunk is defined here. On its first call it loads x360shared.xdl (once for the whole program, with
// its entry point run, as the 360's loader would), looks the ordinal up in the module's export table and
// calls the recompiled function there from then on. While the module isn't there (the package's files not
// copied yet, or not yet in the manifest: dev/agents/acct2-uo/PLAN.md, step 2c) the thunk logs its first calls
// with the ordinal and returns 0, so the program still runs as far as it can. The thunks' ordinals are
// x360-family's (Ghidra's x360shared_ord_<N> at each thunk address).
//
// The loaded image still holds each thunk as the XEX file has it (run acct2-uo-r6, 2026-11-07 23:10): its first
// word is the import record, (1 << 24) | (library index << 16) | ordinal, then the thunk record
// (2 << 24) | (library << 16) | ordinal, then mtctr r11 / bctr. All four thunks logged there match the table
// below. So CODCLASSIC_X360SHARED_THUNK(sub_<address>) needs no ordinal: it reads it from the thunk on the first
// call. That is for the game modules' own x360shared imports (step 2c), whose thunk addresses the link errors
// give.

// The same file is also compiled into the package's game modules that import from x360shared.xdl
// (cgame and ui, campaign and multiplayer; CMakeLists.txt adds it with CODCLASSIC_MODULE_<NAME>): their
// thunks are undefined symbols in each module library, and dlopen refuses a library with any (run
// acct2-uo-r10, 2026-11-07 23:39: "libcodclassic_ui_sp.so: undefined symbol: sub_8904BA28", then the game's
// own "VM_Create on UI failed"). Each module's thunks read their ordinal from the import record on the first
// call. The module and the program share the one loaded x360shared.xdl: whichever copy is called first loads
// it, and the others take it from the kernel (SharedModule below).

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>

#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc/func.h>
#include <rex/system/kernel_state.h>
#include <rex/system/user_module.h>

namespace {

// The module's name as the XEX files import it, which is also its guest_path in the manifests.
constexpr const char* kSharedModuleName = "x360shared.xdl";
// The path it is loaded by. LoadUserModule hooks up the recompiled library only when
// FindRecompiledModule(<the path, normalized>) finds the entry the generated module_registry.cpp made, and the
// normalized path drops only a "device:" prefix. A bare name is joined to the running program's folder first,
// which the SDK resolves to \Device\Harddisk0\Partition1\, so it never matched (runs acct2-uo-r7/r8,
// 2026-11-07: the module's image loaded without its code, and its entry point was "not in function table").
// game:\ is the same folder (the SDK maps it to game_data_root).
constexpr const char* kSharedModulePath = "game:\\x360shared.xdl";
// The recompiled module's library, as module_registry.cpp names it (<project>_x360shared); used only if the
// entry is missing.
#if defined(CODCLASSIC_MP)
constexpr const char* kSharedLibrary = "codclassicmp_x360shared";
#else
constexpr const char* kSharedLibrary = "codclassic_x360shared";
#endif

// Which copy of this file this is, for the log.
#if defined(CODCLASSIC_MODULE_CGAME_SP)
constexpr const char* kThisCopy = "cgame_sp.xdl";
#elif defined(CODCLASSIC_MODULE_UI_SP)
constexpr const char* kThisCopy = "ui_sp.xdl";
#elif defined(CODCLASSIC_MODULE_CGAME_MP)
constexpr const char* kThisCopy = "cgame_mp.xdl";
#elif defined(CODCLASSIC_MODULE_UI_MP)
constexpr const char* kThisCopy = "ui_mp.xdl";
#else
constexpr const char* kThisCopy = "the program";
#endif

rex::system::UserModule* g_shared_module = nullptr;  // loaded once; never unloaded
std::once_flag g_shared_module_once;

rex::system::UserModule* SharedModule() {
  std::call_once(g_shared_module_once, [] {
    auto* kernel = REX_KERNEL_STATE();
    if (!kernel) return;
    // Each copy of this file (the program's and each game module's) has its own g_shared_module. The SDK
    // refuses to load a recompiled module a second time ("Recompiled module 'x360shared.xdl' already loaded;
    // refusing duplicate load"; run acct2-uo-r12, 2026-11-08 00:07, where ui_sp.xdl's imports all returned 0).
    // So a copy called after the DLL is loaded takes the loaded one, whose entry point has already run.
    if (auto existing = kernel->GetModule(kSharedModulePath, /*user_only=*/true)) {
      // user_only: only user modules are searched, so this is a UserModule. The reference is kept.
      g_shared_module = static_cast<rex::system::UserModule*>(existing.release());
      REXLOG_INFO("codclassic: {} is already loaded; the imports of {} go to it", kSharedModuleName, kThisCopy);
      return;
    }
    if (auto info = kernel->FindRecompiledModule(kSharedModuleName)) {
      REXLOG_INFO("codclassic: {} is registered as recompiled (library {})", kSharedModuleName,
                  info->shared_lib_name);
    } else {
      REXLOG_WARN("codclassic: {} was not registered as recompiled; registering it (library {})",
                  kSharedModuleName, kSharedLibrary);
      kernel->RegisterRecompiledModule(kSharedModuleName, kSharedModuleName, kSharedLibrary);
    }
    auto loaded = kernel->LoadUserModule(kSharedModulePath, /*call_entry=*/true);
    // Kept for the program's life (the reference is never released, as XexLoadImage keeps its own).
    g_shared_module = loaded ? loaded.release() : nullptr;
    if (g_shared_module)
      REXLOG_INFO("codclassic: {} loaded by {}; its imports go to the recompiled module", kSharedModuleName,
                  kThisCopy);
    else
      REXLOG_WARN("codclassic: {} not loaded by {} (not in the game's files or not recompiled); its imports "
                  "return 0", kSharedModuleName, kThisCopy);
  });
  return g_shared_module;
}

// The recompiled function behind one ordinal, or null.
PPCFunc* ResolveSharedImport(int ordinal) {
  auto* module = SharedModule();
  if (!module) return nullptr;
  const uint32_t guest_address = module->GetProcAddressByOrdinal(static_cast<uint16_t>(ordinal), 0);
  PPCFunc* function = guest_address ? rex::runtime::ResolveIndirectFunction(guest_address) : nullptr;
  if (function)
    REXLOG_INFO("codclassic: x360shared ordinal {} -> 0x{:08X}", ordinal, guest_address);
  else
    REXLOG_WARN("codclassic: x360shared ordinal {} not found in the module (address 0x{:08X})", ordinal,
                guest_address);
  return function;
}

uint32_t ThunkAddress(const char* thunk) {
  const char* hex = std::strchr(thunk, '_');
  return hex ? static_cast<uint32_t>(std::strtoul(hex + 1, nullptr, 16)) : 0;
}

uint32_t GuestWord(const uint8_t* base, uint32_t address) {
  uint32_t word;
  std::memcpy(&word, base + address, sizeof(word));
  return __builtin_bswap32(word);
}

// The ordinal in a thunk's import record, or -1 when its first word isn't one.
int OrdinalAtThunk(const char* thunk, const uint8_t* base) {
  const uint32_t address = ThunkAddress(thunk);
  if (address == 0) return -1;
  const uint32_t record = GuestWord(base, address);
  return (record >> 24) == 1 ? static_cast<int>(record & 0xFFFF) : -1;
}

// What the loaded image holds at a thunk (four big-endian words), logged once per thunk. On the 360 the thunk's
// first word, before the loader patched it, named the import ((1 << 24) | (library << 16) | ordinal); if the
// SDK leaves it there, the game modules' own x360shared thunks can be mapped at run time instead of by hand
// (step 2c). Diagnostics only.
void LogThunkWords(const char* thunk, const uint8_t* base) {
  const uint32_t address = ThunkAddress(thunk);
  if (address == 0) return;
  uint32_t words[4];
  for (int i = 0; i < 4; ++i) words[i] = GuestWord(base, address + 4u * i);
  REXLOG_INFO("codclassic: thunk {} at 0x{:08X} holds {:08X} {:08X} {:08X} {:08X}", thunk, address, words[0],
              words[1], words[2], words[3]);
}

// Logs the 1st, 2nd, 3rd, then every power-of-two call of one unresolved import, so a lock taken every frame
// doesn't flood the log.
void NoteMissingImport(const char* thunk, int ordinal, std::atomic<uint32_t>& calls, const uint8_t* base) {
  const uint32_t count = calls.fetch_add(1, std::memory_order_relaxed) + 1;
  if (count == 1) LogThunkWords(thunk, base);
  if (count <= 3 || (count & (count - 1)) == 0) {
    REXKRNL_WARN("codclassic: x360shared ordinal {} called ({} via {}) but the module isn't loaded; "
                 "returning 0", ordinal, count, thunk);
  }
}

// ordinal: the table's, or -1 to read it from the thunk (CODCLASSIC_X360SHARED_THUNK).
void CallSharedImport(const char* thunk, int ordinal, std::atomic<PPCFunc*>& target,
                      std::atomic<bool>& resolved, std::atomic<uint32_t>& calls, PPCContext& ctx,
                      uint8_t* base) {
  if (ordinal < 0) ordinal = OrdinalAtThunk(thunk, base);
  if (!resolved.load(std::memory_order_acquire)) {
    const int recorded = OrdinalAtThunk(thunk, base);
    if (recorded >= 0 && recorded != ordinal)
      REXLOG_WARN("codclassic: {} is ordinal {} in the table but {} in its import record", thunk, ordinal,
                  recorded);
    target.store(ordinal >= 0 ? ResolveSharedImport(ordinal) : nullptr, std::memory_order_release);
    resolved.store(true, std::memory_order_release);
  }
  if (PPCFunc* function = target.load(std::memory_order_acquire)) {
    function(ctx, base);
    return;
  }
  NoteMissingImport(thunk, ordinal, calls, base);
  ctx.r3.u64 = 0;
}

}  // namespace

#define CODCLASSIC_X360SHARED_IMPORT(thunk, ordinal)                          \
  extern "C" REX_FUNC(thunk) {                                                \
    static std::atomic<PPCFunc*> target{nullptr};                            \
    static std::atomic<bool> resolved{false};                                 \
    static std::atomic<uint32_t> calls{0};                                    \
    CallSharedImport(#thunk, ordinal, target, resolved, calls, ctx, base);   \
  }

// A thunk whose ordinal is read from its import record on the first call (for the game modules' thunks).
#define CODCLASSIC_X360SHARED_THUNK(thunk) CODCLASSIC_X360SHARED_IMPORT(thunk, -1)

#if defined(CODCLASSIC_MODULE_CGAME_SP)
// cgame_sp.xdl: its 10 x360shared thunks (the library's undefined symbols, b17).
CODCLASSIC_X360SHARED_THUNK(sub_85076078)
CODCLASSIC_X360SHARED_THUNK(sub_85076088)
CODCLASSIC_X360SHARED_THUNK(sub_85076098)
CODCLASSIC_X360SHARED_THUNK(sub_850760A8)
CODCLASSIC_X360SHARED_THUNK(sub_850760B8)
CODCLASSIC_X360SHARED_THUNK(sub_850760C8)
CODCLASSIC_X360SHARED_THUNK(sub_850760D8)
CODCLASSIC_X360SHARED_THUNK(sub_850760E8)
CODCLASSIC_X360SHARED_THUNK(sub_850760F8)
CODCLASSIC_X360SHARED_THUNK(sub_85076108)
#elif defined(CODCLASSIC_MODULE_UI_SP)
// ui_sp.xdl: 12 thunks.
CODCLASSIC_X360SHARED_THUNK(sub_8904B978)
CODCLASSIC_X360SHARED_THUNK(sub_8904B988)
CODCLASSIC_X360SHARED_THUNK(sub_8904B998)
CODCLASSIC_X360SHARED_THUNK(sub_8904B9A8)
CODCLASSIC_X360SHARED_THUNK(sub_8904B9B8)
CODCLASSIC_X360SHARED_THUNK(sub_8904B9C8)
CODCLASSIC_X360SHARED_THUNK(sub_8904B9D8)
CODCLASSIC_X360SHARED_THUNK(sub_8904B9E8)
CODCLASSIC_X360SHARED_THUNK(sub_8904B9F8)
CODCLASSIC_X360SHARED_THUNK(sub_8904BA08)
CODCLASSIC_X360SHARED_THUNK(sub_8904BA18)
CODCLASSIC_X360SHARED_THUNK(sub_8904BA28)
#elif defined(CODCLASSIC_MODULE_CGAME_MP)
// cgame_mp.xdl: 6 thunks.
CODCLASSIC_X360SHARED_THUNK(sub_85082328)
CODCLASSIC_X360SHARED_THUNK(sub_85082338)
CODCLASSIC_X360SHARED_THUNK(sub_85082348)
CODCLASSIC_X360SHARED_THUNK(sub_85082358)
CODCLASSIC_X360SHARED_THUNK(sub_85082368)
CODCLASSIC_X360SHARED_THUNK(sub_85082378)
#elif defined(CODCLASSIC_MODULE_UI_MP)
// ui_mp.xdl: 6 thunks.
CODCLASSIC_X360SHARED_THUNK(sub_89051A4C)
CODCLASSIC_X360SHARED_THUNK(sub_89051A5C)
CODCLASSIC_X360SHARED_THUNK(sub_89051A6C)
CODCLASSIC_X360SHARED_THUNK(sub_89051A7C)
CODCLASSIC_X360SHARED_THUNK(sub_89051A8C)
CODCLASSIC_X360SHARED_THUNK(sub_89051A9C)
#elif defined(CODCLASSIC_MP)
// cod_mp.xex: 13 imports.
CODCLASSIC_X360SHARED_IMPORT(sub_8225FAD4, 3)
CODCLASSIC_X360SHARED_IMPORT(sub_8225FAE4, 8)
CODCLASSIC_X360SHARED_IMPORT(sub_8225FAF4, 7)
CODCLASSIC_X360SHARED_IMPORT(sub_8225FB04, 4)
CODCLASSIC_X360SHARED_IMPORT(sub_8225FB14, 27)
CODCLASSIC_X360SHARED_IMPORT(sub_8225FB24, 9)
CODCLASSIC_X360SHARED_IMPORT(sub_8225FB34, 16)
CODCLASSIC_X360SHARED_IMPORT(sub_8225FB44, 12)
CODCLASSIC_X360SHARED_IMPORT(sub_8225FB54, 10)
CODCLASSIC_X360SHARED_IMPORT(sub_8225FB64, 29)
CODCLASSIC_X360SHARED_IMPORT(sub_8225FB74, 11)
CODCLASSIC_X360SHARED_IMPORT(sub_8225FB84, 1)
CODCLASSIC_X360SHARED_IMPORT(sub_8225FB94, 28)
#else
// default.xex: 17 imports.
CODCLASSIC_X360SHARED_IMPORT(sub_82251B94, 18)
CODCLASSIC_X360SHARED_IMPORT(sub_82251BA4, 12)
CODCLASSIC_X360SHARED_IMPORT(sub_82251BB4, 19)
CODCLASSIC_X360SHARED_IMPORT(sub_82251BC4, 3)
CODCLASSIC_X360SHARED_IMPORT(sub_82251BD4, 8)
CODCLASSIC_X360SHARED_IMPORT(sub_82251BE4, 7)
CODCLASSIC_X360SHARED_IMPORT(sub_82251BF4, 4)
CODCLASSIC_X360SHARED_IMPORT(sub_82251C04, 28)
CODCLASSIC_X360SHARED_IMPORT(sub_82251C14, 27)
CODCLASSIC_X360SHARED_IMPORT(sub_82251C24, 6)
CODCLASSIC_X360SHARED_IMPORT(sub_82251C34, 9)
CODCLASSIC_X360SHARED_IMPORT(sub_82251C44, 16)
CODCLASSIC_X360SHARED_IMPORT(sub_82251C54, 10)
CODCLASSIC_X360SHARED_IMPORT(sub_82251C64, 29)
CODCLASSIC_X360SHARED_IMPORT(sub_82251C74, 11)
CODCLASSIC_X360SHARED_IMPORT(sub_82251C84, 1)
CODCLASSIC_X360SHARED_IMPORT(sub_82251C94, 20)
#endif
