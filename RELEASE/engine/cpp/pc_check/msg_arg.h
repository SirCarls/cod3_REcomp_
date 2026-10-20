// A message buffer argument (the checker's Msg kind, harness_x86.py): a b:
// token holding the msg_t fields overflowed, maxsize, cursize, readcount and
// bit as five big-endian 32-bit numbers, then the data bytes (maxsize of
// them plus the slack the original may run into). The reply carries it back
// in the same layout.
#pragma once

#include <cstdlib>
#include <functional>

#include "iw_pc/net/msg.h"
#include "iw_pc/net/protocol_data.h"
#include "protocol.h"

namespace pc_check {

inline constexpr std::size_t kMsgHeader = 20;

inline std::int32_t be_word(const Bytes& b, std::size_t index) {
    std::size_t at = index * 4;
    return static_cast<std::int32_t>(static_cast<std::uint32_t>(b[at]) << 24 | static_cast<std::uint32_t>(b[at + 1]) << 16 |
                                     static_cast<std::uint32_t>(b[at + 2]) << 8 | b[at + 3]);
}

inline void set_be_word(Bytes& b, std::size_t index, std::int32_t value) {
    auto v = static_cast<std::uint32_t>(value);
    std::size_t at = index * 4;
    b[at] = static_cast<std::uint8_t>(v >> 24);
    b[at + 1] = static_cast<std::uint8_t>(v >> 16);
    b[at + 2] = static_cast<std::uint8_t>(v >> 8);
    b[at + 3] = static_cast<std::uint8_t>(v);
}

/// Runs `f` on the message in argument `i` and writes the message back into
/// the argument. Returns an error text, or empty.
inline std::string with_msg(Args& args, std::size_t i, iw_pc::Game game, const std::function<void(iw_pc::net::Msg&)>& f) {
    Bytes* buffer = args.buffer(i);
    if (!buffer) return "NULL msg";
    if (buffer->size() < kMsgHeader) return "short msg token";
    const char* names[] = {"", "maxsize", "cursize", "readcount", "bit"};
    std::size_t fields[5];
    for (std::size_t k = 1; k < 5; ++k) {
        std::int32_t v = be_word(*buffer, k);
        if (v < 0) return std::string("negative msg ") + names[k];
        fields[k] = static_cast<std::size_t>(v);
    }
    iw_pc::net::Msg msg(game, iw_pc::ByteSpan(*buffer).subspan(kMsgHeader));
    msg.overflowed = be_word(*buffer, 0) != 0;
    msg.maxsize = fields[1];
    msg.cursize = fields[2];
    msg.readcount = fields[3];
    msg.bit = fields[4];
    f(msg);
    set_be_word(*buffer, 0, msg.overflowed ? 1 : 0);
    set_be_word(*buffer, 1, static_cast<std::int32_t>(msg.maxsize));
    set_be_word(*buffer, 2, static_cast<std::int32_t>(msg.cursize));
    set_be_word(*buffer, 3, static_cast<std::int32_t>(msg.readcount));
    set_be_word(*buffer, 4, static_cast<std::int32_t>(msg.bit));
    return {};
}

/// An answer from with_msg()'s error text and a result.
inline Answer after(const std::string& error, Answer ok) { return error.empty() ? std::move(ok) : Answer::fail(error); }

/// `game`'s executable, opened once from the folders IW_PC_INPUT names.
const iw_pc::net::GameExecutable* game_executable(iw_pc::Game game, std::string& error);

}  // namespace pc_check

namespace iw_pc::net {
struct FieldTables;
class MessageTree;
}  // namespace iw_pc::net

namespace pc_check {

/// `game`'s delta field tables, read once from its executable (net_delta.cpp).
const iw_pc::net::FieldTables* field_tables(iw_pc::Game game, std::string& error);
/// `game`'s message Huffman tree, built once from its executable (net_msg.cpp).
const iw_pc::net::MessageTree* message_tree_of(iw_pc::Game game, std::string& error);

}  // namespace pc_check
