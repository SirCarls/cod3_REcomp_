#include "iw_pc/shared/filter.h"

#include <vector>

#include "iw_pc/crt.h"
#include "iw_pc/cstr.h"
#include "iw_pc/shared/strings.h"

namespace iw_pc::shared::filter {

namespace {

// How a * finds the text after it in the rest of the name.
enum class StarSearch {
    Anywhere,   // the first place it fits (United Offensive, Quake III)
    WholeRest,  // only where the rest of the name equals the text (Call of Duty 2)
};

std::int32_t as_signed(std::uint8_t c) { return static_cast<std::int8_t>(c); }

// Two characters as the filter compares them: exactly, or after toupper on
// the signed chars.
bool same(std::uint8_t a, std::uint8_t b, bool case_sensitive) {
    if (case_sensitive) return a == b;
    return crt::toupper(as_signed(a)) == crt::toupper(as_signed(b));
}

// Whether c lies between low and high (signed chars, folded to upper case
// unless case_sensitive).
bool in_range(std::uint8_t c, std::uint8_t low, std::uint8_t high, bool case_sensitive) {
    auto key = [case_sensitive](std::uint8_t x) {
        return case_sensitive ? as_signed(x) : crt::toupper(as_signed(x));
    };
    return key(c) >= key(low) && key(c) <= key(high);
}

// Where `text` starts in `rest` (Com_StringContains), the way each game looks.
std::optional<std::size_t> find(ByteView rest, ByteView text, bool case_sensitive, StarSearch search) {
    if (rest.size() < text.size()) return std::nullopt;
    std::size_t last_start = rest.size() - text.size();
    if (search == StarSearch::Anywhere) {
        for (std::size_t i = 0; i <= last_start; ++i) {
            bool all = true;
            for (std::size_t k = 0; k < text.size() && all; ++k) all = same(rest[i + k], text[k], case_sensitive);
            if (all) return i;
        }
        return std::nullopt;
    }
    // The original tries every start with a whole-string compare; only the
    // start where both have the same length can be equal.
    ByteView rest_from_there = rest.subspan(last_start);
    bool equal = case_sensitive ? strings::strcmp(rest_from_there, text) == 0
                                : strings::stricmp(rest_from_there, text) == 0;
    if (equal) return last_start;
    return std::nullopt;
}

// Walks a [...] class from f (just after the [) until c matches one of its
// characters or ranges, leaving f after the matching part. False when the
// class ends (or the filter does) without a match.
bool class_matches(ByteView filter, std::size_t& f, std::uint8_t c, bool case_sensitive) {
    for (;;) {
        std::uint8_t first = cstr::at(filter, f);
        if (first == 0 || (first == ']' && cstr::at(filter, f + 1) != ']')) return false;
        std::uint8_t high = cstr::at(filter, f + 2);
        bool found;
        if (cstr::at(filter, f + 1) == '-' && high != 0 && (high != ']' || cstr::at(filter, f + 3) == ']')) {
            f += 3;
            found = in_range(c, first, high, case_sensitive);
        } else {
            f += 1;
            found = same(first, c, case_sensitive);
        }
        if (found) return true;
    }
}

bool filter_with(ByteView filter_arg, ByteView name_arg, bool case_sensitive, StarSearch search) {
    ByteView filter = cstr::text(filter_arg);
    ByteView name = cstr::text(name_arg);
    std::size_t f = 0;  // in the filter
    std::size_t n = 0;  // in the name (may run past its end, where it reads zeros)
    for (;;) {
        std::uint8_t c = cstr::at(filter, f);
        if (c == 0) return true;
        if (c == '*') {
            ++f;
            std::size_t start = f;
            while (cstr::at(filter, f) != 0 && cstr::at(filter, f) != '*' && cstr::at(filter, f) != '?') ++f;
            ByteView text = filter.subspan(start, f - start);
            if (text.empty()) continue;
            ByteView rest = n < name.size() ? name.subspan(n) : ByteView{};
            auto at = find(rest, text, case_sensitive, search);
            if (!at) return false;
            n += *at + text.size();
        } else if (c == '?') {
            ++f;
            ++n;
        } else if (c == '[') {
            ++f;
            if (cstr::at(filter, f) == '[') continue;  // the second [ is looked at again: it starts a class
            if (!class_matches(filter, f, cstr::at(name, n), case_sensitive)) return false;
            // Skip to the class's closing ] (a "]]" doesn't close it), and past it.
            for (;;) {
                std::uint8_t k = cstr::at(filter, f);
                if (k == 0 || (k == ']' && cstr::at(filter, f + 1) != ']')) break;
                ++f;
            }
            ++f;
            ++n;
        } else {
            if (!same(c, cstr::at(name, n), case_sensitive)) return false;
            ++f;
            ++n;
        }
    }
}

// A path as Com_FilterPath compares it: its first 63 characters, \ and : as /.
std::vector<std::uint8_t> path_for_filter(ByteView path) {
    ByteView text = cstr::text(path);
    std::vector<std::uint8_t> out;
    for (std::size_t i = 0; i < text.size() && i < kFilterPathKeep; ++i) {
        std::uint8_t c = text[i];
        out.push_back(c == '\\' || c == ':' ? '/' : c);
    }
    return out;
}

}  // namespace

bool filter(ByteView filter_text, ByteView name, bool case_sensitive) {
    return filter_with(filter_text, name, case_sensitive, StarSearch::Anywhere);
}

bool filter_cod2(ByteView filter_text, ByteView name, bool case_sensitive) {
    return filter_with(filter_text, name, case_sensitive, StarSearch::WholeRest);
}

bool filter_path(ByteView filter_text, ByteView name, bool case_sensitive) {
    return filter(path_for_filter(filter_text), path_for_filter(name), case_sensitive);
}

bool filter_path_cod2(ByteView filter_text, ByteView name, bool case_sensitive) {
    return filter_cod2(path_for_filter(filter_text), path_for_filter(name), case_sensitive);
}

}  // namespace iw_pc::shared::filter
