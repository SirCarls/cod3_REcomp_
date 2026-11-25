/* [acct2-fh] 2026-11-15: Finest Hour starts itself again to change levels (fh_relaunch.c).
 *
 * At the end of a level the game saves the profile, puts the next level in its launch data and asks the console for a
 * quick reboot into its own default.xbe; the console then starts the game again and the game reads the launch data
 * and goes straight into that level. xboxrecomp ends the process at that reboot. This keeps the launch data, starts
 * the port again and gives the new start the launch data back, as the console would.
 *
 *   fh_relaunch_init(argc, argv)   in main, after the XBE is mapped and before xbox_kernel_bridge_init: keeps the
 *                                  command line and checks the kernel import the game reboots through
 *   fh_relaunch_start()            in main, after xbox_kernel_bridge_init: notes that import's address and, after a
 *                                  restart, gives the game its launch data back (before the game starts)
 *   fh_relaunch_lookup(va)         from recomp_lookup_manual, on every call through a pointer: the reboot comes here
 */
#ifndef FH_RELAUNCH_H
#define FH_RELAUNCH_H

#include <stdint.h>

#include "recomp_types.h"           /* recomp_func_t */

/* The address the game calls HalReturnToFirmware through once xboxrecomp has filled its import table, or 0 (off). */
extern uint32_t fh_relaunch_hal_va;

void fh_relaunch_hal(void);
void fh_relaunch_init(int argc, char **argv);
void fh_relaunch_start(void);

/* Asked on every call through a pointer, so as cheap as it can be: one comparison when it isn't the reboot. */
static inline recomp_func_t fh_relaunch_lookup(uint32_t xbox_va)
{
    return (xbox_va == fh_relaunch_hal_va && xbox_va) ? fh_relaunch_hal : (recomp_func_t)0;
}

#endif
