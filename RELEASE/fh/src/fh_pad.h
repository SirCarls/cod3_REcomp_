/* [acct2-fh] Finest Hour port: the gamepad, at XAPI's level. See fh_pad.c. */
#pragma once
#include <stdint.h>

/* XAPI's device and gamepad functions (the game's XPP section), done natively. Arguments and results as XAPI has
 * them (guest addresses). */
uint32_t fh_pad_get_devices(uint32_t device_type);
uint32_t fh_pad_get_device_changes(uint32_t device_type, uint32_t insertions, uint32_t removals);
uint32_t fh_pad_open(uint32_t device_type, uint32_t port, uint32_t slot, uint32_t polling);
uint32_t fh_pad_close(uint32_t handle);
uint32_t fh_pad_get_capabilities(uint32_t handle, uint32_t capabilities);
uint32_t fh_pad_get_state(uint32_t handle, uint32_t state);
uint32_t fh_pad_set_state(uint32_t handle, uint32_t feedback);

/* [acct2-fh] 11-15: the pad as the game last read it (its last XInputGetState): the digital buttons (wButtons), the
 * eight analog ones (A B X Y black white, the left and right triggers: 0-255) and the four stick axes (left x, left
 * y, right x, right y; y up-positive), for the console's noclip. All 0 before the game's first read. */
void fh_pad_last_state(uint16_t *digital, uint8_t analog[8], int16_t thumbs[4]);
