// Requests for iw_pc::net::netchan (the C++ version). The specs are in
// tools/decomp/x86/specs/net_netchan.py: a channel token is 11 big-endian
// 32-bit numbers (outgoing sequence, source, dropped, incoming sequence,
// qport, fragment sequence, fragment length, unsent fragments, unsent
// fragment start, unsent length, the original's profiling pointer), the
// 20-byte netadr_t, then the fragment buffer and the unsent buffer.

#include <memory>

#include "iw_pc/net/netchan.h"
#include "msg_arg.h"
#include "net_common.h"

namespace pc_check {

using iw_pc::ByteSpan;
using iw_pc::ByteView;
using iw_pc::Game;
using iw_pc::net::Netchan;
using iw_pc::net::NetSource;
using iw_pc::net::Packet;

namespace {

constexpr std::size_t kChanWords = 11;

std::size_t chan_token_size(Game game) { return kChanWords * 4 + kAddressSize + 2 * iw_pc::net::max_message_length(game); }

std::int32_t word(ByteView b, std::size_t index) {
    std::size_t at = index * 4;
    return static_cast<std::int32_t>(static_cast<std::uint32_t>(b[at]) << 24 | static_cast<std::uint32_t>(b[at + 1]) << 16 |
                                     static_cast<std::uint32_t>(b[at + 2]) << 8 | b[at + 3]);
}

struct Chan {
    std::unique_ptr<Netchan> chan;
    std::int32_t profile = 0;
};

std::optional<Chan> chan_from_bytes(Game game, ByteView bytes, std::string& error) {
    auto address = address_from(bytes.subspan(kChanWords * 4), error);
    if (!address) return std::nullopt;
    std::int32_t source = word(bytes, 1);
    if (source != 0 && source != 1) {
        error = "netsrc " + std::to_string(source);
        return std::nullopt;
    }
    for (std::size_t k : {std::size_t{6}, std::size_t{8}, std::size_t{9}}) {
        if (word(bytes, k) < 0) {
            error = "negative size";
            return std::nullopt;
        }
    }
    Chan c;
    c.chan = std::make_unique<Netchan>(game, static_cast<NetSource>(source), *address, word(bytes, 4));
    Netchan& chan = *c.chan;
    chan.outgoing_sequence = word(bytes, 0);
    chan.dropped = word(bytes, 2);
    chan.incoming_sequence = word(bytes, 3);
    chan.fragment_sequence = word(bytes, 5);
    chan.fragment_length = static_cast<std::size_t>(word(bytes, 6));
    chan.unsent_fragments = word(bytes, 7) != 0;
    chan.unsent_fragment_start = static_cast<std::size_t>(word(bytes, 8));
    chan.unsent_length = static_cast<std::size_t>(word(bytes, 9));
    std::size_t max = iw_pc::net::max_message_length(game);
    ByteView buffers = bytes.subspan(kChanWords * 4 + kAddressSize);
    std::copy(buffers.begin(), buffers.begin() + static_cast<std::ptrdiff_t>(max), chan.fragment_buffer.begin());
    std::copy(buffers.begin() + static_cast<std::ptrdiff_t>(max), buffers.begin() + static_cast<std::ptrdiff_t>(2 * max),
              chan.unsent_buffer.begin());
    c.profile = word(bytes, 10);
    return c;
}

std::optional<Chan> chan_from(Game game, Args& args, std::size_t i, std::string& error) {
    Bytes* bytes = args.buffer(i);
    if (!bytes) {
        error = "NULL netchan";
        return std::nullopt;
    }
    if (bytes->size() != chan_token_size(game)) {
        error = "netchan token size";
        return std::nullopt;
    }
    return chan_from_bytes(game, *bytes, error);
}

void chan_to_bytes(const Netchan& chan, std::int32_t profile, ByteSpan bytes) {
    std::int32_t words[kChanWords] = {chan.outgoing_sequence,
                                      chan.source == NetSource::Client ? 0 : 1,
                                      chan.dropped,
                                      chan.incoming_sequence,
                                      chan.qport,
                                      chan.fragment_sequence,
                                      static_cast<std::int32_t>(chan.fragment_length),
                                      chan.unsent_fragments ? 1 : 0,
                                      static_cast<std::int32_t>(chan.unsent_fragment_start),
                                      static_cast<std::int32_t>(chan.unsent_length),
                                      profile};
    for (std::size_t k = 0; k < kChanWords; ++k) {
        auto v = static_cast<std::uint32_t>(words[k]);
        for (int j = 0; j < 4; ++j) bytes[k * 4 + static_cast<std::size_t>(j)] = static_cast<std::uint8_t>(v >> (24 - 8 * j));
    }
    std::size_t at = kChanWords * 4;
    auto address = address_bytes(chan.remote_address);
    std::copy(address.begin(), address.end(), bytes.begin() + static_cast<std::ptrdiff_t>(at));
    std::size_t max = chan.fragment_buffer.size();
    std::copy(chan.fragment_buffer.begin(), chan.fragment_buffer.end(), bytes.begin() + static_cast<std::ptrdiff_t>(at + kAddressSize));
    std::copy(chan.unsent_buffer.begin(), chan.unsent_buffer.end(),
              bytes.begin() + static_cast<std::ptrdiff_t>(at + kAddressSize + max));
}

}  // namespace

bool chan_from_arg(Game game, Args& args, std::size_t i, Netchan& chan, std::int32_t& profile, std::string& error) {
    auto c = chan_from(game, args, i, error);
    if (!c) return false;
    chan = *c->chan;
    profile = c->profile;
    return true;
}

void chan_to_arg(const Netchan& chan, std::int32_t profile, Args& args, std::size_t i) {
    chan_to_bytes(chan, profile, *args.buffer(i));
}

namespace {

// The original's result after sending: Call of Duty 2 returns whether the
// packet went out (always, here).
Answer sent_result(Game game) { return game == Game::CallOfDuty2 ? integer(1) : nothing(); }

Answer setup(Game game, Args& args) {
    std::string error;
    auto c = chan_from(game, args, 0, error);
    if (!c) return Answer::fail(error);
    std::int32_t source = args.i32(1);
    if (source != 0 && source != 1) return Answer::fail("netsrc " + std::to_string(source));
    auto address = address_from(args.bytes(2), error);
    if (!address) return Answer::fail(error);
    c->chan->setup(static_cast<NetSource>(source), *address, args.i32(3));
    chan_to_bytes(*c->chan, 0, *args.buffer(0));
    return nothing();
}

Answer transmit(Game game, Args& args) {
    std::string error;
    auto c = chan_from(game, args, 0, error);
    if (!c) return Answer::fail(error);
    auto length = static_cast<std::size_t>(args.int64(1));
    ByteView data_arg = args.bytes(2);
    if (length > data_arg.size()) return Answer::fail("length past the data");
    Bytes data(data_arg.begin(), data_arg.begin() + static_cast<std::ptrdiff_t>(length));
    Packet packet;
    if (auto e = c->chan->transmit(data, packet)) return game_error(*e);
    chan_to_bytes(*c->chan, c->profile, *args.buffer(0));
    Bytes* sent = args.buffer(3);
    if (!sent) return Answer::fail("NULL sent");
    record_sent(*sent, c->chan->remote_address, packet.bytes());
    return sent_result(game);
}

Answer transmit_next_fragment(Game game, Args& args) {
    std::string error;
    auto c = chan_from(game, args, 0, error);
    if (!c) return Answer::fail(error);
    Packet packet;
    c->chan->transmit_next_fragment(packet);
    chan_to_bytes(*c->chan, c->profile, *args.buffer(0));
    Bytes* sent = args.buffer(1);
    if (!sent) return Answer::fail("NULL sent");
    record_sent(*sent, c->chan->remote_address, packet.bytes());
    return sent_result(game);
}

Answer process(Game game, Args& args) {
    std::string error;
    auto c = chan_from(game, args, 0, error);
    if (!c) return Answer::fail(error);
    bool whole = false;
    std::string e = with_msg(args, 1, game, [&](iw_pc::net::Msg& m) { whole = c->chan->process(m); });
    if (!e.empty()) return Answer::fail(e);
    chan_to_bytes(*c->chan, c->profile, *args.buffer(0));
    return integer(whole ? 1 : 0);
}

// SV_SendMessageToClient: (client, message, sent). The client token is the
// channel's, a 4-byte length and a description (challenge, text, time, the
// coded body); the reply holds the channel, the 32 frames (sent, acked,
// size) and nextSnapshotTime.
Answer send_message(Game game, Args& args) {
    Bytes message(args.bytes(1).begin(), args.bytes(1).end());
    if (message.size() < 24) return Answer::fail("short message");
    std::array<std::uint8_t, 4> head = {message[20], message[21], message[22], message[23]};
    Bytes* token = args.buffer(0);
    if (!token) return Answer::fail("NULL client");
    std::size_t size = chan_token_size(game);
    if (token->size() < size + 4) return Answer::fail("short client");
    std::string error;
    auto c = chan_from_bytes(game, ByteView(*token).first(size), error);
    if (!c) return Answer::fail(error);
    Bytes extra(token->begin() + static_cast<std::ptrdiff_t>(size + 4), token->end());
    auto d = Description::parse(extra);
    if (!d) return Answer::fail("bad description");
    std::int32_t time = d->integer("time");
    Bytes scratch(iw_pc::net::max_message_length(game) + 4, 0);
    Packet packet;
    auto sent = iw_pc::net::send_server_message(*c->chan, d->integer("challenge"), d->get("text"), head, d->get("body"),
                                                scratch, packet);
    if (!sent) return game_error(sent.error());
    Bytes reply(size, 0);
    chan_to_bytes(*c->chan, c->profile, reply);
    auto put = [&](std::int32_t value) {
        auto v = static_cast<std::uint32_t>(value);
        for (int j = 0; j < 4; ++j) reply.push_back(static_cast<std::uint8_t>(v >> (24 - 8 * j)));
    };
    for (std::size_t frame = 0; frame < 32; ++frame) {
        if (frame == sent.value().frame) {
            put(time);
            put(-1);
            put(static_cast<std::int32_t>(sent.value().size));
        } else {
            put(0);
            put(0);
            put(0);
        }
    }
    put(static_cast<std::int32_t>(static_cast<std::uint32_t>(time) - 1));  // a LAN client may get the next snapshot at once
    *args.buffer(0) = reply;
    Bytes* out = args.buffer(2);
    if (!out) return Answer::fail("NULL sent");
    record_sent(*out, c->chan->remote_address, packet.bytes());
    return nothing();
}

// The scramblers: (data, key state, length). The key state is the key's
// numbers (big-endian 32-bit words, `count` of them), then the text.
template <class Key>
Answer scramble(Args& args, std::size_t count, Key key) {
    Bytes state(args.bytes(1).begin(), args.bytes(1).end());
    if (state.size() < count * 4) return Answer::fail("short key state");
    std::vector<std::int32_t> numbers;
    for (std::size_t k = 0; k < count; ++k) numbers.push_back(word(state, k));
    Bytes text(state.begin() + static_cast<std::ptrdiff_t>(count * 4), state.end());
    std::int64_t length = args.int64(2);
    Bytes* data = args.buffer(0);
    if (!data) return Answer::fail("NULL data");
    // The originals do nothing for a length of 0 or less.
    std::size_t n = length > 0 ? static_cast<std::size_t>(length) : 0;
    if (n > data->size()) return Answer::fail("length past the data");
    iw_pc::net::scramble(ByteSpan(*data).first(n), key(numbers), text);
    return nothing();
}

}  // namespace

std::optional<Answer> dispatch_netchan(Game game, const std::string& f, Args& args) {
    namespace net = iw_pc::net;
    if (f == "Netchan_Setup") return setup(game, args);
    if (f == "Netchan_Transmit") return transmit(game, args);
    if (f == "Netchan_TransmitNextFragment") return transmit_next_fragment(game, args);
    if (f == "Netchan_Process") return process(game, args);
    if (f == "SV_SendMessageToClient") return send_message(game, args);
    if (f == "SV_Netchan_Encode") return scramble(args, 2, [](const auto& n) { return net::server_encode_key(n[0], n[1]); });
    if (f == "SV_Netchan_Decode") return scramble(args, 3, [](const auto& n) { return net::server_decode_key(n[0], n[1], n[2]); });
    if (f == "CL_Netchan_Encode") return scramble(args, 3, [](const auto& n) { return net::client_encode_key(n[0], n[1], n[2]); });
    if (f == "CL_Netchan_Decode") return scramble(args, 2, [](const auto& n) { return net::client_decode_key(n[0], n[1]); });
    return std::nullopt;
}

}  // namespace pc_check
