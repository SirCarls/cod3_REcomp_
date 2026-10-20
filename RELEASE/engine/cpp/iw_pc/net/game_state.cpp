#include "iw_pc/net/game_state.h"

#include <algorithm>
#include <vector>

namespace iw_pc::net {

std::optional<StringTooLong> write_server_commands(Msg& msg, const ServerCommands& commands) {
    ServerOps ops = server_ops(msg.game);
    std::optional<StringTooLong> result;
    std::size_t slots = commands.texts.empty() ? 1 : commands.texts.size();
    // Wrapping 32-bit counting, as the original's registers do it.
    std::uint32_t i = static_cast<std::uint32_t>(commands.acknowledged) + 1;
    while (static_cast<std::int32_t>(i) <= commands.sequence) {
        msg.write_byte(ops.server_command);
        msg.write_long(static_cast<std::int32_t>(i));
        std::size_t slot = i % slots;
        ByteView text = slot < commands.texts.size() ? commands.texts[slot] : ByteView();
        auto refused = msg.write_string(text);
        if (refused && !result) result = refused;
        i += 1;
    }
    return result;
}

std::optional<StringTooLong> write_game_state(Msg& msg, const FieldTables& tables, const GameState& state) {
    // Bytes of an entity state the baselines are coded against (all zero): at least the
    // larger game's entity state (United Offensive: 0xF4).
    static const std::uint8_t kZeroEntity[0x100] = {};
    ServerOps ops = server_ops(msg.game);
    msg.write_long(state.last_client_command);
    std::optional<StringTooLong> result = write_server_commands(msg, state.commands);
    msg.write_byte(ops.game_state);
    msg.write_long(state.commands.sequence);
    std::size_t count = std::min(state.config_strings.size(), kMaxConfigStrings);
    for (std::size_t index = 0; index < count; ++index) {
        ByteView text = state.config_strings[index];
        if (text.empty() || text[0] == 0) continue;
        msg.write_byte(ops.config_string);
        msg.write_short(static_cast<std::int16_t>(index));
        auto refused = msg.write_big_string(text);
        if (refused && !result) result = refused;
    }
    for (const ByteView& baseline : state.baselines) {
        if (word_at(baseline, 0) == 0) continue;
        msg.write_byte(ops.baseline);
        write_delta_entity(msg, tables, ByteView(kZeroEntity, sizeof kZeroEntity), &baseline, true);
    }
    msg.write_byte(ops.eof);
    msg.write_long(state.client_num);
    msg.write_long(state.checksum_feed);
    msg.write_byte(ops.eof);
    return result;
}

std::optional<StringTooLong> send_client_game_state(Game game, const FieldTables& tables, GameStateClient& client,
                                                    const GameState& state, std::int32_t timescale, ByteSpan buffer,
                                                    GameStateLink& link) {
    while (client.state != 0 && client.unsent_fragments) client.unsent_fragments = link.transmit_next_fragment();
    client.state = 3;
    client.pure_authentic = 0;
    client.gamestate_message_num = client.outgoing_sequence;
    Msg msg(game, buffer.first(std::min(game_state_buffer_size(game), buffer.size())));
    std::optional<StringTooLong> result = write_game_state(msg, tables, state);
    client.reliable_sent = state.commands.sequence;
    if (link.send_message(msg)) client.unsent_fragments = true;
    if (game == Game::UnitedOffensive && timescale == 0 && client.state != 0 && client.unsent_fragments)
        client.unsent_fragments = link.transmit_next_fragment();
    return result;
}

void write_packet_entities(Msg& msg, const FieldTables& tables, std::span<const ByteView> from,
                           std::span<const ByteView> to, const std::function<ByteView(std::int32_t)>& baseline) {
    std::size_t old = 0, now = 0;
    while (old < from.size() || now < to.size()) {
        bool have_old = old < from.size(), have_new = now < to.size();
        std::int32_t old_number = have_old ? word_at(from[old], 0) : 0;
        std::int32_t new_number = have_new ? word_at(to[now], 0) : 0;
        if (have_old && have_new && old_number == new_number) {
            write_delta_entity(msg, tables, from[old], &to[now], false);
            ++old;
            ++now;
        } else if (have_new && (!have_old || new_number < old_number)) {
            write_delta_entity(msg, tables, baseline(new_number), &to[now], true);
            ++now;
        } else {
            write_delta_entity(msg, tables, from[old], nullptr, true);
            ++old;
        }
    }
    msg.write_bits(kEntityListEnd, kEntityNumberBits);
}

void write_packet_clients(Msg& msg, const FieldTables& tables, std::span<const ByteView> from,
                          std::span<const ByteView> to) {
    std::size_t old = 0, now = 0;
    while (old < from.size() || now < to.size()) {
        bool have_old = old < from.size(), have_new = now < to.size();
        std::int32_t old_number = have_old ? word_at(from[old], 0) : 0;
        std::int32_t new_number = have_new ? word_at(to[now], 0) : 0;
        if (have_old && have_new && old_number == new_number) {
            write_delta_client(msg, tables, &from[old], &to[now], false);
            ++old;
            ++now;
        } else if (have_new && (!have_old || new_number < old_number)) {
            write_delta_client(msg, tables, nullptr, &to[now], true);
            ++now;
        } else {
            write_delta_client(msg, tables, &from[old], nullptr, true);
            ++old;
        }
    }
    msg.write_bit0();
}

namespace {

// A frame's run in a ring (each index modulo the ring's size).
std::vector<ByteView> run(std::span<const ByteView> ring, std::int32_t first, std::int32_t count) {
    std::vector<ByteView> out;
    std::int64_t size = ring.empty() ? 1 : static_cast<std::int64_t>(ring.size());
    for (std::int32_t k = 0; k < count; ++k) {
        std::int64_t i = (static_cast<std::int64_t>(static_cast<std::int32_t>(static_cast<std::uint32_t>(first) + k)) % size + size) % size;
        if (static_cast<std::size_t>(i) < ring.size()) out.push_back(ring[static_cast<std::size_t>(i)]);
    }
    return out;
}

}  // namespace

std::optional<DeltaRefused> write_snapshot_to_client(Msg& msg, const FieldTables& tables, SnapshotClient& client,
                                                     const SnapshotRings& rings, std::int32_t time,
                                                     std::int32_t snap_flag_server_bit, std::int32_t padding) {
    constexpr std::int32_t kPacketBackup = 32;
    ServerOps ops = server_ops(msg.game);
    auto slot = [](std::int32_t sequence) { return static_cast<std::size_t>(sequence & (kPacketBackup - 1)); };
    const SnapshotFrame& frame = client.frames[slot(client.outgoing_sequence)];
    std::optional<DeltaRefused> refused;
    const SnapshotFrame* old = nullptr;
    std::int32_t last_frame = 0;
    if (client.delta_message > 0 && client.state == 4) {
        std::int32_t back = static_cast<std::int32_t>(static_cast<std::uint32_t>(client.outgoing_sequence) -
                                                      static_cast<std::uint32_t>(client.delta_message));
        if (back >= kPacketBackup - 3) {
            refused = DeltaRefused::OutOfDatePacket;
        } else {
            const SnapshotFrame& candidate = client.frames[slot(client.delta_message)];
            std::int32_t oldest = static_cast<std::int32_t>(static_cast<std::uint32_t>(rings.next_entities) -
                                                            static_cast<std::uint32_t>(rings.entities.size()));
            if (candidate.first_entity < oldest) {
                refused = DeltaRefused::OutOfDateEntities;
            } else {
                old = &candidate;
                last_frame = back;
            }
        }
    }
    msg.write_byte(ops.snapshot);
    msg.write_long(time);
    msg.write_byte(static_cast<std::uint8_t>(last_frame));
    std::int32_t flags = snap_flag_server_bit;
    if (client.rate_delayed) flags |= 1;
    if (client.state == 4) client.active = true;
    else if (client.state != 1) client.active = false;
    if (!client.active) flags |= 2;
    msg.write_byte(static_cast<std::uint8_t>(flags));
    const ByteView* from_player = old ? &old->player : nullptr;
    write_delta_playerstate(msg, tables, from_player, frame.player);
    std::vector<ByteView> from_entities, from_clients;
    if (old) {
        from_entities = run(rings.entities, old->first_entity, old->num_entities);
        from_clients = run(rings.clients, old->first_client, old->num_clients);
    }
    std::vector<ByteView> to_entities = run(rings.entities, frame.first_entity, frame.num_entities);
    std::vector<ByteView> to_clients = run(rings.clients, frame.first_client, frame.num_clients);
    write_packet_entities(msg, tables, from_entities, to_entities, rings.baseline);
    write_packet_clients(msg, tables, from_clients, to_clients);
    for (std::int32_t k = 0; k < padding; ++k) msg.write_byte(ops.nop);
    return refused;
}

}  // namespace iw_pc::net
