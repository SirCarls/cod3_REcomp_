#include "iw_pc/shared/checksum.h"

#include <vector>

namespace iw_pc::shared::checksum {

namespace {

std::uint32_t rotate_left(std::uint32_t x, int n) { return (x << n) | (x >> (32 - n)); }

std::uint32_t f(std::uint32_t x, std::uint32_t y, std::uint32_t z) { return (x & y) | (~x & z); }
std::uint32_t g(std::uint32_t x, std::uint32_t y, std::uint32_t z) { return (x & y) | (x & z) | (y & z); }
std::uint32_t h(std::uint32_t x, std::uint32_t y, std::uint32_t z) { return x ^ y ^ z; }

void append_le32(std::vector<std::uint8_t>& out, std::uint32_t word) {
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<std::uint8_t>(word >> (8 * i)));
}

}  // namespace

std::array<std::uint32_t, 4> md4(ByteView data) {
    std::array<std::uint32_t, 4> state = {0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u};
    std::vector<std::uint8_t> message(data.begin(), data.end());
    std::uint64_t bits = static_cast<std::uint64_t>(data.size()) * 8;
    message.push_back(0x80);
    while (message.size() % 64 != 56) message.push_back(0);
    for (int i = 0; i < 8; ++i) message.push_back(static_cast<std::uint8_t>(bits >> (8 * i)));
    for (std::size_t offset = 0; offset < message.size(); offset += 64) {
        std::uint32_t x[16];
        for (int i = 0; i < 16; ++i) {
            const std::uint8_t* p = &message[offset + 4 * i];
            x[i] = p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
        }
        std::uint32_t a = state[0], b = state[1], c = state[2], d = state[3];
        for (int k : {0, 4, 8, 12}) {
            a = rotate_left(a + f(b, c, d) + x[k], 3);
            d = rotate_left(d + f(a, b, c) + x[k + 1], 7);
            c = rotate_left(c + f(d, a, b) + x[k + 2], 11);
            b = rotate_left(b + f(c, d, a) + x[k + 3], 19);
        }
        constexpr std::uint32_t kRound2 = 0x5A827999u;
        for (int k : {0, 1, 2, 3}) {
            a = rotate_left(a + g(b, c, d) + x[k] + kRound2, 3);
            d = rotate_left(d + g(a, b, c) + x[k + 4] + kRound2, 5);
            c = rotate_left(c + g(d, a, b) + x[k + 8] + kRound2, 9);
            b = rotate_left(b + g(c, d, a) + x[k + 12] + kRound2, 13);
        }
        constexpr std::uint32_t kRound3 = 0x6ED9EBA1u;
        for (int k : {0, 2, 1, 3}) {
            a = rotate_left(a + h(b, c, d) + x[k] + kRound3, 3);
            d = rotate_left(d + h(a, b, c) + x[k + 8] + kRound3, 9);
            c = rotate_left(c + h(d, a, b) + x[k + 4] + kRound3, 11);
            b = rotate_left(b + h(c, d, a) + x[k + 12] + kRound3, 15);
        }
        state[0] += a;
        state[1] += b;
        state[2] += c;
        state[3] += d;
    }
    return state;
}

std::int32_t block_checksum(ByteView data) {
    std::array<std::uint32_t, 4> s = md4(data);
    return static_cast<std::int32_t>(s[0] ^ s[1] ^ s[2] ^ s[3]);
}

std::int32_t block_checksum_key(ByteView data, std::int32_t key) {
    std::vector<std::uint8_t> keyed;
    keyed.reserve(4 + data.size());
    append_le32(keyed, static_cast<std::uint32_t>(key));
    keyed.insert(keyed.end(), data.begin(), data.end());
    return block_checksum(keyed);
}

ArchiveChecksums archive_checksums(std::span<const std::uint32_t> crcs, std::int32_t feed) {
    std::vector<std::uint8_t> bytes;
    bytes.reserve(4 * crcs.size());
    for (std::uint32_t crc : crcs) append_le32(bytes, crc);
    return {block_checksum(bytes), block_checksum_key(bytes, feed)};
}

}  // namespace iw_pc::shared::checksum
