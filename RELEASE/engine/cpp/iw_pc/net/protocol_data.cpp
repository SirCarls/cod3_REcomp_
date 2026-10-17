#include "iw_pc/net/protocol_data.h"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <iterator>

#include "iw_pc/sha256.h"

namespace iw_pc::net {

namespace {

constexpr std::size_t kNetFieldSize = 12;

// CoD2MP_s.exe 1.3. The counts are the ones the code passes with each table.
constexpr TableLocation kCod2Tables[] = {
    {ProtocolTable::HuffmanFrequencies, 0x0059CB58, 256, 4,
     "1cbdca8d6c9bb22247525a4807c5e18cec9b87336c56016424eecd879b505abf"},
    {ProtocolTable::EntityStateFields, 0x0059BDA0, 59, kNetFieldSize,
     "cca9e98aab3df40614c429ae7ceecb01277351eedec0d4171b04ea2648c3f9dc"},
    {ProtocolTable::PlayerStateFields, 0x0059C4A0, 105, kNetFieldSize,
     "f5db42df82893234c22dc84b9d14533883e56e2514da5a3f4d3e31e82cd9e31a"},
    {ProtocolTable::ClientStateFields, 0x0059C398, 22, kNetFieldSize,
     "656a11e60e7c1e554febfdf80013709000df629e13c96de188f9ab663e65213c"},
    {ProtocolTable::ArchivedEntityFields, 0x0059C068, 68, kNetFieldSize,
     "7670ca4f1a66a75b3594916fb7dcbecdb3c0a7b92d6860de40da9d7349c6a3a5"},
    {ProtocolTable::HudElemFields, 0x0059C9D8, 32, kNetFieldSize,
     "1a80d6c27a5a556e4ee03916c77420ab2ea2c3c3ca88b66eef46b91298c5adfe"},
    {ProtocolTable::ObjectiveFields, 0x0059C990, 6, kNetFieldSize,
     "7ecf958791b87136a4e87c8b513c6e4294ef2e4866cb4b103d17203e3995adf1"},
};

// CoDUOMP.exe 1.51.
constexpr TableLocation kUoTables[] = {
    {ProtocolTable::HuffmanFrequencies, 0x005C58D0, 256, 4,
     "1cbdca8d6c9bb22247525a4807c5e18cec9b87336c56016424eecd879b505abf"},
    {ProtocolTable::EntityStateFields, 0x0058EC68, 60, kNetFieldSize,
     "9148f968f20a7efeab5ed17dd820d796806a546a476ac242e65a877784d9e400"},
    {ProtocolTable::EntityStateFieldsAlternate, 0x0058EF38, 60, kNetFieldSize,
     "6796d0116c9cb5dd5150bc71446c7f2ab3f8611dfceb4e226a43e4ae00fb00e8"},
    {ProtocolTable::PlayerStateFields, 0x0058F640, 114, kNetFieldSize,
     "cfb07b9ad449c2d7667ee9ae1959dea72bcadb1ef64ada90bac99b660a02f3ed"},
    {ProtocolTable::ClientStateFields, 0x0058F538, 22, kNetFieldSize,
     "d1326ca8927d0abd54892e0d8afae33ffeabe37218a5f21221a670adfc8423cb"},
    {ProtocolTable::ArchivedEntityFields, 0x0058F208, 68, kNetFieldSize,
     "83d2f8669a3ea946040905a60a3322251a2cab07a5e654e5c5637e19d7c41cd5"},
    {ProtocolTable::HudElemFields, 0x005C5768, 30, kNetFieldSize,
     "740ca7005d7d0259ed09577fcbed1fdffaaf82127623b57d497404a7c1216963"},
    {ProtocolTable::ObjectiveFields, 0x005C5720, 6, kNetFieldSize,
     "24746885ee50d80abd245432b687d55ee4fa0c995eb8d37f962859b7727c8de6"},
};

std::optional<std::uint16_t> u16_at(const std::vector<std::uint8_t>& b, std::size_t at) {
    if (at + 2 > b.size()) return std::nullopt;
    return static_cast<std::uint16_t>(b[at] | b[at + 1] << 8);
}

std::optional<std::uint32_t> u32_at(const std::vector<std::uint8_t>& b, std::size_t at) {
    if (at + 4 > b.size()) return std::nullopt;
    return static_cast<std::uint32_t>(b[at]) | static_cast<std::uint32_t>(b[at + 1]) << 8 |
           static_cast<std::uint32_t>(b[at + 2]) << 16 | static_cast<std::uint32_t>(b[at + 3]) << 24;
}

ProtocolDataError not_an_executable(const char* what) {
    return {std::string("not a 32-bit Windows executable: ") + what};
}

ProtocolDataError wrong_build(Game game, ProtocolTable table) {
    return {std::string("this ") + executable_name(game) + " is not the expected build: its " + description(table) +
            " differs from the one in " + supported_build(game) + ". Use " + executable_name(game) +
            " from an unmodified " + supported_build(game) + " install."};
}

bool same_name(const std::string& a, const std::string& b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
               auto lower = [](char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c + 32) : c; };
               return lower(x) == lower(y);
           });
}

std::optional<std::filesystem::path> search_folder(const std::filesystem::path& dir, const std::string& name, int depth) {
    std::error_code error;
    std::vector<std::filesystem::path> subfolders;
    for (const auto& entry : std::filesystem::directory_iterator(dir, error)) {
        if (entry.is_directory(error)) subfolders.push_back(entry.path());
        else if (same_name(entry.path().filename().string(), name)) return entry.path();
    }
    if (depth == 0) return std::nullopt;
    std::sort(subfolders.begin(), subfolders.end());
    for (const auto& sub : subfolders) {
        if (auto found = search_folder(sub, name, depth - 1)) return found;
    }
    return std::nullopt;
}

}  // namespace

std::optional<TableLocation> location(ProtocolTable table, Game game) {
    const auto& tables = game == Game::CallOfDuty2 ? std::span<const TableLocation>(kCod2Tables)
                                                   : std::span<const TableLocation>(kUoTables);
    for (const auto& t : tables) {
        if (t.table == table) return t;
    }
    return std::nullopt;
}

const char* description(ProtocolTable table) {
    switch (table) {
        case ProtocolTable::HuffmanFrequencies: return "Huffman frequency table";
        case ProtocolTable::EntityStateFields: return "entity state field table";
        case ProtocolTable::EntityStateFieldsAlternate: return "second entity state field table";
        case ProtocolTable::PlayerStateFields: return "player state field table";
        case ProtocolTable::ClientStateFields: return "client state field table";
        case ProtocolTable::ArchivedEntityFields: return "archived entity field table";
        case ProtocolTable::HudElemFields: return "HUD element field table";
        case ProtocolTable::ObjectiveFields: return "objective field table";
    }
    return "";
}

const char* executable_name(Game game) { return game == Game::CallOfDuty2 ? "CoD2MP_s.exe" : "CoDUOMP.exe"; }

const char* supported_build(Game game) {
    return game == Game::CallOfDuty2 ? "Call of Duty 2 1.3 (CoD2MP_s.exe)"
                                     : "Call of Duty: United Offensive 1.51 (CoDUOMP.exe)";
}

std::variant<GameExecutable, ProtocolDataError> GameExecutable::open(Game game, const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return ProtocolDataError{"can't read " + path.string()};
    std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    return from_bytes(game, std::move(bytes));
}

std::variant<GameExecutable, ProtocolDataError> GameExecutable::from_bytes(Game game, std::vector<std::uint8_t> bytes) {
    if (bytes.size() < 2 || bytes[0] != 'M' || bytes[1] != 'Z') return not_an_executable("no MZ header");
    auto pe = u32_at(bytes, 0x3C);
    if (!pe) return not_an_executable("cut short");
    std::size_t at = *pe;
    if (at + 4 > bytes.size() || bytes[at] != 'P' || bytes[at + 1] != 'E' || bytes[at + 2] != 0 || bytes[at + 3] != 0) {
        return not_an_executable("no PE header");
    }
    std::size_t coff = at + 4;
    auto count = u16_at(bytes, coff + 2);
    auto optional_size = u16_at(bytes, coff + 16);
    if (!count || !optional_size) return not_an_executable("cut short");
    std::size_t optional = coff + 20;
    if (u16_at(bytes, optional) != std::optional<std::uint16_t>(0x10B)) return not_an_executable("not a 32-bit (PE32) image");
    auto image_base = u32_at(bytes, optional + 28);
    if (!image_base) return not_an_executable("cut short");
    std::size_t table = optional + *optional_size;
    GameExecutable exe;
    for (std::size_t i = 0; i < *count; ++i) {
        std::size_t entry = table + i * 40;
        auto va = u32_at(bytes, entry + 12), raw_size = u32_at(bytes, entry + 16), raw_offset = u32_at(bytes, entry + 20);
        if (!va || !raw_size || !raw_offset) return not_an_executable("section table cut short");
        exe.sections_.push_back({*va, *raw_size, *raw_offset});
    }
    exe.game_ = game;
    exe.bytes_ = std::move(bytes);
    exe.image_base_ = *image_base;
    return exe;
}

std::variant<ByteView, ProtocolDataError> GameExecutable::read(std::uint32_t address, std::size_t len) const {
    char text[96];
    std::snprintf(text, sizeof text, "the executable holds no %zu bytes at 0x%08X", len, address);
    ProtocolDataError missing{text};
    std::uint64_t rva = static_cast<std::uint32_t>(address - image_base_);
    std::uint64_t end = rva + len;
    for (const Section& s : sections_) {
        std::uint64_t start = s.virtual_address;
        if (start <= rva && end <= start + s.raw_size) {
            std::uint64_t offset = s.raw_offset + rva - start;
            if (offset + len > bytes_.size()) return missing;
            return ByteView(bytes_).subspan(static_cast<std::size_t>(offset), len);
        }
    }
    return missing;
}

std::variant<ByteView, ProtocolDataError> GameExecutable::table(ProtocolTable which) const {
    auto where = location(which, game_);
    if (!where) return ProtocolDataError{std::string(supported_build(game_)) + " has no " + description(which)};
    auto bytes = read(where->address, where->size());
    if (!std::holds_alternative<ByteView>(bytes)) return wrong_build(game_, which);
    ByteView view = std::get<ByteView>(bytes);
    if (sha256::to_hex(sha256::digest(view)) != where->sha256) return wrong_build(game_, which);
    return view;
}

std::variant<std::array<std::int32_t, 256>, ProtocolDataError> GameExecutable::huffman_frequencies() const {
    auto bytes = table(ProtocolTable::HuffmanFrequencies);
    if (auto* error = std::get_if<ProtocolDataError>(&bytes)) return *error;
    ByteView view = std::get<ByteView>(bytes);
    std::array<std::int32_t, 256> frequencies{};
    for (std::size_t i = 0; i < 256; ++i) {
        frequencies[i] = static_cast<std::int32_t>(static_cast<std::uint32_t>(view[4 * i]) |
                                                   static_cast<std::uint32_t>(view[4 * i + 1]) << 8 |
                                                   static_cast<std::uint32_t>(view[4 * i + 2]) << 16 |
                                                   static_cast<std::uint32_t>(view[4 * i + 3]) << 24);
    }
    return frequencies;
}

std::optional<std::filesystem::path> find_executable(Game game, const std::string& folders) {
#ifdef _WIN32
    constexpr char kSeparator = ';';
#else
    constexpr char kSeparator = ':';
#endif
    std::string name = executable_name(game);
    std::size_t start = 0;
    while (start <= folders.size()) {
        std::size_t end = folders.find(kSeparator, start);
        if (end == std::string::npos) end = folders.size();
        if (end > start) {
            if (auto found = search_folder(folders.substr(start, end - start), name, 2)) return found;
        }
        start = end + 1;
    }
    return std::nullopt;
}

}  // namespace iw_pc::net
