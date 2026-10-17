// Encodings shared by the network answerers (specs/net_oob.py, net_netchan.py).
#pragma once

#include <cstring>
#include <string>
#include <vector>

#include "iw_pc/net/netchan.h"
#include "protocol.h"

namespace pc_check {

inline constexpr std::size_t kAddressSize = 20;

/// A netadr_t as the guest holds it (type, ip, port big-endian, ipx).
inline std::optional<iw_pc::net::NetAddress> address_from(iw_pc::ByteView b, std::string& error) {
    if (b.size() < kAddressSize) {
        error = "short address";
        return std::nullopt;
    }
    std::uint32_t kind = static_cast<std::uint32_t>(b[0]) | static_cast<std::uint32_t>(b[1]) << 8 |
                         static_cast<std::uint32_t>(b[2]) << 16 | static_cast<std::uint32_t>(b[3]) << 24;
    if (kind > 6) {
        error = "address type " + std::to_string(kind);
        return std::nullopt;
    }
    iw_pc::net::NetAddress a;
    a.kind = static_cast<iw_pc::net::AddressType>(kind);
    for (int i = 0; i < 4; ++i) a.ip[i] = b[4 + i];
    a.port = static_cast<std::uint16_t>(b[8] << 8 | b[9]);
    for (int i = 0; i < 10; ++i) a.ipx[i] = b[10 + i];
    return a;
}

inline std::array<std::uint8_t, kAddressSize> address_bytes(const iw_pc::net::NetAddress& a) {
    std::array<std::uint8_t, kAddressSize> out{};
    auto kind = static_cast<std::uint32_t>(a.kind);
    for (int i = 0; i < 4; ++i) out[i] = static_cast<std::uint8_t>(kind >> (8 * i));
    for (int i = 0; i < 4; ++i) out[4 + i] = a.ip[i];
    out[8] = static_cast<std::uint8_t>(a.port >> 8);
    out[9] = static_cast<std::uint8_t>(a.port);
    for (int i = 0; i < 10; ++i) out[10 + i] = a.ipx[i];
    return out;
}

/// Appends a packet sent to `to` to a SentPackets buffer: a 4-byte
/// big-endian length, the 16-byte sockaddr_in, the bytes.
inline void record_sent(Bytes& out, const iw_pc::net::NetAddress& to, iw_pc::ByteView packet) {
    auto n = static_cast<std::uint32_t>(packet.size());
    for (int i = 3; i >= 0; --i) out.push_back(static_cast<std::uint8_t>(n >> (8 * i)));
    out.push_back(2);  // AF_INET, little-endian
    out.push_back(0);
    out.push_back(static_cast<std::uint8_t>(to.port >> 8));
    out.push_back(static_cast<std::uint8_t>(to.port));
    out.insert(out.end(), to.ip.begin(), to.ip.end());
    out.insert(out.end(), 8, 0);
    out.insert(out.end(), packet.begin(), packet.end());
}

/// The spec's description of a server or client: (name, value) pairs, each
/// a 1-byte name length, the name, a 4-byte big-endian value length, the value.
class Description {
public:
    static std::optional<Description> parse(iw_pc::ByteView bytes) {
        Description d;
        std::size_t at = 0;
        while (at < bytes.size()) {
            std::size_t n = bytes[at];
            if (at + 1 + n + 4 > bytes.size()) return std::nullopt;
            iw_pc::ByteView name = bytes.subspan(at + 1, n);
            std::size_t l = at + 1 + n;
            std::size_t len = static_cast<std::size_t>(bytes[l]) << 24 | static_cast<std::size_t>(bytes[l + 1]) << 16 |
                              static_cast<std::size_t>(bytes[l + 2]) << 8 | bytes[l + 3];
            if (l + 4 + len > bytes.size()) return std::nullopt;
            d.items_.push_back({name, bytes.subspan(l + 4, len)});
            at = l + 4 + len;
        }
        return d;
    }

    iw_pc::ByteView get(const char* name) const {
        std::size_t n = std::strlen(name);
        for (const auto& [k, v] : items_) {
            if (k.size() == n && std::memcmp(k.data(), name, n) == 0) return v;
        }
        return {};
    }

    /// The value as a decimal number (0 when it isn't one).
    std::int32_t integer(const char* name) const { return parse_int(get(name)); }

    static std::int32_t parse_int(iw_pc::ByteView text) {
        std::string s(text.begin(), text.end());
        if (s.empty()) return 0;
        try {
            std::size_t used = 0;
            long long v = std::stoll(s, &used, 10);
            if (used != s.size() || v < INT32_MIN || v > INT32_MAX) return 0;
            return static_cast<std::int32_t>(v);
        } catch (...) {
            return 0;
        }
    }

private:
    std::vector<std::pair<iw_pc::ByteView, iw_pc::ByteView>> items_;
};

}  // namespace pc_check
