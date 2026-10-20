// The client's parsers of the server's messages (client.h; the Rust module
// iw_pc::client is the full version and documents each one).

#include "iw_pc/client/client.h"

#include <algorithm>
#include <memory>
#include <cstring>
#include <string>

#include "iw_pc/crt.h"
#include "iw_pc/cstr.h"
#include "iw_pc/net/netchan.h"
#include "iw_pc/net/oob.h"
#include "iw_pc/shared/hash.h"
#include "iw_pc/shared/info.h"
#include "iw_pc/shared/strings.h"

namespace iw_pc::client {

namespace {

/// An entity or client number that ends a list.
constexpr std::int32_t kListEnd = 99999;
/// The entity number that ends a snapshot's entity list.
constexpr std::int32_t kEntityListEnd = 1023;

std::int32_t add(std::int32_t a, std::int32_t b) {
    return static_cast<std::int32_t>(static_cast<std::uint32_t>(a) + static_cast<std::uint32_t>(b));
}
std::int32_t sub(std::int32_t a, std::int32_t b) {
    return static_cast<std::int32_t>(static_cast<std::uint32_t>(a) - static_cast<std::uint32_t>(b));
}
std::size_t ring_slot(std::int32_t ring) { return static_cast<std::uint32_t>(ring) & (kMaxParseEntities - 1); }

ByteSpan entity_at(ClientState& cl, std::int32_t ring) {
    std::size_t size = entity_state_size(cl.game);
    return ByteSpan(cl.parse_entities).subspan(ring_slot(ring) * size, size);
}
ByteSpan client_at(ClientState& cl, std::int32_t ring) {
    return ByteSpan(cl.parse_clients).subspan(ring_slot(ring) * kClientStateSize, kClientStateSize);
}

/// The last `len` bytes of snapshot `index` as the game lays it out
/// (clSnapshot_t: seven numbers, the player state, five numbers).
std::vector<std::uint8_t> snapshot_tail(const ClientState& cl, std::size_t index, std::size_t len) {
    const Snapshot& s = cl.snapshots[index];
    std::vector<std::uint8_t> bytes;
    auto put = [&](std::int32_t v) {
        auto u = static_cast<std::uint32_t>(v);
        for (int k = 0; k < 4; ++k) bytes.push_back(static_cast<std::uint8_t>(u >> (8 * k)));
    };
    for (std::int32_t v : {s.valid, s.snap_flags, s.server_time, s.message_num, s.delta_num, s.ping, s.cmd_num}) put(v);
    bytes.insert(bytes.end(), s.player_state.begin(), s.player_state.end());
    for (std::int32_t v : {s.num_entities, s.num_clients, s.parse_entities_num, s.parse_clients_num, s.server_command_num})
        put(v);
    return std::vector<std::uint8_t>(bytes.end() - static_cast<std::ptrdiff_t>(len), bytes.end());
}

/// Walks the old snapshot's list of entities or clients in a ring.
struct OldList {
    std::int32_t count = 0, start = 0, index = 0, ring = 0, number = kListEnd;
};

}  // namespace

ByteView GameStateStrings::get(std::size_t index) const {
    if (index >= offsets.size()) return {};
    auto at = static_cast<std::size_t>(std::max(offsets[index], 0));
    if (at >= data.size()) return {};
    return cstr::text(ByteView(data).subspan(at));
}

void Snapshot::clear() {
    valid = snap_flags = server_time = message_num = delta_num = ping = cmd_num = 0;
    std::fill(player_state.begin(), player_state.end(), std::uint8_t{0});
    num_entities = num_clients = parse_entities_num = parse_clients_num = server_command_num = 0;
}

ClientState::ClientState(Game game)
    : game(game),
      snap(game),
      game_state(game),
      snapshots(kPacketBackup, Snapshot(game)),
      baselines(kMaxEntities * entity_state_size(game), 0),
      parse_entities(kMaxParseEntities * entity_state_size(game), 0),
      parse_clients(kMaxParseEntities * kClientStateSize, 0),
      cmds(kCmdBackup * net::usercmd_size(game), 0),
      reliable_commands(max_reliable_commands(game) * kServerCommandSize, 0),
      server_message(kServerMessageSize, 0),
      server_commands(max_reliable_commands(game) * kServerCommandSize, 0),
      chan(game, net::NetSource::Client, net::NetAddress{}, 0),
      server_name(kServerNameSize, 0) {}

void ClientState::clear() {
    timeout_count = 0;
    snap.clear();
    old_frame_server_time = 0;
    new_snapshots = 0;
    server_time = 0;
    old_server_time = 0;
    frame_server_time = 0;
    server_time_delta = 0;
    extrapolated_snapshot = 0;
    std::fill(game_state.offsets.begin(), game_state.offsets.end(), 0);
    std::fill(game_state.data.begin(), game_state.data.end(), std::uint8_t{0});
    game_state.data_count = 0;
    parse_entities_num = 0;
    parse_clients_num = 0;
    out_packets = {};
    for (Snapshot& s : snapshots) s.clear();
    std::fill(baselines.begin(), baselines.end(), std::uint8_t{0});
    std::fill(parse_entities.begin(), parse_entities.end(), std::uint8_t{0});
    std::fill(parse_clients.begin(), parse_clients.end(), std::uint8_t{0});
    server_id = 0;
    std::fill(cmds.begin(), cmds.end(), std::uint8_t{0});
    cmd_number = 0;
    user_cmd_weapon = 0;
    user_cmd_offhand = 0;
    cgame_angles = {};
    view_angles = {};
}

ByteView ClientState::server_command(std::int32_t sequence) const {
    std::size_t slot = static_cast<std::uint32_t>(sequence) % max_reliable_commands(game);
    return cstr::text(ByteView(server_commands).subspan(slot * kServerCommandSize, kServerCommandSize));
}

void parse_command_string(net::Msg& msg, ClientState& cl) {
    std::int32_t sequence = msg.read_long();
    std::array<std::uint8_t, net::kMaxStringChars> text{};
    ByteView s = msg.read_string(text);
    if (cl.server_command_sequence < sequence) {
        cl.server_command_sequence = sequence;
        std::size_t slot = static_cast<std::uint32_t>(sequence) % max_reliable_commands(cl.game);
        ByteSpan dest = ByteSpan(cl.server_commands).subspan(slot * kServerCommandSize, kServerCommandSize);
        crt::strncpy(dest.first(kServerCommandSize - 1), s);
        dest[kServerCommandSize - 1] = 0;
    }
}

Status parse_game_state(net::Msg& msg, const net::FieldTables& tables, ClientState& cl) {
    Game game = cl.game;
    ServerOps ops = server_ops(game);
    cl.connect_packet_count = 0;
    cl.clear();
    cl.server_command_sequence = msg.read_long();
    cl.game_state.data_count = 1;
    std::size_t size = entity_state_size(game);
    std::vector<std::uint8_t> zeros(size, 0);
    for (;;) {
        std::int32_t op = msg.read_byte();
        if (op == ops.eof) break;
        if (op == ops.config_string) {
            std::int32_t index = msg.read_short();
            if (index < 0 || index >= static_cast<std::int32_t>(kMaxConfigStrings))
                return GameError::drop("configstring > MAX_CONFIGSTRINGS");
            std::array<std::uint8_t, net::kBigInfoString> text{};
            ByteView s = msg.read_big_string(text);
            GameStateStrings& gs = cl.game_state;
            std::int64_t start = gs.data_count;
            std::int64_t end = start + static_cast<std::int64_t>(s.size()) + 1;
            if (end > static_cast<std::int64_t>(max_game_state_chars(game))) return GameError::drop("MAX_GAMESTATE_CHARS exceeded");
            gs.offsets[static_cast<std::size_t>(index)] = static_cast<std::int32_t>(start);
            auto at = static_cast<std::size_t>(start);
            if (!s.empty()) std::memmove(gs.data.data() + at, s.data(), s.size());
            gs.data[at + s.size()] = 0;
            gs.data_count = static_cast<std::int32_t>(end);
        } else if (op == ops.baseline) {
            std::int32_t number = msg.read_bits(net::kEntityNumberBits);
            if (number < 0 || number >= static_cast<std::int32_t>(kMaxEntities))
                return GameError::drop("Baseline number out of range: " + std::to_string(number));
            ByteSpan baseline = ByteSpan(cl.baselines).subspan(static_cast<std::size_t>(number) * size, size);
            net::read_delta_entity(msg, tables, zeros, baseline, number);
        } else if (game == Game::CallOfDuty2) {
            return GameError::drop(
                "CL_ParseGamestate: bad command byte (GameState may exceed MAX_GAMESTATE_CHARS, or packet may exceed "
                "MAX_MSGLEN)");
        } else {
            return GameError::drop("CL_ParseGamestate: bad command byte");
        }
    }
    cl.client_num = msg.read_long();
    cl.checksum_feed = msg.read_long();
    return std::nullopt;
}

Status parse_packet_entities(net::Msg& msg, const net::FieldTables& tables, ClientState& cl, std::optional<std::size_t> old,
                             Snapshot& snapshot) {
    bool cod2 = cl.game == Game::CallOfDuty2;
    std::size_t size = entity_state_size(cl.game);
    snapshot.parse_entities_num = cl.parse_entities_num;
    snapshot.num_entities = 0;
    OldList o;
    if (old) {
        o.count = cl.snapshots[*old].num_entities;
        o.start = cl.snapshots[*old].parse_entities_num;
    }
    auto number_at = [&](std::int32_t ring) { return net::word_at(entity_at(cl, ring), 0); };
    o.ring = o.start;
    o.number = old && o.count >= 1 ? number_at(o.ring) : kListEnd;
    auto next_old = [&]() {
        if (o.index < o.count) {
            o.ring = add(o.start, o.index);
            o.number = number_at(o.ring);
        } else {
            o.number = kListEnd;
        }
    };
    auto copy_unchanged = [&](std::int32_t ring) {
        ByteSpan from = entity_at(cl, ring), to = entity_at(cl, cl.parse_entities_num);
        if (from.data() != to.data()) std::memmove(to.data(), from.data(), size);
        cl.parse_entities_num = add(cl.parse_entities_num, 1);
        ++snapshot.num_entities;
    };
    auto stopped = [&]() { return cod2 && msg.overflowed; };
    std::vector<std::uint8_t> old_state(size);
    std::int32_t new_num = stopped() ? kEntityListEnd : msg.read_bits(net::kEntityNumberBits);
    while (new_num != kEntityListEnd) {
        if (msg.cursize < msg.readcount) return GameError::drop("CL_ParsePacketEntities: end of message");
        while (o.number < new_num && !stopped()) {
            copy_unchanged(o.ring);
            ++o.index;
            next_old();
        }
        bool from_old = o.number == new_num;
        if (from_old) {
            ByteSpan from = entity_at(cl, o.ring);
            std::copy(from.begin(), from.end(), old_state.begin());
        } else if (new_num >= 0) {
            auto at = static_cast<std::size_t>(new_num) * size;
            std::copy(cl.baselines.begin() + static_cast<std::ptrdiff_t>(at),
                      cl.baselines.begin() + static_cast<std::ptrdiff_t>(at + size), old_state.begin());
        } else {
            // Call of Duty 2's number of a message that ran out: -1, whose
            // "baseline" is what precedes the baselines in the game's cl.
            old_state = snapshot_tail(cl, kPacketBackup - 1, size);
        }
        bool removed = net::read_delta_entity(msg, tables, old_state, entity_at(cl, cl.parse_entities_num), new_num);
        if (!removed) {
            cl.parse_entities_num = add(cl.parse_entities_num, 1);
            ++snapshot.num_entities;
        }
        if (from_old) {
            ++o.index;
            next_old();
        }
        new_num = stopped() ? kEntityListEnd : msg.read_bits(net::kEntityNumberBits);
    }
    while (o.number != kListEnd && !stopped()) {
        copy_unchanged(o.ring);
        ++o.index;
        next_old();
    }
    return std::nullopt;
}

Status parse_packet_clients(net::Msg& msg, const net::FieldTables& tables, ClientState& cl, std::optional<std::size_t> old,
                            Snapshot& snapshot) {
    bool cod2 = cl.game == Game::CallOfDuty2;
    snapshot.parse_clients_num = cl.parse_clients_num;
    snapshot.num_clients = 0;
    OldList o;
    if (old) {
        o.count = cl.snapshots[*old].num_clients;
        o.start = cl.snapshots[*old].parse_clients_num;
    }
    auto number_at = [&](std::int32_t ring) { return net::word_at(client_at(cl, ring), 0); };
    o.ring = o.start;
    o.number = old && o.count >= 1 ? number_at(o.ring) : kListEnd;
    auto next_old = [&]() {
        if (o.index < o.count) {
            o.ring = add(o.start, o.index);
            o.number = number_at(o.ring);
        } else {
            o.number = kListEnd;
        }
    };
    auto copy_unchanged = [&](std::int32_t ring) {
        ByteSpan from = client_at(cl, ring), to = client_at(cl, cl.parse_clients_num);
        if (from.data() != to.data()) std::memmove(to.data(), from.data(), kClientStateSize);
        cl.parse_clients_num = add(cl.parse_clients_num, 1);
        ++snapshot.num_clients;
    };
    std::array<std::uint8_t, kClientStateSize> old_state{};
    for (;;) {
        if (cod2 && msg.overflowed) break;
        // One more client? Call of Duty 2's message that ran out reads -1
        // here, as if 1, and then a client numbered -1.
        if (msg.read_bit() == 0) break;
        std::int32_t new_num = msg.read_bits(net::kClientNumberBits);
        if (msg.cursize < msg.readcount) return GameError::drop("CL_ParsePacketClients: end of message");
        while (o.number < new_num) {
            copy_unchanged(o.ring);
            ++o.index;
            next_old();
        }
        bool from_old = o.number == new_num;
        ByteView from_view;
        if (from_old) {
            ByteSpan from = client_at(cl, o.ring);
            std::copy(from.begin(), from.end(), old_state.begin());
            from_view = ByteView(old_state);
        }
        bool removed =
            net::read_delta_client(msg, tables, from_old ? &from_view : nullptr, client_at(cl, cl.parse_clients_num), new_num);
        if (!removed) {
            cl.parse_clients_num = add(cl.parse_clients_num, 1);
            ++snapshot.num_clients;
        }
        if (from_old) {
            ++o.index;
            next_old();
        }
    }
    while (o.number != kListEnd && !(cod2 && msg.overflowed)) {
        copy_unchanged(o.ring);
        ++o.index;
        next_old();
    }
    return std::nullopt;
}

Status parse_snapshot(net::Msg& msg, const net::FieldTables& tables, ClientState& cl) {
    Game game = cl.game;
    Snapshot snapshot(game);
    snapshot.server_command_num = cl.server_command_sequence;
    snapshot.server_time = msg.read_long();
    snapshot.message_num = cl.server_message_sequence;
    std::int32_t delta_byte = msg.read_byte();
    snapshot.delta_num = delta_byte == 0 ? -1 : sub(snapshot.message_num, delta_byte);
    snapshot.snap_flags = msg.read_byte();
    std::optional<std::size_t> old;
    if (snapshot.delta_num < 1) {
        cl.demo_waiting = 0;
        snapshot.valid = 1;
    } else {
        std::size_t i = static_cast<std::uint32_t>(snapshot.delta_num) % kPacketBackup;
        const Snapshot& o = cl.snapshots[i];
        bool complete = o.valid != 0 && o.message_num == snapshot.delta_num &&
                        sub(cl.parse_entities_num, o.parse_entities_num) <= kMaxRingDistance &&
                        sub(cl.parse_clients_num, o.parse_clients_num) <= kMaxRingDistance;
        if (complete) snapshot.valid = 1;
        // The originals keep reading from the old snapshot even when it's
        // incomplete (the result is then thrown away).
        old = i;
    }
    if (old) {
        std::vector<std::uint8_t> from = cl.snapshots[*old].player_state;
        ByteView view(from);
        net::read_delta_playerstate(msg, tables, &view, snapshot.player_state);
    } else {
        net::read_delta_playerstate(msg, tables, nullptr, snapshot.player_state);
    }
    if (Status e = parse_packet_entities(msg, tables, cl, old, snapshot)) return e;
    if (Status e = parse_packet_clients(msg, tables, cl, old, snapshot)) return e;
    if (game == Game::CallOfDuty2 && msg.overflowed) return std::nullopt;
    if (snapshot.valid == 0) return std::nullopt;
    // Snapshots skipped since the last one can't be deltas any more.
    std::int32_t skipped = add(cl.snap.message_num, 1);
    if (sub(snapshot.message_num, skipped) >= static_cast<std::int32_t>(kPacketBackup))
        skipped = sub(snapshot.message_num, static_cast<std::int32_t>(kPacketBackup) - 1);
    while (skipped < snapshot.message_num) {
        cl.snapshots[static_cast<std::uint32_t>(skipped) % kPacketBackup].valid = 0;
        skipped = add(skipped, 1);
    }
    cl.old_frame_server_time = cl.snap.server_time;
    cl.snap = snapshot;
    cl.snap.ping = 999;
    std::int32_t command_time = net::word_at(cl.snap.player_state, 0);
    std::int32_t packet = cl.chan.outgoing_sequence;
    for (std::size_t k = 0; k < kPacketBackup; ++k) {
        packet = sub(packet, 1);
        const OutPacket& p = cl.out_packets[static_cast<std::uint32_t>(packet) % kPacketBackup];
        if (p.server_time <= command_time) {
            cl.snap.ping = sub(cl.realtime, p.realtime);
            break;
        }
    }
    cl.snapshots[static_cast<std::uint32_t>(cl.snap.message_num) % kPacketBackup] = cl.snap;
    cl.new_snapshots = 1;
    return std::nullopt;
}

Expected<bool> parse_server_message(net::Msg& msg, const net::MessageTree& tree, const net::FieldTables& tables,
                                    ClientState& cl, ServerMessageParts& parts) {
    Game game = cl.game;
    ServerOps ops = server_ops(game);
    std::vector<std::uint8_t> buffer(net::max_message_length(game), 0);
    std::size_t start = std::min(msg.readcount, msg.data.size());
    std::size_t end = std::max(std::min(msg.cursize, msg.data.size()), start);
    std::size_t count = tree.read_bits_compress(ByteView(msg.data).subspan(start, end - start), buffer);
    net::Msg m(game, buffer);
    m.cursize = count;
    for (;;) {
        if (game == Game::UnitedOffensive && m.cursize < m.readcount)
            return GameError::drop("CL_ParseServerMessage: read past end of server message");
        std::int32_t op = -1;
        if (m.readcount < m.cursize) {
            // (A message can decode to more than the buffer holds: zeros past it.)
            op = m.readcount < m.data.size() ? m.data[m.readcount] : 0;
            ++m.readcount;
            if (op == ops.eof) return true;
        }
        if (op == ops.nop) {
            // nothing
        } else if (op == ops.game_state) {
            if (Status e = parse_game_state(m, tables, cl)) return *e;
            if (!parts.game_state_parsed(cl)) return false;
        } else if (op == ops.server_command) {
            parse_command_string(m, cl);
        } else if (op == ops.download) {
            if (!parts.download(m, cl)) return false;
        } else if (op == ops.snapshot) {
            if (Status e = parse_snapshot(m, tables, cl)) return *e;
        } else {
            return GameError::drop("CL_ParseServerMessage: Illegible server message " + std::to_string(op));
        }
        // Call of Duty 2 stops at a part that read past its message's end
        // or found a field count past its table.
        if (game == Game::CallOfDuty2 && m.overflowed) return true;
    }
}

Expected<PacketOutcome> packet_event(ClientState& cl, const net::NetAddress& from, net::Msg& msg,
                                     const net::MessageTree& tree, const net::FieldTables& tables,
                                     ServerMessageParts& parts) {
    Game game = cl.game;
    if (msg.cursize >= 4 && msg.data.size() >= 4 && msg.data[0] == 0xFF && msg.data[1] == 0xFF && msg.data[2] == 0xFF &&
        msg.data[3] == 0xFF)
        return PacketOutcome{PacketOutcome::Kind::Connectionless};
    if (cl.connection_state < (game == Game::CallOfDuty2 ? 5 : 3)) return PacketOutcome{PacketOutcome::Kind::NotConnected};
    if (msg.cursize < 4) return PacketOutcome{PacketOutcome::Kind::Runt};
    if (!net::same_address(from, cl.chan.remote_address)) return PacketOutcome{PacketOutcome::Kind::NotFromServer};
    cl.last_packet_time = cl.realtime;
    if (!cl.chan.process(msg)) return PacketOutcome{PacketOutcome::Kind::Dropped};
    std::int32_t saved_sequence = cl.server_message_sequence, saved_acknowledge = cl.reliable_acknowledge;
    cl.server_message_sequence = net::word_at(msg.data, 0);
    std::int32_t acknowledge = msg.read_long();
    std::size_t ring = max_reliable_commands(game);
    if (acknowledge < sub(cl.reliable_sequence, static_cast<std::int32_t>(ring))) {
        cl.reliable_acknowledge = cl.reliable_sequence;
        return PacketOutcome{PacketOutcome::Kind::TooOld};
    }
    cl.reliable_acknowledge = acknowledge;
    std::size_t slot = static_cast<std::uint32_t>(acknowledge) % ring * kServerCommandSize;
    std::vector<std::uint8_t> text(cl.reliable_commands.begin() + static_cast<std::ptrdiff_t>(slot),
                                   cl.reliable_commands.begin() + static_cast<std::ptrdiff_t>(slot + kServerCommandSize));
    std::uint8_t key = net::client_decode_key(cl.challenge, cl.server_message_sequence);
    std::size_t start = std::min(msg.readcount, msg.data.size());
    std::size_t end = std::max(std::min(msg.cursize, msg.data.size()), start);
    net::scramble(msg.data.subspan(start, end - start), key, text);
    Expected<bool> parsed = parse_server_message(msg, tree, tables, cl, parts);
    if (!parsed) return parsed.error();
    // One of `parts` stopped the message: nothing after it happens.
    if (!parsed.value()) return PacketOutcome{PacketOutcome::Kind::Read, false};
    if (game == Game::CallOfDuty2 && msg.overflowed) {
        cl.server_message_sequence = saved_sequence;
        cl.reliable_acknowledge = saved_acknowledge;
        return PacketOutcome{PacketOutcome::Kind::Illegible};
    }
    return PacketOutcome{PacketOutcome::Kind::Read, cl.demo_recording != 0 && cl.demo_waiting == 0};
}

namespace {
// The client's message types (clc_ops_e): 3 bits in Call of Duty 2, 2 in United Offensive.
constexpr std::int32_t kClcMove = 0, kClcMoveNoDelta = 1, kClcClientCommand = 2, kClcEof = 3;
}  // namespace

Status write_packet(ClientState& cl, const net::MessageTree& tree, const SendSettings& settings,
                    const std::function<void(const net::NetAddress&, ByteView)>& send) {
    Game game = cl.game;
    if (cl.demo_playing != 0 || sends_nothing_in(game, cl.connection_state)) return std::nullopt;
    std::size_t size = net::usercmd_size(game);
    std::vector<std::uint8_t> previous(size, 0);
    net::set_default_usercmd(game, cl.snap.player_state, previous);
    std::vector<std::uint8_t> buffer(net::max_message_length(game), 0);
    net::Msg msg(game, buffer);
    std::uint32_t bits = game == Game::CallOfDuty2 ? 3 : 2;
    msg.write_byte(static_cast<std::uint8_t>(cl.server_id));
    msg.write_long(cl.server_message_sequence);
    msg.write_long(cl.server_command_sequence);
    std::size_t ring = max_reliable_commands(game);
    for (std::int32_t i = add(cl.reliable_acknowledge, 1); i <= cl.reliable_sequence; i = add(i, 1)) {
        msg.write_bits(kClcClientCommand, bits);
        msg.write_long(i);
        std::size_t slot = static_cast<std::uint32_t>(i) % ring * kServerCommandSize;
        // (A slot holds at most 1023 characters: never too long to write.)
        (void)msg.write_string(cstr::text(ByteView(cl.reliable_commands).subspan(slot, kServerCommandSize)));
    }
    std::int32_t packet_dup = game == Game::CallOfDuty2 ? settings.packet_dup : std::clamp(settings.packet_dup, 0, 5);
    std::int32_t back = sub(sub(cl.chan.outgoing_sequence, packet_dup), 1);
    const OutPacket& old_packet = cl.out_packets[static_cast<std::uint32_t>(back) & 31];
    std::int32_t count = std::min(sub(cl.cmd_number, old_packet.cmd_number), kMaxPacketUsercmds);
    if (count >= 1) {
        bool delta_base = !settings.no_delta && cl.snap.valid != 0 && cl.demo_waiting == 0 &&
                          cl.server_message_sequence == cl.snap.message_num;
        msg.write_bits(delta_base ? kClcMove : kClcMoveNoDelta, bits);
        msg.write_byte(static_cast<std::uint8_t>(count));
        std::int32_t key = cl.checksum_feed ^ cl.server_message_sequence ^
                           shared::hash::hash_key(cl.server_command(cl.server_command_sequence), 32);
        for (std::int32_t k = 0; k < count; ++k) {
            std::int32_t number = add(add(sub(cl.cmd_number, count), 1), k);
            std::size_t at = static_cast<std::uint32_t>(number) % kCmdBackup * size;
            ByteView cmd = ByteView(cl.cmds).subspan(at, size);
            net::write_delta_usercmd_key(msg, key, previous, cmd);
            std::copy(cmd.begin(), cmd.end(), previous.begin());
        }
    }
    msg.write_bits(kClcEof, bits);
    std::size_t length = msg.cursize;
    // The packet: the 9 header bytes as they are, the rest coded.
    std::vector<std::uint8_t> data(9 + 4 * length + 64, 0);
    std::copy(buffer.begin(), buffer.begin() + 9, data.begin());
    std::size_t coded = tree.write_bits_compress(ByteView(buffer).subspan(9, length - 9), ByteSpan(data).subspan(9));
    OutPacket& slot = cl.out_packets[static_cast<std::uint32_t>(cl.chan.outgoing_sequence) & 31];
    slot = {cl.cmd_number, net::word_at(previous, 0), cl.realtime};
    cl.last_packet_sent_time = cl.realtime;
    std::uint8_t key = net::client_encode_key(cl.challenge, cl.server_id, cl.server_message_sequence);
    ByteView text = cl.server_command(cl.server_command_sequence);
    std::vector<std::uint8_t> text_copy(text.begin(), text.end());
    net::scramble(ByteSpan(data).subspan(9, coded), key, text_copy);
    net::Packet packet;
    if (Status e = cl.chan.transmit(ByteView(data).first(9 + coded), packet)) return e;
    send(cl.chan.remote_address, packet.bytes());
    while (cl.chan.unsent_fragments) {
        cl.chan.transmit_next_fragment(packet);
        send(cl.chan.remote_address, packet.bytes());
    }
    return std::nullopt;
}

ClientPacket connectionless_packet(ClientState& cl, const net::NetAddress& from, net::Msg& msg,
                                   const std::function<void(const net::NetAddress&, ByteView)>& send) {
    using Kind = ClientPacket::Kind;
    namespace strings = shared::strings;
    Game game = cl.game;
    bool cod2 = game == Game::CallOfDuty2;
    // MSG_BeginReading, then the -1 (read by hand).
    if (cod2) msg.overflowed = false;
    msg.readcount = 0;
    msg.bit = 0;
    if (msg.cursize < 4) {
        if (cod2) msg.overflowed = true;
    } else {
        msg.readcount = 4;
    }
    if (net::is_punkbuster(msg.data)) return {Kind::PunkBuster, {}};
    std::array<std::uint8_t, net::kMaxStringChars> line_buffer{};
    ByteView line_view = msg.read_string_line(line_buffer);
    std::vector<std::uint8_t> line(line_view.begin(), line_view.end());
    auto tokens = std::make_unique<net::Tokens>();
    tokens->tokenize(game, line, 0);
    ByteView argv0 = tokens->argv(0);
    std::vector<std::uint8_t> command(argv0.begin(), argv0.end());
    std::int32_t limit = cod2 ? strings::kStricmpLimitCod2 : strings::kStricmpLimitUo;
    auto is = [&](const char* name) {
        return strings::stricmpn(ByteView(reinterpret_cast<const std::uint8_t*>(name), std::strlen(name)), command, limit) == 0;
    };
    auto starts_exactly = [&](const char* prefix, std::int32_t n) {
        return strings::strncmp(command, ByteView(reinterpret_cast<const std::uint8_t*>(prefix), std::strlen(prefix)), n) == 0;
    };
    std::int32_t connecting = cod2 ? 3 : 1, challenging = cod2 ? 4 : 2, connected = cod2 ? 5 : 3;
    auto set_server_message = [&](ByteView text) {
        crt::strncpy(ByteSpan(cl.server_message).first(kServerMessageSize - 1), text);
        cl.server_message[kServerMessageSize - 1] = 0;
    };
    if (cod2 && is("v")) return {Kind::Voice, {}};
    if (is("challengeResponse")) {
        if (cl.connection_state != connecting) return {Kind::ChallengeUnwanted, {}};
        cl.challenge = net::atol(tokens->argv(1));
        if (!cod2) cl.only_visible_clients = tokens->argc() < 3 ? 0 : net::atol(tokens->argv(2));
        cl.connection_state = challenging;
        cl.connect_packet_count = 0;
        cl.connect_time = -99999;
        cl.server_address = from;
        return {Kind::ChallengeTaken, {}};
    }
    if (is("connectResponse")) {
        if (cl.connection_state >= connected) return {Kind::DuplicateConnect, {}};
        if (cl.connection_state != challenging) return {Kind::NotConnecting, {}};
        if (!net::same_base_address(game, from, cl.server_address)) return {Kind::ConnectFromElsewhere, {}};
        cl.chan.setup(net::NetSource::Client, from, cl.qport);
        cl.connection_state = connected;
        cl.last_packet_time = cl.realtime;
        cl.last_packet_sent_time = -9999;
        return {Kind::Connected, {}};
    }
    if (is("infoResponse")) return {Kind::Info, {}};
    if (is("statusResponse")) return {Kind::Status, {}};
    if (is("disconnect")) return {Kind::Disconnect, {}};
    if (is("echo")) {
        std::vector<std::uint8_t> packet(net::kMaxStringChars + 8, 0);
        if (auto n = net::out_of_band_print(game, tokens->argv(1), packet)) send(from, ByteView(packet).first(*n));
        return {Kind::Echo, {}};
    }
    if (is("keyAuthorize")) return {Kind::KeyAuthorize, {}};
    if (!cod2 && is("motd")) return {Kind::Motd, {}};
    if (is("print")) {
        std::array<std::uint8_t, net::kBigInfoString> text{};
        set_server_message(msg.read_big_string(text));
        return {Kind::Print, {}};
    }
    if (is("error")) {
        if (cl.connection_state != 0 && net::same_base_address(game, from, cl.server_address)) {
            std::array<std::uint8_t, net::kBigInfoString> text{};
            ByteView s = msg.read_big_string(text);
            return {Kind::ServerError, std::vector<std::uint8_t>(s.begin(), s.end())};
        }
        return {Kind::ErrorIgnored, {}};
    }
    if (is("updateResponse")) return {Kind::Update, {}};
    if (starts_exactly("getserversResponse", 18)) return {Kind::Servers, {}};
    if (starts_exactly("needcdkey", 9)) {
        set_server_message(bytes("EXE_AWAITINGCDKEYAUTH"));
        return {Kind::NeedCdKey, {}};
    }
    if (is("loadingnewmap")) {
        if (!net::same_base_address(game, from, cl.server_address)) return {Kind::NewMapIgnored, {}};
        std::array<std::uint8_t, net::kMaxStringChars> first{};
        ByteView map = msg.read_string_line(first);
        // (Call of Duty 2 keeps 63 characters of it.)
        if (cod2 && map.size() > 63) map = map.first(63);
        std::vector<std::uint8_t> name(map.begin(), map.end());
        std::array<std::uint8_t, net::kMaxStringChars> second{};
        msg.read_string_line(second);
        return {Kind::NewMap, std::move(name)};
    }
    return {Kind::Unknown, {}};
}

Resend check_for_resend(ClientState& cl, const ResendSettings& settings,
                        const std::function<void(const net::NetAddress&, ByteView)>& send) {
    Game game = cl.game;
    bool cod2 = game == Game::CallOfDuty2;
    std::int32_t connecting = cod2 ? 3 : 1, challenging = cod2 ? 4 : 2;
    if (cl.demo_playing != 0 || (cl.connection_state != connecting && cl.connection_state != challenging) ||
        sub(cl.realtime, cl.connect_time) < resend_time(game)) {
        return {};
    }
    cl.connect_packet_count = add(cl.connect_packet_count, 1);
    cl.connect_time = cl.realtime;
    net::NetAddress server = cl.server_address;
    if (cl.connection_state == connecting) {
        bool authorize = settings.lan_authorize || !net::is_lan_address(server, settings.local_addresses);
        std::vector<std::uint8_t> text;
        if (cod2) {
            const char* head = "getchallenge 0 \"";
            text.assign(head, head + std::strlen(head));
            ByteView hash = cstr::text(settings.key_hash);
            text.insert(text.end(), hash.begin(), hash.end());
            text.push_back('"');
        } else {
            const char* word = "getchallenge";
            text.assign(word, word + std::strlen(word));
        }
        std::vector<std::uint8_t> packet(text.size() + 8);
        if (auto n = net::out_of_band_print(game, text, packet)) send(server, ByteView(packet).first(*n));
        return {Resend::Kind::Challenge, authorize};
    }
    std::array<std::uint8_t, net::kMaxStringChars> info{};
    crt::strncpy(ByteSpan(info).first(net::kMaxStringChars - 1), settings.userinfo);
    info[net::kMaxStringChars - 1] = 0;
    auto bytes = [](const std::string& t) { return std::vector<std::uint8_t>(t.begin(), t.end()); };
    auto word = [](const char* t) { return ByteView(reinterpret_cast<const std::uint8_t*>(t), std::strlen(t)); };
    std::vector<std::uint8_t> protocol = bytes(std::to_string(net::protocol_version(game)));
    std::vector<std::uint8_t> challenge = bytes(std::to_string(cl.challenge));
    std::vector<std::uint8_t> qport = bytes(std::to_string(cl.qport));
    // (A setting too long for the string is refused with a warning, as the original prints.)
    {
        (void)shared::info::set_value_for_key(info, word("protocol"), protocol);
        if (cod2) {
            (void)shared::info::set_value_for_key(info, word("challenge"), challenge);
            (void)shared::info::set_value_for_key(info, word("qport"), qport);
        } else {
            (void)shared::info::set_value_for_key(info, word("qport"), qport);
            (void)shared::info::set_value_for_key(info, word("challenge"), challenge);
        }
    }
    const char* head = "connect \"";
    std::vector<std::uint8_t> data(head, head + std::strlen(head));
    ByteView text = cstr::text(info);
    data.insert(data.end(), text.begin(), text.end());
    data.push_back('"');
    if (!cod2) {
        // The text's 1024-byte buffer is right in front of the one it is copied to: it starts over.
        constexpr std::size_t kConnectTextBuffer = 1024;
        for (std::size_t k = kConnectTextBuffer; k < data.size(); ++k) data[k] = data[k - kConnectTextBuffer];
    }
    std::vector<std::uint8_t> packet(2 * data.size() + 64);
    if (auto n = net::out_of_band_data(game, data, packet)) {
        std::size_t length = *n;
        if (!cod2) {
            net::Msg msg(game, packet);
            msg.cursize = length;
            net::compress_adaptive(msg, 12);
            length = msg.cursize;
        }
        send(server, ByteView(packet).first(length));
    }
    return {Resend::Kind::Connect, false};
}

Expected<std::optional<SystemInfo>> system_info_changed(ClientState& cl, bool server_running) {
    ByteView view = cl.game_state.get(kSystemInfo);
    std::vector<std::uint8_t> info(view.begin(), view.end());
    auto word = [](const char* t) { return ByteView(reinterpret_cast<const std::uint8_t*>(t), std::strlen(t)); };
    auto value = [&](const char* key) -> Expected<std::vector<std::uint8_t>> {
        auto v = shared::info::value_for_key(info, word(key));
        if (!v) return v.error();
        return std::vector<std::uint8_t>(v.value().begin(), v.value().end());
    };
    auto id = value("sv_serverid");
    if (!id) return id.error();
    cl.server_id = net::atol(id.value());
    if (cl.demo_playing != 0) return std::optional<SystemInfo>{};
    bool cod2 = cl.game == Game::CallOfDuty2;
    SystemInfo out;
    const char* keys[5] = {"sv_cheats", cod2 ? "sv_iwds" : "sv_paks", cod2 ? "sv_iwdNames" : "sv_pakNames",
                           cod2 ? "sv_referencedIwds" : "sv_referencedPaks",
                           cod2 ? "sv_referencedIwdNames" : "sv_referencedPakNames"};
    std::vector<std::uint8_t> values[5];
    for (int k = 0; k < 5; ++k) {
        auto v = value(keys[k]);
        if (!v) return v.error();
        values[k] = std::move(v.value());
    }
    out.cheats_off = net::atol(values[0]) == 0;
    out.loaded_sums = std::move(values[1]);
    out.loaded_names = std::move(values[2]);
    out.referenced_sums = std::move(values[3]);
    out.referenced_names = std::move(values[4]);
    if (!server_running) {
        // Info_NextPair, inlined: one leading \ skipped, the key up to the next \ (or the end:
        // an empty value), the value up to the next.
        auto at = [&](std::size_t i) { return cstr::at(info, i); };
        std::size_t i = 0;
        for (;;) {
            if (at(i) == '\\') ++i;
            std::size_t start = i;
            while (at(i) != '\\' && at(i) != 0) ++i;
            std::vector<std::uint8_t> key(info.begin() + static_cast<std::ptrdiff_t>(start),
                                          info.begin() + static_cast<std::ptrdiff_t>(i));
            std::vector<std::uint8_t> val;
            if (at(i) == '\\') {
                ++i;
                std::size_t from = i;
                while (at(i) != '\\' && at(i) != 0) ++i;
                val.assign(info.begin() + static_cast<std::ptrdiff_t>(from), info.begin() + static_cast<std::ptrdiff_t>(i));
            }
            if (key.empty()) break;
            out.settings.emplace_back(std::move(key), std::move(val));
        }
    }
    return std::optional<SystemInfo>{std::move(out)};
}

namespace {

// The client time the client game's clock runs on: Call of Duty 2's realtime, United Offensive's sim_time.
std::int32_t clock_now(const ClientState& cl) { return cl.game == Game::CallOfDuty2 ? cl.realtime : cl.sim_time; }

}  // namespace

bool first_snapshot(ClientState& cl) {
    if (cl.snap.snap_flags & kSnapflagNotActive) return false;
    cl.connection_state = playing_states(cl.game).active;
    cl.server_time_delta = sub(cl.snap.server_time, clock_now(cl));
    cl.old_server_time = cl.snap.server_time;
    cl.time_demo_base_time = cl.snap.server_time;
    return true;
}

void adjust_time_delta(ClientState& cl, const ClockSettings& settings) {
    cl.new_snapshots = 0;
    if (cl.demo_playing != 0) return;
    bool cod2 = cl.game == Game::CallOfDuty2;
    std::int32_t snap_time = cl.snap.server_time;
    std::int32_t between = sub(snap_time, cl.old_frame_server_time);
    std::int32_t now = clock_now(cl);
    std::int32_t new_delta = cod2 ? sub(sub(sub(snap_time, now), between), 5) : sub(snap_time, now);
    std::int32_t off = sub(new_delta, cl.server_time_delta);
    bool drift = false;
    if (off <= 0) {
        off = sub(0, off);
    } else if (static_cast<std::uint32_t>(between) <= 500) {
        off = sub(off, between);
        if (cod2) {
            drift = off < 0;
        } else {
            off = std::max(off, 0);
            if (between == 0) {
                cl.server_time_delta = new_delta;
                cl.old_server_time = snap_time;
                cl.server_time = snap_time;
            }
        }
    }
    if (!drift && off > 500) {
        cl.server_time_delta = new_delta;
        cl.old_server_time = snap_time;
        cl.server_time = snap_time;
    } else if (!drift && off > 100) {
        cl.server_time_delta = add(cl.server_time_delta, new_delta) >> 1;
    } else {
        std::uint32_t bits;
        std::memcpy(&bits, &settings.timescale, 4);
        bool normal = cod2 ? settings.timescale == 1.0f : (settings.timescale == 0.0f || bits == 0x3F800000u);
        if (!normal) return;
        if (cl.extrapolated_snapshot != 0) {
            cl.extrapolated_snapshot = 0;
            cl.server_time_delta = sub(cl.server_time_delta, 2);
        } else if (!cod2) {
            cl.server_time_delta = add(cl.server_time_delta, 1);
        } else if (new_delta > cl.server_time_delta) {
            cl.server_time_delta += 1;
        } else if (new_delta < cl.server_time_delta) {
            cl.server_time_delta -= 1;
        }
    }
}

Status set_cgame_time(ClientState& cl, const ClockSettings& settings, ClockParts& parts) {
    Game game = cl.game;
    bool cod2 = game == Game::CallOfDuty2;
    PlayingStates states = playing_states(game);
    auto first = [&] {
        if (first_snapshot(cl)) parts.became_active(cl);
    };
    if (cl.connection_state != states.active) {
        if (cl.connection_state != states.primed) return std::nullopt;
        if (cl.demo_playing != 0) {
            if (cl.first_demo_frame_skipped == 0) {
                cl.first_demo_frame_skipped = 1;
                return std::nullopt;
            }
            if (!parts.read_demo_message(cl)) return std::nullopt;
        }
        if (cl.new_snapshots != 0) {
            cl.new_snapshots = 0;
            first();
        }
        if (cl.connection_state != states.active) return std::nullopt;
    }
    if (cl.snap.valid == 0) return GameError::drop("CL_SetCGameTime: !cl.snap.valid");
    if (settings.client_paused && settings.server_paused && settings.server_running) return std::nullopt;
    if (cl.snap.server_time < cl.frame_server_time) {
        std::int32_t limit = cod2 ? INT32_MAX : shared::strings::kStricmpLimitUo;
        const char* local = "localhost";
        if (shared::strings::stricmpn(cl.server_name, ByteView(reinterpret_cast<const std::uint8_t*>(local), 9), limit) == 0) {
            first();
        } else {
            return GameError::drop("cl.snap.serverTime < cl.oldFrameServerTime");
        }
    }
    std::int32_t snap_time = cl.snap.server_time;
    cl.frame_server_time = snap_time;
    if (!(cl.demo_playing != 0 && settings.freeze_demo)) {
        std::int32_t nudge = cod2 ? 0 : std::clamp(settings.time_nudge, -30, 30);
        std::int32_t now = add(clock_now(cl), cl.server_time_delta);
        cl.server_time = std::max(sub(now, nudge), cl.old_server_time);
        cl.old_server_time = cl.server_time;
        if (now >= sub(snap_time, 5)) cl.extrapolated_snapshot = 1;
    }
    if (cl.new_snapshots != 0) adjust_time_delta(cl, settings);
    if (cl.demo_playing == 0) return std::nullopt;
    bool time_demo = cod2 ? cl.time_demo != 0 : settings.time_demo;
    if (time_demo && !parts.time_demo_frame(cl)) return std::nullopt;
    while (cl.server_time >= cl.snap.server_time) {
        if (!parts.read_demo_message(cl) || cl.connection_state != states.active) return std::nullopt;
    }
    return std::nullopt;
}

void finish_move(const ClientState& cl, ByteSpan cmd) {
    constexpr float kAngle16Scale = 65536.0f / 360.0f;
    bool cod2 = cl.game == Game::CallOfDuty2;
    if (cod2) {
        cmd[8] = static_cast<std::uint8_t>(cl.user_cmd_weapon);
        cmd[9] = static_cast<std::uint8_t>(cl.user_cmd_offhand);
    } else {
        cmd[6] = static_cast<std::uint8_t>(cl.user_cmd_weapon);
    }
    std::int32_t snap_time = cl.snap.server_time;
    std::int32_t time = cl.server_time;
    if (sub(time, snap_time) > 5000) time = add(snap_time, 5000);
    net::set_word(cmd, 0, time);
    std::uint32_t angles = cod2 ? 0xC : 8;
    for (std::uint32_t i = 0; i < 3; ++i) {
        float a = cl.cgame_angles[i], b = cl.view_angles[i];
        double product = cod2 ? static_cast<double>((a + b) * kAngle16Scale)
                              : (static_cast<double>(a) + static_cast<double>(b)) * static_cast<double>(kAngle16Scale);
        net::set_word(cmd, angles + 4 * i, static_cast<std::int32_t>(net::ftol2(product)) & 0xFFFF);
    }
}

void create_new_commands(ClientState& cl, std::int32_t frame_time, InputTiming& timing,
                         const std::function<std::vector<std::uint8_t>(ClientState&, const InputTiming&)>& create) {
    Game game = cl.game;
    if (cl.connection_state < playing_states(game).primed) return;
    if (game == Game::UnitedOffensive) {
        std::int32_t msec = sub(frame_time, timing.old_frame_time);
        timing.frame_msec = static_cast<std::uint32_t>(msec) > 200 ? 200 : msec;
        timing.old_frame_time = frame_time;
    }
    cl.cmd_number = add(cl.cmd_number, 1);
    std::vector<std::uint8_t> cmd = create(cl, timing);
    std::size_t size = net::usercmd_size(game);
    std::size_t at = (static_cast<std::uint32_t>(cl.cmd_number) % kCmdBackup) * size;
    std::copy(cmd.begin(), cmd.begin() + static_cast<std::ptrdiff_t>(size), cl.cmds.begin() + static_cast<std::ptrdiff_t>(at));
}

bool ready_to_send_packet(const ClientState& cl, bool downloading,
                          std::span<const std::array<std::uint8_t, 4>> local_addresses, std::int32_t& max_packets) {
    Game game = cl.game;
    if (cl.demo_playing != 0 || sends_nothing_in(game, cl.connection_state)) return false;
    std::int32_t since = sub(cl.realtime, cl.last_packet_sent_time);
    if (downloading && since < 50) return false;
    PlayingStates states = playing_states(game);
    if (cl.connection_state != states.active && cl.connection_state != states.primed && !downloading && since < 1000) {
        return false;
    }
    const net::NetAddress& to = cl.chan.remote_address;
    if (to.kind == net::AddressType::Loopback || net::is_lan_address(to, local_addresses)) return true;
    if (game == Game::UnitedOffensive) {
        if (max_packets < 15) {
            max_packets = 15;
        } else if (max_packets > 100) {
            max_packets = 100;
        }
    }
    const OutPacket& old = cl.out_packets[static_cast<std::uint32_t>(sub(cl.chan.outgoing_sequence, 1)) & (kPacketBackup - 1)];
    // (Call of Duty 2's dvar keeps cl_maxpackets above 0; the original divides by it.)
    std::int32_t divisor = std::max(max_packets, 1);
    std::int32_t interval = 1000 / divisor;
    return sub(cl.realtime, old.realtime) >= interval;
}

Queued add_reliable_command(ClientState& cl, ByteView command) {
    std::size_t ring = max_reliable_commands(cl.game);
    if (sub(cl.reliable_sequence, cl.reliable_acknowledge) > static_cast<std::int32_t>(ring)) {
        return {GameError::drop("EXE_ERR_CLIENT_CMD_OVERFLOW"), std::nullopt};
    }
    cl.reliable_sequence = add(cl.reliable_sequence, 1);
    std::size_t slot = static_cast<std::uint32_t>(cl.reliable_sequence) % ring * kServerCommandSize;
    ByteSpan out = ByteSpan(cl.reliable_commands).subspan(slot, kServerCommandSize);
    return {std::nullopt, net::write_reliable_command_to_buffer(cl.game, command, out)};
}

Status send_pure_checksums(ClientState& cl, std::span<const fs::SearchPath> search_paths, std::int32_t feed,
                           std::int32_t fake) {
    std::vector<std::uint8_t> checksums = fs::referenced_pure_checksums(cl.game, search_paths, feed, fake);
    std::array<std::uint8_t, kServerCommandSize> command{};
    crt::sprintf(command, kServerCommandSize, bytes("Va "));
    // Never fails: "Va " is shorter than the buffer.
    shared::strings::strcat(command, checksums);
    // The original spells it "Va " so that "cp " isn't in the executable.
    command[0] = static_cast<std::uint8_t>(command[0] + 13);
    command[1] = static_cast<std::uint8_t>(command[1] + 15);
    return add_reliable_command(cl, command).error;
}

Expected<std::optional<std::size_t>> configstring_modified(ClientState& cl, ByteView index_word, ByteView text) {
    std::int32_t index = net::atol(index_word);
    if (index < 0 || index >= static_cast<std::int32_t>(kMaxConfigStrings)) {
        return GameError::drop("configstring > MAX_CONFIGSTRINGS");
    }
    std::size_t at = static_cast<std::size_t>(index);
    text = cstr::text(text);
    ByteView current = cl.game_state.get(at);
    if (std::equal(current.begin(), current.end(), text.begin(), text.end())) return std::optional<std::size_t>();
    GameStateStrings old = cl.game_state;
    GameStateStrings& state = cl.game_state;
    std::fill(state.offsets.begin(), state.offsets.end(), 0);
    std::fill(state.data.begin(), state.data.end(), 0);
    state.data_count = 1;
    std::size_t limit = max_game_state_chars(cl.game);
    for (std::size_t i = 0; i < kMaxConfigStrings; ++i) {
        ByteView string = i == at ? text : old.get(i);
        if (string.empty()) continue;
        std::size_t start = static_cast<std::size_t>(state.data_count);
        if (start + string.size() + 1 > limit) return GameError::drop("MAX_GAMESTATE_CHARS exceeded");
        state.offsets[i] = state.data_count;
        std::copy(string.begin(), string.end(), state.data.begin() + static_cast<std::ptrdiff_t>(start));
        state.data[start + string.size()] = 0;
        state.data_count += static_cast<std::int32_t>(string.size()) + 1;
    }
    return std::optional<std::size_t>(at);
}

Expected<ServerCommand> get_server_command(ClientState& cl, std::int32_t number, net::Tokens& tokens,
                                           std::span<std::uint8_t, kBigConfigStringSize> big) {
    Game game = cl.game;
    std::int32_t ring = static_cast<std::int32_t>(max_reliable_commands(game));
    ServerCommand out;
    if (number <= sub(cl.server_command_sequence, ring)) {
        if (cl.demo_playing != 0) {
            out.kind = ServerCommand::Kind::Kept;
            return out;
        }
        return GameError::drop("CL_GetServerCommand: \x14" "EXE_ERR_RELIABLE_CYCLED_OUT");
    }
    if (number > cl.server_command_sequence) return GameError::drop("CL_GetServerCommand: \x14" "EXE_ERR_NOT_RECEIVED");
    cl.last_executed_server_command = number;
    ByteView command = cl.server_command(number);
    std::vector<std::uint8_t> text(command.begin(), command.end());
    bool rescanning = false;
    for (;;) {
        tokens.tokenize(game, text, 0);
        ByteView first = tokens.argv(0);
        std::uint8_t letter = first.empty() ? 0 : first[0];
        switch (letter) {
            case 'B':
            case 'n':
                std::fill(cl.cmds.begin(), cl.cmds.end(), 0);
                out.kind = ServerCommand::Kind::MapRestart;
                return out;
            case 'd': {
                tokens.tokenize(game, text, 3);
                ByteView w1 = tokens.argv(1), w2 = tokens.argv(2);
                std::vector<std::uint8_t> index(w1.begin(), w1.end()), string(w2.begin(), w2.end());
                auto changed = configstring_modified(cl, index, string);
                if (!changed) return changed.error();
                tokens.tokenize(game, text, 3);
                out.kind = ServerCommand::Kind::ConfigString;
                out.changed = changed.value();
                return out;
            }
            case 'w': {
                out.kind = ServerCommand::Kind::Disconnected;
                if (tokens.argc() >= 2) {
                    ByteView reason = tokens.argv(1);
                    out.reason = std::vector<std::uint8_t>(reason.begin(), reason.end());
                }
                if (game == Game::CallOfDuty2 && tokens.argc() >= 3) {
                    ByteView mark = tokens.argv(2);
                    out.localized_reason = !(mark.size() == 2 && (mark[0] | 0x20) == 'p' && (mark[1] | 0x20) == 'b');
                }
                return out;
            }
            case 'x': {
                tokens.tokenize(game, text, 3);
                std::vector<std::uint8_t> formatted = {'d', ' '};
                ByteView w1 = tokens.argv(1), w2 = tokens.argv(2);
                formatted.insert(formatted.end(), w1.begin(), w1.end());
                formatted.push_back(' ');
                formatted.insert(formatted.end(), w2.begin(), w2.end());
                crt::sprintf(big, kBigConfigStringSize, formatted);
                out.kind = ServerCommand::Kind::Kept;
                return out;
            }
            case 'y':
            case 'z': {
                tokens.tokenize(game, text, 3);
                ByteView part = tokens.argv(2);
                std::size_t length = cstr::len(ByteView(big.data(), big.size()));
                // y may fill the buffer to its last byte, z one less.
                std::size_t room = letter == 'y' ? 0 : 1;
                if (length + part.size() + room >= kBigConfigStringSize) {
                    return GameError::drop("bcs exceeded BIG_INFO_STRING");
                }
                std::copy(part.begin(), part.end(), big.begin() + static_cast<std::ptrdiff_t>(length));
                big[length + part.size()] = 0;
                if (letter == 'y') {
                    out.kind = ServerCommand::Kind::Kept;
                    return out;
                }
                if (rescanning && part.empty()) {  // the same text again: the original's endless loop
                    out.kind = ServerCommand::Kind::Endless;
                    return out;
                }
                rescanning = true;
                text.assign(big.begin(), big.begin() + static_cast<std::ptrdiff_t>(cstr::len(ByteView(big.data(), big.size()))));
                break;
            }
            default:
                out.kind = ServerCommand::Kind::ClientGame;
                return out;
        }
    }
}

Status check_timeout(ClientState& cl, const TimeoutSettings& settings) {
    bool paused = settings.client_paused && settings.server_paused;
    double quiet = static_cast<double>(sub(cl.realtime, cl.last_packet_time));
    Game game = cl.game;
    // Call of Duty 2's x87 rounds the product to a float's precision; United Offensive's is exact.
    auto longer = [&](float seconds) {
        return game == Game::CallOfDuty2 ? quiet > static_cast<double>(seconds * 1000.0f)
                                         : quiet > static_cast<double>(seconds) * 1000.0;
    };
    std::int32_t state = cl.connection_state;
    bool connected = game == Game::CallOfDuty2 ? state >= 5 : (state >= 3 && state != 7 && state != 8);
    if (!paused && connected && longer(settings.timeout)) {
        cl.timeout_count = add(cl.timeout_count, 1);
        if (cl.timeout_count > 5) return GameError::drop("EXE_ERR_SERVER_TIMEOUT");
        return std::nullopt;
    }
    if (game == Game::CallOfDuty2 && (state == 3 || state == 4) && cl.last_packet_time > 0 &&
        longer(settings.connect_timeout)) {
        return GameError::drop("EXE_ERR_SERVER_TIMEOUT");
    }
    cl.timeout_count = 0;
    return std::nullopt;
}

UserinfoSent check_userinfo(ClientState& cl, bool paused, std::int32_t& modified_flags, ByteView userinfo) {
    std::int32_t joining = cl.game == Game::CallOfDuty2 ? 4 : 2;
    UserinfoSent out;
    if (cl.connection_state < joining || paused || (modified_flags & 2) == 0) return out;
    modified_flags &= ~2;
    std::vector<std::uint8_t> command = {'u', 's', 'e', 'r', 'i', 'n', 'f', 'o', ' ', '"'};
    ByteView text = cstr::text(userinfo);
    command.insert(command.end(), text.begin(), text.end());
    command.push_back('"');
    out.sent = true;
    // (Call of Duty 2's va takes at most 1023 characters: longer settings are a dropping error there.)
    auto formatted = shared::strings::va(cl.game, std::move(command));
    if (!formatted) {
        out.queued.error = formatted.error();
        return out;
    }
    out.queued = add_reliable_command(cl, formatted.value());
    return out;
}

}  // namespace iw_pc::client
