// [acct2-decomp] The checker's line protocol (protocol.h), as
// engine/tools/decomp_check/src/main.rs speaks it.
#include "cod3_check/protocol.h"

#include "cod3/q_shared.h"

#include <bit>
#include <charconv>
#include <cstdio>
#include <stdexcept>

namespace cod3::check {

float load_be_float(const std::uint8_t* b) {
    const std::uint32_t bits = (std::uint32_t{b[0]} << 24) | (std::uint32_t{b[1]} << 16) |
                               (std::uint32_t{b[2]} << 8) | std::uint32_t{b[3]};
    return std::bit_cast<float>(bits);
}

void store_be_float(std::uint8_t* b, float value) {
    const std::uint32_t bits = std::bit_cast<std::uint32_t>(value);
    b[0] = static_cast<std::uint8_t>(bits >> 24);
    b[1] = static_cast<std::uint8_t>(bits >> 16);
    b[2] = static_cast<std::uint8_t>(bits >> 8);
    b[3] = static_cast<std::uint8_t>(bits);
}

namespace {

[[noreturn]] void wrong_kind(std::size_t i, const char* expected) {
    throw std::invalid_argument("argument " + std::to_string(i) + " is not " + expected);
}

int hex_digit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

std::vector<std::uint8_t> parse_hex(std::string_view text) {
    if (text.size() % 2 != 0) {
        throw std::invalid_argument("odd hex length in \"" + std::string(text) + "\"");
    }
    std::vector<std::uint8_t> out(text.size() / 2);
    for (std::size_t i = 0; i < out.size(); ++i) {
        const int hi = hex_digit(text[2 * i]);
        const int lo = hex_digit(text[2 * i + 1]);
        if (hi < 0 || lo < 0) {
            throw std::invalid_argument("bad hex in \"" + std::string(text) + "\"");
        }
        out[i] = static_cast<std::uint8_t>(hi * 16 + lo);
    }
    return out;
}

std::string hex(const std::vector<std::uint8_t>& bytes) {
    static const char digits[] = "0123456789abcdef";
    std::string out;
    out.reserve(bytes.size() * 2);
    for (const std::uint8_t b : bytes) {
        out += digits[b >> 4];
        out += digits[b & 15];
    }
    return out;
}

template <typename T>
T parse_number(std::string_view text, int base) {
    if (!text.empty() && text.front() == '+') {
        text.remove_prefix(1);
    }
    T value{};
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value, base);
    if (error != std::errc() || end != text.data() + text.size() || text.empty()) {
        throw std::invalid_argument("bad number \"" + std::string(text) + "\"");
    }
    return value;
}

std::int32_t parse_i32(std::string_view text) { return parse_number<std::int32_t>(text, 10); }

MsgArg parse_msg(std::string_view text) {
    MsgArg m;
    std::string_view rest = text;
    std::int32_t* fields[] = {&m.overflowed, &m.maxsize, &m.cursize, &m.readcount};
    for (std::int32_t* field : fields) {
        const std::size_t comma = rest.find(',');
        if (comma == std::string_view::npos) {
            throw std::invalid_argument("bad message \"" + std::string(text) + "\"");
        }
        *field = parse_i32(rest.substr(0, comma));
        rest.remove_prefix(comma + 1);
    }
    m.data = parse_hex(rest);
    return m;
}

Arg parse_arg(std::string_view token) {
    const std::size_t colon = token.find(':');
    if (colon == std::string_view::npos) {
        throw std::invalid_argument("bad token \"" + std::string(token) + "\"");
    }
    const std::string_view kind = token.substr(0, colon);
    const std::string_view value = token.substr(colon + 1);
    Arg a;
    if (kind == "i") {
        a.kind = ArgKind::Int;
        a.integer = parse_number<std::int64_t>(value, 10);
    } else if (kind == "f") {
        a.kind = ArgKind::F32;
        a.f32 = std::bit_cast<float>(parse_number<std::uint32_t>(value, 16));
    } else if (kind == "d") {
        a.kind = ArgKind::F64;
        a.f64 = std::bit_cast<double>(parse_number<std::uint64_t>(value, 16));
    } else if (kind == "s") {
        a.kind = ArgKind::Str;
        if (value == "-") {
            a.null = true;
        } else {
            a.bytes = parse_hex(value);
        }
    } else if (kind == "v") {
        a.kind = ArgKind::Bytes;
        a.bytes = parse_hex(value);
    } else if (kind == "b") {
        a.kind = ArgKind::Buffer;
        if (value == "-") {
            a.null = true;
        } else {
            a.bytes = parse_hex(value);
        }
    } else if (kind == "m") {
        a.kind = ArgKind::Msg;
        a.msg = parse_msg(value);
    } else {
        throw std::invalid_argument("unknown token kind \"" + std::string(kind) + "\"");
    }
    return a;
}

std::string msg_token(const MsgArg& m) {
    return "m:" + std::to_string(m.overflowed) + "," + std::to_string(m.maxsize) + "," +
           std::to_string(m.cursize) + "," + std::to_string(m.readcount) + "," + hex(m.data);
}

std::string bits_token(char kind, std::uint64_t bits, int digits) {
    char text[24];
    std::snprintf(text, sizeof text, "%c:%0*llx", kind, digits, static_cast<unsigned long long>(bits));
    return text;
}

std::optional<Answer> dispatch(std::string_view name, Args& args) {
    if (auto a = dispatch_math(name, args)) return a;
    if (auto a = dispatch_script(name, args)) return a;
    if (auto a = dispatch_shared(name, args)) return a;
    if (auto a = dispatch_gfx(name, args)) return a;
    return std::nullopt;
}

}  // namespace

std::int64_t Args::integer(std::size_t i) const {
    if (i >= args_.size() || args_[i].kind != ArgKind::Int) wrong_kind(i, "an integer");
    return args_[i].integer;
}

float Args::f32(std::size_t i) const {
    if (i >= args_.size() || args_[i].kind != ArgKind::F32) wrong_kind(i, "a float");
    return args_[i].f32;
}

double Args::f64(std::size_t i) const {
    if (i >= args_.size() || args_[i].kind != ArgKind::F64) wrong_kind(i, "a double");
    return args_[i].f64;
}

const std::vector<std::uint8_t>& Args::bytes(std::size_t i) const {
    if (i < args_.size() && (args_[i].kind == ArgKind::Bytes || (args_[i].kind == ArgKind::Buffer && !args_[i].null))) {
        return args_[i].bytes;
    }
    wrong_kind(i, "bytes");
}

std::vector<std::uint8_t>* Args::buffer(std::size_t i) {
    if (i >= args_.size() || args_[i].kind != ArgKind::Buffer) wrong_kind(i, "a buffer");
    return args_[i].null ? nullptr : &args_[i].bytes;
}

const std::vector<std::uint8_t>* Args::str(std::size_t i) const {
    if (i >= args_.size() || args_[i].kind != ArgKind::Str) wrong_kind(i, "a string");
    return args_[i].null ? nullptr : &args_[i].bytes;
}

void Args::set_floats(std::size_t i, const float* values, std::size_t count) {
    std::vector<std::uint8_t>* b = buffer(i);
    if (b == nullptr) {
        return;
    }
    if (b->size() < count * 4) {
        throw_short(i);
    }
    for (std::size_t k = 0; k < count; ++k) {
        store_be_float(b->data() + k * 4, values[k]);
    }
}

std::uint32_t Args::global_u32(std::size_t i) {
    const std::vector<std::uint8_t>* b = buffer(i);
    if (b == nullptr || b->size() < 4) {
        throw std::invalid_argument("a global is never NULL");
    }
    const std::vector<std::uint8_t>& g = *b;
    return (std::uint32_t{g[0]} << 24) | (std::uint32_t{g[1]} << 16) | (std::uint32_t{g[2]} << 8) | std::uint32_t{g[3]};
}

void Args::set_global_u32(std::size_t i, std::uint32_t value) {
    std::vector<std::uint8_t>* b = buffer(i);
    if (b == nullptr || b->size() < 4) {
        return;
    }
    (*b)[0] = static_cast<std::uint8_t>(value >> 24);
    (*b)[1] = static_cast<std::uint8_t>(value >> 16);
    (*b)[2] = static_cast<std::uint8_t>(value >> 8);
    (*b)[3] = static_cast<std::uint8_t>(value);
}

void Args::throw_short(std::size_t i) {
    throw std::invalid_argument("argument " + std::to_string(i) + " is too short");
}

std::string answer(std::string_view line) {
    std::vector<std::string_view> parts;
    std::size_t start = 0;
    while (true) {
        const std::size_t space = line.find(' ', start);
        parts.push_back(line.substr(start, space == std::string_view::npos ? std::string_view::npos : space - start));
        if (space == std::string_view::npos) break;
        start = space + 1;
    }
    const std::string_view name = parts.front();
    std::vector<Arg> decoded;
    try {
        for (std::size_t k = 1; k < parts.size(); ++k) {
            if (!parts[k].empty()) {
                decoded.push_back(parse_arg(parts[k]));
            }
        }
    } catch (const std::exception& e) {
        return std::string("err bad-request ") + e.what();
    }
    Args args(std::move(decoded));
    std::optional<Answer> result;
    try {
        result = dispatch(name, args);
    } catch (const ComError& e) {
        // The game's Com_Error, as the Rust answerer reports it: the exit and its level.
        return std::string("err Com_Error ") + (e.code == ERR_DROP ? "Drop" : "Fatal");
    } catch (const std::exception& e) {
        return std::string("err bad-request ") + e.what();
    }
    if (!result) {
        return "err unknown-function " + std::string(name);
    }
    if (!result->ok) {
        return "err " + result->reason;
    }
    std::string reply = "ok ";
    const Ret& ret = result->ret;
    switch (ret.kind) {
        case RetKind::Void: reply += "n:"; break;
        case RetKind::Int: reply += "i:" + std::to_string(ret.integer); break;
        case RetKind::F32: reply += bits_token('f', std::bit_cast<std::uint32_t>(ret.f32), 8); break;
        case RetKind::F64: reply += bits_token('d', std::bit_cast<std::uint64_t>(ret.f64), 16); break;
        case RetKind::Str: reply += ret.null ? std::string("s:-") : "s:" + hex(ret.str); break;
    }
    for (const Arg& a : args.all()) {
        if (a.kind == ArgKind::Buffer) {
            reply += a.null ? std::string(" b:-") : " b:" + hex(a.bytes);
        } else if (a.kind == ArgKind::Msg) {
            reply += " " + msg_token(a.msg);
        }
    }
    return reply;
}

}  // namespace cod3::check
