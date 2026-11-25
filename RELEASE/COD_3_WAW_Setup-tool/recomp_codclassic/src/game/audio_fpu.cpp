// codclassic - the floating-point state of the game's audio callback. [acct2-uo]
//
// acct2-cod2 found (CoD2, 2026-11-07 15:17 and 19:27) that the SDK runs a game's audio callback, the client the
// game registers with XAudioRegisterRenderDriverClient, on a host thread of its own (the "Audio Worker") whose
// guest context never had its floating-point state set up: its copy is 0. The game's first vector code writes
// that copy back with the flush-to-zero bits added, which unmasks every SSE exception, and the next inexact
// conversion (or the SDK's XMA decoder opening under the callback) stops the program with SIGFPE.
//
// CoD Classic registers its callback through the XAudio wrapper at 0x82139BE8 (campaign) / 0x821480E0 (MP):
// (object r3, callback r4) -> XAudioRegisterRenderDriverClient({callback, object +0xC}, object +0x18). The
// wrapper is called through a vtable and the callback comes in r4, so instead of a fixed hook this file puts a
// guard in front of whatever callback the game registers: the SDK's function table entry for the callback's
// guest address is pointed at GuardedAudioCallback, which sets the context up the way a guest thread's is set up
// (FPSCRRegister::InitHost) and masks every SSE exception in the host's MXCSR (x86; nothing in the game relies on
// SSE traps), then runs the game's callback. Done before the registration, so the SDK sees the guard from the
// start. It can't be tried before the package is readable: the boot stops before the audio starts.

#if defined(__x86_64__) || defined(_M_X64)
#include <xmmintrin.h>
#endif

#include <atomic>
#include <cstdint>

#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/platform/fpscr.h>
#include <rex/ppc/func.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

#if defined(CODCLASSIC_MP)
#define CODCLASSIC_AUDIO_REGISTER sub_821480E0
#else
#define CODCLASSIC_AUDIO_REGISTER sub_82139BE8
#endif
#define CODCLASSIC_AUDIO_IMP_(name) __imp__##name
#define CODCLASSIC_AUDIO_IMP(name) CODCLASSIC_AUDIO_IMP_(name)

REX_EXTERN(CODCLASSIC_AUDIO_IMP(CODCLASSIC_AUDIO_REGISTER));  // (object r3, callback r4) -> HRESULT r3

namespace {

std::atomic<uint32_t> g_guarded_address{0};       // the callback's guest address, once guarded
std::atomic<PPCFunc*> g_game_callback{nullptr};    // the game's own translated callback

// True when the context's copy masks (x86) or disables (ARM) every exception, as InitHost leaves it.
bool ExceptionsMasked(uint32_t csr) {
  constexpr uint32_t mask = rex::platform::FPSCRPlatform::ExceptionMask;
#if defined(__x86_64__) || defined(_M_X64)
  return (csr & mask) == mask;
#else
  return (csr & mask) == 0;
#endif
}

void GuardedAudioCallback(PPCContext& ctx, uint8_t* base) {
  if (!ExceptionsMasked(ctx.fpscr.csr)) ctx.fpscr.InitHost();
#if defined(__x86_64__) || defined(_M_X64)
  _mm_setcsr(_mm_getcsr() | 0x1F80u);
#endif
  if (PPCFunc* callback = g_game_callback.load(std::memory_order_acquire)) callback(ctx, base);
}

void GuardCallback(uint32_t guest_address) {
  if (guest_address == 0 || g_guarded_address.load() == guest_address) return;
  PPCFunc* game_callback = rex::runtime::ResolveIndirectFunction(guest_address);
  if (!game_callback || game_callback == &GuardedAudioCallback) return;
  if (g_guarded_address.load() != 0) {
    REXLOG_WARN("codclassic: a second audio callback 0x{:08X} (0x{:08X} is guarded); left as it is",
                guest_address, g_guarded_address.load());
    return;
  }
  g_game_callback.store(game_callback, std::memory_order_release);
  if (REX_KERNEL_MEMORY()->SetFunction(guest_address, &GuardedAudioCallback)) {
    g_guarded_address.store(guest_address);
    REXLOG_INFO("codclassic: audio callback 0x{:08X} runs with its floating-point state set up", guest_address);
  } else {
    REXLOG_WARN("codclassic: couldn't guard the audio callback 0x{:08X} (outside the function tables)",
                guest_address);
  }
}

}  // namespace

REX_HOOK_RAW(CODCLASSIC_AUDIO_REGISTER) {
  GuardCallback(ctx.r4.u32);
  CODCLASSIC_AUDIO_IMP(CODCLASSIC_AUDIO_REGISTER)(ctx, base);
}
