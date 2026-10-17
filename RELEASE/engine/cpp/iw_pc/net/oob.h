// Packets without a connection ("out of band"): FF FF FF FF, then a line of
// text the receiver splits into words like a console command (getinfo xxx,
// getchallenge, connect "<userinfo>"). A server answers with the same kind
// of packet (infoResponse, challengeResponse, error ...). This is how a
// client finds servers on the LAN and starts a connection
// (docs/PC_MULTIPLAYER.md section 3). Everything a server sends here is the
// byte string a CB dedicated server of the same version sends.
//
// United Offensive turns every byte above 127 into . when it reads the
// line, Call of Duty 2 keeps them (and its tokenizer treats them as spaces);
// Call of Duty 2's tokenizer reads \" inside quotes as a quote; a line of
// more than 512 words gives no words in Call of Duty 2, the first 512 in UO.
#pragma once

#include <array>
#include <functional>
#include <vector>

#include "iw_pc/base.h"
#include "iw_pc/net/netchan.h"

namespace iw_pc::net {

/// The four bytes a packet without a connection starts with.
inline constexpr std::array<std::uint8_t, 4> kConnectionlessPrefix = {0xFF, 0xFF, 0xFF, 0xFF};

/// At least four bytes, the first four all 0xFF (SV_PacketEvent's first test).
bool is_connectionless(ByteView packet);

/// MAX_STRING_CHARS: the line buffer (the longest line is 1023 characters).
inline constexpr std::size_t kMaxLine = 1024;

/// The first line of a packet without a connection, as the server reads it.
class Line {
public:
    ByteView bytes() const { return ByteView(text_).first(len_); }
    /// Reads the first line of `packet` (the whole packet, prefix included):
    /// from byte 4 to \n, a zero byte or the end, at most 1023 characters,
    /// % as ., 0x92 as ' (UO: every byte above 127 as .).
    void read(Game game, ByteView packet);
    /// The line `text` as it is (at most 1023 bytes).
    void set(ByteView text);

private:
    std::array<std::uint8_t, kMaxLine> text_{};
    std::size_t len_ = 0;
};

/// MAX_STRING_TOKENS: the most words a line is split into.
inline constexpr std::size_t kMaxTokens = 512;
/// The tokenizer's text buffer: BIG_INFO_STRING + MAX_STRING_TOKENS bytes.
inline constexpr std::size_t kTokenTextSize = 8192 + kMaxTokens;

/// A line split into words, the games' Cmd_Argc / Cmd_Argv.
class Tokens {
public:
    std::size_t argc() const { return count_; }
    /// Word i, or an empty string past the last one.
    ByteView argv(std::size_t i) const;
    /// No words (United Offensive's tokenizer given a NULL text).
    void clear();
    /// The text buffer as the original leaves it: the words, each followed by 0.
    ByteView raw_text() const { return ByteView(text_).first(used_); }
    /// Where each word the last split wrote starts (Call of Duty 2's
    /// disowned ones included).
    std::span<const std::uint16_t> raw_starts() const { return std::span<const std::uint16_t>(starts_).first(written_); }

    /// Splits `line` into words, as the console and the network code do:
    /// spaces (bytes up to ' ' as signed chars) separate words; "..." is one
    /// word; // ends the line; /* ... */ is skipped between words; `limit`
    /// n > 0 makes the nth word the rest of the line; after 512 words Call
    /// of Duty 2 reports none, United Offensive keeps the 512.
    /// Original: Cmd_TokenizeStringInternal at 0x004210D0 (CoD2MP_s.exe 1.3)
    /// Original: Cmd_TokenizeStringInternal at 0x0042C240 (CoDUOMP.exe 1.51)
    void tokenize(Game game, ByteView line, std::uint32_t limit);

private:
    std::array<std::uint8_t, kTokenTextSize> text_{};
    std::size_t used_ = 0;
    std::array<std::uint16_t, kMaxTokens> starts_{};
    std::size_t written_ = 0;
    std::size_t count_ = 0;
};

/// The longest text Call of Duty 2 sends without a connection.
inline constexpr std::size_t kMaxPrintTextCod2 = 0x1FFFC - 1;
/// The longest text United Offensive sends without a connection.
inline constexpr std::size_t kMaxPrintTextUo = 0x8000 - 4 - 1;
/// The most data United Offensive's NET_OutOfBandData takes.
inline constexpr std::size_t kMaxDataUo = 0x10000 - 4;
/// The most data Call of Duty 2's NET_OutOfBandData takes.
inline constexpr std::size_t kMaxDataCod2 = 0x20000 - 4;

/// The four 0xFF bytes and `text` (up to its first zero byte) in `out`;
/// the length, or nullopt when the game wouldn't send it.
/// Original: NET_OutOfBandPrint at 0x00448910 (CoD2MP_s.exe 1.3)
/// Original: NET_OutOfBandPrint at 0x0044E140 (CoDUOMP.exe 1.51)
std::optional<std::size_t> out_of_band_print(Game game, ByteView text, ByteSpan out);

/// The four 0xFF bytes and `data` as it is (United Offensive then
/// compresses from byte 12 with compress_adaptive(); the caller does that).
/// Original: NET_OutOfBandData at 0x00448A70 (CoD2MP_s.exe 1.3)
/// Original: NET_OutOfBandData at 0x0044E240 (CoDUOMP.exe 1.51)
std::optional<std::size_t> out_of_band_data(Game game, ByteView data, ByteSpan out);

/// What a server does with a packet without a connection.
enum class ServerRequest { PunkBuster, Status, Voice, Info, Challenge, Connect, IpAuthorize, RemoteCommand, Ignored, Unknown };

/// The packet's text starts with pb_ in any case.
bool is_punkbuster(ByteView packet);
/// Whether United Offensive decompresses this packet (text starting with "connect").
bool connect_is_compressed(Game game, ByteView packet);

/// Decides what a server does with a packet without a connection; reads
/// its first line into `line` and splits it into `tokens`.
/// Original: SV_ConnectionlessPacket at 0x0045B430 (CoD2MP_s.exe 1.3)
/// Original: SV_ConnectionlessPacket at 0x004623C0 (CoDUOMP.exe 1.51)
ServerRequest server_request(Game game, ByteView packet, Line& line, Tokens& tokens);

/// The longest infoResponse text Call of Duty 2 sends.
inline constexpr std::size_t kMaxInfoResponseCod2 = 1023;
/// The longest statusResponse text Call of Duty 2 sends.
inline constexpr std::size_t kMaxStatusResponseCod2 = 0x2000 - 1;
/// A client slot's state at which a player counts as on the server.
inline constexpr std::int32_t kClientStateConnected = 2;

/// A setting looked up by name: missing, a number, or text.
struct Setting {
    enum class Kind { Missing, Number, Text } kind = Kind::Missing;
    std::int32_t number = 0;
    ByteView text;
    static Setting of_number(std::int32_t n) { return {Kind::Number, n, {}}; }
    static Setting of_text(ByteView t) { return {Kind::Text, 0, t}; }
};

/// The C runtime's atol: white space, one sign, decimal digits (32-bit wrap).
/// Original: atol at 0x0057A751 (CoD2MP_s.exe 1.3)
/// Original: atol at 0x0056C695 (CoDUOMP.exe 1.51)
std::int32_t atol(ByteView text);

/// What a server reports about itself in infoResponse.
struct ServerInfo {
    ByteView challenge, hostname, mapname, gametype;
    std::int32_t max_clients = 0, private_clients = 0;
    std::span<const std::int32_t> client_states;
    std::int32_t pure = 0, server_iwd_count = 0, min_ping = 0, max_ping = 0;
    ByteView fs_game;
    std::int32_t allow_anonymous = 0, disable_client_console = 0;
    ByteView password;
    Setting friendly_fire, killcam, timeouts_allowed, allow_jeeps, allow_tanks;
    bool dedicated = false, modded = false;
    std::int32_t voice = 0, punkbuster = 0;
};

/// Whether the server reports mod 1: always, unless sv_pure is on and
/// fs_game is empty; then only if a referenced archive isn't official
/// (`is_official` answers that).
bool runs_mods(Game game, bool pure, ByteView fs_game, ByteView referenced_archives, Tokens& tokens,
               const std::function<bool(ByteView)>& is_official);

/// Writes infoResponse\n and the info string into `out`; returns the length.
/// Original: SVC_Info at 0x0045AE80 (CoD2MP_s.exe 1.3)
/// Original: SVC_Info at 0x004616F0 (CoDUOMP.exe 1.51)
std::size_t info_response(Game game, const ServerInfo& info, ByteSpan out);

struct PlayerStatus {
    std::int32_t score = 0, ping = 0;
    ByteView name;
};

struct ServerStatus {
    ByteView server_info, challenge;
    bool restricted = false;
    std::span<const PlayerStatus> players;
    std::int32_t disable_client_console = 0;
    ByteView password;
    bool modded = false;
};

/// Writes statusResponse\n, the server info with the game's keys, \n and a
/// line per player into `out`; returns the length.
/// Original: SVC_Status at 0x0045A880 (CoD2MP_s.exe 1.3)
/// Original: SVC_Status at 0x00461190 (CoDUOMP.exe 1.51)
std::size_t status_response(Game game, const ServerStatus& status, ByteSpan out);

/// How many challenges a server remembers.
inline constexpr std::size_t kMaxChallenges = 1024;

/// NET_CompareAdr's "the same": same type; IPv4/IPX: same port and bytes.
/// Original: NET_CompareAdrSigned at 0x00448650 (CoD2MP_s.exe 1.3)
/// Original: NET_CompareAdrSigned at 0x0044DEC0 (CoDUOMP.exe 1.51)
bool same_address(const NetAddress& a, const NetAddress& b);

/// One remembered challenge, the games' challenge_t.
struct ChallengeSlot {
    NetAddress address;
    std::int32_t challenge = 0, time = 0, ping_time = 0, first_time = 0, first_ping = 0;
    bool connected = false;
    std::array<std::uint8_t, 33> punkbuster_guid{};  // Call of Duty 2 only
};

/// The challenges a server remembers (svs.challenges).
class ChallengeTable {
public:
    std::vector<ChallengeSlot> slots = std::vector<ChallengeSlot>(kMaxChallenges);
    /// The slot for `from` (the first not yet connected with its address),
    /// or the slot given out longest ago, started over for it.
    std::size_t find_or_add(const NetAddress& from, std::int32_t time, const std::function<std::int32_t()>& new_challenge);
};

/// ((rand() << 16) ^ rand()) ^ clock.
inline std::int32_t challenge_number(std::int32_t first, std::int32_t second, std::int32_t clock) {
    return static_cast<std::int32_t>((static_cast<std::uint32_t>(first) << 16) ^ static_cast<std::uint32_t>(second) ^
                                     static_cast<std::uint32_t>(clock));
}

struct ChallengeRequest {
    NetAddress from;
    std::int32_t time = 0;
    bool lan_authorize = false, from_lan = false, authorize_known = false;
    std::int32_t last_master_contact = 0;
    bool from_master = false;
    std::optional<ByteView> punkbuster_guid;  // Call of Duty 2 only
};

/// What a server does about a getchallenge.
struct ChallengeAnswer {
    enum class Kind { Respond, AuthorizeUnknown, Authorize } kind;
    std::size_t slot = 0;
};

/// Decides how a server answers getchallenge, keeping the table up to date.
/// Original: SV_GetChallenge at 0x00452DE0 (CoD2MP_s.exe 1.3)
/// Original: SV_GetChallenge at 0x00458C50 (CoDUOMP.exe 1.51)
ChallengeAnswer get_challenge(Game game, ChallengeTable& table, const ChallengeRequest& request,
                              const std::function<std::int32_t()>& new_challenge);

/// challengeResponse <n> (UO: and <m> when sv_onlyVisibleClients isn't 0).
std::size_t challenge_response_text(Game game, std::int32_t challenge, std::int32_t only_visible_clients, ByteSpan out);

/// NET_CompareBaseAdr's "the same machine".
/// Original: NET_CompareBaseAdrSigned at 0x004485B0 (CoD2MP_s.exe 1.3)
/// Original: NET_CompareBaseAdrSigned at 0x0044DD50 (CoDUOMP.exe 1.51)
bool same_base_address(Game game, const NetAddress& a, const NetAddress& b);

/// What SV_DirectConnect reads of a client slot.
struct ClientSlotInfo {
    std::int32_t state = 0;
    NetAddress address;
    std::int32_t qport = 0, last_connect_time = 0;
};

struct ConnectRequest {
    NetAddress from;
    ByteView userinfo;
    std::int32_t time = 0, reconnect_limit = 0, max_clients = 0, private_clients = 0;
    ByteView private_password;
    double min_ping = 0, max_ping = 0;
    bool from_lan = false;
    std::span<const ClientSlotInfo> clients;
};

/// What a server does with a connect.
struct ConnectDecision {
    enum class Kind { WrongProtocol, ReconnectTooSoon, BadChallenge, PingTooLow, PingTooHigh, Reconnect, NewSlot, ServerFull } kind;
    std::size_t slot = 0;
    std::optional<std::size_t> challenge;
    bool free_first = false;
    static ConnectDecision of(Kind kind, std::size_t slot = 0, std::optional<std::size_t> challenge = std::nullopt,
                              bool free_first = false) {
        ConnectDecision d{kind, slot, challenge, free_first};
        return d;
    }
    /// The refusal's answer text, or nullopt when nothing is sent now.
    std::optional<ByteView> reply(Game game) const;
};

/// 118 (Call of Duty 2 1.3), 22 (United Offensive 1.51).
inline std::int32_t protocol_version(Game game) { return game == Game::CallOfDuty2 ? 118 : 22; }

/// Decides what a server does with a connect (changes only the challenge table).
/// Original: SV_DirectConnect at 0x00453C20 (CoD2MP_s.exe 1.3)
/// Original: SV_DirectConnect at 0x00459B20 (CoDUOMP.exe 1.51)
ConnectDecision direct_connect(Game game, ChallengeTable& table, const ConnectRequest& request);

/// Whether a client lists the server that sent this infoResponse.
/// Original: CL_ServerInfoPacket at 0x004B34E0 (CoD2MP_s.exe 1.3)
/// Original: CL_ServerInfoPacket at 0x004155F0 (CoDUOMP.exe 1.51)
bool info_response_accepted(Game game, ByteView info, ByteView debug_protocol_text, float debug_protocol_value);

/// One server in a client's server list (serverInfo_t).
struct ServerListEntry {
    std::uint8_t net_type = 0, clients = 0, max_clients = 0, allow_anonymous = 0, password = 0, pure = 0;
    std::uint8_t friendly_fire = 0, killcam = 0, console_disabled = 0, hardware = 0, modded = 0, voice = 0, punkbuster = 0;
    std::int16_t min_ping = 0, max_ping = 0, ping = 0, timeouts_allowed = 0, jeeps = 0, tanks = 0;
    std::array<std::uint8_t, 32> hostname{}, mapname{};
    std::array<std::uint8_t, 24> game{};
    std::array<std::uint8_t, 16> gametype{};
};

/// Fills a server list entry from a server's info string (nullptr: only the ping).
/// Original: CL_SetServerInfo at 0x004B3300 (CoD2MP_s.exe 1.3)
/// Original: CL_SetServerInfo at 0x00415180 (CoDUOMP.exe 1.51)
void set_server_info(Game game, ServerListEntry& entry, const ByteView* info, std::int16_t ping);

}  // namespace iw_pc::net
