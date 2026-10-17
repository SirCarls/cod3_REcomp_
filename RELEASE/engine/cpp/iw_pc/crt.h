// The pieces of the Microsoft C runtime the games link in that ports depend
// on, with that runtime's exact behavior. Both executables carry the same
// runtime code; the games never select a locale, so it stays "C".
#pragma once

#include "iw_pc/cstr.h"

namespace iw_pc::crt {

/// tolower in the "C" locale: only A-Z change. The games pass movsx bytes,
/// so 0x80 and up arrive negative and come back unchanged.
inline std::int32_t tolower(std::int32_t c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }

/// toupper in the "C" locale: only a-z change.
inline std::int32_t toupper(std::int32_t c) { return c >= 'a' && c <= 'z' ? c - 32 : c; }

/// strncpy(dest, src, dest.size()): copies src up to its terminator and
/// zero-fills the rest; a src of dest.size() characters or more leaves no
/// terminator.
inline void strncpy(ByteSpan dest, ByteView src) {
    std::size_t n = std::min(cstr::len(src), dest.size());
    if (n > 0) std::memmove(dest.data(), src.data(), n);
    std::fill(dest.begin() + n, dest.end(), std::uint8_t{0});
}

/// What Com_sprintf(dest, size, ...) leaves for an already formatted text,
/// and what it returns: _vsnprintf writes at most `size` characters and a
/// terminator only if there is room, returning the length or -1 when the
/// text didn't fit; then dest[size - 1] = 0. `size` is at least 1 and at
/// most dest.size().
inline std::int32_t sprintf(ByteSpan dest, std::size_t size, ByteView formatted) {
    std::size_t room = std::min(size, dest.size());
    if (room > 0) {
        std::size_t n = std::min(formatted.size(), room);
        if (n > 0) std::memmove(dest.data(), formatted.data(), n);
        if (n < room) dest[n] = 0;
        dest[room - 1] = 0;
    }
    return formatted.size() > size ? -1 : static_cast<std::int32_t>(formatted.size());
}

}  // namespace iw_pc::crt
