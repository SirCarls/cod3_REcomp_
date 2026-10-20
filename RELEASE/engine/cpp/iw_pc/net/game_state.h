// What the server writes into a client's messages (see the Rust version,
// iw_pc::net::game_state). The message types are here too, for both sides.
#pragma once

#include <functional>
#include <optional>
#include <span>

#include "iw_pc/base.h"
#include "iw_pc/net/delta.h"
#include "iw_pc/net/msg.h"

namespace iw_pc::net {

/// The server's message types (svc_ops_e): Call of Duty 2 starts at nop = 0
/// (no svc_bad); United Offensive has Quake III's numbers.
struct ServerOps {
    std::uint8_t nop, game_state, config_string, baseline, server_command, download, snapshot, eof;
};
inline ServerOps server_ops(Game game) {
    if (game == Game::CallOfDuty2) return {0, 1, 2, 3, 4, 5, 6, 7};
    return {1, 2, 3, 4, 5, 6, 7, 8};
}

/// The server's reliable commands a client hasn't acknowledged yet.
struct ServerCommands {
    std::int32_t acknowledged = 0;     // the last command the client acknowledged
    std::int32_t sequence = 0;         // the last command queued
    std::span<const ByteView> texts;   // a ring indexed by sequence number
};

/// Writes every reliable command after the acknowledged one: the message
/// type, the sequence number, the text. Returns the first text that was too
/// long (sent empty, as the original does after printing a warning).
/// Original: SV_UpdateServerCommandsToClient at 0x0045D9F0 (CoD2MP_s.exe 1.3)
/// Original: SV_UpdateServerCommandsToClient at 0x00464580 (CoDUOMP.exe 1.51)
std::optional<StringTooLong> write_server_commands(Msg& msg, const ServerCommands& commands);

/// The size of the buffer the server writes a game state into: Call of Duty 2 0x20000
/// bytes, United Offensive 0x8000.
inline std::size_t game_state_buffer_size(Game game) { return game == Game::CallOfDuty2 ? 0x20000 : 0x8000; }

/// The number of config strings (MAX_CONFIGSTRINGS).
inline constexpr std::size_t kMaxConfigStrings = 2048;

/// What a game state carries.
struct GameState {
    std::int32_t last_client_command = 0;   // the message's first long
    ServerCommands commands;                // reliable commands still to deliver
    std::span<const ByteView> config_strings;  // by index; empty ones are not sent; at most 2048
    std::span<const ByteView> baselines;       // entity states; one whose number is 0 is not sent
    std::int32_t client_num = 0;
    std::int32_t checksum_feed = 0;
};

/// Writes a game state: the last client command, pending reliable commands, the game
/// state marker and reliable sequence, every non-empty config string (index and big
/// string), every baseline (as a change from an all-zero entity, forced), then the end
/// marker, the client number, the checksum feed and a second end marker. Returns the first
/// config string or command that was too long (sent empty).
/// Original: SV_SendClientGameState at 0x004549A0 (CoD2MP_s.exe 1.3; the message part)
/// Original: SV_SendClientGameState at 0x0045A8F0 (CoDUOMP.exe 1.51; the message part)
std::optional<StringTooLong> write_game_state(Msg& msg, const FieldTables& tables, const GameState& state);

/// The parts of a client (client_t) sending it a game state reads and changes.
struct GameStateClient {
    std::int32_t state = 0;                 // 0: a free slot; 3 (primed) after
    bool unsent_fragments = false;          // the connection still has fragments to send
    std::int32_t pure_authentic = 0;        // cleared
    std::int32_t gamestate_message_num = 0; // set to the outgoing sequence
    std::int32_t outgoing_sequence = 0;     // read only
    std::int32_t reliable_sent = 0;         // set to the last command queued
};

/// The connection a game state goes out on (the server's netchan).
class GameStateLink {
public:
    virtual ~GameStateLink() = default;
    /// Sends the connection's next fragment (Netchan_TransmitNextFragment); whether more remain.
    virtual bool transmit_next_fragment() = 0;
    /// Sends the finished message (SV_SendMessageToClient); whether it left fragments to send.
    virtual bool send_message(const Msg& msg) = 0;
};

/// Sends a client its game state (see the Rust version): the fragments still going out
/// (while the slot is in use), the client primed, the game state written into `buffer`
/// (at most game_state_buffer_size bytes of it) and sent; United Offensive then sends one
/// fragment at once when the message left some and `timescale` (the setting's integer) is 0.
/// Original: SV_SendClientGameState at 0x004549A0 (CoD2MP_s.exe 1.3; client on the stack)
/// Original: SV_SendClientGameState at 0x0045A8F0 (CoDUOMP.exe 1.51; client on the stack)
std::optional<StringTooLong> send_client_game_state(Game game, const FieldTables& tables, GameStateClient& client,
                                                    const GameState& state, std::int32_t timescale, ByteSpan buffer,
                                                    GameStateLink& link);

/// The number that ends an entity list (ENTITYNUM_NONE).
inline constexpr std::int32_t kEntityListEnd = 1023;

/// Writes the changes between two sorted entity lists: a changed entity as a
/// change from its old state, a new one as a change from its baseline
/// (forced), a gone one as removed; then the end number.
/// Original: SV_EmitPacketEntities at 0x0045D450 (CoD2MP_s.exe 1.3)
/// Original: SV_EmitPacketEntities at 0x00464000 (CoDUOMP.exe 1.51)
void write_packet_entities(Msg& msg, const FieldTables& tables, std::span<const ByteView> from,
                           std::span<const ByteView> to, const std::function<ByteView(std::int32_t)>& baseline);

/// The same for clients: each change starts with a 1 bit, a 0 bit ends the list.
/// Original: SV_EmitPacketClients at 0x0045D5C0 (CoD2MP_s.exe 1.3)
/// Original: SV_EmitPacketClients at 0x00464170 (CoDUOMP.exe 1.51)
void write_packet_clients(Msg& msg, const FieldTables& tables, std::span<const ByteView> from,
                          std::span<const ByteView> to);

/// One of a client's snapshot frames as the server keeps it (clientSnapshot_t).
struct SnapshotFrame {
    ByteView player;
    std::int32_t num_entities = 0, num_clients = 0, first_entity = 0, first_client = 0;
};

/// The server's rings of snapshot entities and clients, and the baselines.
struct SnapshotRings {
    std::span<const ByteView> entities;
    std::span<const ByteView> clients;
    std::int32_t next_entities = 0;  // svs.nextSnapshotEntities
    std::function<ByteView(std::int32_t)> baseline;
};

/// The client's side of a snapshot (client_t's fields the writer reads).
struct SnapshotClient {
    std::int32_t state = 0;             // 4: in the game; 1: a zombie
    bool active = false;                // the flags' "in the game" word (kept in state 1)
    std::int32_t delta_message = 0;     // the last message acknowledged (0 or less: none)
    bool rate_delayed = false;
    std::int32_t outgoing_sequence = 0;
    std::span<const SnapshotFrame> frames;  // 32
};

/// Why a snapshot went out whole although the client had acknowledged one.
enum class DeltaRefused { OutOfDatePacket, OutOfDateEntities };

/// Writes a client's snapshot (see the Rust version): the frame for this
/// message, as a change from the acknowledged frame when usable, with the
/// time, the distance, the flags, the player state, the lists and padding.
/// Original: SV_WriteSnapshotToClient at 0x0045D7C0 (CoD2MP_s.exe 1.3; client ecx, msg eax)
/// Original: SV_WriteSnapshotToClient at 0x00464390 (CoDUOMP.exe 1.51; client eax, msg esi)
std::optional<DeltaRefused> write_snapshot_to_client(Msg& msg, const FieldTables& tables, SnapshotClient& client,
                                                     const SnapshotRings& rings, std::int32_t time,
                                                     std::int32_t snap_flag_server_bit, std::int32_t padding);

}  // namespace iw_pc::net
