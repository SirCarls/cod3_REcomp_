#include "iw_pc/net/netchan.h"

#include <cstring>
#include <string>

#include "iw_pc/net/msg.h"

namespace iw_pc::net {

namespace {

void put_u32(Packet& packet, std::uint32_t v) {
    const std::uint8_t b[4] = {static_cast<std::uint8_t>(v), static_cast<std::uint8_t>(v >> 8),
                               static_cast<std::uint8_t>(v >> 16), static_cast<std::uint8_t>(v >> 24)};
    packet.put(b);
}

void put_u16(Packet& packet, std::uint16_t v) {
    const std::uint8_t b[2] = {static_cast<std::uint8_t>(v), static_cast<std::uint8_t>(v >> 8)};
    packet.put(b);
}

void copy_into(ByteSpan data, std::size_t at, ByteView bytes) {
    if (at >= data.size()) return;
    std::size_t n = std::min(bytes.size(), data.size() - at);
    if (n > 0) std::memmove(data.data() + at, bytes.data(), n);
}

void start_reading(Msg& msg, bool cod2) {
    if (cod2) {
        msg.begin_reading();
    } else {
        msg.readcount = 0;
        msg.bit = 0;
    }
}

}  // namespace

void Packet::put(ByteView bytes) {
    std::size_t end = len_ + bytes.size();
    if (end <= kMaxPacketSize) {
        if (!bytes.empty()) std::memcpy(data_.data() + len_, bytes.data(), bytes.size());
        len_ = end;
    }
}

Netchan::Netchan(Game game, NetSource source, NetAddress remote_address, std::int32_t qport)
    : game(game),
      source(source),
      remote_address(remote_address),
      qport(qport),
      fragment_buffer(max_message_length(game), 0),
      unsent_buffer(max_message_length(game), 0) {}

void Netchan::setup(NetSource new_source, NetAddress new_address, std::int32_t new_qport) {
    source = new_source;
    remote_address = new_address;
    qport = new_qport;
    outgoing_sequence = 1;
    incoming_sequence = 0;
    dropped = 0;
    fragment_sequence = 0;
    fragment_length = 0;
    std::fill(fragment_buffer.begin(), fragment_buffer.end(), std::uint8_t{0});
    unsent_fragments = false;
    unsent_fragment_start = 0;
    unsent_length = 0;
    std::fill(unsent_buffer.begin(), unsent_buffer.end(), std::uint8_t{0});
}

Status Netchan::transmit(ByteView data, Packet& packet) {
    std::size_t length = data.size();
    if (length > max_message_length(game)) {
        return GameError::drop("Netchan_Transmit: length = " + std::to_string(length));
    }
    unsent_fragment_start = 0;
    if (length >= kFragmentSize) {
        unsent_fragments = true;
        unsent_length = length;
        std::memcpy(unsent_buffer.data(), data.data(), length);
        transmit_next_fragment(packet);
        return std::nullopt;
    }
    packet.clear();
    put_u32(packet, static_cast<std::uint32_t>(outgoing_sequence));
    outgoing_sequence = static_cast<std::int32_t>(static_cast<std::uint32_t>(outgoing_sequence) + 1);
    if (source == NetSource::Client) put_u16(packet, static_cast<std::uint16_t>(qport));
    packet.put(data);
    return std::nullopt;
}

void Netchan::transmit_next_fragment(Packet& packet) {
    packet.clear();
    put_u32(packet, static_cast<std::uint32_t>(outgoing_sequence) | kFragmentBit);
    if (source == NetSource::Client) put_u16(packet, static_cast<std::uint16_t>(qport));
    std::size_t start = unsent_fragment_start;
    std::size_t length = std::min(kFragmentSize, unsent_length > start ? unsent_length - start : 0);
    if (game == Game::CallOfDuty2) put_u32(packet, static_cast<std::uint32_t>(start));
    else put_u16(packet, static_cast<std::uint16_t>(start));
    put_u16(packet, static_cast<std::uint16_t>(length));
    packet.put(ByteView(unsent_buffer).subspan(start, length));
    unsent_fragment_start = start + length;
    if (unsent_fragment_start == unsent_length && length != kFragmentSize) {
        outgoing_sequence = static_cast<std::int32_t>(static_cast<std::uint32_t>(outgoing_sequence) + 1);
        unsent_fragments = false;
    }
}

bool Netchan::process(Msg& msg) {
    bool cod2 = game == Game::CallOfDuty2;
    start_reading(msg, cod2);
    std::int32_t sequence = msg.read_long();
    bool fragmented = sequence < 0;
    if (fragmented) sequence &= 0x7FFFFFFF;
    if (source == NetSource::Server) msg.read_short();  // the qport, read by the caller before
    std::int32_t fragment_start = 0, this_length = 0;
    if (fragmented) {
        fragment_start = cod2 ? msg.read_long() : msg.read_short();
        this_length = msg.read_short();
    }
    if (sequence <= incoming_sequence) return false;  // out of order or a duplicate
    dropped = static_cast<std::int32_t>(static_cast<std::uint32_t>(sequence) - static_cast<std::uint32_t>(incoming_sequence) - 1);
    if (!fragmented) {
        incoming_sequence = sequence;
        return true;
    }
    if (sequence != fragment_sequence) {
        fragment_sequence = sequence;
        fragment_length = 0;
    }
    if (fragment_start != static_cast<std::int32_t>(fragment_length)) return false;  // a fragment went missing
    if (this_length < 0) return false;
    auto length = static_cast<std::size_t>(this_length);
    if (msg.readcount + length > msg.cursize || fragment_length + length > max_message_length(game)) {
        return false;  // an illegal fragment length
    }
    std::size_t from = msg.readcount;
    for (std::size_t i = 0; i < length; ++i) {
        fragment_buffer[fragment_length + i] = from + i < msg.data.size() ? msg.data[from + i] : 0;
    }
    fragment_length += length;
    if (length == kFragmentSize) return false;  // more to come
    if (fragment_length > msg.maxsize) return false;
    // The whole message: its sequence number, then the fragments' bytes.
    std::size_t total = fragment_length;
    auto s = static_cast<std::uint32_t>(sequence);
    const std::uint8_t head[4] = {static_cast<std::uint8_t>(s), static_cast<std::uint8_t>(s >> 8),
                                  static_cast<std::uint8_t>(s >> 16), static_cast<std::uint8_t>(s >> 24)};
    copy_into(msg.data, 0, head);
    copy_into(msg.data, 4, ByteView(fragment_buffer).first(total));
    msg.cursize = total + 4;
    fragment_length = 0;
    start_reading(msg, cod2);
    msg.read_long();  // past the sequence number
    if (cod2) incoming_sequence = sequence;
    return true;
}

Expected<SentMessage> send_server_message(Netchan& chan, std::int32_t challenge, ByteView last_client_command,
                                          std::array<std::uint8_t, 4> message_head, ByteView compressed_body,
                                          ByteSpan scratch, Packet& packet) {
    std::size_t size = 4 + compressed_body.size();
    std::memcpy(scratch.data(), message_head.data(), 4);
    if (!compressed_body.empty()) std::memcpy(scratch.data() + 4, compressed_body.data(), compressed_body.size());
    auto frame = static_cast<std::size_t>(chan.outgoing_sequence & 31);
    std::uint8_t key = server_encode_key(challenge, chan.outgoing_sequence);
    scramble(scratch.subspan(4, size - 4), key, last_client_command);
    if (auto error = chan.transmit(scratch.first(size), packet)) return *error;
    return SentMessage{size, frame};
}

void scramble(ByteSpan data, std::uint8_t key, ByteView text_arg) {
    std::size_t text_len = 0;
    while (text_len < text_arg.size() && text_arg[text_len] != 0) ++text_len;
    ByteView text = text_arg.first(text_len);
    std::size_t index = 0;
    for (std::size_t i = 0; i < data.size(); ++i) {
        if (index >= text.size()) index = 0;
        std::uint8_t c = index < text.size() ? text[index] : 0;
        key = static_cast<std::uint8_t>(key ^ (c << (i & 1)));
        data[i] ^= key;
        ++index;
    }
}

}  // namespace iw_pc::net
