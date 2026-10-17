// The tables the network protocol needs that are game data, read at run time
// from the player's own executable (never stored in the code): the Huffman
// frequencies (256 numbers) and the delta field tables (netField_t: name
// pointer, offset, bits; 12 bytes, little-endian). The executable is only
// read as a file; each table is checked against the SHA-256 of the same
// table in the supported build (Call of Duty 2 1.3, United Offensive 1.51).
#pragma once

#include <array>
#include <filesystem>
#include <string>
#include <vector>

#include "iw_pc/base.h"

namespace iw_pc::net {

enum class ProtocolTable {
    HuffmanFrequencies,          // msg_hData
    EntityStateFields,           // entityStateFields
    EntityStateFieldsAlternate,  // United Offensive's table for entities of type 12
    PlayerStateFields,
    ClientStateFields,
    ArchivedEntityFields,
    HudElemFields,
    ObjectiveFields,
};

/// Where a table is in one game's executable, and its SHA-256 in the supported build.
struct TableLocation {
    ProtocolTable table;
    std::uint32_t address;  // virtual address (the executables load at 0x00400000)
    std::size_t entries;
    std::size_t entry_size;  // 4 for the frequencies, 12 for a netField_t
    const char* sha256;
    std::size_t size() const { return entries * entry_size; }
};

/// Where `table` is in `game`'s executable, or nullopt when the game has none.
std::optional<TableLocation> location(ProtocolTable table, Game game);
const char* description(ProtocolTable table);
const char* executable_name(Game game);
const char* supported_build(Game game);

/// Why protocol data couldn't be read (a message for people).
struct ProtocolDataError {
    std::string message;
};

/// A game executable opened for reading its protocol tables.
class GameExecutable {
public:
    /// Reads `game`'s executable from `path` (the whole file, once).
    static std::variant<GameExecutable, ProtocolDataError> open(Game game, const std::filesystem::path& path);
    /// The executable from its bytes: reads the PE headers.
    static std::variant<GameExecutable, ProtocolDataError> from_bytes(Game game, std::vector<std::uint8_t> bytes);

    Game game() const { return game_; }
    /// `len` bytes at the virtual address `address`, as stored in the file.
    std::variant<ByteView, ProtocolDataError> read(std::uint32_t address, std::size_t len) const;
    /// A table's bytes, checked against the supported build's SHA-256.
    std::variant<ByteView, ProtocolDataError> table(ProtocolTable table) const;
    /// The Huffman frequency table, checked.
    std::variant<std::array<std::int32_t, 256>, ProtocolDataError> huffman_frequencies() const;

private:
    struct Section {
        std::uint32_t virtual_address;  // relative to the image base
        std::uint32_t raw_size;
        std::uint32_t raw_offset;
    };
    Game game_ = Game::CallOfDuty2;
    std::vector<std::uint8_t> bytes_;
    std::uint32_t image_base_ = 0;
    std::vector<Section> sections_;
};

/// Finds a game's executable in the folders a variable like IW_PC_INPUT
/// names (several separated like PATH), by name in any case, up to two
/// folders down. For tools and tests.
std::optional<std::filesystem::path> find_executable(Game game, const std::string& folders);

}  // namespace iw_pc::net
