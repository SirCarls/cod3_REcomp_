// Requests for iw_pc::net::game_state (the C++ version): what the server
// writes into a client's messages (specs/net_game_state.py).

#include <algorithm>
#include <cstring>

#include "iw_pc/net/game_state.h"
#include "iw_pc/net/netchan.h"
#include "msg_arg.h"
#include "net_common.h"

namespace pc_check {

using iw_pc::ByteView;
using iw_pc::Game;
namespace net = iw_pc::net;

namespace {

// The old and the new list from a ring token (specs/net_game_state.py RingLists): the old
// count (u32 big-endian), then the states.
bool ring_lists(ByteView data, std::size_t size, std::vector<ByteView>& old, std::vector<ByteView>& now) {
    if (data.size() < 4 || (data.size() - 4) % size != 0) return false;
    std::size_t old_count = (static_cast<std::size_t>(data[0]) << 24) | (data[1] << 16) | (data[2] << 8) | data[3];
    std::size_t count = (data.size() - 4) / size;
    if (old_count > count) return false;
    for (std::size_t k = 0; k < count; ++k) (k < old_count ? old : now).push_back(data.subspan(4 + k * size, size));
    return true;
}

// The check's connection (specs/net_game_state.py): each fragment sent is one of `left`; a
// message too big for one packet leaves `after` of them. Both calls are logged as the
// original's stand-ins record them.
class CheckLink : public net::GameStateLink {
public:
    CheckLink(std::int32_t left, std::int32_t after) : left(left), after(after) {}
    bool transmit_next_fragment() override {
        log.call("Netchan_TransmitNextFragment", {});
        left -= 1;
        return left > 0;
    }
    bool send_message(const net::Msg& msg) override {
        std::size_t end = std::min(msg.cursize, msg.data.size());
        log.call("SV_SendMessageToClient", {CallLog::i(msg.overflowed ? 1 : 0), CallLog::s(msg.data.first(end))});
        if (after > 0) {
            left = after;
            return true;
        }
        return false;
    }
    CallLog log;
    std::int32_t left, after;
};

// The config strings from their token (index u16, length u32, text each), all 2048.
bool config_strings(ByteView data, std::vector<Bytes>& strings) {
    strings.assign(net::kMaxConfigStrings, Bytes());
    std::size_t at = 0;
    while (at < data.size()) {
        if (at + 6 > data.size()) return false;
        std::size_t index = (static_cast<std::size_t>(data[at]) << 8) | data[at + 1];
        std::size_t n = (static_cast<std::size_t>(data[at + 2]) << 24) | (static_cast<std::size_t>(data[at + 3]) << 16) |
                        (static_cast<std::size_t>(data[at + 4]) << 8) | data[at + 5];
        if (index >= strings.size() || at + 6 + n > data.size()) return false;
        strings[index].assign(data.begin() + static_cast<std::ptrdiff_t>(at + 6),
                              data.begin() + static_cast<std::ptrdiff_t>(at + 6 + n));
        at += 6 + n;
    }
    return true;
}

}  // namespace

std::optional<Answer> dispatch_game_state(Game game, const std::string& f, Args& args) {
    if (f == "SV_SendClientGameState") {
        // client, sv.configstrings, the baselines, sv.checksumFeed, timescale's integer, the call
        // log -> the first warning; the client's changed fields
        std::string error;
        const net::FieldTables* tables = field_tables(game, error);
        if (!tables) return Answer::fail(error);
        Bytes* client_buffer = args.buffer(0);
        if (!client_buffer) return Answer::fail("NULL client");
        Bytes token = *client_buffer;
        if (token.size() < 32) return Answer::fail("short client token");
        std::int32_t numbers[8];
        for (int k = 0; k < 8; ++k) numbers[k] = be_word(token, static_cast<std::size_t>(k));
        std::size_t ring = net::max_reliable_commands(game);
        std::vector<ByteView> texts;
        std::size_t at = 32;
        for (std::size_t k = 0; k < ring; ++k) {
            if (at + 2 > token.size()) return Answer::fail("short client token");
            std::size_t n = (static_cast<std::size_t>(token[at]) << 8) | token[at + 1];
            if (at + 2 + n > token.size()) return Answer::fail("short client token");
            texts.push_back(ByteView(token).subspan(at + 2, n));
            at += 2 + n;
        }
        std::vector<Bytes> strings;
        if (!config_strings(args.bytes(1), strings)) return Answer::fail("bad config string token");
        std::vector<ByteView> string_views(strings.begin(), strings.end());
        Bytes baselines = args.region(2);
        std::size_t size = game == Game::CallOfDuty2 ? 0xF0 : 0xF4;
        std::size_t stride = game == Game::CallOfDuty2 ? 0x174 : 0x180;
        std::vector<ByteView> bases;
        for (std::size_t k = 0; k < 1024; ++k)
            bases.push_back(k * stride + size <= baselines.size() ? ByteView(baselines).subspan(k * stride, size) : ByteView());
        net::GameState state;
        state.last_client_command = numbers[3];
        state.commands = net::ServerCommands{numbers[7], numbers[6], texts};
        state.config_strings = string_views;
        state.baselines = bases;
        state.client_num = numbers[4];
        state.checksum_feed = args.i32(3);
        // (Whatever pureAuthentic, gamestateMessageNum and reliableSent held: all three are set.)
        net::GameStateClient client;
        client.state = numbers[0];
        client.unsent_fragments = numbers[1] > 0;
        client.outgoing_sequence = numbers[2];
        CheckLink link(numbers[1], numbers[5]);
        Bytes buffer(net::game_state_buffer_size(game), 0);
        auto refused = net::send_client_game_state(game, *tables, client, state, args.i32(4), buffer, link);
        client_buffer = args.buffer(0);
        client_buffer->assign(20, 0);
        std::int32_t out[5] = {client.state, client.pure_authentic, client.gamestate_message_num,
                               client.unsent_fragments ? 1 : 0, client.reliable_sent};
        for (std::size_t k = 0; k < 5; ++k) set_be_word(*client_buffer, k, out[k]);
        Bytes* log = args.buffer(5);
        if (!log) return Answer::fail("NULL call log");
        *log = link.log.out;
        if (!refused) return Answer(Ret(Printed{}));
        const char* format = net::message_format(*refused);
        return Answer(Ret(Printed{Bytes(format, format + std::strlen(format))}));
    }
    if (f == "SV_WriteSnapshotToClient") {
        // msg, client, the rings, the baselines, sv.time, the map-restart bit, svs.nextSnapshotEntities, sv_padPackets
        std::string error;
        const net::FieldTables* tables = field_tables(game, error);
        if (!tables) return Answer::fail(error);
        Bytes* client_buffer = args.buffer(1);
        if (!client_buffer) return Answer::fail("NULL client");
        Bytes token = *client_buffer;
        if (token.size() < 20) return Answer::fail("short client token");
        std::size_t ps_size = net::player_state_size(game);
        std::vector<Bytes> players;
        std::vector<net::SnapshotFrame> frames(32);
        std::size_t at = 20;
        players.reserve(32);
        for (std::size_t k = 0; k < 32; ++k) {
            if (at + 20 > token.size()) return Answer::fail("short client token");
            std::int32_t n[5];
            for (int j = 0; j < 5; ++j) n[j] = be_word(Bytes(token.begin() + at + 4 * j, token.begin() + at + 4 * j + 4), 0);
            std::size_t length = static_cast<std::uint32_t>(n[4]);
            if (at + 20 + length > token.size()) return Answer::fail("short client token");
            players.push_back(length ? Bytes(token.begin() + at + 20, token.begin() + at + 20 + length) : Bytes(ps_size, 0));
            frames[k].num_entities = n[0];
            frames[k].num_clients = n[1];
            frames[k].first_entity = n[2];
            frames[k].first_client = n[3];
            at += 20 + length;
        }
        for (std::size_t k = 0; k < 32; ++k) frames[k].player = players[k];
        net::SnapshotClient client;
        client.state = be_word(token, 0);
        client.active = be_word(token, 1) != 0;
        client.delta_message = be_word(token, 2);
        client.rate_delayed = be_word(token, 3) != 0;
        client.outgoing_sequence = be_word(token, 4);
        client.frames = frames;
        ByteView rings = args.bytes(2);
        if (rings.size() < 8) return Answer::fail("bad rings token");
        auto u32 = [&](std::size_t i) {
            return (static_cast<std::size_t>(rings[i]) << 24) | (rings[i + 1] << 16) | (rings[i + 2] << 8) | rings[i + 3];
        };
        std::size_t entity_count = u32(0), client_count = u32(4);
        std::size_t entity_size = game == Game::CallOfDuty2 ? 0xF0 : 0xF4;
        if (rings.size() != 8 + entity_count * entity_size + client_count * 0x5C) return Answer::fail("bad rings token");
        std::vector<ByteView> entities, clients;
        for (std::size_t k = 0; k < entity_count; ++k) entities.push_back(rings.subspan(8 + k * entity_size, entity_size));
        std::size_t clients_at = 8 + entity_count * entity_size;
        for (std::size_t k = 0; k < client_count; ++k) clients.push_back(rings.subspan(clients_at + k * 0x5C, 0x5C));
        Bytes baselines = args.region(3);
        std::size_t stride = game == Game::CallOfDuty2 ? 0x174 : 0x180;
        net::SnapshotRings ring_view;
        ring_view.entities = entities;
        ring_view.clients = clients;
        ring_view.next_entities = args.i32(6);
        ring_view.baseline = [&](std::int32_t n) {
            return ByteView(baselines).subspan(static_cast<std::size_t>(std::clamp(n, 0, 1023)) * stride, entity_size);
        };
        std::int32_t time = args.i32(4), bit = args.i32(5), padding = args.i32(7);
        error = with_msg(args, 0, game, [&](net::Msg& m) {
            net::write_snapshot_to_client(m, *tables, client, ring_view, time, bit, padding);
        });
        if (!error.empty()) return Answer::fail(error);
        client_buffer = args.buffer(1);
        client_buffer->assign(4, 0);
        set_be_word(*client_buffer, 0, client.active ? 1 : 0);
        return nothing();
    }
    if (f == "SV_EmitPacketEntities" || f == "SV_EmitPacketClients") {
        // msg, four numbers (the lists' places, as each game passes them), the lists[, the baselines]
        std::string error;
        const net::FieldTables* tables = field_tables(game, error);
        if (!tables) return Answer::fail(error);
        bool entities = f == "SV_EmitPacketEntities";
        std::size_t size = entities ? (game == Game::CallOfDuty2 ? 0xF0 : 0xF4) : 0x5C;
        std::vector<ByteView> old, now;
        if (!ring_lists(args.bytes(5), size, old, now)) return Answer::fail("bad ring token");
        if (entities) {
            Bytes baselines = args.region(6);
            std::size_t stride = game == Game::CallOfDuty2 ? 0x174 : 0x180;
            auto baseline = [&](std::int32_t n) {
                std::size_t at = static_cast<std::size_t>(std::clamp(n, 0, 1023)) * stride;
                return ByteView(baselines).subspan(at, size);
            };
            error = with_msg(args, 0, game, [&](net::Msg& m) { net::write_packet_entities(m, *tables, old, now, baseline); });
        } else {
            error = with_msg(args, 0, game, [&](net::Msg& m) { net::write_packet_clients(m, *tables, old, now); });
        }
        if (!error.empty()) return Answer::fail(error);
        return nothing();
    }
    if (f == "SV_UpdateServerCommandsToClient") {  // msg, client -> the first warning; the client's last-sent number
        Bytes* client = args.buffer(1);
        if (!client || client->size() < 8) return Answer::fail("short client token");
        Bytes data = *client;
        std::int32_t sequence = be_word(data, 0), acknowledge = be_word(data, 1);
        std::size_t ring = net::max_reliable_commands(game);
        std::vector<ByteView> texts;
        std::size_t at = 8;
        for (std::size_t k = 0; k < ring; ++k) {
            if (at + 2 > data.size()) return Answer::fail("short client token");
            std::size_t n = (static_cast<std::size_t>(data[at]) << 8) | data[at + 1];
            if (at + 2 + n > data.size()) return Answer::fail("short client token");
            texts.push_back(ByteView(data).subspan(at + 2, n));
            at += 2 + n;
        }
        net::ServerCommands commands{acknowledge, sequence, texts};
        std::optional<net::StringTooLong> refused;
        std::string error = with_msg(args, 0, game, [&](net::Msg& m) { refused = net::write_server_commands(m, commands); });
        if (!error.empty()) return Answer::fail(error);
        // (The original then records the commands as sent: reliableSent = reliableSequence.)
        client->assign(4, 0);
        set_be_word(*client, 0, sequence);
        if (!refused) return Answer(Ret(Printed{}));
        const char* format = net::message_format(*refused);
        return Answer(Ret(Printed{Bytes(format, format + std::strlen(format))}));
    }
    return std::nullopt;
}

}  // namespace pc_check
