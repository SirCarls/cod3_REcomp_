// Requests for iw_pc::net::oob (the C++ version). The specs are
// tools/decomp/x86/specs/net_oob.py; the encodings of the structured
// arguments are described there.

#include <memory>

#include "iw_pc/net/oob.h"
#include "net_common.h"

namespace pc_check {

using iw_pc::ByteSpan;
using iw_pc::ByteView;
using iw_pc::Game;
namespace net = iw_pc::net;

namespace {

// Writes `value` big-endian at entry `index` of a buffer of 32-bit words.
void put_word(Bytes& buffer, std::size_t index, std::uint32_t value) {
    for (int j = 0; j < 4; ++j) buffer[4 * index + static_cast<std::size_t>(j)] = static_cast<std::uint8_t>(value >> (24 - 8 * j));
}

// The packet buffer the framing writes into (the largest the games send).
constexpr std::size_t kPacketBuffer = 0x20000;

// Cmd_TokenizeStringInternal: (text, limit, text buffer, argv[512]) and, for
// United Offensive, its argc global. The argv entries are offsets into the text.
std::optional<Answer> tokenize(Game game, Args& args) {
    const Bytes* text = args.str(0);
    Bytes copy = text ? *text : Bytes{};
    auto limit = static_cast<std::uint32_t>(args.int64(1));
    auto tokens = std::make_unique<net::Tokens>();
    if (text) tokens->tokenize(game, copy, limit);
    else tokens->clear();
    ByteView raw = tokens->raw_text();
    Bytes* buffer = args.buffer(2);
    if (!buffer) return std::nullopt;
    std::copy(raw.begin(), raw.end(), buffer->begin());
    Bytes* argv = args.buffer(3);
    if (!argv) return std::nullopt;
    auto starts = tokens->raw_starts();
    for (std::size_t i = 0; i < starts.size(); ++i) put_word(*argv, i, starts[i]);
    if (game == Game::CallOfDuty2) return integer(static_cast<std::int64_t>(tokens->argc()));
    Bytes* argc = args.buffer(4);
    if (!argc) return std::nullopt;
    put_word(*argc, 0, static_cast<std::uint32_t>(tokens->argc()));
    return nothing();
}

// NET_OutOfBandPrint: CoD2 (sock, address, text, sent); UO (sock, address,
// format, argument, formatted text, sent). CoD2 returns whether it sent.
Answer out_of_band_print(Game game, Args& args) {
    std::size_t text_arg = game == Game::CallOfDuty2 ? 2 : 4, sent_arg = game == Game::CallOfDuty2 ? 3 : 5;
    std::string error;
    auto to = address_from(args.bytes(1), error);
    if (!to) return Answer::fail(error);
    Bytes text(args.text(text_arg).begin(), args.text(text_arg).end());
    Bytes packet(kPacketBuffer, 0);
    auto length = net::out_of_band_print(game, text, packet);
    if (length) {
        Bytes* sent = args.buffer(sent_arg);
        if (!sent) return Answer::fail("NULL sent");
        record_sent(*sent, *to, ByteView(packet).first(*length));
    }
    return game == Game::CallOfDuty2 ? integer(length ? 1 : 0) : nothing();
}

// NET_OutOfBandData: (sock, address, data, length, sent).
Answer out_of_band_data(Game game, Args& args) {
    std::string error;
    auto to = address_from(args.bytes(1), error);
    if (!to) return Answer::fail(error);
    auto length = static_cast<std::size_t>(args.int64(3));
    if (length > args.bytes(2).size()) return Answer::fail("length past the data");
    Bytes data(args.bytes(2).begin(), args.bytes(2).begin() + static_cast<std::ptrdiff_t>(length));
    Bytes packet(kPacketBuffer, 0);
    auto n = net::out_of_band_data(game, data, packet);
    if (!n) return Answer::fail("too long");
    Bytes* sent = args.buffer(4);
    if (!sent) return Answer::fail("NULL sent");
    record_sent(*sent, *to, ByteView(packet).first(*n));
    return game == Game::CallOfDuty2 ? integer(1) : nothing();
}

Answer label(const char* text) { return string(ByteView(reinterpret_cast<const std::uint8_t*>(text), std::strlen(text))); }

// SV_ConnectionlessPacket: (address, packet) -> the handler it runs.
Answer connectionless_packet(Game game, Args& args) {
    Bytes packet(args.bytes(1).begin(), args.bytes(1).end());
    auto line = std::make_unique<net::Line>();
    auto tokens = std::make_unique<net::Tokens>();
    switch (net::server_request(game, packet, *line, *tokens)) {
        case net::ServerRequest::PunkBuster: return label("pb");
        case net::ServerRequest::Status: return label("status");
        case net::ServerRequest::Voice: return label("voice");
        case net::ServerRequest::Info: return label("info");
        case net::ServerRequest::Challenge: return label("challenge");
        case net::ServerRequest::Connect: return label("connect");
        case net::ServerRequest::IpAuthorize: return label("ipAuthorize");
        case net::ServerRequest::RemoteCommand: return label("rcon");
        case net::ServerRequest::Ignored: return label("return");
        case net::ServerRequest::Unknown: return label(game == Game::UnitedOffensive ? "unknown" : "return");
    }
    return label("return");
}

net::Setting setting(const Description& d, const char* name) {
    ByteView value = d.get(name);
    if (value.size() >= 2 && value[0] == 'n' && value[1] == ':') return net::Setting::of_number(Description::parse_int(value.subspan(2)));
    if (value.size() >= 2 && value[0] == 't' && value[1] == ':') return net::Setting::of_text(value.subspan(2));
    return {};
}

std::vector<ByteView> split(ByteView text, std::uint8_t separator) {
    std::vector<ByteView> parts;
    std::size_t start = 0;
    for (std::size_t i = 0; i <= text.size(); ++i) {
        if (i == text.size() || text[i] == separator) {
            parts.push_back(text.subspan(start, i - start));
            start = i + 1;
        }
    }
    return parts;
}

// Whether the server runs mods, answering the archive checks with the spec's answers.
bool modded(const Description& d, Game game, bool pure, net::Tokens& tokens) {
    auto answers = split(d.get("official"), ',');
    std::size_t next = 0;
    return net::runs_mods(game, pure, d.get("fs_game"), d.get("referenced"), tokens, [&](ByteView) {
        bool yes = next < answers.size() && answers[next].size() == 1 && answers[next][0] == '1';
        ++next;
        return yes;
    });
}

// The effective sv_pure: CoD2's dvar is a true/false byte, UO's a number.
bool pure_flag(Game game, std::int32_t pure) {
    return game == Game::CallOfDuty2 ? static_cast<std::uint8_t>(pure) != 0 : pure != 0;
}

int hex_digit(std::uint8_t c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return 0;
}

// SVC_Info / SVC_Status: (address, server description, sent).
Answer respond(Game game, Args& args, bool status) {
    std::string error;
    auto to = address_from(args.bytes(0), error);
    if (!to) return Answer::fail(error);
    Bytes bytes(args.bytes(1).begin(), args.bytes(1).end());
    auto d = Description::parse(bytes);
    if (!d) return Answer::fail("bad description");
    auto tokens = std::make_unique<net::Tokens>();
    std::int32_t pure = d->integer("pure");
    bool is_modded = modded(*d, game, pure_flag(game, pure), *tokens);
    Bytes text(0x8000, 0);
    std::size_t length;
    if (status) {
        std::vector<Bytes> names;
        std::vector<std::pair<std::int32_t, std::int32_t>> numbers;
        for (ByteView line : split(d->get("players"), '\n')) {
            if (line.empty()) continue;
            auto parts = split(line, ' ');
            auto number = [&](std::size_t k) { return k < parts.size() ? Description::parse_int(parts[k]) : 0; };
            numbers.emplace_back(number(0), number(1));
            ByteView hex = parts.size() > 2 ? parts[2] : ByteView{};
            Bytes name;
            for (std::size_t i = 0; i + 1 < hex.size(); i += 2) {
                name.push_back(static_cast<std::uint8_t>(hex_digit(hex[i]) * 16 + hex_digit(hex[i + 1])));
            }
            names.push_back(std::move(name));
        }
        std::vector<net::PlayerStatus> players;
        for (std::size_t i = 0; i < names.size(); ++i) players.push_back({numbers[i].first, numbers[i].second, names[i]});
        net::ServerStatus server;
        server.server_info = d->get("server_info");
        server.challenge = d->get("challenge");
        server.restricted = d->integer("restricted") != 0;
        server.players = players;
        server.disable_client_console = d->integer("disable_console");
        server.password = d->get("password");
        server.modded = is_modded;
        length = net::status_response(game, server, text);
    } else {
        std::vector<std::int32_t> states;
        for (ByteView t : split(d->get("states"), ',')) {
            if (!t.empty()) states.push_back(Description::parse_int(t));
        }
        net::ServerInfo server;
        server.challenge = d->get("challenge");
        server.hostname = d->get("hostname");
        server.mapname = d->get("mapname");
        server.gametype = d->get("gametype");
        server.max_clients = d->integer("max_clients");
        server.private_clients = d->integer("private_clients");
        server.client_states = states;
        server.pure = pure;
        server.server_iwd_count = d->integer("server_iwds");
        server.min_ping = d->integer("min_ping");
        server.max_ping = d->integer("max_ping");
        server.fs_game = d->get("fs_game");
        server.allow_anonymous = d->integer("allow_anonymous");
        server.disable_client_console = d->integer("disable_console");
        server.password = d->get("password");
        server.friendly_fire = setting(*d, "ff");
        server.killcam = setting(*d, "kc");
        server.timeouts_allowed = setting(*d, "timeouts");
        server.allow_jeeps = setting(*d, "jeeps");
        server.allow_tanks = setting(*d, "tanks");
        server.dedicated = d->integer("dedicated") != 0;
        server.modded = is_modded;
        server.voice = d->integer("voice");
        server.punkbuster = d->integer("pb");
        length = net::info_response(game, server, text);
    }
    Bytes packet(kPacketBuffer, 0);
    if (auto n = net::out_of_band_print(game, ByteView(text).first(length), packet)) {
        Bytes* sent = args.buffer(2);
        if (!sent) return Answer::fail("NULL sent");
        record_sent(*sent, *to, ByteView(packet).first(*n));
    }
    return nothing();
}

// Records a packet NET_SendPacket would hand to sendto (IPv4 and broadcast).
void record_if_sent(Bytes& out, const net::NetAddress& to_arg, ByteView packet) {
    if (to_arg.kind == net::AddressType::Ip || to_arg.kind == net::AddressType::Broadcast) {
        net::NetAddress to = to_arg;
        if (to.kind == net::AddressType::Broadcast) to.ip = {255, 255, 255, 255};
        record_sent(out, to, packet);
    }
}

// challenge_t's size and where Call of Duty 2 keeps the PunkBuster hash.
std::size_t challenge_stride(Game game) { return game == Game::CallOfDuty2 ? 0x74 : 0x54; }
constexpr std::size_t kGuidAt = 0x51;

std::int32_t read_i32(ByteView b, std::size_t at) {
    return static_cast<std::int32_t>(static_cast<std::uint32_t>(b[at]) | static_cast<std::uint32_t>(b[at + 1]) << 8 |
                                     static_cast<std::uint32_t>(b[at + 2]) << 16 | static_cast<std::uint32_t>(b[at + 3]) << 24);
}

// The challenge table in argument 1 (the spec's canonical slots).
std::unique_ptr<net::ChallengeTable> read_table(Game game, Args& args, std::string& error) {
    std::size_t stride = challenge_stride(game);
    Bytes* raw = args.buffer(1);
    if (!raw) {
        error = "NULL table";
        return nullptr;
    }
    if (raw->size() != net::kMaxChallenges * stride) {
        error = "table size";
        return nullptr;
    }
    auto table = std::make_unique<net::ChallengeTable>();
    for (std::size_t i = 0; i < net::kMaxChallenges; ++i) {
        ByteView bytes = ByteView(*raw).subspan(i * stride, stride);
        auto address = address_from(bytes, error);
        if (!address) return nullptr;
        net::ChallengeSlot& slot = table->slots[i];
        slot.address = *address;
        slot.challenge = read_i32(bytes, 0x14);
        slot.time = read_i32(bytes, 0x18);
        slot.ping_time = read_i32(bytes, 0x1C);
        slot.first_time = read_i32(bytes, 0x20);
        slot.first_ping = read_i32(bytes, 0x24);
        slot.connected = read_i32(bytes, 0x28) != 0;
        if (game == Game::CallOfDuty2) std::copy(bytes.begin() + kGuidAt, bytes.begin() + kGuidAt + 33, slot.punkbuster_guid.begin());
    }
    return table;
}

// Writes the table's fields back into argument 1.
void write_table(Game game, const net::ChallengeTable& table, Args& args) {
    std::size_t stride = challenge_stride(game);
    Bytes& raw = *args.buffer(1);
    for (std::size_t i = 0; i < net::kMaxChallenges; ++i) {
        const net::ChallengeSlot& slot = table.slots[i];
        std::uint8_t* bytes = raw.data() + i * stride;
        auto address = address_bytes(slot.address);
        std::copy(address.begin(), address.end(), bytes);
        std::int32_t fields[] = {slot.challenge, slot.time, slot.ping_time, slot.first_time, slot.first_ping, slot.connected ? 1 : 0};
        for (std::size_t k = 0; k < 6; ++k) {
            auto v = static_cast<std::uint32_t>(fields[k]);
            for (int j = 0; j < 4; ++j) bytes[0x14 + 4 * k + static_cast<std::size_t>(j)] = static_cast<std::uint8_t>(v >> (8 * j));
        }
        if (game == Game::CallOfDuty2) std::copy(slot.punkbuster_guid.begin(), slot.punkbuster_guid.end(), bytes + kGuidAt);
    }
}

// SV_GetChallenge: (from, table, world, sent) -> "authorize" or "return".
Answer get_challenge(Game game, Args& args) {
    std::string error;
    auto from = address_from(args.bytes(0), error);
    if (!from) return Answer::fail(error);
    Bytes world(args.bytes(2).begin(), args.bytes(2).end());
    auto d = Description::parse(world);
    if (!d) return Answer::fail("bad description");
    auto table = read_table(game, args, error);
    if (!table) return Answer::fail(error);
    net::ChallengeRequest request;
    request.from = *from;
    request.time = d->integer("time");
    request.lan_authorize = d->integer("lan_authorize") != 0;
    request.from_lan = d->integer("from_lan") != 0;
    request.authorize_known = true;
    request.last_master_contact = d->integer("last_master");
    request.from_master = d->integer("from_master") != 0;
    if (d->integer("guid_given") != 0) request.punkbuster_guid = d->get("guid");
    std::int32_t number = d->integer("challenge");
    auto answer = net::get_challenge(game, *table, request, [number] { return number; });
    write_table(game, *table, args);
    if (answer.kind == net::ChallengeAnswer::Kind::Respond) {
        const net::ChallengeSlot& slot = table->slots[answer.slot];
        std::array<std::uint8_t, 64> text{};
        std::size_t n = net::challenge_response_text(game, slot.challenge, d->integer("only_visible"), text);
        std::array<std::uint8_t, 80> packet{};
        auto length = net::out_of_band_print(game, ByteView(text).first(n), packet);
        if (!length) return Answer::fail("too long");
        Bytes* sent = args.buffer(3);
        if (!sent) return Answer::fail("NULL sent");
        record_if_sent(*sent, slot.address, ByteView(packet).first(*length));
        return label("return");
    }
    return label(answer.kind == net::ChallengeAnswer::Kind::Authorize ? "authorize" : "return");
}

double parse_float(ByteView text) {
    std::string s(text.begin(), text.end());
    if (s.empty()) return 0;
    try {
        std::size_t used = 0;
        double v = std::stod(s, &used);
        return used == s.size() ? v : 0;
    } catch (...) {
        return 0;
    }
}

// SV_DirectConnect: (from, table, world, sent) -> "new <slot>", "reconnect <slot>" or "return".
Answer direct_connect(Game game, Args& args) {
    std::string error;
    auto from = address_from(args.bytes(0), error);
    if (!from) return Answer::fail(error);
    Bytes world(args.bytes(2).begin(), args.bytes(2).end());
    auto d = Description::parse(world);
    if (!d) return Answer::fail("bad description");
    auto table = read_table(game, args, error);
    if (!table) return Answer::fail(error);
    std::vector<net::ClientSlotInfo> clients;
    ByteView raw_clients = d->get("clients");
    for (std::size_t at = 0; at + 32 <= raw_clients.size(); at += 32) {
        ByteView c = raw_clients.subspan(at, 32);
        auto be = [&](std::size_t k) {
            return static_cast<std::int32_t>(static_cast<std::uint32_t>(c[k]) << 24 | static_cast<std::uint32_t>(c[k + 1]) << 16 |
                                             static_cast<std::uint32_t>(c[k + 2]) << 8 | c[k + 3]);
        };
        auto address = address_from(c.subspan(12), error);
        if (!address) return Answer::fail(error);
        clients.push_back({be(0), *address, be(4), be(8)});
    }
    net::ConnectRequest request;
    request.from = *from;
    request.userinfo = d->get("userinfo");
    request.time = d->integer("time");
    request.reconnect_limit = d->integer("reconnect_limit");
    request.max_clients = d->integer("max_clients");
    request.private_clients = d->integer("private_clients");
    request.private_password = d->get("password");
    request.min_ping = parse_float(d->get("min_ping"));
    request.max_ping = parse_float(d->get("max_ping"));
    request.from_lan = d->integer("from_lan") != 0;
    request.clients = clients;
    auto decision = net::direct_connect(game, *table, request);
    write_table(game, *table, args);
    if (auto text = decision.reply(game)) {
        std::array<std::uint8_t, 80> packet{};
        auto length = net::out_of_band_print(game, *text, packet);
        if (!length) return Answer::fail("too long");
        Bytes* sent = args.buffer(3);
        if (!sent) return Answer::fail("NULL sent");
        record_if_sent(*sent, *from, ByteView(packet).first(*length));
    }
    std::string out = "return";
    if (decision.kind == net::ConnectDecision::Kind::Reconnect) out = "reconnect " + std::to_string(decision.slot);
    if (decision.kind == net::ConnectDecision::Kind::NewSlot) out = "new " + std::to_string(decision.slot);
    return label(out.c_str());
}

// Where each game keeps a serverInfo_t field.
struct EntryLayout {
    std::vector<std::pair<std::size_t, std::size_t>> bytes;   // (offset, which)
    std::vector<std::pair<std::size_t, std::size_t>> shorts;  // (offset, which)
    std::vector<std::array<std::size_t, 3>> texts;            // (offset, which, size)
};

// Byte fields: 0 net_type, 1 clients, 2 max_clients, 3 allow_anonymous, 4 password,
// 5 pure, 6 friendly_fire, 7 killcam, 8 console_disabled, 9 hardware, 10 modded,
// 11 voice, 12 punkbuster. Shorts: 0 min_ping, 1 max_ping, 2 ping, 3 timeouts,
// 4 jeeps, 5 tanks. Texts: 0 hostname, 1 mapname, 2 game, 3 gametype.
const EntryLayout& entry_layout(Game game) {
    static const EntryLayout cod2{
        {{0x14, 0}, {0x15, 1}, {0x16, 2}, {0x18, 3}, {0x19, 4}, {0x1A, 5}, {0x1B, 6}, {0x1C, 7}, {0x1D, 8}, {0x1E, 9}, {0x1F, 10}, {0x20, 11}, {0x21, 12}},
        {{0x24, 0}, {0x26, 1}, {0x28, 2}},
        {{0x2A, 0, 32}, {0x4A, 1, 32}, {0x6A, 2, 24}, {0x82, 3, 16}}};
    static const EntryLayout uo{
        {{0x14, 0}, {0x15, 1}, {0x16, 2}, {0x18, 3}, {0x19, 4}, {0x1A, 5}, {0x1B, 6}, {0x1C, 7}, {0x1D, 8}, {0x1E, 9}, {0x1F, 10}, {0x20, 12}},
        {{0x22, 0}, {0x24, 1}, {0x26, 2}, {0x28, 3}, {0x2A, 4}, {0x2C, 5}},
        {{0x2E, 0, 32}, {0x4E, 1, 32}, {0x6E, 2, 24}, {0x86, 3, 16}}};
    return game == Game::CallOfDuty2 ? cod2 : uo;
}

std::array<std::uint8_t*, 13> entry_bytes(net::ServerListEntry& e) {
    return {&e.net_type, &e.clients, &e.max_clients, &e.allow_anonymous, &e.password, &e.pure, &e.friendly_fire,
            &e.killcam, &e.console_disabled, &e.hardware, &e.modded, &e.voice, &e.punkbuster};
}

std::array<std::int16_t*, 6> entry_shorts(net::ServerListEntry& e) {
    return {&e.min_ping, &e.max_ping, &e.ping, &e.timeouts_allowed, &e.jeeps, &e.tanks};
}

std::array<ByteSpan, 4> entry_texts(net::ServerListEntry& e) { return {e.hostname, e.mapname, e.game, e.gametype}; }

// CL_SetServerInfo: (entry, info or NULL, ping).
Answer set_server_info(Game game, Args& args) {
    const Bytes* info = args.str(1);
    Bytes info_copy = info ? *info : Bytes{};
    ByteView info_view = info_copy;
    auto ping = static_cast<std::int16_t>(args.int64(2));
    const EntryLayout& layout = entry_layout(game);
    Bytes* raw = args.buffer(0);
    if (!raw) return Answer::fail("NULL entry");
    net::ServerListEntry entry;
    for (auto [at, which] : layout.bytes) *entry_bytes(entry)[which] = (*raw)[at];
    for (auto [at, which] : layout.shorts) {
        *entry_shorts(entry)[which] = static_cast<std::int16_t>((*raw)[at] | (*raw)[at + 1] << 8);
    }
    for (auto t : layout.texts) {
        std::copy(raw->begin() + static_cast<std::ptrdiff_t>(t[0]), raw->begin() + static_cast<std::ptrdiff_t>(t[0] + t[2]),
                  entry_texts(entry)[t[1]].begin());
    }
    net::set_server_info(game, entry, info ? &info_view : nullptr, ping);
    for (auto [at, which] : layout.bytes) (*raw)[at] = *entry_bytes(entry)[which];
    for (auto [at, which] : layout.shorts) {
        auto v = static_cast<std::uint16_t>(*entry_shorts(entry)[which]);
        (*raw)[at] = static_cast<std::uint8_t>(v);
        (*raw)[at + 1] = static_cast<std::uint8_t>(v >> 8);
    }
    for (auto t : layout.texts) {
        ByteSpan field = entry_texts(entry)[t[1]];
        std::copy(field.begin(), field.end(), raw->begin() + static_cast<std::ptrdiff_t>(t[0]));
    }
    return nothing();
}

// CL_ServerInfoPacket's protocol test: (from, msg holding the info string,
// debug_protocol) -> "accepted" or "return".
Answer server_info_packet(Game game, Args& args) {
    Bytes debug(args.bytes(2).begin(), args.bytes(2).end());
    auto zero = std::find(debug.begin(), debug.end(), std::uint8_t{0});
    if (zero == debug.end() || debug.end() - zero < 5) return Answer::fail("bad debug_protocol");
    std::size_t z = static_cast<std::size_t>(zero - debug.begin());
    std::uint32_t bits = static_cast<std::uint32_t>(debug[z + 1]) << 24 | static_cast<std::uint32_t>(debug[z + 2]) << 16 |
                         static_cast<std::uint32_t>(debug[z + 3]) << 8 | debug[z + 4];
    float value;
    std::memcpy(&value, &bits, 4);
    ByteView msg = args.bytes(1);
    ByteView data = msg.subspan(20);
    std::size_t end = 0;
    while (end < data.size() && data[end] != 0) ++end;
    bool accepted = net::info_response_accepted(game, data.first(end), ByteView(debug).first(z), value);
    return label(accepted ? "accepted" : "return");
}

}  // namespace

std::optional<Answer> dispatch_netchan(Game game, const std::string& f, Args& args);

std::optional<Answer> dispatch_oob(Game game, const std::string& f, Args& args) {
    if (auto answer = dispatch_netchan(game, f, args)) return answer;
    if (f == "Cmd_TokenizeStringInternal") return tokenize(game, args);
    if (f == "NET_OutOfBandPrint") return out_of_band_print(game, args);
    if (f == "NET_OutOfBandData") return out_of_band_data(game, args);
    if (f == "SV_ConnectionlessPacket") return connectionless_packet(game, args);
    if (f == "SVC_Info") return respond(game, args, false);
    if (f == "SVC_Status") return respond(game, args, true);
    if (f == "atol") return integer(net::atol(args.text(0)));
    if (f == "SV_GetChallenge") return get_challenge(game, args);
    if (f == "SV_DirectConnect") return direct_connect(game, args);
    if (f == "CL_SetServerInfo") return set_server_info(game, args);
    if (f == "CL_ServerInfoPacket") return server_info_packet(game, args);
    if (f == "NET_CompareAdrSigned" || f == "NET_CompareBaseAdrSigned") {
        std::string error;
        auto a = address_from(args.bytes(0), error);
        if (!a) return Answer::fail(error);
        auto b = address_from(args.bytes(1), error);
        if (!b) return Answer::fail(error);
        bool same = f == "NET_CompareAdrSigned" ? net::same_address(*a, *b) : net::same_base_address(game, *a, *b);
        return integer(same ? 1 : 0);
    }
    return std::nullopt;
}

}  // namespace pc_check
