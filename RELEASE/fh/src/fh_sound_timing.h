/* [waw-decomp-fh2] r17 2026-11-24: the game's DirectSound buffers keep their play time (fh_sound_timing.c). */
#pragma once
#include <stdint.h>

uint32_t fh_sound_create_buffer(uint32_t dsound, uint32_t desc, uint32_t out_buffer);  /* 0x00236454 */
uint32_t fh_sound_set_buffer_data(uint32_t buffer, uint32_t data, uint32_t bytes);     /* 0x00235A65 */
uint32_t fh_sound_play(uint32_t buffer, uint32_t flags);                               /* 0x002349B5 */
uint32_t fh_sound_stop(uint32_t buffer);                                               /* 0x002349D9, 0x002349F1 */
uint32_t fh_sound_get_status(uint32_t buffer, uint32_t out_status);                    /* 0x00234A35 */
uint32_t fh_sound_get_position(uint32_t buffer, uint32_t out_play, uint32_t out_write);/* 0x00234A51 */
uint32_t fh_sound_set_format(uint32_t buffer, uint32_t format);                          /* 0x00235A49, r18 */
uint32_t fh_sound_set_play_region(uint32_t buffer, uint32_t start, uint32_t length);   /* 0x00235457, r18 */
uint32_t fh_sound_set_position(uint32_t buffer, uint32_t position);                    /* 0x00234A71, r18 */
uint32_t fh_sound_release(uint32_t buffer);                                            /* 0x00233585 */
