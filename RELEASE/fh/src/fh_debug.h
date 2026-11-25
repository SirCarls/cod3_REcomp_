/* [acct2-fh] Finest Hour port: crash reports, write guards and "where" reports (fh_debug.c, from Big Red One's
 * bro_debug.c). Call once, after the game's image is mapped and before it starts. Reads FH_FAULT_REPORT,
 * FH_WRITE_GUARD, FH_GUARD_MIRRORS and FH_WHERE. */
#pragma once

void fh_debug_init(void);
