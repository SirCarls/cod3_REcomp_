#include "iw_pc/net/delta.h"

#include <algorithm>
#include <cstring>

#include "iw_pc/net/msg.h"

namespace iw_pc::net {

namespace {

constexpr std::size_t kMaxFieldName = 32;
constexpr std::size_t kClientStateSize = 0x5C;

std::int32_t wrap_add(std::int32_t a, std::int32_t b) {
    return static_cast<std::int32_t>(static_cast<std::uint32_t>(a) + static_cast<std::uint32_t>(b));
}
std::int32_t wrap_sub(std::int32_t a, std::int32_t b) {
    return static_cast<std::int32_t>(static_cast<std::uint32_t>(a) - static_cast<std::uint32_t>(b));
}
// x86's shl: the count taken modulo 32.
std::int32_t shl(std::int32_t value, std::uint32_t count) {
    return static_cast<std::int32_t>(static_cast<std::uint32_t>(value) << (count & 31));
}
float float_of(std::int32_t bits) {
    float f;
    std::memcpy(&f, &bits, 4);
    return f;
}
std::int32_t bits_of(float f) {
    std::int32_t bits;
    std::memcpy(&bits, &f, 4);
    return bits;
}

std::int32_t number_of(ByteView state) { return word_at(state, 0); }

void copy_word(ByteView from, ByteSpan to, std::uint32_t offset) { set_word(to, offset, word_at(from, offset)); }

// A float field sent as a whole number when it is one and fits: `bias`
// (4096 or 512) added, `low_bits` (5 or 2) in the bit stream and the rest
// as a raw byte; else a 1 bit and the raw 32 bits. With the "is zero" bit
// first unless `player` (the player state's coder has none).
void write_float(Msg& msg, std::int32_t raw, std::int32_t bias, std::uint32_t low_bits, bool player) {
    float value = float_of(raw);
    // __ftol2 gives 64 bits; the code keeps the low 32.
    auto truncated = static_cast<std::int32_t>(static_cast<std::uint32_t>(static_cast<std::uint64_t>(ftol2(value))));
    if (!player) {
        if (value == 0.0f) {
            msg.write_bit0();
            return;
        }
        msg.write_bit1();
    }
    std::int32_t biased = wrap_add(truncated, bias);
    if (static_cast<double>(truncated) == static_cast<double>(value) && biased >= 0 && biased < bias * 2) {
        msg.write_bit0();
        msg.write_bits(biased, low_bits);
        msg.write_byte(static_cast<std::uint8_t>(biased >> low_bits));
    } else {
        msg.write_bit1();
        msg.write_long(raw);
    }
}

void write_integer(Msg& msg, std::int32_t value, std::int32_t bits) {
    std::uint32_t left = bits < 0 ? 0u - static_cast<std::uint32_t>(bits) : static_cast<std::uint32_t>(bits);
    std::uint32_t low = left & 7;
    if (low != 0) {
        msg.write_bits(value, low);
        value >>= low;
        left -= low;
    }
    while (left != 0) {
        msg.write_byte(static_cast<std::uint8_t>(value));
        value >>= 8;
        left -= 8;
    }
}

// A float sent by write_float() after its "is zero" bit.
std::int32_t read_float(Msg& msg, std::int32_t bias, std::uint32_t low_bits) {
    if (msg.read_bit() != 0) return msg.read_long();
    std::int32_t low = msg.read_bits(low_bits);
    std::int32_t high = msg.read_byte();
    std::int32_t whole = wrap_sub(wrap_add(low, shl(high, low_bits)), bias);
    return bits_of(static_cast<float>(whole));  // exact: within 24 bits
}

// An integer of `bits` bits: the low |bits| % 8 bits, then raw bytes, each
// shifted by its place modulo 32. Call of Duty 2 extends the sign of a
// field with a negative bit count; United Offensive doesn't.
std::int32_t read_integer(Msg& msg, std::int32_t bits) {
    std::int32_t width = bits < 0 ? wrap_sub(0, bits) : bits;
    std::int32_t low = width & 7;
    std::int32_t value = low != 0 ? msg.read_bits(static_cast<std::uint32_t>(low)) : 0;
    for (std::int32_t shift = low; shift < width; shift += 8) {
        value |= shl(msg.read_byte(), static_cast<std::uint32_t>(shift));
    }
    if (bits < 0 && msg.game == Game::CallOfDuty2 && (value & shl(1, static_cast<std::uint32_t>(width - 1))) != 0) {
        value |= ~wrap_sub(shl(1, static_cast<std::uint32_t>(width)), 1);
    }
    return value;
}

// Where the parts of a player state are, in one game.
struct PlayerStateLayout {
    std::size_t size, stats, ammo, clip, objectives, hud, hud_count;
    std::uint32_t hud_count_bits;
    std::size_t hud_elem_size;
};

constexpr PlayerStateLayout kCod2PlayerState{0x26A8, 0x12C, 0x144, 0x344, 0x5E4, 0x7A8, 31, 5, 0x80};
constexpr PlayerStateLayout kUoPlayerState{0x4504, 0x11C, 0x134, 0x334, 0x638, 0x7F8, 63, 6, 0x7C};
constexpr std::size_t kObjectives = 16;
constexpr std::size_t kObjectiveSize = 28;
constexpr std::size_t kAmmoGroups = 4;

const PlayerStateLayout& player_state_layout(Game game) {
    return game == Game::CallOfDuty2 ? kCod2PlayerState : kUoPlayerState;
}

const std::uint8_t* zero_player_state() {
    static const std::vector<std::uint8_t> zeros(0x4504, 0);
    return zeros.data();
}

// One field of the player state: no "is zero" bit for floats and integers.
void write_player_field(Msg& msg, ByteView from, ByteView to, NetField field) {
    std::int32_t old_value = word_at(from, field.offset), new_value = word_at(to, field.offset);
    if (old_value == new_value) {
        msg.write_bit0();
        return;
    }
    msg.write_bit1();
    if (field.bits == 0) {
        write_float(msg, new_value, 4096, 5, true);
    } else if (field.bits == kAngleField) {
        if (new_value == 0) {
            msg.write_bit0();
        } else {
            msg.write_bit1();
            msg.write_angle16(float_of(new_value));
        }
    } else {
        write_integer(msg, new_value, field.bits);
    }
}

void read_player_field(Msg& msg, ByteView from, ByteSpan to, NetField field) {
    if (msg.read_bit() == 0) {
        copy_word(from, to, field.offset);
        return;
    }
    std::int32_t value;
    if (field.bits == 0) {
        value = read_float(msg, 4096, 5);
    } else if (field.bits == kAngleField) {
        value = msg.read_bit() == 0 ? 0 : bits_of(msg.read_angle16());
    } else {
        value = read_integer(msg, field.bits);
    }
    set_word(to, field.offset, value);
}

// A 16-bit mask of which of 16 numbers (at `at`, 4 bytes apart) differ.
std::uint32_t changed_mask(ByteView from, ByteView to, std::size_t at) {
    std::uint32_t mask = 0;
    for (std::uint32_t j = 0; j < 16; ++j) {
        auto offset = static_cast<std::uint32_t>(at + 4 * j);
        if (word_at(from, offset) != word_at(to, offset)) mask |= 1u << j;
    }
    return mask;
}

void write_short_group(Msg& msg, ByteView to, std::size_t at, std::uint32_t mask) {
    msg.write_short(static_cast<std::int16_t>(mask));
    for (std::uint32_t j = 0; j < 16; ++j) {
        if (mask & (1u << j)) msg.write_short(static_cast<std::int16_t>(word_at(to, static_cast<std::uint32_t>(at + 4 * j))));
    }
}

void read_short_group(Msg& msg, ByteSpan to, std::size_t at) {
    std::int32_t mask = msg.read_short();
    for (std::uint32_t j = 0; j < 16; ++j) {
        if (mask & (1 << j)) {
            std::int32_t value = msg.read_short();
            set_word(to, static_cast<std::uint32_t>(at + 4 * j), value);
        }
    }
}

// Original: MSG_WriteDeltaHudElems at 0x00445C60 (CoD2MP_s.exe 1.3)
// Original: MSG_WriteDeltaHudElems at 0x0044B510 (CoDUOMP.exe 1.51)
void write_hud_elems(Msg& msg, std::span<const NetField> fields, const PlayerStateLayout& layout, ByteView from, ByteView to,
                     std::size_t count) {
    std::size_t size = layout.hud_elem_size;
    std::size_t in_use = 0;
    while (in_use < count && word_at(to, static_cast<std::uint32_t>(in_use * size)) != 0) ++in_use;
    msg.write_bits(static_cast<std::int32_t>(in_use), layout.hud_count_bits);
    for (std::size_t i = 0; i < in_use; ++i) {
        ByteView old_elem = from.subspan(i * size, size), new_elem = to.subspan(i * size, size);
        std::size_t last = 0;
        for (std::size_t k = fields.size(); k > 0; --k) {
            if (word_at(old_elem, fields[k - 1].offset) != word_at(new_elem, fields[k - 1].offset)) {
                last = k - 1;
                break;
            }
        }
        msg.write_bits(static_cast<std::int32_t>(last), 5);
        std::size_t through = fields.empty() ? 0 : std::min(last, fields.size() - 1) + 1;
        for (std::size_t k = 0; k < through; ++k) write_delta_field(msg, old_elem, new_elem, fields[k]);
    }
}

// Original: MSG_ReadDeltaFields at 0x00445990 (CoD2MP_s.exe 1.3; UO has it inline)
void read_delta_fields(Msg& msg, std::span<const NetField> fields, ByteView from, ByteSpan to) {
    if (msg.read_bit() != 0) {
        for (const NetField& f : fields) read_delta_field(msg, from, to, f);
    } else {
        for (const NetField& f : fields) copy_word(from, to, f.offset);
    }
}

// One array of HUD elements at `at` in the player state. United Offensive's
// table has 30 fields but the index has 5 bits: an index of 30 or 31 (never
// sent by its writer) has the original read entries past its table; the
// port reads the table and marks the message overflowed.
// Original: MSG_ReadDeltaHudElems at 0x00445DB0 (CoD2MP_s.exe 1.3)
// Original: MSG_ReadDeltaHudElems at 0x0044B690 (CoDUOMP.exe 1.51)
void read_hud_elems(Msg& msg, std::span<const NetField> fields, const PlayerStateLayout& layout, ByteView from, ByteSpan to,
                    std::size_t at, std::size_t hud_count) {
    std::size_t size = layout.hud_elem_size;
    std::int32_t count = msg.read_bits(layout.hud_count_bits);
    std::size_t sent = std::min(static_cast<std::size_t>(std::max(count, 0)), hud_count);
    for (std::size_t i = 0; i < sent; ++i) {
        std::size_t elem = at + i * size;
        ByteView old_elem = from.subspan(elem, size);
        ByteSpan new_elem = to.subspan(elem, size);
        std::int32_t last = msg.read_bits(5);
        std::size_t read = 0;
        if (last >= 0) {
            read = static_cast<std::size_t>(last) + 1;
            if (read > fields.size()) {
                msg.overflowed = true;
                read = fields.size();
            }
            for (std::size_t k = 0; k < read; ++k) read_delta_field(msg, old_elem, new_elem, fields[k]);
        }
        for (std::size_t k = read; k < fields.size(); ++k) copy_word(old_elem, new_elem, fields[k].offset);
    }
    std::int64_t start = static_cast<std::int64_t>(at) +
                         static_cast<std::int64_t>(std::min<std::int32_t>(count, static_cast<std::int32_t>(hud_count))) *
                             static_cast<std::int64_t>(size);
    std::size_t end = at + hud_count * size;
    std::size_t from_byte = start < 0 ? 0 : static_cast<std::size_t>(start);
    if (from_byte < end) std::fill(to.begin() + static_cast<std::ptrdiff_t>(from_byte), to.begin() + static_cast<std::ptrdiff_t>(end), std::uint8_t{0});
}

// --- player commands ---------------------------------------------------------

struct UsercmdLayout {
    std::size_t buttons;  // the 32-bit button word (CoD2) or the button byte (UO)
    std::size_t weapon;
    std::size_t second;   // CoD2: the offhand weapon byte; UO: a byte at +6
    std::size_t angles;   // three 16-bit view angles (kept in 32-bit words)
    std::size_t forward;  // forward and right movement (signed bytes), and UO's up
};

constexpr UsercmdLayout kCod2Usercmd{4, 8, 9, 0xC, 0x18};
constexpr UsercmdLayout kUoUsercmd{4, 5, 6, 8, 0x14};

// The direction of a signed movement byte: 1 past 10, 2 below -10, else 0.
std::int32_t move_direction(std::uint8_t movement) {
    auto m = static_cast<std::int8_t>(movement);
    return m >= 11 ? 1 : m <= -11 ? 2 : 0;
}

// The four direction bits: 1 forward, 2 back, 4 right, 8 left.
std::int32_t move_mask(ByteView cmd, const UsercmdLayout& layout) {
    return move_direction(cmd[layout.forward]) | move_direction(cmd[layout.forward + 1]) << 2;
}

// Movement bytes back from direction bits.
std::uint8_t movement_from(std::int32_t mask, std::int32_t plus, std::int32_t minus) {
    if (mask & plus) return 0x7F;
    if (mask & minus) return 0x81;
    return 0;
}

std::int32_t up_direction(ByteView cmd) { return move_direction(cmd[kUoUsercmd.forward + 2]); }

void put_word(ByteSpan cmd, std::size_t at, std::int32_t value) {
    auto v = static_cast<std::uint32_t>(value);
    for (int j = 0; j < 4; ++j) cmd[at + static_cast<std::size_t>(j)] = static_cast<std::uint8_t>(v >> (8 * j));
}

std::int32_t angle_of(ByteView cmd, const UsercmdLayout& layout, std::size_t i) {
    return word_at(cmd, static_cast<std::uint32_t>(layout.angles + 4 * i));
}

void write_usercmd_cod2(Msg& msg, std::int32_t key, std::int32_t time, ByteView from, ByteView to) {
    const UsercmdLayout& layout = kCod2Usercmd;
    std::int32_t to_moves = move_mask(to, layout), from_moves = move_mask(from, layout);
    auto buttons = [&](ByteView cmd) { return word_at(cmd, static_cast<std::uint32_t>(layout.buttons)); };
    auto angle = [&](ByteView cmd, std::size_t i) { return angle_of(cmd, layout, i); };
    std::int32_t changed_buttons = buttons(to) ^ buttons(from);
    bool short_form = (changed_buttons & ~1) == 0 && to[layout.weapon] == from[layout.weapon] &&
                      to[layout.second] == from[layout.second] && angle(to, 2) == angle(from, 2);
    if (short_form) {
        if (angle(to, 0) == angle(from, 0) && angle(to, 1) == angle(from, 1) && (changed_buttons & 1) == 0 &&
            to_moves == from_moves) {
            msg.write_bits(key, 1);
            return;
        }
        msg.write_bits(key ^ 1, 1);
        msg.write_bits(key, 1);
        std::int32_t timed = key ^ time;
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
    std::int32_t timed = key ^ time;
    msg.write_delta_key_short(timed, angle(from, 2), angle(to, 2));
    msg.write_delta_key(timed, buttons(from) >> 1, buttons(to) >> 1, 18);
    msg.write_delta_key(timed, from[layout.weapon], to[layout.weapon], 7);
    msg.write_delta_key(timed, from[layout.second], to[layout.second], 7);
}

void write_usercmd_uo(Msg& msg, std::int32_t key, std::int32_t time, ByteView from, ByteView to) {
    const UsercmdLayout& layout = kUoUsercmd;
    std::int32_t to_moves = move_mask(to, layout), from_moves = move_mask(from, layout);
    std::int32_t to_up = up_direction(to), from_up = up_direction(from);
    auto buttons = [&](ByteView cmd) { return static_cast<std::int32_t>(cmd[layout.buttons]); };
    auto angle = [&](ByteView cmd, std::size_t i) { return angle_of(cmd, layout, i); };
    std::int32_t changed_buttons = buttons(to) ^ buttons(from);
    bool short_form = (changed_buttons & 0xFE) == 0 && to[layout.weapon] == from[layout.weapon] &&
                      to[layout.second] == from[layout.second] && angle(to, 2) == angle(from, 2) && to_up == from_up;
    if (short_form) {
        if (angle(to, 0) == angle(from, 0) && angle(to, 1) == angle(from, 1) && (changed_buttons & 1) == 0 &&
            to_moves == from_moves) {
            msg.write_bits(key, 1);
            return;
        }
        msg.write_bits(key ^ 1, 1);
        msg.write_bits(key, 1);
        std::int32_t timed = key ^ time;
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
    std::int32_t timed = key ^ time;
    msg.write_delta_key_short(timed, angle(from, 2), angle(to, 2));
    msg.write_delta_key(timed, buttons(from) >> 1, buttons(to) >> 1, 6);
    msg.write_delta_key_byte(timed, from[layout.weapon], to[layout.weapon]);
    msg.write_delta_key(timed, from_up, to_up, 2);
    msg.write_delta_key(timed, from[layout.second], to[layout.second], 7);
}

void read_usercmd_cod2(Msg& msg, std::int32_t key, std::int32_t time, ByteView from, ByteSpan to) {
    const UsercmdLayout& layout = kCod2Usercmd;
    std::size_t buttons_at = layout.buttons;
    std::int32_t from_moves = move_mask(from, layout);
    auto set_moves = [&](std::int32_t mask) {
        to[layout.forward] = movement_from(mask, 1, 2);
        to[layout.forward + 1] = movement_from(mask, 4, 8);
    };
    auto first_bit = [&](std::int32_t k) {
        std::int32_t bit = msg.read_bits(1) ^ (k & 1);
        put_word(to, buttons_at, word_at(to, static_cast<std::uint32_t>(buttons_at)) | bit);
    };
    auto read_angle = [&](std::int32_t k, std::size_t i) {
        std::int32_t a = msg.read_delta_key_short(k, angle_of(from, layout, i));
        put_word(to, layout.angles + 4 * i, a & 0xFFFF);
    };
    put_word(to, buttons_at, word_at(to, static_cast<std::uint32_t>(buttons_at)) & ~1);
    if (msg.read_bits(1) == (key & 1)) {
        std::int32_t timed = key ^ time;
        first_bit(timed);
        read_angle(timed, 0);
        read_angle(timed, 1);
        set_moves(msg.read_delta_key(timed, from_moves, 4));
        return;
    }
    first_bit(key);
    read_angle(key, 0);
    read_angle(key, 1);
    set_moves(msg.read_delta_key(key, from_moves, 4));
    std::int32_t timed = key ^ time;
    read_angle(timed, 2);
    std::int32_t low = word_at(to, static_cast<std::uint32_t>(buttons_at)) & 1;
    std::int32_t rest = msg.read_delta_key(timed, word_at(from, static_cast<std::uint32_t>(buttons_at)) >> 1, 18);
    put_word(to, buttons_at, low | shl(rest, 1));
    to[layout.weapon] = static_cast<std::uint8_t>(msg.read_delta_key(timed, from[layout.weapon], 7));
    to[layout.second] = static_cast<std::uint8_t>(msg.read_delta_key(timed, from[layout.second], 7));
}

void read_usercmd_uo(Msg& msg, std::int32_t key, std::int32_t time, ByteView from, ByteSpan to) {
    const UsercmdLayout& layout = kUoUsercmd;
    std::int32_t from_moves = move_mask(from, layout);
    auto first_bit = [&](std::int32_t k) {
        std::int32_t bit = msg.read_bits(1) ^ (k & 1);
        to[layout.buttons] = static_cast<std::uint8_t>(to[layout.buttons] | static_cast<std::uint8_t>(bit));
    };
    auto read_angles = [&](std::int32_t k, std::size_t count) {
        for (std::size_t i = 0; i < count; ++i) {
            std::int32_t a = msg.read_delta_key_short(k, angle_of(from, layout, i));
            put_word(to, layout.angles + 4 * i, a & 0xFFFF);
        }
    };
    auto set_moves = [&](std::int32_t mask) {
        to[layout.forward] = movement_from(mask, 1, 2);
        to[layout.forward + 1] = movement_from(mask, 4, 8);
    };
    to[layout.buttons] &= 0xFE;
    if (msg.read_bits(1) == (key & 1)) {
        std::int32_t timed = key ^ time;
        first_bit(timed);
        read_angles(timed, 2);
        set_moves(msg.read_delta_key(timed, from_moves, 4));
        return;
    }
    first_bit(key);
    read_angles(key, 2);
    set_moves(msg.read_delta_key(key, from_moves, 4));
    std::int32_t timed = key ^ time;
    std::int32_t a = msg.read_delta_key_short(timed, angle_of(from, layout, 2));
    put_word(to, layout.angles + 8, a & 0xFFFF);
    to[layout.buttons] &= 1;
    std::int32_t rest = msg.read_delta_key(timed, from[layout.buttons] >> 1, 6);
    to[layout.buttons] = static_cast<std::uint8_t>(to[layout.buttons] | static_cast<std::uint8_t>(shl(rest, 1)));
    to[layout.weapon] = static_cast<std::uint8_t>(msg.read_delta_key_byte(timed, from[layout.weapon]));
    std::int32_t up = msg.read_delta_key(timed, up_direction(from), 2);
    to[layout.forward + 2] = movement_from(up, 1, 2);
    to[layout.second] = static_cast<std::uint8_t>(msg.read_delta_key(timed, from[layout.second], 7));
}

}  // namespace

// --- tables --------------------------------------------------------------------

FieldTable FieldTable::from_bytes(ByteView bytes) {
    FieldTable table;
    for (std::size_t at = 0; at + 12 <= bytes.size(); at += 12) {
        NetField f;
        f.offset = static_cast<std::uint32_t>(word_at(bytes, static_cast<std::uint32_t>(at + 4)));
        f.bits = word_at(bytes, static_cast<std::uint32_t>(at + 8));
        table.fields_.push_back(f);
        table.names_.emplace_back();
    }
    return table;
}

FieldTable FieldTable::from_executable(const GameExecutable& exe, ByteView bytes) {
    FieldTable table = from_bytes(bytes);
    for (std::size_t i = 0; i < table.fields_.size(); ++i) {
        auto pointer = static_cast<std::uint32_t>(word_at(bytes, static_cast<std::uint32_t>(12 * i)));
        auto text = exe.read(pointer, kMaxFieldName);
        if (auto* view = std::get_if<ByteView>(&text)) {
            std::size_t end = 0;
            while (end < view->size() && (*view)[end] != 0) ++end;
            table.names_[i].assign(view->begin(), view->begin() + static_cast<std::ptrdiff_t>(end));
        }
    }
    return table;
}

std::optional<NetField> FieldTable::field(const std::string& name) const {
    for (std::size_t i = 0; i < fields_.size(); ++i) {
        if (names_[i] == name) return fields_[i];
    }
    return std::nullopt;
}

std::size_t FieldTable::struct_size() const {
    std::size_t size = 0;
    for (const NetField& f : fields_) size = std::max<std::size_t>(size, f.offset + 4);
    return size;
}

std::variant<FieldTables, ProtocolDataError> FieldTables::from_executable(const GameExecutable& exe) {
    Game game = exe.game();
    std::optional<ProtocolDataError> failure;
    auto table = [&](ProtocolTable which) -> FieldTable {
        auto bytes = exe.table(which);
        if (auto* error = std::get_if<ProtocolDataError>(&bytes)) {
            if (!failure) failure = *error;
            return {};
        }
        FieldTable t = FieldTable::from_executable(exe, std::get<ByteView>(bytes));
        if (auto where = location(which, game)) {
            auto before = exe.read(where->address - 8, 4);
            if (auto* view = std::get_if<ByteView>(&before)) {
                t.set_preceding_offset(static_cast<std::uint32_t>(word_at(*view, 0)));
            }
        }
        return t;
    };
    FieldTables tables{game,
                       table(ProtocolTable::EntityStateFields),
                       std::nullopt,
                       table(ProtocolTable::PlayerStateFields),
                       table(ProtocolTable::ClientStateFields),
                       table(ProtocolTable::ArchivedEntityFields),
                       table(ProtocolTable::HudElemFields),
                       table(ProtocolTable::ObjectiveFields)};
    if (game == Game::UnitedOffensive) tables.entity_alternate = table(ProtocolTable::EntityStateFieldsAlternate);
    if (failure) return *failure;
    return tables;
}

std::int32_t word_at(ByteView bytes, std::uint32_t offset) {
    std::size_t at = offset;
    if (at + 4 > bytes.size() || at + 4 < at) return 0;
    return static_cast<std::int32_t>(static_cast<std::uint32_t>(bytes[at]) | static_cast<std::uint32_t>(bytes[at + 1]) << 8 |
                                     static_cast<std::uint32_t>(bytes[at + 2]) << 16 |
                                     static_cast<std::uint32_t>(bytes[at + 3]) << 24);
}

void set_word(ByteSpan bytes, std::uint32_t offset, std::int32_t value) {
    std::size_t at = offset;
    if (at + 4 > bytes.size() || at + 4 < at) return;
    auto v = static_cast<std::uint32_t>(value);
    for (int j = 0; j < 4; ++j) bytes[at + static_cast<std::size_t>(j)] = static_cast<std::uint8_t>(v >> (8 * j));
}

// --- fields and structures ---------------------------------------------------------

void write_delta_field(Msg& msg, ByteView from, ByteView to, NetField field) {
    std::int32_t old_value = word_at(from, field.offset), new_value = word_at(to, field.offset);
    if (old_value == new_value) {
        msg.write_bit0();
        return;
    }
    msg.write_bit1();
    if (field.bits == 0) {
        write_float(msg, new_value, 4096, 5, false);
    } else if (field.bits == kHudPositionField && msg.game == Game::CallOfDuty2) {
        write_float(msg, new_value, 512, 2, false);
    } else if (field.bits == kAngleField) {
        if (new_value == 0) {
            msg.write_bit0();
        } else {
            msg.write_bit1();
            msg.write_angle16(float_of(new_value));
        }
    } else {
        if (new_value == 0) {
            msg.write_bit0();
            return;
        }
        msg.write_bit1();
        write_integer(msg, new_value, field.bits);
    }
}

void read_delta_field(Msg& msg, ByteView from, ByteSpan to, NetField field) {
    if (msg.read_bit() == 0) {
        copy_word(from, to, field.offset);
        return;
    }
    std::int32_t value = 0;
    if (msg.read_bit() != 0) {
        if (field.bits == 0) value = read_float(msg, 4096, 5);
        else if (field.bits == kHudPositionField && msg.game == Game::CallOfDuty2) value = read_float(msg, 512, 2);
        else if (field.bits == kAngleField) value = bits_of(msg.read_angle16());
        else value = read_integer(msg, field.bits);
    }
    set_word(to, field.offset, value);
}

void write_entity_table_flag(Msg& msg, const ByteView* state) {
    if (state && word_at(*state, 4) == kAlternateEntityType) msg.write_bit1();
    else msg.write_bit0();
}

void write_delta_struct(Msg& msg, ByteView from, const ByteView* to, bool force, const StructCoding& coding) {
    if (!to) {
        if (coding.change_bit) msg.write_bit1();
        msg.write_bits(number_of(from), coding.index_bits);
        if (coding.table_flag) coding.table_flag(msg, nullptr);
        msg.write_bit1();
        return;
    }
    std::size_t last_changed = 0;
    for (std::size_t k = coding.fields.size(); k > 0; --k) {
        if (word_at(from, coding.fields[k - 1].offset) != word_at(*to, coding.fields[k - 1].offset)) {
            last_changed = k;
            break;
        }
    }
    if (last_changed == 0 && !force) return;
    if (coding.change_bit) msg.write_bit1();
    msg.write_bits(number_of(*to), coding.index_bits);
    if (coding.table_flag) coding.table_flag(msg, to);
    msg.write_bit0();
    if (last_changed == 0) {
        msg.write_bit0();
        return;
    }
    msg.write_bit1();
    msg.write_byte(static_cast<std::uint8_t>(last_changed));
    for (std::size_t k = 0; k < last_changed; ++k) write_delta_field(msg, from, *to, coding.fields[k]);
}

void write_delta_entity(Msg& msg, const FieldTables& tables, ByteView from, const ByteView* to, bool force) {
    const FieldTable* table = &tables.entity;
    TableFlag flag = nullptr;
    if (tables.game == Game::UnitedOffensive) {
        if (to && word_at(*to, 4) == kAlternateEntityType && tables.entity_alternate) table = &*tables.entity_alternate;
        flag = write_entity_table_flag;
    }
    write_delta_struct(msg, from, to, force, {table->fields(), kEntityNumberBits, false, flag});
}

void write_delta_client(Msg& msg, const FieldTables& tables, const ByteView* from, const ByteView* to, bool force) {
    static const std::array<std::uint8_t, kClientStateSize> zeros{};
    write_delta_struct(msg, from ? *from : ByteView(zeros), to, force, {tables.client.fields(), kClientNumberBits, true, nullptr});
}

bool read_delta_struct(Msg& msg, ByteView from, ByteSpan to, std::int32_t number, const FieldTable& table) {
    // A message that ran out (Call of Duty 2: -1) is not a removal...
    if (msg.read_bit() == 1) return true;
    const auto& fields = table.fields();
    // ... but is a change.
    if (msg.read_bit() == 0) {
        std::size_t n = std::min({4 * (fields.size() + 1), from.size(), to.size()});
        std::copy(from.begin(), from.begin() + static_cast<std::ptrdiff_t>(n), to.begin());
        return false;
    }
    std::int32_t count = msg.read_byte();
    if (count > static_cast<std::int32_t>(fields.size())) {
        if (msg.game == Game::CallOfDuty2) {
            msg.overflowed = true;
            return false;
        }
        count = static_cast<std::int32_t>(fields.size());
    }
    set_word(to, 0, number);
    auto read = static_cast<std::size_t>(std::max(count, 0));
    for (std::size_t k = 0; k < read; ++k) read_delta_field(msg, from, to, fields[k]);
    if (count < 0 && table.preceding_offset()) copy_word(from, to, *table.preceding_offset());
    for (std::size_t k = read; k < fields.size(); ++k) copy_word(from, to, fields[k].offset);
    return false;
}

void set_default_usercmd(Game game, ByteView ps, ByteSpan cmd) {
    std::size_t size = usercmd_size(game);
    std::fill(cmd.begin(), cmd.begin() + static_cast<std::ptrdiff_t>(size), std::uint8_t{0});
    auto float_at = [&](std::uint32_t offset) {
        auto bits = static_cast<std::uint32_t>(word_at(ps, offset));
        float f;
        std::memcpy(&f, &bits, 4);
        return f;
    };
    bool cod2 = game == Game::CallOfDuty2;
    std::uint32_t delta_angles = cod2 ? 0x54 : 0x4C, angles = cod2 ? 0xC : 8;
    if (cod2) {
        cmd[8] = ps[0xD4];
        cmd[9] = ps[0xD0];
    } else {
        cmd[6] = ps[0xD8];
    }
    for (std::uint32_t i = 0; i < 2; ++i) {
        float view = float_at(0xE8 + 4 * i);
        // view * -65536/360 at the game's x87 precision (msg.cpp's angle rule), truncated.
        double product = cod2 ? static_cast<double>(view * -(65536.0f / 360.0f))
                              : static_cast<double>(view) * static_cast<double>(-(65536.0f / 360.0f));
        auto steps = static_cast<std::int32_t>(static_cast<std::uint64_t>(ftol2(product)));
        auto delta = static_cast<std::uint32_t>(word_at(ps, delta_angles + 4 * i));
        set_word(cmd, angles + 4 * i, static_cast<std::int32_t>((0u - delta - static_cast<std::uint32_t>(steps)) & 0xFFFF));
    }
    std::int32_t pm_flags = word_at(ps, 0xC);
    if (cod2) {
        if ((pm_flags & 0x800000) == 0) return;
        std::int32_t buttons = word_at(cmd, 4);
        std::int32_t lean = word_at(ps, 0xA0);
        if (lean & 8) buttons |= 0x100;
        else if (lean & 4) buttons |= 0x200;
        float f = float_at(0x4C);
        if (f > 0.0f) buttons |= 0x80;
        else if (f < 0.0f) buttons |= 0x40;
        if (float_at(0xDC) != 0.0f) buttons |= 0x1000;
        set_word(cmd, 4, buttons);
        return;
    }
    if ((pm_flags & 0x80000) == 0) return;
    std::int32_t lean = word_at(ps, 0x84);
    if (lean & 0x40) {
        cmd[5] |= 0x40;
        cmd[0x16] = 0x81;
    } else if (lean & 0x20) {
        cmd[5] |= 0x80;
        cmd[0x16] = 0x81;
    }
    float f = float_at(0x44);
    if (f > 0.0f) cmd[5] |= 0x20;
    else if (f < 0.0f) cmd[5] |= 0x10;
    if (float_at(0xE0) != 0.0f) cmd[4] |= 0x10;
    if (pm_flags & 0x10000) cmd[4] |= 8;
}

bool read_delta_entity(Msg& msg, const FieldTables& tables, ByteView from, ByteSpan to, std::int32_t number) {
    const FieldTable* table = &tables.entity;
    if (tables.game == Game::UnitedOffensive && tables.entity_alternate && msg.read_bit() != 0) table = &*tables.entity_alternate;
    return read_delta_struct(msg, from, to, number, *table);
}

bool read_delta_archived_entity(Msg& msg, const FieldTables& tables, ByteView from, ByteSpan to, std::int32_t number) {
    return read_delta_struct(msg, from, to, number, tables.archived_entity);
}

bool read_delta_client(Msg& msg, const FieldTables& tables, const ByteView* from, ByteSpan to, std::int32_t number) {
    static const std::array<std::uint8_t, kClientStateSize> zeros{};
    return read_delta_struct(msg, from ? *from : ByteView(zeros), to, number, tables.client);
}

// --- the player's own state ------------------------------------------------------

std::size_t player_state_size(Game game) { return player_state_layout(game).size; }
std::size_t hud_elem_count(Game game) { return player_state_layout(game).hud_count; }
std::size_t hud_elem_size(Game game) { return player_state_layout(game).hud_elem_size; }

void write_delta_hud_elems(Msg& msg, const FieldTables& tables, ByteView from, ByteView to, std::size_t count) {
    write_hud_elems(msg, tables.hud_elem.fields(), player_state_layout(tables.game), from, to, count);
}

void read_delta_hud_elems(Msg& msg, const FieldTables& tables, ByteView from, ByteSpan to, std::size_t at, std::size_t count) {
    read_hud_elems(msg, tables.hud_elem.fields(), player_state_layout(tables.game), from, to, at, count);
}

void write_delta_playerstate(Msg& msg, const FieldTables& tables, const ByteView* from_arg, ByteView to) {
    const PlayerStateLayout& layout = player_state_layout(tables.game);
    ByteView from = from_arg ? *from_arg : ByteView(zero_player_state(), layout.size);

    // The fields up to the last changed one.
    const auto& fields = tables.player.fields();
    std::size_t last_changed = 0;
    for (std::size_t k = fields.size(); k > 0; --k) {
        if (word_at(from, fields[k - 1].offset) != word_at(to, fields[k - 1].offset)) {
            last_changed = k;
            break;
        }
    }
    msg.write_byte(static_cast<std::uint8_t>(last_changed));
    for (std::size_t k = 0; k < last_changed; ++k) write_player_field(msg, from, to, fields[k]);

    // Statistics.
    auto stat = [&](ByteView state, std::size_t i) { return word_at(state, static_cast<std::uint32_t>(layout.stats + 4 * i)); };
    std::uint32_t stats_mask = 0;
    for (std::uint32_t i = 0; i < 6; ++i) {
        if (stat(from, i) != stat(to, i)) stats_mask |= 1u << i;
    }
    if (stats_mask == 0) {
        msg.write_bit0();
    } else {
        msg.write_bit1();
        msg.write_bits(static_cast<std::int32_t>(stats_mask), 6);
        for (std::uint32_t i = 0; i < 6; ++i) {
            if ((stats_mask & (1u << i)) == 0) continue;
            if (i == 3) msg.write_bits(stat(to, i), 6);
            else if (i == 5) msg.write_byte(static_cast<std::uint8_t>(stat(to, i)));
            else msg.write_short(static_cast<std::int16_t>(stat(to, i)));
        }
    }

    // Ammunition: a bit for the whole array, then one per group.
    std::array<std::uint32_t, kAmmoGroups> ammo_masks{};
    bool any = false;
    for (std::size_t g = 0; g < kAmmoGroups; ++g) {
        ammo_masks[g] = changed_mask(from, to, layout.ammo + 64 * g);
        any = any || ammo_masks[g] != 0;
    }
    if (!any) {
        msg.write_bit0();
    } else {
        msg.write_bit1();
        for (std::size_t g = 0; g < kAmmoGroups; ++g) {
            if (ammo_masks[g] == 0) {
                msg.write_bit0();
            } else {
                msg.write_bit1();
                write_short_group(msg, to, layout.ammo + 64 * g, ammo_masks[g]);
            }
        }
    }

    // Ammunition in the clip: one bit per group.
    for (std::size_t g = 0; g < kAmmoGroups; ++g) {
        std::size_t at = layout.clip + 64 * g;
        std::uint32_t mask = changed_mask(from, to, at);
        if (mask == 0) {
            msg.write_bit0();
        } else {
            msg.write_bit1();
            write_short_group(msg, to, at, mask);
        }
    }

    // Objectives: each one's state in 3 bits, then its fields when any changed.
    std::size_t all = kObjectives * kObjectiveSize;
    if (std::memcmp(from.data() + layout.objectives, to.data() + layout.objectives, all) == 0) {
        msg.write_bit0();
    } else {
        msg.write_bit1();
        const auto& objective_fields = tables.objective.fields();
        for (std::size_t k = 0; k < kObjectives; ++k) {
            std::size_t at = layout.objectives + k * kObjectiveSize;
            ByteView old_obj = from.subspan(at, kObjectiveSize), new_obj = to.subspan(at, kObjectiveSize);
            msg.write_bits(word_at(new_obj, 0), 3);
            bool changed = false;
            for (const NetField& f : objective_fields) changed = changed || word_at(old_obj, f.offset) != word_at(new_obj, f.offset);
            if (changed) {
                msg.write_bit1();
                for (const NetField& f : objective_fields) write_delta_field(msg, old_obj, new_obj, f);
            } else {
                msg.write_bit0();
            }
        }
    }

    // HUD elements: the current array, then the archived one.
    std::size_t array = layout.hud_count * layout.hud_elem_size;
    std::size_t archived = layout.hud, current = layout.hud + array;
    if (std::memcmp(from.data() + archived, to.data() + archived, 2 * array) == 0) {
        msg.write_bit0();
    } else {
        msg.write_bit1();
        const auto& hud_fields = tables.hud_elem.fields();
        for (std::size_t at : {current, archived})
            write_hud_elems(msg, hud_fields, layout, from.subspan(at, array), to.subspan(at, array), layout.hud_count);
    }
}

void read_delta_playerstate(Msg& msg, const FieldTables& tables, const ByteView* from_arg, ByteSpan to) {
    const PlayerStateLayout& layout = player_state_layout(tables.game);
    ByteView from = from_arg ? *from_arg : ByteView(zero_player_state(), layout.size);
    std::copy(from.begin(), from.begin() + static_cast<std::ptrdiff_t>(layout.size), to.begin());

    // The fields up to the count; the rest stay as copied.
    const auto& fields = tables.player.fields();
    std::int32_t count = msg.read_byte();
    std::size_t read = 0;
    if (count > 0) {
        read = static_cast<std::size_t>(count);
        if (read > fields.size()) {
            msg.overflowed = true;
            read = fields.size();
        }
        for (std::size_t k = 0; k < read; ++k) read_player_field(msg, from, to, fields[k]);
    }
    for (std::size_t k = read; k < fields.size(); ++k) copy_word(from, to, fields[k].offset);

    // Statistics.
    if (msg.read_bit() != 0) {
        std::int32_t mask = msg.read_bits(6);
        for (int i = 0; i < 6; ++i) {
            if ((mask & (1 << i)) == 0) continue;
            std::int32_t value = i == 3 ? msg.read_bits(6) : i == 5 ? msg.read_byte() : msg.read_short();
            set_word(to, static_cast<std::uint32_t>(layout.stats + 4 * static_cast<std::size_t>(i)), value);
        }
    }

    // Ammunition: a bit for the whole array, then one per group.
    if (msg.read_bit() != 0) {
        for (std::size_t g = 0; g < kAmmoGroups; ++g) {
            if (msg.read_bit() != 0) read_short_group(msg, to, layout.ammo + 64 * g);
        }
    }

    // Ammunition in the clip: one bit per group.
    for (std::size_t g = 0; g < kAmmoGroups; ++g) {
        if (msg.read_bit() != 0) read_short_group(msg, to, layout.clip + 64 * g);
    }

    // Objectives: each one's state in 3 bits, then its fields.
    if (msg.read_bit() != 0) {
        const auto& objective_fields = tables.objective.fields();
        for (std::size_t k = 0; k < kObjectives; ++k) {
            std::size_t at = layout.objectives + k * kObjectiveSize;
            std::int32_t state = msg.read_bits(3);
            set_word(to, static_cast<std::uint32_t>(at), state);
            read_delta_fields(msg, objective_fields, from.subspan(at, kObjectiveSize), to.subspan(at, kObjectiveSize));
        }
    }

    // HUD elements: the current array, then the archived one.
    if (msg.read_bit() != 0) {
        std::size_t array = layout.hud_count * layout.hud_elem_size;
        const auto& hud_fields = tables.hud_elem.fields();
        for (std::size_t at : {layout.hud + array, layout.hud}) read_hud_elems(msg, hud_fields, layout, from, to, at, layout.hud_count);
    }
}

// --- player commands -----------------------------------------------------------------

void write_delta_usercmd_key(Msg& msg, std::int32_t key, ByteView from, ByteView to) {
    std::int32_t time = word_at(to, 0);
    auto elapsed = static_cast<std::uint32_t>(wrap_sub(time, word_at(from, 0)));
    if (elapsed < 0x100) {
        msg.write_bit1();
        msg.write_byte(static_cast<std::uint8_t>(elapsed));
    } else {
        msg.write_bit0();
        msg.write_long(time);
    }
    if (msg.game == Game::CallOfDuty2) write_usercmd_cod2(msg, key, time, from, to);
    else write_usercmd_uo(msg, key, time, from, to);
}

void read_delta_usercmd_key(Msg& msg, std::int32_t key, ByteView from, ByteSpan to) {
    std::size_t size = usercmd_size(msg.game);
    std::copy(from.begin(), from.begin() + static_cast<std::ptrdiff_t>(size), to.begin());
    std::int32_t time = msg.read_bit() == 0 ? msg.read_long() : wrap_add(word_at(from, 0), msg.read_byte());
    put_word(to, 0, time);
    if (msg.read_bits(1) == (key & 1)) return;
    if (msg.game == Game::CallOfDuty2) read_usercmd_cod2(msg, key, time, from, to);
    else read_usercmd_uo(msg, key, time, from, to);
}

}  // namespace iw_pc::net
