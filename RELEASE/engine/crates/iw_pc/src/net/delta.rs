//! Delta coding: how the games send a structure (an entity, a client, the
//! player's own state) as the fields that changed since the last one the
//! other side has.
//!
//! A structure is sent field by field from a *field table*, the games'
//! `netField_t` arrays: each field's offset in the structure and its bit
//! count. The tables are game data, so they are not in this code: they are
//! read from the player's executable ([`protocol_data`](super::protocol_data))
//! into [`FieldTables`] once, at start.
//!
//! The structures themselves are handled here as their bytes (little-endian,
//! as the games keep them), and every field as the 32-bit word at its offset,
//! which is how the originals compare and send them. Typed views come with
//! the server and client code that fill them.
//!
//! # A field on the wire
//!
//! Unchanged: one 0 bit. Changed: a 1 bit, then by the field's bit count:
//!
//! | bits | value |
//! |---|---|
//! | 0 (a float) | 0.0 → a 0 bit; a whole number in [-4096, 4096) → 0 bit, its low 5 bits (biased by 4096), then the rest as a raw byte; anything else → 1 bit and the raw 32 bits |
//! | -99 (Call of Duty 2 only: HUD positions) | as a float, biased by 512 with 2 bits in the bit stream |
//! | -100 (an angle) | 0 → a 0 bit; else a 1 bit and the angle in 16 bits |
//! | any other | 0 → a 0 bit; else a 1 bit, the low `|bits| % 8` bits in the bit stream, then whole raw bytes, lowest first |

use super::msg::{ftol2, Msg};
use super::protocol_data::{GameExecutable, ProtocolDataError, ProtocolTable};
use crate::Game;

/// The most fields any table has (United Offensive's player state: 114).
pub const MAX_FIELDS: usize = 128;

/// The bit count of a field sent as a 16-bit angle.
pub const ANGLE_FIELD: i32 = -100;

/// The bit count of a Call of Duty 2 HUD position (a float sent like the
/// whole-number floats, but biased by 512 with 2 bits).
pub const HUD_POSITION_FIELD: i32 = -99;

/// Bits of an entity number (1024 entities).
pub const ENTITY_NUMBER_BITS: u32 = 10;

/// Bits of a client number in a client state (64 clients).
pub const CLIENT_NUMBER_BITS: u32 = 6;

/// One field of a table: where it is in the structure and how it is sent.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub struct NetField {
    /// Its byte offset in the structure.
    pub offset: u32,
    /// Its bit count: 0 a float, [`ANGLE_FIELD`], [`HUD_POSITION_FIELD`], or
    /// the number of bits of an integer (the sign is ignored).
    pub bits: i32,
}

/// The longest field name kept ([`FieldTable::name`]).
pub const MAX_FIELD_NAME: usize = 32;

/// A field table, as read from the executable.
#[derive(Debug, Clone)]
pub struct FieldTable {
    fields: [NetField; MAX_FIELDS],
    count: usize,
    names: [[u8; MAX_FIELD_NAME]; MAX_FIELDS],
    preceding_offset: Option<u32>,
}

impl FieldTable {
    /// Reads a table of `netField_t` entries (12 bytes: name pointer,
    /// offset, bits; little-endian).
    pub fn from_bytes(bytes: &[u8]) -> FieldTable {
        let mut table = FieldTable {
            fields: [NetField::default(); MAX_FIELDS],
            count: 0,
            names: [[0; MAX_FIELD_NAME]; MAX_FIELDS],
            preceding_offset: None,
        };
        for (slot, entry) in table.fields.iter_mut().zip(bytes.chunks_exact(12)) {
            let word = |at: usize| {
                i32::from_le_bytes([entry[at], entry[at + 1], entry[at + 2], entry[at + 3]])
            };
            *slot = NetField {
                offset: word(4) as u32,
                bits: word(8),
            };
            table.count += 1;
        }
        table
    }

    /// Reads a table and its field names (the name pointers point into the
    /// same executable).
    pub fn from_executable(exe: &GameExecutable, bytes: &[u8]) -> FieldTable {
        let mut table = FieldTable::from_bytes(bytes);
        for (i, entry) in bytes.chunks_exact(12).take(table.count).enumerate() {
            let pointer = u32::from_le_bytes([entry[0], entry[1], entry[2], entry[3]]);
            if let Ok(text) = exe.read(pointer, MAX_FIELD_NAME) {
                let end = text.iter().position(|&c| c == 0).unwrap_or(MAX_FIELD_NAME);
                table.names[i][..end].copy_from_slice(&text[..end]);
            }
        }
        table
    }

    /// Field `i`'s name as the game's table spells it (`"clientNum"`,
    /// `"pos.trBase[0]"`), or empty when it wasn't read.
    pub fn name(&self, i: usize) -> &[u8] {
        let name = &self.names[i];
        &name[..name.iter().position(|&c| c == 0).unwrap_or(MAX_FIELD_NAME)]
    }

    /// The field called `name`, if the table has one.
    pub fn field(&self, name: &str) -> Option<NetField> {
        (0..self.count)
            .find(|&i| self.name(i) == name.as_bytes())
            .map(|i| self.fields[i])
    }

    /// The fields, in the order they are sent.
    pub fn fields(&self) -> &[NetField] {
        &self.fields[..self.count]
    }

    /// The smallest structure every field fits in, in bytes.
    pub fn struct_size(&self) -> usize {
        self.fields()
            .iter()
            .map(|f| f.offset as usize + 4)
            .max()
            .unwrap_or(0)
    }

    /// The offset word of the 12 bytes in front of the table in the
    /// executable, which the structure reader takes as field -1 when a
    /// message ends where its field count should be ([`read_delta_struct`]);
    /// `None` when it wasn't read.
    pub fn preceding_offset(&self) -> Option<u32> {
        self.preceding_offset
    }
}

/// A game's field tables, read and checked once from its executable.
#[derive(Debug, Clone)]
pub struct FieldTables {
    /// Which game they belong to.
    pub game: Game,
    /// Entities (`entityState_t`).
    pub entity: FieldTable,
    /// United Offensive's table for entities of type 12 (`None` in Call of
    /// Duty 2).
    pub entity_alternate: Option<FieldTable>,
    /// The player's own state (`playerState_t`).
    pub player: FieldTable,
    /// Clients (`clientState_t`): name, team, model of every player.
    pub client: FieldTable,
    /// Archived entities (demos and killcams).
    pub archived_entity: FieldTable,
    /// HUD elements.
    pub hud_elem: FieldTable,
    /// Objectives.
    pub objective: FieldTable,
}

impl FieldTables {
    /// Reads every table from `exe`, each checked against the supported
    /// build's hash.
    pub fn from_executable(exe: &GameExecutable) -> Result<FieldTables, ProtocolDataError> {
        let game = exe.game();
        let table = |t: ProtocolTable| -> Result<FieldTable, ProtocolDataError> {
            let mut table = FieldTable::from_executable(exe, exe.table(t)?);
            table.preceding_offset = t
                .location(game)
                .and_then(|l| exe.read(l.address.wrapping_sub(8), 4).ok())
                .map(|b| u32::from_le_bytes([b[0], b[1], b[2], b[3]]));
            Ok(table)
        };
        Ok(FieldTables {
            game,
            entity: table(ProtocolTable::EntityStateFields)?,
            entity_alternate: match game {
                Game::CallOfDuty2 => None,
                Game::UnitedOffensive => Some(table(ProtocolTable::EntityStateFieldsAlternate)?),
            },
            player: table(ProtocolTable::PlayerStateFields)?,
            client: table(ProtocolTable::ClientStateFields)?,
            archived_entity: table(ProtocolTable::ArchivedEntityFields)?,
            hud_elem: table(ProtocolTable::HudElemFields)?,
            objective: table(ProtocolTable::ObjectiveFields)?,
        })
    }
}

/// The 32-bit word at `offset` in a structure's bytes (0 past its end).
#[inline]
pub fn word_at(bytes: &[u8], offset: u32) -> i32 {
    let at = offset as usize;
    match bytes.get(at..at + 4) {
        Some(b) => i32::from_le_bytes([b[0], b[1], b[2], b[3]]),
        None => 0,
    }
}

/// Stores the 32-bit word at `offset` in a structure's bytes. An offset past
/// the end is skipped: only a broken message gets the originals to write
/// outside a structure (see [`read_delta_struct`]).
#[inline]
pub fn set_word(bytes: &mut [u8], offset: u32, value: i32) {
    let at = offset as usize;
    if let Some(b) = bytes.get_mut(at..at + 4) {
        b.copy_from_slice(&value.to_le_bytes());
    }
}

/// Copies one field's word from `from` to `to`.
#[inline]
fn copy_word(from: &[u8], to: &mut [u8], offset: u32) {
    set_word(to, offset, word_at(from, offset));
}

/// The entity, client or HUD element number: a structure's first word.
#[inline]
fn number_of(state: &[u8]) -> i32 {
    word_at(state, 0)
}

/// A float field sent as a whole number when it is one and fits:
/// `bias` (4096 or 512) added, `low_bits` (5 or 2) in the bit stream and the
/// rest as a raw byte; else a 1 bit and the raw 32 bits.
fn write_float(msg: &mut Msg, raw: i32, bias: i32, low_bits: u32) {
    let value = f32::from_bits(raw as u32);
    // `__ftol2` gives 64 bits; the code keeps the low 32.
    let truncated = ftol2(f64::from(value)) as i32;
    if value == 0.0 {
        msg.write_bit0();
        return;
    }
    msg.write_bit1();
    let biased = truncated.wrapping_add(bias);
    if f64::from(truncated) == f64::from(value) && (0..bias * 2).contains(&biased) {
        msg.write_bit0();
        msg.write_bits(biased, low_bits);
        msg.write_byte((biased >> low_bits) as u8);
    } else {
        msg.write_bit1();
        msg.write_long(raw);
    }
}

/// Writes one field of `to`, as a change from `from` (module documentation:
/// "A field on the wire"). Fields are compared as 32-bit words, so +0.0 and
/// -0.0 differ (and -0.0 is then sent as 0.0).
///
/// Original: `MSG_WriteDeltaField` at 0x00445180 (CoD2MP_s.exe 1.3)
/// Original: `MSG_WriteDeltaField` at 0x0044AA60 (CoDUOMP.exe 1.51)
pub fn write_delta_field(msg: &mut Msg, from: &[u8], to: &[u8], field: NetField) {
    let old = word_at(from, field.offset);
    let new = word_at(to, field.offset);
    if old == new {
        msg.write_bit0();
        return;
    }
    msg.write_bit1();
    match field.bits {
        0 => write_float(msg, new, 4096, 5),
        HUD_POSITION_FIELD if msg.game == Game::CallOfDuty2 => write_float(msg, new, 512, 2),
        ANGLE_FIELD => {
            if new == 0 {
                msg.write_bit0();
            } else {
                msg.write_bit1();
                msg.write_angle16(f32::from_bits(new as u32));
            }
        }
        bits => {
            if new == 0 {
                msg.write_bit0();
                return;
            }
            msg.write_bit1();
            let mut left = bits.unsigned_abs();
            let mut value = new;
            let low = left & 7;
            if low != 0 {
                msg.write_bits(value, low);
                value >>= low;
                left -= low;
            }
            while left != 0 {
                msg.write_byte(value as u8);
                value >>= 8;
                left -= 8;
            }
        }
    }
}

/// A float sent by [`write_float`] after its "is zero" bit: a 0 bit, the low
/// `low_bits` bits and a raw byte of the whole number plus `bias`, or a 1
/// bit and the raw 32 bits. Returns the float's bits. A byte missing at the
/// end of the message reads as -1 (see [`Msg::read_byte`]).
fn read_float(msg: &mut Msg, bias: i32, low_bits: u32) -> i32 {
    if msg.read_bit() != 0 {
        return msg.read_long();
    }
    let low = msg.read_bits(low_bits);
    let high = msg.read_byte();
    let whole = low
        .wrapping_add(high.wrapping_shl(low_bits))
        .wrapping_sub(bias);
    // Exact: the number is within 24 bits.
    (whole as f32).to_bits() as i32
}

/// An integer of `bits` bits (the sign ignored): the low `|bits| % 8` bits,
/// then raw bytes, lowest first, each shifted by its place modulo 32 (as
/// x86 shifts count). A missing byte reads as -1, which sets every bit from
/// its place up. Call of Duty 2 extends the sign of a field with a negative
/// bit count; United Offensive doesn't.
fn read_integer(msg: &mut Msg, bits: i32) -> i32 {
    let width = bits.wrapping_abs();
    let low = width & 7;
    let mut value = if low != 0 {
        msg.read_bits(low as u32)
    } else {
        0
    };
    let mut shift = low;
    while shift < width {
        value |= msg.read_byte().wrapping_shl(shift as u32);
        shift += 8;
    }
    if bits < 0
        && msg.game == Game::CallOfDuty2
        && value & 1i32.wrapping_shl(width.wrapping_sub(1) as u32) != 0
    {
        value |= !1i32.wrapping_shl(width as u32).wrapping_sub(1);
    }
    value
}

/// Reads one field into `to`, as [`write_delta_field`] wrote it: unchanged,
/// it is copied from `from`; changed and "zero", it is 0.
///
/// A message that runs out reads the way the originals do (Call of Duty 2:
/// flag bits as 1, numbers as -1 and the message marked overflowed; United
/// Offensive reads its bits on past the end). The originals can also print
/// each field read (the `cl_shownet` setting); that debug output is left
/// out.
///
/// Original: `MSG_ReadDeltaField` at 0x00445700 (CoD2MP_s.exe 1.3)
/// Original: `MSG_ReadDeltaField` at 0x0044B030 (CoDUOMP.exe 1.51)
pub fn read_delta_field(msg: &mut Msg, from: &[u8], to: &mut [u8], field: NetField) {
    if msg.read_bit() == 0 {
        copy_word(from, to, field.offset);
        return;
    }
    let value = if msg.read_bit() == 0 {
        0
    } else {
        match field.bits {
            0 => read_float(msg, 4096, 5),
            HUD_POSITION_FIELD if msg.game == Game::CallOfDuty2 => read_float(msg, 512, 2),
            ANGLE_FIELD => msg.read_angle16().to_bits() as i32,
            bits => read_integer(msg, bits),
        }
    };
    set_word(to, field.offset, value);
}

/// United Offensive's extra bit after an entity's number: which table the
/// rest is in (1 for entities of type 12, which use the second table).
pub type TableFlag = fn(&mut Msg, Option<&[u8]>);

/// United Offensive's [`TableFlag`] for entities.
///
/// Original: `MSG_WriteEntityTableFlag` at 0x0044AF00 (CoDUOMP.exe 1.51; the
/// callback `MSG_WriteDeltaEntity` passes)
pub fn write_entity_table_flag(msg: &mut Msg, state: Option<&[u8]>) {
    match state {
        Some(state) if word_at(state, 4) == ALTERNATE_ENTITY_TYPE => msg.write_bit1(),
        _ => msg.write_bit0(),
    }
}

/// The entity type (`eType`, at offset 4) United Offensive sends with its
/// second entity table.
pub const ALTERNATE_ENTITY_TYPE: i32 = 12;

/// How a structure is delta-coded (the arguments the games pass with each
/// table besides the states).
#[derive(Debug, Clone, Copy)]
pub struct StructCoding<'t> {
    /// The fields.
    pub fields: &'t [NetField],
    /// Bits of the structure's number.
    pub index_bits: u32,
    /// Whether every structure starts with a 1 bit ("one more follows";
    /// clients), so a list ends with a 0 bit instead of a reserved number.
    pub change_bit: bool,
    /// United Offensive's entity table flag, written after the number.
    pub table_flag: Option<TableFlag>,
}

/// Writes `to` as a change from `from`.
///
/// * `to` is `None`: the structure is removed: its number (from `from`) and
///   a 1 bit.
/// * Nothing changed: nothing at all, unless `force`, then the number and
///   two 0 bits.
/// * Otherwise: the number, a 0 bit and a 1 bit, a raw byte with the index of
///   the last changed field + 1, then every field up to it
///   ([`write_delta_field`]).
///
/// Each of these starts with a 1 bit when the coding has a change bit, and
/// United Offensive's entities add their table flag after the number. (The
/// originals also print the removal when the `cl_shownet` setting is 2 or
/// -1; that debug output is left out.)
///
/// Original: `MSG_WriteDeltaStruct` at 0x00445480 (CoD2MP_s.exe 1.3)
/// Original: `MSG_WriteDeltaStruct` at 0x0044ACD0 (CoDUOMP.exe 1.51)
pub fn write_delta_struct(
    msg: &mut Msg,
    from: &[u8],
    to: Option<&[u8]>,
    force: bool,
    coding: StructCoding,
) {
    let Some(to) = to else {
        if coding.change_bit {
            msg.write_bit1();
        }
        msg.write_bits(number_of(from), coding.index_bits);
        if let Some(flag) = coding.table_flag {
            flag(msg, None);
        }
        msg.write_bit1();
        return;
    };
    let last_changed = coding
        .fields
        .iter()
        .rposition(|f| word_at(from, f.offset) != word_at(to, f.offset))
        .map_or(0, |i| i + 1);
    if last_changed == 0 && !force {
        return;
    }
    if coding.change_bit {
        msg.write_bit1();
    }
    msg.write_bits(number_of(to), coding.index_bits);
    if let Some(flag) = coding.table_flag {
        flag(msg, Some(to));
    }
    msg.write_bit0();
    if last_changed == 0 {
        msg.write_bit0();
        return;
    }
    msg.write_bit1();
    msg.write_byte(last_changed as u8);
    for &field in &coding.fields[..last_changed] {
        write_delta_field(msg, from, to, field);
    }
}

/// Writes an entity as a change from `from` (`to` `None`: removed). Call of
/// Duty 2: its entity table; United Offensive: the second table for entities
/// of type 12, with the table flag after the number.
///
/// Original: `MSG_WriteDeltaEntity` at 0x00445680 (CoD2MP_s.exe 1.3)
/// Original: `MSG_WriteDeltaEntity` at 0x0044AF50 (CoDUOMP.exe 1.51)
pub fn write_delta_entity(
    msg: &mut Msg,
    tables: &FieldTables,
    from: &[u8],
    to: Option<&[u8]>,
    force: bool,
) {
    let (table, table_flag): (&FieldTable, Option<TableFlag>) = match tables.game {
        Game::CallOfDuty2 => (&tables.entity, None),
        Game::UnitedOffensive => {
            let alternate = matches!(to, Some(t) if word_at(t, 4) == ALTERNATE_ENTITY_TYPE);
            let table = match (&tables.entity_alternate, alternate) {
                (Some(second), true) => second,
                _ => &tables.entity,
            };
            (table, Some(write_entity_table_flag as TableFlag))
        }
    };
    let coding = StructCoding {
        fields: table.fields(),
        index_bits: ENTITY_NUMBER_BITS,
        change_bit: false,
        table_flag,
    };
    write_delta_struct(msg, from, to, force, coding);
}

/// Bytes of a client state the originals zero when there is no previous one.
const CLIENT_STATE_SIZE: usize = 0x5C;

/// Writes a client state as a change from `from` (`None`: from all zeros;
/// `to` `None`: removed). Every client starts with a 1 bit.
///
/// Original: `MSG_WriteDeltaClient` at 0x004456C0 (CoD2MP_s.exe 1.3)
/// Original: `MSG_WriteDeltaClient` at 0x0044AFE0 (CoDUOMP.exe 1.51)
pub fn write_delta_client(
    msg: &mut Msg,
    tables: &FieldTables,
    from: Option<&[u8]>,
    to: Option<&[u8]>,
    force: bool,
) {
    const ZEROS: [u8; CLIENT_STATE_SIZE] = [0; CLIENT_STATE_SIZE];
    let coding = StructCoding {
        fields: tables.client.fields(),
        index_bits: CLIENT_NUMBER_BITS,
        change_bit: true,
        table_flag: None,
    };
    write_delta_struct(msg, from.unwrap_or(&ZEROS), to, force, coding);
}

/// Reads structure number `number` (the caller has read it) as a change from
/// `from` into `to`, as [`write_delta_struct`] wrote it after the number,
/// with the fields of `table`. Returns whether the structure was removed;
/// `to` is then left as it was.
///
/// * Not removed, nothing changed: `to` becomes a copy of `from` (its first
///   `4 * (fields + 1)` bytes, the number included: the originals don't set
///   it on this path).
/// * Changed: `to`'s number is set, the fields up to the count read
///   ([`read_delta_field`]) and the rest copied from `from`.
///
/// A count past the table: Call of Duty 2 marks the message overflowed and
/// stops; United Offensive reads the whole table. A count missing at the end
/// of the message (-1) has every field copied, starting with the one at the
/// table's [`preceding_offset`](FieldTable::preceding_offset) (the originals
/// take the entry in front of the table as field -1). The originals' debug
/// output (`cl_shownet`) is left out.
///
/// Original: `MSG_ReadDeltaStruct` at 0x00445A40 (CoD2MP_s.exe 1.3)
/// Original: `MSG_ReadDeltaStruct` at 0x0044B280 (CoDUOMP.exe 1.51)
pub fn read_delta_struct(
    msg: &mut Msg,
    from: &[u8],
    to: &mut [u8],
    number: i32,
    table: &FieldTable,
) -> bool {
    // A message that ran out (Call of Duty 2: -1) is not a removal...
    if msg.read_bit() == 1 {
        return true;
    }
    let fields = table.fields();
    // ... but is a change.
    if msg.read_bit() == 0 {
        let n = (4 * (fields.len() + 1)).min(from.len()).min(to.len());
        to[..n].copy_from_slice(&from[..n]);
        return false;
    }
    let mut count = msg.read_byte();
    if count > fields.len() as i32 {
        if msg.game == Game::CallOfDuty2 {
            msg.overflowed = true;
            return false;
        }
        count = fields.len() as i32;
    }
    set_word(to, 0, number);
    let read = count.max(0) as usize;
    for &field in &fields[..read] {
        read_delta_field(msg, from, to, field);
    }
    if count < 0 {
        if let Some(offset) = table.preceding_offset() {
            copy_word(from, to, offset);
        }
    }
    for field in &fields[read..] {
        copy_word(from, to, field.offset);
    }
    false
}

/// Reads entity `number` as a change from `from` (see [`read_delta_struct`];
/// returns whether it was removed). United Offensive first reads its table
/// flag (1: the second table, for entities of type 12).
///
/// Original: `MSG_ReadDeltaEntity` at 0x00445BE0 (CoD2MP_s.exe 1.3)
/// Original: `MSG_ReadDeltaEntity` at 0x0044B420 (CoDUOMP.exe 1.51)
pub fn read_delta_entity(
    msg: &mut Msg,
    tables: &FieldTables,
    from: &[u8],
    to: &mut [u8],
    number: i32,
) -> bool {
    let table = match (tables.game, &tables.entity_alternate) {
        (Game::UnitedOffensive, Some(second)) if msg.read_bit() != 0 => second,
        _ => &tables.entity,
    };
    read_delta_struct(msg, from, to, number, table)
}

/// Reads archived entity `number` (the entities kept for killcams and demos)
/// as a change from `from` (see [`read_delta_struct`]).
///
/// Original: `MSG_ReadDeltaArchivedEntity` at 0x00445C00 (CoD2MP_s.exe 1.3)
/// Original: `MSG_ReadDeltaArchivedEntity` at 0x0044B4A0 (CoDUOMP.exe 1.51)
pub fn read_delta_archived_entity(
    msg: &mut Msg,
    tables: &FieldTables,
    from: &[u8],
    to: &mut [u8],
    number: i32,
) -> bool {
    read_delta_struct(msg, from, to, number, &tables.archived_entity)
}

/// Reads client `number` as a change from `from` (`None`: from all zeros;
/// see [`read_delta_struct`]). The "one more client" bit in front of it is
/// the caller's.
///
/// Original: `MSG_ReadDeltaClient` at 0x00445C20 (CoD2MP_s.exe 1.3)
/// Original: `MSG_ReadDeltaClient` at 0x0044B4C0 (CoDUOMP.exe 1.51)
pub fn read_delta_client(
    msg: &mut Msg,
    tables: &FieldTables,
    from: Option<&[u8]>,
    to: &mut [u8],
    number: i32,
) -> bool {
    const ZEROS: [u8; CLIENT_STATE_SIZE] = [0; CLIENT_STATE_SIZE];
    read_delta_struct(msg, from.unwrap_or(&ZEROS), to, number, &tables.client)
}

// --- the player's own state ------------------------------------------------------------

/// Where the parts of a player state (`playerState_t`) are, in one game.
/// These are the structure's layout as the players state writer walks it,
/// not table data.
#[derive(Debug, Clone, Copy)]
struct PlayerStateLayout {
    /// The structure's size (the originals zero this much when there is no
    /// previous state).
    size: usize,
    /// Six statistics (health and the like): three shorts, a 6-bit number,
    /// a short, a byte.
    stats: usize,
    /// Ammunition: 4 groups of 16 numbers, each sent as a short.
    ammo: usize,
    /// Ammunition in the clip: the same.
    clip: usize,
    /// 16 objectives of 28 bytes.
    objectives: usize,
    /// The two HUD element arrays, back to back: the archived ones, then the
    /// current ones.
    hud: usize,
    /// HUD elements per array.
    hud_count: usize,
    /// Bits of the number of HUD elements sent.
    hud_count_bits: u32,
    /// Bytes of one HUD element.
    hud_elem_size: usize,
}

const COD2_PLAYER_STATE: PlayerStateLayout = PlayerStateLayout {
    size: 0x26A8,
    stats: 0x12C,
    ammo: 0x144,
    clip: 0x344,
    objectives: 0x5E4,
    hud: 0x7A8,
    hud_count: 31,
    hud_count_bits: 5,
    hud_elem_size: 0x80,
};

const UO_PLAYER_STATE: PlayerStateLayout = PlayerStateLayout {
    size: 0x4504,
    stats: 0x11C,
    ammo: 0x134,
    clip: 0x334,
    objectives: 0x638,
    hud: 0x7F8,
    hud_count: 63,
    hud_count_bits: 6,
    hud_elem_size: 0x7C,
};

fn player_state_layout(game: Game) -> &'static PlayerStateLayout {
    match game {
        Game::CallOfDuty2 => &COD2_PLAYER_STATE,
        Game::UnitedOffensive => &UO_PLAYER_STATE,
    }
}

/// The size of `game`'s player state in bytes.
pub fn player_state_size(game: Game) -> usize {
    player_state_layout(game).size
}

/// HUD elements in each of the player state's two arrays (Call of Duty 2 31,
/// United Offensive 63): the length the player state's coders pass to
/// [`write_delta_hud_elems`] and [`read_delta_hud_elems`].
pub fn hud_elem_count(game: Game) -> usize {
    player_state_layout(game).hud_count
}

/// Bytes of one HUD element (Call of Duty 2 0x80, United Offensive 0x7C).
pub fn hud_elem_size(game: Game) -> usize {
    player_state_layout(game).hud_elem_size
}

/// Objectives in a player state, and the bytes of one.
const OBJECTIVES: usize = 16;
const OBJECTIVE_SIZE: usize = 28;

/// Ammunition groups (of 16) in each of the two ammunition arrays.
const AMMO_GROUPS: usize = 4;

/// The largest player state (United Offensive's), all zeros: the previous
/// state when there is none.
static ZERO_PLAYER_STATE: [u8; 0x4504] = [0; 0x4504];

/// One field of the player state's field table. The same as
/// [`write_delta_field`] except that floats and integers have no "is zero"
/// bit (as in Quake III's player state; angles keep theirs).
fn write_player_field(msg: &mut Msg, from: &[u8], to: &[u8], field: NetField) {
    let old = word_at(from, field.offset);
    let new = word_at(to, field.offset);
    if old == new {
        msg.write_bit0();
        return;
    }
    msg.write_bit1();
    match field.bits {
        0 => {
            let value = f32::from_bits(new as u32);
            let truncated = ftol2(f64::from(value)) as i32;
            let biased = truncated.wrapping_add(4096);
            if f64::from(truncated) == f64::from(value) && (0..8192).contains(&biased) {
                msg.write_bit0();
                msg.write_bits(biased, 5);
                msg.write_byte((biased >> 5) as u8);
            } else {
                msg.write_bit1();
                msg.write_long(new);
            }
        }
        ANGLE_FIELD => {
            if new == 0 {
                msg.write_bit0();
            } else {
                msg.write_bit1();
                msg.write_angle16(f32::from_bits(new as u32));
            }
        }
        bits => {
            let mut left = bits.unsigned_abs();
            let mut value = new;
            let low = left & 7;
            if low != 0 {
                msg.write_bits(value, low);
                value >>= low;
                left -= low;
            }
            while left != 0 {
                msg.write_byte(value as u8);
                value >>= 8;
                left -= 8;
            }
        }
    }
}

/// A 16-bit mask of which of 16 numbers (at `at`, 4 bytes apart) differ.
fn changed_mask(from: &[u8], to: &[u8], at: usize) -> u32 {
    (0..16).fold(0, |mask, j| {
        let offset = (at + 4 * j) as u32;
        if word_at(from, offset) != word_at(to, offset) {
            mask | 1 << j
        } else {
            mask
        }
    })
}

/// A group of 16 numbers: the mask as a short, then each changed number as
/// a short.
fn write_short_group(msg: &mut Msg, to: &[u8], at: usize, mask: u32) {
    msg.write_short(mask as i16);
    for j in 0..16 {
        if mask & 1 << j != 0 {
            msg.write_short(word_at(to, (at + 4 * j) as u32) as i16);
        }
    }
}

/// Writes one array of HUD elements (`from` and `to`: arrays of `count`
/// elements; the games pass [`hud_elem_count`]) as a change from `from`: how
/// many are in use at the start of `to` (type not 0; 5 bits in Call of Duty
/// 2, 6 in United Offensive), then each as the index of its last changed
/// field in 5 bits and the fields up to and including it (at least the first
/// one, even unchanged).
///
/// Original: `MSG_WriteDeltaHudElems` at 0x00445C60 (CoD2MP_s.exe 1.3; count in ecx)
/// Original: `MSG_WriteDeltaHudElems` at 0x0044B510 (CoDUOMP.exe 1.51; count in ecx, from in ebx)
pub fn write_delta_hud_elems(
    msg: &mut Msg,
    tables: &FieldTables,
    from: &[u8],
    to: &[u8],
    count: usize,
) {
    let layout = player_state_layout(tables.game);
    let fields = tables.hud_elem.fields();
    let size = layout.hud_elem_size;
    let in_use = (0..count)
        .take_while(|&i| word_at(to, (i * size) as u32) != 0)
        .count();
    msg.write_bits(in_use as i32, layout.hud_count_bits);
    for i in 0..in_use {
        let (old, new) = (&from[i * size..][..size], &to[i * size..][..size]);
        let last = fields
            .iter()
            .rposition(|f| word_at(old, f.offset) != word_at(new, f.offset))
            .unwrap_or(0);
        msg.write_bits(last as i32, 5);
        for &field in &fields[..=last.min(fields.len().saturating_sub(1))] {
            write_delta_field(msg, old, new, field);
        }
    }
}

/// Writes the player's own state as a change from `from` (`None`: from all
/// zeros): the fields of the player state table up to the last changed one,
/// the six statistics, the two ammunition arrays, the objectives, and the
/// HUD elements (current, then archived), each part behind a "changed" bit.
///
/// `to` (and `from`) must be at least [`player_state_size`] bytes.
///
/// Original: `MSG_WriteDeltaPlayerstate` at 0x00445EC0 (CoD2MP_s.exe 1.3)
/// Original: `MSG_WriteDeltaPlayerstate` at 0x0044B7B0 (CoDUOMP.exe 1.51)
pub fn write_delta_playerstate(
    msg: &mut Msg,
    tables: &FieldTables,
    from: Option<&[u8]>,
    to: &[u8],
) {
    let layout = player_state_layout(tables.game);
    let from = from.unwrap_or(&ZERO_PLAYER_STATE[..layout.size]);

    // The fields up to the last changed one.
    let fields = tables.player.fields();
    let last_changed = fields
        .iter()
        .rposition(|f| word_at(from, f.offset) != word_at(to, f.offset))
        .map_or(0, |i| i + 1);
    msg.write_byte(last_changed as u8);
    for &field in &fields[..last_changed] {
        write_player_field(msg, from, to, field);
    }

    // Statistics.
    let stat = |state: &[u8], i: usize| word_at(state, (layout.stats + 4 * i) as u32);
    let stats_mask = (0..6).fold(0u32, |m, i| {
        if stat(from, i) != stat(to, i) {
            m | 1 << i
        } else {
            m
        }
    });
    if stats_mask == 0 {
        msg.write_bit0();
    } else {
        msg.write_bit1();
        msg.write_bits(stats_mask as i32, 6);
        for i in 0..6 {
            if stats_mask & 1 << i == 0 {
                continue;
            }
            match i {
                3 => msg.write_bits(stat(to, i), 6),
                5 => msg.write_byte(stat(to, i) as u8),
                _ => msg.write_short(stat(to, i) as i16),
            }
        }
    }

    // Ammunition: a bit for the whole array, then one per group.
    let mut ammo_masks = [0u32; AMMO_GROUPS];
    for (g, mask) in ammo_masks.iter_mut().enumerate() {
        *mask = changed_mask(from, to, layout.ammo + 64 * g);
    }
    if ammo_masks.iter().all(|&m| m == 0) {
        msg.write_bit0();
    } else {
        msg.write_bit1();
        for (g, &mask) in ammo_masks.iter().enumerate() {
            if mask == 0 {
                msg.write_bit0();
            } else {
                msg.write_bit1();
                write_short_group(msg, to, layout.ammo + 64 * g, mask);
            }
        }
    }

    // Ammunition in the clip: one bit per group.
    for g in 0..AMMO_GROUPS {
        let at = layout.clip + 64 * g;
        let mask = changed_mask(from, to, at);
        if mask == 0 {
            msg.write_bit0();
        } else {
            msg.write_bit1();
            write_short_group(msg, to, at, mask);
        }
    }

    // Objectives: each one's state in 3 bits, then its fields when any changed.
    let all = layout.objectives..layout.objectives + OBJECTIVES * OBJECTIVE_SIZE;
    if from[all.clone()] == to[all] {
        msg.write_bit0();
    } else {
        msg.write_bit1();
        let objective_fields = tables.objective.fields();
        for k in 0..OBJECTIVES {
            let at = layout.objectives + k * OBJECTIVE_SIZE;
            let (old, new) = (&from[at..at + OBJECTIVE_SIZE], &to[at..at + OBJECTIVE_SIZE]);
            msg.write_bits(word_at(new, 0), 3);
            if objective_fields
                .iter()
                .any(|f| word_at(old, f.offset) != word_at(new, f.offset))
            {
                msg.write_bit1();
                for &field in objective_fields {
                    write_delta_field(msg, old, new, field);
                }
            } else {
                msg.write_bit0();
            }
        }
    }

    // HUD elements: the current array, then the archived one.
    let array = layout.hud_count * layout.hud_elem_size;
    let (archived, current) = (layout.hud, layout.hud + array);
    if from[archived..current + array] == to[archived..current + array] {
        msg.write_bit0();
    } else {
        msg.write_bit1();
        for at in [current, archived] {
            let range = at..at + array;
            write_delta_hud_elems(
                msg,
                tables,
                &from[range.clone()],
                &to[range],
                layout.hud_count,
            );
        }
    }
}

/// One field of the player state, as [`write_player_field`] wrote it (no
/// "is zero" bit for floats and integers).
fn read_player_field(msg: &mut Msg, from: &[u8], to: &mut [u8], field: NetField) {
    if msg.read_bit() == 0 {
        copy_word(from, to, field.offset);
        return;
    }
    let value = match field.bits {
        0 => read_float(msg, 4096, 5),
        ANGLE_FIELD => {
            if msg.read_bit() == 0 {
                0
            } else {
                msg.read_angle16().to_bits() as i32
            }
        }
        bits => read_integer(msg, bits),
    };
    set_word(to, field.offset, value);
}

/// A changed bit, then every field read ([`read_delta_field`]), or, when it
/// is 0, every field copied from `from`: how an objective is read.
///
/// Original: `MSG_ReadDeltaFields` at 0x00445990 (CoD2MP_s.exe 1.3; United
/// Offensive has it inline in its player state reader)
fn read_delta_fields(msg: &mut Msg, fields: &[NetField], from: &[u8], to: &mut [u8]) {
    if msg.read_bit() != 0 {
        for &field in fields {
            read_delta_field(msg, from, to, field);
        }
    } else {
        for field in fields {
            copy_word(from, to, field.offset);
        }
    }
}

/// A group of 16 numbers as [`write_short_group`] wrote it.
fn read_short_group(msg: &mut Msg, to: &mut [u8], at: usize) {
    let mask = msg.read_short();
    for j in 0..16 {
        if mask & 1 << j != 0 {
            let value = msg.read_short();
            set_word(to, (at + 4 * j) as u32, value);
        }
    }
}

/// Reads one array of HUD elements (`count` of them, at `at` in `from` and
/// `to`; the games pass [`hud_elem_count`] and the arrays' place in the
/// player state), as [`write_delta_hud_elems`] wrote it: each element sent
/// has its fields up to the index read and the rest copied from `from`; the
/// elements after the last one sent are zeroed. A count missing at the end of
/// a Call of Duty 2 message (-1) zeroes from the element in front of the
/// array (the original's arithmetic; inside the player state, the elements in
/// front of `at` here; none when `at` is 0); a missing index has every field
/// copied.
///
/// United Offensive's table has 30 fields but the index has 5 bits: an index
/// of 30 or 31, which its writer never sends, has the original read entries
/// past its table (other data, as offsets outside the element). The port
/// reads the 30 fields and marks the message overflowed instead. (A message
/// sending more elements than `count`, which the full length rules out, has
/// the originals write past the array; the port reads `count`.)
///
/// Original: `MSG_ReadDeltaHudElems` at 0x00445DB0 (CoD2MP_s.exe 1.3)
/// Original: `MSG_ReadDeltaHudElems` at 0x0044B690 (CoDUOMP.exe 1.51)
pub fn read_delta_hud_elems(
    msg: &mut Msg,
    tables: &FieldTables,
    from: &[u8],
    to: &mut [u8],
    at: usize,
    count: usize,
) {
    let layout = player_state_layout(tables.game);
    let fields = tables.hud_elem.fields();
    let size = layout.hud_elem_size;
    let hud_count = count;
    let count = msg.read_bits(layout.hud_count_bits);
    let sent = (count.max(0) as usize).min(hud_count);
    for i in 0..sent {
        let elem = at + i * size;
        let (old, new) = (&from[elem..elem + size], &mut to[elem..elem + size]);
        let last = msg.read_bits(5);
        let mut read = 0;
        if last >= 0 {
            read = last as usize + 1;
            if read > fields.len() {
                msg.overflowed = true;
                read = fields.len();
            }
            for &field in &fields[..read] {
                read_delta_field(msg, old, new, field);
            }
        }
        for field in &fields[read..] {
            copy_word(old, new, field.offset);
        }
    }
    let start = at as isize + count.min(hud_count as i32) as isize * size as isize;
    let end = at + hud_count * size;
    to[start.max(0) as usize..end].fill(0);
}

/// Reads the player's own state into `to`, as [`write_delta_playerstate`]
/// wrote it, as a change from `from` (`None`: from all zeros). `to` starts
/// as a copy of `from`; `from` and `to` must be at least
/// [`player_state_size`] bytes.
///
/// A field count past the player state table (which the writers never send)
/// has the originals read entries past their table; the port reads the
/// table and marks the message overflowed. A message that runs out reads as
/// the originals do (see [`read_delta_field`]). The originals' debug output
/// (`cl_shownet`) is left out.
///
/// Original: `MSG_ReadDeltaPlayerstate` at 0x00446CA0 (CoD2MP_s.exe 1.3)
/// Original: `MSG_ReadDeltaPlayerstate` at 0x0044C5C0 (CoDUOMP.exe 1.51)
pub fn read_delta_playerstate(
    msg: &mut Msg,
    tables: &FieldTables,
    from: Option<&[u8]>,
    to: &mut [u8],
) {
    let layout = player_state_layout(tables.game);
    let from = from.unwrap_or(&ZERO_PLAYER_STATE[..layout.size]);
    to[..layout.size].copy_from_slice(&from[..layout.size]);

    // The fields up to the count; the rest stay as copied.
    let fields = tables.player.fields();
    let count = msg.read_byte();
    let mut read = 0;
    if count > 0 {
        read = count as usize;
        if read > fields.len() {
            msg.overflowed = true;
            read = fields.len();
        }
        for &field in &fields[..read] {
            read_player_field(msg, from, to, field);
        }
    }
    for field in &fields[read..] {
        copy_word(from, to, field.offset);
    }

    // Statistics.
    if msg.read_bit() != 0 {
        let mask = msg.read_bits(6);
        for i in 0..6 {
            if mask & 1 << i == 0 {
                continue;
            }
            let value = match i {
                3 => msg.read_bits(6),
                5 => msg.read_byte(),
                _ => msg.read_short(),
            };
            set_word(to, (layout.stats + 4 * i) as u32, value);
        }
    }

    // Ammunition: a bit for the whole array, then one per group.
    if msg.read_bit() != 0 {
        for g in 0..AMMO_GROUPS {
            if msg.read_bit() != 0 {
                read_short_group(msg, to, layout.ammo + 64 * g);
            }
        }
    }

    // Ammunition in the clip: one bit per group.
    for g in 0..AMMO_GROUPS {
        if msg.read_bit() != 0 {
            read_short_group(msg, to, layout.clip + 64 * g);
        }
    }

    // Objectives: each one's state in 3 bits, then its fields.
    if msg.read_bit() != 0 {
        let objective_fields = tables.objective.fields();
        for k in 0..OBJECTIVES {
            let at = layout.objectives + k * OBJECTIVE_SIZE;
            let state = msg.read_bits(3);
            set_word(to, at as u32, state);
            let (old, new) = (
                &from[at..at + OBJECTIVE_SIZE],
                &mut to[at..at + OBJECTIVE_SIZE],
            );
            read_delta_fields(msg, objective_fields, old, new);
        }
    }

    // HUD elements: the current array, then the archived one.
    if msg.read_bit() != 0 {
        let array = layout.hud_count * layout.hud_elem_size;
        for at in [layout.hud + array, layout.hud] {
            read_delta_hud_elems(msg, tables, from, to, at, layout.hud_count);
        }
    }
}

// --- player commands ---------------------------------------------------------------------

/// Bytes of a player command (`usercmd_t`): Call of Duty 2 28, United
/// Offensive 24.
pub fn usercmd_size(game: Game) -> usize {
    match game {
        Game::CallOfDuty2 => 28,
        Game::UnitedOffensive => 24,
    }
}

/// The player command a client's packet codes its first command against:
/// all zeros, then from the player's state `ps` (the latest snapshot's):
/// the weapon bytes, the pitch and yaw as 16-bit steps relative to the
/// state's delta angles (`(-delta - viewangle * -65536 / 360) & 0xFFFF`,
/// at the game's x87 precision, truncated by `__ftol2`); and, when the
/// player is leaning or proning in some way (a flag in the state's
/// `pm_flags`), the matching buttons:
///
/// * Call of Duty 2 (flag 0x800000): the button word gets 0x100 or 0x200
///   (bit 8 or 4 of the word at 0xA0), 0x80 or 0x40 (the float at 0x4C
///   above or below zero), 0x1000 (the float at 0xDC not zero).
/// * United Offensive (flag 0x80000): byte 5 gets 0x40 or 0x80 (bit 0x40
///   or 0x20 of the word at 0x84; byte 0x16 then becomes 0x81), 0x20 or
///   0x10 (the float at 0x44 above or below zero); byte 4 gets 0x10 (the
///   float at 0xE0 not zero) and 8 (`pm_flags` bit 0x10000).
///
/// `ps` is a [`player_state_size`] state, `cmd` [`usercmd_size`] bytes.
///
/// Original: `MSG_SetDefaultUserCmd` at 0x004449C0 (CoD2MP_s.exe 1.3; ps on the stack, cmd in ebx)
/// Original: `MSG_SetDefaultUserCmd` at 0x0044A1E0 (CoDUOMP.exe 1.51; ps on the stack, cmd in edi)
pub fn set_default_usercmd(game: Game, ps: &[u8], cmd: &mut [u8]) {
    let size = usercmd_size(game);
    cmd[..size].fill(0);
    let float_at = |offset: u32| f32::from_bits(word_at(ps, offset) as u32);
    let (delta_angles, angles) = match game {
        Game::CallOfDuty2 => {
            cmd[8] = ps[0xD4];
            cmd[9] = ps[0xD0];
            (0x54, 0xC)
        }
        Game::UnitedOffensive => {
            cmd[6] = ps[0xD8];
            (0x4C, 8)
        }
    };
    for i in 0..2u32 {
        let view = float_at(0xE8 + 4 * i);
        let steps = ftol2(super::msg::scaled(game, view, -super::msg::ANGLE16_SCALE)) as i32;
        let delta = word_at(ps, delta_angles + 4 * i);
        set_word(
            cmd,
            angles + 4 * i,
            delta.wrapping_neg().wrapping_sub(steps) & 0xFFFF,
        );
    }
    let pm_flags = word_at(ps, 0xC);
    match game {
        Game::CallOfDuty2 => {
            if pm_flags & 0x80_0000 == 0 {
                return;
            }
            let mut buttons = word_at(cmd, 4);
            let lean = word_at(ps, 0xA0);
            if lean & 8 != 0 {
                buttons |= 0x100;
            } else if lean & 4 != 0 {
                buttons |= 0x200;
            }
            let f = float_at(0x4C);
            if f > 0.0 {
                buttons |= 0x80;
            } else if f < 0.0 {
                buttons |= 0x40;
            }
            if float_at(0xDC) != 0.0 {
                buttons |= 0x1000;
            }
            set_word(cmd, 4, buttons);
        }
        Game::UnitedOffensive => {
            if pm_flags & 0x8_0000 == 0 {
                return;
            }
            let lean = word_at(ps, 0x84);
            if lean & 0x40 != 0 {
                cmd[5] |= 0x40;
                cmd[0x16] = 0x81;
            } else if lean & 0x20 != 0 {
                cmd[5] |= 0x80;
                cmd[0x16] = 0x81;
            }
            let f = float_at(0x44);
            if f > 0.0 {
                cmd[5] |= 0x20;
            } else if f < 0.0 {
                cmd[5] |= 0x10;
            }
            if float_at(0xE0) != 0.0 {
                cmd[4] |= 0x10;
            }
            if pm_flags & 0x1_0000 != 0 {
                cmd[4] |= 8;
            }
        }
    }
}

/// Where a player command's parts are, in one game.
#[derive(Debug, Clone, Copy)]
struct UsercmdLayout {
    /// The 32-bit button word (Call of Duty 2) or the button byte (United Offensive).
    buttons: usize,
    /// The weapon byte.
    weapon: usize,
    /// Call of Duty 2: the offhand weapon byte; United Offensive: a byte at +6.
    second: usize,
    /// The three 16-bit view angles (kept in 32-bit words).
    angles: usize,
    /// Forward and right movement (signed bytes), and United Offensive's up.
    forward: usize,
}

const COD2_USERCMD: UsercmdLayout = UsercmdLayout {
    buttons: 4,
    weapon: 8,
    second: 9,
    angles: 0xC,
    forward: 0x18,
};

const UO_USERCMD: UsercmdLayout = UsercmdLayout {
    buttons: 4,
    weapon: 5,
    second: 6,
    angles: 8,
    forward: 0x14,
};

/// The direction of a signed movement byte: 1 past 10, 2 below -10, else 0.
fn move_direction(movement: u8) -> i32 {
    match movement as i8 {
        11.. => 1,
        ..=-11 => 2,
        _ => 0,
    }
}

/// The four direction bits of forward and right movement: 1 forward, 2
/// back, 4 right, 8 left. Only these cross the wire: the movement arrives
/// as full speed (127, -127) or none.
fn move_mask(cmd: &[u8], layout: &UsercmdLayout) -> i32 {
    move_direction(cmd[layout.forward]) | move_direction(cmd[layout.forward + 1]) << 2
}

/// Movement bytes back from direction bits: (bit for +127, bit for -127).
fn movement_from(mask: i32, plus: i32, minus: i32) -> u8 {
    if mask & plus != 0 {
        0x7F
    } else if mask & minus != 0 {
        0x81
    } else {
        0
    }
}

/// Writes a player command as a change from `from`, keyed with `key` (the
/// client's checksum feed, message sequence and last server command, see
/// `docs/PC_MULTIPLAYER.md`): the time (a byte of difference when under 256,
/// else 32 bits), then one bit when nothing else changed, else either the
/// short form (first button bit, two angles, movement directions) or the
/// long form (also the third angle, the other buttons and both weapons).
/// Only the direction of movement is sent.
///
/// `from` and `to` are [`usercmd_size`] bytes.
///
/// Original: `MSG_WriteDeltaUsercmdKey` at 0x00444B10 (CoD2MP_s.exe 1.3)
/// Original: `MSG_WriteDeltaUsercmdKey` at 0x0044A360 (CoDUOMP.exe 1.51)
pub fn write_delta_usercmd_key(msg: &mut Msg, key: i32, from: &[u8], to: &[u8]) {
    let time = word_at(to, 0);
    let elapsed = time.wrapping_sub(word_at(from, 0)) as u32;
    if elapsed < 0x100 {
        msg.write_bit1();
        msg.write_byte(elapsed as u8);
    } else {
        msg.write_bit0();
        msg.write_long(time);
    }
    match msg.game {
        Game::CallOfDuty2 => write_usercmd_cod2(msg, key, time, from, to),
        Game::UnitedOffensive => write_usercmd_uo(msg, key, time, from, to),
    }
}

/// Call of Duty 2's command after the time: a 32-bit button word, the
/// weapon and offhand weapon.
fn write_usercmd_cod2(msg: &mut Msg, key: i32, time: i32, from: &[u8], to: &[u8]) {
    let layout = &COD2_USERCMD;
    let (to_moves, from_moves) = (move_mask(to, layout), move_mask(from, layout));
    let buttons = |cmd: &[u8]| word_at(cmd, layout.buttons as u32);
    let angle = |cmd: &[u8], i: usize| word_at(cmd, (layout.angles + 4 * i) as u32);
    let changed_buttons = buttons(to) ^ buttons(from);
    let short_form = changed_buttons & !1 == 0
        && to[layout.weapon] == from[layout.weapon]
        && to[layout.second] == from[layout.second]
        && angle(to, 2) == angle(from, 2);
    if short_form {
        if angle(to, 0) == angle(from, 0)
            && angle(to, 1) == angle(from, 1)
            && changed_buttons & 1 == 0
            && to_moves == from_moves
        {
            msg.write_bits(key, 1);
            return;
        }
        msg.write_bits(key ^ 1, 1);
        msg.write_bits(key, 1);
        let timed = key ^ time;
        msg.write_bits(buttons(to) ^ timed, 1);
        msg.write_delta_key_short(timed, angle(from, 0), angle(to, 0));
        msg.write_delta_key_short(timed, angle(from, 1), angle(to, 1));
        msg.write_delta_key(timed, from_moves, to_moves, 4);
        return;
    }
    msg.write_bits(key ^ 1, 1);
    msg.write_bits(key ^ 1, 1);
    msg.write_bits(buttons(to) ^ key, 1);
    msg.write_delta_key_short(key, angle(from, 0), angle(to, 0));
    msg.write_delta_key_short(key, angle(from, 1), angle(to, 1));
    msg.write_delta_key(key, from_moves, to_moves, 4);
    let timed = key ^ time;
    msg.write_delta_key_short(timed, angle(from, 2), angle(to, 2));
    msg.write_delta_key(timed, buttons(from) >> 1, buttons(to) >> 1, 18);
    let byte = |cmd: &[u8], at: usize| i32::from(cmd[at]);
    msg.write_delta_key(timed, byte(from, layout.weapon), byte(to, layout.weapon), 7);
    msg.write_delta_key(timed, byte(from, layout.second), byte(to, layout.second), 7);
}

fn put_word(cmd: &mut [u8], at: usize, value: i32) {
    cmd[at..at + 4].copy_from_slice(&value.to_le_bytes());
}

/// Reads what [`write_delta_usercmd_key`] wrote into `to`, starting from a
/// copy of `from`. Angles come back as their low 16 bits (0 to 65535), the
/// movement as 127, -127 or 0. A message that runs out reads the way the
/// original does: the time as `from`'s - 1 (or -1), the flag bits as changed.
///
/// Original: `MSG_ReadDeltaUsercmdKey` at 0x00444EC0 (CoD2MP_s.exe 1.3)
/// Original: `MSG_ReadDeltaUsercmdKey` at 0x0044A770 (CoDUOMP.exe 1.51)
pub fn read_delta_usercmd_key(msg: &mut Msg, key: i32, from: &[u8], to: &mut [u8]) {
    let size = usercmd_size(msg.game);
    to[..size].copy_from_slice(&from[..size]);
    let time = if msg.read_bit() == 0 {
        msg.read_long()
    } else {
        word_at(from, 0).wrapping_add(msg.read_byte())
    };
    put_word(to, 0, time);
    if msg.read_bits(1) == key & 1 {
        return;
    }
    match msg.game {
        Game::CallOfDuty2 => read_usercmd_cod2(msg, key, time, from, to),
        Game::UnitedOffensive => read_usercmd_uo(msg, key, time, from, to),
    }
}

/// The rest of a changed Call of Duty 2 command (see [`read_delta_usercmd_key`]).
fn read_usercmd_cod2(msg: &mut Msg, key: i32, time: i32, from: &[u8], to: &mut [u8]) {
    let layout = &COD2_USERCMD;
    let buttons_at = layout.buttons;
    let angle_at = |i: usize| layout.angles + 4 * i;
    let from_moves = move_mask(from, layout);
    let set_moves = |to: &mut [u8], mask: i32| {
        to[layout.forward] = movement_from(mask, 1, 2);
        to[layout.forward + 1] = movement_from(mask, 4, 8);
    };
    let first_bit = |msg: &mut Msg, to: &mut [u8], key: i32| {
        let bit = msg.read_bits(1) ^ (key & 1);
        let buttons = word_at(to, buttons_at as u32) | bit;
        put_word(to, buttons_at, buttons);
    };
    put_word(to, buttons_at, word_at(to, buttons_at as u32) & !1);
    if msg.read_bits(1) == key & 1 {
        let timed = key ^ time;
        first_bit(msg, to, timed);
        for i in 0..2 {
            let a = msg.read_delta_key_short(timed, word_at(from, angle_at(i) as u32));
            put_word(to, angle_at(i), a & 0xFFFF);
        }
        let moves = msg.read_delta_key(timed, from_moves, 4);
        set_moves(to, moves);
        return;
    }
    first_bit(msg, to, key);
    for i in 0..2 {
        let a = msg.read_delta_key_short(key, word_at(from, angle_at(i) as u32));
        put_word(to, angle_at(i), a & 0xFFFF);
    }
    let moves = msg.read_delta_key(key, from_moves, 4);
    set_moves(to, moves);
    let timed = key ^ time;
    let a = msg.read_delta_key_short(timed, word_at(from, angle_at(2) as u32));
    put_word(to, angle_at(2), a & 0xFFFF);
    let low = word_at(to, buttons_at as u32) & 1;
    let rest = msg.read_delta_key(timed, word_at(from, buttons_at as u32) >> 1, 18);
    put_word(to, buttons_at, low | rest << 1);
    to[layout.weapon] = msg.read_delta_key(timed, i32::from(from[layout.weapon]), 7) as u8;
    to[layout.second] = msg.read_delta_key(timed, i32::from(from[layout.second]), 7) as u8;
}

/// The direction of United Offensive's up/down movement byte: 1 up, 2 down.
fn up_direction(cmd: &[u8]) -> i32 {
    move_direction(cmd[UO_USERCMD.forward + 2])
}

/// United Offensive's command after the time: a button byte, the weapon,
/// a second byte and up/down movement.
fn write_usercmd_uo(msg: &mut Msg, key: i32, time: i32, from: &[u8], to: &[u8]) {
    let layout = &UO_USERCMD;
    let (to_moves, from_moves) = (move_mask(to, layout), move_mask(from, layout));
    let (to_up, from_up) = (up_direction(to), up_direction(from));
    let buttons = |cmd: &[u8]| i32::from(cmd[layout.buttons]);
    let byte = |cmd: &[u8], at: usize| i32::from(cmd[at]);
    let angle = |cmd: &[u8], i: usize| word_at(cmd, (layout.angles + 4 * i) as u32);
    let changed_buttons = buttons(to) ^ buttons(from);
    let short_form = changed_buttons & 0xFE == 0
        && to[layout.weapon] == from[layout.weapon]
        && to[layout.second] == from[layout.second]
        && angle(to, 2) == angle(from, 2)
        && to_up == from_up;
    if short_form {
        if angle(to, 0) == angle(from, 0)
            && angle(to, 1) == angle(from, 1)
            && changed_buttons & 1 == 0
            && to_moves == from_moves
        {
            msg.write_bits(key, 1);
            return;
        }
        msg.write_bits(key ^ 1, 1);
        msg.write_bits(key, 1);
        let timed = key ^ time;
        msg.write_bits(buttons(to) ^ timed, 1);
        msg.write_delta_key_short(timed, angle(from, 0), angle(to, 0));
        msg.write_delta_key_short(timed, angle(from, 1), angle(to, 1));
        msg.write_delta_key(timed, from_moves, to_moves, 4);
        return;
    }
    msg.write_bits(key ^ 1, 1);
    msg.write_bits(key ^ 1, 1);
    msg.write_bits(buttons(to) ^ key, 1);
    msg.write_delta_key_short(key, angle(from, 0), angle(to, 0));
    msg.write_delta_key_short(key, angle(from, 1), angle(to, 1));
    msg.write_delta_key(key, from_moves, to_moves, 4);
    let timed = key ^ time;
    msg.write_delta_key_short(timed, angle(from, 2), angle(to, 2));
    msg.write_delta_key(timed, buttons(from) >> 1, buttons(to) >> 1, 6);
    msg.write_delta_key_byte(timed, byte(from, layout.weapon), byte(to, layout.weapon));
    msg.write_delta_key(timed, from_up, to_up, 2);
    msg.write_delta_key(timed, byte(from, layout.second), byte(to, layout.second), 7);
}

/// The rest of a changed United Offensive command (see [`read_delta_usercmd_key`]).
fn read_usercmd_uo(msg: &mut Msg, key: i32, time: i32, from: &[u8], to: &mut [u8]) {
    let layout = &UO_USERCMD;
    let angle_at = |i: usize| layout.angles + 4 * i;
    let from_moves = move_mask(from, layout);
    let first_bit = |msg: &mut Msg, to: &mut [u8], key: i32| {
        let bit = msg.read_bits(1) ^ (key & 1);
        to[layout.buttons] |= bit as u8;
    };
    let read_angles = |msg: &mut Msg, to: &mut [u8], key: i32, from: &[u8], count: usize| {
        for i in 0..count {
            let a = msg.read_delta_key_short(key, word_at(from, angle_at(i) as u32));
            put_word(to, angle_at(i), a & 0xFFFF);
        }
    };
    let set_moves = |to: &mut [u8], mask: i32| {
        to[layout.forward] = movement_from(mask, 1, 2);
        to[layout.forward + 1] = movement_from(mask, 4, 8);
    };
    to[layout.buttons] &= 0xFE;
    if msg.read_bits(1) == key & 1 {
        let timed = key ^ time;
        first_bit(msg, to, timed);
        read_angles(msg, to, timed, from, 2);
        let moves = msg.read_delta_key(timed, from_moves, 4);
        set_moves(to, moves);
        return;
    }
    first_bit(msg, to, key);
    read_angles(msg, to, key, from, 2);
    let moves = msg.read_delta_key(key, from_moves, 4);
    set_moves(to, moves);
    let timed = key ^ time;
    let a = msg.read_delta_key_short(timed, word_at(from, angle_at(2) as u32));
    put_word(to, angle_at(2), a & 0xFFFF);
    to[layout.buttons] &= 1;
    let rest = msg.read_delta_key(timed, i32::from(from[layout.buttons] >> 1), 6);
    to[layout.buttons] |= (rest << 1) as u8;
    to[layout.weapon] = msg.read_delta_key_byte(timed, i32::from(from[layout.weapon])) as u8;
    let up = msg.read_delta_key(timed, up_direction(from), 2);
    to[layout.forward + 2] = movement_from(up, 1, 2);
    to[layout.second] = msg.read_delta_key(timed, i32::from(from[layout.second]), 7) as u8;
}

#[cfg(test)]
mod tests {
    use super::*;

    fn usercmd(
        game: Game,
        time: i32,
        angles: [i32; 3],
        forward: u8,
        right: u8,
        weapon: u8,
    ) -> Vec<u8> {
        let mut cmd = vec![0u8; usercmd_size(game)];
        let layout = match game {
            Game::CallOfDuty2 => &COD2_USERCMD,
            Game::UnitedOffensive => &UO_USERCMD,
        };
        put_word(&mut cmd, 0, time);
        for (i, a) in angles.iter().enumerate() {
            put_word(&mut cmd, layout.angles + 4 * i, *a);
        }
        cmd[layout.forward] = forward;
        cmd[layout.forward + 1] = right;
        cmd[layout.weapon] = weapon;
        cmd
    }

    #[test]
    fn player_commands_survive_the_wire() {
        for game in [Game::CallOfDuty2, Game::UnitedOffensive] {
            let from = usercmd(game, 1000, [10, 20, 30], 0, 0, 3);
            let cases = [
                usercmd(game, 1016, [10, 20, 30], 0, 0, 3),
                usercmd(game, 1050, [11, 65535, 30], 0x7F, 0x81, 3),
                usercmd(game, 9000, [11, 21, 31], 0x81, 0, 5),
            ];
            for to in cases {
                let mut buffer = [0u8; 64];
                let mut msg = Msg::new(game, &mut buffer);
                write_delta_usercmd_key(&mut msg, 0x1234_5678, &from, &to);
                msg.begin_reading();
                let mut back = vec![0xEEu8; usercmd_size(game)];
                read_delta_usercmd_key(&mut msg, 0x1234_5678, &from, &mut back);
                assert_eq!(back, to, "{game:?}");
                assert!(!msg.overflowed);
            }
        }
    }

    /// A made-up table (the games' tables are game data): one field of each
    /// kind, 4 bytes apart after the number.
    fn table(kinds: &[i32]) -> FieldTable {
        let mut bytes = Vec::new();
        for (i, &bits) in kinds.iter().enumerate() {
            bytes.extend_from_slice(&0u32.to_le_bytes());
            bytes.extend_from_slice(&(4 + 4 * i as u32).to_le_bytes());
            bytes.extend_from_slice(&bits.to_le_bytes());
        }
        FieldTable::from_bytes(&bytes)
    }

    fn state(words: &[i32]) -> Vec<u8> {
        words.iter().flat_map(|w| w.to_le_bytes()).collect()
    }

    fn float(x: f32) -> i32 {
        x.to_bits() as i32
    }

    #[test]
    fn structures_survive_the_wire() {
        // float, angle, 8-bit, 32-bit, 10-bit fields.
        let fields = table(&[0, ANGLE_FIELD, 8, 32, 10]);
        let from = state(&[7, float(1.5), float(90.0), 3, -5, 1000]);
        let cases = [
            state(&[7, float(1.5), float(90.0), 3, -5, 1000]), // unchanged
            state(&[7, float(-4096.0), float(0.0), 255, 0, 1023]), // whole float, zeros
            state(&[7, float(0.25), float(-45.0), 0, i32::MIN, 1]), // raw float
            state(&[7, float(4095.0), float(90.0), 3, -5, 999]), // the last field only
        ];
        for game in [Game::CallOfDuty2, Game::UnitedOffensive] {
            for to in &cases {
                let mut buffer = [0u8; 128];
                let mut msg = Msg::new(game, &mut buffer);
                let coding = StructCoding {
                    fields: fields.fields(),
                    index_bits: ENTITY_NUMBER_BITS,
                    change_bit: false,
                    table_flag: None,
                };
                write_delta_struct(&mut msg, &from, Some(to), true, coding);
                msg.begin_reading();
                let number = msg.read_bits(ENTITY_NUMBER_BITS);
                let mut back = vec![0xEE; to.len()];
                assert!(!read_delta_struct(
                    &mut msg, &from, &mut back, number, &fields
                ));
                assert_eq!(&back, to, "{game:?}");
                assert_eq!(msg.readcount, msg.cursize);
            }
            // Removed: the reader says so and leaves `to` alone.
            let mut buffer = [0u8; 16];
            let mut msg = Msg::new(game, &mut buffer);
            let coding = StructCoding {
                fields: fields.fields(),
                index_bits: ENTITY_NUMBER_BITS,
                change_bit: false,
                table_flag: None,
            };
            write_delta_struct(&mut msg, &from, None, true, coding);
            msg.begin_reading();
            assert_eq!(msg.read_bits(ENTITY_NUMBER_BITS), 7);
            let mut back = vec![0xEE; from.len()];
            assert!(read_delta_struct(&mut msg, &from, &mut back, 7, &fields));
            assert!(back.iter().all(|&b| b == 0xEE));
        }
    }

    #[test]
    fn only_call_of_duty_2_extends_the_sign_of_negative_fields() {
        let field = NetField {
            offset: 0,
            bits: -8,
        };
        let from = [0u8; 4];
        let to = (-3i32).to_le_bytes();
        for (game, expected) in [(Game::CallOfDuty2, -3), (Game::UnitedOffensive, 0xFD)] {
            let mut buffer = [0u8; 16];
            let mut msg = Msg::new(game, &mut buffer);
            write_delta_field(&mut msg, &from, &to, field);
            msg.begin_reading();
            let mut back = [0u8; 4];
            read_delta_field(&mut msg, &from, &mut back, field);
            assert_eq!(i32::from_le_bytes(back), expected, "{game:?}");
        }
    }

    #[test]
    fn a_missing_field_count_copies_from_the_entry_in_front_of_the_table() {
        let mut fields = table(&[8, 8]);
        fields.preceding_offset = Some(0);
        let from = state(&[5, 1, 2]);
        for game in [Game::CallOfDuty2, Game::UnitedOffensive] {
            // "not removed", "changed", then the message ends.
            let mut buffer = [0b10u8, 0, 0, 0];
            let mut msg = Msg::new(game, &mut buffer);
            msg.cursize = 1;
            let mut back = vec![0xEE; from.len()];
            assert!(!read_delta_struct(&mut msg, &from, &mut back, 9, &fields));
            // The number set to 9, then overwritten by "field -1" (offset 0).
            assert_eq!(back, from, "{game:?}");
            assert_eq!(msg.overflowed, game == Game::CallOfDuty2);
        }
    }

    #[test]
    fn a_changed_integer_field_is_flagged_then_sent_low_bits_first() {
        let field = NetField {
            offset: 4,
            bits: 10,
        };
        let from = [0u8; 8];
        let mut to = [0u8; 8];
        to[4..8].copy_from_slice(&0x2AB_i32.to_le_bytes());
        let mut buffer = [0u8; 16];
        let mut msg = Msg::new(Game::CallOfDuty2, &mut buffer);
        write_delta_field(&mut msg, &from, &to, field);
        // changed, not zero, low 2 bits (0b11) in the bit byte, then one raw byte (0xAA).
        assert_eq!(&msg.data[..msg.cursize], &[0b1111, 0xAA]);
    }
}
