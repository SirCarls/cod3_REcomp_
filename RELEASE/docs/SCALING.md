# Scaling to the machine

How Call of Duty 3 sizes itself to the computer it runs on, in the ReXGlue port and in the Rust engine, and how the work to make it faster carries over from one to the other.

The owner's rules:

- 2026-10-13 01:19: "the game needs to scale to what the machine actually has. I want this to be in-engine."
- 22:04: "optimizations need to consider the rust port as well."
- 22:06: "the rust port shouldn't be limited by the 1 gpu thread."

So the port sizes itself from the machine, from a 2-core laptop to a 24-thread desktop, and finds what slows a frame by itself (no outside tools). The Xbox 360's six hardware threads and 512 MB aren't a template; they only show how little CPU the game's own code needs. Nothing here changes the game's speed: its clock follows real time.

The plan behind this, with the reasons for each step, is in the private notes (`dev/agents/hw-scaling/PLAN.md`); this page is the published summary.

## 1. What the game reads from the machine

Both engines read the same things, once at start, from the operating system, never guessed from the CPU's name:

| What | Linux | Windows |
|---|---|---|
| Logical CPUs, and the ones this process may use | `/sys/devices/system/cpu/online`; the affinity mask (`sched_getaffinity` in C++, `Cpus_allowed_list` in `/proc/self/status` in Rust) | `GetProcessAffinityMask` (processor group 0) |
| A container's CPU limit | cgroup v2 `cpu.max` of the process's cgroup and its parents (the tightest wins) | — |
| Physical cores | `topology/core_cpus_list` per CPU | `GetLogicalProcessorInformationEx(RelationProcessorCore)` |
| Groups of cores sharing the last-level cache (the two halves of a Ryzen 9 5900X) | `cache/index*/level` and `shared_cpu_list` | the same call, `RelationCache` level 3 |
| Efficiency cores (Intel hybrid CPUs) | `/sys/bus/event_source/devices/cpu_atom/cpus` | `EfficiencyClass` below the highest |
| Memory, total and available | `/proc/meminfo` | `GlobalMemoryStatusEx` |
| Running under Wine or Proton | — | `wine_get_version` exported by `ntdll.dll` |

**Usable CPUs** are the allowed ones, capped by the container's quota (rounded down, at least 1). Every budget below starts from that number. A game started with `taskset`, in a container, or by a launcher that limits it, gets less than the machine has, and sizes itself to what it gets.

Where the code is:

- ReXGlue port: `COD_3_WAW_Setup-tool/recomp/src/game/machine_profile.{h,cpp}`.
- Rust engine: `engine/crates/machine`.

Both print the same one-line summary, for example `Intel(R) Xeon(R) Processor @ 2.80GHz: 2 logical CPUs, 2 cores; RAM 7.8 GB (7.2 GB free)`. The port writes it to its log once the game runs; `cargo run -p machine --example show` prints it with the budgets.

## 2. The thread budgets

### Rules both engines follow

- **The OS keeps one logical CPU** from 8 usable up.
- **Spinning (waiting by checking in a loop) only with cores to spare:** from 6 usable CPUs. Below that, a waiting thread sleeps, so it never takes time from a thread with work.
- **The threads every frame passes through get physical cores of their own** when there are at least 4 cores and 4 usable CPUs.
- **Nothing in the budget is applied by force yet.** The budgets are shown in the log, in `cod3_report` and in F2; thread placement comes once the profiler (section 4) shows it helps.

### The ReXGlue port

Each frame passes through four threads in a row: the game's main thread, its render job, the SDK's GPU emulation thread, and presenting. The budget gives:

- **Frame threads on their own cores:** yes or no, by the rule above.
- **Draw workers:** threads that would build draws beside the GPU emulation thread. None below 8 usable CPUs, then one per 6, at most 8.
- **Shared pool:** what is left, at least 1. It covers shaders, pipelines, copies and files.

### The Rust engine

The Rust engine has **no GPU emulation thread and no single render thread**. One game thread runs the simulation. All other work runs as jobs on one pool: draw recording, culling, animation, streaming, shader and pipeline builds, uploads.

- **Workers:** the usable CPUs minus the game thread and the OS's one, at least 1.
- **Frame parallelism:** a frame's parallel loop splits into workers + 1 pieces, because the thread that asks works too.
- **Background limit:** background jobs (shader warm-up, cache writes, HD textures) hold at most half the workers, so frame work always finds a free one.
- **Priorities:** frame jobs run first, then "now" jobs (what a frame waits for: a streamed zone's decompression, a pipeline a draw needs), then background.

The pool is `engine/crates/jobs` (`Pool::for_this_machine()`).

### Test machines

`engine/crates/machine/budget_cases.tsv` lists 13 machines with the budgets both engines must give them. A few rows:

| Machine | Usable CPUs | Port: own cores / draw workers / pool | Rust: workers / background at most |
|---|---|---|---|
| one CPU | 1 | no / 0 / 1 | 1 / 1 |
| 2 cores, 4 threads | 4 | no / 0 / 2 | 3 / 1 |
| 4 cores, 8 threads | 8 | yes / 1 / 2 | 6 / 3 |
| 12 cores, 24 threads (Ryzen 9 5900X) | 24 | yes / 4 / 15 | 22 / 11 |
| 6 P-cores + 8 E-cores (20 threads) | 20 | yes / 3 / 12 | 18 / 9 |
| container: 16 CPUs, quota 3.5 | 3 | no / 0 / 1 | 2 / 1 |

Both sides are checked against the whole table:

- **Rust:** `cargo test -p machine`.
- **C++, from the project folder:** `c++ -std=c++20 -I COD_3_WAW_Setup-tool/recomp/src -o check_budgets tools/check_budgets.cpp COD_3_WAW_Setup-tool/recomp/src/game/machine_profile.cpp`, then `./check_budgets engine/crates/machine/budget_cases.tsv`.

A change to the rules changes both files and the table together.

## 3. The Rust renderer's threads

How the Rust engine will draw a frame without waiting on one thread. The renderer isn't written yet: this is the design it follows. The pool it records on (`engine/crates/jobs`) is built and tested.

1. **Simulation and rendering overlap.** The game thread simulates frame N+1 while frame N is rendered by jobs. Game time still follows the real clock.
2. **Recording is parallel.**
   - Each render pass's draws are cut into chunks, about four per thread (`Pool::for_each_chunk`).
   - Each chunk records into its own command buffer: a Vulkan secondary command buffer from that thread's own command pool, or a Direct3D 12 command list.
   - The pass then runs the buffers in chunk order (`vkCmdExecuteCommands` / `ExecuteCommandLists`), so the picture is the same as one thread recording everything.
   - The documentation of both APIs describes this. Vulkan command pools may only be used by one thread at a time, hence one per thread. Direct3D 12 command lists "can be recorded concurrently, which takes advantage of modern, multi-core processors" (see `docs/SOURCES.md`).
3. **Submitting stays one cheap call per queue per frame.** It runs at the end of the frame's jobs.
4. **Uploads go on a transfer queue.** Textures and buffers are copied there, recorded by a job, so drawing doesn't wait for copies.
5. **Pipelines are built ahead of time.** The engine knows its shaders, so it builds pipelines at load on the pool. Anything new along the way is a "now" job.
6. **Presenting waits for V-Sync on a job or the window thread**, never on the game thread.

On the cloud test machine (2 CPUs, software GPU), the pool's benchmark (`cargo run --release -p jobs --example bench`) records 3,000 stand-in draws of about 2 µs each in 1.70 ms on one thread and 0.97 ms on the pool (1.8x on 2 cores). On the 24-thread desktop the same split goes 23 ways.

## 4. Measuring before changing

The port measures itself with its thread profiler, which runs by itself and writes to the log:

- the machine and thread plan, once the game runs;
- the per-thread table of any frame of 250 ms or more;
- the table again when the game's main thread is stuck for the watchdog's time (`cod3_watchdog_seconds`). Frames over 3 s never reach the frame statistics, so this is how long stalls get a table too.

The profiler is `COD_3_WAW_Setup-tool/recomp/src/game/thread_profiler.{h,cpp}`. The SDK side of the measurements is patch `0031-cpu-thread-timing`.

The verdict line says what limits the frame: the frame limit, the CPU (all usable cores busy), the graphics card, the GPU emulation thread, the game's main thread, or the game's other threads. A thread that waits for the SDK's global lock also shows who held it ("global lock 19% (held by ...)", patch 0038; Linux).

When the verdict names a thread, the **function profiler** says what that thread does. Once a session, after a minute of play, it profiles the busy frame threads by itself (`cod3_profile_auto`: the GPU emulation, the game's main thread and the window thread, 10 s each) and `cod3_report` carries the summaries; by hand, `cod3_profile main 10` (or `gpu`, `window`, or the start of a thread's name) in the F10 log console samples the thread about once per millisecond of its CPU time for 10 s, starting 3 s later so the console can be closed, and writes the hottest functions, each game function with its Xbox 360 address, to `userdata/logs/profile-<thread>-<time>.txt`. It uses no outside tools: a per-thread CPU-time timer on Linux, suspend-and-read on Windows (`recomp/src/game/function_profiler.{h,cpp}`). Code without a name of its own (a graphics driver, the C library) is listed with who called it: each sample keeps the stack words that point into code, and the first one right after a call instruction is taken as the return address (a good guess, not a certainty). memcpy, memset and the like are named even without the C library's debug symbols. On Linux each sample also notes whether the thread held the SDK's global lock, and the report lists what it did while holding it; the automatic profiles include the thread the GPU emulation waits behind most.

The per-second cost line also counts the window's presents (patch 0037): well above the frame rate means the window repaints on its own.

Each change below is measured with it first. The thread table also says how long the game's Direct3D waited for the GPU emulation and for what (ring space, command memory, fences, locks, resource release: the reason codes of its wait helpers, named in `docs/COD3_D3D.md`; `game/gpu_wait.h`). What the measurements found on the owner's PC (Ryzen 9 5900X, RTX 3080, Linux, 2026-10-15):

- Shader translation while a draw waits: 0.0 ms a frame. Moving it off the GPU thread would gain nothing there.
- The window thread at 93-97% of a core in every scene: the achievement toast asked for continuous repaints, so the window presented 240 times a second besides the game's frames (patch 0037 fixes it).
- The game's main thread at 86-95% in fights: in the cloud the function profiler put ~95% of its time in the game's Direct3D spinning on the GPU's progress (`recomp/src/game/gpu_wait.h` fixes it with patch 0039). On the 2-CPU test machine the frame rate went from 9.2 to 13.2 when the spin stopped taking a core from the software GPU.
- The GPU emulation thread waits 8-22% of its time for the global lock, held mostly by the game's main thread and its four job workers (patch 0038). The main thread's share was the game waiting for its own render job: 0x8212A110 helps with a runnable job and looks again without a pause, and every look takes and releases the job queue's kernel mutex, two kernel calls under the global lock (1.2 million round trips a second in the cloud). `recomp/src/game/job_wait.h` makes it sleep until the queue changes (cloud: the main thread from 75% to 3% of a core, 11.8 to 18-19 fps), and patch 0045 has the lock spin briefly before a waiter sleeps, for what contention is left.
- A report written from the console showed the window thread busier than in play (an open console repaints continuously): `cod3_report` now also shows the last second of play without that.
- With patch 0037 the window still used 66-97% of a core: 78% of its time was the NVIDIA driver compiling shaders, because the SDK's Vulkan presenter rebuilt its paint pipeline at every paint (the format the pipeline was made for was never stored, so it always looked outdated). Patch 0046 keeps it.
- The GPU emulation thread (owner's profile, 2026-10-16): 7.4% was a register table search at every GPU register write, only for a debug line (patch 0047: a bit per register, 7.5 -> 0.85 ns a write; runs of registers take the range path). The C library took 19.4% and the NVIDIA driver 15.2%; in the cloud the profiler's new caller lines put the C library's memcpy under the resolve readback (`IssueCopy_ReadbackResolvePath`; `readback_resolve full` copies 8160 KB a frame back to the game on the owner's PC), which belongs to the readback and renderer work (patch 0024; the planned Rust renderer).
- With 0046-0047 (owner's PC, Falaise Road, 1080p): about 90 fps, limited by the GPU emulation thread; the window thread at 2-7% of a core. On the GPU thread, `vkUpdateDescriptorSets` for every draw's textures was 5.2% (patch 0048 reuses a set when a draw binds the same textures as an earlier one in the submission: 83% of draws in the cloud), the out-of-line bit scans 2.7% (0048 inlines them), the readback's memcpy 10.9%, and waits for the global lock 17%, held mostly by the game's main thread: the function profiler now shows what a thread does while it holds the lock.
- The readback's whole 8160 KB a frame is for one texel: the game's CPU reads a single 32-bit value of that resolved surface each frame (0x82420F48, under `RE_RenderScene`; found with gdb in the cloud). SDK patch 0049 copies back only the 4 KB pieces the CPU touches (two in the cloud: 8 KB instead of 3680 KB a frame) and keeps the rest watched.

## 5. How each improvement carries over

| Improvement | ReXGlue port | Rust engine |
|---|---|---|
| Machine profile and budgets | `machine_profile.cpp` (done) | `machine` crate (done), same numbers, one test table |
| Profiler: per-thread time, waits, verdict, stall tables | `thread_profiler.cpp` + SDK patch 0031 (done) | The same verdicts from the pool's own counts (next) |
| One shared pool with priorities, background capped | Planned: the SDK's separate pools (pipelines alone take 3/4 of the CPUs) become one | `jobs` crate (done) |
| Draws built on several threads | Planned, hard: the emulated GPU's commands come in one stream and must keep their order | From the start: parallel recording (section 3) |
| Shader translation off the GPU thread | Dropped: measured at 0.0 ms a frame on the owner's PC | Not needed: pipelines at load, "now" jobs for the rest |
| Thread placement (frame threads in one cache group, background on E-cores) | Planned, from the profile | The same, from the same profile |
| Spinning only with spare cores | Done for the GPU thread (patch 0022), budget rule | Pool workers spin only when the budget allows |
| Preloading levels when memory allows | Planned | The same rule, from the profile's available memory |
| Waking the game's job workers reliably | `game/job_wake.cpp` (done) | The pool's queue does it |
| Waiting for the GPU without spinning: a short spin, then sleep until the GPU signals progress | `game/gpu_wait.cpp` + SDK patch 0039 (done; `cod3_gpu_wait_sleeps`) | `jobs::Progress` (done): whoever makes progress signals it, a waiter spins only while the budget allows, then sleeps |
| No repainting without something new to show | SDK patch 0037 (done) | Present once per finished frame; UI-only repaints at most at the display's rate, and only while something animates |
| Function profiler: which functions a thread spends its time in | `game/function_profiler.cpp` (done) | The same sampler, from the same rules (next) |
| Knowing who holds a lock others wait for | SDK patch 0038 (done, Linux) | No global lock: each subsystem owns its own; the pool counts its waits |
| Waiting for jobs without polling the queue: help, then sleep until the queue changes | `game/job_wait.cpp` (done; `cod3_job_wait_sleeps`) | `jobs`: a loop's caller helps, then sleeps on the pool's condition variable (`Pool::for_each_chunk`, `Pending::wait`) |
| Short locks spin briefly before sleeping, only with cores to spare | SDK patch 0045 (done; `global_lock_spin_us`) | `jobs` workers spin 50 us before sleeping, only when the budget allows spinning |
| Profiles without anyone asking | `cod3_profile_auto` (done) | The same: once a session, the busiest threads |
| Library and driver time reported by caller | `game/function_profiler.cpp` (done): stack scan for the return address, memcpy named without debug symbols | The same sampler; the engine's own code has symbols |
| Presenting without rebuilding state | SDK patch 0046 (done): the paint pipeline is kept until the window's format changes | Pipelines made when the swapchain is created, rebuilt only when it is |
| Cheap state writes on the one GPU thread | SDK patch 0047 (done): no table search per register write; register runs in one copy | No emulated register file: draw state is typed data recorded by many threads |
| Descriptor sets written once, reused while they still match | SDK patch 0048 (done): a draw with the same textures reuses the submission's set | Material descriptor sets made at load; per-draw data in push constants or one dynamic buffer |
| Copying back only what the CPU reads | SDK patch 0049 (done): resolve destinations in 4 KB pieces, the rest watched | The engine reads its own results where it computes them (a visibility query or a GPU-side test), no CPU readback of pictures |
| Seeing what a lock holder does while others wait | Function profiler: samples taken while holding the SDK's global lock (done, Linux) | No global lock; the pool's own locks report their holders |
