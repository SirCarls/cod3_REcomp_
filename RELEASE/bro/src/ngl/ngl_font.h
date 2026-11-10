/* [acct2-bro] Big Red One port: NGL's fonts and text, natively. See ngl_font.c. */
#pragma once
#include <stdint.h>

/* nglLoadFont(name, data): the font called name (a 32-byte NGL name), parsed from the font file the caller read into
 * guest memory at data the first time. Returns the font's guest address (an NGL font object), or 0. */
uint32_t ngl_font_load(uint32_t name, uint32_t data);

/* nglFindFontAddRef(name): a font already loaded under name, counted up, or 0. */
uint32_t ngl_font_find(uint32_t name);

/* nglListAddText(font, text, x, y, z, color, scale_x, scale_y); args: guest address of x. */
void ngl_text_add(uint32_t font, uint32_t text, uint32_t args);

/* nglListAddTextf(font, x, y, z, color, scale_x, scale_y, format, ...); args: guest address of font. */
void ngl_text_addf(uint32_t args);

/* nglGetTextExtent(font, text, &width, &height, scale_x, scale_y): width and height as whole pixels (ints, as the
 * original stores them); args: guest address of font. */
void ngl_text_extent(uint32_t args);

/* nglGetTextExtentf(font, &width, &height, format, ...); args: guest address of font. */
void ngl_text_extentf(uint32_t args);

/* nglGetTextSizef(font, &width, &height, scale_x, scale_y, format, ...); args: guest address of font. */
void ngl_text_sizef(uint32_t args);
