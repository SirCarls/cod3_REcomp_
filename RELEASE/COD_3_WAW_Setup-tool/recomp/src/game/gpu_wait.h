// cod3 - [hw-scaling] the game's waits for the GPU emulation sleep instead
// of holding a CPU core.
//
// The game's Direct3D waits for the GPU by spinning on values the GPU writes
// to memory: the ring buffer's read pointer, fences, a frame counter. Four
// loops do it (0x822F2380, 0x822F1738, 0x822F16A0, 0x82302768); each starts
// the wait with 0x822EBFB8 and calls 0x822EC168 between two checks. On the
// Xbox 360 that is a hardware thread the game owns. Here it held a CPU core at
// 100% for as long as the GPU emulation was behind - in a GPU-bound scene,
// most of every frame: the function profiler (cod3_profile main) put
// 0x822EC168 and its loop at ~95% of the game main thread's time. A busy
// core takes power and boost clock from the threads doing the work, and on a
// PC with few cores, the core itself.
//
// Now the first kSpinMicros of a wait still spin (a short wait ends sooner
// than a sleep could), then the thread sleeps until the GPU emulation signals
// progress (rex/graphics/gpu_progress.h, SDK patch 0039) and checks again;
// at most kSleepMicros at a time, so a write the signal doesn't cover only
// costs a little latency. The checks and their results are the game's own;
// game time runs on the game's clock and isn't touched. The sleeping part
// shows as a wait of the game main thread in the stall table.
//
// cod3_gpu_wait_sleeps (default on) switches it; off is the game's own spin.
// Without the SDK patch the hooks only call the game's code.

#pragma once

// Nothing to call: the hooks in gpu_wait.cpp replace the game's functions.
