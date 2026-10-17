// cod3 - [hw-scaling] the game's wait for its own jobs sleeps instead of
// spinning on the job queue's lock.
//
// The job system (four worker threads running 0x8212A520; queue and job pool
// at 0x8290E930, game/job_wake.cpp) guards its queue with a kernel mutex
// (CreateMutex; handle at 0x8290E920; taken with WaitForSingleObject
// 0x82343880, released with ReleaseMutex 0x82344FC0). 0x8212A110 waits for a
// group of jobs (or all of them, -1): until none is left it helps run them
// (0x8212A520 in helper mode, which takes the lock, finds nothing runnable
// and returns when the rest run on the workers) and looks again, with no
// pause at all. The game's main thread waits there for the render job every
// frame.
//
// On the console a mutex round trip is a few dozen instructions. Here it is
// two kernel calls (~0.3 us and ~0.15 us), each taking the SDK's global lock:
// measured in the cloud, the main thread went round that loop 1.2 million
// times a second, 2.5 million global lock round trips, while the GPU
// emulation thread (which takes the same lock for every draw) waited for it.
// On the owner's PC the main thread showed 86-95% of a core and the GPU
// thread 9-21% of its time waiting for the global lock, held mostly by the
// main thread and the job workers (SDK patch 0038).
//
// Now 0x8212A110 is the same loop, plus a pause when there was nothing to
// help with: it sleeps until another thread releases the queue's lock (a
// worker took or finished a job, or a job was queued) - every change to the
// queue happens under that lock - or at most kSleepMicros, then looks again.
// With cores to spare (machine profile: busy-waiting allowed) it first keeps
// looking for kSpinMicros without the kernel calls. Which jobs run where, and
// in which order, is the game's own; nothing about game time changes.
//
// cod3_job_wait_sleeps (default on) switches it; off is the game's own loop.
// The sleeping shows as a wait of the game main thread in the stall table.

#pragma once

// Nothing to call: the hooks in job_wait.cpp replace the game's functions.
