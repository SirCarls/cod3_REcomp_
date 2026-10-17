#include "iw_pc/shared/info.h"

#include <cstring>
#include <vector>

#include "iw_pc/crt.h"
#include "iw_pc/cstr.h"
#include "iw_pc/shared/strings.h"

namespace iw_pc::shared::info {

namespace {

// One \key\value pair found while walking an info string.
struct Pair {
    std::size_t start;  // its leading \ included
    std::size_t key_start, key_end;
    std::size_t value_start, value_end;
};

// The pair from i on, walked like the originals do (one leading \ skipped).
// A key with no \ after it ends the walk.
std::optional<Pair> next_pair(ByteView info, std::size_t i) {
    Pair pair{};
    pair.start = i;
    if (cstr::at(info, i) == '\\') ++i;
    pair.key_start = i;
    while (cstr::at(info, i) != '\\') {
        if (cstr::at(info, i) == 0) return std::nullopt;
        ++i;
    }
    pair.key_end = i;
    ++i;
    pair.value_start = i;
    while (cstr::at(info, i) != '\\' && cstr::at(info, i) != 0) ++i;
    pair.value_end = i;
    return pair;
}

bool contains(ByteView s, std::uint8_t c) {
    for (std::uint8_t x : s) {
        if (x == c) return true;
    }
    return false;
}

bool equal(ByteView a, ByteView b) {
    return a.size() == b.size() && (a.empty() || std::memcmp(a.data(), b.data(), a.size()) == 0);
}

// Removes the first pair whose key is exactly `key`: the rest of the string
// moves down over it (strcpy). A key containing \ is ignored.
void remove_pair(ByteSpan info, ByteView key_arg) {
    ByteView key = cstr::text(key_arg);
    if (contains(key, '\\')) return;
    std::size_t i = 0;
    while (auto pair = next_pair(info, i)) {
        if (equal(ByteView(info).subspan(pair->key_start, pair->key_end - pair->key_start), key)) {
            std::size_t rest = pair->value_end;
            std::size_t rest_len = cstr::len(ByteView(info).subspan(rest));
            std::memmove(info.data() + pair->start, info.data() + rest, rest_len);
            info[pair->start + rest_len] = 0;
            return;
        }
        if (cstr::at(info, pair->value_end) == 0) return;
        i = pair->value_end;
    }
}

// The sizes one variant of Info_SetValueForKey works with.
struct SetLimits {
    std::size_t accepted;     // the string must be shorter than this to start with
    std::size_t value_scan;   // how much of the value is looked at
    std::size_t pair_buffer;  // the buffer \key\value is formatted into
    std::size_t longest;      // the longest the string may become
    Refusal too_long;
};

std::optional<Refusal> set_value(ByteSpan info, ByteView key_arg, ByteView value_arg, const SetLimits& limits) {
    ByteView key = cstr::text(key_arg);
    ByteView value = cstr::text(value_arg);
    if (cstr::len(info) >= limits.accepted) return Refusal::Oversize;
    std::vector<std::uint8_t> clean;
    for (std::size_t i = 0; i < value.size() && i < limits.value_scan; ++i) {
        std::uint8_t c = value[i];
        if (c != '\\' && c != ';' && c != '"') clean.push_back(c);
    }
    if (contains(key, '\\')) return Refusal::KeyWithBackslash;
    if (contains(key, ';')) return Refusal::KeyWithSemicolon;
    if (contains(key, '"')) return Refusal::KeyWithQuote;
    remove_pair(info, key);
    if (clean.empty()) return std::nullopt;
    std::vector<std::uint8_t> text;
    text.reserve(key.size() + clean.size() + 2);
    text.push_back('\\');
    text.insert(text.end(), key.begin(), key.end());
    text.push_back('\\');
    text.insert(text.end(), clean.begin(), clean.end());
    std::vector<std::uint8_t> formatted(limits.pair_buffer, 0);
    if (crt::sprintf(formatted, limits.pair_buffer, text) <= 0) return Refusal::PairTooLong;
    ByteView pair = cstr::text(formatted);
    if (pair.size() + cstr::len(info) > limits.longest) return limits.too_long;
    if (!cstr::append(info, pair)) return Refusal::NoRoom;
    return std::nullopt;
}

}  // namespace

Expected<ByteView> value_for_key(ByteView info_arg, ByteView key) {
    ByteView info = cstr::text(info_arg);
    if (info.size() >= kBigInfoString) return GameError::drop("Info_ValueForKey: oversize infostring");
    std::size_t i = 0;
    while (auto pair = next_pair(info, i)) {
        // Call of Duty 2 passes INT_MAX, United Offensive 99999: keys this
        // short never reach either limit.
        ByteView pair_key = info.subspan(pair->key_start, pair->key_end - pair->key_start);
        if (strings::stricmpn(key, pair_key, strings::kStricmpLimitUo) == 0) {
            return info.subspan(pair->value_start, pair->value_end - pair->value_start);
        }
        if (cstr::at(info, pair->value_end) == 0) break;
        i = pair->value_end;
    }
    return ByteView{};
}

Status remove_key(ByteSpan info, ByteView key) {
    if (cstr::len(info) >= kMaxInfoString) return GameError::drop("Info_RemoveKey: oversize infostring");
    remove_pair(info, key);
    return std::nullopt;
}

Status remove_key_big(ByteSpan info, ByteView key) {
    if (cstr::len(info) >= kBigInfoString) return GameError::drop("Info_RemoveKey_Big: oversize infostring");
    remove_pair(info, key);
    return std::nullopt;
}

bool validate(ByteView s) {
    ByteView text = cstr::text(s);
    return !contains(text, '"') && !contains(text, ';');
}

const char* message_format(Refusal refusal, Game game) {
    switch (refusal) {
        case Refusal::Oversize: return "Info_SetValueForKey: oversize infostring";
        case Refusal::KeyWithBackslash: return "Can't use keys with a \\\nkey: '%s'\nvalue: '%s'";
        case Refusal::KeyWithSemicolon: return "Can't use keys with a semicolon\nkey: '%s'\nvalue: '%s'";
        case Refusal::KeyWithQuote: return "Can't use keys with a \"\nkey: '%s'\nvalue: '%s'";
        case Refusal::PairTooLong:
            return game == Game::CallOfDuty2
                       ? "Info buffer length exceeded, not including key/value pair in response\n"
                       : "Server info buffer length exceeded, not including key/value pair in response\n";
        case Refusal::StringTooLong:
            return "Info string length exceeded\nkey: '%s'\nvalue: '%s'\nInfo string:\n%s\n";
        case Refusal::BigStringTooLong:
            return "BIG Info string length exceeded\nkey: '%s'\nvalue: '%s'\nInfo string:\n%s\n";
        case Refusal::NoRoom: return "";
    }
    return "";
}

std::optional<Refusal> set_value_for_key(ByteSpan info, ByteView key, ByteView value) {
    static constexpr SetLimits limits{kMaxInfoString, kMaxInfoString - 1, kMaxInfoString, kMaxInfoString,
                                      Refusal::StringTooLong};
    return set_value(info, key, value, limits);
}

std::optional<Refusal> set_value_for_key_big(Game game, ByteSpan info, ByteView key, ByteView value) {
    const SetLimits limits = game == Game::CallOfDuty2
                                 ? SetLimits{kBigInfoString, kBigInfoString - 1, kBigInfoString, kMaxInfoString,
                                             Refusal::StringTooLong}
                                 : SetLimits{kBigInfoString, kBigInfoString - 1, kBigInfoString, kBigInfoString,
                                             Refusal::BigStringTooLong};
    return set_value(info, key, value, limits);
}

}  // namespace iw_pc::shared::info
