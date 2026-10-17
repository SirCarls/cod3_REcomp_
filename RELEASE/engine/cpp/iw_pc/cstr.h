// NUL-terminated strings in fixed-size byte buffers, the way the games keep
// text. A string argument is a ByteView holding just the characters
// (reading past its end gives 0: at()); a string buffer is a ByteSpan the
// size of the game's array, holding the characters, a 0, then whatever was
// there before (len(), text()).
#pragma once

#include <algorithm>
#include <cstring>

#include "iw_pc/base.h"

namespace iw_pc::cstr {

/// The byte at `index`, or 0 past the end.
inline std::uint8_t at(ByteView s, std::size_t index) { return index < s.size() ? s[index] : 0; }

/// The byte at `index` as x86's movsx loads it (a signed char).
inline std::int32_t signed_at(ByteView s, std::size_t index) {
    return static_cast<std::int8_t>(at(s, index));
}

/// The length of the string in `buffer`: up to its first 0, or all of it.
inline std::size_t len(ByteView buffer) {
    auto zero = std::find(buffer.begin(), buffer.end(), std::uint8_t{0});
    return static_cast<std::size_t>(zero - buffer.begin());
}

/// The characters of the string in `buffer`.
inline ByteView text(ByteView buffer) { return buffer.first(len(buffer)); }

/// Writes `s` and a terminator at `offset`, as far as the buffer allows.
/// True when all of it and the terminator fit.
inline bool write_at(ByteSpan buffer, std::size_t offset, ByteView s) {
    std::size_t room = buffer.size() > offset ? buffer.size() - offset : 0;
    std::size_t n = std::min(s.size(), room);
    if (n > 0) std::memmove(buffer.data() + offset, s.data(), n);
    if (n < room) {
        buffer[offset + n] = 0;
        return n == s.size();
    }
    return false;
}

/// Appends `s` to the string in `buffer` (see write_at()).
inline bool append(ByteSpan buffer, ByteView s) { return write_at(buffer, len(buffer), s); }

}  // namespace iw_pc::cstr
