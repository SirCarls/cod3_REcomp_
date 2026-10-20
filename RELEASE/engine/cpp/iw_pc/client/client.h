// The client's side of the network protocol: what a client keeps of the
// server's messages and how it reads them, so that a client can join a
// server of the original PC games (a CB or Steam copy's dedicated server).
// The C++ version of the Rust module iw_pc::client (see its documentation).
//
// ClientState holds what the original keeps for it in the client's `cl`
// (the game state's config strings, the entity baselines, the last 32
// snapshots and the rings their entities and clients are kept in) and in
// `clc`/`cls` (the server's command sequence and commands, the checksum
// feed, the client number). The parsers:
//
//   parse_server_message   one message from the server, after the netchan:
//                          decompressed, then its parts in order
//   parse_game_state       the game state (config strings, baselines)
//   parse_snapshot         a snapshot, as a change from an earlier one, with
//   parse_packet_entities  its entities
//   parse_packet_clients   and its clients
//   parse_command_string   one reliable command from the server
//   write_packet           the client's own message to the server
//   packet_event           a packet from the network (connectionless_packet
//                          for one without a connection)
//   check_for_resend       getchallenge and connect while joining
//   system_info_changed    the server's system info (its id, settings, archives)
//
// Each is checked against the original with the client's globals as they
// are in the game (tools/decomp/x86/specs/net_client.py).
//
// Left to the caller, as in the Rust version: the console (Con_Close), the
// system info and file system restart, authorization and downloads after a
// game state, the debug output (cl_shownet, cl_shownuments). Not ported yet:
// United Offensive's sv_onlyVisibleClients handling (a trace through the map
// for each player entity carried over); the parsers read snapshots as the
// original does with it off.
#pragma once

#include <array>
#include <functional>
#include <optional>
#include <vector>

#include "iw_pc/base.h"
#include "iw_pc/fs/fs.h"
#include "iw_pc/net/delta.h"
#include "iw_pc/net/game_state.h"
#include "iw_pc/net/huffman.h"
#include "iw_pc/net/msg.h"
#include "iw_pc/net/netchan.h"
#include "iw_pc/net/oob.h"

namespace iw_pc::client {

/// Config strings (MAX_CONFIGSTRINGS).
inline constexpr std::size_t kMaxConfigStrings = 2048;
/// Snapshots kept (PACKET_BACKUP).
inline constexpr std::size_t kPacketBackup = 32;
/// Entities and clients kept for snapshots (MAX_PARSE_ENTITIES, MAX_PARSE_CLIENTS).
inline constexpr std::size_t kMaxParseEntities = 2048;
/// Entity numbers (MAX_GENTITIES).
inline constexpr std::size_t kMaxEntities = 1024;
/// A snapshot's entities or clients may come from at most this many entries back in the ring.
inline constexpr std::int32_t kMaxRingDistance = 1920;
/// A client state's size (clientState_t).
inline constexpr std::size_t kClientStateSize = 0x5C;
/// Bytes of one server command in ClientState::server_commands.
inline constexpr std::size_t kServerCommandSize = net::kMaxStringChars;
/// Bytes of ClientState::server_message.
inline constexpr std::size_t kServerMessageSize = 256;
/// Bytes of ClientState::server_name.
inline constexpr std::size_t kServerNameSize = 256;
/// Player commands kept (CMD_BACKUP).
inline constexpr std::size_t kCmdBackup = 128;
/// The most player commands one packet carries (MAX_PACKET_USERCMDS).
inline constexpr std::int32_t kMaxPacketUsercmds = 32;

/// An entity state's size (entityState_t): Call of Duty 2 0xF0, United Offensive 0xF4.
inline std::size_t entity_state_size(Game game) { return game == Game::CallOfDuty2 ? 0xF0 : 0xF4; }
/// The room for config string text (MAX_GAMESTATE_CHARS): 0x20000, 0x5000.
inline std::size_t max_game_state_chars(Game game) { return game == Game::CallOfDuty2 ? 0x20000 : 0x5000; }
/// Reliable commands kept (MAX_RELIABLE_COMMANDS): 128, 64.
inline std::size_t max_reliable_commands(Game game) { return game == Game::CallOfDuty2 ? 128 : 64; }

/// The server's message types (net/game_state.h).
using net::ServerOps;
using net::server_ops;

/// The game state: every config string's text, one after the other
/// (gameState_t). Offset 0 (where unset strings point) is the empty string.
struct GameStateStrings {
    std::vector<std::int32_t> offsets;  // where each config string starts in data
    std::vector<std::uint8_t> data;     // the texts, each with its terminator
    std::int32_t data_count = 0;        // the bytes of data in use

    explicit GameStateStrings(Game game) : offsets(kMaxConfigStrings, 0), data(max_game_state_chars(game), 0) {}
    /// Config string `index`'s text (empty when unset or out of range).
    ByteView get(std::size_t index) const;
};

/// One snapshot as the client keeps it (clSnapshot_t).
struct Snapshot {
    std::int32_t valid = 0;  // complete: the client has the snapshot it changes
    std::int32_t snap_flags = 0;
    std::int32_t server_time = 0;
    std::int32_t message_num = 0;  // the server message it came in
    std::int32_t delta_num = 0;    // the message of the snapshot it changes (-1: none)
    std::int32_t ping = 0;
    std::int32_t cmd_num = 0;               // the word after ping (the parsers only copy it)
    std::vector<std::uint8_t> player_state;  // playerState_t
    std::int32_t num_entities = 0;
    std::int32_t num_clients = 0;
    std::int32_t parse_entities_num = 0;  // where its entities start in the entity ring
    std::int32_t parse_clients_num = 0;   // where its clients start in the client ring
    std::int32_t server_command_num = 0;  // the last server command when it arrived

    explicit Snapshot(Game game) : player_state(net::player_state_size(game), 0) {}
    void clear();
};

/// When a command the client sent left (outPacket_t).
struct OutPacket {
    std::int32_t cmd_number = 0;
    std::int32_t server_time = 0;
    std::int32_t realtime = 0;
};

/// What a client keeps of the server's messages.
struct ClientState {
    Game game;
    // --- the original's cl (cleared by a new game state) ---
    std::int32_t timeout_count = 0;        // frames in a row the server was quiet too long (check_timeout)
    Snapshot snap;                         // the latest complete snapshot
    std::int32_t old_frame_server_time = 0;  // the server time of the snapshot before it
    std::int32_t new_snapshots = 0;        // a new snapshot arrived
    std::int32_t server_time = 0;          // cl.serverTime: the client game's server time (set_cgame_time())
    std::int32_t old_server_time = 0;      // the last server_time (it never runs backwards)
    std::int32_t frame_server_time = 0;    // the snapshot time set_cgame_time() saw last (Q3's oldFrameServerTime)
    std::int32_t server_time_delta = 0;    // the server's time minus the client's (adjust_time_delta())
    std::int32_t extrapolated_snapshot = 0;  // the clock got near the latest snapshot's time
    GameStateStrings game_state;
    std::int32_t parse_entities_num = 0;  // the next free entry of the entity ring (counts up)
    std::int32_t parse_clients_num = 0;   // the next free entry of the client ring
    std::array<OutPacket, kPacketBackup> out_packets{};
    std::vector<Snapshot> snapshots;      // the last 32, by message number modulo 32
    std::vector<std::uint8_t> baselines;  // entityState_t each, by number
    std::vector<std::uint8_t> parse_entities;  // the entity ring
    std::vector<std::uint8_t> parse_clients;   // the client ring
    std::int32_t server_id = 0;                // sv_serverid: its low byte starts each client message
    std::vector<std::uint8_t> cmds;            // the last kCmdBackup player commands, by number % 128
    std::int32_t cmd_number = 0;               // the latest player command
    std::int32_t user_cmd_weapon = 0;          // cl.cgameUserCmdValue: the weapon (its low byte in each command)
    std::int32_t user_cmd_offhand = 0;         // Call of Duty 2's off-hand item (0x0098FDCC)
    std::array<float, 3> cgame_angles{};       // angles the client game adds to the view angles
    std::array<float, 3> view_angles{};        // cl.viewangles, degrees
    // --- clc and cls: the connection (not cleared by a game state) ---
    std::int32_t client_num = 0;
    std::int32_t checksum_feed = 0;
    std::int32_t challenge = 0;             // from the server (keys the scrambling)
    std::int32_t reliable_sequence = 0;     // the last reliable command queued
    std::int32_t reliable_acknowledge = 0;  // the last the server acknowledged
    std::vector<std::uint8_t> reliable_commands;  // a ring of max_reliable_commands() texts of 1024 bytes
    std::int32_t last_packet_sent_time = 0;  // cls.realtime when the last packet went out
    std::int32_t last_packet_time = 0;       // cls.realtime when the last packet from the server came in
    std::int32_t demo_playing = 0;           // a demo is playing (nothing is sent)
    std::int32_t demo_recording = 0;         // a demo is being recorded
    net::NetAddress server_address{};        // clc.serverAddress: the server being joined
    std::int32_t connect_time = 0;           // realtime of the last getchallenge/connect (-99999: at once)
    std::vector<std::uint8_t> server_message;  // the server's last message for the player (256 bytes)
    std::int32_t only_visible_clients = 0;   // United Offensive: the third word of challengeResponse
    std::int32_t qport = 0;                  // the client's net_qport
    std::int32_t server_message_sequence = 0;  // the last server message received
    std::int32_t server_command_sequence = 0;  // the last reliable command from the server
    std::vector<std::uint8_t> server_commands;  // a ring of max_reliable_commands() texts of 1024 bytes
    std::int32_t last_executed_server_command = 0;  // the last the client game took (get_server_command)
    std::int32_t connect_packet_count = 0;
    std::int32_t demo_waiting = 0;
    std::int32_t first_demo_frame_skipped = 0;  // clc.firstDemoFrameSkipped
    std::int32_t time_demo = 0;                 // clc.timeDemo (Call of Duty 2's; United Offensive reads a cvar)
    std::int32_t time_demo_base_time = 0;       // clc.timeDemoBaseTime: the first snapshot's server time
    net::Netchan chan;                 // clc.netchan
    std::int32_t connection_state = 0;  // cls.state (see write_packet())
    std::vector<std::uint8_t> server_name;  // cls.servername (256 bytes): "localhost" may go back in time
    std::int32_t realtime = 0;          // cls.realtime
    std::int32_t sim_time = 0;          // United Offensive's second clock (0x04AD3E80), its client game's

    explicit ClientState(Game game);
    /// Clears what the original's CL_ClearState clears: everything of cl.
    void clear();
    /// Server command `sequence`'s text (from the ring).
    ByteView server_command(std::int32_t sequence) const;
};

/// Reads one reliable command (its sequence number and text) and keeps it
/// when it is newer than the last one: at most 1023 characters, the rest of
/// its slot zeroed.
/// Original: CL_ParseCommandString at 0x00414380 (CoD2MP_s.exe 1.3; CoD2x calls it CL_ServerCommand)
/// Original: CL_ParseCommandString at 0x004194A0 (CoDUOMP.exe 1.51)
void parse_command_string(net::Msg& msg, ClientState& cl);

/// Reads a game state into cl (cleared first): the server command sequence,
/// the config strings and baselines, the end, the client number and the
/// checksum feed. Errors (ERR_DROP): a config string index outside 0-2047,
/// more text than max_game_state_chars(), a baseline number outside 0-1023,
/// any other part type.
/// Original: CL_ParseGamestate at 0x00413BD0 (CoD2MP_s.exe 1.3; the message part)
/// Original: CL_ParseGamestate at 0x00418D10 (CoDUOMP.exe 1.51; the message part)
Status parse_game_state(net::Msg& msg, const net::FieldTables& tables, ClientState& cl);

/// Reads a snapshot's entities into the entity ring, as changes from `old`
/// (an index into cl.snapshots; nullopt: none) or from the baselines; old's
/// entities the message doesn't name are carried over.
/// Original: CL_ParsePacketEntities at 0x00412FB0 (CoD2MP_s.exe 1.3)
/// Original: CL_ParsePacketEntities at 0x00418150 (CoDUOMP.exe 1.51)
Status parse_packet_entities(net::Msg& msg, const net::FieldTables& tables, ClientState& cl, std::optional<std::size_t> old,
                             Snapshot& snapshot);

/// The same for a snapshot's clients (a 1 bit and the number before each).
/// Original: CL_ParsePacketClients at 0x00413310 (CoD2MP_s.exe 1.3)
/// Original: CL_ParsePacketClients at 0x004184E0 (CoDUOMP.exe 1.51)
Status parse_packet_clients(net::Msg& msg, const net::FieldTables& tables, ClientState& cl, std::optional<std::size_t> old,
                            Snapshot& snapshot);

/// Reads a snapshot (after its message type); a complete one becomes
/// cl.snap, with its ping, and is kept in cl.snapshots. Call of Duty 2 drops
/// a snapshot whose message ran out.
/// Original: CL_ParseSnapshot at 0x004136C0 (CoD2MP_s.exe 1.3)
/// Original: CL_ParseSnapshot at 0x00418870 (CoDUOMP.exe 1.51)
Status parse_snapshot(net::Msg& msg, const net::FieldTables& tables, ClientState& cl);

/// What the caller does with the parts of a server message the ports leave
/// to it. Each returns whether to go on reading the message.
class ServerMessageParts {
public:
    virtual ~ServerMessageParts() = default;
    /// After a game state: the system info, file system restart, authorization, downloads.
    virtual bool game_state_parsed(ClientState& cl) = 0;
    /// A download block (svc_download), to read from msg (CL_ParseDownload).
    virtual bool download(net::Msg& msg, ClientState& cl) = 0;
};

/// Reads one message from the server (msg after the netchan header and the
/// acknowledge: Huffman-coded from readcount): decompressed into a buffer
/// of the game's largest message, then each part until the end marker.
/// Running out of data or an unknown part type is the original's error;
/// Call of Duty 2 also stops after a part that left the message overflowed.
/// Returns whether it got to the end (false: one of `parts` stopped it).
/// Original: CL_ParseServerMessage at 0x004143E0 (CoD2MP_s.exe 1.3)
/// Original: CL_ParseServerMessage at 0x00419500 (CoDUOMP.exe 1.51)
Expected<bool> parse_server_message(net::Msg& msg, const net::MessageTree& tree, const net::FieldTables& tables,
                                    ClientState& cl, ServerMessageParts& parts);

/// What connectionless_packet() did with a packet without a connection (see
/// the Rust version's ClientPacket for each).
struct ClientPacket {
    enum class Kind {
        PunkBuster, Voice, ChallengeTaken, ChallengeUnwanted, Connected, DuplicateConnect, NotConnecting,
        ConnectFromElsewhere, Info, Status, Disconnect, Echo, KeyAuthorize, Motd, Print, ServerError, ErrorIgnored,
        Update, Servers, NeedCdKey, NewMap, NewMapIgnored, Unknown
    };
    Kind kind;
    std::vector<std::uint8_t> text;  // ServerError: the message; NewMap: the map's name
    /// Call of Duty 2's result: false for the packets it ignores as unwanted, duplicate, misplaced or unknown.
    bool cod2_result() const {
        return kind != Kind::ChallengeUnwanted && kind != Kind::DuplicateConnect && kind != Kind::NotConnecting &&
               kind != Kind::ConnectFromElsewhere && kind != Kind::ErrorIgnored && kind != Kind::Unknown;
    }
};

/// What a client does with a packet without a connection from `from` (msg
/// holds it, the four 0xFF bytes included): its first line's first word
/// picks the handling. challengeResponse while connecting keeps the
/// challenge and the server's address; connectResponse while sending
/// connect, from the server, sets up the channel; print and needcdkey set
/// server_message; echo answers through `send`.
/// Original: CL_ConnectionlessPacket at 0x0040E750 (CoD2MP_s.exe 1.3; from, msg and a word on the stack)
/// Original: CL_ConnectionlessPacket at 0x00412880 (CoDUOMP.exe 1.51; from, msg and a word on the stack)
ClientPacket connectionless_packet(ClientState& cl, const net::NetAddress& from, net::Msg& msg,
                                   const std::function<void(const net::NetAddress&, ByteView)>& send);

/// What packet_event() did with a packet (see the Rust version).
struct PacketOutcome {
    enum class Kind { Connectionless, NotConnected, Runt, NotFromServer, Dropped, TooOld, Read, Illegible };
    Kind kind;
    bool demo = false;  // Read: the original now writes the message into the demo being recorded
    /// Call of Duty 2's CL_PacketEvent result: true for a runt packet and a message read.
    bool cod2_result() const { return kind == Kind::Runt || kind == Kind::Read; }
};

/// A packet from the network (msg as it arrived, from `from`): a
/// connectionless one goes back to the caller; a connected client takes one
/// from the server's address through the channel, notes its message number
/// and the acknowledged reliable command, unscrambles the rest and parses it.
/// Original: CL_PacketEvent at 0x0040F250 (CoD2MP_s.exe 1.3; msg in eax, from on the stack)
/// Original: CL_PacketEvent at 0x004131B0 (CoDUOMP.exe 1.51; msg in ecx, from on the stack)
Expected<PacketOutcome> packet_event(ClientState& cl, const net::NetAddress& from, net::Msg& msg,
                                     const net::MessageTree& tree, const net::FieldTables& tables,
                                     ServerMessageParts& parts);

/// The client's settings write_packet() reads.
struct SendSettings {
    std::int32_t packet_dup = 0;  // cl_packetdup (0-5; United Offensive clamps it there)
    bool no_delta = false;        // cl_nodelta
};

/// Whether the original sends nothing in this connection state: Call of
/// Duty 2 in states 1 and 2, United Offensive in 7 and 8.
inline bool sends_nothing_in(Game game, std::int32_t state) {
    return game == Game::CallOfDuty2 ? state == 1 || state == 2 : state == 7 || state == 8;
}

/// Builds and sends the client's message to the server (see the Rust
/// version): the raw header, the unacknowledged reliable commands, the
/// player commands since the packet packet_dup + 1 back, the end; Huffman-
/// coded from byte 9, scrambled, recorded in out_packets, sent through the
/// channel (fragments too): `send` gets each packet and its destination.
/// Original: CL_WritePacket at 0x00408E80 (CoD2MP_s.exe 1.3)
/// Original: CL_WritePacket at 0x0040CC80 (CoDUOMP.exe 1.51)
Status write_packet(ClientState& cl, const net::MessageTree& tree, const SendSettings& settings,
                    const std::function<void(const net::NetAddress&, ByteView)>& send);

/// What check_for_resend() did (see the Rust version's Resend).
struct Resend {
    enum class Kind { Nothing, Challenge, Connect };
    Kind kind = Kind::Nothing;
    /// Challenge: the original first asks the key server about the player's
    /// key (CL_RequestAuthorization, the caller's): the server isn't on the
    /// local network, or net_lanauthorize is set.
    bool authorize = false;
};

/// How long a client waits before sending getchallenge or connect again, in
/// milliseconds: Call of Duty 2 2000, United Offensive 3000.
inline std::int32_t resend_time(Game game) { return game == Game::CallOfDuty2 ? 2000 : 3000; }

/// What check_for_resend() needs besides the client's state.
struct ResendSettings {
    ByteView userinfo;  // the player's settings as an info string
    ByteView key_hash;  // Call of Duty 2's getchallenge: CL_BuildMd5StrFromCDKey's hash (not ported)
    bool lan_authorize = false;
    std::span<const std::array<std::uint8_t, 4>> local_addresses;  // the machine's own IPv4 addresses
};

/// While joining, sends the next getchallenge or connect to the server every
/// resend_time() (see the Rust version: the texts, United Offensive's
/// compression and its connect text starting over past 1024 bytes).
/// Original: CL_CheckForResend at 0x0040E100 (CoD2MP_s.exe 1.3)
/// Original: CL_CheckForResend at 0x00411E60 (CoDUOMP.exe 1.51)
Resend check_for_resend(ClientState& cl, const ResendSettings& settings,
                        const std::function<void(const net::NetAddress&, ByteView)>& send);

/// The config string holding the server's system info (CS_SYSTEMINFO).
inline constexpr std::size_t kSystemInfo = 1;

/// What the server's system info asks of the rest of the client (see the
/// Rust version's SystemInfo).
struct SystemInfo {
    bool cheats_off = false;  // sv_cheats reads as 0: cheat-protected settings back to their defaults
    std::vector<std::uint8_t> loaded_sums, loaded_names;          // sv_iwds, sv_iwdNames (UO: sv_paks ...)
    std::vector<std::uint8_t> referenced_sums, referenced_names;  // sv_referencedIwds ... (UO: ...Paks ...)
    /// Every \key\value pair in order, for the client's own settings (none while it runs the server).
    std::vector<std::pair<std::vector<std::uint8_t>, std::vector<std::uint8_t>>> settings;
};

/// The system info changed: sv_serverid becomes cl.server_id; while a demo
/// plays that is all (nullopt), otherwise the rest is for the caller.
/// Errors: a system info of 8192 characters or more.
/// Original: CL_SystemInfoChanged at 0x004139E0 (CoD2MP_s.exe 1.3)
/// Original: CL_SystemInfoChanged at 0x00418B80 (CoDUOMP.exe 1.51)
Expected<std::optional<SystemInfo>> system_info_changed(ClientState& cl, bool server_running);

/// The connection states after joining (Call of Duty 2: 6, 7, 8; United
/// Offensive: 4, 5, 6).
struct PlayingStates {
    std::int32_t loading, primed, active;
};
inline PlayingStates playing_states(Game game) {
    return game == Game::CallOfDuty2 ? PlayingStates{6, 7, 8} : PlayingStates{4, 5, 6};
}

/// A snapshot's flag: the server doesn't have the player in the game yet.
inline constexpr std::int32_t kSnapflagNotActive = 2;

/// The first snapshot in the game makes the client active (see the Rust
/// version); returns whether it did (the caller then runs activeAction).
/// Original: CL_FirstSnapshot at 0x00402EC0 (CoD2MP_s.exe 1.3)
/// Original: CL_FirstSnapshot at 0x004056A0 (CoDUOMP.exe 1.51)
bool first_snapshot(ClientState& cl);

/// The settings the client's clock reads.
struct ClockSettings {
    bool client_paused = false, server_paused = false, server_running = false;  // all three: standing still
    bool freeze_demo = false;   // cl_freezeDemo
    std::int32_t time_nudge = 0;  // United Offensive's cl_timeNudge (-30 to 30 used)
    float timescale = 1.0f;     // the slow drift runs at 1 (United Offensive: 0 or 1)
    bool time_demo = false;     // United Offensive's timedemo cvar
};

/// Keeps cl.server_time_delta in step after a new snapshot (see the Rust version).
/// Original: CL_AdjustTimeDelta at 0x00402D70 (CoD2MP_s.exe 1.3)
/// Original: CL_AdjustTimeDelta at 0x00405560 (CoDUOMP.exe 1.51)
void adjust_time_delta(ClientState& cl, const ClockSettings& settings);

/// What the client's clock leaves to the caller.
class ClockParts {
public:
    virtual ~ClockParts() = default;
    /// The first snapshot made the client active: the originals run activeAction once and clear it.
    virtual void became_active(ClientState& cl) = 0;
    /// The next message of the demo playing (CL_ReadDemoMessage); whether to go on.
    virtual bool read_demo_message(ClientState& cl) = 0;
    /// timedemo's frame timing; whether to go on.
    virtual bool time_demo_frame(ClientState& cl) = 0;
};

/// The client game's clock, once a frame (see the Rust version). Errors:
/// active without a valid snapshot; a snapshot earlier than the last one
/// except from a server named localhost.
/// Original: CL_SetCGameTime at 0x00403080 (CoD2MP_s.exe 1.3)
/// Original: CL_SetCGameTime at 0x00405840 (CoDUOMP.exe 1.51)
Status set_cgame_time(ClientState& cl, const ClockSettings& settings, ClockParts& parts);

/// A player command's last fields: the client game's weapon (and off-hand
/// item), the server time (at most 5 s past the snapshot's), the angles.
/// Original: CL_FinishMove at 0x00408A90 (CoD2MP_s.exe 1.3; cmd in eax)
/// Original: CL_FinishMove at 0x0040C8A0 (CoDUOMP.exe 1.51; cmd in eax)
void finish_move(const ClientState& cl, ByteSpan cmd);

/// United Offensive's input timing (frame_msec, at most 200, and the frame time it came from).
struct InputTiming {
    std::int32_t frame_msec = 0;
    std::int32_t old_frame_time = 0;
};

/// Once loaded, adds the player command `create` makes (the caller's
/// CL_CreateCmd) to the ring; United Offensive first times the frame.
/// Original: CL_CreateNewCommands at 0x00408BB0 (CoD2MP_s.exe 1.3)
/// Original: CL_CreateNewCommands at 0x0040CAD0 (CoDUOMP.exe 1.51)
void create_new_commands(ClientState& cl, std::int32_t frame_time, InputTiming& timing,
                         const std::function<std::vector<std::uint8_t>(ClientState&, const InputTiming&)>& create);

/// Whether to send the next message now (see the Rust version); United
/// Offensive keeps max_packets within 15 to 100 (the caller's cvar).
/// Original: CL_ReadyToSendPacket at 0x00408C10 (CoD2MP_s.exe 1.3)
/// Original: CL_ReadyToSendPacket at 0x0040CB60 (CoDUOMP.exe 1.51)
bool ready_to_send_packet(const ClientState& cl, bool downloading,
                          std::span<const std::array<std::uint8_t, 4>> local_addresses, std::int32_t& max_packets);

/// What add_reliable_command() did: queued (with the copy's warning, if
/// any), or an error (more than the ring's size unacknowledged).
struct Queued {
    Status error;
    std::optional<net::ReliableCommandWarning> warning;
};

/// Queues a command for the server in the next slot of the ring (see the
/// Rust version).
/// Original: CL_AddReliableCommand at 0x0040C000 (CoD2MP_s.exe 1.3)
/// Original: CL_AddReliableCommand at 0x0040F930 (CoDUOMP.exe 1.51)
Queued add_reliable_command(ClientState& cl, ByteView command);

/// Queues "cp " and the referenced archives' pure checksums (cut to 1020
/// characters) for a pure server.
/// Original: CL_SendPureChecksums at 0x0040D680 (CoD2MP_s.exe 1.3)
/// Original: CL_SendPureChecksums at 0x00411450 (CoDUOMP.exe 1.51)
Status send_pure_checksums(ClientState& cl, std::span<const fs::SearchPath> search_paths, std::int32_t feed,
                           std::int32_t fake);

/// A config string's new text, from a server command's words ("d <index>
/// <text>"): the game state is packed again with it; returns the index when
/// the text changed (the original then runs CL_SystemInfoChanged for
/// kSystemInfo: the caller's), nothing when it was the same.
/// Original: CL_ConfigstringModified at 0x00401550 (CoD2MP_s.exe 1.3)
/// Original: CL_ConfigstringModified at 0x00401480 (CoDUOMP.exe 1.51)
Expected<std::optional<std::size_t>> configstring_modified(ClientState& cl, ByteView index_word, ByteView text);

/// The buffer a big config string is put together in (x, y, z).
inline constexpr std::size_t kBigConfigStringSize = 0x2000;

/// What a server command asks of the client (see the Rust version).
struct ServerCommand {
    // Endless: a big config string that is itself a z command adding nothing, which the
    // original runs again and again and never returns from; the port stops.
    enum class Kind { ClientGame, ConfigString, MapRestart, Kept, Disconnected, Endless };
    Kind kind = Kind::ClientGame;
    std::optional<std::size_t> changed;   // ConfigString: the index, if the text changed
    std::optional<std::vector<std::uint8_t>> reason;  // Disconnected: the second word
    bool localized_reason = true;          // Disconnected: false for Call of Duty 2's "PB"
};

/// The words of server command `number` for the client game, and what the
/// command does to the client first (letters: d, x y z, B n, w).
/// Original: CL_GetServerCommand at 0x00401710 (CoD2MP_s.exe 1.3)
/// Original: CL_GetServerCommand at 0x00401630 (CoDUOMP.exe 1.51)
Expected<ServerCommand> get_server_command(ClientState& cl, std::int32_t number, net::Tokens& tokens,
                                           std::span<std::uint8_t, kBigConfigStringSize> big);

/// What check_timeout() reads besides the client.
struct TimeoutSettings {
    bool client_paused = false;  // cl_paused and sv_paused: no time-out while both are set
    bool server_paused = false;
    float timeout = 200.0f;          // cl_timeout, seconds
    float connect_timeout = 200.0f;  // Call of Duty 2's cl_connectTimeout, seconds
};

/// Whether the server went quiet (see the Rust version): a sixth frame in a
/// row past cl_timeout is a dropping error; Call of Duty 2 also gives up
/// joining past cl_connectTimeout.
/// Original: CL_CheckTimeout at 0x0040F470 (CoD2MP_s.exe 1.3)
/// Original: CL_CheckTimeout at 0x00413390 (CoDUOMP.exe 1.51)
Status check_timeout(ClientState& cl, const TimeoutSettings& settings);

/// Queues `userinfo "<settings>"` once joining and not paused, when the
/// settings' changed flag (2) is set (and clears it); nothing (sent false)
/// otherwise.
/// Original: CL_CheckUserinfo at 0x0040F530 (CoD2MP_s.exe 1.3)
/// Original: CL_CheckUserinfo at 0x004137C0 (CoDUOMP.exe 1.51)
struct UserinfoSent {
    bool sent = false;
    Queued queued;
};
UserinfoSent check_userinfo(ClientState& cl, bool paused, std::int32_t& modified_flags, ByteView userinfo);

}  // namespace iw_pc::client
