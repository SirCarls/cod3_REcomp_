// Requests for iw_pc::net::{msg, huffman} (the C++ version).

#include <array>
#include <cstring>
#include <memory>

#include "iw_pc/net/huffman.h"
#include "msg_arg.h"

namespace pc_check {

using iw_pc::ByteSpan;
using iw_pc::ByteView;
using iw_pc::Game;
using iw_pc::net::Msg;

const iw_pc::net::GameExecutable* game_executable(Game game, std::string& error) {
    struct Slot {
        std::unique_ptr<iw_pc::net::GameExecutable> exe;
        std::string error;
        bool tried = false;
    };
    static Slot slots[2];
    Slot& slot = slots[game == Game::UnitedOffensive ? 1 : 0];
    if (!slot.tried) {
        slot.tried = true;
        const char* folders = std::getenv("IW_PC_INPUT");
        if (!folders) {
            slot.error = "IW_PC_INPUT is not set";
        } else if (auto path = iw_pc::net::find_executable(game, folders)) {
            auto opened = iw_pc::net::GameExecutable::open(game, *path);
            if (auto* exe = std::get_if<iw_pc::net::GameExecutable>(&opened)) {
                slot.exe = std::make_unique<iw_pc::net::GameExecutable>(std::move(*exe));
            } else {
                slot.error = std::get<iw_pc::net::ProtocolDataError>(opened).message;
            }
        } else {
            slot.error = std::string(iw_pc::net::executable_name(game)) + " not found in IW_PC_INPUT";
        }
    }
    error = slot.error;
    return slot.exe.get();
}

namespace {

// The message tree of `game`, built once from its executable.
const iw_pc::net::MessageTree* message_tree(Game game, std::string& error) {
    static std::unique_ptr<iw_pc::net::MessageTree> trees[2];
    static std::string errors[2];
    std::size_t k = game == Game::UnitedOffensive ? 1 : 0;
    if (!trees[k] && errors[k].empty()) {
        const auto* exe = game_executable(game, errors[k]);
        if (exe) {
            auto frequencies = exe->huffman_frequencies();
            if (auto* f = std::get_if<std::array<std::int32_t, 256>>(&frequencies)) {
                trees[k] = std::make_unique<iw_pc::net::MessageTree>(*f);
            } else {
                errors[k] = std::get<iw_pc::net::ProtocolDataError>(frequencies).message;
            }
        }
    }
    error = errors[k];
    return trees[k].get();
}

// A 32-bit number in a 4-byte buffer argument, little-endian (as the guest keeps it).
std::optional<std::int32_t> le_int(Args& args, std::size_t i) {
    Bytes* b = args.buffer(i);
    if (!b || b->size() < 4) return std::nullopt;
    return static_cast<std::int32_t>(static_cast<std::uint32_t>((*b)[0]) | static_cast<std::uint32_t>((*b)[1]) << 8 |
                                     static_cast<std::uint32_t>((*b)[2]) << 16 | static_cast<std::uint32_t>((*b)[3]) << 24);
}

void set_le_int(Args& args, std::size_t i, std::int32_t value) {
    if (Bytes* b = args.buffer(i)) {
        auto v = static_cast<std::uint32_t>(value);
        for (int k = 0; k < 4; ++k) (*b)[k] = static_cast<std::uint8_t>(v >> (8 * k));
    }
}

// A global the checker passes as a big-endian 4-byte buffer (GlobalU32).
std::optional<std::int32_t> global(Args& args, std::size_t i) {
    Bytes* b = args.buffer(i);
    if (!b || b->size() < 4) return std::nullopt;
    return be_word(*b, 0);
}

void set_global(Args& args, std::size_t i, std::int32_t value) {
    if (Bytes* b = args.buffer(i)) set_be_word(*b, 0, value);
}

Answer printed_text(const char* format) {
    if (!format) return Answer(Ret(Printed{}));
    return Answer(Ret(Printed{Bytes(format, format + std::strlen(format))}));
}

template <std::size_t N>
Answer read_string(Args& args, Game game, ByteView (Msg::*read)(std::array<std::uint8_t, N>&)) {
    Bytes* buffer = args.buffer(1);
    if (!buffer) return Answer::fail("NULL buffer");
    if (buffer->size() != N) return Answer::fail("the string buffer must be " + std::to_string(N) + " bytes");
    std::array<std::uint8_t, N> out;
    std::copy(buffer->begin(), buffer->end(), out.begin());
    Bytes text;
    std::string error = with_msg(args, 0, game, [&](Msg& m) {
        ByteView t = (m.*read)(out);
        text.assign(t.begin(), t.end());
    });
    std::copy(out.begin(), out.end(), args.buffer(1)->begin());
    return after(error, Answer(Ret(Str{text})));
}

std::optional<Answer> huffman_dispatch(Game game, const std::string& f, Args& args) {
    bool uo = game == Game::UnitedOffensive;
    if (f == "MSG_WriteBitsCompress" || f == "MSG_ReadBitsCompress" || f == "Huff_offsetTransmit" ||
        f == "Huff_offsetReceive" || f == "Huff_transmit" || f == "Huff_Receive") {
        std::string error;
        const auto* tree = message_tree(game, error);
        if (!tree) return Answer::fail(error);
        if (f == "MSG_WriteBitsCompress" || f == "MSG_ReadBitsCompress") {
            auto length = static_cast<std::size_t>(args.int64(2));
            Bytes input(args.bytes(0).begin(), args.bytes(0).begin() + static_cast<std::ptrdiff_t>(length));
            Bytes* out = args.buffer(1);
            if (!out) return Answer::fail("NULL buffer");
            std::size_t n = f == "MSG_WriteBitsCompress" ? tree->write_bits_compress(input, *out)
                                                         : tree->read_bits_compress(input, *out);
            return integer(static_cast<std::int64_t>(n));
        }
        if (f == "Huff_offsetTransmit") {  // (huff, ch, fout, &offset), the bit position also in the global
            auto symbol = static_cast<std::uint16_t>(args.int64(1));
            auto offset_word = le_int(args, 3);
            if (!offset_word) return Answer::fail("NULL pointer");
            auto offset = static_cast<std::size_t>(*offset_word);
            Bytes* out = args.buffer(2);
            if (!out) return Answer::fail("NULL buffer");
            tree->offset_transmit(symbol, *out, offset);
            set_le_int(args, 3, static_cast<std::int32_t>(offset));
            set_global(args, 4, static_cast<std::int32_t>(offset));
            return nothing();
        }
        if (f == "Huff_offsetReceive") {  // (node, &ch, fin, &offset)
            Bytes input(args.bytes(2).begin(), args.bytes(2).end());
            auto offset_word = le_int(args, 3);
            if (!offset_word) return Answer::fail("NULL pointer");
            auto offset = static_cast<std::size_t>(*offset_word);
            std::uint16_t symbol = tree->offset_receive(input, offset);
            set_le_int(args, 1, symbol);
            set_le_int(args, 3, static_cast<std::int32_t>(offset));
            set_global(args, 4, static_cast<std::int32_t>(offset));
            return nothing();
        }
        if (f == "Huff_transmit") {  // (huff, ch, fout), the bit position in the global
            auto symbol = static_cast<std::uint16_t>(args.int64(1));
            auto bit_word = global(args, 3);
            if (!bit_word) return Answer::fail("NULL global");
            auto bit = static_cast<std::size_t>(*bit_word);
            Bytes* out = args.buffer(2);
            if (!out) return Answer::fail("NULL buffer");
            tree->tree().transmit(symbol, *out, bit);
            set_global(args, 3, static_cast<std::int32_t>(bit));
            return nothing();
        }
        // Huff_Receive: (node, &ch, fin) -> the symbol, or 0 without touching ch.
        Bytes input(args.bytes(2).begin(), args.bytes(2).end());
        auto bit_word = global(args, 3);
        if (!bit_word) return Answer::fail("NULL global");
        auto bit = static_cast<std::size_t>(*bit_word);
        auto symbol = tree->tree().receive(input, bit);
        if (symbol) set_le_int(args, 1, *symbol);
        set_global(args, 3, static_cast<std::int32_t>(bit));
        return integer(symbol ? *symbol : 0);
    }
    if ((f == "Huff_Compress" || f == "Huff_Decompress") && uo) {
        auto offset = static_cast<std::size_t>(args.int64(1));
        bool compress = f == "Huff_Compress";
        return after(with_msg(args, 0, game,
                              [&](Msg& m) {
                                  if (compress) iw_pc::net::compress_adaptive(m, offset);
                                  else iw_pc::net::decompress_adaptive(m, offset);
                              }),
                     nothing());
    }
    return std::nullopt;
}

}  // namespace

std::optional<Answer> dispatch_msg(Game game, const std::string& f, Args& args) {
    if (auto answer = huffman_dispatch(game, f, args)) return answer;
    bool cod2 = game == Game::CallOfDuty2;
    std::int64_t result = 0;
    auto run = [&](const std::function<void(Msg&)>& body) { return with_msg(args, 0, game, body); };
    // --- bits ---------------------------------------------------------------------
    if (f == "MSG_BeginReading" && cod2) return after(run([](Msg& m) { m.begin_reading(); }), nothing());
    if (f == "MSG_GetUsedBitCount" && cod2) {
        std::string e = run([&](Msg& m) { result = static_cast<std::int32_t>(m.used_bit_count()); });
        return after(e, integer(result));
    }
    if (f == "MSG_WriteBits") {
        std::int32_t value = args.i32(1);
        auto bits = static_cast<std::uint32_t>(args.int64(2));
        return after(run([&](Msg& m) { m.write_bits(value, bits); }), nothing());
    }
    if (f == "MSG_WriteBit0") return after(run([](Msg& m) { m.write_bit0(); }), nothing());
    if (f == "MSG_WriteBit1") return after(run([](Msg& m) { m.write_bit1(); }), nothing());
    if (f == "MSG_ReadBits") {
        // The original takes an int: zero or less reads nothing.
        auto bits = static_cast<std::uint32_t>(std::max<std::int64_t>(args.int64(1), 0));
        std::string e = run([&](Msg& m) { result = m.read_bits(bits); });
        return after(e, integer(result));
    }
    if (f == "MSG_ReadBit") {
        std::string e = run([&](Msg& m) { result = m.read_bit(); });
        return after(e, integer(result));
    }
    // --- bytes --------------------------------------------------------------------
    if (f == "MSG_WriteByte") {
        auto c = static_cast<std::uint8_t>(args.int64(1));
        return after(run([&](Msg& m) { m.write_byte(c); }), nothing());
    }
    if (f == "MSG_WriteData") {
        auto length = static_cast<std::size_t>(args.int64(2));
        Bytes bytes(args.bytes(1).begin(), args.bytes(1).begin() + static_cast<std::ptrdiff_t>(length));
        return after(run([&](Msg& m) { m.write_data(bytes); }), nothing());
    }
    if (f == "MSG_WriteShort") {
        auto c = static_cast<std::int16_t>(args.int64(1));
        return after(run([&](Msg& m) { m.write_short(c); }), nothing());
    }
    if (f == "MSG_WriteLong") {
        std::int32_t c = args.i32(1);
        return after(run([&](Msg& m) { m.write_long(c); }), nothing());
    }
    if (f == "MSG_ReadByte" || f == "MSG_ReadShort" || f == "MSG_ReadLong") {
        std::string e = run([&](Msg& m) {
            result = f == "MSG_ReadByte" ? m.read_byte() : f == "MSG_ReadShort" ? m.read_short() : m.read_long();
        });
        return after(e, integer(result));
    }
    if (f == "MSG_ReadData") {
        auto length = static_cast<std::size_t>(args.int64(2));
        Bytes* out_arg = args.buffer(1);
        if (!out_arg) return Answer::fail("NULL buffer");
        Bytes out = *out_arg;
        std::string e = run([&](Msg& m) { m.read_data(ByteSpan(out).first(length)); });
        *args.buffer(1) = out;
        return after(e, nothing());
    }
    // --- strings ------------------------------------------------------------------
    if (f == "MSG_WriteString" || f == "MSG_WriteBigString") {
        Bytes text(args.text(0).begin(), args.text(0).end());  // United Offensive: NULL is ""
        bool big = f == "MSG_WriteBigString";
        std::optional<iw_pc::net::StringTooLong> refusal;
        std::string e = with_msg(args, 1, game, [&](Msg& m) { refusal = big ? m.write_big_string(text) : m.write_string(text); });
        return after(e, printed_text(refusal ? iw_pc::net::message_format(*refusal) : nullptr));
    }
    if (f == "MSG_ReadString") return read_string<iw_pc::net::kMaxStringChars>(args, game, &Msg::read_string);
    if (f == "MSG_ReadBigString") return read_string<iw_pc::net::kBigInfoString>(args, game, &Msg::read_big_string);
    if (f == "MSG_ReadStringLine") return read_string<iw_pc::net::kMaxStringChars>(args, game, &Msg::read_string_line);
    if (f == "MSG_WriteReliableCommandToBuffer") {
        Bytes command(args.text(0).begin(), args.text(0).end());
        auto size = static_cast<std::size_t>(args.int64(2));
        Bytes* out = args.buffer(1);
        if (!out) return Answer::fail("NULL buffer");
        auto warning = iw_pc::net::write_reliable_command_to_buffer(game, command, ByteSpan(*out).first(size));
        return printed_text(warning ? iw_pc::net::message_format(*warning) : nullptr);
    }
    // --- angles -------------------------------------------------------------------
    if (f == "MSG_WriteAngle" || f == "MSG_WriteAngle16") {
        float angle = args.f32(1);
        bool eight = f == "MSG_WriteAngle";
        return after(run([&](Msg& m) { eight ? m.write_angle(angle) : m.write_angle16(angle); }), nothing());
    }
    if (f == "MSG_ReadAngle16") {
        float angle = 0;
        std::string e = run([&](Msg& m) { angle = m.read_angle16(); });
        return after(e, Answer(Ret(angle)));
    }
    // --- key-coded values -----------------------------------------------------------
    if (f == "MSG_WriteDeltaKey") {
        std::int32_t key = args.i32(1), old_value = args.i32(2), new_value = args.i32(3);
        auto bits = static_cast<std::uint32_t>(args.int64(4));
        return after(run([&](Msg& m) { m.write_delta_key(key, old_value, new_value, bits); }), nothing());
    }
    if (f == "MSG_ReadDeltaKey") {
        std::int32_t key = args.i32(1), old_value = args.i32(2);
        auto bits = static_cast<std::uint32_t>(args.int64(3));
        std::string e = run([&](Msg& m) { result = m.read_delta_key(key, old_value, bits); });
        return after(e, integer(result));
    }
    if (f == "MSG_WriteDeltaKeyByte" || f == "MSG_WriteDeltaKeyShort") {
        std::int32_t key = args.i32(1), old_value = args.i32(2), new_value = args.i32(3);
        bool byte = f == "MSG_WriteDeltaKeyByte";
        return after(run([&](Msg& m) {
                         if (byte) m.write_delta_key_byte(key, old_value, new_value);
                         else m.write_delta_key_short(key, old_value, new_value);
                     }),
                     nothing());
    }
    if (f == "MSG_ReadDeltaKeyByte" || f == "MSG_ReadDeltaKeyShort") {
        std::int32_t key = args.i32(1), old_value = args.i32(2);
        bool byte = f == "MSG_ReadDeltaKeyByte";
        std::string e = run([&](Msg& m) {
            result = byte ? m.read_delta_key_byte(key, old_value) : m.read_delta_key_short(key, old_value);
        });
        return after(e, integer(result));
    }
    return std::nullopt;
}

}  // namespace pc_check
