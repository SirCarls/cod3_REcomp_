// The checker's request/reply protocol (tools/decomp/x86/harness_x86.py),
// the same one the Rust answerer (engine/tools/pc_check) speaks:
//
//   request: <image>.<function> <arg>...   reply: ok <result> <buffer>...  or  err <reason>
//   i:<decimal>  f:<f32 bits, 8 hex>  d:<f64 bits, 16 hex>  s:<hex string> | s:-  v:<hex bytes, read only>
//   b:<hex bytes the function may change> | b:-   n: (no result)   p:<hex message> | p:- (printed or not)
//   g:<hex sparse region: size u32 BE, then runs of offset u32 BE, length u32 BE, bytes>; sent back as b:
#pragma once

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

#include "iw_pc/base.h"
#include "iw_pc/fs/fs.h"

namespace pc_check {

using Bytes = std::vector<std::uint8_t>;

struct Str {
    std::optional<Bytes> value;  // nullopt: a NULL pointer
};
struct ReadOnly {
    Bytes value;
};
struct Buffer {
    std::optional<Bytes> value;  // nullopt: a NULL pointer
};
/// A block of the game's globals (g:, sparse-coded; main.cpp), changed in
/// place and sent back whole as b: (sparse-coded the same way).
struct Region {
    Bytes value;
};
using Arg = std::variant<std::int64_t, float, double, Str, ReadOnly, Buffer, Region>;

/// A request's arguments, in the C prototype's order.
class Args {
public:
    std::vector<Arg> items;

    std::int64_t int64(std::size_t i) const { return get<std::int64_t>(i, "an integer"); }
    std::int32_t i32(std::size_t i) const { return static_cast<std::int32_t>(int64(i)); }
    float f32(std::size_t i) const { return get<float>(i, "a float"); }
    double f64(std::size_t i) const { return get<double>(i, "a double"); }
    /// A string argument; nullptr for NULL.
    const Bytes* str(std::size_t i) const {
        const auto& s = get<Str>(i, "a string");
        return s.value ? &*s.value : nullptr;
    }
    /// A string argument, NULL read as empty.
    iw_pc::ByteView text(std::size_t i) const {
        const Bytes* s = str(i);
        return s ? iw_pc::ByteView(*s) : iw_pc::ByteView{};
    }
    /// Read-only bytes (a v: token, or a b: buffer read as bytes).
    iw_pc::ByteView bytes(std::size_t i) const {
        if (auto* r = std::get_if<ReadOnly>(&items.at(i))) return r->value;
        if (auto* b = std::get_if<Buffer>(&items.at(i)); b && b->value) return *b->value;
        throw std::runtime_error("argument " + std::to_string(i) + " is not bytes");
    }
    /// A block of globals (g:), changed in place.
    Bytes& region(std::size_t i) {
        auto* r = std::get_if<Region>(&items.at(i));
        if (!r) throw std::runtime_error("argument " + std::to_string(i) + " is not a region");
        return r->value;
    }
    /// A buffer the function may change; nullptr for NULL.
    Bytes* buffer(std::size_t i) {
        auto* b = std::get_if<Buffer>(&items.at(i));
        if (!b) throw std::runtime_error("argument " + std::to_string(i) + " is not a buffer");
        return b->value ? &*b->value : nullptr;
    }

private:
    template <class T>
    const T& get(std::size_t i, const char* what) const {
        const T* v = std::get_if<T>(&items.at(i));
        if (!v) throw std::runtime_error("argument " + std::to_string(i) + " is not " + what);
        return *v;
    }
};

/// What a port returned.
struct Void {};
struct Printed {
    std::optional<Bytes> message;  // nullopt: nothing printed
};
using Ret = std::variant<Void, std::int64_t, float, double, Str, Printed>;

/// A port's answer: its result, or why there is none (err <reason>).
struct Answer {
    std::optional<Ret> ret;
    std::string error;

    Answer(Ret r) : ret(std::move(r)) {}
    static Answer fail(std::string reason) {
        Answer a{Void{}};
        a.ret.reset();
        a.error = std::move(reason);
        return a;
    }
};

/// The reply for a port that stopped with the game's error.
inline Answer game_error(const iw_pc::GameError& e) {
    return Answer::fail(std::string(iw_pc::GameError::exit_name()) + " " +
                        (e.level == iw_pc::ErrorLevel::Fatal ? "0" : "1"));
}

inline Answer integer(std::int64_t v) { return Answer(Ret(v)); }
inline Answer string(iw_pc::ByteView s) { return Answer(Ret(Str{Bytes(s.begin(), s.end())})); }
inline Answer nothing() { return Answer(Ret(Void{})); }

/// Which game an image name belongs to.
inline iw_pc::Game game_of(const std::string& image) {
    return image == "cod2mp" ? iw_pc::Game::CallOfDuty2 : iw_pc::Game::UnitedOffensive;
}

// Each area's requests; nullopt when the function isn't one of its own.
std::optional<Answer> dispatch_shared(iw_pc::Game game, const std::string& function, Args& args);
/// A search path from its token (specs/fs_pure.py; shared.cpp).
std::vector<iw_pc::fs::SearchPath> search_paths(iw_pc::ByteView data);
std::optional<Answer> dispatch_msg(iw_pc::Game game, const std::string& function, Args& args);
std::optional<Answer> dispatch_oob(iw_pc::Game game, const std::string& function, Args& args);
std::optional<Answer> dispatch_delta(iw_pc::Game game, const std::string& function, Args& args);
std::optional<Answer> dispatch_client(iw_pc::Game game, const std::string& function, Args& args);
std::optional<Answer> dispatch_game_state(iw_pc::Game game, const std::string& function, Args& args);
std::optional<Answer> dispatch_game(iw_pc::Game game, const std::string& function, Args& args);

}  // namespace pc_check
