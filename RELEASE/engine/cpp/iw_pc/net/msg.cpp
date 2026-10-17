#include "iw_pc/net/msg.h"

#include <cmath>
#include <cstring>

#include "iw_pc/cstr.h"

namespace iw_pc::net {

namespace {

// 65536 / 360 as the games store it (a float): degrees to 16-bit steps.
constexpr float kAngle16Scale = 65536.0f / 360.0f;
// 256 / 360 as a float: degrees to 8-bit steps.
constexpr float kAngle8Scale = 256.0f / 360.0f;
// 360 / 65536, exact in a float: 16-bit steps to degrees.
constexpr float kAngle16ToDegrees = 360.0f / 65536.0f;

// kbitmask[bits]: the low `bits` bits set (0 to 32).
std::int32_t low_bits_mask(std::uint32_t bits) {
    return bits >= 32 ? -1 : static_cast<std::int32_t>((1u << bits) - 1);
}

// angle * scale as the game's x87 computes it: Call of Duty 2 rounds the
// product to 24 bits (a float store stands in for that; the two only differ
// beyond float's exponent range, where truncation gives the same result),
// United Offensive keeps it exact in 53 bits.
double scaled(Game game, float angle, float scale) {
    if (game == Game::CallOfDuty2) {
        float product = angle * scale;
        return product;
    }
    return static_cast<double>(angle) * static_cast<double>(scale);
}

// A character as the string writers send it.
std::uint8_t written_char(Game game, std::uint8_t c) {
    if (c == 0x92) return '\'';
    if (c >= 0x80 && game == Game::UnitedOffensive) return '.';
    return c;
}

// A character as the string readers keep it.
std::uint8_t read_char(Game game, std::uint8_t c, bool percent) {
    if (c == '%' && percent) return '.';
    if (c == 0x92) return '\'';
    if (c >= 0x80 && game == Game::UnitedOffensive) return '.';
    return c;
}

}  // namespace

const char* message_format(StringTooLong refusal) {
    return refusal == StringTooLong::Normal ? "MSG_WriteString: MAX_STRING_CHARS" : "MSG_WriteString: BIG_INFO_STRING";
}

const char* message_format(ReliableCommandWarning warning) {
    return warning == ReliableCommandWarning::TooLong
               ? "WARNING: Reliable command is too long (%i/%i) and will be truncated: '%s'\n"
               : "WARNING: Empty reliable command\n";
}

std::int64_t ftol2(double value) {
    constexpr double kLimit = 9223372036854775808.0;  // 2^63
    if (std::isnan(value) || !(value >= -kLimit && value < kLimit)) return INT64_MIN;
    return static_cast<std::int64_t>(std::trunc(value));
}

void Msg::copy_to(std::size_t at, ByteView bytes) {
    if (at >= data.size()) return;
    std::size_t n = std::min(bytes.size(), data.size() - at);
    if (n > 0) std::memmove(data.data() + at, bytes.data(), n);
}

void Msg::start_bit_byte() {
    bit = cursize * 8;
    set_byte(cursize, 0);
    ++cursize;
}

std::optional<std::int32_t> Msg::next_read_bit() {
    std::size_t shift = bit & 7;
    if (shift == 0) {
        if (cod2() && readcount >= cursize) {
            overflowed = true;
            return std::nullopt;
        }
        bit = readcount * 8;
        ++readcount;
    }
    std::int32_t value = (byte_at(bit >> 3) >> shift) & 1;
    ++bit;
    return value;
}

void Msg::begin_reading() {
    overflowed = false;
    readcount = 0;
    bit = 0;
}

std::size_t Msg::used_bit_count() const { return cursize * 8 - ((0 - bit) & 7); }

void Msg::write_bits(std::int32_t value, std::uint32_t bits) {
    if (cursize + 4 > maxsize) {
        overflowed = true;
        return;
    }
    for (std::uint32_t i = 0; i < bits; ++i) {
        std::size_t shift = bit & 7;
        if (shift == 0) start_bit_byte();
        if (value & 1) or_byte(bit >> 3, static_cast<std::uint8_t>(1u << shift));
        ++bit;
        value >>= 1;  // arithmetic: past 32 bits the sign repeats
    }
}

void Msg::write_bit0() {
    if (cursize >= maxsize) {
        overflowed = true;
        return;
    }
    if ((bit & 7) == 0) start_bit_byte();
    ++bit;
}

void Msg::write_bit1() {
    if (cursize >= maxsize) {
        overflowed = true;
        return;
    }
    std::size_t shift = bit & 7;
    if (shift == 0) start_bit_byte();
    or_byte(bit >> 3, static_cast<std::uint8_t>(1u << shift));
    ++bit;
}

std::int32_t Msg::read_bits(std::uint32_t bits) {
    std::uint32_t value = 0;
    for (std::uint32_t i = 0; i < bits; ++i) {
        auto b = next_read_bit();
        if (!b) return -1;
        value |= static_cast<std::uint32_t>(*b) << (i & 31);
    }
    return static_cast<std::int32_t>(value);
}

std::int32_t Msg::read_bit() { return next_read_bit().value_or(-1); }

void Msg::write_byte(std::uint8_t c) {
    if (cursize >= maxsize) {
        overflowed = true;
        return;
    }
    set_byte(cursize, c);
    ++cursize;
}

void Msg::write_data(ByteView bytes) {
    if (cursize + bytes.size() > maxsize) {
        overflowed = true;
        return;
    }
    copy_to(cursize, bytes);
    cursize += bytes.size();
}

void Msg::write_short(std::int16_t c) {
    auto u = static_cast<std::uint16_t>(c);
    const std::uint8_t b[2] = {static_cast<std::uint8_t>(u), static_cast<std::uint8_t>(u >> 8)};
    write_data(b);
}

void Msg::write_long(std::int32_t c) {
    auto u = static_cast<std::uint32_t>(c);
    const std::uint8_t b[4] = {static_cast<std::uint8_t>(u), static_cast<std::uint8_t>(u >> 8),
                               static_cast<std::uint8_t>(u >> 16), static_cast<std::uint8_t>(u >> 24)};
    write_data(b);
}

std::optional<StringTooLong> Msg::write_limited_string(ByteView s, std::size_t limit, StringTooLong refusal) {
    std::size_t length = cstr::len(s);
    if (length >= limit) {
        const std::uint8_t zero[1] = {0};
        write_data(zero);
        return refusal;
    }
    if (cursize + length + 1 > maxsize) {
        overflowed = true;
        return std::nullopt;
    }
    for (std::size_t i = 0; i < length; ++i) set_byte(cursize + i, written_char(game, s[i]));
    set_byte(cursize + length, 0);
    cursize += length + 1;
    return std::nullopt;
}

std::optional<StringTooLong> Msg::write_string(ByteView s) {
    return write_limited_string(s, kMaxStringChars, StringTooLong::Normal);
}

std::optional<StringTooLong> Msg::write_big_string(ByteView s) {
    return write_limited_string(s, kBigInfoString, StringTooLong::Big);
}

void Msg::write_angle(float angle) {
    if (cursize >= maxsize) {
        overflowed = true;
        return;
    }
    std::int64_t steps = ftol2(scaled(game, angle, kAngle8Scale));
    set_byte(cursize, static_cast<std::uint8_t>(steps));
    ++cursize;
}

void Msg::write_angle16(float angle) {
    if (cursize + 2 > maxsize) {
        overflowed = true;
        return;
    }
    auto steps = static_cast<std::uint16_t>(ftol2(scaled(game, angle, kAngle16Scale)));
    const std::uint8_t b[2] = {static_cast<std::uint8_t>(steps), static_cast<std::uint8_t>(steps >> 8)};
    copy_to(cursize, b);
    cursize += 2;
}

bool Msg::read_raw(std::uint8_t* out, std::size_t n) {
    if (readcount + n > cursize) {
        if (cod2()) overflowed = true;
        return false;
    }
    for (std::size_t i = 0; i < n; ++i) out[i] = byte_at(readcount + i);
    readcount += n;
    return true;
}

std::int32_t Msg::read_byte() {
    std::uint8_t b[1];
    return read_raw(b, 1) ? b[0] : -1;
}

std::int32_t Msg::read_short() {
    std::uint8_t b[2];
    if (!read_raw(b, 2)) return -1;
    return static_cast<std::int16_t>(static_cast<std::uint16_t>(b[0] | b[1] << 8));
}

std::int32_t Msg::read_long() {
    std::uint8_t b[4];
    if (!read_raw(b, 4)) return -1;
    return static_cast<std::int32_t>(static_cast<std::uint32_t>(b[0]) | static_cast<std::uint32_t>(b[1]) << 8 |
                                     static_cast<std::uint32_t>(b[2]) << 16 | static_cast<std::uint32_t>(b[3]) << 24);
}

void Msg::read_data(ByteSpan out) {
    if (readcount + out.size() > cursize) {
        if (cod2()) overflowed = true;
        std::fill(out.begin(), out.end(), std::uint8_t{0xFF});
        return;
    }
    for (std::size_t i = 0; i < out.size(); ++i) out[i] = byte_at(readcount + i);
    readcount += out.size();
}

ByteView Msg::read_string_into(ByteSpan out, StringReader reader) {
    std::size_t last = reader.size - 1;
    std::size_t length = 0;
    if (cod2()) {
        for (;;) {
            std::uint8_t c = 0;
            std::uint8_t b[1];
            if (read_raw(b, 1)) c = (reader.line && b[0] == '\n') ? 0 : b[0];  // missing: overflowed is set
            if (length < reader.size) out[length] = read_char(game, c, reader.percent);
            if (c == 0) break;
            ++length;
        }
        out[last] = 0;
        length = std::min(length, last);
    } else {
        while (length < last) {
            std::uint8_t b[1];
            if (!read_raw(b, 1)) break;
            std::uint8_t c = b[0];
            if (c == 0 || (reader.line && c == '\n')) break;
            out[length++] = read_char(game, c, reader.percent);
        }
        out[length] = 0;
    }
    return ByteView(out).first(length);
}

ByteView Msg::read_string(std::array<std::uint8_t, kMaxStringChars>& out) {
    return read_string_into(out, {kMaxStringChars, false, false});
}

ByteView Msg::read_big_string(std::array<std::uint8_t, kBigInfoString>& out) {
    return read_string_into(out, {kBigInfoString, true, false});
}

ByteView Msg::read_string_line(std::array<std::uint8_t, kMaxStringChars>& out) {
    return read_string_into(out, {kMaxStringChars, true, true});
}

float Msg::read_angle16() { return static_cast<float>(read_short()) * kAngle16ToDegrees; }

void Msg::write_delta_key(std::int32_t key, std::int32_t old_value, std::int32_t new_value, std::uint32_t bits) {
    if (old_value == new_value) {
        write_bit0();
        return;
    }
    write_bit1();
    write_bits(new_value ^ key, bits);
}

std::int32_t Msg::read_delta_key(std::int32_t key, std::int32_t old_value, std::uint32_t bits) {
    if (!read_change_bit()) return old_value;
    return read_bits(bits) ^ (key & low_bits_mask(bits));
}

void Msg::write_delta_key_byte(std::int32_t key, std::int32_t old_value, std::int32_t new_value) {
    if (static_cast<std::uint8_t>(old_value) == static_cast<std::uint8_t>(new_value)) {
        write_bit0();
        return;
    }
    write_bit1();
    write_byte(static_cast<std::uint8_t>(key ^ new_value));
}

std::int32_t Msg::read_delta_key_byte(std::int32_t key, std::int32_t old_value) {
    if (!read_change_bit()) return old_value;
    return static_cast<std::uint8_t>(static_cast<std::uint8_t>(key) ^ static_cast<std::uint8_t>(read_byte()));
}

void Msg::write_delta_key_short(std::int32_t key, std::int32_t old_value, std::int32_t new_value) {
    if (static_cast<std::int16_t>(old_value) == static_cast<std::int16_t>(new_value)) {
        write_bit0();
        return;
    }
    write_bit1();
    write_short(static_cast<std::int16_t>(key ^ new_value));
}

std::int32_t Msg::read_delta_key_short(std::int32_t key, std::int32_t old_value) {
    if (!read_change_bit()) return old_value;
    return static_cast<std::int16_t>(static_cast<std::int16_t>(key) ^ static_cast<std::int16_t>(read_short()));
}

std::optional<ReliableCommandWarning> write_reliable_command_to_buffer(Game game, ByteView command, ByteSpan out) {
    std::size_t length = cstr::len(command);
    std::optional<ReliableCommandWarning> warning;
    if (length >= out.size()) warning = ReliableCommandWarning::TooLong;
    else if (length == 0) warning = ReliableCommandWarning::Empty;
    for (std::size_t i = 0; i < out.size(); ++i) {
        std::uint8_t c = cstr::at(command, i);
        if (c == 0) {
            out[i] = 0;
            return warning;
        }
        std::uint8_t sent = written_char(game, c);
        out[i] = sent == '%' ? '.' : sent;
    }
    if (!out.empty()) out[out.size() - 1] = 0;
    return warning;
}

}  // namespace iw_pc::net
