// The C++ version of the PC games' decomp (Call of Duty 2 1.3 and United
// Offensive 1.51), for developers. The Rust crate `iw_pc` is the full
// version; this follows it function by function, and each function is
// checked against the game's own machine code the same way
// (`tools/decomp/x86/pcdecomp.py check --impl cpp`). See engine/cpp/README.md.
//
// Conventions (the same as the Rust crate's):
// * Every ported function has an "Original:" line per executable.
// * Strings are bytes, not text: a string argument is a ByteView holding just
//   its characters (reading past its end gives 0, see cstr.h); a string in a
//   fixed buffer is a ByteSpan the size of the game's array.
// * The game's Com_Error is a returned GameError at the same point, with the
//   same level; a warning the original prints instead is a returned refusal.
// * Characters are signed where the original uses movsx.
// * No game data: tables the protocol needs are read from the player's own
//   executable (net/protocol_data.h).
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <variant>

namespace iw_pc {

/// Read-only bytes (a string argument, a structure's bytes).
using ByteView = std::span<const std::uint8_t>;
/// Bytes a function may change (a string buffer, a message's data).
using ByteSpan = std::span<std::uint8_t>;

/// Which of the two games, where they differ.
enum class Game {
    CallOfDuty2,      // CoD2MP_s.exe 1.3
    UnitedOffensive,  // CoDUOMP.exe 1.51
};

enum class ErrorLevel { Fatal, Drop };

/// The game's Com_Error: the level and the message.
struct GameError {
    ErrorLevel level;
    std::string message;

    static GameError fatal(std::string message) { return {ErrorLevel::Fatal, std::move(message)}; }
    static GameError drop(std::string message) { return {ErrorLevel::Drop, std::move(message)}; }
    /// The name the checker reports for it.
    static constexpr const char* exit_name() { return "Com_Error"; }
};

/// Nothing, or the game's error.
using Status = std::optional<GameError>;

/// A value, or the game's error.
template <class T>
class Expected {
public:
    Expected(T value) : data_(std::move(value)) {}
    Expected(GameError error) : data_(std::move(error)) {}
    bool has_value() const { return data_.index() == 0; }
    explicit operator bool() const { return has_value(); }
    const T& value() const { return std::get<0>(data_); }
    T& value() { return std::get<0>(data_); }
    const GameError& error() const { return std::get<1>(data_); }

private:
    std::variant<T, GameError> data_;
};

/// Bytes of a string literal, without its terminator: bytes("abc").
template <std::size_t N>
constexpr ByteView bytes(const char (&text)[N]) {
    return ByteView(reinterpret_cast<const std::uint8_t*>(text), N - 1);
}

}  // namespace iw_pc
