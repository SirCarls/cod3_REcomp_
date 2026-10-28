// cod3 - host fibers for the game's script threads (see script_fiber.h).

#if defined(__APPLE__) && !defined(_XOPEN_SOURCE)
#define _XOPEN_SOURCE 700  // Darwin hides ucontext without it
#endif

#include "game/script_fiber.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <mutex>

#if defined(_WIN32)
#include <windows.h>
#else
#include <sys/mman.h>
#include <ucontext.h>
#include <unistd.h>
#endif

namespace cod3 {

#if defined(_WIN32)

struct ScriptFiber::Impl {
  void* handle = nullptr;
  bool is_thread = false;
};

namespace {
// Windows fibers start in a WINAPI function; this carries the real entry.
struct StartArgs {
  ScriptFiber::Entry entry;
  void* arg;
};
void WINAPI WinStart(void* p) {
  auto* args = static_cast<StartArgs*>(p);
  ScriptFiber::Entry entry = args->entry;
  void* arg = args->arg;
  delete args;
  entry(arg);
  std::fprintf(stderr, "cod3: a script fiber's entry returned\n");
  std::abort();
}
}  // namespace

ScriptFiber* ScriptFiber::ForCurrentThread() {
  auto* f = new ScriptFiber();
  f->impl_ = new Impl();
  f->impl_->is_thread = true;
  f->impl_->handle = ConvertThreadToFiberEx(nullptr, FIBER_FLAG_FLOAT_SWITCH);
  if (!f->impl_->handle) f->impl_->handle = GetCurrentFiber();  // already a fiber
  return f;
}

ScriptFiber* ScriptFiber::Create(Entry entry, void* arg, size_t stack_reserve) {
  constexpr size_t kCommit = 64 * 1024;
  auto* args = new StartArgs{entry, arg};
  void* handle = CreateFiberEx(kCommit, stack_reserve, FIBER_FLAG_FLOAT_SWITCH, WinStart, args);
  if (!handle) {
    delete args;
    return nullptr;
  }
  auto* f = new ScriptFiber();
  f->impl_ = new Impl();
  f->impl_->handle = handle;
  return f;
}

void ScriptFiber::Switch(ScriptFiber* /*from*/, ScriptFiber* to) {
  SwitchToFiber(to->impl_->handle);
}

void ScriptFiber::Destroy(ScriptFiber* fiber) {
  if (!fiber) return;
  if (!fiber->impl_->is_thread) DeleteFiber(fiber->impl_->handle);
  delete fiber->impl_;
  delete fiber;
}

#else  // POSIX

struct ScriptFiber::Impl {
  ucontext_t context{};
  void* stack = nullptr;       // whole mapping, guard page included
  size_t stack_size = 0;
  Entry entry = nullptr;
  void* arg = nullptr;
};

namespace {
// makecontext passes int arguments only; the Impl pointer is split in two.
void PosixStart(unsigned lo, unsigned hi) {
  auto* impl = reinterpret_cast<ScriptFiber::Impl*>((std::uintptr_t(hi) << 32) | std::uintptr_t(lo));
  impl->entry(impl->arg);
  std::fprintf(stderr, "cod3: a script fiber's entry returned\n");
  std::abort();
}

// [claude-9b-think] 2026-10-27 UNTESTED (FINDINGS-FRAME-CALLS): the stacks of destroyed fibers, kept for the
// next Create instead of munmap now and mmap + mprotect then. claude-9b-test's s138 profile (mp_merv, 4 players
// moving): mmap, munmap, makecontext and the guard page's mprotect from Create / Destroy were about 1.6% of the
// game's main thread (~66 us a frame); multiplayer's scripts start and end many short threads. A kept stack
// keeps its guard page and the pages it has touched (MAP_NORESERVE: only those use memory), at most
// kKeptStacks of them; more go back to the system as before. Script threads run on the game's main thread
// only, but Destroy comes from the thread object's destructor, so the list has a lock (never contended).
constexpr size_t kKeptStacks = 64;
struct KeptStack {
  void* mem;
  size_t size;
};
std::mutex g_kept_mutex;
KeptStack g_kept[kKeptStacks];
size_t g_kept_count = 0;

// A kept stack of `size` bytes (guard page included), or nullptr.
void* TakeKeptStack(size_t size) {
  std::lock_guard<std::mutex> lock(g_kept_mutex);
  for (size_t i = g_kept_count; i-- > 0;) {
    if (g_kept[i].size != size) continue;
    void* mem = g_kept[i].mem;
    g_kept[i] = g_kept[--g_kept_count];
    return mem;
  }
  return nullptr;
}

// True if the stack was kept (not unmapped).
bool KeepStack(void* mem, size_t size) {
  std::lock_guard<std::mutex> lock(g_kept_mutex);
  if (g_kept_count == kKeptStacks) return false;
  g_kept[g_kept_count++] = {mem, size};
  return true;
}
}  // namespace

ScriptFiber* ScriptFiber::ForCurrentThread() {
  auto* f = new ScriptFiber();
  f->impl_ = new Impl();
  getcontext(&f->impl_->context);
  return f;
}

ScriptFiber* ScriptFiber::Create(Entry entry, void* arg, size_t stack_reserve) {
  const size_t page = size_t(sysconf(_SC_PAGESIZE));
  const size_t size = (stack_reserve + page - 1) / page * page + page;  // + guard page
  // [claude-9b-think] UNTESTED: a stack kept from a destroyed fiber first (its guard page is still set).
  void* mem = TakeKeptStack(size);
  if (!mem) {
    // MAP_NORESERVE: pages are only backed once the fiber actually uses them.
    mem = mmap(nullptr, size, PROT_READ | PROT_WRITE,
               MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE
#ifdef MAP_STACK
                   | MAP_STACK
#endif
               ,
               -1, 0);
    if (mem == MAP_FAILED) return nullptr;
    mprotect(mem, page, PROT_NONE);  // stacks grow down: overflow hits the guard
  }

  auto* f = new ScriptFiber();
  f->impl_ = new Impl();
  Impl* impl = f->impl_;
  impl->stack = mem;
  impl->stack_size = size;
  impl->entry = entry;
  impl->arg = arg;
  getcontext(&impl->context);
  impl->context.uc_stack.ss_sp = static_cast<char*>(mem) + page;
  impl->context.uc_stack.ss_size = size - page;
  impl->context.uc_link = nullptr;
  const std::uintptr_t p = reinterpret_cast<std::uintptr_t>(impl);
  makecontext(&impl->context, reinterpret_cast<void (*)()>(&PosixStart), 2, unsigned(p),
              unsigned(p >> 32));
  return f;
}

void ScriptFiber::Switch(ScriptFiber* from, ScriptFiber* to) {
  swapcontext(&from->impl_->context, &to->impl_->context);
}

void ScriptFiber::Destroy(ScriptFiber* fiber) {
  if (!fiber) return;
  // [claude-9b-think] UNTESTED: kept for the next Create while there is room (see KeepStack).
  if (fiber->impl_->stack && !KeepStack(fiber->impl_->stack, fiber->impl_->stack_size))
    munmap(fiber->impl_->stack, fiber->impl_->stack_size);
  delete fiber->impl_;
  delete fiber;
}

#endif

}  // namespace cod3
