// cod3 - host fibers for the game's script threads.
//
// A small cooperative fiber: its own host stack, switched to and from on one
// OS thread. Unlike rex::thread::Fiber this reserves the stack lazily
// (mmap / CreateFiberEx with a small commit), because a level keeps hundreds
// of script threads alive and each needs a deep stack only now and then.
//
// Linux/macOS: ucontext (makecontext / swapcontext) on an mmap'd stack with
// a guard page. Windows: ConvertThreadToFiberEx / CreateFiberEx / SwitchToFiber.

#pragma once

#include <cstddef>

namespace cod3 {

class ScriptFiber {
 public:
  using Entry = void (*)(void* arg);

  // The calling thread's own context, as a fiber to switch back to. Created
  // once per OS thread; never destroyed while the thread lives.
  static ScriptFiber* ForCurrentThread();

  // A new fiber that runs entry(arg) when first switched to. The entry must
  // never return: it ends by switching to another fiber for good.
  static ScriptFiber* Create(Entry entry, void* arg, size_t stack_reserve);

  // Suspends the running fiber (`from`) and continues `to`. Returns when
  // something switches back to `from`.
  static void Switch(ScriptFiber* from, ScriptFiber* to);

  // Frees the fiber and its stack. Must not be the running fiber.
  static void Destroy(ScriptFiber* fiber);

  struct Impl;  // platform state (script_fiber.cpp)

 private:
  Impl* impl_ = nullptr;
};

}  // namespace cod3
