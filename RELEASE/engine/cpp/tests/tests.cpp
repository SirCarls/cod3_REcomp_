// Unit tests of the C++ version that need no game files (the checker,
// tools/decomp/x86/pcdecomp.py check --impl cpp, compares every function
// with the originals). Run: ctest --test-dir engine/cpp/build

#include <cstdio>
#include <cstring>
#include <vector>

#include "iw_pc/net/delta.h"
#include "iw_pc/net/msg.h"
#include "iw_pc/shared/info.h"
#include "iw_pc/shared/strings.h"

using namespace iw_pc;

static int failures = 0;

#define CHECK(condition)                                                     \
    do {                                                                     \
        if (!(condition)) {                                                  \
            std::printf("%s:%d: failed: %s\n", __FILE__, __LINE__, #condition); \
            ++failures;                                                      \
        }                                                                    \
    } while (0)

static void bits_and_bytes_interleave() {
    std::uint8_t buffer[16];
    std::memset(buffer, 0xAA, sizeof buffer);
    net::Msg msg(Game::CallOfDuty2, buffer);
    msg.write_bits(0b101, 3);
    msg.write_byte(0x7F);
    msg.write_bit1();
    msg.write_bits(-1, 6);
    CHECK(msg.cursize == 3);
    CHECK(buffer[0] == 0b11111101 && buffer[1] == 0x7F && buffer[2] == 0b11);
    msg.begin_reading();
    CHECK(msg.read_bits(3) == 0b101);
    CHECK(msg.read_byte() == 0x7F);
    CHECK(msg.read_bit() == 1);
    CHECK(msg.read_bits(6) == 0b111111);
    CHECK(!msg.overflowed);
}

static void readers_past_the_end_differ_by_game() {
    std::uint8_t a[4] = {1, 2, 3, 4};
    net::Msg cod2(Game::CallOfDuty2, a);
    cod2.cursize = 1;
    CHECK(cod2.read_bits(16) == -1);
    CHECK(cod2.overflowed);
    std::uint8_t b[4] = {1, 2, 3, 4};
    net::Msg uo(Game::UnitedOffensive, b);
    uo.cursize = 1;
    CHECK(uo.read_bits(16) == 0x0201);
    CHECK(uo.read_byte() == -1);
    CHECK(!uo.overflowed);
}

static void info_strings() {
    std::uint8_t info[1024] = {};
    CHECK(!shared::info::set_value_for_key(info, bytes("name"), bytes("Tester")));
    CHECK(!shared::info::set_value_for_key(info, bytes("rate"), bytes("25000")));
    auto value = shared::info::value_for_key(info, bytes("NAME"));
    CHECK(value && value.value().size() == 6 && std::memcmp(value.value().data(), "Tester", 6) == 0);
    CHECK(shared::strings::stricmpn(bytes("abd"), bytes("ABC"), 99) == 1);
}

// A made-up table (the games' tables are game data): one field of each
// kind, 4 bytes apart after the number.
static net::FieldTable table(std::vector<std::int32_t> kinds) {
    std::vector<std::uint8_t> raw;
    for (std::size_t i = 0; i < kinds.size(); ++i) {
        std::uint32_t words[3] = {0, static_cast<std::uint32_t>(4 + 4 * i), static_cast<std::uint32_t>(kinds[i])};
        for (std::uint32_t w : words) {
            for (int j = 0; j < 4; ++j) raw.push_back(static_cast<std::uint8_t>(w >> (8 * j)));
        }
    }
    return net::FieldTable::from_bytes(raw);
}

static std::vector<std::uint8_t> state(std::vector<std::int32_t> words) {
    std::vector<std::uint8_t> out;
    for (std::int32_t w : words) {
        for (int j = 0; j < 4; ++j) out.push_back(static_cast<std::uint8_t>(static_cast<std::uint32_t>(w) >> (8 * j)));
    }
    return out;
}

static std::int32_t f(float x) {
    std::int32_t bits;
    std::memcpy(&bits, &x, 4);
    return bits;
}

static void structures_survive_the_wire() {
    net::FieldTable fields = table({0, net::kAngleField, 8, 32, 10});
    auto from = state({7, f(1.5f), f(90.0f), 3, -5, 1000});
    std::vector<std::vector<std::uint8_t>> cases = {
        state({7, f(1.5f), f(90.0f), 3, -5, 1000}),
        state({7, f(-4096.0f), f(0.0f), 255, 0, 1023}),
        state({7, f(0.25f), f(-45.0f), 0, INT32_MIN, 1}),
        state({7, f(4095.0f), f(90.0f), 3, -5, 999}),
    };
    for (Game game : {Game::CallOfDuty2, Game::UnitedOffensive}) {
        for (const auto& to : cases) {
            std::uint8_t buffer[128] = {};
            net::Msg msg(game, buffer);
            ByteView to_view = to;
            net::write_delta_struct(msg, from, &to_view, true, {fields.fields(), net::kEntityNumberBits, false, nullptr});
            msg.begin_reading();
            std::int32_t number = msg.read_bits(net::kEntityNumberBits);
            std::vector<std::uint8_t> back(to.size(), 0xEE);
            CHECK(!net::read_delta_struct(msg, from, back, number, fields));
            CHECK(back == to);
            CHECK(msg.readcount == msg.cursize);
        }
    }
}

static void only_call_of_duty_2_extends_the_sign() {
    net::NetField field{0, -8};
    std::uint8_t from[4] = {}, to[4];
    std::int32_t minus_three = -3;
    std::memcpy(to, &minus_three, 4);
    for (Game game : {Game::CallOfDuty2, Game::UnitedOffensive}) {
        std::uint8_t buffer[16] = {};
        net::Msg msg(game, buffer);
        net::write_delta_field(msg, from, to, field);
        msg.begin_reading();
        std::uint8_t back[4] = {};
        net::read_delta_field(msg, from, back, field);
        std::int32_t value;
        std::memcpy(&value, back, 4);
        CHECK(value == (game == Game::CallOfDuty2 ? -3 : 0xFD));
    }
}

static void player_commands_survive_the_wire() {
    for (Game game : {Game::CallOfDuty2, Game::UnitedOffensive}) {
        std::size_t size = net::usercmd_size(game);
        std::size_t angles = game == Game::CallOfDuty2 ? 0xC : 8, forward = game == Game::CallOfDuty2 ? 0x18 : 0x14;
        auto cmd = [&](std::int32_t time, std::int32_t a0, std::uint8_t fwd) {
            std::vector<std::uint8_t> c(size, 0);
            std::memcpy(c.data(), &time, 4);
            std::memcpy(c.data() + angles, &a0, 4);
            c[forward] = fwd;
            return c;
        };
        auto from = cmd(1000, 10, 0);
        for (const auto& to : {cmd(1016, 10, 0), cmd(1050, 65535, 0x7F), cmd(9000, 11, 0x81)}) {
            std::uint8_t buffer[64] = {};
            net::Msg msg(game, buffer);
            net::write_delta_usercmd_key(msg, 0x12345678, from, to);
            msg.begin_reading();
            std::vector<std::uint8_t> back(size, 0xEE);
            net::read_delta_usercmd_key(msg, 0x12345678, from, back);
            CHECK(back == to);
        }
    }
}

int main() {
    bits_and_bytes_interleave();
    readers_past_the_end_differ_by_game();
    info_strings();
    structures_survive_the_wire();
    only_call_of_duty_2_extends_the_sign();
    player_commands_survive_the_wire();
    if (failures == 0) std::printf("all tests passed\n");
    return failures == 0 ? 0 : 1;
}
