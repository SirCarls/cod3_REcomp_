// cod3 - [hw-scaling] the game's render job, timed: the renderer plan's
// first measurement (dev/agents/rust-renderer/DESIGN.md, "What M0 must
// answer first", items 1 and 4).
//
// The game's main thread builds a frame's render lists. nglListSend
// (0x8213DAA0) then queues one job a frame on a game job worker: 0x82130980,
// which runs the back end 0x8214F770 (every node's Render, through the game's
// Direct3D into the emulated GPU's command stream) and then presents. When a
// renderer takes over at Direct3D's level, this job is the part that stays
// in order on one thread, so its time a frame is the ceiling the plan
// estimates (4-6 ms on the owner's 5900X).
//
// Hooks: the job (wall time and the worker's CPU time), its back end, and
// Direct3D's Swap (0x822F4C28), counted by whether the render job called it
// (item 4: does anything else swap, Direct3D's own thread during a load?).
// The thread profiler reads the totals (game/thread_profiler.h
// SetRenderJobReader) and prints them with its table and verdict.
//
// Campaign only: the multiplayer program has other addresses and doesn't
// build this file. Costs two clock reads per call; nothing changes timing.

#pragma once

namespace cod3 {}  // everything is in render_job_timing.cpp
