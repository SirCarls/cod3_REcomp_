// Delta coding: how the games send a structure (an entity, a client, the
// player's own state) as the fields that changed since the last one the
// other side has. A structure is sent field by field from a field table (the
// games' netField_t arrays: offset and bit count), read from the player's
// executable (protocol_data.h). Structures are handled as their bytes, every
// field as the 32-bit word at its offset.
//
// A field on the wire: unchanged, a 0 bit. Changed, a 1 bit, then by its bits:
//   0 (a float)   0.0: a 0 bit; a whole number in [-4096, 4096): 0 bit, low 5
//                 bits (biased by 4096), the rest as a raw byte; else 1 bit and 32 raw bits
//   -99           (Call of Duty 2: HUD positions) as a float, biased by 512 with 2 bits
//   -100 (angle)  0: a 0 bit; else a 1 bit and the angle in 16 bits
//   other         0: a 0 bit; else a 1 bit, the low |bits| % 8 bits, then raw bytes
#pragma once

#include <string>
#include <vector>

#include "iw_pc/base.h"
#include "iw_pc/net/protocol_data.h"

namespace iw_pc::net {

class Msg;

inline constexpr std::int32_t kAngleField = -100;
inline constexpr std::int32_t kHudPositionField = -99;
inline constexpr std::uint32_t kEntityNumberBits = 10;
inline constexpr std::uint32_t kClientNumberBits = 6;
/// The entity type (eType, offset 4) United Offensive sends with its second table.
inline constexpr std::int32_t kAlternateEntityType = 12;

/// One field: where it is in the structure and how it is sent.
struct NetField {
    std::uint32_t offset = 0;
    std::int32_t bits = 0;  // 0 a float, kAngleField, kHudPositionField, or an integer's bit count
};

/// A field table, as read from the executable.
class FieldTable {
public:
    /// netField_t entries (12 bytes: name pointer, offset, bits).
    static FieldTable from_bytes(ByteView bytes);
    /// The same, with the field names (the pointers point into `exe`).
    static FieldTable from_executable(const GameExecutable& exe, ByteView bytes);

    const std::vector<NetField>& fields() const { return fields_; }
    /// Field i's name as the game spells it, or empty.
    const std::string& name(std::size_t i) const { return names_[i]; }
    /// The field called `name`, if the table has one.
    std::optional<NetField> field(const std::string& name) const;
    /// The smallest structure every field fits in.
    std::size_t struct_size() const;
    /// The offset word of the 12 bytes in front of the table in the
    /// executable, which the structure reader takes as field -1 when a
    /// message ends where its field count should be.
    std::optional<std::uint32_t> preceding_offset() const { return preceding_offset_; }
    void set_preceding_offset(std::optional<std::uint32_t> offset) { preceding_offset_ = offset; }

private:
    std::vector<NetField> fields_;
    std::vector<std::string> names_;
    std::optional<std::uint32_t> preceding_offset_;
};

/// A game's field tables, read and checked once from its executable.
struct FieldTables {
    Game game;
    FieldTable entity;
    std::optional<FieldTable> entity_alternate;  // United Offensive's table for entities of type 12
    FieldTable player, client, archived_entity, hud_elem, objective;

    static std::variant<FieldTables, ProtocolDataError> from_executable(const GameExecutable& exe);
};

/// The 32-bit word at `offset` (0 past the end).
std::int32_t word_at(ByteView bytes, std::uint32_t offset);
/// Stores the word at `offset` (skipped past the end).
void set_word(ByteSpan bytes, std::uint32_t offset, std::int32_t value);

/// Writes one field of `to` as a change from `from`.
/// Original: MSG_WriteDeltaField at 0x00445180 (CoD2MP_s.exe 1.3)
/// Original: MSG_WriteDeltaField at 0x0044AA60 (CoDUOMP.exe 1.51)
void write_delta_field(Msg& msg, ByteView from, ByteView to, NetField field);

/// Reads one field into `to` as write_delta_field() wrote it (unchanged:
/// copied from `from`). The originals' debug printing is left out.
/// Original: MSG_ReadDeltaField at 0x00445700 (CoD2MP_s.exe 1.3)
/// Original: MSG_ReadDeltaField at 0x0044B030 (CoDUOMP.exe 1.51)
void read_delta_field(Msg& msg, ByteView from, ByteSpan to, NetField field);

/// United Offensive's extra bit after an entity's number.
using TableFlag = void (*)(Msg&, const ByteView* state);

/// Original: MSG_WriteEntityTableFlag at 0x0044AF00 (CoDUOMP.exe 1.51)
void write_entity_table_flag(Msg& msg, const ByteView* state);

/// How a structure is delta-coded.
struct StructCoding {
    std::span<const NetField> fields;
    std::uint32_t index_bits;
    bool change_bit;       // every structure starts with a 1 bit (clients)
    TableFlag table_flag;  // or nullptr
};

/// Writes `to` (nullptr: removed) as a change from `from`.
/// Original: MSG_WriteDeltaStruct at 0x00445480 (CoD2MP_s.exe 1.3)
/// Original: MSG_WriteDeltaStruct at 0x0044ACD0 (CoDUOMP.exe 1.51)
void write_delta_struct(Msg& msg, ByteView from, const ByteView* to, bool force, const StructCoding& coding);

/// Original: MSG_WriteDeltaEntity at 0x00445680 (CoD2MP_s.exe 1.3)
/// Original: MSG_WriteDeltaEntity at 0x0044AF50 (CoDUOMP.exe 1.51)
void write_delta_entity(Msg& msg, const FieldTables& tables, ByteView from, const ByteView* to, bool force);

/// `from` nullptr: from all zeros.
/// Original: MSG_WriteDeltaClient at 0x004456C0 (CoD2MP_s.exe 1.3)
/// Original: MSG_WriteDeltaClient at 0x0044AFE0 (CoDUOMP.exe 1.51)
void write_delta_client(Msg& msg, const FieldTables& tables, const ByteView* from, const ByteView* to, bool force);

/// Reads structure `number` as a change from `from` into `to`; returns
/// whether it was removed (`to` then left alone). See the Rust version's
/// documentation for the originals' handling of broken counts.
/// Original: MSG_ReadDeltaStruct at 0x00445A40 (CoD2MP_s.exe 1.3)
/// Original: MSG_ReadDeltaStruct at 0x0044B280 (CoDUOMP.exe 1.51)
bool read_delta_struct(Msg& msg, ByteView from, ByteSpan to, std::int32_t number, const FieldTable& table);

/// Original: MSG_ReadDeltaEntity at 0x00445BE0 (CoD2MP_s.exe 1.3)
/// Original: MSG_ReadDeltaEntity at 0x0044B420 (CoDUOMP.exe 1.51)
bool read_delta_entity(Msg& msg, const FieldTables& tables, ByteView from, ByteSpan to, std::int32_t number);

/// Original: MSG_ReadDeltaArchivedEntity at 0x00445C00 (CoD2MP_s.exe 1.3)
/// Original: MSG_ReadDeltaArchivedEntity at 0x0044B4A0 (CoDUOMP.exe 1.51)
bool read_delta_archived_entity(Msg& msg, const FieldTables& tables, ByteView from, ByteSpan to, std::int32_t number);

/// `from` nullptr: from all zeros.
/// Original: MSG_ReadDeltaClient at 0x00445C20 (CoD2MP_s.exe 1.3)
/// Original: MSG_ReadDeltaClient at 0x0044B4C0 (CoDUOMP.exe 1.51)
bool read_delta_client(Msg& msg, const FieldTables& tables, const ByteView* from, ByteSpan to, std::int32_t number);

/// The size of `game`'s player state in bytes.
std::size_t player_state_size(Game game);

/// HUD elements in each of the player state's two arrays (31, 63) and the bytes of one (0x80, 0x7C).
std::size_t hud_elem_count(Game game);
std::size_t hud_elem_size(Game game);

/// Writes one array of HUD elements (`count` of them; the games pass hud_elem_count()) as a change
/// from `from` (see the Rust version).
/// Original: MSG_WriteDeltaHudElems at 0x00445C60 (CoD2MP_s.exe 1.3; count in ecx)
/// Original: MSG_WriteDeltaHudElems at 0x0044B510 (CoDUOMP.exe 1.51; count in ecx, from in ebx)
void write_delta_hud_elems(Msg& msg, const FieldTables& tables, ByteView from, ByteView to, std::size_t count);

/// Reads one array of HUD elements (`count` of them, at `at` in `from` and `to`) as
/// write_delta_hud_elems() wrote it (see the Rust version for broken messages).
/// Original: MSG_ReadDeltaHudElems at 0x00445DB0 (CoD2MP_s.exe 1.3)
/// Original: MSG_ReadDeltaHudElems at 0x0044B690 (CoDUOMP.exe 1.51)
void read_delta_hud_elems(Msg& msg, const FieldTables& tables, ByteView from, ByteSpan to, std::size_t at, std::size_t count);

/// Writes the player's own state as a change from `from` (nullptr: zeros).
/// Original: MSG_WriteDeltaPlayerstate at 0x00445EC0 (CoD2MP_s.exe 1.3)
/// Original: MSG_WriteDeltaPlayerstate at 0x0044B7B0 (CoDUOMP.exe 1.51)
void write_delta_playerstate(Msg& msg, const FieldTables& tables, const ByteView* from, ByteView to);

/// Reads the player's own state into `to` (starts as a copy of `from`).
/// Original: MSG_ReadDeltaPlayerstate at 0x00446CA0 (CoD2MP_s.exe 1.3)
/// Original: MSG_ReadDeltaPlayerstate at 0x0044C5C0 (CoDUOMP.exe 1.51)
void read_delta_playerstate(Msg& msg, const FieldTables& tables, const ByteView* from, ByteSpan to);

/// Bytes of a player command: Call of Duty 2 28, United Offensive 24.
inline std::size_t usercmd_size(Game game) { return game == Game::CallOfDuty2 ? 28 : 24; }

/// The player command a client's packet codes its first command against:
/// zeros, then the weapon bytes, the pitch and yaw relative to the delta
/// angles, and (with the player state's lean/stance flag) the matching
/// buttons, from the player state `ps` (see the Rust version).
/// Original: MSG_SetDefaultUserCmd at 0x004449C0 (CoD2MP_s.exe 1.3; ps on the stack, cmd in ebx)
/// Original: MSG_SetDefaultUserCmd at 0x0044A1E0 (CoDUOMP.exe 1.51; ps on the stack, cmd in edi)
void set_default_usercmd(Game game, ByteView ps, ByteSpan cmd);

/// Writes a player command as a change from `from`, keyed with `key`.
/// Original: MSG_WriteDeltaUsercmdKey at 0x00444B10 (CoD2MP_s.exe 1.3)
/// Original: MSG_WriteDeltaUsercmdKey at 0x0044A360 (CoDUOMP.exe 1.51)
void write_delta_usercmd_key(Msg& msg, std::int32_t key, ByteView from, ByteView to);

/// Reads what write_delta_usercmd_key() wrote into `to`.
/// Original: MSG_ReadDeltaUsercmdKey at 0x00444EC0 (CoD2MP_s.exe 1.3)
/// Original: MSG_ReadDeltaUsercmdKey at 0x0044A770 (CoDUOMP.exe 1.51)
void read_delta_usercmd_key(Msg& msg, std::int32_t key, ByteView from, ByteSpan to);

}  // namespace iw_pc::net
