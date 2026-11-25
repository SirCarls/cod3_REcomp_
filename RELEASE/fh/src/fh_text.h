/* [waw-decomp-fh2 r19] 2026-11-24 The game's XFONT text (its subtitles and messages) drawn at the picture's own size
 * (fh_text.c). */
#ifndef FH_TEXT_H
#define FH_TEXT_H

#include <stdint.h>

/* The text drawn since the last Swap, as a layer the size of ngl_vk's picture: 4 bytes a pixel (blue, green, red,
 * alpha; the colors already times alpha), width x height. Only rows top..top+rows-1 hold any. Returns 0 when there is
 * none. */
int fh_text_layer(const uint8_t **pixels, uint32_t *width, uint32_t *height, uint32_t *top, uint32_t *rows);

/* 1 when text has been drawn since the last Swap (fh_present.c then draws the frame through fh_draw.c). */
int fh_text_waiting(void);

/* At the end of each Swap: the layer is emptied for the next frame's text. */
void fh_text_swap(void);

#endif
