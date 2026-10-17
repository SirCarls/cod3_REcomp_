#include "iw_pc/shared/strings.h"

#include <vector>

#include "iw_pc/crt.h"
#include "iw_pc/cstr.h"

namespace iw_pc::shared::strings {

namespace {

// What Q_stricmpn and friends return for two different characters.
std::int32_t order(std::int32_t c1, std::int32_t c2) { return c1 >= c2 ? 1 : -1; }

bool is_digit(std::uint8_t c) { return c >= '0' && c <= '9'; }

// A color code: ^ and a digit.
bool is_color_code(ByteView s, std::size_t i) {
    return cstr::at(s, i) == '^' && is_digit(cstr::at(s, i + 1));
}

// Where the extension of the last path component starts: its last dot, or
// the end of the name (/ and \ end a component).
std::size_t extension_start(ByteView name) {
    std::optional<std::size_t> dot;
    for (std::size_t i = 0; i < name.size(); ++i) {
        if (name[i] == '.') dot = i;
        else if (name[i] == '/' || name[i] == '\\') dot.reset();
    }
    return dot.value_or(name.size());
}

}  // namespace

std::int32_t stricmpn(ByteView s1, ByteView s2, std::int32_t n) {
    std::uint32_t remaining = static_cast<std::uint32_t>(n);
    for (std::size_t i = 0;; ++i) {
        std::int32_t c1 = cstr::signed_at(s1, i);
        std::int32_t c2 = cstr::signed_at(s2, i);
        if (remaining == 0) return 0;
        --remaining;
        if (c1 != c2) {
            c1 = crt::toupper(c1);
            c2 = crt::toupper(c2);
            if (c1 != c2) return order(c1, c2);
        }
        if (c1 == 0) return 0;
    }
}

std::int32_t strncmp(ByteView s1, ByteView s2, std::int32_t n) {
    std::uint32_t remaining = static_cast<std::uint32_t>(n);
    for (std::size_t i = 0;; ++i) {
        std::int32_t c1 = cstr::signed_at(s1, i);
        std::int32_t c2 = cstr::signed_at(s2, i);
        if (remaining == 0) return 0;
        --remaining;
        if (c1 != c2) return order(c1, c2);
        if (c1 == 0) return 0;
    }
}

std::int32_t stricmp(ByteView s1, ByteView s2) { return stricmpn(s1, s2, kStricmpLimitCod2); }

std::int32_t strcmp(ByteView s1, ByteView s2) { return strncmp(s1, s2, kStricmpLimitCod2); }

std::int32_t stricmp_nullable(const ByteView* s1, const ByteView* s2) {
    if (s1 == nullptr || s2 == nullptr) return -1;
    return stricmpn(*s1, *s2, kStricmpLimitUo);
}

std::int32_t stricmp_equal_or_minus_one(ByteView s1, ByteView s2) {
    std::int32_t remaining = kStricmpLimitUo;
    for (std::size_t i = 0;; ++i) {
        std::int32_t c1 = cstr::signed_at(s1, i);
        std::int32_t c2 = cstr::signed_at(s2, i);
        bool limit_reached = remaining == 0;
        --remaining;
        if (limit_reached) return 0;
        if (c1 != c2) {
            c1 = crt::toupper(c1);
            c2 = crt::toupper(c2);
            if (c1 != c2) return -1;
        }
        if (c1 == 0) return 0;
    }
}

void strncpyz(ByteSpan dest, ByteView src) {
    std::size_t last = dest.size() - 1;
    crt::strncpy(dest.first(last), src);
    dest[last] = 0;
}

void strlwr(ByteSpan buffer) {
    std::size_t n = cstr::len(buffer);
    for (std::size_t i = 0; i < n; ++i) {
        if (buffer[i] >= 'A' && buffer[i] <= 'Z') buffer[i] += 32;
    }
}

void strupr(ByteSpan buffer) {
    std::size_t n = cstr::len(buffer);
    for (std::size_t i = 0; i < n; ++i) {
        if (buffer[i] >= 'a' && buffer[i] <= 'z') buffer[i] -= 32;
    }
}

Status strcat(ByteSpan dest, ByteView src) {
    std::size_t size = dest.size();
    std::size_t start = cstr::len(dest);
    if (start >= size) return GameError::fatal("Q_strcat: already overflowed");
    std::size_t room = size - start - 1;
    ByteView text = cstr::text(src);
    for (std::size_t k = 0; k < room; ++k) dest[start + k] = k < text.size() ? text[k] : 0;
    dest[size - 1] = 0;
    return std::nullopt;
}

std::int32_t print_strlen(ByteView s) {
    std::int32_t count = 0;
    std::size_t i = 0;
    while (cstr::at(s, i) != 0) {
        if (is_color_code(s, i)) {
            i += 2;
        } else {
            ++count;
            ++i;
        }
    }
    return count;
}

void clean_str(ByteSpan buffer) {
    std::size_t n = cstr::len(buffer);
    std::size_t kept = 0;
    for (std::size_t i = 0; i < n; ++i) {
        std::uint8_t c = buffer[i];
        if (is_color_code(buffer, i)) {
            ++i;  // the digit goes with the loop's step
        } else if (c >= 0x20 && c <= 0x7E) {
            buffer[kept++] = c;
        }
    }
    buffer[kept] = 0;
}

ByteView skip_path(ByteView path) {
    ByteView text = cstr::text(path);
    for (std::size_t i = text.size(); i > 0; --i) {
        if (text[i - 1] == '/') return text.subspan(i);
    }
    return text;
}

ByteView extension_with_dot(ByteView name) {
    ByteView text = cstr::text(name);
    return text.subspan(extension_start(text));
}

ByteView strip_extension(ByteView name) {
    ByteView text = cstr::text(name);
    return text.first(extension_start(text));
}

ByteView strip_extension_at_first_dot(ByteView name) {
    ByteView text = cstr::text(name);
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '.') return text.first(i);
    }
    return text;
}

void default_extension(ByteSpan path, std::size_t max_size, ByteView extension) {
    std::size_t n = cstr::len(path);
    if (n > 0) {
        std::size_t i = n - 1;
        if (path[i] != '/') {
            while (i != 0) {
                if (path[i] == '.') return;
                --i;
                if (path[i] == '/') break;
            }
        }
    }
    ByteView ext = cstr::text(extension);
    std::vector<std::uint8_t> formatted(path.begin(), path.begin() + std::min(n, kDefaultExtensionKeep));
    formatted.insert(formatted.end(), ext.begin(), ext.end());
    crt::sprintf(path, max_size, formatted);
}

}  // namespace iw_pc::shared::strings
