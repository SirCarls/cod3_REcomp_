// [waw-decomp] 2026-11-23: A(usa), this build's address of a USA guest address.
//
// The sources are written for the USA programs. tools/discmap writes build_addresses.h for each other build it
// maps (the German and Fr/Es/It multiplayer programs, ...): there A(usa) is that build's own address, worked out
// while compiling, and a USA address the build's map lacks stops the compile. A build that has such a file defines
// COD3_BUILD_ADDRESSES and puts the file on its include path; without it (the USA build) A(usa) is usa.
#pragma once

#if defined(COD3_BUILD_ADDRESSES)
#include "build_addresses.h"
#else
#define A(usa) (usa)
#endif
