// String and file-name helpers (Quake III's q_shared.c family).
//
// What differs from Quake III and from Call of Duty 3's PowerPC versions:
// the compares fold case to upper and return only -1, 0 or 1; color codes
// are ^ and a digit only; Q_strncpyz has no NULL or size checks; the two
// games strip extensions differently.
#pragma once

#include "iw_pc/base.h"

namespace iw_pc::shared::strings {

/// The n Call of Duty 2's Q_stricmp gives Q_stricmpn (INT_MAX).
inline constexpr std::int32_t kStricmpLimitCod2 = 0x7FFFFFFF;
/// The n United Offensive's Q_stricmp gives Q_stricmpn (Quake III's 99999).
inline constexpr std::int32_t kStricmpLimitUo = 99999;
/// The text COM_DefaultExtension keeps of the old name.
inline constexpr std::size_t kDefaultExtensionKeep = 63;

/// Compares at most n characters ignoring ASCII case: 0 when equal, else 1
/// or -1 by the first different characters folded to upper case, as signed
/// chars. A negative n never runs out. (Call of Duty 2: I_strnicmp.)
/// Original: Q_stricmpn at 0x0044A670 (CoD2MP_s.exe 1.3)
/// Original: Q_stricmpn at 0x0044F830 (CoDUOMP.exe 1.51)
std::int32_t stricmpn(ByteView s1, ByteView s2, std::int32_t n);

/// Compares at most n characters exactly: 0, or 1 / -1 as signed chars.
/// Original: Q_strncmp at 0x0044A6D0 (CoD2MP_s.exe 1.3)
/// Original: Q_strncmp at 0x0044F890 (CoDUOMP.exe 1.51)
std::int32_t strncmp(ByteView s1, ByteView s2, std::int32_t n);

/// Call of Duty 2's Q_stricmp: stricmpn() with INT_MAX.
/// Original: Q_stricmp at 0x0044A710 (CoD2MP_s.exe 1.3)
std::int32_t stricmp(ByteView s1, ByteView s2);

/// Call of Duty 2's Q_strcmp: strncmp() with INT_MAX.
/// Original: Q_strcmp at 0x0044A730 (CoD2MP_s.exe 1.3)
std::int32_t strcmp(ByteView s1, ByteView s2);

/// United Offensive's Q_stricmp: -1 when either string is missing (NULL),
/// else stricmpn() limited to 99999 characters.
/// Original: Q_stricmp at 0x0044F8D0 (CoDUOMP.exe 1.51)
std::int32_t stricmp_nullable(const ByteView* s1, const ByteView* s2);

/// An unused second Q_stricmp in United Offensive: 0 or -1 only, case folded
/// to upper, 99999 characters at most (Call of Duty 3's Q_stricmp).
/// Original: sub_0044FAA0 at 0x0044FAA0 (CoDUOMP.exe 1.51)
std::int32_t stricmp_equal_or_minus_one(ByteView s1, ByteView s2);

/// Copies src into dest (the game's array): at most dest.size() - 1
/// characters, the rest zero-filled, the last byte always 0. dest must hold
/// at least one byte.
/// Original: Q_strncpyz at 0x0044A650 (CoD2MP_s.exe 1.3)
/// Original: Q_strncpyz at 0x0044F810 (CoDUOMP.exe 1.51)
void strncpyz(ByteSpan dest, ByteView src);

/// Lower-cases A-Z in place.
/// Original: Q_strlwr at 0x0044A7F0 (CoD2MP_s.exe 1.3)
/// Original: Q_strlwr at 0x0044F8F0 (CoDUOMP.exe 1.51)
void strlwr(ByteSpan buffer);

/// Upper-cases a-z in place.
/// Original: Q_strupr at 0x0044A820 (CoD2MP_s.exe 1.3)
/// Original: Q_strupr at 0x0044F920 (CoDUOMP.exe 1.51)
void strupr(ByteSpan buffer);

/// Appends src to the string in dest, zero-filling the rest like strncpy;
/// the last byte is always 0. A string already as long as the array is a
/// fatal error ("Q_strcat: already overflowed").
/// Original: Q_strcat at 0x0044A850 (CoD2MP_s.exe 1.3)
/// Original: Q_strcat at 0x0044F950 (CoDUOMP.exe 1.51)
Status strcat(ByteSpan dest, ByteView src);

/// The length of a string as printed: color codes (^ and a digit) don't count.
/// Original: Q_PrintStrlen at 0x0044A8A0 (CoD2MP_s.exe 1.3)
/// Original: Q_PrintStrlen at 0x0044F9A0 (CoDUOMP.exe 1.51)
std::int32_t print_strlen(ByteView s);

/// Removes color codes and every byte outside 0x20-0x7E, in place.
/// Original: Q_CleanStr at 0x0044A8D0 (CoD2MP_s.exe 1.3)
/// Original: Q_CleanStr at 0x0044F9D0 (CoDUOMP.exe 1.51)
void clean_str(ByteSpan buffer);

/// The file name after the last / (a \ doesn't count).
/// Original: COM_SkipPath at 0x00449F60 (CoD2MP_s.exe 1.3)
/// Original: COM_SkipPath at 0x0044F310 (CoDUOMP.exe 1.51)
ByteView skip_path(ByteView path);

/// The extension of a file name with its dot (".iwd"), or empty.
/// Original: Com_GetExtensionSubString at 0x0044A040 (CoD2MP_s.exe 1.3)
ByteView extension_with_dot(ByteView name);

/// Call of Duty 2's COM_StripExtension: the name without the extension of
/// its last component. (The original copies it into its out argument.)
/// Original: COM_StripExtension at 0x0044A070 (CoD2MP_s.exe 1.3)
ByteView strip_extension(ByteView name);

/// United Offensive's COM_StripExtension: the name up to its first dot.
/// Original: COM_StripExtension at 0x0044F330 (CoDUOMP.exe 1.51)
ByteView strip_extension_at_first_dot(ByteView name);

/// Adds `extension` to the name in `path` (its first max_size bytes are the
/// original's maxSize) if the name has none: the name cut to 63 characters,
/// the extension added, written like Com_sprintf(path, maxSize, "%s%s").
/// Original: COM_DefaultExtension at 0x0044A100 (CoD2MP_s.exe 1.3)
/// Original: COM_DefaultExtension at 0x0044F3A0 (CoDUOMP.exe 1.51)
void default_extension(ByteSpan path, std::size_t max_size, ByteView extension);

}  // namespace iw_pc::shared::strings
