// Requests for iw_pc::net::delta (the C++ version). States arrive as their
// bytes (s: tokens; s:- is a NULL state). A field arrives as v: with its
// offset and bit count (two little-endian 32-bit numbers), read by the spec
// from the executable's own table; whole tables come from the executable
// found through IW_PC_INPUT.

#include <memory>

#include "iw_pc/net/delta.h"
#include "msg_arg.h"

namespace pc_check {

using iw_pc::ByteSpan;
using iw_pc::ByteView;
using iw_pc::Game;
namespace net = iw_pc::net;

namespace {

// `game`'s field tables, read once from its executable.
const net::FieldTables* tables(Game game, std::string& error) {
    static std::unique_ptr<net::FieldTables> cache[2];
    static std::string errors[2];
    std::size_t k = game == Game::UnitedOffensive ? 1 : 0;
    if (!cache[k] && errors[k].empty()) {
        const auto* exe = game_executable(game, errors[k]);
        if (exe) {
            auto read = net::FieldTables::from_executable(*exe);
            if (auto* t = std::get_if<net::FieldTables>(&read)) cache[k] = std::make_unique<net::FieldTables>(std::move(*t));
            else errors[k] = std::get<net::ProtocolDataError>(read).message;
        }
    }
    error = errors[k];
    return cache[k].get();
}

std::optional<net::NetField> field(ByteView bytes) {
    if (bytes.size() < 8) return std::nullopt;
    net::NetField f;
    f.offset = static_cast<std::uint32_t>(net::word_at(bytes, 0));
    f.bits = net::word_at(bytes, 4);
    return f;
}

// A state argument: its bytes, or nullopt for NULL.
std::optional<Bytes> state(Args& args, std::size_t i) {
    const Bytes* s = args.str(i);
    if (!s) return std::nullopt;
    return *s;
}

// Runs `f` on a copy of buffer argument `i` (the structure a reader fills) and puts it back.
template <class F>
std::string read_into(Args& args, std::size_t i, F f) {
    Bytes* slot = args.buffer(i);
    if (!slot) return "NULL to";
    Bytes to = *slot;
    std::string error = f(to);
    *args.buffer(i) = to;
    return error;
}

// A table argument (its name, as specs/net_delta.py's TABLES has it); nullptr if unknown.
const net::FieldTable* table(const net::FieldTables& t, ByteView name) {
    std::string n(name.begin(), name.end());
    if (n == "entity") return &t.entity;
    if (n == "entity12") return t.entity_alternate ? &*t.entity_alternate : nullptr;
    if (n == "client") return &t.client;
    if (n == "archived") return &t.archived_entity;
    if (n == "player") return &t.player;
    if (n == "hud") return &t.hud_elem;
    if (n == "objective") return &t.objective;
    return nullptr;
}

// United Offensive's MSG_WriteEntityTableFlag: the callback a check of MSG_WriteDeltaStruct may pass.
constexpr std::int32_t kUoTableFlag = 0x0044AF00;

}  // namespace

const net::FieldTables* field_tables(Game game, std::string& error) { return tables(game, error); }

std::optional<Answer> dispatch_delta(Game game, const std::string& f, Args& args) {
    std::string error;
    auto need_tables = [&]() { return tables(game, error); };
    if (f == "MSG_WriteDeltaField") {  // from, to, msg, field
        auto from = state(args, 0), to = state(args, 1);
        if (!from) return Answer::fail("NULL from");
        if (!to) return Answer::fail("NULL to");
        auto fl = field(args.bytes(3));
        if (!fl) return Answer::fail("short field token");
        return after(with_msg(args, 2, game, [&](net::Msg& m) { net::write_delta_field(m, *from, *to, *fl); }), nothing());
    }
    if (f == "MSG_WriteDeltaEntity" || f == "MSG_WriteDeltaClient") {  // msg, from, to, force
        const auto* t = need_tables();
        if (!t) return Answer::fail(error);
        auto from = state(args, 1), to = state(args, 2);
        bool force = args.int64(3) != 0;
        ByteView from_view = from ? ByteView(*from) : ByteView{}, to_view = to ? ByteView(*to) : ByteView{};
        if (f == "MSG_WriteDeltaEntity") {
            if (!from) return Answer::fail("NULL from");
            return after(with_msg(args, 0, game, [&](net::Msg& m) { net::write_delta_entity(m, *t, from_view, to ? &to_view : nullptr, force); }),
                         nothing());
        }
        return after(with_msg(args, 0, game,
                              [&](net::Msg& m) {
                                  net::write_delta_client(m, *t, from ? &from_view : nullptr, to ? &to_view : nullptr, force);
                              }),
                     nothing());
    }
    if (f == "MSG_WriteDeltaStruct") {  // msg, from, to, force, field count, number bits, table, [UO: callback,] change bit
        const auto* t = need_tables();
        if (!t) return Answer::fail(error);
        auto from = state(args, 1), to = state(args, 2);
        if (!from) return Answer::fail("NULL from");
        const net::FieldTable* tb = table(*t, args.bytes(6));
        if (!tb) return Answer::fail("unknown table");
        std::int32_t count = args.i32(4);
        if (count < 0 || static_cast<std::size_t>(count) > tb->fields().size()) return Answer::fail("field count past the table");
        net::TableFlag flag = nullptr;
        std::int32_t change_bit = args.i32(7);
        if (game == Game::UnitedOffensive) {
            std::int32_t callback = args.i32(7);
            if (callback == kUoTableFlag) flag = net::write_entity_table_flag;
            else if (callback != 0) return Answer::fail("unknown callback");
            change_bit = args.i32(8);
        }
        net::StructCoding coding{std::span<const net::NetField>(tb->fields()).first(static_cast<std::size_t>(count)),
                                 static_cast<std::uint32_t>(args.i32(5)), change_bit != 0, flag};
        bool force = args.i32(3) != 0;
        ByteView from_view(*from), to_view = to ? ByteView(*to) : ByteView{};
        return after(with_msg(args, 0, game,
                              [&](net::Msg& m) { net::write_delta_struct(m, from_view, to ? &to_view : nullptr, force, coding); }),
                     nothing());
    }
    if (f == "MSG_WriteDeltaHudElems") {  // msg, from, to, count
        const auto* t = need_tables();
        if (!t) return Answer::fail(error);
        auto from = state(args, 1), to = state(args, 2);
        if (!from) return Answer::fail("NULL from");
        if (!to) return Answer::fail("NULL to");
        std::int32_t count = args.i32(3);
        if (count < 0) return Answer::fail("negative count");
        return after(with_msg(args, 0, game,
                              [&](net::Msg& m) { net::write_delta_hud_elems(m, *t, *from, *to, static_cast<std::size_t>(count)); }),
                     nothing());
    }
    if (f == "MSG_WriteDeltaPlayerstate") {  // msg, from, to
        const auto* t = need_tables();
        if (!t) return Answer::fail(error);
        auto from = state(args, 1), to = state(args, 2);
        if (!to) return Answer::fail("NULL to");
        ByteView from_view = from ? ByteView(*from) : ByteView{};
        return after(with_msg(args, 0, game, [&](net::Msg& m) { net::write_delta_playerstate(m, *t, from ? &from_view : nullptr, *to); }),
                     nothing());
    }
    if (f == "MSG_WriteDeltaUsercmdKey") {  // msg, from, to, key
        auto from = state(args, 1), to = state(args, 2);
        if (!from) return Answer::fail("NULL from");
        if (!to) return Answer::fail("NULL to");
        std::int32_t key = args.i32(3);
        return after(with_msg(args, 0, game, [&](net::Msg& m) { net::write_delta_usercmd_key(m, key, *from, *to); }), nothing());
    }
    if (f == "MSG_ReadDeltaUsercmdKey") {  // msg, key, from, to (out)
        std::int32_t key = args.i32(1);
        auto from = state(args, 2);
        if (!from) return Answer::fail("NULL from");
        std::string e = read_into(args, 3, [&](Bytes& to) {
            return with_msg(args, 0, game, [&](net::Msg& m) { net::read_delta_usercmd_key(m, key, *from, to); });
        });
        return after(e, nothing());
    }
    if (f == "MSG_ReadDeltaField") {  // msg, from, to (out), field, print flag
        auto from = state(args, 1);
        if (!from) return Answer::fail("NULL from");
        auto fl = field(args.bytes(3));
        if (!fl) return Answer::fail("short field token");
        std::string e = read_into(args, 2, [&](Bytes& to) {
            return with_msg(args, 0, game, [&](net::Msg& m) { net::read_delta_field(m, *from, to, *fl); });
        });
        return after(e, nothing());
    }
    if (f == "MSG_ReadDeltaEntity" || f == "MSG_ReadDeltaArchivedEntity" || f == "MSG_ReadDeltaClient") {  // msg, from, to (out), number
        const auto* t = need_tables();
        if (!t) return Answer::fail(error);
        auto from = state(args, 1);
        std::int32_t number = args.i32(3);
        if (!from && f != "MSG_ReadDeltaClient") return Answer::fail("NULL from");
        ByteView from_view = from ? ByteView(*from) : ByteView{};
        bool removed = false;
        std::string e = read_into(args, 2, [&](Bytes& to) {
            return with_msg(args, 0, game, [&](net::Msg& m) {
                if (f == "MSG_ReadDeltaEntity") removed = net::read_delta_entity(m, *t, from_view, to, number);
                else if (f == "MSG_ReadDeltaArchivedEntity") removed = net::read_delta_archived_entity(m, *t, from_view, to, number);
                else removed = net::read_delta_client(m, *t, from ? &from_view : nullptr, to, number);
            });
        });
        return after(e, integer(removed ? 1 : 0));
    }
    if (f == "MSG_ReadDeltaStruct") {  // msg, from, to (out), number, field count, table
        const auto* t = need_tables();
        if (!t) return Answer::fail(error);
        auto from = state(args, 1);
        if (!from) return Answer::fail("NULL from");
        std::int32_t number = args.i32(3);
        const net::FieldTable* tb = table(*t, args.bytes(5));
        if (!tb) return Answer::fail("unknown table");
        if (args.i32(4) < 0 || static_cast<std::size_t>(args.i32(4)) != tb->fields().size()) return Answer::fail("the checks pass whole tables");
        bool removed = false;
        std::string e = read_into(args, 2, [&](Bytes& to) {
            return with_msg(args, 0, game, [&](net::Msg& m) { removed = net::read_delta_struct(m, *from, to, number, *tb); });
        });
        return after(e, integer(removed ? 1 : 0));
    }
    if (f == "MSG_ReadDeltaHudElems") {  // msg, from, to (out), count
        const auto* t = need_tables();
        if (!t) return Answer::fail(error);
        auto from = state(args, 1);
        if (!from) return Answer::fail("NULL from");
        std::int32_t count = args.i32(3);
        if (count < 0) return Answer::fail("negative count");
        std::string e = read_into(args, 2, [&](Bytes& to) {
            return with_msg(args, 0, game,
                            [&](net::Msg& m) { net::read_delta_hud_elems(m, *t, *from, to, 0, static_cast<std::size_t>(count)); });
        });
        return after(e, nothing());
    }
    if (f == "MSG_ReadDeltaPlayerstate") {  // msg, from, to (out)
        const auto* t = need_tables();
        if (!t) return Answer::fail(error);
        auto from = state(args, 1);
        ByteView from_view = from ? ByteView(*from) : ByteView{};
        std::string e = read_into(args, 2, [&](Bytes& to) {
            return with_msg(args, 0, game, [&](net::Msg& m) { net::read_delta_playerstate(m, *t, from ? &from_view : nullptr, to); });
        });
        return after(e, nothing());
    }
    return std::nullopt;
}

}  // namespace pc_check
