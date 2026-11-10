/* Big Red One port: x86 registers that only the kernel can really use, included before every generated file.
 *
 * The recompiler turns "mov drN, reg" into an assignment to a variable named drN but declares none. Game code never
 * sets the debug registers; where such an instruction shows up, the bytes are data that were taken for code (both
 * cases so far, 0x003DCA50 and 0x003E1B50, sit in the DirectSound library, which is all stand-ins). Each generated
 * file gets its own unused copies, so the code compiles and nothing the game reads changes. */
#pragma once
#include <stdint.h>

static uint32_t dr0, dr1, dr2, dr3, dr6, dr7;
