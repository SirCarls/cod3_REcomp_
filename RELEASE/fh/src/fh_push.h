/* [acct2-fh] Finest Hour port: the game's own writes to the NV2A push buffer, read for what the port needs. See
 * fh_push.c. */
#pragma once
#include <stdint.h>

/* D3DDevice_MakeSpace (0x002252C0): room in the push buffer. Reads what was written since the last time (fh_push_read)
 * and hands back the start of the port's push area, where the caller goes on writing. */
uint32_t fh_push_make_space(void);

/* Reads the words written into the push area since the last read and starts it over. The draws and the vertex
 * constant writers call it first, so the game's writes take effect in the order it made them. */
void fh_push_read(void);
