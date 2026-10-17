// The message buffer, the games' msg_t: what every packet of a connection is
// written into and read from, bit for bit.
//
// Single bits and whole bytes are interleaved: a bit write that needs a new
// byte starts one at the end of the message (cursize), and the following
// bits fill that byte, lowest first; byte, short, long and string writes
// append raw bytes after it. Reading mirrors this with readcount and bit.
// Numbers are little-endian.
//
//                              Call of Duty 2 1.3             United Offensive 1.51
//   reading past cursize       marks overflowed, returns -1   bit readers read on; byte readers -1
//   string writers change      0x92 to '                      0x92 to ', every byte > 127 to .
//   string readers change      0x92 to '; % to . (lines, big) the same, and every byte > 127 to .
//   a string longer than the   the rest is read and dropped   reading stops
//   buffer
//   angles (x87 precision)     24 bits                        53 bits
//
// The original reads past the message through a pointer; the port reads
// `data` as far as it goes and zeros after it. Writes stay inside maxsize
// (and inside `data`).
#pragma once

#include <array>

#include "iw_pc/base.h"

namespace iw_pc::net {

/// MAX_STRING_CHARS: a normal string's buffer, terminator included.
inline constexpr std::size_t kMaxStringChars = 1024;
/// BIG_INFO_STRING: a big string's buffer (config strings).
inline constexpr std::size_t kBigInfoString = 8192;

/// A string writer's refusal: the string was too long, so an empty string
/// was written instead (the original prints a warning).
enum class StringTooLong { Normal, Big };
const char* message_format(StringTooLong refusal);

/// What write_reliable_command_to_buffer() warns about (it copies anyway).
enum class ReliableCommandWarning { TooLong, Empty };
const char* message_format(ReliableCommandWarning warning);

/// __ftol2, the C runtime's float-to-integer conversion (MSVC 7.1): the x87
/// value truncated toward zero to 64 bits; NaN and anything outside the
/// 64-bit range give INT64_MIN (the x87's "integer indefinite").
/// Original: __ftol2 at 0x0057F114 (CoD2MP_s.exe 1.3)
/// Original: __ftol2 at 0x00570FD8 (CoDUOMP.exe 1.51)
std::int64_t ftol2(double value);

/// The msg_t buffer: the message bytes and the positions in them. The fields
/// are public, as in the original, where the network code sets them directly.
class Msg {
public:
    Game game;
    ByteSpan data;            // usually maxsize long; may be longer (reads past maxsize see it)
    bool overflowed = false;  // a write didn't fit, or (CoD2) a read went past cursize
    std::size_t maxsize;
    std::size_t cursize = 0;
    std::size_t readcount = 0;
    std::size_t bit = 0;  // bit position from the start of data; bit % 8 == 0: the next bit starts a byte

    /// An empty message over `data`, maxsize its length.
    /// Original: MSG_Init at 0x00443DE0 (CoD2MP_s.exe 1.3)
    Msg(Game game, ByteSpan data) : game(game), data(data), maxsize(data.size()) {}

    /// Back to the start for reading: overflowed, readcount and bit cleared.
    /// Original: MSG_BeginReading at 0x00443E20 (CoD2MP_s.exe 1.3)
    void begin_reading();

    /// The number of bits written.
    /// Original: MSG_GetUsedBitCount at 0x00443E30 (CoD2MP_s.exe 1.3)
    std::size_t used_bit_count() const;

    /// Writes the low `bits` bits of value, lowest first; needs room for 4
    /// more bytes however few bits are written. Past 32 bits the sign repeats.
    /// Original: MSG_WriteBits at 0x00443E50 (CoD2MP_s.exe 1.3)
    /// Original: MSG_WriteBits at 0x004495F0 (CoDUOMP.exe 1.51)
    void write_bits(std::int32_t value, std::uint32_t bits);

    /// Original: MSG_WriteBit0 at 0x00443EC0 (CoD2MP_s.exe 1.3)
    /// Original: MSG_WriteBit0 at 0x00449660 (CoDUOMP.exe 1.51)
    void write_bit0();

    /// Original: MSG_WriteBit1 at 0x00443EF0 (CoD2MP_s.exe 1.3)
    /// Original: MSG_WriteBit1 at 0x00449690 (CoDUOMP.exe 1.51)
    void write_bit1();

    /// Reads `bits` bits, lowest first (bit 32 and up wrap, as x86 shifts).
    /// Call of Duty 2: no byte left before cursize marks the message
    /// overflowed and returns -1. Pass 0 for a count of zero or less.
    /// Original: MSG_ReadBits at 0x00443F40 (CoD2MP_s.exe 1.3)
    /// Original: MSG_ReadBits at 0x004496E0 (CoDUOMP.exe 1.51)
    std::int32_t read_bits(std::uint32_t bits);

    /// One bit: 0, 1, or -1 (see read_bits()).
    /// Original: MSG_ReadBit at 0x00443FB0 (CoD2MP_s.exe 1.3)
    /// Original: MSG_ReadBit at 0x00449740 (CoDUOMP.exe 1.51)
    std::int32_t read_bit();

    /// Original: MSG_WriteByte at 0x004440B0 (CoD2MP_s.exe 1.3)
    /// Original: MSG_WriteByte at 0x00449830 (CoDUOMP.exe 1.51)
    void write_byte(std::uint8_t c);

    /// Raw bytes, all or none.
    /// Original: MSG_WriteData at 0x004440D0 (CoD2MP_s.exe 1.3)
    /// Original: MSG_WriteData at 0x00449850 (CoDUOMP.exe 1.51)
    void write_data(ByteView bytes);

    /// Original: MSG_WriteShort at 0x00444110 (CoD2MP_s.exe 1.3)
    /// Original: MSG_WriteShort at 0x00449890 (CoDUOMP.exe 1.51)
    void write_short(std::int16_t c);

    /// Original: MSG_WriteLong at 0x00444140 (CoD2MP_s.exe 1.3)
    /// Original: MSG_WriteLong at 0x004498C0 (CoDUOMP.exe 1.51)
    void write_long(std::int32_t c);

    /// Appends s (up to its first NUL) and a terminator, all or none, 0x92
    /// sent as ' (UO: every byte above 127 as .). A string of 1024
    /// characters or more: an empty string is written and refused.
    /// Original: MSG_WriteString at 0x004441B0 (CoD2MP_s.exe 1.3)
    /// Original: MSG_WriteString at 0x004498F0 (CoDUOMP.exe 1.51)
    std::optional<StringTooLong> write_string(ByteView s);

    /// write_string() for config strings: the limit is 8192.
    /// Original: MSG_WriteBigString at 0x00444270 (CoD2MP_s.exe 1.3)
    /// Original: MSG_WriteBigString at 0x004499F0 (CoDUOMP.exe 1.51)
    std::optional<StringTooLong> write_big_string(ByteView s);

    /// An angle in degrees as one byte (256 steps a turn).
    /// Original: MSG_WriteAngle at 0x00444350 (CoD2MP_s.exe 1.3)
    /// Original: MSG_WriteAngle at 0x00449B50 (CoDUOMP.exe 1.51)
    void write_angle(float angle);

    /// An angle in degrees as 16 bits: angle * (65536 / 360) at the game's
    /// x87 precision (CoD2 24 bits, UO 53), truncated by __ftol2.
    /// Original: MSG_WriteAngle16 at 0x00444380 (CoD2MP_s.exe 1.3)
    /// Original: MSG_WriteAngle16 at 0x00449B80 (CoDUOMP.exe 1.51)
    void write_angle16(float angle);

    /// One raw byte (0 to 255), or -1 when none is left before cursize.
    /// Original: MSG_ReadByte at 0x004443C0 (CoD2MP_s.exe 1.3)
    /// Original: MSG_ReadByte at 0x00449BC0 (CoDUOMP.exe 1.51)
    std::int32_t read_byte();

    /// Original: MSG_ReadShort at 0x004443E0 (CoD2MP_s.exe 1.3)
    /// Original: MSG_ReadShort at 0x00449BE0 (CoDUOMP.exe 1.51)
    std::int32_t read_short();

    /// Original: MSG_ReadLong at 0x00444410 (CoD2MP_s.exe 1.3)
    /// Original: MSG_ReadLong at 0x00449C10 (CoDUOMP.exe 1.51)
    std::int32_t read_long();

    /// out.size() raw bytes; when fewer are left, nothing is read and out is
    /// filled with 0xFF.
    /// Original: MSG_ReadData at 0x00444600 (CoD2MP_s.exe 1.3)
    /// Original: MSG_ReadData at 0x00449D90 (CoDUOMP.exe 1.51)
    void read_data(ByteSpan out);

    /// Reads a string into `out` (the original's static buffer) and returns
    /// its characters. CoD2 keeps 1023 characters and reads the rest of a
    /// longer string; UO stops after 1023.
    /// Original: MSG_ReadString at 0x00444480 (CoD2MP_s.exe 1.3)
    /// Original: MSG_ReadString at 0x00449C30 (CoDUOMP.exe 1.51)
    ByteView read_string(std::array<std::uint8_t, kMaxStringChars>& out);

    /// read_string() for config strings: up to 8191 characters, % kept as .
    /// Original: MSG_ReadBigString at 0x004444E0 (CoD2MP_s.exe 1.3)
    /// Original: MSG_ReadBigString at 0x00449C80 (CoDUOMP.exe 1.51)
    ByteView read_big_string(std::array<std::uint8_t, kBigInfoString>& out);

    /// read_string() that also ends at a newline (read and dropped), % as .
    /// Original: MSG_ReadStringLine at 0x00444540 (CoD2MP_s.exe 1.3)
    /// Original: MSG_ReadStringLine at 0x00449CE0 (CoDUOMP.exe 1.51)
    ByteView read_string_line(std::array<std::uint8_t, kMaxStringChars>& out);

    /// A 16-bit angle in degrees (short * 360 / 65536, exact).
    /// Original: MSG_ReadAngle16 at 0x004445B0 (CoD2MP_s.exe 1.3)
    /// Original: MSG_ReadAngle16 at 0x00449D40 (CoDUOMP.exe 1.51)
    float read_angle16();

    /// A value as a change from old: a 0 bit when equal, else a 1 bit and
    /// the low `bits` bits of new ^ key.
    /// Original: MSG_WriteDeltaKey at 0x00444650 (CoD2MP_s.exe 1.3)
    /// Original: MSG_WriteDeltaKey at 0x00449E90 (CoDUOMP.exe 1.51)
    void write_delta_key(std::int32_t key, std::int32_t old_value, std::int32_t new_value, std::uint32_t bits);

    /// Original: MSG_ReadDeltaKey at 0x004446A0 (CoD2MP_s.exe 1.3)
    /// Original: MSG_ReadDeltaKey at 0x00449EE0 (CoDUOMP.exe 1.51)
    std::int32_t read_delta_key(std::int32_t key, std::int32_t old_value, std::uint32_t bits);

    /// Original: MSG_WriteDeltaKeyByte at 0x00444750 (CoD2MP_s.exe 1.3)
    /// Original: MSG_WriteDeltaKeyByte at 0x00449F80 (CoDUOMP.exe 1.51)
    void write_delta_key_byte(std::int32_t key, std::int32_t old_value, std::int32_t new_value);

    /// Original: MSG_ReadDeltaKeyByte at 0x004447B0 (CoD2MP_s.exe 1.3)
    /// Original: MSG_ReadDeltaKeyByte at 0x00449FE0 (CoDUOMP.exe 1.51)
    std::int32_t read_delta_key_byte(std::int32_t key, std::int32_t old_value);

    /// Original: MSG_WriteDeltaKeyShort at 0x00444840 (CoD2MP_s.exe 1.3)
    /// Original: MSG_WriteDeltaKeyShort at 0x0044A060 (CoDUOMP.exe 1.51)
    void write_delta_key_short(std::int32_t key, std::int32_t old_value, std::int32_t new_value);

    /// Original: MSG_ReadDeltaKeyShort at 0x004448B0 (CoD2MP_s.exe 1.3)
    /// Original: MSG_ReadDeltaKeyShort at 0x0044A0D0 (CoDUOMP.exe 1.51)
    std::int32_t read_delta_key_short(std::int32_t key, std::int32_t old_value);

private:
    struct StringReader {
        std::size_t size;
        bool percent;  // % becomes .
        bool line;     // a newline ends the string
    };

    bool cod2() const { return game == Game::CallOfDuty2; }
    std::uint8_t byte_at(std::size_t index) const { return index < data.size() ? data[index] : 0; }
    void set_byte(std::size_t index, std::uint8_t value) {
        if (index < data.size()) data[index] = value;
    }
    void or_byte(std::size_t index, std::uint8_t value) {
        if (index < data.size()) data[index] |= value;
    }
    void copy_to(std::size_t at, ByteView bytes);
    void start_bit_byte();
    std::optional<std::int32_t> next_read_bit();
    bool read_change_bit() { return next_read_bit() != std::optional<std::int32_t>(0); }
    bool read_raw(std::uint8_t* out, std::size_t n);
    std::optional<StringTooLong> write_limited_string(ByteView s, std::size_t limit, StringTooLong refusal);
    ByteView read_string_into(ByteSpan out, StringReader reader);
};

/// Copies a reliable command into `out` (out.size() being the original's
/// buffer size): at most out.size() - 1 characters and a terminator, 0x92
/// as ', % as . (UO: every byte above 127 as . too). The warning about a
/// cut or empty command comes back after the copy.
/// Original: MSG_WriteReliableCommandToBuffer at 0x00444940 (CoD2MP_s.exe 1.3)
/// Original: MSG_WriteReliableCommandToBuffer at 0x0044A150 (CoDUOMP.exe 1.51)
std::optional<ReliableCommandWarning> write_reliable_command_to_buffer(Game game, ByteView command, ByteSpan out);

}  // namespace iw_pc::net
