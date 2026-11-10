/* Big Red One's script threads, done natively (bro_script_threads.c).
 *
 * Each of these is the whole of one game function, named in bro/config/stand_in_functions.tsv with a "native NAME"
 * entry: the generated stand_ins.c defines the function's sub_<address> as a call to it, and every call to that
 * address, direct or through a pointer, comes here. Each reads its own arguments and pops the stack as the
 * original's ret does. */
#pragma once

void scr_resume_thread(void);        /* Scr_ResumeThread 0x002FE100: start or resume a script thread (thread in eax) */
void scr_suspend_wait_time(void);    /* Scr_SuspendWaitTime 0x0011D950: pause the running thread for a time */
void scr_suspend_wait_frames(void);  /* Scr_SuspendWaitFrames 0x0011D990: pause it for some frames */
void scr_suspend_wait_till(void);    /* Scr_SuspendWaitTill 0x0011D9D0: pause it until a notify */
void scr_jump_to_scheduler(void);    /* 0x002F5660: the jump from a wait back to Scr_ResumeThread */
void scr_cxx_throw(void);            /* _CxxThrowException 0x001F2302: ends the running script thread */
