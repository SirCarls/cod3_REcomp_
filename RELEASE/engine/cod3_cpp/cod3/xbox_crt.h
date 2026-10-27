// The Xbox 360 C runtime's sin and cos, which the game calls for all its
// trigonometry (view angles, the level scripts' sin/cos, effects). [acct2-decomp]
//
// Not the game's own code but Microsoft's runtime, linked into default.xex;
// the PC's own sin and cos give other last bits, so the game's math needs
// these. Each reduces x by a multiple of pi in two parts (pi split into a
// 34-bit head and a tail, "Cody-Waite") and evaluates one odd polynomial of
// degree 17 with fused multiply-adds. From 2.2e8 on they return a NaN with
// the sign bit set.
#pragma once

#include <cstdarg>
#include <cstddef>

namespace cod3::xbox_crt {

// 0x8234B538; recompiled: sin_guest
double sin(double x);

// cos(x) = sin(|x| + pi/2), with the shift folded into the reduction.
// 0x8234B610; recompiled: cos_guest
double cos(double x);

// ---- characters (the "C" locale the game runs in) ---------------------------
//
// The game passes its characters as signed chars (lbz + extsb), so bytes 0x80
// and up arrive negative: they are not letters and come back as they went in.

// 0x8234BFD8; recompiled: isalpha_guest
inline int isalpha(int c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'); }

// 0x8234BFC0; recompiled: tolower_guest
inline int tolower(int c) { return c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c; }

// Microsoft's _vsnprintf, which Com_sprintf calls: at most count characters
// are written, and the terminator only when there is room for it (unlike the
// C standard's vsnprintf). Returns the length, or -1 when the text was cut.
// Called by Com_sprintf through recompiled: sub_8234F0D0
int _vsnprintf(char* buffer, std::size_t count, const char* format, std::va_list args);

}  // namespace cod3::xbox_crt
