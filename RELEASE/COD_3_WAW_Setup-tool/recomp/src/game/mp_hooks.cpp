// cod3mp - hooks only the multiplayer program (codmp_xenonf.xex) has.
//
// Addresses are codmp_xenonf.xex's; evidence in tools/name_mp_functions.py
// and dev/agents/multiplayer/NOTES.md. The campaign's version of each hook
// is named next to it.

#include <rex/hook.h>

#include "game/guest.h"
#include "game/watchdog.h"

namespace {

// The job workers' auto-reset event: Jobs_WakeWorkers loads it from here
// (`lis r11,-0x7d57; lwz r3,-0x4764(r11)`). Campaign: 0x8290E98C.
constexpr uint32_t kJobWakeEvent = 0x82A8B89C;

}  // namespace

// BOOL SetEvent(HANDLE): the same 16 instructions as the campaign's
// 0x82345000 (NtSetEvent, then BaseSetLastNTError when it fails).
REX_EXTERN(sub_823143D0);

// Waking the job workers. The engine's job system is the campaign's (four
// workers asleep on one auto-reset event, woken with PulseEvent after a job
// is queued), and so is its lost wake-up: a pulse only releases a worker
// that is waiting at that moment, so a level load can wait for good on
// decompression jobs nobody runs. SetEvent keeps the wake-up until a worker
// takes it. The reasoning in full: game/job_wake.cpp (campaign: the hook on
// 0x8212A100).
REX_HOOK_RAW(Jobs_WakeWorkers) {
  ctx.r3.u64 = cod3::guest::ReadU32(base, kJobWakeEvent);
  sub_823143D0(ctx, base);
}

// The campaign's hang watchdog (game/watchdog.cpp) isn't in this program: it
// hooks default.xex functions by address (sub_82462FB0, sub_82525190), which
// would replace whatever function has that address here, and its call-site
// labels are default.xex's. The marks the shared code makes
// (game/script_threads.cpp) go nowhere until it has multiplayer addresses.
namespace cod3::watchdog {
void Start() {}
void Mark(const char*) {}
void MarkFrame() {}
void SetGameContext(const PPCContext*) {}
uint64_t MarkCount() { return 0; }
}  // namespace cod3::watchdog
