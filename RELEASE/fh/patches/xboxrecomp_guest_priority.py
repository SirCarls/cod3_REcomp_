#!/usr/bin/env python3
"""[acct2-fh] 2026-11-13 Finest Hour port: thread priorities for xboxrecomp's guest CPU lock (RECOMP_GUEST_LOCK=1).

    python3 xboxrecomp_guest_priority.py XBOXRECOMP_DIR [--check]

Applies this project's change to the xboxrecomp checkout Finest Hour builds with (FH_DIR/xboxrecomp): three files,
src/kernel/kernel_guest_cpu.c, src/kernel/kernel_bridge.c and src/kernel/kernel.h. Each edit replaces one exact piece
of text, found exactly once, so a checkout that differs from the one this was written against (xboxrecomp v0.16.0,
f0f8a46) stops with a message instead of being changed in the wrong place. A checkout that already has the change is
left alone. --check says whether the change is there and changes nothing. fh_gen3.sh runs this before each build.

Why (dev/agents/acct2-fh/TEST-RUNS.md, acct2-fh-boot24 and boot25):
  - the guest CPU lock lets one guest thread run guest code at a time, as on the Xbox's one CPU, and hands it over
    when a waiter has waited a quantum (4 ms);
  - Finest Hour's level streamer (its thread routine at 0x00068D90, made by 0x00069450, which calls CreateThread and
    then SetThreadPriority(thread, THREAD_PRIORITY_LOWEST)) polls its request ring without ever blocking;
  - on the console the dispatcher always runs the highest-priority ready thread, so that thread ran only while the
    main thread waited. Under the lock it took a whole 4 ms turn after each of the main thread's turns, and the main
    thread waited for the CPU 30-40% of every level frame (boot24's profile; boot25's RECOMP_GUEST_CPU_TRACE showed
    the streamer's thread holding it 4.1 ms at a time and handing over at its loop's back edge 0x000691DB).

The change: the lock knows each guest thread's priority (KeSetBasePriorityThread, ordinal 143, records it against the
guest thread object), and a quantum hand-over goes only to a waiter whose priority is at least the holder's. A lower
waiter still gets the CPU whenever the holder blocks or yields (every wait, sleep and file read soft-releases it), and
a thread woken from a wait still asks for it at once, as before. RECOMP_GUEST_STARVE (ms, default 100) lets a lower
waiter ask anyway after waiting that long, so a title that spins on a lower thread's work without blocking still moves;
RECOMP_GUEST_PRIORITY=0 turns the change off (the quantum alone, as before).
"""
import os
import sys

MARK = "[acct2-fh] guest CPU priorities"

# --- src/kernel/kernel_guest_cpu.c ---------------------------------------------------------------------------------

GUEST_CPU = [
    # 1. The header: what the lock now does with priorities, and the two settings.
    (""" *  - NtYieldExecution and the thread's end hand over at once.
 *
 * RECOMP_GUEST_QUANTUM (ms, default 4) is the quantum; RECOMP_GUEST_CPU_TRACE
 * (ms) prints each turn held at least that long and where it ended.
 */""",
     """ *  - NtYieldExecution and the thread's end hand over at once.
 *  - """ + MARK + """ (2026-11-13): the console's dispatcher ran the
 *    highest-priority ready thread, so a thread never lost the CPU to a
 *    lower-priority one when its quantum ended. The lock now does the same:
 *    KeSetBasePriorityThread records a thread's priority, and the quantum
 *    hand-over (at a back edge or a kernel call) goes only to a waiter of at
 *    least the holder's priority. A lower waiter still runs whenever the
 *    holder blocks or yields, and a woken thread still asks at once. Finest
 *    Hour's level streamer (THREAD_PRIORITY_LOWEST, polling with no wait)
 *    took a 4 ms turn after each of the main thread's and cost it a third of
 *    every level frame without this.
 *
 * RECOMP_GUEST_QUANTUM (ms, default 4) is the quantum; RECOMP_GUEST_CPU_TRACE
 * (ms) prints each turn held at least that long and where it ended.
 * RECOMP_GUEST_PRIORITY=0 hands over by the quantum alone, whatever the
 * priorities; RECOMP_GUEST_STARVE (ms, default 100, 0 never) lets a lower
 * waiter ask for the CPU after waiting that long anyway, so a title that
 * spins on a lower thread's work without blocking still moves.
 */"""),

    # 2. The state: each thread's priority, the holder's, the waiters' by priority.
    ("""static uint64_t s_quantum_ns;               /* RECOMP_GUEST_QUANTUM */
""",
     """static uint64_t s_quantum_ns;               /* RECOMP_GUEST_QUANTUM */

/* """ + MARK + """. Priorities as Win32 numbers them, clamped to
 * -2 (lowest) .. 2 (highest); 0 is normal. A thread's own is looked up again
 * only when one changes (s_prio_gen). */
#define GUEST_PRIO_SLOTS 64
#define GUEST_PRIO_MIN   (-2)
#define GUEST_PRIO_MAX   2
static int s_priorities = 1;                /* RECOMP_GUEST_PRIORITY */
static uint64_t s_starve_ns;                /* RECOMP_GUEST_STARVE */
static volatile LONG s_prio_va[GUEST_PRIO_SLOTS];   /* guest thread objects not at normal */
static volatile LONG s_prio_of[GUEST_PRIO_SLOTS];   /* and their priorities */
static volatile LONG s_prio_gen;            /* bumped at each change */
static volatile LONG s_prio_lines;          /* the "priority" lines printed */
static volatile LONG s_owner_prio;          /* the holder's priority */
static unsigned s_waiting_at[GUEST_PRIO_MAX - GUEST_PRIO_MIN + 1];  /* under s_lock: waiters by priority */
"""),

    # 3. The thread-locals: the thread's own priority, cached.
    ("""static XBOX_THREAD_LOCAL uint32_t t_soft_where;
""",
     """static XBOX_THREAD_LOCAL uint32_t t_soft_where;
static XBOX_THREAD_LOCAL LONG t_prio_gen = -1;  /* s_prio_gen when t_prio was read */
static XBOX_THREAD_LOCAL LONG t_prio;           /* this thread's priority */
"""),

    # 4. The settings.
    ("""    long qms = q && *q ? strtol(q, NULL, 0) : 4;

    s_on = e && *e == '1';""",
     """    long qms = q && *q ? strtol(q, NULL, 0) : 4;
    const char *pr = getenv("RECOMP_GUEST_PRIORITY");
    const char *st = getenv("RECOMP_GUEST_STARVE");
    long sms = st && *st ? strtol(st, NULL, 0) : 100;

    s_priorities = !(pr && *pr == '0');
    s_starve_ns = (uint64_t)(sms > 0 ? sms : 0) * 1000000ull;
    s_on = e && *e == '1';"""),
    ("""        fprintf(stderr, "  [KERNEL] guest CPU lock on: one guest thread runs"
                        " guest code at a time, %lu ms quantum\\n",
                (unsigned long)(s_quantum_ns / 1000000ull));""",
     """        fprintf(stderr, "  [KERNEL] guest CPU lock on: one guest thread runs"
                        " guest code at a time, %lu ms quantum; priorities %s"
                        " (a lower waiter asks anyway after %lu ms)\\n",
                (unsigned long)(s_quantum_ns / 1000000ull),
                s_priorities ? "on" : "off (RECOMP_GUEST_PRIORITY=0)",
                (unsigned long)(s_starve_ns / 1000000ull));"""),

    # 5. The helpers, before the trace that prints a thread's priority.
    ("""/* Where a turn ended: the kernel ordinal called, the loop back edge's VA,""",
     """/* """ + MARK + """: the calling thread's priority, from the table
 * KeSetBasePriorityThread fills (xbox_GuestCpuSetPriority), keyed by the
 * guest thread object; normal when it is not there. */
static LONG my_priority(void)
{
    LONG gen = s_prio_gen;

    if (gen != t_prio_gen) {
        uint32_t va = xbox_GuestThreadObject();
        int i;

        t_prio_gen = gen;
        t_prio = 0;
        for (i = 0; va && i < GUEST_PRIO_SLOTS; i++)
            if ((uint32_t)s_prio_va[i] == va) {
                t_prio = s_prio_of[i];
                break;
            }
    }
    return t_prio;
}

/* A queued waiter asks for the CPU once it has waited its quantum, unless
 * the holder outranks it; RECOMP_GUEST_STARVE lets it ask after that long
 * anyway. */
static int may_ask_for_turn(uint64_t t0)
{
    uint64_t waited = xbox_HostNowNs() - t0;

    if (waited < s_quantum_ns)
        return 0;
    if (!s_priorities || my_priority() >= s_owner_prio)
        return 1;
    return s_starve_ns && waited >= s_starve_ns;
}

/* The holder's quantum hand-over at a kernel call: only when a waiter has at
 * least the holder's priority (read without the lock: a hint, as s_waiting
 * is where it is read the same way). */
static int a_waiter_ranks(void)
{
    LONG p;

    if (!s_priorities)
        return 1;
    for (p = my_priority(); p <= GUEST_PRIO_MAX; p++)
        if (s_waiting_at[p - GUEST_PRIO_MIN])
            return 1;
    return 0;
}

/* Where a turn ended: the kernel ordinal called, the loop back edge's VA,"""),

    # 6. The trace says whose priority the turn ran at.
    ("""            fprintf(stderr, "  [GUEST] cpu held %.2f ms by tid %lu, released at"
                    " %s\\n", us / 1000.0, (unsigned long)GetCurrentThreadId(),
                    where_str(kind, where, w, sizeof w));""",
     """            fprintf(stderr, "  [GUEST] cpu held %.2f ms by tid %lu (priority"
                    " %ld), released at %s\\n", us / 1000.0,
                    (unsigned long)GetCurrentThreadId(), (long)my_priority(),
                    where_str(kind, where, w, sizeof w));"""),

    # 7. A waiter is counted by priority while it waits, and asks by may_ask_for_turn.
    ("""        my = s_next_ticket++;
        s_waiting++;
        t0 = xbox_HostNowNs();""",
     """        my = s_next_ticket++;
        s_waiting++;
        prio = my_priority();
        s_waiting_at[prio - GUEST_PRIO_MIN]++;
        t0 = xbox_HostNowNs();"""),
    ("""            if (!preempt_get() && !(s_free && my == s_head)
                && (barge || xbox_HostNowNs() - t0 >= s_quantum_ns))
                preempt_set(1);
        }
        s_free = 0;
        s_granted = 0;
        s_head++;
        s_waiting--;
    }
    InterlockedExchange(&s_owner_tid, (LONG)GetCurrentThreadId());""",
     """            if (!preempt_get() && !(s_free && my == s_head)
                && (barge || may_ask_for_turn(t0)))
                preempt_set(1);
        }
        s_free = 0;
        s_granted = 0;
        s_head++;
        s_waiting--;
        s_waiting_at[prio - GUEST_PRIO_MIN]--;
    }
    InterlockedExchange(&s_owner_tid, (LONG)GetCurrentThreadId());
    InterlockedExchange(&s_owner_prio, my_priority());"""),
    ("""static void guest_cpu_acquire(int barge)
{
    unsigned my;
    uint64_t t0 = 0;""",
     """static void guest_cpu_acquire(int barge)
{
    unsigned my;
    uint64_t t0 = 0;
    LONG prio = 0;"""),

    # 8. The quantum hand-over at a kernel call asks a_waiter_ranks.
    ("""                && (preempt_get()
                    || xbox_HostNowNs() - t_turn_ns >= s_quantum_ns)))
            guest_cpu_handoff(kind, where);""",
     """                && (preempt_get()
                    || (xbox_HostNowNs() - t_turn_ns >= s_quantum_ns
                        && a_waiter_ranks()))))
            guest_cpu_handoff(kind, where);"""),

    # 9. A thread that ends forgets its priority (its guest object is used again).
    ("""void xbox_GuestCpuPart(void)
{
    if (t_held)
        guest_cpu_handoff(XBOX_GUEST_CPU_EXIT, 0);
    t_guest = 0;
}""",
     """void xbox_GuestCpuPart(void)
{
    if (t_held)
        guest_cpu_handoff(XBOX_GUEST_CPU_EXIT, 0);
    t_guest = 0;
    xbox_GuestCpuSetPriority(xbox_GuestThreadObject(), 0);  /* """ + MARK + """ */
}"""),

    # 10. The entry point KeSetBasePriorityThread calls.
    ("""unsigned long xbox_GuestCpuOwner(void)
{""",
     """/* """ + MARK + """: KeSetBasePriorityThread (kernel_bridge.c) records a
 * guest thread object's priority here; 0 (normal) takes it off the table. */
void xbox_GuestCpuSetPriority(uint32_t thread_va, LONG increment)
{
    LONG p = increment < GUEST_PRIO_MIN ? GUEST_PRIO_MIN
           : increment > GUEST_PRIO_MAX ? GUEST_PRIO_MAX : increment;
    int i, slot = -1, changed = 0;

    if (!thread_va)
        return;
    InitOnceExecuteOnce(&s_once, guest_cpu_init, NULL, NULL);
    EnterCriticalSection(&s_lock);
    for (i = 0; i < GUEST_PRIO_SLOTS; i++) {
        if ((uint32_t)s_prio_va[i] == thread_va) {
            slot = i;
            break;
        }
        if (!s_prio_va[i] && slot < 0 && p)
            slot = i;
    }
    if (slot >= 0 && ((uint32_t)s_prio_va[slot] != thread_va || s_prio_of[slot] != p)) {
        s_prio_va[slot] = p ? (LONG)thread_va : 0;
        s_prio_of[slot] = p;
        InterlockedIncrement(&s_prio_gen);
        changed = 1;
    }
    LeaveCriticalSection(&s_lock);
    if (changed && p && s_on && InterlockedIncrement(&s_prio_lines) <= 32) {
        fprintf(stderr, "  [KERNEL] guest thread object 0x%08X at priority %ld:"
                " %s\\n", thread_va, (long)p,
                p < 0 ? "it gets the CPU when the threads above it block"
                      : "it takes its turn before lower threads");
        fflush(stderr);
    }
}

unsigned long xbox_GuestCpuOwner(void)
{"""),
]

# --- src/kernel/kernel_bridge.c ------------------------------------------------------------------------------------

BRIDGE = [
    ("""static void bridge_KeSetBasePriorityThread(void)
{
    g_eax = (uint32_t)xbox_KeSetBasePriorityThread(
        XBOX_TO_NATIVE(STACK_ARG(0)), (LONG)STACK_ARG(1));
}""",
     """static void bridge_KeSetBasePriorityThread(void)
{
    g_eax = (uint32_t)xbox_KeSetBasePriorityThread(
        XBOX_TO_NATIVE(STACK_ARG(0)), (LONG)STACK_ARG(1));
    /* """ + MARK + """: the guest CPU lock hands a quantum over only to
     * a thread of at least the holder's priority (kernel_guest_cpu.c). The
     * argument is the guest thread object (ObReferenceObjectByHandle). */
    xbox_GuestCpuSetPriority(STACK_ARG(0), (LONG)STACK_ARG(1));
}"""),
    ("""static uint32_t thobj_current(void)
{
    if (!g_thread_obj_va)
        g_thread_obj_va = thobj_alloc();
    return g_thread_obj_va;
}""",
     """static uint32_t thobj_current(void)
{
    if (!g_thread_obj_va)
        g_thread_obj_va = thobj_alloc();
    return g_thread_obj_va;
}

/* """ + MARK + """: the calling thread's guest object, 0 when it has
 * none yet (none is made here), for kernel_guest_cpu.c's priority table. */
uint32_t xbox_GuestThreadObject(void)
{
    return g_thread_obj_va;
}"""),
]

# --- src/kernel/kernel.h -------------------------------------------------------------------------------------------

HEADER_ANCHOR = "unsigned long xbox_GuestCpuOwner(void);"
HEADER_ADD = """
/* """ + MARK + """ (kernel_guest_cpu.c, kernel_bridge.c): a guest thread
 * object's priority for the lock's hand-overs, and the calling thread's object. */
void     xbox_GuestCpuSetPriority(uint32_t thread_va, LONG increment);
uint32_t xbox_GuestThreadObject(void);"""


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


def edit_header(path):
    text = open(path, encoding="utf-8", newline="").read()
    if MARK in text:
        return "already has the change"
    n = text.count(HEADER_ANCHOR)
    if n != 1:
        sys.exit("%s: %r found %d times (expected once)" % (path, HEADER_ANCHOR, n))
    end = text.index("\n", text.index(HEADER_ANCHOR))
    return text[:end] + HEADER_ADD + text[end:]


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    root = sys.argv[1]
    check = "--check" in sys.argv[2:]
    kernel = os.path.join(root, "src", "kernel")
    jobs = [
        (os.path.join(kernel, "kernel_guest_cpu.c"), lambda p: edit_pairs(p, GUEST_CPU)),
        (os.path.join(kernel, "kernel_bridge.c"), lambda p: edit_pairs(p, BRIDGE)),
        (os.path.join(kernel, "kernel.h"), edit_header),
    ]
    results = []
    for path, fn in jobs:            # every file is checked before any is written
        if not os.path.isfile(path):
            sys.exit("no %s" % path)
        results.append((path, fn(path)))
    for path, result in results:
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
