// cod3_check_cpp's side of the checker's line protocol: the same requests and
// replies as engine/tools/decomp_check (the Rust ports' answerer), so the
// checker (tools/decomp/decomp.py check) and the recorded answers
// (engine/tools/decomp_check/golden) can ask the C++ version too. [acct2-decomp]
//
//   request: <function> <arg>...        reply: ok <result> <buffer>...   or   err <reason>
//   i:<decimal>  f:<f32 bits, 8 hex>  d:<f64 bits, 16 hex>  s:<hex string> | s:-  v:<hex bytes, read only>
//   b:<hex bytes the function may change> | b:-   n: (no result)
//   m:<overflowed>,<maxsize>,<cursize>,<readcount>,<hex data>   a msg_t and its data buffer
//
// Every b: and m: argument comes back in the reply, in order, as the function
// left it. Each area has its own dispatch file (math.cpp, script.cpp ...).
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace cod3::check {

// A network message (msg_t) and its data buffer, from an m: token.
struct MsgArg {
    std::int32_t overflowed = 0;
    std::int32_t maxsize = 0;
    std::int32_t cursize = 0;
    std::int32_t readcount = 0;
    std::vector<std::uint8_t> data;
};

enum class ArgKind { Int, F32, F64, Str, Bytes, Buffer, Msg };

// One decoded argument.
struct Arg {
    ArgKind kind = ArgKind::Int;
    std::int64_t integer = 0;
    float f32 = 0.0f;
    double f64 = 0.0;
    bool null = false;                // s:- or b:- (a NULL pointer)
    std::vector<std::uint8_t> bytes;  // s:, v:, b:
    MsgArg msg;
};

// A big-endian float as the guest keeps it, and back.
float load_be_float(const std::uint8_t* bytes);
void store_be_float(std::uint8_t* bytes, float value);

class Args {
public:
    explicit Args(std::vector<Arg> args) : args_(std::move(args)) {}

    std::int64_t integer(std::size_t i) const;
    std::int32_t i32(std::size_t i) const { return static_cast<std::int32_t>(integer(i)); }
    float f32(std::size_t i) const;
    double f64(std::size_t i) const;
    // Read-only bytes: a v: argument, or a b: one that isn't NULL.
    const std::vector<std::uint8_t>& bytes(std::size_t i) const;
    // A b: argument the function may change; nullptr for b:-.
    std::vector<std::uint8_t>* buffer(std::size_t i);
    // An s: argument's characters; nullptr for s:- (a NULL pointer).
    const std::vector<std::uint8_t>* str(std::size_t i) const;

    // N big-endian floats, as the guest keeps a vec2_t / vec3_t.
    template <std::size_t N>
    std::array<float, N> floats(std::size_t i) const {
        const std::vector<std::uint8_t>& b = bytes(i);
        if (b.size() < N * 4) {
            throw_short(i);
        }
        std::array<float, N> out{};
        for (std::size_t k = 0; k < N; ++k) {
            out[k] = load_be_float(b.data() + k * 4);
        }
        return out;
    }
    // Stores floats into a b: argument (nothing for a NULL one).
    void set_floats(std::size_t i, const float* values, std::size_t count);

    // A 32-bit global passed as a 4-byte buffer (big-endian).
    std::uint32_t global_u32(std::size_t i);
    void set_global_u32(std::size_t i, std::uint32_t value);

    const std::vector<Arg>& all() const { return args_; }

private:
    [[noreturn]] static void throw_short(std::size_t i);
    std::vector<Arg> args_;
};

enum class RetKind { Void, Int, F32, F64, Str };

// What a port returned.
struct Ret {
    RetKind kind = RetKind::Void;
    std::int64_t integer = 0;
    float f32 = 0.0f;
    double f64 = 0.0;
    bool null = false;
    std::vector<std::uint8_t> str;

    static Ret Void() { return Ret{}; }
    static Ret Int(std::int64_t v) {
        Ret r;
        r.kind = RetKind::Int;
        r.integer = v;
        return r;
    }
    static Ret F32(float v) {
        Ret r;
        r.kind = RetKind::F32;
        r.f32 = v;
        return r;
    }
    static Ret F64(double v) {
        Ret r;
        r.kind = RetKind::F64;
        r.f64 = v;
        return r;
    }
    // A string the function returned (its characters, without the terminator).
    static Ret Str(std::string_view text) {
        Ret r;
        r.kind = RetKind::Str;
        r.str.assign(text.begin(), text.end());
        return r;
    }
};

// An area's answer: the result, or the reason the port refused (the reply's "err <reason>").
struct Answer {
    bool ok = true;
    Ret ret;
    std::string reason;
};
inline Answer Ok(Ret ret) { return Answer{true, std::move(ret), {}}; }

// Each area answers the functions it has, and nothing for the others.
std::optional<Answer> dispatch_math(std::string_view name, Args& args);
std::optional<Answer> dispatch_script(std::string_view name, Args& args);
std::optional<Answer> dispatch_shared(std::string_view name, Args& args);
std::optional<Answer> dispatch_gfx(std::string_view name, Args& args);

// One request line to its reply line.
std::string answer(std::string_view line);

}  // namespace cod3::check
