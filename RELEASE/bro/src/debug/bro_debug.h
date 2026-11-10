/* Big Red One port: crash reports and write guards (bro_debug.c). */
#pragma once

/* Call once the game's image is in guest memory, before the game starts. Reads BRO_FAULT_REPORT, BRO_WRITE_GUARD
 * and BRO_GUARD_MIRRORS. */
void bro_debug_init(void);
