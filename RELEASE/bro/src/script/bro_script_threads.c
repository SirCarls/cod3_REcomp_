/* Big Red One's script threads, done natively.
 *
 * The level scripts are C++ compiled into the program. A script thread is a function that runs until it waits
 * ("wait 1.5", "waittill ..."), and the game pauses and resumes threads by copying stacks and jumping:
 *   - Scr_ResumeThread (0x002FE100) saves its registers in a context (0x0092709C, through 0x002F5690, like setjmp).
 *     Then it either starts the thread with Scr_ThreadEntry (0x001FA400) 8 KB below its own frame, or copies a
 *     paused thread's stack back (Scr_RestoreThreadStack 0x002F6600) and jumps into it
 *     (mov esp; pop fs:[0]; popa; popfw; ret).
 *   - A wait (Scr_SuspendWaitTime/Frames/Till 0x0011D950/990/9D0) pushes the flags, every register and the SEH
 *     chain head. It saves the stack from there up to the thread's base (Scr_SaveThreadLocals), records the wait
 *     (Scr_WaitRecord15/16/17), and jumps back to the context (0x002F5660, like longjmp). It never returns there:
 *     the resume returns to the script in its place.
 *   - A thread being ended (endon, or the level ending) is resumed with g_scrThreadAborting set. The wait's caller
 *     then throws a C++ exception, and Scr_ThreadEntry's catch (...) {} stops it.
 *
 * Recompiled, every function is a C function with its own native frame, so copying the guest stack back can't bring
 * a thread back: the native frames of the script functions it was in are gone, and the jumps can't be translated.
 * Here each script thread runs on its own native stack (a coroutine: ucontext, or a fiber on Windows), and the
 * jumps become switches between stacks. The game's own bookkeeping is unchanged. Its stack copies still happen, at
 * the same guest addresses, so everything the game reads about a thread (its flags, its saved stack, its waits) is
 * as the original leaves it. */
#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>
#else
#include <sys/mman.h>
#include <ucontext.h>
#include <unistd.h>
#endif

#include "recomp_types.h"
#include "xbox_memory_layout.h"     /* XBOX_PTR: guest memory */
#include "bro_script_threads.h"

/* The game's functions this calls (generated) */
void sub_001FA400(void);    /* Scr_ThreadEntry: try { thread->Run(); } catch (...) {} (thread in ecx) */
void sub_002FC440(void);    /* Scr_ResumeThread's first call, with the thread in eax and esi */
void sub_002F6600(void);    /* Scr_RestoreThreadStack: ecx = where the stack goes, eax = the saved pieces */

/* The game's data */
#define CURRENT_THREAD      0x008DDA2Cu   /* the script thread running now, 0 between threads */
#define SCRIPT_STATE        0x0095F49Cu   /* -> a block whose byte at +0x24 gets the thread's flag 0x40 */
#define SAVE_THREAD_LOCALS  0x00922174u   /* -> Scr_SaveThreadLocals (the script interface's function table) */
#define WAIT_RECORD_TIME    0x0092217Cu   /* -> Scr_WaitRecord15 */
#define WAIT_RECORD_FRAMES  0x00922180u   /* -> Scr_WaitRecord16 */
#define WAIT_RECORD_TILL    0x00922184u   /* -> Scr_WaitRecord17 */
#define WAIT_TIME           0x00922124u   /* the wait's argument, stored by its caller */
#define WAIT_FRAMES         0x00922128u

/* A thread object's fields */
#define THREAD_RUNNABLE     0x0Cu   /* the object Scr_ThreadEntry runs (its vtable's second entry) */
#define THREAD_FLAGS        0x10u
#define THREAD_STACK_BASE   0x28u   /* the top of its stack: 8 KB below Scr_ResumeThread's frame */
#define THREAD_SAVED        0x2Cu   /* its saved stack, in 252-byte pieces */
#define THREAD_SAVED_SIZE   0x30u

/* The flags Scr_ResumeThread tests */
#define FLAG_START          0x01u   /* not started yet */
#define FLAG_WAITING        0x02u   /* paused in a wait (a wait record sets 0x212) */
#define FLAG_WOKEN          0x04u   /* its wait is over */
#define FLAG_ENDED          0x08u   /* set when Scr_ThreadEntry returns */
#define FLAG_IN_WAIT        0x10u
#define FLAG_DONT_RUN       0x40u   /* end it without running it */

#define THREAD_STACK_GAP    0x2000u     /* sub esp, 0x2000 before Scr_ThreadEntry */
#define NATIVE_STACK_BYTES  (4u << 20)  /* each thread's native stack; untouched pages cost nothing */

/* ---- coroutines ------------------------------------------------------------------------------------------- */

struct coroutine {
#if defined(_WIN32)
    void *fiber;
#else
    ucontext_t context;
    void *stack;
#endif
};

struct script_thread {
    uint32_t guest;                 /* the game's thread object */
    struct script_thread *next;     /* in its bucket of the table */
    struct coroutine own;           /* runs the thread, on its own native stack */
    struct coroutine *resumer;      /* where to switch back to when it waits or ends */
    jmp_buf after_wait;             /* in scr_suspend: where a resumed thread carries on */
    uint32_t resume_esp;            /* the guest esp its resume sets: its pushed registers */
    int in_wait;                    /* between recording a wait and jumping back to the scheduler */
    int ended;
};

static RECOMP_TLS struct script_thread *s_current;     /* the thread running now on this native thread */
static void thread_main(struct script_thread *t);

#if defined(_WIN32)
static RECOMP_TLS void *s_scheduler_fiber;

static void CALLBACK fiber_entry(void *param)
{
    thread_main((struct script_thread *)param);
}

static int coroutine_create(struct coroutine *c, struct script_thread *t)
{
    if (!s_scheduler_fiber)
        s_scheduler_fiber = IsThreadAFiber() ? GetCurrentFiber() : ConvertThreadToFiber(NULL);
    c->fiber = CreateFiberEx(64 * 1024, NATIVE_STACK_BYTES, 0, fiber_entry, t);
    return c->fiber != NULL;
}

static void coroutine_switch(struct coroutine *from, struct coroutine *to)
{
    from->fiber = GetCurrentFiber();
    SwitchToFiber(to->fiber);
}

static void coroutine_free(struct coroutine *c)
{
    DeleteFiber(c->fiber);
}
#else
enum { SPARE_STACK_LIMIT = 32 };
static void *s_spare_stacks[SPARE_STACK_LIMIT];     /* kept for the next threads: threads start all the time */
static int s_spare_stack_count;
static RECOMP_TLS struct script_thread *s_starting; /* makecontext can't pass a pointer portably */

static void context_entry(void)
{
    thread_main(s_starting);
}

static int coroutine_create(struct coroutine *c, struct script_thread *t)
{
    void *stack = s_spare_stack_count ? s_spare_stacks[--s_spare_stack_count] : NULL;

    if (!stack) {
        stack = mmap(NULL, NATIVE_STACK_BYTES, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE,
                     -1, 0);
        if (stack == MAP_FAILED)
            return 0;
        mprotect(stack, (size_t)sysconf(_SC_PAGESIZE), PROT_NONE);  /* a guard page under the stack */
    }
    c->stack = stack;
    if (getcontext(&c->context) != 0)
        return 0;
    c->context.uc_stack.ss_sp = stack;
    c->context.uc_stack.ss_size = NATIVE_STACK_BYTES;
    c->context.uc_link = NULL;
    makecontext(&c->context, context_entry, 0);
    s_starting = t;
    return 1;
}

static void coroutine_switch(struct coroutine *from, struct coroutine *to)
{
    swapcontext(&from->context, &to->context);
}

static void coroutine_free(struct coroutine *c)
{
    if (s_spare_stack_count < SPARE_STACK_LIMIT)
        s_spare_stacks[s_spare_stack_count++] = c->stack;
    else
        munmap(c->stack, NATIVE_STACK_BYTES);
}
#endif

/* ---- the threads, by guest address ------------------------------------------------------------------------ */

enum { TABLE_SIZE = 1024 };
static struct script_thread *s_table[TABLE_SIZE];
static unsigned long s_started, s_resumed, s_ended, s_thrown, s_abandoned;

static struct script_thread **bucket(uint32_t guest)
{
    return &s_table[(guest >> 4) & (TABLE_SIZE - 1)];
}

static struct script_thread *find(uint32_t guest)
{
    struct script_thread *t;

    for (t = *bucket(guest); t; t = t->next)
        if (t->guest == guest)
            return t;
    return NULL;
}

static void forget(struct script_thread *t)
{
    struct script_thread **link;

    for (link = bucket(t->guest); *link; link = &(*link)->next)
        if (*link == t) {
            *link = t->next;
            break;
        }
    coroutine_free(&t->own);
    free(t);
}

static int trace_on(void)
{
    static int on = -1;

    if (on < 0) {
        const char *v = getenv("BRO_SCRIPT_TRACE");
        on = v && *v && *v != '0';
    }
    return on;
}

/* The first few of each event, every one with BRO_SCRIPT_TRACE=1, and a count now and then */
static void note(const char *what, unsigned long count, uint32_t guest)
{
    if (count <= 4 || trace_on())
        fprintf(stderr, "[script] %s thread 0x%08X (#%lu)\n", what, guest, count);
    if (count % 1000 == 0)
        fprintf(stderr, "[script] threads: %lu started, %lu resumes, %lu ended, %lu ended by a throw, "
                "%lu abandoned\n", s_started, s_resumed, s_ended, s_thrown, s_abandoned);
}

/* BRO_SCRIPT_WATCH=ADDRESS: report when the guest word there changes, and at which step of a script thread, to
 * find what writes over it (a stack copy put back at the wrong place, or the thread's own code) */
static void watch(const char *step, uint32_t guest)
{
    static int state = -1;
    static uint32_t address, value;
    uint32_t now;

    if (state < 0) {
        const char *v = getenv("BRO_SCRIPT_WATCH");
        state = v && *v;
        if (state) {
            address = (uint32_t)strtoul(v, NULL, 0);
            value = MEM32(address);
            fprintf(stderr, "[script] watching 0x%08X (0x%08X)\n", address, value);
        }
    }
    if (!state)
        return;
    now = MEM32(address);
    if (now != value) {
        fprintf(stderr, "[script] watch: 0x%08X changed 0x%08X -> 0x%08X at %s, thread 0x%08X\n", address, value,
                now, step, guest);
        value = now;
    }
}

/* ---- guest stack and calls -------------------------------------------------------------------------------- */

static void push32(uint32_t value)
{
    g_esp -= 4u;
    MEM32(g_esp) = value;
}

static uint32_t pop32(void)
{
    uint32_t value = MEM32(g_esp);

    g_esp += 4u;
    return value;
}

static void call_direct(recomp_func_t fn, uint32_t return_address)
{
    push32(return_address);
    fn();
}

static void call_through(uint32_t pointer, uint32_t return_address)
{
    uint32_t va = MEM32(pointer);
    recomp_func_t fn = recomp_lookup_manual(va);

    if (!fn)
        fn = recomp_lookup(va);
    if (!fn) {
        fprintf(stderr, "[script] no code at 0x%08X (called through 0x%08X)\n", va, pointer);
        abort();
    }
    push32(return_address);
    fn();
}

/* Switches to the thread until it waits or ends; returns whether it ended. The x87 registers are this side's. */
static int run(struct script_thread *t)
{
    struct coroutine here;
    struct script_thread *outer = s_current;
    double fp_stack[8];
    int fp_top = g_fp_top;

    memcpy(fp_stack, (const void *)g_fp_stack, sizeof fp_stack);
    t->resumer = &here;
    s_current = t;
    coroutine_switch(&here, &t->own);
    s_current = outer;
    memcpy((void *)g_fp_stack, fp_stack, sizeof fp_stack);
    g_fp_top = fp_top;
    return t->ended;
}

/* A new thread's native stack starts here: Scr_ThreadEntry at the thread's base, as Scr_ResumeThread calls it */
static void thread_main(struct script_thread *t)
{
    g_esp = MEM32(t->guest + THREAD_STACK_BASE);    /* sub esp, 0x2000 */
    g_ecx = MEM32(t->guest + THREAD_RUNNABLE);
    g_esi = t->guest;
    call_direct(sub_001FA400, 0x002FE17Bu);
    t->ended = 1;
    coroutine_switch(&t->own, t->resumer);
    abort();    /* an ended thread is never switched to again */
}

/* ---- Scr_ResumeThread ------------------------------------------------------------------------------------- */

static int start_thread(uint32_t guest)
{
    struct script_thread *t = find(guest);
    struct script_thread **link;

    if (t) {
        /* The object was reused while its last thread was still paused: the game dropped that one */
        s_abandoned++;
        forget(t);
    }
    t = calloc(1, sizeof *t);
    if (!t || !coroutine_create(&t->own, t)) {
        fprintf(stderr, "[script] no native stack for thread 0x%08X\n", guest);
        abort();
    }
    t->guest = guest;
    link = bucket(guest);
    t->next = *link;
    *link = t;
    note("start", ++s_started, guest);
    if (!run(t))
        return 0;
    note("end", ++s_ended, guest);
    forget(t);
    return 1;
}

static int resume_thread(uint32_t guest, uint32_t resume_esp)
{
    struct script_thread *t = find(guest);

    if (!t) {
        fprintf(stderr, "[script] resume of thread 0x%08X, which has no native stack: ending it\n", guest);
        return 1;
    }
    t->resume_esp = resume_esp;
    if (++s_resumed <= 4 || trace_on())
        fprintf(stderr, "[script] resume thread 0x%08X (#%lu)\n", guest, s_resumed);
    if (!run(t))
        return 0;
    note("end", ++s_ended, guest);
    forget(t);
    return 1;
}

/* Scr_ResumeThread 0x002FE100 (thread in eax): starts or resumes it, and returns when it waits or ends */
void scr_resume_thread(void)
{
    uint32_t thread = g_eax;
    uint32_t entry_esp = g_esp;         /* [esp]: the return address */
    uint32_t ebx0 = g_ebx, esi0 = g_esi, edi0 = g_edi, ebp0 = g_ebp, seh_ebp0 = g_seh_ebp;
    uint32_t frame = entry_esp - 20u;   /* esp after push ebp, ecx, ebx, esi, edi */
    uint32_t flags;

    /* The frame the original leaves in guest memory */
    MEM32(entry_esp - 4u) = ebp0;
    MEM32(entry_esp - 8u) = g_ecx;
    MEM32(entry_esp - 12u) = ebx0;
    MEM32(entry_esp - 16u) = esi0;
    MEM32(entry_esp - 20u) = edi0;
    g_ebp = g_seh_ebp = entry_esp - 4u;
    g_esp = frame;
    g_esi = thread;
    MEM32(CURRENT_THREAD) = thread;
    call_direct(sub_002FC440, 0x002FE114u);
    flags = MEM32(thread + THREAD_FLAGS);
    MEM8(MEM32(SCRIPT_STATE) + 0x24u) = (uint8_t)((flags >> 6) & 1u);

    /* Here the original saves its registers (0x002F5690). A thread that waits jumps back to just after that, and
     * the flags are looked at again; with the wait recorded, that ends this call. */
    for (;;) {
        g_esp = frame;
        g_ebp = g_seh_ebp = entry_esp - 4u;
        g_esi = thread;
        flags = MEM32(thread + THREAD_FLAGS);
        if ((flags & FLAG_START) && !(flags & FLAG_IN_WAIT)) {
            flags &= ~FLAG_START;
            MEM32(thread + THREAD_FLAGS) = flags;
            flags &= ~FLAG_WOKEN;
            MEM32(thread + THREAD_FLAGS) = flags;
            if (!(flags & FLAG_DONT_RUN)) {
                MEM32(thread + THREAD_STACK_BASE) = frame - THREAD_STACK_GAP;
                watch("before start", thread);
                if (!start_thread(thread)) {
                    watch("new thread waited", thread);
                    continue;
                }
                watch("new thread ended", thread);
            }
            MEM32(thread + THREAD_FLAGS) |= FLAG_ENDED;
            break;
        }
        if ((flags & FLAG_WAITING) && (flags & FLAG_WOKEN)) {
            uint32_t base = frame - THREAD_STACK_GAP;
            uint32_t size = MEM32(thread + THREAD_SAVED_SIZE);
            uint32_t to = base - size;

            flags &= ~FLAG_WAITING;
            MEM32(thread + THREAD_FLAGS) = flags;
            flags &= ~FLAG_WOKEN;
            MEM32(thread + THREAD_FLAGS) = flags;
            MEM32(thread + THREAD_STACK_BASE) = base;
            if (size == 0 || size > 0x40000u) {
                /* Putting this back would write over memory far below the stack */
                fprintf(stderr, "[script] thread 0x%08X has a saved stack of %u bytes (flags 0x%08X): ending it\n",
                        thread, size, flags);
                MEM32(thread + THREAD_FLAGS) |= FLAG_ENDED;
                break;
            }
            watch("before restore", thread);
            g_ecx = to;
            g_eax = thread + THREAD_SAVED;
            call_direct(sub_002F6600, 0x002FE1E0u);
            watch("restore", thread);
            if (!resume_thread(thread, to)) {
                watch("resumed thread waited", thread);
                continue;
            }
            watch("resumed thread ended", thread);
            MEM32(thread + THREAD_FLAGS) |= FLAG_ENDED;
            break;
        }
        break;
    }
    MEM32(CURRENT_THREAD) = 0;
    g_ebx = ebx0;
    g_esi = esi0;
    g_edi = edi0;
    g_ebp = ebp0;
    g_seh_ebp = seh_ebp0;
    g_esp = entry_esp + 4u;             /* ret */
}

/* ---- the waits -------------------------------------------------------------------------------------------- */

enum wait_kind { WAIT_KIND_TIME, WAIT_KIND_FRAMES, WAIT_KIND_TILL };

/* Scr_SuspendWait*: pause the running thread, and return to the script when Scr_ResumeThread resumes it */
static void scr_suspend(enum wait_kind kind, uint32_t slot, uint32_t record, uint32_t after_save,
                        uint32_t after_record)
{
    struct script_thread *t = s_current;
    uint32_t seh_ebp = g_seh_ebp;
    uint32_t pushed_esp, saved;

    if (!t) {
        static int reported;
        if (!reported++)
            fprintf(stderr, "[script] a wait outside a script thread (0x%08X): going on at once\n", MEM32(record));
        g_esp += 4u;    /* ret */
        return;
    }

    /* pushfw; pusha; push fs:[0]; mov [slot], esp */
    g_esp -= 2u;
    MEM16(g_esp) = 0x0002u;
    pushed_esp = g_esp;
    push32(g_eax);
    push32(g_ecx);
    push32(g_edx);
    push32(g_ebx);
    push32(pushed_esp);
    push32(g_ebp);
    push32(g_esi);
    push32(g_edi);
    push32(MEM32(XBOX_FS_BASE));
    saved = g_esp;
    MEM32(slot) = saved;

    /* sub esp, 0x80; mov ebp, esp; push [slot]; call [Scr_SaveThreadLocals]; add esp, 4; add esp, 0x80 */
    g_esp -= 0x80u;
    g_ebp = g_seh_ebp = g_esp;
    g_eax = saved;
    push32(saved);
    call_through(SAVE_THREAD_LOCALS, after_save);
    g_esp += 4u + 0x80u;
    {
        uint32_t base = MEM32(t->guest + THREAD_STACK_BASE), size = MEM32(t->guest + THREAD_SAVED_SIZE);
        if (base - saved != size || size > 0x40000u)
            fprintf(stderr, "[script] thread 0x%08X saved %u bytes from 0x%08X, but its base is 0x%08X\n", t->guest,
                    size, saved, base);
    }
    watch("save", t->guest);

    /* The wait's arguments, then call [Scr_WaitRecordN], which jumps back to the scheduler */
    if (kind == WAIT_KIND_TILL) {
        g_ecx = MEM32(0x00922138u);
        g_eax = MEM32(0x00922E60u);
        g_edx = MEM8(0x00922E5Cu);
        push32(g_ecx);
        g_ecx = MEM32(0x0092212Cu);
        push32(g_edx);
        g_edx = MEM32(0x00922120u);
        push32(g_eax);
        g_eax = MEM32(0x00922130u);
        push32(g_ecx);
        g_ecx = MEM32(0x00922E6Cu);
        push32(g_edx);
        g_edx = MEM32(0x00922134u);
        push32(g_eax);
        push32(g_ecx);
        push32(g_edx);
    } else {
        g_ecx = MEM32(kind == WAIT_KIND_TIME ? WAIT_TIME : WAIT_FRAMES);
        push32(g_ecx);
    }
    if (setjmp(t->after_wait) == 0) {
        t->in_wait = 1;
        call_through(record, after_record);
        /* A wait record always jumps back to the scheduler; this one returned */
        t->in_wait = 0;
        fprintf(stderr, "[script] the wait record 0x%08X returned instead of pausing the thread\n", MEM32(record));
        t->resume_esp = saved;
    }

    /* Resumed: Scr_ResumeThread copied the stack back and set esp to the pushed registers.
     * pop fs:[0]; popa; popfw; ret */
    g_esp = t->resume_esp;
    MEM32(XBOX_FS_BASE) = pop32();
    g_edi = pop32();
    g_esi = pop32();
    g_ebp = pop32();
    g_esp += 4u;
    g_ebx = pop32();
    g_edx = pop32();
    g_ecx = pop32();
    g_eax = pop32();
    g_esp += 2u;
    g_seh_ebp = seh_ebp;
    g_esp += 4u;
}

void scr_suspend_wait_time(void)
{
    scr_suspend(WAIT_KIND_TIME, 0x00922ED4u, WAIT_RECORD_TIME, 0x0011D974u, 0x0011D98Au);
}

void scr_suspend_wait_frames(void)
{
    scr_suspend(WAIT_KIND_FRAMES, 0x00922ED8u, WAIT_RECORD_FRAMES, 0x0011D9B4u, 0x0011D9CAu);
}

void scr_suspend_wait_till(void)
{
    scr_suspend(WAIT_KIND_TILL, 0x00922EDCu, WAIT_RECORD_TILL, 0x0011D9F4u, 0x0011DA3Bu);
}

/* 0x002F5660 (the context in [esp+4]): the end of every wait record. Switch back to Scr_ResumeThread; when it
 * resumes this thread, carry on in scr_suspend after the wait. */
void scr_jump_to_scheduler(void)
{
    struct script_thread *t = s_current;

    if (!t || !t->in_wait) {
        fprintf(stderr, "[script] the jump back to the scheduler (0x002F5660) %s\n",
                t ? "outside a wait" : "outside a script thread");
        abort();
    }
    t->in_wait = 0;
    coroutine_switch(&t->own, t->resumer);
    longjmp(t->after_wait, 1);
}

/* _CxxThrowException 0x001F2302 (object, type; ret 8). In a script thread it is how the thread is ended: the
 * wait's caller throws when g_scrThreadAborting is set, and Scr_ThreadEntry's catch (...) {} stops it, after which
 * Scr_ThreadEntry returns normally. Destructors of the frames in between are not run. */
void scr_cxx_throw(void)
{
    struct script_thread *t = s_current;
    uint32_t object = MEM32(g_esp + 4u), type = MEM32(g_esp + 8u);

    if (t) {
        /* Scr_ThreadEntry's frame is just under the thread's base (push ebp after the call); its SEH record is at
         * ebp - 0x0C, and its return puts back the chain head that record points to */
        uint32_t entry_ebp = MEM32(t->guest + THREAD_STACK_BASE) - 8u;

        MEM32(XBOX_FS_BASE) = MEM32(entry_ebp - 0x0Cu);
        note("throw ends", ++s_thrown, t->guest);
        t->in_wait = 0;
        t->ended = 1;
        coroutine_switch(&t->own, t->resumer);
        abort();    /* an ended thread is never switched to again */
    }
    fprintf(stderr, "[script] C++ exception outside a script thread (object 0x%08X, type 0x%08X): ignored\n",
            object, type);
    g_esp += 12u;   /* ret 8 */
}
