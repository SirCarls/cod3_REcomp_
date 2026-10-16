// cod3 - waking the engine's job workers without losing the wake-up.
//
// The job system (four worker threads running 0x8212A520, queue and job pool
// at 0x8290E930) puts its workers to sleep on one auto-reset event, created
// by 0x82129E90 with CreateEvent(NULL, FALSE, FALSE, NULL). After queuing a
// job, 0x82129948 wakes them through 0x8212A100, which is PulseEvent on that
// event (0x82345898 -> NtPulseEvent). It is the game's only PulseEvent.
//
// A pulse only releases a thread that is blocked in its wait at that moment,
// and it gets lost two ways:
//  - the game's own race: a worker that found the queue empty and released
//    the queue lock but hasn't reached its wait yet misses it;
//  - the SDK's POSIX PulseEvent is Signal + ~10 us sleep + Reset: a worker
//    that is woken but doesn't get a CPU within those microseconds sees the
//    event reset again and goes back to sleep. On a busy PC (or any machine
//    with few cores) that is most of the time.
// On the console the main thread queues jobs every frame, so a lost pulse is
// made up by the next one. While a level loads it isn't: the file reader
// thread (0x82123DE0) queues the LZO decompression jobs of a compressed
// block of a .cod file and then waits for them before it reads on. If that
// pulse is lost, no one ever queues another job and the game stays on
// "Loading ..." for good (seen loading saint_lo: two decompression jobs for
// saint_lo_Weapons.xenon.ib queued, all four workers asleep, 20+ minutes).
//
// SetEvent on the auto-reset event does the same when a worker is waiting
// (exactly one is released) and isn't lost when none is: the event stays set
// and the next worker to wait goes straight through and looks at the queue
// again (at worst one extra look that finds it empty).

#include <rex/hook.h>

#include "game/addresses.h"
#include "game/guest.h"

REX_EXTERN(sub_82345000);  // BOOL SetEvent(HANDLE) (NtSetEvent, sets the last error)

REX_HOOK_RAW(sub_8212A100) {
  ctx.r3.u64 = cod3::guest::ReadU32(base, cod3::addr::jobs::kWakeEvent);
  sub_82345000(ctx, base);
}
