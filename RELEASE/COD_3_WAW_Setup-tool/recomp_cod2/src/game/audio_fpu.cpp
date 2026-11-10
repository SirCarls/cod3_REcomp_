// cod2 - the floating-point state of the game's audio callback. [acct2-cod2]
//
// The SDK runs the game's audio callback (the client the game registers with
// XAudioRegisterRenderDriverClient) on a host thread of its own, the "Audio
// Worker". A guest thread's context takes the host's MXCSR, with every
// floating-point exception masked, when the thread starts (XThread::Execute,
// FPSCRRegister::InitHost); the audio worker's guest context never does, so
// its copy is 0. The callback's first vector code turns flush-to-zero on by
// writing that copy back with the flush bits added (0x8040), which unmasks
// every SSE exception, and the next inexact conversion stops the program with
// SIGFPE (first seen 2026-11-07 15:17, 0.4 s into the first run with the
// disc's files: cvtdq2ps in 0x820D1E98, under the callback 0x820C5A90).
//
// So before the callback runs, a context whose copy has the exceptions
// unmasked takes the host's state the way a guest thread's does. The general
// fix belongs in the SDK (a host thread's context set up before it calls
// guest code); until then, this hook on CoD2's callback.
//
// Second case (2026-11-07 19:27, cod2mp's loopback pair, both copies; gdb run
// 20261107-192935-cod2mp-gdb): SIGFPE on the same Audio Worker, this time in
// the SDK's own XMA decoder: the game's audio code under the callback
// (0x820C68C0 -> ... -> 0x820C2C28) calls XMAEnableContext, the SDK opens its
// decoder there (avcodec_open2 -> ff_mdct_init -> init_ff_cos_tabs -> cos), and
// the host's MXCSR had the exceptions unmasked. So on x86 the callback and
// 0x820C2C28 also mask every SSE exception in the host's MXCSR before the game's
// code runs (nothing in the game relies on SSE traps). Multiplayer only so far:
// the campaign's tour (27 levels) never hit it.

#if defined(__x86_64__) || defined(_M_X64)
#include <xmmintrin.h>
#endif

#include <cstdint>

#include <rex/hook.h>
#include <rex/platform/fpscr.h>

namespace {

// True when the context's copy masks (x86) or disables (ARM) every exception,
// as FPSCRRegister::InitHost leaves it.
bool ExceptionsMasked(uint32_t csr) {
  constexpr uint32_t mask = rex::platform::FPSCRPlatform::ExceptionMask;
#if defined(__x86_64__) || defined(_M_X64)
  return (csr & mask) == mask;
#else
  return (csr & mask) == 0;
#endif
}

// Every SSE exception masked in this host thread's MXCSR (x86 only).
void MaskHostExceptions() {
#if defined(__x86_64__) || defined(_M_X64)
  _mm_setcsr(_mm_getcsr() | 0x1F80u);
#endif
}

}  // namespace

#if defined(COD2_MP)
// 0x820C68C0: the multiplayer program's audio client callback (the campaign's
// 0x820C5A90 translates to the same code, score 0.993 by find_fn.py).
REX_EXTERN(__imp__sub_820C68C0);
REX_HOOK_RAW(sub_820C68C0) {
  if (!ExceptionsMasked(ctx.fpscr.csr)) ctx.fpscr.InitHost();
  MaskHostExceptions();
  __imp__sub_820C68C0(ctx, base);
}

// 0x820C2C28: the multiplayer program's audio code that calls XMAEnableContext
// (the gdb run above).
REX_EXTERN(__imp__sub_820C2C28);
REX_HOOK_RAW(sub_820C2C28) {
  if (!ExceptionsMasked(ctx.fpscr.csr)) ctx.fpscr.InitHost();
  MaskHostExceptions();
  __imp__sub_820C2C28(ctx, base);
}
#else
// 0x820C5A90: the campaign's audio client callback (called once per audio frame).
REX_EXTERN(__imp__sub_820C5A90);
REX_HOOK_RAW(sub_820C5A90) {
  if (!ExceptionsMasked(ctx.fpscr.csr)) ctx.fpscr.InitHost();
  __imp__sub_820C5A90(ctx, base);
}
#endif
