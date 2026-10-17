// The connection layer, the games' netchan_t: every packet starts with a
// header (sequence number, the client's qport, fragment information);
// messages of 1300 bytes or more go out in fragments and are put back
// together on arrival; late or repeated packets are dropped. Inside, the
// message body is Huffman-coded and scrambled with a running XOR key
// (scramble()). docs/PC_MULTIPLAYER.md section 5.
//
//   sequence         32 bits   always; the top bit set means "fragment"
//   qport            16 bits   client to server only
//   fragment start   32 bits in Call of Duty 2, 16 in United Offensive (fragments only)
//   fragment length  16 bits   fragments only
//   payload          the rest
//
// Differences between the games: the fragment start's size; the largest
// message (128 KiB / 32 KiB); United Offensive doesn't update
// incoming_sequence when a fragmented message is complete (Quake III 1.17's
// behavior); United Offensive's reads past a packet's end don't mark it.
#pragma once

#include <array>
#include <vector>

#include "iw_pc/base.h"

namespace iw_pc::net {

class Msg;

/// A message this long or longer goes out in pieces of this size.
inline constexpr std::size_t kFragmentSize = 1300;
/// The most a packet holds, header included (MAX_PACKETLEN).
inline constexpr std::size_t kMaxPacketSize = 1400;
/// The top bit of the sequence number: this packet is a fragment.
inline constexpr std::uint32_t kFragmentBit = 0x80000000u;

/// MAX_MSGLEN: 128 KiB in Call of Duty 2, 32 KiB in United Offensive.
inline std::size_t max_message_length(Game game) { return game == Game::CallOfDuty2 ? 0x20000 : 0x8000; }

/// How many reliable commands each side keeps: 128 (CoD2), 64 (UO).
inline std::size_t max_reliable_commands(Game game) { return game == Game::CallOfDuty2 ? 128 : 64; }

/// netadrtype_t (the same numbers in both games and Quake III).
enum class AddressType : std::uint32_t { Bot = 0, Bad = 1, Loopback = 2, Broadcast = 3, Ip = 4, Ipx = 5, BroadcastIpx = 6 };

/// A network address, the games' 20-byte netadr_t. The port is a number
/// (the games store it in network byte order).
struct NetAddress {
    AddressType kind = AddressType::Bad;
    std::array<std::uint8_t, 4> ip{};
    std::uint16_t port = 0;
    std::array<std::uint8_t, 10> ipx{};

    static NetAddress from_ip(std::array<std::uint8_t, 4> ip, std::uint16_t port) {
        NetAddress a;
        a.kind = AddressType::Ip;
        a.ip = ip;
        a.port = port;
        return a;
    }
    bool operator==(const NetAddress&) const = default;
};

/// netsrc_t: only a client sends a qport, only a server reads one.
enum class NetSource { Client = 0, Server = 1 };

/// One packet to send, built in place (at most kMaxPacketSize bytes).
class Packet {
public:
    ByteView bytes() const { return ByteView(data_).first(len_); }
    void clear() { len_ = 0; }
    /// Appends `bytes` if they fit (as the games' fixed send buffer does).
    void put(ByteView bytes);

private:
    std::array<std::uint8_t, kMaxPacketSize> data_{};
    std::size_t len_ = 0;
};

/// One connection's state, the games' netchan_t.
class Netchan {
public:
    Game game;
    NetSource source;
    NetAddress remote_address;
    std::int32_t qport;                  // 16 bits go on the wire
    std::int32_t outgoing_sequence = 1;  // the next message sent
    std::int32_t incoming_sequence = 0;  // the last message received in full
    std::int32_t dropped = 0;            // packets missing before the last one received
    std::int32_t fragment_sequence = 0;  // the fragmented message being put together
    std::size_t fragment_length = 0;
    std::vector<std::uint8_t> fragment_buffer;  // max_message_length() long
    bool unsent_fragments = false;
    std::size_t unsent_fragment_start = 0;
    std::size_t unsent_length = 0;
    std::vector<std::uint8_t> unsent_buffer;  // max_message_length() long

    /// A channel as setup() leaves it (allocates its two buffers once).
    Netchan(Game game, NetSource source, NetAddress remote_address, std::int32_t qport);

    /// Starts the channel over: everything zeroed (the buffers too), the
    /// source, address and qport set, the outgoing sequence at 1.
    /// Original: Netchan_Setup at 0x00447D50 (CoD2MP_s.exe 1.3)
    /// Original: Netchan_Setup at 0x0044D530 (CoDUOMP.exe 1.51)
    void setup(NetSource source, NetAddress remote_address, std::int32_t qport);

    /// Sends a message: builds the first packet in `packet`. A message of
    /// kFragmentSize or more is copied into the channel and its first
    /// fragment goes out. Longer than max_message_length(): the game's error.
    /// Original: Netchan_Transmit at 0x00447FB0 (CoD2MP_s.exe 1.3)
    /// Original: Netchan_Transmit at 0x0044D790 (CoDUOMP.exe 1.51)
    Status transmit(ByteView data, Packet& packet);

    /// The next fragment of the message being sent.
    /// Original: Netchan_TransmitNextFragment at 0x00447DB0 (CoD2MP_s.exe 1.3)
    /// Original: Netchan_TransmitNextFragment at 0x0044D590 (CoDUOMP.exe 1.51)
    void transmit_next_fragment(Packet& packet);

    /// Reads a packet of this connection (msg holds it as it arrived). False
    /// for a packet to ignore; true when msg holds a whole message,
    /// positioned after its header (a reassembled one after a 4-byte
    /// sequence number).
    /// Original: Netchan_Process at 0x004481B0 (CoD2MP_s.exe 1.3)
    /// Original: Netchan_Process at 0x0044D970 (CoDUOMP.exe 1.51)
    bool process(Msg& msg);
};

/// What send_server_message() did.
struct SentMessage {
    std::size_t size;   // the bytes handed to the channel (4 + the coded body)
    std::size_t frame;  // outgoingSequence & 31, before sending
};

/// Sends one message from a server to a client as SV_SendMessageToClient
/// does: the 4 raw head bytes, the Huffman-coded body scrambled with
/// server_encode_key() and the client's last command, then transmit().
/// `scratch` holds at least 4 + compressed_body.size() bytes.
/// Original: SV_SendMessageToClient at 0x0045F370 (CoD2MP_s.exe 1.3)
/// Original: SV_SendMessageToClient at 0x00465DA0 (CoDUOMP.exe 1.51)
Expected<SentMessage> send_server_message(Netchan& chan, std::int32_t challenge, ByteView last_client_command,
                                          std::array<std::uint8_t, 4> message_head, ByteView compressed_body,
                                          ByteSpan scratch, Packet& packet);

/// Scrambles (or unscrambles: it is its own inverse) the coded part of a
/// message: for each byte i the key is XORed with the next character of
/// `text` shifted left by i & 1, and the byte with the key; `text` restarts
/// at its end (or a zero byte).
/// Original: SV_Netchan_Encode at 0x0045C8C0 (CoD2MP_s.exe 1.3)
/// Original: SV_Netchan_Decode at 0x0045C920 (CoD2MP_s.exe 1.3)
/// Original: CL_Netchan_Encode at 0x004127F0 (CoD2MP_s.exe 1.3)
/// Original: CL_Netchan_Decode at 0x00412860 (CoD2MP_s.exe 1.3)
/// Original: SV_Netchan_Encode at 0x00463640 (CoDUOMP.exe 1.51)
/// Original: SV_Netchan_Decode at 0x004636A0 (CoDUOMP.exe 1.51)
/// Original: CL_Netchan_Encode at 0x004173F0 (CoDUOMP.exe 1.51)
/// Original: CL_Netchan_Decode at 0x00417460 (CoDUOMP.exe 1.51)
void scramble(ByteSpan data, std::uint8_t key, ByteView text);

/// SV_Netchan_Encode's key: challenge ^ outgoing sequence (low bytes).
inline std::uint8_t server_encode_key(std::int32_t challenge, std::int32_t outgoing_sequence) {
    return static_cast<std::uint8_t>(challenge ^ outgoing_sequence);
}
/// SV_Netchan_Decode's key: challenge ^ server id ^ message acknowledge.
inline std::uint8_t server_decode_key(std::int32_t challenge, std::int32_t server_id, std::int32_t message_acknowledge) {
    return static_cast<std::uint8_t>(challenge ^ server_id ^ message_acknowledge);
}
/// CL_Netchan_Encode's key: challenge ^ server id ^ server message sequence.
inline std::uint8_t client_encode_key(std::int32_t challenge, std::int32_t server_id, std::int32_t server_message_sequence) {
    return static_cast<std::uint8_t>(challenge ^ server_id ^ server_message_sequence);
}
/// CL_Netchan_Decode's key: challenge ^ server message sequence.
inline std::uint8_t client_decode_key(std::int32_t challenge, std::int32_t server_message_sequence) {
    return static_cast<std::uint8_t>(challenge ^ server_message_sequence);
}

}  // namespace iw_pc::net
