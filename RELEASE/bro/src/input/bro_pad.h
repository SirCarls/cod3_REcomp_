/* [acct2-bro] Big Red One port: the gamepad, at XAPI's level. See bro_pad.c. */
#pragma once
#include <stdint.h>

/* XAPI's device and gamepad functions, done natively. Arguments and results as XAPI has them (guest addresses). */
uint32_t bro_pad_get_devices(uint32_t device_type);
uint32_t bro_pad_get_device_changes(uint32_t device_type, uint32_t insertions, uint32_t removals);
uint32_t bro_pad_open(uint32_t device_type, uint32_t port, uint32_t slot, uint32_t polling);
uint32_t bro_pad_close(uint32_t handle);
uint32_t bro_pad_get_capabilities(uint32_t handle, uint32_t capabilities);
uint32_t bro_pad_get_state(uint32_t handle, uint32_t state);
uint32_t bro_pad_set_state(uint32_t handle, uint32_t feedback);

/* Sys_ReadLaunchInfo 0x002221B0, done natively (the stand-in table's "native" row): BRO_DEVMAP=<map> starts the game
 * in that level, as a debugger's launch data would. */
void bro_launch_info(void);

/* [acct2-bro-game] BRO_PAD_SCRIPT's "wait=<text>" entries: whether the script is waiting for a text now, and each
 * string the game draws meanwhile (ngl_font.c hands them over, without their control codes). */
int bro_pad_waiting_for_text(void);
void bro_pad_text_drawn(const char *text);

/* [f2-menus] 2026-11-11 A control setting changed in the F2 window: the next poll reads them all again (any
 * thread may call it; bro/src/f2/bro_f2_store.cpp does). */
void bro_pad_reload(void);
