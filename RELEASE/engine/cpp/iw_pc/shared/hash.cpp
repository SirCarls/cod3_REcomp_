#include "iw_pc/shared/hash.h"

#include "iw_pc/cstr.h"

namespace iw_pc::shared::hash {

namespace {

std::uint8_t lower(std::uint8_t c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }

std::int32_t as_signed(std::uint8_t c) { return static_cast<std::int8_t>(c); }

// Wrapping 32-bit arithmetic, as the originals' registers do it.
std::int32_t wrap_add(std::int32_t a, std::int32_t b) {
    return static_cast<std::int32_t>(static_cast<std::uint32_t>(a) + static_cast<std::uint32_t>(b));
}
std::int32_t wrap_mul(std::int32_t a, std::int32_t b) {
    return static_cast<std::int32_t>(static_cast<std::uint32_t>(a) * static_cast<std::uint32_t>(b));
}

std::int32_t fold(std::int32_t h) { return h ^ (h >> 10) ^ (h >> 20); }

}  // namespace

std::uint32_t file_name_hash(ByteView name, std::uint32_t hash_size) {
    std::int32_t hash = 0;
    ByteView text = cstr::text(name);
    for (std::size_t i = 0; i < text.size(); ++i) {
        std::uint8_t letter = lower(text[i]);
        if (letter == '.') break;
        if (letter == '\\') letter = '/';
        hash = wrap_add(hash, wrap_mul(as_signed(letter), static_cast<std::int32_t>(i) + 119));
    }
    return static_cast<std::uint32_t>(fold(hash)) & (hash_size - 1);
}

std::uint32_t cvar_hash(ByteView name) {
    std::int32_t hash = 0;
    ByteView text = cstr::text(name);
    for (std::size_t i = 0; i < text.size(); ++i) {
        hash = wrap_add(hash, wrap_mul(as_signed(lower(text[i])), static_cast<std::int32_t>(i) + 119));
    }
    return static_cast<std::uint32_t>(hash) & (kCvarHashSize - 1);
}

std::int32_t hash_key(ByteView s, std::int32_t max_len) {
    std::int32_t hash = 0;
    ByteView text = cstr::text(s);
    std::size_t count = max_len > 0 ? static_cast<std::size_t>(max_len) : 0;
    for (std::size_t i = 0; i < text.size() && i < count; ++i) {
        hash = wrap_add(hash, wrap_mul(as_signed(text[i]), static_cast<std::int32_t>(i) + 119));
    }
    return fold(hash);
}

}  // namespace iw_pc::shared::hash
