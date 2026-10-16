// cod3 - the game's script threads, run on host fibers.
//
// CoD3's level scripts (C++ written against Treyarch's "Broc" script library,
// compiled into each level DLL) run as coroutines with their own stacks:
//
//  * Start. ScriptThread_Run (0x824A6150) runs one script thread per call. For a new thread it
//    reserves 8 KB of stack below its own frame and calls the thread's entry
//    functor through ScriptThread_CallEntry (0x823C0B98) (0x824A62A4).
//  * Yield. A wait / waittill in the level calls the engine's suspend through
//    the level's interface table (slots +76 / +80). The level has already
//    saved r3-r31, f14-f31, LR and CTR into a frame at the top of the thread's
//    stack and copied the stack out. The engine's suspend longjmps back to
//    the setjmp in ScriptThread_Run (0x824A6150) (0x824A61DC) and never returns.
//  * Resume. A later ScriptThread_Run (0x824A6150) copies the stack back in (0x824A63A0),
//    switches r1 to the saved frame (0x824A63AC), reloads every register and
//    `blr`s into the middle of the level's yield function (0x824A6478).
//
// A static recompilation can't "branch into the middle of a function", so a
// resumed thread ran on in ScriptThread_Run (0x824A6150)'s caller with the thread's registers.
// The result was r31 = 0 and a crash right after the level loaded. Here each
// script thread runs on its own host fiber instead:
//
//  * ScriptThread_CallEntry (0x823C0B98) (start) runs the entry functor on a new fiber.
//  * The guest longjmp (longjmp_guest (0x8234EEC0)) on a script fiber is a yield: switch back
//    to the engine's fiber, which then performs the longjmp the thread asked for.
//  * The resume point (mid-asm hooks at 0x824A63AC and 0x824A6480, see
//    config/default.toml) switches to the thread's fiber after the engine's own
//    code has loaded the thread's registers into ctx. The thread continues
//    inside the yield call it made, and the level's yield function returns
//    straight to the script (a mid-asm `return` after each +76 / +80 call, see
//    the level configs).
//  * A thread whose entry function returns after a resume continues the
//    engine at 0x824A62A8, the instruction after the start call, with the
//    engine's registers as they were at the resume.
//  * The thread object's destructor (ScriptThread_Destroy (0x824A3040)) frees the fiber.
//
// The guest's own stack copying stays as it is: the fiber's host frames refer
// to the same guest stack addresses, and the engine restores the same bytes
// there before every resume.
//
// [cod3mp] The multiplayer program (codmp_xenonf.xex) has the same engine
// code at other addresses: ScriptThread_Run 0x824C5298 is instruction for
// instruction this one, so the same frame offsets hold, the codegen names
// its start call, longjmp and destructor the same (tools/name_mp_functions.py)
// and its two resume hooks are in recomp/mp/config/codmp.toml. This file is
// compiled into both programs.
//
// The mechanism, and where the seams are, were first worked out by the
// CoD3Recomp project (github.com/FrankHUN88453/CoD3Recomp, CoD3Host/coroutines.cpp).
// See docs/SOURCES.md.

#include <atomic>
#include <cstdint>
#include <type_traits>
#include <unordered_map>

#include <rex/hook.h>
#include <rex/logging.h>

#include "game/guest.h"
#include "game/script_fiber.h"
#include "game/script_threads.h"
#include "game/watchdog.h"
// get_jmp_buf_map(), shared with ppc_setjmp: the program's own generated code.
#if defined(COD3_MP)
#include "generated/codmp/cod3mp_pch.h"  // [cod3mp]
#else
#include "generated/default/cod3_pch.h"
#endif

REX_EXTERN(__imp__ScriptThread_CallEntry);  // the thread's entry functor call
REX_EXTERN(__imp__ScriptThread_Destroy);  // the script thread object's destructor

namespace {

// Guest addresses in default.xex (see the header comment).
constexpr uint32_t kThreadStackReserve = 8192;    // stack ScriptThread_Run (0x824A6150) keeps below its frame
constexpr uint32_t kFrameThreadArg = 436;         // ScriptThread_Run (0x824A6150)'s thread argument, r1-relative

// Host stack for one script thread. Reserved, not committed (script_fiber.cpp).
constexpr size_t kFiberStackReserve = 4u << 20;

struct ScriptThread {
  uint32_t key = 0;  // the guest thread object
  cod3::ScriptFiber* fiber = nullptr;
  cod3::ScriptFiber* resumer = nullptr;  // fiber that switched to it last
  PPCContext* ctx = nullptr;
  uint8_t* base = nullptr;
  bool running = false;   // inside its entry function
  bool finished = false;  // its entry function returned

  // The longjmp the thread asked for when it yielded, performed by the resumer.
  bool jump_pending = false;
  uint32_t jump_buffer = 0;
  int jump_value = 0;

  // The engine's registers at the resume point, for a thread that finishes.
  PPCContext engine_regs{};
  uint32_t engine_frame = 0;
};

// Script threads only ever run on the game's main thread, but the state is
// per OS thread so nothing here needs a lock.
struct ThreadState {
  cod3::ScriptFiber* os_fiber = nullptr;   // the OS thread's own context
  cod3::ScriptFiber* current = nullptr;    // fiber running right now
  ScriptThread* running = nullptr;         // script thread running right now, if any
  uint32_t resume_key = 0;                 // set by the resume-prepare hook
  std::unordered_map<uint32_t, ScriptThread*> threads;
  uint32_t begun = 0, resumed = 0, finished = 0, destroyed = 0;
  uint32_t logged = 0;  // detailed log lines so far (the first few events only)
};
thread_local ThreadState t_state;

ThreadState& State() {
  auto& s = t_state;
  if (!s.os_fiber) {
    s.os_fiber = cod3::ScriptFiber::ForCurrentThread();
    s.current = s.os_fiber;
  }
  return s;
}

// The first events are logged one by one; after that only the summary.
constexpr uint32_t kDetailedLogLines = 40;
bool LogDetail(ThreadState& s) { return s.logged++ < kDetailedLogLines; }

// The setjmp map's value: a 16-byte aligned slot holding the jmp_buf (SDK
// patch 0027), or the bare jmp_buf of an SDK without it.
template <typename Slot>
auto& JmpBufOf(Slot& slot) {
  if constexpr (requires { slot.buf; })
    return slot.buf;
  else
    return slot;
}

#if defined(_WIN32)
// The C runtime's setjmp on x64 Windows saves xmm6-xmm15 with aligned moves:
// a buffer that isn't 16-byte aligned crashes the first script thread of a
// level. Generated code from an SDK without patch 0027 has that problem.
static_assert(alignof(std::remove_reference_t<decltype(get_jmp_buf_map())>::mapped_type) >= 16,
              "setjmp buffers must be 16-byte aligned on Windows: rebuild the SDK with patch 0027 "
              "and run the codegen again");
#endif

[[noreturn]] void HostLongjmp(uint32_t guest_buffer, int value) {
  auto& map = get_jmp_buf_map();
  auto it = map.find(guest_buffer);
  if (it == map.end()) {
    REXLOG_ERROR("cod3: longjmp through guest buffer {:08X}, which no setjmp filled", guest_buffer);
    std::abort();
  }
  longjmp(JmpBufOf(it->second), value);
}

// What the fibers are doing, for the hang watchdog (game/watchdog.h), which
// reads it from its own thread. Plain counters, written by the game thread.
enum class FiberPhase : uint32_t {
  kEngine,    // on the engine's own fiber
  kToThread,  // switching to a script thread's fiber
  kOnThread,  // a script thread runs on its fiber
  kToEngine,  // a script thread yields or ends: switching back
};
struct Published {
  std::atomic<FiberPhase> phase{FiberPhase::kEngine};
  std::atomic<uint32_t> key{0};  // the script thread of the phase
  std::atomic<uint32_t> begun{0}, resumed{0}, finished{0}, destroyed{0};
};
Published g_published;

void Publish(FiberPhase phase, uint32_t key) {
  g_published.key.store(key, std::memory_order_relaxed);
  g_published.phase.store(phase, std::memory_order_release);
}

void PublishCounts(const ThreadState& s) {
  g_published.begun.store(s.begun, std::memory_order_relaxed);
  g_published.resumed.store(s.resumed, std::memory_order_relaxed);
  g_published.finished.store(s.finished, std::memory_order_relaxed);
  g_published.destroyed.store(s.destroyed, std::memory_order_relaxed);
}

void FiberMain(void* arg) {
  auto* t = static_cast<ScriptThread*>(arg);
  Publish(FiberPhase::kOnThread, t->key);
  t->running = true;
  __imp__ScriptThread_CallEntry(*t->ctx, t->base);  // the thread's entry functor
  t->running = false;
  t->finished = true;
  // Back to whoever resumed it last; this fiber is never continued again.
  auto& s = t_state;
  cod3::ScriptFiber* self = t->fiber;
  s.current = t->resumer;
  Publish(FiberPhase::kToEngine, t->key);
  cod3::ScriptFiber::Switch(self, t->resumer);
  REXLOG_ERROR("cod3: finished script thread {:08X} was continued", t->key);
  std::abort();
}

// Switches to the thread's fiber; returns when it yields or finishes.
void RunOnFiber(ThreadState& s, ScriptThread& t) {
  ScriptThread* outer = s.running;
  cod3::ScriptFiber* from = s.current;
  t.resumer = from;
  s.running = &t;
  s.current = t.fiber;
  Publish(FiberPhase::kToThread, t.key);
  cod3::ScriptFiber::Switch(from, t.fiber);
  // Back on `from`: FiberMain or the longjmp hook switched here and set s.current.
  s.running = outer;
  if (outer)
    Publish(FiberPhase::kOnThread, outer->key);
  else
    Publish(FiberPhase::kEngine, 0);
}

void DestroyThread(ThreadState& s, uint32_t key) {
  auto it = s.threads.find(key);
  if (it == s.threads.end()) return;
  ScriptThread* t = it->second;
  s.threads.erase(it);
  ++s.destroyed;
  PublishCounts(s);
  if (t->fiber == s.current) {
    // A thread deleting itself: its fiber can't be freed while it runs.
    REXLOG_WARN("cod3: script thread {:08X} destroyed from inside; its fiber leaks", key);
    return;
  }
  cod3::ScriptFiber::Destroy(t->fiber);
  delete t;
}

}  // namespace

// --- Start: the engine calls the thread's entry functor (only caller: 0x824A62A4).
REX_HOOK_RAW(ScriptThread_CallEntry) {
  cod3::watchdog::Mark("a script thread starts");
  auto& s = State();
  // At the call r1 is ScriptThread_Run (0x824A6150)'s frame minus the 8 KB thread stack.
  const uint32_t key = cod3::guest::ReadU32(base, ctx.r1.u32 + kThreadStackReserve + kFrameThreadArg);
  DestroyThread(s, key);  // a thread object reused for a new thread

  auto* t = new ScriptThread();
  t->key = key;
  t->ctx = &ctx;
  t->base = base;
  t->fiber = cod3::ScriptFiber::Create(&FiberMain, t, kFiberStackReserve);
  if (!t->fiber) {
    REXLOG_ERROR("cod3: no fiber for script thread {:08X}; running it inline", key);
    delete t;
    __imp__ScriptThread_CallEntry(ctx, base);
    return;
  }
  s.threads[key] = t;
  ++s.begun;
  PublishCounts(s);
  RunOnFiber(s, *t);
  if (LogDetail(s))
    REXLOG_INFO("cod3: script thread {:08X} began and {}", key,
                t->finished ? "finished" : t->jump_pending ? "yielded" : "came back (?)");
  if (t->finished) ++s.finished;
  PublishCounts(s);

  if (t->jump_pending) {
    t->jump_pending = false;
    HostLongjmp(t->jump_buffer, t->jump_value);  // the yield's longjmp, on the engine fiber
  }
  // Finished on its first run: an ordinary call that returned.
}

// --- longjmp(jmp_buf, value). On a script fiber this is the thread yielding.
REX_HOOK_RAW(longjmp_guest) {
  auto& s = State();
  const uint32_t buffer = ctx.r3.u32;
  const int value = ctx.r4.s32;
  ScriptThread* t = s.running;
  if (!t) HostLongjmp(buffer, value);

  t->jump_pending = true;
  t->jump_buffer = buffer;
  t->jump_value = value;
  s.current = t->resumer;
  Publish(FiberPhase::kToEngine, t->key);
  cod3::ScriptFiber::Switch(t->fiber, t->resumer);
  Publish(FiberPhase::kOnThread, t->key);
  // Resumed: the engine has loaded this thread's registers into ctx. Return
  // into the engine's suspend function, which returns to the level's yield
  // function, which returns to the script (mid-asm returns, see the configs).
}

// --- Resume, part 1 (mid-asm hook before 0x824A63AC `or r1,r11,r10`): r1 is
// still ScriptThread_Run (0x824A6150)'s frame. Remember which thread and the engine's state.
void cod3_script_resume_prepare(PPCContext& ctx, uint8_t* base) {
  auto& s = State();
  const uint32_t key = cod3::guest::ReadU32(base, ctx.r1.u32 + kFrameThreadArg);
  s.resume_key = key;
  auto it = s.threads.find(key);
  if (it == s.threads.end()) return;
  it->second->engine_regs = ctx;
  it->second->engine_frame = ctx.r1.u32;
}

// --- Resume, part 2 (mid-asm hook before the `blr` at 0x824A6480): ctx holds
// the thread's registers. Returns true when the thread ran to its end, after
// restoring the engine's registers; the generated code then continues at
// 0x824A62A8, the instruction after the start call.
bool cod3_script_resume(PPCContext& ctx, uint8_t* base) {
  auto& s = State();
  const uint32_t key = s.resume_key;
  s.resume_key = 0;
  auto it = s.threads.find(key);
  if (it == s.threads.end() || !it->second->running) {
    REXLOG_ERROR("cod3: script thread {:08X} resumed, but no fiber is suspended for it", key);
    return false;
  }
  ScriptThread& t = *it->second;
  t.ctx = &ctx;
  t.base = base;
  ++s.resumed;
  cod3::watchdog::Mark("a script thread resumes");
  RunOnFiber(s, t);
  if (LogDetail(s))
    REXLOG_INFO("cod3: script thread {:08X} resumed and {}", key,
                t.finished ? "finished" : t.jump_pending ? "yielded" : "came back (?)");
  if (t.finished) ++s.finished;
  PublishCounts(s);

  if (t.jump_pending) {
    t.jump_pending = false;
    HostLongjmp(t.jump_buffer, t.jump_value);
  }
  if (t.finished) {
    ctx = t.engine_regs;
    ctx.r1.u32 = t.engine_frame - kThreadStackReserve;
    return true;
  }
  REXLOG_ERROR("cod3: script thread {:08X} came back without yielding or finishing", key);
  return false;
}

// --- The script thread object's destructor: its fiber goes with it.
REX_HOOK_RAW(ScriptThread_Destroy) {
  DestroyThread(State(), ctx.r3.u32);
  __imp__ScriptThread_Destroy(ctx, base);
}

std::string cod3::ScriptThreadSummary() {
  auto& s = State();
  uint32_t suspended = 0;
  for (const auto& [key, t] : s.threads)
    if (t->running) ++suspended;
  return fmt::format("script threads: {} begun, {} resumed, {} finished, {} destroyed, {} suspended",
                     s.begun, s.resumed, s.finished, s.destroyed, suspended);
}

std::string cod3::ScriptThreadWatchLine() {
  const auto& p = g_published;
  const uint32_t key = p.key.load(std::memory_order_relaxed);
  std::string where;
  switch (p.phase.load(std::memory_order_acquire)) {
    case FiberPhase::kEngine:
      where = "on the engine's own fiber";
      break;
    case FiberPhase::kToThread:
      where = fmt::format("switching to script thread {:08X}'s fiber (switch in progress)", key);
      break;
    case FiberPhase::kOnThread:
      where = fmt::format("running script thread {:08X} on its fiber", key);
      break;
    case FiberPhase::kToEngine:
      where = fmt::format("script thread {:08X} switching back to the engine (switch in progress)",
                          key);
      break;
  }
  return fmt::format("{}; script threads so far: {} begun, {} resumed, {} finished, {} destroyed",
                     where, p.begun.load(std::memory_order_relaxed),
                     p.resumed.load(std::memory_order_relaxed),
                     p.finished.load(std::memory_order_relaxed),
                     p.destroyed.load(std::memory_order_relaxed));
}
