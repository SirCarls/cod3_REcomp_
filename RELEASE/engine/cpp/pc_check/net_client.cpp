// Requests for iw_pc::client (the C++ version): the client's parsers, with
// the client's globals as the games keep them. The client's cl and the part
// of clc the parsers use arrive as regions (g:), read into a ClientState
// with the layout below and written back the same way (the same table is in
// tools/decomp/x86/specs/net_client.py and engine/tools/pc_check's
// net_client.rs). Bytes the port doesn't keep are left as they came.

#include <algorithm>
#include <array>
#include <memory>
#include <bit>
#include <cctype>
#include <cstring>
#include <initializer_list>

#include "iw_pc/client/client.h"
#include "iw_pc/client/connect.h"
#include "iw_pc/client/download.h"
#include "iw_pc/net/delta.h"
#include "iw_pc/net/huffman.h"
#include "iw_pc/net/oob.h"
#include "msg_arg.h"
#include "net_common.h"

namespace pc_check {

using iw_pc::ByteSpan;
using iw_pc::ByteView;
using iw_pc::Game;
namespace client = iw_pc::client;
namespace net = iw_pc::net;

namespace {

// Where the client's state is in one game's memory.
struct Layout {
    std::size_t cl, snap, old_frame, new_snapshots, gs_offsets, gs_data, gs_count, pen, pcn, out_packets, snapshots,
        snap_size, baselines, parse_entities, parse_clients;
    std::size_t clc, client_num, connect_count, checksum_feed, msg_seq, cmd_seq, commands, demo_waiting, outgoing;
    // The sending side.
    std::size_t server_id, cmds, cmd_number, challenge, reliable_seq, reliable_ack, reliable_commands, last_sent,
        demo_playing;
    // CL_PacketEvent.
    std::size_t last_packet_time, demo_recording;
    // CL_ConnectionlessPacket (only_visible 0: none).
    std::size_t server_address, connect_time, server_message, only_visible;
    // The client game's clock (time_demo 0: none).
    std::size_t server_time, old_server_time, frame_server_time, server_time_delta, extrapolated,
        first_demo_frame_skipped, time_demo, time_demo_base;
    // The player's commands, CL_FinishMove (user_cmd_offhand 0: none).
    std::size_t user_cmd_weapon, user_cmd_offhand, cgame_angles, view_angles;
};

constexpr Layout kCod2{0x0096B650, 0x0096B668, 0x0096DD50, 0x0096DD58, 0x0096DD5C, 0x0096FD5C, 0x0098FD5C, 0x0098FDA0,
                       0x0098FDA4, 0x009D0C30, 0x009D0DB0, 0x26D8,     0x00A1E8B0, 0x00A5A8B0, 0x00AD28B0, 0x00609FE8,
                       0x00609FE8, 0x0060A00C, 0x0060A114, 0x0062A120, 0x0062A124, 0x0062A12C, 0x0064A178, 0x0064A198,
                       0x0098FDF8, 0x009CFE2C, 0x009D0C2C, 0x0060A110, 0x0060A118, 0x0060A11C, 0x0060A120, 0x00609FEC,
                       0x0064A170, 0x00609FF0, 0x0064A16C, 0x00609FF4, 0x0060A008, 0x0060A010, 0,
                       0x0096DD40, 0x0096DD44, 0x0096DD48, 0x0096DD4C, 0x0096DD54, 0x0064A17C, 0x0064A174, 0x0064A194,
                       0x0098FDC8, 0x0098FDCC, 0x0098FDE0, 0x0098FDEC};
constexpr Layout kUo{0x049581A0, 0x049581A4, 0x0495C6E8, 0x0495C6F0, 0x0495C6F4, 0x0495E6F4, 0x049636F4, 0x04963738,
                     0x0496373C, 0x049643D0, 0x04964550, 0x4534,     0x049EEBD0, 0x04A2BBD0, 0x04AA5BD0, 0x04DC8860,
                     0x04DC8860, 0x04DC8884, 0x04DC898C, 0x04DD899C, 0x04DD89A0, 0x04DD89A8, 0x04DE9610, 0x04DE967C,
                     0x049637A8, 0x049637CC, 0x049643CC, 0x04DC8988, 0x04DC8994, 0x04DC8998, 0x04DC899C, 0x04DC8864,
                     0x04DE960C, 0x04DC8868, 0x04DE9608, 0x04DC886C, 0x04DC8880, 0x04DC8888, 0x04DC8990,
                     0x0495C6D8, 0x0495C6DC, 0x0495C6E0, 0x0495C6E4, 0x0495C6EC, 0x04DE9614, 0,          0x04DE962C,
                     0x0496376C, 0,          0x04963790, 0x0496379C};

// cl.timeoutcount (CL_CheckTimeout), outside the table above.
std::size_t timeout_count_address(Game game) { return game == Game::CallOfDuty2 ? 0x0096B664 : 0x049581A0; }
// clc.lastExecutedServerCommand (CL_GetServerCommand), outside the table above.
std::size_t last_executed_address(Game game) { return game == Game::CallOfDuty2 ? 0x0062A128 : 0x04DD89A4; }

const Layout& layout(Game game) { return game == Game::CallOfDuty2 ? kCod2 : kUo; }

// A block of the game's memory starting at `base`, read and written by address.
struct Block {
    Bytes& bytes;
    std::size_t base;

    bool holds(std::size_t address, std::size_t len) const {
        return address >= base && address - base + len <= bytes.size();
    }
    std::size_t at(std::size_t address, std::size_t len) const {
        if (address < base || address - base + len > bytes.size()) throw std::runtime_error("address outside the region");
        return address - base;
    }
    std::int32_t get(std::size_t address) const { return net::word_at(ByteView(bytes).subspan(at(address, 4), 4), 0); }
    void put(std::size_t address, std::int32_t value) { net::set_word(ByteSpan(bytes).subspan(at(address, 4), 4), 0, value); }
    void read(std::size_t address, std::vector<std::uint8_t>& to) const {
        std::size_t a = at(address, to.size());
        std::memcpy(to.data(), bytes.data() + a, to.size());
    }
    void write(std::size_t address, const std::vector<std::uint8_t>& from) {
        std::size_t a = at(address, from.size());
        std::memcpy(bytes.data() + a, from.data(), from.size());
    }
};

// A netadr_t as the game keeps it, in the canonical layout (Call of Duty 2's: type, ip, port,
// ipx; United Offensive keeps the port last), and back.
std::vector<std::uint8_t> canonical_address(Game game, const std::vector<std::uint8_t>& guest) {
    if (game == Game::CallOfDuty2) return guest;
    std::vector<std::uint8_t> out(guest.begin(), guest.begin() + 8);
    out.insert(out.end(), guest.begin() + 18, guest.begin() + 20);
    out.insert(out.end(), guest.begin() + 8, guest.begin() + 18);
    return out;
}

std::vector<std::uint8_t> guest_address(Game game, const std::vector<std::uint8_t>& canonical) {
    if (game == Game::CallOfDuty2) return canonical;
    std::vector<std::uint8_t> out(canonical.begin(), canonical.begin() + 8);
    out.insert(out.end(), canonical.begin() + 10, canonical.begin() + 20);
    out.insert(out.end(), canonical.begin() + 8, canonical.begin() + 10);
    return out;
}

// A snapshot from its bytes (clSnapshot_t: seven numbers, the player state, five numbers).
client::Snapshot snapshot_from(Game game, ByteView bytes) {
    client::Snapshot s(game);
    std::size_t ps = s.player_state.size();
    if (bytes.size() < 0x30 + ps) throw std::runtime_error("short snapshot");
    auto word = [&](std::size_t at) { return net::word_at(bytes, static_cast<std::uint32_t>(at)); };
    s.valid = word(0);
    s.snap_flags = word(4);
    s.server_time = word(8);
    s.message_num = word(12);
    s.delta_num = word(16);
    s.ping = word(20);
    s.cmd_num = word(24);
    std::memcpy(s.player_state.data(), bytes.data() + 28, ps);
    std::size_t tail = 28 + ps;
    s.num_entities = word(tail);
    s.num_clients = word(tail + 4);
    s.parse_entities_num = word(tail + 8);
    s.parse_clients_num = word(tail + 12);
    s.server_command_num = word(tail + 16);
    return s;
}

void snapshot_into(const client::Snapshot& s, ByteSpan bytes) {
    std::size_t ps = s.player_state.size();
    auto put = [&](std::size_t at, std::int32_t v) { net::set_word(bytes, static_cast<std::uint32_t>(at), v); };
    std::int32_t head[] = {s.valid, s.snap_flags, s.server_time, s.message_num, s.delta_num, s.ping, s.cmd_num};
    for (std::size_t k = 0; k < 7; ++k) put(4 * k, head[k]);
    std::memcpy(bytes.data() + 28, s.player_state.data(), ps);
    std::int32_t tail[] = {s.num_entities, s.num_clients, s.parse_entities_num, s.parse_clients_num, s.server_command_num};
    for (std::size_t k = 0; k < 5; ++k) put(28 + ps + 4 * k, tail[k]);
}

void read_cl(client::ClientState& cl, Bytes& bytes) {
    const Layout& lay = layout(cl.game);
    Block b{bytes, lay.cl};
    auto snapshot_at = [&](std::size_t address) {
        return snapshot_from(cl.game, ByteView(bytes).subspan(b.at(address, lay.snap_size), lay.snap_size));
    };
    cl.snap = snapshot_at(lay.snap);
    cl.timeout_count = b.get(timeout_count_address(cl.game));
    cl.old_frame_server_time = b.get(lay.old_frame);
    cl.new_snapshots = b.get(lay.new_snapshots);
    cl.server_time = b.get(lay.server_time);
    cl.old_server_time = b.get(lay.old_server_time);
    cl.frame_server_time = b.get(lay.frame_server_time);
    cl.server_time_delta = b.get(lay.server_time_delta);
    cl.extrapolated_snapshot = b.get(lay.extrapolated);
    for (std::size_t i = 0; i < cl.game_state.offsets.size(); ++i) cl.game_state.offsets[i] = b.get(lay.gs_offsets + 4 * i);
    b.read(lay.gs_data, cl.game_state.data);
    cl.game_state.data_count = b.get(lay.gs_count);
    cl.parse_entities_num = b.get(lay.pen);
    cl.parse_clients_num = b.get(lay.pcn);
    for (std::size_t i = 0; i < cl.out_packets.size(); ++i) {
        std::size_t at = lay.out_packets + 12 * i;
        cl.out_packets[i] = {b.get(at), b.get(at + 4), b.get(at + 8)};
    }
    for (std::size_t i = 0; i < client::kPacketBackup; ++i) cl.snapshots[i] = snapshot_at(lay.snapshots + i * lay.snap_size);
    b.read(lay.baselines, cl.baselines);
    b.read(lay.parse_entities, cl.parse_entities);
    b.read(lay.parse_clients, cl.parse_clients);
    cl.server_id = b.get(lay.server_id);
    b.read(lay.cmds, cl.cmds);
    cl.cmd_number = b.get(lay.cmd_number);
    cl.user_cmd_weapon = b.get(lay.user_cmd_weapon);
    if (lay.user_cmd_offhand) cl.user_cmd_offhand = b.get(lay.user_cmd_offhand);
    for (std::size_t i = 0; i < 3; ++i) {
        cl.cgame_angles[i] = std::bit_cast<float>(b.get(lay.cgame_angles + 4 * i));
        cl.view_angles[i] = std::bit_cast<float>(b.get(lay.view_angles + 4 * i));
    }
}

void write_cl(const client::ClientState& cl, Bytes& bytes) {
    const Layout& lay = layout(cl.game);
    Block b{bytes, lay.cl};
    auto snapshot_at = [&](std::size_t address, const client::Snapshot& s) {
        snapshot_into(s, ByteSpan(bytes).subspan(b.at(address, lay.snap_size), lay.snap_size));
    };
    snapshot_at(lay.snap, cl.snap);
    b.put(timeout_count_address(cl.game), cl.timeout_count);
    b.put(lay.old_frame, cl.old_frame_server_time);
    b.put(lay.new_snapshots, cl.new_snapshots);
    b.put(lay.server_time, cl.server_time);
    b.put(lay.old_server_time, cl.old_server_time);
    b.put(lay.frame_server_time, cl.frame_server_time);
    b.put(lay.server_time_delta, cl.server_time_delta);
    b.put(lay.extrapolated, cl.extrapolated_snapshot);
    for (std::size_t i = 0; i < cl.game_state.offsets.size(); ++i) b.put(lay.gs_offsets + 4 * i, cl.game_state.offsets[i]);
    b.write(lay.gs_data, cl.game_state.data);
    b.put(lay.gs_count, cl.game_state.data_count);
    b.put(lay.pen, cl.parse_entities_num);
    b.put(lay.pcn, cl.parse_clients_num);
    for (std::size_t i = 0; i < cl.out_packets.size(); ++i) {
        std::size_t at = lay.out_packets + 12 * i;
        b.put(at, cl.out_packets[i].cmd_number);
        b.put(at + 4, cl.out_packets[i].server_time);
        b.put(at + 8, cl.out_packets[i].realtime);
    }
    for (std::size_t i = 0; i < client::kPacketBackup; ++i) snapshot_at(lay.snapshots + i * lay.snap_size, cl.snapshots[i]);
    b.write(lay.baselines, cl.baselines);
    b.write(lay.parse_entities, cl.parse_entities);
    b.write(lay.parse_clients, cl.parse_clients);
    b.put(lay.server_id, cl.server_id);
    b.write(lay.cmds, cl.cmds);
    b.put(lay.cmd_number, cl.cmd_number);
    b.put(lay.user_cmd_weapon, cl.user_cmd_weapon);
    if (lay.user_cmd_offhand) b.put(lay.user_cmd_offhand, cl.user_cmd_offhand);
    for (std::size_t i = 0; i < 3; ++i) {
        b.put(lay.cgame_angles + 4 * i, std::bit_cast<std::int32_t>(cl.cgame_angles[i]));
        b.put(lay.view_angles + 4 * i, std::bit_cast<std::int32_t>(cl.view_angles[i]));
    }
}

void read_clc(client::ClientState& cl, Bytes& bytes) {
    const Layout& lay = layout(cl.game);
    Block b{bytes, lay.clc};
    cl.client_num = b.get(lay.client_num);
    cl.connect_packet_count = b.get(lay.connect_count);
    cl.checksum_feed = b.get(lay.checksum_feed);
    cl.server_message_sequence = b.get(lay.msg_seq);
    cl.server_command_sequence = b.get(lay.cmd_seq);
    b.read(lay.commands, cl.server_commands);
    cl.demo_waiting = b.get(lay.demo_waiting);
    // (The parsers' region ends with the channel's outgoing sequence; the
    // sender's stops before the channel, which comes whole.)
    if (b.holds(lay.outgoing, 4)) cl.chan.outgoing_sequence = b.get(lay.outgoing);
    cl.challenge = b.get(lay.challenge);
    cl.reliable_sequence = b.get(lay.reliable_seq);
    cl.reliable_acknowledge = b.get(lay.reliable_ack);
    b.read(lay.reliable_commands, cl.reliable_commands);
    cl.last_packet_sent_time = b.get(lay.last_sent);
    cl.demo_playing = b.get(lay.demo_playing);
    cl.last_executed_server_command = b.get(last_executed_address(cl.game));
    cl.last_packet_time = b.get(lay.last_packet_time);
    cl.demo_recording = b.get(lay.demo_recording);
    std::vector<std::uint8_t> address(kAddressSize);
    b.read(lay.server_address, address);
    std::string error;
    auto server = address_from(canonical_address(cl.game, address), error);
    if (!server) throw std::runtime_error(error);
    cl.server_address = *server;
    cl.connect_time = b.get(lay.connect_time);
    b.read(lay.server_message, cl.server_message);
    if (lay.only_visible) cl.only_visible_clients = b.get(lay.only_visible);
    cl.first_demo_frame_skipped = b.get(lay.first_demo_frame_skipped);
    if (lay.time_demo) cl.time_demo = b.get(lay.time_demo);
    cl.time_demo_base_time = b.get(lay.time_demo_base);
}

void write_clc(const client::ClientState& cl, Bytes& bytes) {
    const Layout& lay = layout(cl.game);
    Block b{bytes, lay.clc};
    b.put(lay.client_num, cl.client_num);
    b.put(lay.connect_count, cl.connect_packet_count);
    b.put(lay.checksum_feed, cl.checksum_feed);
    b.put(lay.msg_seq, cl.server_message_sequence);
    b.put(lay.cmd_seq, cl.server_command_sequence);
    b.write(lay.commands, cl.server_commands);
    b.put(lay.demo_waiting, cl.demo_waiting);
    if (b.holds(lay.outgoing, 4)) b.put(lay.outgoing, cl.chan.outgoing_sequence);
    b.put(lay.challenge, cl.challenge);
    b.put(lay.reliable_seq, cl.reliable_sequence);
    b.put(lay.reliable_ack, cl.reliable_acknowledge);
    b.write(lay.reliable_commands, cl.reliable_commands);
    b.put(lay.last_sent, cl.last_packet_sent_time);
    b.put(lay.demo_playing, cl.demo_playing);
    b.put(last_executed_address(cl.game), cl.last_executed_server_command);
    b.put(lay.last_packet_time, cl.last_packet_time);
    b.put(lay.demo_recording, cl.demo_recording);
    auto canonical = address_bytes(cl.server_address);
    b.write(lay.server_address, guest_address(cl.game, std::vector<std::uint8_t>(canonical.begin(), canonical.end())));
    b.put(lay.connect_time, cl.connect_time);
    b.write(lay.server_message, cl.server_message);
    if (lay.only_visible) b.put(lay.only_visible, cl.only_visible_clients);
    b.put(lay.first_demo_frame_skipped, cl.first_demo_frame_skipped);
    if (lay.time_demo) b.put(lay.time_demo, cl.time_demo);
    b.put(lay.time_demo_base, cl.time_demo_base_time);
}

// Which of the client's regions a request carries (argument indexes; -1 none).
struct Regions {
    int cl = -1, clc = -1;
};

client::ClientState load(Game game, Args& args, Regions r) {
    client::ClientState cl(game);
    if (r.cl >= 0) read_cl(cl, args.region(static_cast<std::size_t>(r.cl)));
    if (r.clc >= 0) read_clc(cl, args.region(static_cast<std::size_t>(r.clc)));
    return cl;
}

void store(const client::ClientState& cl, Args& args, Regions r) {
    if (r.cl >= 0) write_cl(cl, args.region(static_cast<std::size_t>(r.cl)));
    if (r.clc >= 0) write_clc(cl, args.region(static_cast<std::size_t>(r.clc)));
}

// cls.realtime: a 4-byte big-endian buffer argument.
std::int32_t realtime(Args& args, std::size_t i) {
    Bytes* b = args.buffer(i);
    if (!b || b->size() != 4) throw std::runtime_error("realtime is not 4 bytes");
    return be_word(*b, 0);
}

Answer label(const char* stopped) { return string(ByteView(reinterpret_cast<const std::uint8_t*>(stopped), std::strlen(stopped))); }

// The originals' stop points after a game state and at a download: the checks end the call there.
class StopAt : public client::ServerMessageParts {
public:
    const char* stopped = "return";
    bool game_state_parsed(client::ClientState&) override {
        stopped = "systeminfo";
        return false;
    }
    bool download(net::Msg&, client::ClientState&) override {
        stopped = "download";
        return false;
    }
};

// CL_ParseServerMessage: the plain message in argument 0 (v:, header words then data), coded
// here as the guest's is coded by the original's MSG_WriteBitsCompress.
Answer server_message(Game game, Args& args) {
    std::string error;
    const auto* tree = message_tree_of(game, error);
    if (!tree) return Answer::fail(error);
    const auto* tables = field_tables(game, error);
    if (!tables) return Answer::fail(error);
    Bytes token(args.bytes(0).begin(), args.bytes(0).end());
    if (token.size() < kMsgHeader) return Answer::fail("short msg token");
    std::int32_t cursize = be_word(token, 2), readcount = be_word(token, 3);
    if (readcount < 0 || cursize < readcount || kMsgHeader + static_cast<std::size_t>(cursize) > token.size())
        return Answer::fail("msg sizes past its data");
    ByteView data = ByteView(token).subspan(kMsgHeader);
    ByteView plain = data.subspan(static_cast<std::size_t>(readcount), static_cast<std::size_t>(cursize - readcount));
    Bytes coded(4 * plain.size() + 64, 0);
    std::size_t length = tree->write_bits_compress(plain, coded);
    Bytes buffer(data.begin(), data.begin() + readcount);
    buffer.insert(buffer.end(), coded.begin(), coded.begin() + static_cast<std::ptrdiff_t>(length));
    std::size_t size = buffer.size();
    buffer.resize(size + 64, 0);
    Regions regions{1, 2};
    client::ClientState cl = load(game, args, regions);
    cl.realtime = realtime(args, 3);
    net::Msg msg(game, buffer);
    msg.maxsize = size + 16;
    msg.cursize = size;
    msg.readcount = static_cast<std::size_t>(readcount);
    StopAt parts;
    if (auto parsed = client::parse_server_message(msg, *tree, *tables, cl, parts); !parsed) return game_error(parsed.error());
    store(cl, args, regions);
    return label(parts.stopped);
}

// CL_ParsePacketEntities and CL_ParsePacketClients: (msg, old snapshot index or -1, new snapshot bytes, cl).
Answer packet(Game game, Args& args, bool clients) {
    std::string error;
    const auto* tables = field_tables(game, error);
    if (!tables) return Answer::fail(error);
    Regions regions{3, -1};
    client::ClientState cl = load(game, args, regions);
    std::int32_t index = args.i32(1);
    if (index >= static_cast<std::int32_t>(client::kPacketBackup)) return Answer::fail("old snapshot index past 31");
    std::optional<std::size_t> old;
    if (index >= 0) old = static_cast<std::size_t>(index);
    Bytes* new_bytes = args.buffer(2);
    if (!new_bytes) return Answer::fail("NULL new snapshot");
    client::Snapshot snapshot = snapshot_from(game, *new_bytes);
    iw_pc::Status status;
    std::string msg_error = with_msg(args, 0, game, [&](net::Msg& m) {
        status = clients ? client::parse_packet_clients(m, *tables, cl, old, snapshot)
                         : client::parse_packet_entities(m, *tables, cl, old, snapshot);
    });
    if (!msg_error.empty()) return Answer::fail(msg_error);
    if (status) return game_error(*status);
    snapshot_into(snapshot, *args.buffer(2));
    store(cl, args, regions);
    return nothing();
}

// CL_WritePacket: (cls.state, cl, clc before the channel, the channel, realtime, cl_packetdup,
// cl_nodelta, the packets sent).
Answer write_packet(Game game, Args& args) {
    std::string error;
    const auto* tree = message_tree_of(game, error);
    if (!tree) return Answer::fail(error);
    std::int32_t state = realtime(args, 0);
    Regions regions{1, 2};
    client::ClientState cl = load(game, args, regions);
    cl.connection_state = state;
    std::int32_t profile = 0;
    if (!chan_from_arg(game, args, 3, cl.chan, profile, error)) return Answer::fail(error);
    cl.realtime = realtime(args, 4);
    client::SendSettings settings{args.i32(5), args.i32(6) != 0};
    Bytes sent;
    iw_pc::Status status = client::write_packet(cl, *tree, settings, [&](const net::NetAddress& to, ByteView bytes) {
        record_sent(sent, to, bytes);
    });
    if (status) return game_error(*status);
    store(cl, args, regions);
    chan_to_arg(cl.chan, profile, args, 3);
    Bytes* out = args.buffer(7);
    if (!out) return Answer::fail("NULL sent packets");
    *out = std::move(sent);
    return nothing();
}

// CL_PacketEvent: (the packet's msg, from, the original's extra word, cls.state, cl, clc before the
// channel, the channel, realtime) -> the stop reached, else Call of Duty 2's result (i:) or "return".
Answer packet_event(Game game, Args& args) {
    std::string error;
    const auto* tree = message_tree_of(game, error);
    if (!tree) return Answer::fail(error);
    const auto* tables = field_tables(game, error);
    if (!tables) return Answer::fail(error);
    auto from = address_from(args.bytes(1), error);
    if (!from) return Answer::fail(error);
    std::int32_t state = realtime(args, 3);
    Regions regions{4, 5};
    client::ClientState cl = load(game, args, regions);
    cl.connection_state = state;
    std::int32_t profile = 0;
    if (!chan_from_arg(game, args, 6, cl.chan, profile, error)) return Answer::fail(error);
    cl.realtime = realtime(args, 7);
    StopAt parts;
    std::optional<iw_pc::Expected<client::PacketOutcome>> outcome;
    std::string msg_error = with_msg(args, 0, game, [&](net::Msg& m) {
        outcome.emplace(client::packet_event(cl, *from, m, *tree, *tables, parts));
    });
    if (!msg_error.empty()) return Answer::fail(msg_error);
    if (!*outcome) return game_error(outcome->error());
    store(cl, args, regions);
    chan_to_arg(cl.chan, profile, args, 6);
    const client::PacketOutcome& o = outcome->value();
    using Kind = client::PacketOutcome::Kind;
    if (o.kind == Kind::Connectionless) return label("connectionless");
    if (std::strcmp(parts.stopped, "return") != 0) return label(parts.stopped);
    if (o.kind == Kind::Read && o.demo) return label("demo");
    if (game == Game::CallOfDuty2) return integer(o.cod2_result() ? 1 : 0);
    return label("return");
}

// CL_ConnectionlessPacket: (from, msg, the original's extra word, cls.state, clc before the channel,
// the channel, realtime, the qport, the packets sent) -> the stop reached (the handlers the port
// leaves to its caller), else Call of Duty 2's result (i:) or "return".
Answer connectionless(Game game, Args& args) {
    using Kind = client::ClientPacket::Kind;
    std::string error;
    auto from = address_from(args.bytes(0), error);
    if (!from) return Answer::fail(error);
    std::int32_t state = realtime(args, 3);
    Regions regions{-1, 4};
    client::ClientState cl = load(game, args, regions);
    cl.connection_state = state;
    std::int32_t profile = 0;
    if (!chan_from_arg(game, args, 5, cl.chan, profile, error)) return Answer::fail(error);
    cl.realtime = realtime(args, 6);
    cl.qport = args.i32(7);
    Bytes sent;
    std::optional<client::ClientPacket> packet;
    std::string msg_error = with_msg(args, 1, game, [&](net::Msg& m) {
        packet = client::connectionless_packet(cl, *from, m, [&](const net::NetAddress& to, ByteView bytes) {
            record_sent(sent, to, bytes);
        });
    });
    if (!msg_error.empty()) return Answer::fail(msg_error);
    bool cod2 = game == Game::CallOfDuty2;
    const char* stop = nullptr;
    switch (packet->kind) {
        case Kind::Voice: stop = "voice"; break;
        case Kind::Info: stop = "info"; break;
        case Kind::Status: stop = "status"; break;
        case Kind::Disconnect: stop = "disconnect"; break;
        case Kind::Motd: stop = "motd"; break;
        case Kind::Update: stop = "update"; break;
        case Kind::Servers: stop = "servers"; break;
        case Kind::NeedCdKey: stop = cod2 ? "needcdkey" : "localize"; break;
        case Kind::NewMap: stop = "newmap"; break;
        // (United Offensive's print goes to a console window: the check stops there.)
        case Kind::Print: stop = cod2 ? nullptr : "print"; break;
        case Kind::ServerError:
            if (cod2) return game_error(iw_pc::GameError::drop(std::string(packet->text.begin(), packet->text.end())));
            stop = "localize";
            break;
        default: break;
    }
    Bytes* state_out = args.buffer(3);
    if (!state_out || state_out->size() != 4) return Answer::fail("bad state token");
    set_be_word(*state_out, 0, cl.connection_state);
    store(cl, args, regions);
    chan_to_arg(cl.chan, profile, args, 5);
    Bytes* out = args.buffer(8);
    if (!out) return Answer::fail("NULL sent packets");
    *out = std::move(sent);
    if (stop) return label(stop);
    if (cod2) return integer(packet->cod2_result() ? 1 : 0);
    return label("return");
}

// The 4-byte addresses of a LocalAddresses token.
std::vector<std::array<std::uint8_t, 4>> local_addresses(ByteView bytes) {
    std::vector<std::array<std::uint8_t, 4>> out;
    for (std::size_t i = 0; i + 4 <= bytes.size(); i += 4) out.push_back({bytes[i], bytes[i + 1], bytes[i + 2], bytes[i + 3]});
    return out;
}

// CL_CheckForResend: (cls.state, clc before the channel, realtime, the qport, the userinfo the
// original built, Call of Duty 2's key hash, net_lanauthorize, the local addresses,
// cvar_modifiedFlags, sent packets) -> "authorize" when the original goes on to ask the key
// server (the check stops there, before the packet), else "return".
Answer check_for_resend(Game game, Args& args) {
    std::int32_t state = realtime(args, 0);
    Regions regions{-1, 1};
    client::ClientState cl = load(game, args, regions);
    cl.connection_state = state;
    cl.realtime = realtime(args, 2);
    cl.qport = args.i32(3);
    const Bytes* userinfo = args.str(4);
    const Bytes* key_hash = args.str(5);
    if (!userinfo || !key_hash) return Answer::fail("NULL userinfo or key hash");
    auto local = local_addresses(args.bytes(7));
    client::ResendSettings settings{*userinfo, *key_hash, args.i32(6) != 0, local};
    Bytes sent;
    client::Resend resend = client::check_for_resend(cl, settings, [&](const net::NetAddress& to, ByteView bytes) {
        record_sent(sent, to, bytes);
    });
    const char* stop = "return";
    if (resend.kind == client::Resend::Kind::Challenge && resend.authorize) {
        sent.clear();
        stop = "authorize";
    } else if (resend.kind == client::Resend::Kind::Connect) {
        // The caller marks the settings as sent: the userinfo bit of the changed flags.
        Bytes* flags = args.buffer(8);
        if (!flags || flags->size() != 4) return Answer::fail("bad modified flags");
        set_be_word(*flags, 0, static_cast<std::int32_t>(static_cast<std::uint32_t>(be_word(*flags, 0)) & ~2u));
    }
    Bytes* state_out = args.buffer(0);
    if (!state_out || state_out->size() != 4) return Answer::fail("bad state token");
    set_be_word(*state_out, 0, cl.connection_state);
    store(cl, args, regions);
    Bytes* out = args.buffer(9);
    if (!out) return Answer::fail("NULL sent packets");
    *out = std::move(sent);
    return label(stop);
}

// CL_SystemInfoChanged: (cl, clc before the channel, sv_running, the settings the client has, the
// call log) -> nothing; what the port hands the caller becomes the calls the original makes.
Answer system_info_changed(Game game, Args& args) {
    Regions regions{0, 1};
    client::ClientState cl = load(game, args, regions);
    bool running = args.i32(2) != 0;
    std::vector<Bytes> existing;
    ByteView names = args.bytes(3);
    std::size_t start = 0;
    for (std::size_t k = 0; k <= names.size(); ++k) {
        if (k == names.size() || names[k] == 0) {
            if (k > start) {
                Bytes n(names.begin() + static_cast<std::ptrdiff_t>(start), names.begin() + static_cast<std::ptrdiff_t>(k));
                for (auto& ch : n) ch = static_cast<std::uint8_t>(std::tolower(ch));
                existing.push_back(std::move(n));
            }
            start = k + 1;
        }
    }
    auto info = client::system_info_changed(cl, running);
    if (!info) return game_error(info.error());
    CallLog log;
    if (info.value()) {
        const client::SystemInfo& si = *info.value();
        if (si.cheats_off) log.call("cheats", {});
        log.call("loaded", {CallLog::s(si.loaded_sums), CallLog::s(si.loaded_names)});
        log.call("referenced", {CallLog::s(si.referenced_sums), CallLog::s(si.referenced_names)});
        for (const auto& [key, value] : si.settings) {
            if (game == Game::CallOfDuty2) {
                Bytes lower = key;
                for (auto& ch : lower) ch = static_cast<std::uint8_t>(std::tolower(ch));
                log.call("find", {CallLog::s(key)});
                if (std::find(existing.begin(), existing.end(), lower) != existing.end()) {
                    log.call("set", {CallLog::s(value), CallLog::i(0)});
                } else {
                    log.call("find", {CallLog::s(key)});
                    log.call("register", {CallLog::s(key), CallLog::i(7), CallLog::i(0x4000), CallLog::s(value),
                                          CallLog::i(0), CallLog::i(0)});
                }
            } else {
                log.call("set", {CallLog::s(key), CallLog::s(value), CallLog::i(1)});
            }
        }
        log.call("find", {CallLog::s("sv_pure")});  // the client's own, for cl_connectedToPureServer
    }
    store(cl, args, regions);
    Bytes* out = args.buffer(4);
    if (!out) return Answer::fail("NULL call log");
    *out = std::move(log.out);
    return nothing();
}

// The check's side of the client's clock: activeAction's calls go into the log; demo playback is
// where the checks stop.
class ClockCheck : public client::ClockParts {
public:
    ClockCheck(CallLog& log, ByteView action) : log_(log), action_(action) {}
    void became_active(client::ClientState& cl) override {
        if (action_.empty()) return;
        log_.call("cbuf", {CallLog::s(action_)});
        log_.call("cbuf", {CallLog::s("\n")});
        if (cl.game == Game::CallOfDuty2) {
            log_.call("clear", {CallLog::s(""), CallLog::i(0)});
        } else {
            log_.call("set", {CallLog::s("activeAction"), CallLog::s(""), CallLog::i(1)});
        }
    }
    bool read_demo_message(client::ClientState&) override {
        stopped = "readdemo";
        return false;
    }
    bool time_demo_frame(client::ClientState&) override {
        stopped = "timedemo";
        return false;
    }
    const char* stopped = "return";

private:
    CallLog& log_;
    ByteView action_;
};

// CL_SetCGameTime: (cls.state, cl, clc before the channel, the client game's clock, the server's
// name, cl_paused, sv_paused, sv_running, cl_freezeDemo, cl_timeNudge, timescale, timedemo,
// activeAction, the call log) -> the stop reached (demo playback), else "return".
Answer set_cgame_time(Game game, Args& args) {
    std::int32_t state = realtime(args, 0);
    Regions regions{1, 2};
    client::ClientState cl = load(game, args, regions);
    cl.connection_state = state;
    std::int32_t now = realtime(args, 3);
    (game == Game::CallOfDuty2 ? cl.realtime : cl.sim_time) = now;
    const Bytes* name = args.str(4);
    const Bytes* action = args.str(12);
    if (!name || !action) return Answer::fail("NULL server name or activeAction");
    std::fill(cl.server_name.begin(), cl.server_name.end(), std::uint8_t{0});
    std::copy(name->begin(), name->end(), cl.server_name.begin());
    client::ClockSettings settings;
    settings.client_paused = args.i32(5) != 0;
    settings.server_paused = args.i32(6) != 0;
    settings.server_running = args.i32(7) != 0;
    settings.freeze_demo = args.i32(8) != 0;
    settings.time_nudge = args.i32(9);
    settings.timescale = args.f32(10);
    settings.time_demo = args.i32(11) != 0;
    CallLog log;
    ClockCheck parts(log, *action);
    if (iw_pc::Status status = client::set_cgame_time(cl, settings, parts)) return game_error(*status);
    Bytes* state_out = args.buffer(0);
    if (!state_out || state_out->size() != 4) return Answer::fail("bad state token");
    set_be_word(*state_out, 0, cl.connection_state);
    store(cl, args, regions);
    Bytes* out = args.buffer(13);
    if (!out) return Answer::fail("NULL call log");
    *out = std::move(log.out);
    return label(parts.stopped);
}

// CL_CreateNewCommands: (cls.state, cl, com_frameTime, United Offensive's old frame time and
// frame_msec, the command CL_CreateCmd makes, the call log) -> nothing.
Answer create_new_commands(Game game, Args& args) {
    std::int32_t state = realtime(args, 0);
    Regions regions{1, -1};
    client::ClientState cl = load(game, args, regions);
    cl.connection_state = state;
    std::int32_t frame_time = args.i32(2);
    client::InputTiming timing;
    timing.old_frame_time = realtime(args, 3);
    timing.frame_msec = realtime(args, 4);
    ByteView made_view = args.bytes(5);
    std::vector<std::uint8_t> made(made_view.begin(), made_view.end());
    CallLog log;
    client::create_new_commands(cl, frame_time, timing, [&](client::ClientState&, const client::InputTiming&) {
        log.call("create", {});
        return made;
    });
    Bytes* state_out = args.buffer(0);
    Bytes* old_out = args.buffer(3);
    Bytes* msec_out = args.buffer(4);
    Bytes* log_out = args.buffer(6);
    if (!state_out || !old_out || !msec_out || !log_out) return Answer::fail("NULL buffer");
    set_be_word(*state_out, 0, cl.connection_state);
    store(cl, args, regions);
    set_be_word(*old_out, 0, timing.old_frame_time);
    set_be_word(*msec_out, 0, timing.frame_msec);
    *log_out = std::move(log.out);
    return nothing();
}

// CL_ReadyToSendPacket: (cls.state, cl, clc before the channel, the channel, realtime,
// downloading, the local addresses, cl_maxpackets, the call log) -> whether to send.
Answer ready_to_send_packet(Game game, Args& args) {
    std::int32_t state = realtime(args, 0);
    Regions regions{1, 2};
    client::ClientState cl = load(game, args, regions);
    cl.connection_state = state;
    std::string error;
    std::int32_t profile = 0;
    if (!chan_from_arg(game, args, 3, cl.chan, profile, error)) return Answer::fail(error);
    cl.realtime = realtime(args, 4);
    bool downloading = (args.i32(5) & 0xFF) != 0;
    auto local = local_addresses(args.bytes(6));
    std::int32_t max_packets = args.i32(7);
    std::int32_t before = max_packets;
    bool ready = client::ready_to_send_packet(cl, downloading, local, max_packets);
    CallLog log;
    if (max_packets != before) {
        std::string text = std::to_string(max_packets);
        log.call("set", {CallLog::s("cl_maxpackets"), CallLog::s(ByteView(reinterpret_cast<const std::uint8_t*>(text.data()), text.size())),
                         CallLog::i(1)});
    }
    Bytes* out = args.buffer(8);
    if (!out) return Answer::fail("NULL call log");
    *out = std::move(log.out);
    return integer(ready ? 1 : 0);
}


// --- downloads (specs/net_download.py) -------------------------------------------------------

// Where United Offensive keeps the download's numbers in clc (offsets from 0x04DC8860), and
// Call of Duty 2 in its download block (0x00966DAC, after the three names) and its menus' block.
constexpr std::size_t kUoFile = 0x20148, kUoBlock = 0x20150, kUoCount = 0x20154, kUoSize = 0x20158,
                      kUoFlags = 0x2015C, kUoWww = 0x20560, kUoAborting = 0x20564, kUoBad = 0x20968;
constexpr std::size_t kCod2Flags = 0x248, kCod2Www = 0x24C, kCod2File = 0x250, kCod2Block = 0x254,
                      kCod2Size = 0x258;
constexpr std::size_t kUiSize = 0x10, kUiCount = 0x14, kUiName = 0x1C;

std::int32_t le_word(const Bytes& b, std::size_t at) {
    if (at + 4 > b.size()) throw std::runtime_error("short download region");
    return static_cast<std::int32_t>(static_cast<std::uint32_t>(b[at]) | static_cast<std::uint32_t>(b[at + 1]) << 8 |
                                     static_cast<std::uint32_t>(b[at + 2]) << 16 |
                                     static_cast<std::uint32_t>(b[at + 3]) << 24);
}

void set_le_word(Bytes& b, std::size_t at, std::int32_t v) {
    if (at + 4 > b.size()) throw std::runtime_error("short download region");
    for (std::size_t k = 0; k < 4; ++k) b[at + k] = static_cast<std::uint8_t>(static_cast<std::uint32_t>(v) >> (8 * k));
}

// The check's host: the stand-ins' answers, their calls logged; Call of Duty 2's menus' numbers
// written into their block.
class CheckDownloadHost : public client::DownloadHost {
public:
    Game game;
    std::int32_t handle = 0, begin = 0, shell = 0, window = 0;
    std::optional<Bytes> home;
    Bytes ui;
    CallLog log;

    std::int32_t open(ByteView temp_name) override {
        log.call("FS_SV_FOpenFileWrite", {CallLog::s(temp_name)});
        return handle;
    }
    void write(ByteView data, std::int32_t length, std::int32_t file) override {
        log.call("FS_Write", {CallLog::s(data), CallLog::i(length), CallLog::i(file)});
    }
    void close(std::int32_t file) override { log.call("FS_FCloseFile", {CallLog::i(file)}); }
    void rename(ByteView from, ByteView to) override { log.call("FS_SV_Rename", {CallLog::s(from), CallLog::s(to)}); }
    void write_packet() override { log.call("CL_WritePacket", {}); }
    void next_download() override { log.call("CL_NextDownload", {}); }
    void cvar_value(const char* name, std::int32_t value) {
        log.call("Cvar_SetValue", {CallLog::s(name), CallLog::i(std::bit_cast<std::int32_t>(static_cast<float>(value)))});
    }
    void set_size(std::int32_t size) override {
        if (game == Game::CallOfDuty2) set_le_word(ui, kUiSize, size);
        else cvar_value("cl_downloadSize", size);
    }
    void set_count(std::int32_t count) override {
        if (game == Game::CallOfDuty2) set_le_word(ui, kUiCount, count);
        else cvar_value("cl_downloadCount", count);
    }
    void clear_name() override {
        if (game == Game::CallOfDuty2) {
            if (kUiName < ui.size()) ui[kUiName] = 0;
        } else {
            log.call("Cvar_Set2", {CallLog::s("cl_downloadName"), CallLog::s(""), CallLog::i(1)});
        }
    }
    std::vector<std::uint8_t> home_path() override {
        log.call(game == Game::CallOfDuty2 ? "Dvar_FindVar" : "Cvar_FindVar", {CallLog::s("fs_homepath")});
        return home ? *home : Bytes();
    }
    std::vector<std::uint8_t> os_path(ByteView base, ByteView g, ByteView qpath) override {
        log.call("FS_BuildOSPath", {CallLog::s(base), CallLog::s(g), CallLog::s(qpath), CallLog::i(0)});
        // The check's FS_BuildOSPath: base/game/qpath, at most 255 characters.
        Bytes path(base.begin(), base.end());
        path.push_back('/');
        path.insert(path.end(), g.begin(), g.end());
        path.push_back('/');
        path.insert(path.end(), qpath.begin(), qpath.end());
        if (path.size() > 255) path.resize(255);
        return path;
    }
    bool open_url(ByteView url) override {
        if (game == Game::CallOfDuty2) {
            log.call("ShellExecuteA", {CallLog::s("open"), CallLog::s(url), CallLog::i(9)});
            if (shell == 0) return false;
            log.call("GetForegroundWindow", {});
            if (window != 0) log.call("ShowWindow", {CallLog::i(window), CallLog::i(3)});
            return true;
        }
        log.call("Sys_OpenURL", {CallLog::s(url), CallLog::i(1)});
        return true;
    }
    void add_text(ByteView text) override { log.call("Cbuf_AddText", {CallLog::s(text)}); }
    bool begin_download(ByteView local, ByteView remote) override {
        log.call("DL_BeginDownload", {CallLog::s(local), CallLog::s(remote)});
        return begin != 0;
    }
    void end_download() override { log.call("DL_EndDownload", {}); }
};

// CL_ParseDownload and Call of Duty 2's CL_ParseDownloadRedirect: (msg, clc before the channel,
// the download block, the menus' block, the stand-ins' answers, the call log) -> the first warning.
Answer parse_download_answer(Game game, Args& args, bool redirect) {
    Regions regions{-1, 1};
    client::ClientState cl = load(game, args, regions);
    client::DownloadState dl;
    Bytes block = args.region(2);
    if (block.size() < 0x244) return Answer::fail("short download block");
    dl.www_disconnected = le_word(block, 0) != 0;
    std::copy(block.begin() + 4, block.begin() + 0x104, dl.name.begin());
    std::copy(block.begin() + 0x104, block.begin() + 0x204, dl.temp_name.begin());
    std::copy(block.begin() + 0x204, block.begin() + 0x244, dl.original_name.begin());
    Bytes* ui = args.buffer(3);
    if (!ui) return Answer::fail("NULL menus' block");
    CheckDownloadHost host;
    host.game = game;
    host.ui = *ui;
    if (game == Game::CallOfDuty2) {
        dl.flags = le_word(block, kCod2Flags);
        dl.www = le_word(block, kCod2Www) != 0;
        dl.file = le_word(block, kCod2File);
        dl.block = le_word(block, kCod2Block);
        dl.size = le_word(block, kCod2Size);
        dl.count = le_word(host.ui, kUiCount);
    } else {
        Bytes clc = args.region(1);
        dl.file = le_word(clc, kUoFile);
        dl.block = le_word(clc, kUoBlock);
        dl.count = le_word(clc, kUoCount);
        dl.size = le_word(clc, kUoSize);
        dl.flags = le_word(clc, kUoFlags);
        dl.www = le_word(clc, kUoWww) != 0;
        dl.www_aborting = le_word(clc, kUoAborting) != 0;
        if (kUoBad < clc.size()) {
            ByteView bad = client::c_str(ByteView(clc).subspan(kUoBad));
            dl.bad_checksums.assign(bad.begin(), bad.end());
        }
    }
    ByteView answers = args.bytes(4);
    if (answers.size() < 17) return Answer::fail("short answers token");
    auto be = [&](std::size_t i) {
        return static_cast<std::int32_t>(static_cast<std::uint32_t>(answers[4 * i]) << 24 |
                                         static_cast<std::uint32_t>(answers[4 * i + 1]) << 16 |
                                         static_cast<std::uint32_t>(answers[4 * i + 2]) << 8 | answers[4 * i + 3]);
    };
    host.handle = be(0);
    host.begin = be(1);
    host.shell = be(2);
    host.window = be(3);
    if (answers[16] == 1) host.home = Bytes(answers.begin() + 17, answers.end());
    else if (answers[16] != 0) return Answer::fail("bad answers token");
    std::optional<iw_pc::Expected<std::optional<client::DownloadWarning>>> result;
    std::string error = with_msg(args, 0, game, [&](net::Msg& m) {
        result = redirect ? client::parse_redirect(m, cl, dl, host) : client::parse_download(m, cl, dl, host);
    });
    if (!error.empty()) return Answer::fail(error);
    store(cl, args, regions);
    Bytes& out = args.region(2);
    set_le_word(out, 0, dl.www_disconnected ? 1 : 0);
    std::copy(dl.name.begin(), dl.name.end(), out.begin() + 4);
    std::copy(dl.temp_name.begin(), dl.temp_name.end(), out.begin() + 0x104);
    std::copy(dl.original_name.begin(), dl.original_name.end(), out.begin() + 0x204);
    if (game == Game::CallOfDuty2) {
        set_le_word(out, kCod2Flags, dl.flags);
        set_le_word(out, kCod2Www, dl.www ? 1 : 0);
        set_le_word(out, kCod2File, dl.file);
        set_le_word(out, kCod2Block, dl.block);
        set_le_word(out, kCod2Size, dl.size);
    } else {
        Bytes& clc = args.region(1);
        set_le_word(clc, kUoFile, dl.file);
        set_le_word(clc, kUoBlock, dl.block);
        set_le_word(clc, kUoCount, dl.count);
        set_le_word(clc, kUoSize, dl.size);
        set_le_word(clc, kUoFlags, dl.flags);
        set_le_word(clc, kUoWww, dl.www ? 1 : 0);
        set_le_word(clc, kUoAborting, dl.www_aborting ? 1 : 0);
    }
    *args.buffer(3) = host.ui;
    Bytes* log = args.buffer(5);
    if (!log) return Answer::fail("NULL call log");
    *log = std::move(host.log.out);
    if (!result->has_value()) return game_error(result->error());
    const std::optional<client::DownloadWarning>& warning = result->value();
    if (!warning) return Answer(Ret(Printed{}));
    const char* format = client::message_format(*warning);
    return Answer(Ret(Printed{Bytes(format, format + std::strlen(format))}));
}

// --- the connect command (specs/net_connect.py) ---------------------------------------

// Offsets in the clc part the check passes (from clc.serverAddress), and Call of Duty 2's
// menus' block: the server-quit flag and the two texts.
constexpr std::size_t kConnectTime = 0x14, kConnectCount = 0x18, kServerMessage = 0x1C;
constexpr std::size_t kUiQuit = 0xDD, kUiTexts[2] = {0x5C, 0x9C};

class CheckConnectHost : public client::ConnectHost {
public:
    Game game = Game::CallOfDuty2;
    bool running = false;
    std::optional<net::NetAddress> resolved;
    bool key = false;
    std::int32_t qport = 0;
    Bytes ui, keys;
    std::optional<std::int32_t> key_catchers;
    CallLog log;

    bool server_running() override { return running; }
    void stop_sounds() override { log.call(game == Game::CallOfDuty2 ? "SND_StopSounds" : "S_StopAllSounds", {CallLog::i(0)}); }
    void set_setting(ByteView name, ByteView value) override { log.call("Cvar_Set2", {CallLog::s(name), CallLog::s(value), CallLog::i(1)}); }
    void local_server_quit() override {
        if (game == Game::CallOfDuty2) {
            if (kUiQuit < ui.size()) ui[kUiQuit] = 1;
        } else {
            log.call("Com_ShutdownServer", {CallLog::s("EXE_SERVERQUIT")});
        }
    }
    void clear_menu_texts() override {
        for (std::size_t at : kUiTexts)
            if (at < ui.size()) ui[at] = 0;
    }
    void server_frame() override { log.call("SV_Frame", {CallLog::i(0)}); }
    void disconnect() override {
        if (game == Game::CallOfDuty2) log.call("CL_Disconnect", {});
        else log.call("CL_Disconnect", {CallLog::i(1)});
    }
    void close_console() override { log.call("Con_Close", {}); }
    bool resolve(ByteView name, net::NetAddress& address) override {
        log.call("NET_StringToAdr", {CallLog::s(name)});
        if (!resolved) return false;
        address = *resolved;
        return true;
    }
    bool cd_key_valid() override {
        log.call("CL_CDKeyValidate", {});
        return key;
    }
    void clear_key_catchers() override {
        if (game == Game::CallOfDuty2) {
            for (std::size_t i = 0; i < 5 && i < keys.size(); ++i) keys[i] = 0;  // the word at 0x0096B654 and the byte after it
        } else {
            key_catchers = 0;
        }
    }
    std::int32_t net_qport() override { return qport; }
    void close_menus() override {
        log.call("UI_CloseAll", {});
        log.call("SCR_UpdateScreen", {});
    }
};

// CL_Connect_f: (argc, the second word, sv_running, the menus' block, cls, clc from the server's
// address, cls.servername, [CoD2: the key catchers, net_qport,] the stand-ins' answers, the call
// log) -> what it printed.
Answer connect_answer(Game game, Args& args) {
    bool cod2 = game == Game::CallOfDuty2;
    std::size_t answers_at = cod2 ? 9 : 7, log_at = cod2 ? 10 : 8;
    ByteView answers = args.bytes(answers_at);
    if (answers.empty()) return Answer::fail("short answers");
    CheckConnectHost host;
    host.game = game;
    std::string error;
    if (answers[0] == 1) {
        if (answers.size() < 22) return Answer::fail("short answers");
        host.resolved = address_from(answers.subspan(1, 20), error);
        if (!host.resolved) return Answer::fail(error);
    } else if (answers[0] != 0) {
        return Answer::fail("bad answers token");
    }
    host.key = answers.back() != 0;

    client::ClientState cl(game);
    const Bytes cls = args.region(4);
    cl.connection_state = le_word(cls, 0);
    if (cod2) cl.qport = le_word(cls, 4);
    const Bytes clc = args.region(5);
    if (clc.size() < kServerMessage + client::kServerMessageSize) return Answer::fail("short clc");
    auto server = address_from(canonical_address(game, Bytes(clc.begin(), clc.begin() + 20)), error);
    if (!server) return Answer::fail(error);
    cl.server_address = *server;
    cl.connect_time = le_word(clc, kConnectTime);
    cl.connect_packet_count = le_word(clc, kConnectCount);
    std::copy(clc.begin() + kServerMessage, clc.begin() + kServerMessage + client::kServerMessageSize, cl.server_message.begin());
    const Bytes name = args.region(6);
    if (name.size() < client::kServerNameSize) return Answer::fail("short server name");
    std::copy(name.begin(), name.begin() + client::kServerNameSize, cl.server_name.begin());

    host.running = (cod2 ? (args.i32(2) & 0xFF) : args.i32(2)) != 0;
    host.qport = cod2 ? args.i32(8) : 0;
    if (Bytes* ui = args.buffer(3)) host.ui = *ui;
    if (cod2) host.keys = args.region(7);
    std::int32_t argc = args.i32(0);
    const Bytes* second = args.str(1);
    Bytes word = second ? *second : Bytes{};
    std::vector<ByteView> words;
    for (std::int32_t i = 0; i < argc; ++i) words.push_back(i == 1 ? ByteView(word) : ByteView(reinterpret_cast<const std::uint8_t*>("connect"), 7));
    auto result = client::connect_command(cl, words, host);

    // What the call changed (also when it ends in an error).
    Bytes& cls_out = args.region(4);
    set_le_word(cls_out, 0, cl.connection_state);
    if (cod2) set_le_word(cls_out, 4, cl.qport);
    else if (host.key_catchers) set_le_word(cls_out, 4, *host.key_catchers);
    Bytes& clc_out = args.region(5);
    auto canonical = address_bytes(cl.server_address);
    Bytes guest = guest_address(game, Bytes(canonical.begin(), canonical.end()));
    std::copy(guest.begin(), guest.end(), clc_out.begin());
    set_le_word(clc_out, kConnectTime, cl.connect_time);
    set_le_word(clc_out, kConnectCount, cl.connect_packet_count);
    std::copy(cl.server_message.begin(), cl.server_message.end(), clc_out.begin() + kServerMessage);
    Bytes& name_out = args.region(6);
    std::copy(cl.server_name.begin(), cl.server_name.end(), name_out.begin());
    if (cod2) args.region(7) = host.keys;
    if (Bytes* ui = args.buffer(3)) *ui = host.ui;
    Bytes* log = args.buffer(log_at);
    if (!log) return Answer::fail("NULL call log");
    *log = std::move(host.log.out);
    if (!result.has_value()) return game_error(result.error());
    const char* format = result.value() == client::ConnectResult::Connecting ? client::kResolvedFormat : client::message_format(result.value());
    return Answer(Ret(Printed{Bytes(format, format + std::strlen(format))}));
}

}  // namespace

std::optional<Answer> dispatch_client(Game game, const std::string& f, Args& args) {
    if (f == "CL_ParseDownload") return parse_download_answer(game, args, false);
    if (f == "CL_Connect_f") return connect_answer(game, args);
    if (f == "CL_ParseDownloadRedirect" && game == Game::CallOfDuty2) return parse_download_answer(game, args, true);
    if (f == "CL_AdjustTimeDelta") {  // cl, clc, the client game's clock, timescale
        Regions regions{0, 1};
        client::ClientState cl = load(game, args, regions);
        std::int32_t now = realtime(args, 2);
        cl.realtime = now;
        cl.sim_time = now;
        client::ClockSettings settings;
        settings.timescale = args.f32(3);
        client::adjust_time_delta(cl, settings);
        store(cl, args, regions);
        return nothing();
    }
    if (f == "CL_FirstSnapshot") {  // cls.state, cl, clc, the client game's clock, activeAction, the call log
        std::int32_t state = realtime(args, 0);
        Regions regions{1, 2};
        client::ClientState cl = load(game, args, regions);
        cl.connection_state = state;
        std::int32_t now = realtime(args, 3);
        cl.realtime = now;
        cl.sim_time = now;
        const Bytes* action = args.str(4);
        if (!action) return Answer::fail("NULL activeAction");
        CallLog log;
        ClockCheck parts(log, *action);
        if (client::first_snapshot(cl)) parts.became_active(cl);
        Bytes* state_out = args.buffer(0);
        Bytes* out = args.buffer(5);
        if (!state_out || !out) return Answer::fail("NULL buffer");
        set_be_word(*state_out, 0, cl.connection_state);
        store(cl, args, regions);
        *out = std::move(log.out);
        return nothing();
    }
    if (f == "CL_CreateNewCommands") return create_new_commands(game, args);
    if (f == "CL_ReadyToSendPacket") return ready_to_send_packet(game, args);
    if (f == "CL_CheckTimeout") {  // cls.state, cl, clc, realtime, cl_paused, sv_paused, cl_timeout, cl_connectTimeout
        std::int32_t state = realtime(args, 0);
        Regions regions{1, 2};
        client::ClientState cl = load(game, args, regions);
        cl.connection_state = state;
        cl.realtime = realtime(args, 3);
        client::TimeoutSettings settings;
        settings.client_paused = args.i32(4) != 0;
        settings.server_paused = args.i32(5) != 0;
        settings.timeout = args.f32(6);
        settings.connect_timeout = game == Game::CallOfDuty2 ? args.f32(7) : 0.0f;
        if (iw_pc::Status status = client::check_timeout(cl, settings)) return game_error(*status);
        store(cl, args, regions);
        return nothing();
    }
    if (f == "CL_CheckUserinfo") {  // cls.state, cl_paused, the modified flags, the settings text, clc, the call log
        std::int32_t state = realtime(args, 0);
        Regions regions{-1, 4};
        client::ClientState cl = load(game, args, regions);
        cl.connection_state = state;
        std::int32_t flags = realtime(args, 2);
        client::UserinfoSent sent = client::check_userinfo(cl, args.i32(1) != 0, flags, args.text(3));
        CallLog log;
        std::optional<net::ReliableCommandWarning> warning;
        if (sent.sent) {
            log.call("infostring", {CallLog::i(2)});
            if (sent.queued.error) return game_error(*sent.queued.error);
            warning = sent.queued.warning;
        }
        store(cl, args, regions);
        Bytes* flags_out = args.buffer(2);
        Bytes* log_out = args.buffer(5);
        if (!flags_out || !log_out) return Answer::fail("NULL buffer");
        set_be_word(*flags_out, 0, flags);
        *log_out = log.out;
        if (!warning) return Answer(Ret(Printed{}));
        const char* format = net::message_format(*warning);
        return Answer(Ret(Printed{Bytes(format, format + std::strlen(format))}));
    }
    if (f == "CL_ConfigstringModified") {  // argv(1), argv(2), argc, cl, the call log
        Regions regions{3, -1};
        client::ClientState cl = load(game, args, regions);
        // The original reads "" past the last word (an unsigned argc).
        auto argc = static_cast<std::uint32_t>(args.i32(2));
        ByteView index = argc > 1 ? args.text(0) : ByteView();
        ByteView text = argc > 2 ? args.text(1) : ByteView();
        auto changed = client::configstring_modified(cl, index, text);
        if (!changed) return game_error(changed.error());
        store(cl, args, regions);
        CallLog log;
        if (changed.value() == client::kSystemInfo) log.call("systeminfo", {});
        Bytes* out = args.buffer(4);
        if (!out) return Answer::fail("NULL call log");
        *out = log.out;
        return nothing();
    }
    if (f == "CL_GetServerCommand") {  // number, cl, clc, the big config string, the words, the call log
        Regions regions{1, 2};
        client::ClientState cl = load(game, args, regions);
        std::array<std::uint8_t, client::kBigConfigStringSize> big{};
        Bytes& region = args.region(3);
        std::copy(region.begin(), region.begin() + client::kBigConfigStringSize, big.begin());
        auto tokens = std::make_unique<net::Tokens>();
        auto command = client::get_server_command(cl, args.i32(0), *tokens, big);
        if (!command) return game_error(command.error());
        CallLog log;
        Ret result = Ret(std::int64_t{1});
        using Kind = client::ServerCommand::Kind;
        switch (command.value().kind) {
            case Kind::Disconnected:
                // Com_Error(ERR_SERVERDISCONNECT, ...): the game's error, level 2.
                return Answer::fail(std::string(iw_pc::GameError::exit_name()) + " 2");
            case Kind::MapRestart:
                log.call("notify", {});
                result = Ret(Str{Bytes{'r', 'e', 's', 't', 'a', 'r', 't'}});
                break;
            case Kind::ConfigString:
                if (command.value().changed == client::kSystemInfo) log.call("systeminfo", {});
                break;
            case Kind::Endless:
                return Answer::fail("the original never returns (the checks don't give this)");
            case Kind::ClientGame:
                break;
            case Kind::Kept:
                result = Ret(std::int64_t{0});
                break;
        }
        store(cl, args, regions);
        std::copy(big.begin(), big.end(), args.region(3).begin());
        Bytes words;
        auto put32 = [&](std::uint32_t v) {
            for (int k = 3; k >= 0; --k) words.push_back(static_cast<std::uint8_t>(v >> (8 * k)));
        };
        put32(static_cast<std::uint32_t>(tokens->argc()));
        for (std::size_t i = 0; i < tokens->argc(); ++i) {
            ByteView w = tokens->argv(i);
            put32(static_cast<std::uint32_t>(w.size()));
            words.insert(words.end(), w.begin(), w.end());
        }
        Bytes* words_out = args.buffer(4);
        Bytes* log_out = args.buffer(5);
        if (!words_out || !log_out) return Answer::fail("NULL buffer");
        *words_out = words;
        *log_out = log.out;
        return Answer(result);
    }
    if (f == "CL_AddReliableCommand") {  // command, clc -> the copy's warning
        Regions regions{-1, 1};
        client::ClientState cl = load(game, args, regions);
        const Bytes* command = args.str(0);
        if (!command) return Answer::fail("NULL command");
        client::Queued queued = client::add_reliable_command(cl, *command);
        if (queued.error) return game_error(*queued.error);
        store(cl, args, regions);
        if (!queued.warning) return Answer(Ret(Printed{}));
        const char* format = net::message_format(*queued.warning);
        return Answer(Ret(Printed{Bytes(format, format + std::strlen(format))}));
    }
    if (f == "CL_SendPureChecksums") {  // search path, checksum feed, fake checksum, clc
        Regions regions{-1, 3};
        client::ClientState cl = load(game, args, regions);
        std::vector<iw_pc::fs::SearchPath> paths = search_paths(args.bytes(0));
        if (iw_pc::Status status = client::send_pure_checksums(cl, paths, args.i32(1), args.i32(2))) {
            return game_error(*status);
        }
        store(cl, args, regions);
        return nothing();
    }
    if (f == "CL_FinishMove") {  // cmd, cl
        Regions regions{1, -1};
        client::ClientState cl = load(game, args, regions);
        Bytes* cmd = args.buffer(0);
        if (!cmd || cmd->size() < net::usercmd_size(game)) return Answer::fail("short cmd");
        client::finish_move(cl, *cmd);
        return nothing();
    }
    if (f == "CL_SetCGameTime") return set_cgame_time(game, args);
    if (f == "CL_SystemInfoChanged") return system_info_changed(game, args);
    if (f == "Sys_IsLANAddress") {  // netadr, local addresses -> bool
        std::string error;
        auto address = address_from(args.bytes(0), error);
        if (!address) return Answer::fail(error);
        auto local = local_addresses(args.bytes(1));
        return integer(net::is_lan_address(*address, local) ? 1 : 0);
    }
    if (f == "CL_CheckForResend") return check_for_resend(game, args);
    if (f == "CL_ParseCommandString") {  // msg, clc
        Regions regions{-1, 1};
        client::ClientState cl = load(game, args, regions);
        std::string error = with_msg(args, 0, game, [&](net::Msg& m) { client::parse_command_string(m, cl); });
        if (!error.empty()) return Answer::fail(error);
        store(cl, args, regions);
        return nothing();
    }
    if (f == "CL_ParseGamestate" || f == "CL_ParseSnapshot") {  // msg, cl, clc (, realtime)
        std::string error;
        const auto* tables = field_tables(game, error);
        if (!tables) return Answer::fail(error);
        Regions regions{1, 2};
        client::ClientState cl = load(game, args, regions);
        bool snapshot = f == "CL_ParseSnapshot";
        if (snapshot) cl.realtime = realtime(args, 3);
        iw_pc::Status status;
        error = with_msg(args, 0, game, [&](net::Msg& m) {
            status = snapshot ? client::parse_snapshot(m, *tables, cl) : client::parse_game_state(m, *tables, cl);
        });
        if (!error.empty()) return Answer::fail(error);
        if (status) return game_error(*status);
        store(cl, args, regions);
        // The game state: where the original goes on to apply the system info.
        return snapshot ? nothing() : label("systeminfo");
    }
    if (f == "CL_ParsePacketEntities") return packet(game, args, false);
    if (f == "CL_ParsePacketClients") return packet(game, args, true);
    if (f == "CL_ParseServerMessage") return server_message(game, args);  // coded msg, cl, clc, realtime
    if (f == "MSG_SetDefaultUserCmd") {  // ps, cmd
        ByteView ps = args.bytes(0);
        if (ps.size() < net::player_state_size(game)) return Answer::fail("short player state");
        Bytes* cmd = args.buffer(1);
        if (!cmd || cmd->size() < net::usercmd_size(game)) return Answer::fail("short cmd");
        net::set_default_usercmd(game, ps, *cmd);
        return nothing();
    }
    if (f == "CL_WritePacket") return write_packet(game, args);
    if (f == "CL_PacketEvent") return packet_event(game, args);
    if (f == "CL_ConnectionlessPacket") return connectionless(game, args);
    return std::nullopt;
}

}  // namespace pc_check
