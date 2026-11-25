#!/usr/bin/env python3
"""[acct2-fh] 2026-11-14 Finest Hour port: xboxrecomp's PsCreateSystemThreadEx honours CreateSuspended.

    python3 xboxrecomp_create_suspended.py XBOXRECOMP_DIR [--check]

Applies this project's change to the xboxrecomp checkout Finest Hour builds with (FH_DIR/xboxrecomp): one file,
src/kernel/kernel_bridge.c. Each edit replaces one exact piece of text, found exactly once, so a checkout that differs
from the one this was written against (xboxrecomp v0.16.0, f0f8a46, with fh/patches/xboxrecomp_guest_priority.py's
change, which touches other parts of the file) stops with a message instead of being changed in the wrong place. A
checkout that already has the change is left alone. --check says whether the change is there and changes nothing.
fh_gen3.sh runs this before each build, after xboxrecomp_guest_priority.py.

Why (dev/agents/acct2-fh/TEST-RUNS.md, acct2-fh-boot22 and boot34, both "signal 11 touching 0x77c" in the host's
pthread_mutex_lock a few seconds in):
  - PsCreateSystemThreadEx (kernel ordinal 255) takes CreateSuspended as its eighth argument. XAPI's CreateThread
    (0x00131BAB in Finest Hour) passes (dwCreationFlags >> 2) & 1, that is CREATE_SUSPENDED, there;
  - Finest Hour's Bink timer thread is made that way: 0x00254460 allocates 0x24 bytes, calls
    CreateThread(NULL, 0x3000, 0x002543F0, block, CREATE_SUSPENDED, &block[0x10]), then creates the two events at
    block[0x14] and block[8] and only then calls ResumeThread. The thread's routine 0x002543F0 waits on block[8];
  - the bridge read arguments 0, 3, 5, 6 and 9 and never 7, and bridge_spawn_thread started every host thread at once,
    so the routine could run before its creator had written the event handles and wait on whatever the allocation
    held. When it did, the host's WaitForSingleObject took that value for one of its objects and crashed. Whether it
    did depended on which thread the host ran first, so it was intermittent.

The change: bridge_PsCreateSystemThreadEx reads argument 7 and passes it through bridge_spawn_guest_thread to
bridge_spawn_thread, which gives CREATE_SUSPENDED to CreateThread. xboxrecomp's own CreateThread
(src/platform/win32_compat.c) already holds a thread made that way before its start routine until ResumeThread brings
its count to zero, and the guest's ResumeThread reaches it (NtResumeThread, ordinal 224, resolves the handle token
bridge_spawn_guest_thread wrote). PsCreateSystemThread (ordinal 254) has no CreateSuspended and passes 0. The worker
paths that run a routine on the calling thread (RECOMP_WORKERS=inline, or no thread stack left) are unchanged.
"""
import os
import sys

MARK = "[acct2-fh] CreateSuspended"

BRIDGE = [
    # 1. bridge_spawn_thread: what the new argument means, and the argument.
    (r"""static HANDLE bridge_spawn_thread(recomp_func_t fn, uint32_t ctx1,
                                  uint32_t ctx2, uint32_t stack_top,
                                  uint32_t obj_va, uint32_t tls_size)
{""",
     r"""/* """ + MARK + r""" (2026-11-14): suspended != 0 makes the host thread
 * wait, before it runs any guest code, until a ResumeThread (the guest's
 * NtResumeThread, ordinal 224) brings its count to zero, as
 * PsCreateSystemThreadEx's CreateSuspended does on the console. XAPI's
 * CreateThread passes CREATE_SUSPENDED there, and its caller fills in what the
 * thread will use before resuming it. */
static HANDLE bridge_spawn_thread(recomp_func_t fn, uint32_t ctx1,
                                  uint32_t ctx2, uint32_t stack_top,
                                  uint32_t obj_va, uint32_t tls_size,
                                  int suspended)
{"""),
    # 2. ... which reaches CreateThread.
    (r"""    th = CreateThread(NULL, 0, bridge_thread_main, s, 0, NULL);""",
     r"""    th = CreateThread(NULL, 0, bridge_thread_main, s,
                      suspended ? CREATE_SUSPENDED : 0, NULL);"""),
    # 3. bridge_spawn_guest_thread passes it on.
    (r"""static HANDLE bridge_spawn_guest_thread(recomp_func_t fn, uint32_t ctx1,
                                        uint32_t ctx2, uint32_t stack_top,
                                        uint32_t handle_va, uint32_t tls_size)
{""",
     r"""static HANDLE bridge_spawn_guest_thread(recomp_func_t fn, uint32_t ctx1,
                                        uint32_t ctx2, uint32_t stack_top,
                                        uint32_t handle_va, uint32_t tls_size,
                                        int suspended)
{"""),
    (r"""    th = bridge_spawn_thread(fn, ctx1, ctx2, stack_top, obj, tls_size);""",
     r"""    th = bridge_spawn_thread(fn, ctx1, ctx2, stack_top, obj, tls_size,
                             suspended);"""),
    # 4. PsCreateSystemThreadEx reads its CreateSuspended argument ...
    (r"""    uint32_t start_context2  = STACK_ARG(6);
    uint32_t start_routine   = STACK_ARG(9);""",
     r"""    uint32_t start_context2  = STACK_ARG(6);
    /* """ + MARK + r""": BOOLEAN CreateSuspended. Finest Hour's Bink
     * timer thread (made at 0x00254460, routine 0x002543F0) is created
     * suspended and waits on an event its creator writes after CreateThread
     * returns; started at once, it could wait on a handle not written yet.
     * A BOOLEAN: only its low byte counts. */
    uint32_t create_suspended = STACK_ARG(7) & 0xFFu;
    uint32_t start_routine   = STACK_ARG(9);"""),
    # 5. ... and gives it to the worker it spawns, and says so.
    (r"""                    bridge_spawn_guest_thread(fn, start_context1, start_context2,
                                              stack_top, xbox_handle_ptr,
                                              tls_size);
                    fprintf(stderr, "  [KERNEL] PsCreateSystemThreadEx: spawned "
                            "worker 0x%08X (ctx=0x%08X, stack top 0x%08X)\n",
                            start_routine, start_context1, stack_top);""",
     r"""                    bridge_spawn_guest_thread(fn, start_context1, start_context2,
                                              stack_top, xbox_handle_ptr,
                                              tls_size, create_suspended != 0);
                    fprintf(stderr, "  [KERNEL] PsCreateSystemThreadEx: spawned "
                            "worker 0x%08X (ctx=0x%08X, stack top 0x%08X%s)\n",
                            start_routine, start_context1, stack_top,
                            create_suspended ? ", suspended" : "");"""),
    # 6. PsCreateSystemThread has no such argument.
    (r"""                    bridge_spawn_guest_thread(fn, start_context1, start_context2,
                                              stack_top, xbox_handle_ptr, 0);""",
     r"""                    bridge_spawn_guest_thread(fn, start_context1, start_context2,
                                              stack_top, xbox_handle_ptr, 0,
                                              0 /* no CreateSuspended here */);"""),
]


def edit_pairs(path, pairs):
    text = open(path, encoding="utf-8", newline="").read()
    if MARK in text:
        return "already has the change"
    for old, new in pairs:
        n = text.count(old)
        if n != 1:
            sys.exit("%s: the text to change was found %d times (expected once):\n%s" % (path, n, old[:300]))
        text = text.replace(old, new)
    return text


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    root = sys.argv[1]
    check = "--check" in sys.argv[2:]
    path = os.path.join(root, "src", "kernel", "kernel_bridge.c")
    if not os.path.isfile(path):
        sys.exit("no %s" % path)
    result = edit_pairs(path, BRIDGE)
    name = os.path.relpath(path, root)
    if result == "already has the change":
        print("%s: already has the change" % name)
    elif check:
        print("%s: the change is not there yet (it applies cleanly)" % name)
    else:
        with open(path, "w", encoding="utf-8", newline="") as f:
            f.write(result)
        print("%s: changed" % name)


if __name__ == "__main__":
    main()
