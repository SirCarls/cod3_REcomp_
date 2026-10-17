#include "iw_pc/net/oob.h"

#include <climits>
#include <cstring>
#include <string>

#include "iw_pc/crt.h"
#include "iw_pc/cstr.h"
#include "iw_pc/shared/info.h"
#include "iw_pc/shared/strings.h"

namespace iw_pc::net {

namespace strings = iw_pc::shared::strings;
namespace info = iw_pc::shared::info;

namespace {

// What the tokenizer calls a space: any byte up to ' ' as a signed char,
// so 0x80 to 0xFF are spaces too.
bool is_space(std::uint8_t c) { return static_cast<std::int8_t>(c) <= ' '; }

ByteView after_prefix(ByteView packet) { return packet.size() > 4 ? packet.subspan(4) : ByteView{}; }

std::optional<std::size_t> frame(ByteView body, ByteSpan out) {
    std::size_t length = 4 + body.size();
    if (length > out.size()) return std::nullopt;
    std::memcpy(out.data(), kConnectionlessPrefix.data(), 4);
    if (!body.empty()) std::memcpy(out.data() + 4, body.data(), body.size());
    return length;
}

// A number as va("%i", n) writes it.
std::string decimal(std::int32_t n) { return std::to_string(n); }

ByteView view(const std::string& s) { return ByteView(reinterpret_cast<const std::uint8_t*>(s.data()), s.size()); }

// Appends bytes to `out`, up to `limit` and out's end (the rest is cut).
class Writer {
public:
    Writer(ByteSpan out, std::size_t limit) : out_(out), limit_(std::min(limit, out.size())) {}
    void put(ByteView bytes) {
        std::size_t n = std::min(bytes.size(), limit_ - len_);
        if (n > 0) std::memcpy(out_.data() + len_, bytes.data(), n);
        len_ += n;
    }
    std::size_t len() const { return len_; }

private:
    ByteSpan out_;
    std::size_t limit_;
    std::size_t len_ = 0;
};

// An info string being built (MAX_INFO_STRING bytes, as the games' arrays).
class InfoBuilder {
public:
    std::array<std::uint8_t, kMaxLine> text{};
    // Info_SetValueForKey: a refused pair is left out, as the originals do.
    void set(ByteView key, ByteView value) { (void)info::set_value_for_key(text, key, value); }
    void set_number(ByteView key, std::int32_t value) { set(key, view(decimal(value))); }
    ByteView bytes() const { return cstr::text(text); }
};

// The players in the first private_clients slots, and in all max_clients.
std::pair<std::int32_t, std::int32_t> count_players(const ServerInfo& info) {
    auto connected = [&](std::int32_t slot) {
        return slot >= 0 && static_cast<std::size_t>(slot) < info.client_states.size() &&
               info.client_states[static_cast<std::size_t>(slot)] >= kClientStateConnected;
    };
    std::int32_t private_count = 0;
    for (std::int32_t i = 0; i < std::max(info.private_clients, 0); ++i) private_count += connected(i) ? 1 : 0;
    std::int32_t public_count = 0;
    for (std::int32_t i = std::max(info.private_clients, 0); i < info.max_clients; ++i) public_count += connected(i) ? 1 : 0;
    return {private_count, private_count + public_count};
}

// A by-name number setting as Call of Duty 2's SVC_Info reads it.
std::int32_t cod2_number(const Setting& setting) {
    switch (setting.kind) {
        case Setting::Kind::Missing: return 0;
        case Setting::Kind::Number: return setting.number;
        case Setting::Kind::Text: return atol(setting.text);
    }
    return 0;
}

// United Offensive's by-name number settings: sent as their text when its number isn't 0.
void uo_flag(InfoBuilder& builder, ByteView key, const Setting& setting) {
    if (setting.kind == Setting::Kind::Number) {
        if (setting.number != 0) builder.set_number(key, setting.number);
    } else if (setting.kind == Setting::Kind::Text) {
        if (atol(setting.text) != 0) builder.set(key, setting.text);
    }
}

ByteView value_or_empty(ByteView text, ByteView key) {
    auto value = info::value_for_key(text, key);
    return value ? value.value() : ByteView{};
}

}  // namespace

bool is_connectionless(ByteView packet) {
    return packet.size() >= 4 && std::memcmp(packet.data(), kConnectionlessPrefix.data(), 4) == 0;
}

void Line::read(Game game, ByteView packet) {
    ByteView body = after_prefix(packet);
    std::size_t len = 0;
    for (std::uint8_t byte : body) {
        if (byte == '\n' || byte == 0 || len == kMaxLine - 1) break;
        std::uint8_t c = byte;
        if (byte == '%') c = '.';
        else if (byte == 0x92) c = '\'';
        else if (byte >= 0x80 && game == Game::UnitedOffensive) c = '.';
        text_[len++] = c;
    }
    len_ = len;
}

void Line::set(ByteView text) {
    std::size_t len = std::min(text.size(), kMaxLine - 1);
    if (len > 0) std::memcpy(text_.data(), text.data(), len);
    len_ = len;
}

ByteView Tokens::argv(std::size_t i) const {
    if (i >= count_) return {};
    std::size_t start = starts_[i];
    std::size_t len = 0;
    while (start + len < used_ && text_[start + len] != 0) ++len;
    return ByteView(text_).subspan(start, len);
}

void Tokens::clear() {
    used_ = 0;
    written_ = 0;
    count_ = 0;
}

void Tokens::tokenize(Game game, ByteView line_arg, std::uint32_t limit) {
    ByteView line = line_arg.first(std::min(line_arg.size(), kTokenTextSize - 1));
    line = cstr::text(line);
    // The original reads the C string; past its end it sees the terminator.
    auto at = [&](std::size_t i) -> std::uint8_t { return i < line.size() ? line[i] : 0; };
    std::size_t p = 0, out = 0, written = 0;
    std::uint32_t remaining = limit;
    for (;;) {
        --remaining;
        if (remaining == 0) {
            // The rest of the line, as it is, is the last word.
            if (at(p) != 0) {
                starts_[written++] = static_cast<std::uint16_t>(out);
                ByteView rest = line.subspan(p);
                std::memcpy(text_.data() + out, rest.data(), rest.size());
                out += rest.size();
                text_[out++] = 0;
            }
            count_ = written;
            break;
        }
        // Spaces and comments before the word.
        bool ended = false;
        for (;;) {
            std::uint8_t c = at(p);
            if (c == 0) {
                ended = true;
                break;
            }
            while (is_space(c)) {
                ++p;
                c = at(p);
                if (c == 0) break;
            }
            if (c == 0) {
                ended = true;
                break;
            }
            if (c == '/') {
                std::uint8_t next = at(p + 1);
                if (next == '/') {
                    ended = true;
                    break;
                }
                if (next == '*') {
                    // Up to the first */, counting the * of the /*.
                    std::uint8_t d = c;
                    for (;;) {
                        if (d == '*' && at(p + 1) == '/') break;
                        d = at(p + 1);
                        ++p;
                        if (d == 0) break;
                    }
                    if (d == 0) {
                        ended = true;
                        break;
                    }
                    p += 2;
                    continue;
                }
            }
            break;
        }
        if (ended) {
            count_ = written;
            break;
        }
        // The word.
        starts_[written++] = static_cast<std::uint16_t>(out);
        if (at(p) == '"') {
            ++p;
            std::uint8_t d = at(p);
            while (d != 0 && d != '"') {
                if (game == Game::CallOfDuty2 && d == '\\' && at(p + 1) == '"') ++p;
                text_[out++] = at(p);
                d = at(p + 1);
                ++p;
            }
            text_[out++] = 0;
            if (at(p) == 0) {
                count_ = written;
                break;
            }
            ++p;
        } else {
            std::uint8_t d = at(p);
            if (!is_space(d)) {
                for (;;) {
                    if (d == '"') break;
                    if (d == '/' && (at(p + 1) == '/' || at(p + 1) == '*')) break;
                    text_[out++] = d;
                    d = at(p + 1);
                    ++p;
                    if (is_space(d)) break;
                }
            }
            text_[out++] = 0;
        }
        // One space after the word is consumed with it.
        std::uint8_t c = at(p);
        if (c == 0) {
            count_ = written;
            break;
        }
        if (is_space(c)) ++p;
        if (written == kMaxTokens) {
            count_ = game == Game::CallOfDuty2 ? 0 : written;
            break;
        }
    }
    used_ = out;
    written_ = written;
}

std::optional<std::size_t> out_of_band_print(Game game, ByteView text_arg, ByteSpan out) {
    ByteView text = cstr::text(text_arg);
    std::size_t limit = game == Game::CallOfDuty2 ? kMaxPrintTextCod2 : kMaxPrintTextUo;
    if (text.size() > limit) return std::nullopt;
    return frame(text, out);
}

std::optional<std::size_t> out_of_band_data(Game game, ByteView data, ByteSpan out) {
    std::size_t limit = game == Game::CallOfDuty2 ? kMaxDataCod2 : kMaxDataUo;
    if (data.size() > limit) return std::nullopt;
    return frame(data, out);
}

bool is_punkbuster(ByteView packet) { return strings::stricmpn(after_prefix(packet), bytes("pb_"), 3) == 0; }

bool connect_is_compressed(Game game, ByteView packet) {
    return game == Game::UnitedOffensive && strings::strncmp(after_prefix(packet), bytes("connect"), 7) == 0;
}

ServerRequest server_request(Game game, ByteView packet, Line& line, Tokens& tokens) {
    if (is_punkbuster(packet)) return ServerRequest::PunkBuster;
    line.read(game, packet);
    tokens.tokenize(game, line.bytes(), 0);
    ByteView command = tokens.argv(0);
    std::int32_t limit = game == Game::CallOfDuty2 ? strings::kStricmpLimitCod2 : strings::kStricmpLimitUo;
    auto is = [&](ByteView name) { return strings::stricmpn(name, command, limit) == 0; };
    bool cod2 = game == Game::CallOfDuty2;
    if (is(bytes("getstatus"))) return ServerRequest::Status;
    if (cod2 && is(bytes("v"))) return ServerRequest::Voice;
    if (is(bytes("getinfo"))) return ServerRequest::Info;
    if (is(bytes("getchallenge"))) return ServerRequest::Challenge;
    if (is(bytes("connect"))) return ServerRequest::Connect;
    if (is(bytes("ipAuthorize"))) return ServerRequest::IpAuthorize;
    if (is(bytes("rcon"))) return ServerRequest::RemoteCommand;
    if (!cod2 && is(bytes("disconnect"))) return ServerRequest::Ignored;
    return ServerRequest::Unknown;
}

std::int32_t atol(ByteView text) {
    auto at = [&](std::size_t i) -> std::uint8_t { return i < text.size() ? text[i] : 0; };
    std::size_t i = 0;
    while (at(i) == ' ' || (at(i) >= '\t' && at(i) <= '\r')) ++i;
    std::uint8_t sign = at(i);
    if (sign == '-' || sign == '+') ++i;
    std::uint32_t total = 0;
    while (at(i) >= '0' && at(i) <= '9') {
        total = total * 10 + static_cast<std::uint32_t>(at(i) - '0');
        ++i;
    }
    if (sign == '-') total = 0 - total;
    return static_cast<std::int32_t>(total);
}

bool runs_mods(Game game, bool pure, ByteView fs_game, ByteView referenced_archives, Tokens& tokens,
               const std::function<bool(ByteView)>& is_official) {
    if (!pure || !fs_game.empty()) return true;
    if (referenced_archives.empty()) return false;
    tokens.tokenize(game, referenced_archives, 0);
    for (std::size_t i = 0; i < tokens.argc(); ++i) {
        if (!is_official(tokens.argv(i))) return true;
    }
    return false;
}

std::size_t info_response(Game game, const ServerInfo& info, ByteSpan out) {
    bool cod2 = game == Game::CallOfDuty2;
    auto [private_players, players] = count_players(info);
    InfoBuilder b;
    b.set(bytes("challenge"), info.challenge);
    b.set_number(bytes("protocol"), cod2 ? 118 : 22);
    b.set(bytes("hostname"), info.hostname);
    b.set(bytes("mapname"), info.mapname);
    if (players != 0) b.set_number(bytes("clients"), players);
    auto slots = static_cast<std::int32_t>(static_cast<std::uint32_t>(info.max_clients) -
                                           static_cast<std::uint32_t>(info.private_clients) +
                                           static_cast<std::uint32_t>(private_players));
    if (slots > 0) b.set_number(bytes("sv_maxclients"), slots);
    b.set(bytes("gametype"), info.gametype);
    if (cod2) {
        if (static_cast<std::uint8_t>(info.pure) != 0 || info.server_iwd_count != 0) b.set(bytes("pure"), bytes("1"));
    } else if (info.pure != 0) {
        b.set_number(bytes("pure"), info.pure);
    }
    if (info.min_ping != 0) b.set_number(bytes("minPing"), info.min_ping);
    if (info.max_ping != 0) b.set_number(bytes("maxPing"), info.max_ping);
    if (!info.fs_game.empty()) b.set(bytes("game"), info.fs_game);
    if (cod2) {
        auto anonymous = static_cast<std::uint8_t>(info.allow_anonymous);
        if (anonymous != 0) b.set_number(bytes("sv_allowAnonymous"), anonymous);
        auto console = static_cast<std::uint8_t>(info.disable_client_console);
        if (console != 0) b.set_number(bytes("con_disabled"), console);
    } else if (info.allow_anonymous != 0) {
        b.set_number(bytes("sv_allowAnonymous"), info.allow_anonymous);
    }
    if (!info.password.empty()) b.set(bytes("pswrd"), bytes("1"));
    if (cod2) {
        std::int32_t ff = cod2_number(info.friendly_fire);
        if (ff != 0) b.set_number(bytes("ff"), ff);
        std::int32_t kc = cod2_number(info.killcam);
        if (kc != 0) b.set_number(bytes("kc"), kc);
        b.set_number(bytes("hw"), info.dedicated ? 2 : 5);
        b.set_number(bytes("mod"), info.modded ? 1 : 0);
        b.set_number(bytes("voice"), static_cast<std::uint8_t>(info.voice));
        b.set_number(bytes("pb"), info.punkbuster);
    } else {
        uo_flag(b, bytes("ff"), info.friendly_fire);
        uo_flag(b, bytes("kc"), info.killcam);
        uo_flag(b, bytes("timeoutsAllowed"), info.timeouts_allowed);
        uo_flag(b, bytes("jps"), info.allow_jeeps);
        uo_flag(b, bytes("tnk"), info.allow_tanks);
        b.set_number(bytes("hw"), info.dedicated ? 2 : 4);
        b.set_number(bytes("pb"), info.punkbuster);
        b.set_number(bytes("mod"), info.modded ? 1 : 0);
    }
    Writer text(out, cod2 ? kMaxInfoResponseCod2 : SIZE_MAX);
    text.put(bytes("infoResponse\n"));
    text.put(b.bytes());
    return text.len();
}

std::size_t status_response(Game game, const ServerStatus& status, ByteSpan out) {
    bool cod2 = game == Game::CallOfDuty2;
    InfoBuilder b;
    ByteView base = status.server_info.first(std::min(status.server_info.size(), kMaxLine - 1));
    if (!base.empty()) std::memcpy(b.text.data(), base.data(), base.size());
    b.set(bytes("challenge"), status.challenge);
    if (status.restricted) {
        std::array<std::uint8_t, kMaxLine> keywords{};
        ByteView old = value_or_empty(b.bytes(), bytes("sv_keywords"));
        Writer w(keywords, kMaxLine - 1);
        w.put(bytes("demo "));
        w.put(old);
        b.set(bytes("sv_keywords"), ByteView(keywords).first(w.len()));
    }
    if (!cod2 && status.disable_client_console != 0) b.set_number(bytes("con_disabled"), status.disable_client_console);
    b.set(bytes("pswrd"), status.password.empty() ? bytes("0") : bytes("1"));
    b.set_number(bytes("mod"), status.modded ? 1 : 0);

    Writer text(out, cod2 ? kMaxStatusResponseCod2 : SIZE_MAX);
    text.put(bytes("statusResponse\n"));
    text.put(b.bytes());
    text.put(bytes("\n"));
    std::size_t list_limit = cod2 ? 0x20000 : 0x8000;
    std::size_t list_length = 0;
    for (const PlayerStatus& player : status.players) {
        std::array<std::uint8_t, kMaxLine> line{};
        Writer w(line, kMaxLine - 1);
        w.put(view(decimal(player.score)));
        w.put(bytes(" "));
        w.put(view(decimal(player.ping)));
        w.put(bytes(" \""));
        w.put(cstr::text(player.name));
        w.put(bytes("\"\n"));
        std::size_t n = w.len();
        if (list_length + n >= list_limit) break;
        list_length += n;
        text.put(ByteView(line).first(n));
    }
    return text.len();
}

bool same_address(const NetAddress& a, const NetAddress& b) {
    if (a.kind != b.kind) return false;
    switch (a.kind) {
        case AddressType::Ip: return a.port == b.port && a.ip == b.ip;
        case AddressType::Ipx: return a.port == b.port && a.ipx == b.ipx;
        default: return true;
    }
}

std::size_t ChallengeTable::find_or_add(const NetAddress& from, std::int32_t time,
                                        const std::function<std::int32_t()>& new_challenge) {
    std::size_t oldest = 0;
    std::int32_t oldest_time = INT32_MAX;
    for (std::size_t i = 0; i < slots.size(); ++i) {
        const ChallengeSlot& slot = slots[i];
        if (!slot.connected && same_address(from, slot.address)) return i;
        if (slot.time < oldest_time) {
            oldest_time = slot.time;
            oldest = i;
        }
    }
    ChallengeSlot& slot = slots[oldest];
    slot.challenge = new_challenge();
    slot.address = from;
    slot.first_time = time;
    slot.first_ping = 0;
    slot.time = time;
    slot.connected = false;
    return oldest;
}

ChallengeAnswer get_challenge(Game game, ChallengeTable& table, const ChallengeRequest& request,
                              const std::function<std::int32_t()>& new_challenge) {
    std::size_t slot = table.find_or_add(request.from, request.time, new_challenge);
    if (!request.lan_authorize && request.from_lan) {
        table.slots[slot].ping_time = request.time;
        return {ChallengeAnswer::Kind::Respond, slot};
    }
    if (!request.authorize_known) return {ChallengeAnswer::Kind::AuthorizeUnknown, 0};
    auto wrap_sub = [](std::int32_t a, std::int32_t b) {
        return static_cast<std::int32_t>(static_cast<std::uint32_t>(a) - static_cast<std::uint32_t>(b));
    };
    std::int32_t asking_for = wrap_sub(request.time, table.slots[slot].first_time);
    bool timed_out = game == Game::CallOfDuty2
                         ? wrap_sub(request.time, request.last_master_contact) > 1200000 && asking_for > 7000
                         : asking_for > 5000;
    if (timed_out && !request.from_master) {
        table.slots[slot].ping_time = request.time;
        return {ChallengeAnswer::Kind::Respond, slot};
    }
    if (game == Game::CallOfDuty2 && request.punkbuster_guid) {
        // strncpy 32 characters, then a terminator.
        auto& guid = table.slots[slot].punkbuster_guid;
        crt::strncpy(ByteSpan(guid).first(32), *request.punkbuster_guid);
        guid[32] = 0;
    }
    return {ChallengeAnswer::Kind::Authorize, slot};
}

std::size_t challenge_response_text(Game game, std::int32_t challenge, std::int32_t only_visible_clients, ByteSpan out) {
    Writer w(out, SIZE_MAX);
    w.put(bytes("challengeResponse "));
    w.put(view(decimal(challenge)));
    if (game == Game::UnitedOffensive && only_visible_clients != 0) {
        w.put(bytes(" "));
        w.put(view(decimal(only_visible_clients)));
    }
    return w.len();
}

bool same_base_address(Game game, const NetAddress& a, const NetAddress& b) {
    if (a.kind != b.kind) return false;
    switch (a.kind) {
        case AddressType::Loopback: return game == Game::UnitedOffensive || a.port == b.port;
        case AddressType::Bot: return a.port == b.port;
        case AddressType::Ip: return a.ip == b.ip;
        case AddressType::Ipx: return a.ipx == b.ipx;
        default: return true;
    }
}

std::optional<ByteView> ConnectDecision::reply(Game game) const {
    switch (kind) {
        case Kind::WrongProtocol:
            return game == Game::CallOfDuty2 ? bytes("error\nEXE_SERVER_IS_DIFFERENT_VER\x15" "1.3\n")
                                             : bytes("error\nEXE_SERVER_IS_DIFFERENT_VER\x15" "1.51\n");
        case Kind::BadChallenge: return bytes("error\nEXE_BAD_CHALLENGE");
        case Kind::PingTooLow: return bytes("error\nEXE_ERR_HIGH_PING_ONLY");
        case Kind::PingTooHigh: return bytes("error\nEXE_ERR_LOW_PING_ONLY");
        case Kind::ServerFull: return bytes("error\nEXE_SERVERISFULL");
        default: return std::nullopt;
    }
}

ConnectDecision direct_connect(Game game, ChallengeTable& table, const ConnectRequest& request) {
    ByteView userinfo = cstr::text(request.userinfo.first(std::min(request.userinfo.size(), kMaxLine - 1)));
    auto value = [&](ByteView key) { return value_or_empty(userinfo, key); };
    if (atol(value(bytes("protocol"))) != protocol_version(game)) return ConnectDecision::of(ConnectDecision::Kind::WrongProtocol);
    std::int32_t challenge = atol(value(bytes("challenge")));
    std::int32_t qport = atol(value(bytes("qport")));
    const NetAddress& from = request.from;
    std::size_t slots = request.max_clients > 0 ? static_cast<std::size_t>(request.max_clients) : 0;
    auto client = [&](std::size_t i) { return i < request.clients.size() ? request.clients[i] : ClientSlotInfo{}; };
    auto same_player = [&](const ClientSlotInfo& cl) {
        return same_base_address(game, from, cl.address) && (cl.qport == qport || cl.address.port == from.port);
    };
    for (std::size_t i = 0; i < slots; ++i) {
        ClientSlotInfo cl = client(i);
        if (same_player(cl)) {
            auto since = static_cast<std::int32_t>(static_cast<std::uint32_t>(request.time) -
                                                   static_cast<std::uint32_t>(cl.last_connect_time));
            if (since < static_cast<std::int32_t>(static_cast<std::uint32_t>(request.reconnect_limit) * 1000u)) {
                return ConnectDecision::of(ConnectDecision::Kind::ReconnectTooSoon);
            }
            break;
        }
    }
    std::optional<std::size_t> challenge_slot;
    if (from.kind != AddressType::Loopback && from.kind != AddressType::Bot) {
        std::optional<std::size_t> found;
        for (std::size_t i = 0; i < table.slots.size(); ++i) {
            if (same_address(from, table.slots[i].address) && table.slots[i].challenge == challenge) {
                found = i;
                break;
            }
        }
        if (!found) return ConnectDecision::of(ConnectDecision::Kind::BadChallenge);
        challenge_slot = found;
        ChallengeSlot& ch = table.slots[*found];
        std::int32_t ping = ch.first_ping;
        if (ping == 0) {
            ping = static_cast<std::int32_t>(static_cast<std::uint32_t>(request.time) - static_cast<std::uint32_t>(ch.ping_time));
            ch.first_ping = ping;
        }
        ch.connected = true;
        if (!request.from_lan) {
            double p = ping;
            if (request.min_ping != 0.0 && p < request.min_ping) return ConnectDecision::of(ConnectDecision::Kind::PingTooLow);
            if (request.max_ping != 0.0 && request.max_ping < p) return ConnectDecision::of(ConnectDecision::Kind::PingTooHigh);
        }
    }
    for (std::size_t i = 0; i < slots; ++i) {
        ClientSlotInfo cl = client(i);
        if (cl.state != 0 && same_player(cl)) {
            return ConnectDecision::of(ConnectDecision::Kind::Reconnect, i, challenge_slot, cl.state >= kClientStateConnected);
        }
    }
    ByteView password = value(bytes("password"));
    ByteView private_password = cstr::text(request.private_password);
    bool right = password.size() == private_password.size() &&
                 (password.empty() || std::memcmp(password.data(), private_password.data(), password.size()) == 0);
    std::int32_t start = right ? 0 : request.private_clients;
    for (std::int32_t i = std::max(start, 0); i < request.max_clients; ++i) {
        if (client(static_cast<std::size_t>(i)).state == 0) {
            return ConnectDecision::of(ConnectDecision::Kind::NewSlot, static_cast<std::size_t>(i), challenge_slot);
        }
    }
    return ConnectDecision::of(ConnectDecision::Kind::ServerFull);
}

bool info_response_accepted(Game game, ByteView info, ByteView debug_protocol_text, float debug_protocol_value) {
    std::int32_t protocol = atol(value_or_empty(info, bytes("protocol")));
    ByteView debug = cstr::text(debug_protocol_text);
    if (game == Game::CallOfDuty2) {
        std::int32_t expected = debug.empty() ? 118 : atol(debug);
        return protocol == expected;
    }
    double expected = debug.empty() ? 22.0 : static_cast<double>(debug_protocol_value);
    return static_cast<double>(protocol) == expected;
}

void set_server_info(Game game, ServerListEntry& entry, const ByteView* info_arg, std::int16_t ping) {
    if (info_arg) {
        ByteView info_text = *info_arg;
        auto value = [&](const char* key) {
            return value_or_empty(info_text, ByteView(reinterpret_cast<const std::uint8_t*>(key), std::strlen(key)));
        };
        auto number = [&](const char* key) { return atol(value(key)); };
        auto text = [&](ByteSpan field, const char* key) {
            std::size_t last = field.size() - 1;
            crt::strncpy(field.first(last), value(key));
            field[last] = 0;
        };
        entry.clients = static_cast<std::uint8_t>(number("clients"));
        text(entry.hostname, "hostname");
        text(entry.mapname, "mapname");
        entry.max_clients = static_cast<std::uint8_t>(number("sv_maxclients"));
        text(entry.game, "game");
        text(entry.gametype, "gametype");
        entry.net_type = static_cast<std::uint8_t>(number("nettype"));
        entry.min_ping = static_cast<std::int16_t>(number("minping"));
        entry.max_ping = static_cast<std::int16_t>(number("maxping"));
        entry.allow_anonymous = static_cast<std::uint8_t>(number("sv_allowAnonymous"));
        entry.console_disabled = static_cast<std::uint8_t>(number("con_disabled"));
        entry.password = static_cast<std::uint8_t>(number("pswrd"));
        entry.pure = static_cast<std::uint8_t>(number("pure"));
        entry.friendly_fire = static_cast<std::uint8_t>(number("ff"));
        entry.killcam = static_cast<std::uint8_t>(number("kc"));
        entry.hardware = static_cast<std::uint8_t>(number("hw"));
        entry.modded = static_cast<std::uint8_t>(number("mod"));
        if (game == Game::CallOfDuty2) {
            entry.voice = static_cast<std::uint8_t>(number("voice"));
        } else {
            entry.timeouts_allowed = static_cast<std::int16_t>(number("timeoutsallowed"));
            entry.jeeps = static_cast<std::int16_t>(number("jps"));
            entry.tanks = static_cast<std::int16_t>(number("tnk"));
        }
        entry.punkbuster = static_cast<std::uint8_t>(number("pb"));
    }
    entry.ping = ping;
}

}  // namespace iw_pc::net
