// Info strings: \key\value\key\value text, the games' dictionaries (server
// info, user info, system info, config strings), sent over the network as
// they are. Keys can't contain \, ; or "; values silently lose them.
//
//                                       CoD2 1.3          UO 1.51
//   Info_ValueForKey accepts            under 8192        under 8192
//   Info_RemoveKey / _Big accept        1024 / 8192       1024 / 8192
//   Info_SetValueForKey refusals        printed           printed
//   Info_SetValueForKey_Big longest     1024 (a bug)      8192
//
// Looking a key up ignores case, removing one doesn't.
#pragma once

#include "iw_pc/base.h"

namespace iw_pc::shared::info {

/// MAX_INFO_STRING: what Info_RemoveKey and Info_SetValueForKey accept.
inline constexpr std::size_t kMaxInfoString = 1024;
/// BIG_INFO_STRING: what Info_ValueForKey and the _Big functions accept.
inline constexpr std::size_t kBigInfoString = 8192;

/// The value of `key`, ignoring case, or empty when it's missing; an info
/// string of 8192 bytes or more is the game's error. (The original copies
/// the value into a static buffer; the port returns its place in `info`.)
/// Original: Info_ValueForKey at 0x0044AA90 (CoD2MP_s.exe 1.3)
/// Original: Info_ValueForKey at 0x0044FB70 (CoDUOMP.exe 1.51)
Expected<ByteView> value_for_key(ByteView info, ByteView key);

/// Removes `key` (exact case); a key containing \ is ignored; a string of
/// 1024 bytes or more is the game's error.
/// Original: Info_RemoveKey at 0x0044ABE0 (CoD2MP_s.exe 1.3)
/// Original: Info_RemoveKey at 0x0044FCE0 (CoDUOMP.exe 1.51)
Status remove_key(ByteSpan info, ByteView key);

/// remove_key() for strings up to 8192 bytes.
/// Original: Info_RemoveKey_Big at 0x0044ACE0 (CoD2MP_s.exe 1.3)
/// Original: Info_RemoveKey_Big at 0x0044FE00 (CoDUOMP.exe 1.51)
Status remove_key_big(ByteSpan info, ByteView key);

/// Whether a string can go into an info string as a value: no " and no ;.
/// Original: Info_Validate at 0x0044ADE0 (CoD2MP_s.exe 1.3)
/// Original: Info_Validate at 0x0044FF20 (CoDUOMP.exe 1.51)
bool validate(ByteView s);

/// Why Info_SetValueForKey left the string as it was (the originals print
/// a message and return).
enum class Refusal {
    Oversize,
    KeyWithBackslash,
    KeyWithSemicolon,
    KeyWithQuote,
    PairTooLong,       // \key\value didn't fit the formatting buffer
    StringTooLong,     // the result would be too long (old pair already removed)
    BigStringTooLong,  // the same in United Offensive's _Big
    NoRoom,            // the caller's array is smaller than the game's (not the original's)
};

/// The format of the message the original prints, without its 0x15 marker.
const char* message_format(Refusal refusal, Game game);

/// Sets `key` to `value`: removes the old pair (exact case), then appends
/// \key\value; an empty value only removes it. The value loses \ ; " and
/// anything past 1023 characters.
/// Original: Info_SetValueForKey at 0x0044AE10 (CoD2MP_s.exe 1.3)
/// Original: Info_SetValueForKey at 0x0044FF50 (CoDUOMP.exe 1.51)
std::optional<Refusal> set_value_for_key(ByteSpan info, ByteView key, ByteView value);

/// set_value_for_key() for strings up to 8192 bytes. Call of Duty 2's final
/// check still uses 1024; United Offensive's uses 8192 with its own message.
/// Original: Info_SetValueForKey_Big at 0x0044AFE0 (CoD2MP_s.exe 1.3)
/// Original: Info_SetValueForKey_Big at 0x00450180 (CoDUOMP.exe 1.51)
std::optional<Refusal> set_value_for_key_big(Game game, ByteSpan info, ByteView key, ByteView value);

}  // namespace iw_pc::shared::info
