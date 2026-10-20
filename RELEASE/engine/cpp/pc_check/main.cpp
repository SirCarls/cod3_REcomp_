// Answers `tools/decomp/x86/pcdecomp.py check --impl cpp`: runs the C++
// ports (engine/cpp/iw_pc) on the checker's test cases, one request per
// line on stdin, one reply per line on stdout (protocol.h).

#include <cstdio>
#include <cstring>
#include <iostream>
#include <string>

#include "protocol.h"

namespace pc_check {
namespace {

int hex_digit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    throw std::runtime_error(std::string("bad hex digit ") + c);
}

Bytes parse_hex(const std::string& text) {
    if (text.size() % 2 != 0) throw std::runtime_error("odd hex length");
    Bytes out(text.size() / 2);
    for (std::size_t i = 0; i < out.size(); ++i) {
        out[i] = static_cast<std::uint8_t>(hex_digit(text[2 * i]) * 16 + hex_digit(text[2 * i + 1]));
    }
    return out;
}

std::string hex(iw_pc::ByteView bytes) {
    static const char digits[] = "0123456789abcdef";
    std::string out;
    out.reserve(bytes.size() * 2);
    for (std::uint8_t b : bytes) {
        out += digits[b >> 4];
        out += digits[b & 15];
    }
    return out;
}

template <class T>
T bits_of(const std::string& text) {
    std::uint64_t v = std::stoull(text, nullptr, 16);
    T out;
    if constexpr (sizeof(T) == 4) {
        std::uint32_t w = static_cast<std::uint32_t>(v);
        std::memcpy(&out, &w, 4);
    } else {
        std::memcpy(&out, &v, 8);
    }
    return out;
}

// Zero bytes that split a run of a sparse region (harness_x86.py SPARSE_GAP).
constexpr std::size_t kSparseGap = 16;

std::size_t be_u32(const Bytes& b, std::size_t at) {
    if (at + 4 > b.size()) throw std::runtime_error("short sparse region");
    return static_cast<std::size_t>(static_cast<std::uint32_t>(b[at]) << 24 | static_cast<std::uint32_t>(b[at + 1]) << 16 |
                                    static_cast<std::uint32_t>(b[at + 2]) << 8 | b[at + 3]);
}

void put_be_u32(Bytes& out, std::size_t v) {
    for (int k = 3; k >= 0; --k) out.push_back(static_cast<std::uint8_t>(v >> (8 * k)));
}

Bytes sparse_decode(const Bytes& coded) {
    Bytes out(be_u32(coded, 0), 0);
    std::size_t at = 4;
    while (at < coded.size()) {
        std::size_t offset = be_u32(coded, at), len = be_u32(coded, at + 4);
        if (at + 8 + len > coded.size()) throw std::runtime_error("short sparse run");
        if (offset + len > out.size()) throw std::runtime_error("sparse run past the region");
        std::copy(coded.begin() + static_cast<std::ptrdiff_t>(at + 8), coded.begin() + static_cast<std::ptrdiff_t>(at + 8 + len),
                  out.begin() + static_cast<std::ptrdiff_t>(offset));
        at += 8 + len;
    }
    return out;
}

Bytes sparse_encode(const Bytes& data) {
    Bytes out;
    put_be_u32(out, data.size());
    bool open = false;
    std::size_t lo = 0, hi = 0;
    auto flush = [&]() {
        put_be_u32(out, lo);
        put_be_u32(out, hi - lo);
        out.insert(out.end(), data.begin() + static_cast<std::ptrdiff_t>(lo), data.begin() + static_cast<std::ptrdiff_t>(hi));
    };
    for (std::size_t i = 0; i < data.size(); ++i) {
        if (data[i] == 0) continue;
        if (open && i - hi < kSparseGap) {
            hi = i + 1;
            continue;
        }
        if (open) flush();
        open = true;
        lo = i;
        hi = i + 1;
    }
    if (open) flush();
    return out;
}

Arg parse_arg(const std::string& token) {
    auto colon = token.find(':');
    if (colon == std::string::npos) throw std::runtime_error("bad token " + token);
    std::string kind = token.substr(0, colon), value = token.substr(colon + 1);
    if (kind == "i") return static_cast<std::int64_t>(std::stoll(value));
    if (kind == "f") return bits_of<float>(value);
    if (kind == "d") return bits_of<double>(value);
    if (kind == "s") return value == "-" ? Str{} : Str{parse_hex(value)};
    if (kind == "v") return ReadOnly{parse_hex(value)};
    if (kind == "b") return value == "-" ? Buffer{} : Buffer{parse_hex(value)};
    if (kind == "g") return Region{sparse_decode(parse_hex(value))};
    throw std::runtime_error("unknown token kind " + kind);
}

std::optional<Answer> dispatch(const std::string& name, Args& args) {
    auto dot = name.find('.');
    if (dot == std::string::npos) return std::nullopt;
    std::string image = name.substr(0, dot), function = name.substr(dot + 1);
    if (image != "cod2mp" && image != "uomp") return std::nullopt;
    iw_pc::Game game = game_of(image);
    for (auto area : {dispatch_msg, dispatch_oob, dispatch_delta, dispatch_client, dispatch_game_state, dispatch_game, dispatch_shared}) {
        if (auto answer = area(game, function, args)) return answer;
    }
    return std::nullopt;
}

std::string reply_for(const Ret& ret) {
    char text[40];
    if (std::holds_alternative<Void>(ret)) return "n:";
    if (auto* v = std::get_if<std::int64_t>(&ret)) return "i:" + std::to_string(*v);
    if (auto* v = std::get_if<float>(&ret)) {
        std::uint32_t w;
        std::memcpy(&w, v, 4);
        std::snprintf(text, sizeof text, "f:%08x", w);
        return text;
    }
    if (auto* v = std::get_if<double>(&ret)) {
        std::uint64_t w;
        std::memcpy(&w, v, 8);
        std::snprintf(text, sizeof text, "d:%016llx", static_cast<unsigned long long>(w));
        return text;
    }
    if (auto* v = std::get_if<Str>(&ret)) return v->value ? "s:" + hex(*v->value) : "s:-";
    const auto& p = std::get<Printed>(ret);
    return p.message ? "p:" + hex(*p.message) : "p:-";
}

std::string answer(const std::string& line) {
    std::vector<std::string> parts;
    std::size_t start = 0;
    while (start <= line.size()) {
        std::size_t end = line.find(' ', start);
        if (end == std::string::npos) end = line.size();
        if (end > start) parts.push_back(line.substr(start, end - start));
        start = end + 1;
    }
    if (parts.empty()) return "err bad-request empty";
    Args args;
    try {
        for (std::size_t i = 1; i < parts.size(); ++i) args.items.push_back(parse_arg(parts[i]));
    } catch (const std::exception& e) {
        return std::string("err bad-request ") + e.what();
    }
    std::optional<Answer> result;
    try {
        result = dispatch(parts[0], args);
    } catch (const std::exception& e) {
        return std::string("err ") + e.what();
    }
    if (!result) return "err unknown-function " + parts[0];
    if (!result->ret) return "err " + result->error;
    std::string reply = "ok " + reply_for(*result->ret);
    for (const Arg& arg : args.items) {
        if (auto* b = std::get_if<Buffer>(&arg)) reply += b->value ? " b:" + hex(*b->value) : " b:-";
        if (auto* r = std::get_if<Region>(&arg)) reply += " b:" + hex(sparse_encode(r->value));
    }
    return reply;
}

}  // namespace
}  // namespace pc_check

int main() {
    std::ios::sync_with_stdio(false);
    std::string line;
    while (std::getline(std::cin, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n' || line.back() == ' ')) line.pop_back();
        std::cout << pc_check::answer(line) << '\n' << std::flush;
    }
    return 0;
}
