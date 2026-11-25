// Requests for iw_pc::game::pmove and iw_pc::shared::math (the C++ version; Call of Duty 2;
// specs/game_pmove.py): the player state and the move's scratch state arrive as buffers of
// their bytes, the settings as their values.

#include <algorithm>
#include <cstring>
#include <optional>

#include "iw_pc/game/anim.h"
#include "iw_pc/game/duck.h"
#include "iw_pc/game/ground.h"
#include "iw_pc/game/jump.h"
#include "iw_pc/game/mantle.h"
#include "iw_pc/game/moves.h"
#include "iw_pc/game/pmove.h"
#include "iw_pc/game/prone.h"
#include "iw_pc/game/slide.h"
#include "iw_pc/game/view.h"
#include "iw_pc/game/weapon.h"
#include "iw_pc/game/weapon_state.h"
#include "iw_pc/shared/math.h"
#include "net_common.h"
#include "protocol.h"

namespace pc_check {

namespace pm = iw_pc::game::pmove;

namespace {

template <std::size_t N>
std::array<float, N> floats(const Bytes& b) {
    std::array<float, N> out{};
    std::memcpy(out.data(), b.data(), std::min(b.size(), 4 * N));
    return out;
}

template <std::size_t N>
void put_floats(Bytes& b, const std::array<float, N>& v) {
    std::memcpy(b.data(), v.data(), std::min(b.size(), 4 * N));
}

bool flag(const Args& args, std::size_t i) { return (args.i32(i) & 0xFF) != 0; }

template <class T>
T from_bytes(const Bytes& b) {
    T s;
    std::copy(b.begin(), b.begin() + static_cast<std::ptrdiff_t>(std::min(b.size(), s.bytes.size())), s.bytes.begin());
    return s;
}

template <class T>
void back(Bytes& b, const T& s) {
    std::copy(s.bytes.begin(), s.bytes.begin() + static_cast<std::ptrdiff_t>(b.size()), b.begin());
}

/// The little-endian words of b.
std::vector<std::int32_t> words(iw_pc::ByteView b) {
    std::vector<std::int32_t> out(b.size() / 4);
    if (!out.empty()) std::memcpy(out.data(), b.data(), 4 * out.size());
    return out;
}

/// The check's world: each trace is logged as the original's stand-ins record it and answered with
/// the case's next result (nothing hit once they run out); the weapons' move-speed scales and the
/// animation scripts are the case's (specs/game_pmove.py WeaponScales, AnimAnswers).
class CheckWorld : public iw_pc::game::trace::MoveHost {
public:
    /// With the weapons' definitions (WeaponDefs: the highest weapon number, then the definitions).
    CheckWorld& defs(iw_pc::ByteView b) {
        namespace wp = iw_pc::game::weapon;
        if (b.size() >= 4) {
            std::memcpy(&weapon_count_, b.data(), 4);
            for (std::size_t at = 4; at < b.size(); at += wp::kWeaponDefSize) {
                wp::WeaponDef d;
                std::size_t n = std::min(wp::kWeaponDefSize, b.size() - at);
                std::memcpy(d.bytes.data(), b.data() + at, n);
                defs_.push_back(d);
            }
        }
        return *this;
    }
    /// With the player's animation state (ClientAnim).
    CheckWorld& anim_info(const Bytes& b) {
        anim_info_ = from_bytes<iw_pc::game::anim::ClientAnimInfo>(b);
        return *this;
    }
    const iw_pc::game::weapon::WeaponDef& weapon_def(std::int32_t weapon) const override {
        if (weapon < 0 || static_cast<std::size_t>(weapon) >= defs_.size()) return no_def_;
        return defs_[static_cast<std::size_t>(weapon)];
    }
    std::int32_t weapon_count() const override { return weapon_count_; }
    /// The case's state, whichever client asks.
    iw_pc::game::anim::ClientAnimInfo& client_anim_info(std::int32_t) override { return anim_info_; }
    const iw_pc::game::anim::ClientAnimInfo& anim_info() const { return anim_info_; }
    CheckWorld& weapons(iw_pc::ByteView scales) {
        for (std::int32_t w : words(scales)) {
            float f;
            std::memcpy(&f, &w, 4);
            weapon_scales_.push_back(f);
        }
        return *this;
    }
    CheckWorld& scripts(iw_pc::ByteView answers) {
        auto w = words(answers);
        if (w.size() >= 4 && w[3] >= 0 && w.size() == 4 + 2 * static_cast<std::size_t>(w[3])) {
            has_anim_ = true;
            commands_ = w[0];
            rand_ = w[1];
            duration_ = w[2];
            items_.clear();
            for (std::size_t k = 4; k + 1 < w.size(); k += 2) items_.push_back({w[k], w[k + 1]});
        }
        return *this;
    }
    float weapon_move_speed_scale(std::int32_t weapon) const override {
        if (weapon < 0 || static_cast<std::size_t>(weapon) >= weapon_scales_.size()) return 0.0f;
        return weapon_scales_[static_cast<std::size_t>(weapon)];
    }
    /// As BG_AnimScriptEvent goes, with the case's scripts (see the Rust version).
    std::int32_t anim_script_event(pm::PlayerState& ps, std::int32_t event, bool is_continue, bool force) override {
        namespace tr = iw_pc::game::trace;
        if (event != tr::kAnimEtDeath && ps.pm_type() >= pm::pm_type::kDead) return -1;
        if (!has_anim_) return -1;
        std::int32_t items = 0;
        for (const auto& [e, n] : items_)
            if (e == event) {
                items = n;
                break;
            }
        if (items == 0) return -1;
        log.call("anim_first_valid_item", {CallLog::i(ps.integer(pm::ps::kClientNum)), CallLog::i(event)});
        if (commands_ <= 0) return -1;
        log.call("rand", {});
        std::int32_t command = rand_ % commands_;
        log.call("anim_execute_command",
                 {CallLog::i(command), CallLog::i(1), CallLog::i(is_continue ? 1 : 0), CallLog::i(force ? 1 : 0)});
        return 0;
    }
    /// Logged as the original's Com_Printf stand-in records it (its format).
    void print(const char* message) override { log.call("com_printf", {CallLog::s(message)}); }
    /// The case's duration for animation 0 (the only one the spec gives).
    std::int32_t anim_duration(std::int32_t anim) const override { return has_anim_ && anim == 0 ? duration_ : 0; }
    explicit CheckWorld(iw_pc::ByteView answers) {
        namespace tr = iw_pc::game::trace;
        for (std::size_t at = 0; at + tr::kTraceSize <= answers.size(); at += tr::kTraceSize) {
            tr::Trace t;
            std::copy(answers.begin() + static_cast<std::ptrdiff_t>(at),
                      answers.begin() + static_cast<std::ptrdiff_t>(at + tr::kTraceSize), t.bytes.begin());
            answers_.push_back(t);
        }
    }
    iw_pc::game::trace::Trace trace(std::uint8_t handler, const std::array<float, 3>& start,
                                    const std::array<float, 3>& mins, const std::array<float, 3>& maxs,
                                    const std::array<float, 3>& end, std::int32_t pass_entity,
                                    std::int32_t content_mask) override {
        std::array<Bytes, 4> v;
        const std::array<const std::array<float, 3>*, 4> from{&start, &mins, &maxs, &end};
        for (std::size_t k = 0; k < 4; ++k) {
            v[k].resize(12);
            std::memcpy(v[k].data(), from[k]->data(), 12);
        }
        log.call("trace", {CallLog::i(handler), CallLog::s(v[0]), CallLog::s(v[1]), CallLog::s(v[2]),
                           CallLog::s(v[3]), CallLog::i(pass_entity), CallLog::i(content_mask)});
        std::size_t k = next_++;
        if (k < answers_.size()) return answers_[k];
        iw_pc::game::trace::Trace t;
        float one = 1.0f;
        std::memcpy(t.bytes.data(), &one, 4);
        t.bytes[0x1C] = 0xFF;
        t.bytes[0x1D] = 0x03;
        return t;
    }
    CallLog log;

private:
    std::vector<iw_pc::game::trace::Trace> answers_;
    std::size_t next_ = 0;
    std::vector<float> weapon_scales_;
    bool has_anim_ = false;
    std::int32_t commands_ = -1, rand_ = 0, duration_ = 0;
    std::vector<std::pair<std::int32_t, std::int32_t>> items_;
    std::vector<iw_pc::game::weapon::WeaponDef> defs_;
    std::int32_t weapon_count_ = 0;
    iw_pc::game::weapon::WeaponDef no_def_;
    iw_pc::game::anim::ClientAnimInfo anim_info_;
};

/// The handler of a trace function's address (specs/game_pmove.py TRACE_FUNCTIONS); -1: none.
int trace_handler(std::int32_t address) {
    switch (static_cast<std::uint32_t>(address)) {
    case 0x004DE690u: return 0;
    case 0x004FD900u: return 1;
    default: return -1;
    }
}

/// How many dvars a whole move reads (specs/game_pmove.py MOVE_DVARS).
constexpr std::size_t kMoveDvarCount = 22;

/// The step slide move's three dvars at first (specs/game_pmove.py STEP_DVARS).
pm::MoveSettings step_settings(const Args& args, std::size_t first) {
    pm::MoveSettings s;
    s.jump_height = args.f32(first);
    s.jump_step_size = args.f32(first + 1);
    s.footsteps_threshold = args.f32(first + 2);
    return s;
}

/// The fall damage's two dvars at first (specs/game_pmove.py FALL_DVARS).
pm::MoveSettings fall_settings(const Args& args, std::size_t first) {
    pm::MoveSettings s;
    s.fall_damage_min_height = args.f32(first);
    s.fall_damage_max_height = args.f32(first + 1);
    return s;
}

/// The view's six dvars at first (specs/game_pmove.py VIEW_DVARS).
void view_settings(const Args& args, std::size_t first, pm::MoveSettings& s) {
    s.view_pitch_up = args.f32(first);
    s.view_pitch_down = args.f32(first + 1);
    s.ladder_yaw_cap = args.f32(first + 2);
    s.prone_yaw_cap = args.f32(first + 3);
    s.mantle.enable = flag(args, first + 4);
    s.mantle.view_yaw_cap = args.f32(first + 5);
}

/// The settings a whole move reads, from the kMoveDvarCount dvar arguments at first (specs/game_pmove.py MOVE_DVARS).
pm::MoveSettings move_settings(const Args& args, std::size_t first) {
    pm::MoveSettings s;
    s.stop_speed = args.f32(first);
    s.friction = args.f32(first + 1);
    s.jump_slowdown_enable = flag(args, first + 2);
    s.inertia_max = args.f32(first + 3);
    s.inertia_angle = args.f32(first + 4);
    s.inertia_debug = flag(args, first + 5);
    s.spectate_speed_scale = args.f32(first + 6);
    s.jump_height = args.f32(first + 7);
    s.jump_step_size = args.f32(first + 8);
    s.footsteps_threshold = args.f32(first + 9);
    s.back_speed_scale = args.f32(first + 10);
    s.strafe_speed_scale = args.f32(first + 11);
    s.dmg_timer_max_time = args.f32(first + 12);
    s.dmg_timer_min_scale = args.f32(first + 13);
    s.jump_spread_add = args.f32(first + 14);
    s.jump_ladder_push_vel = args.f32(first + 15);
    view_settings(args, first + 16, s);
    return s;
}

/// The bytes of pmove_t after its player state pointer that a pm buffer carries first
/// (specs/game_pmove.py PM_BLOCK), then the player state.
constexpr std::size_t kPmBlock = pm::kPmoveSize - 4;

/// Runs f on a pm buffer as a move and puts the bytes back; false: too short.
template <class F>
bool with_pm(Bytes& b, F f) {
    if (b.size() < kPmBlock) return false;
    pm::Pmove m;
    std::copy(b.begin(), b.begin() + static_cast<std::ptrdiff_t>(kPmBlock), m.bytes.begin() + 4);
    Bytes state(b.begin() + static_cast<std::ptrdiff_t>(kPmBlock), b.end());
    m.ps = from_bytes<pm::PlayerState>(state);
    f(m);
    std::copy(m.bytes.begin() + 4, m.bytes.end(), b.begin());
    std::copy(m.ps.bytes.begin(), m.ps.bytes.begin() + static_cast<std::ptrdiff_t>(state.size()),
              b.begin() + static_cast<std::ptrdiff_t>(kPmBlock));
    return true;
}

}  // namespace

std::optional<Answer> dispatch_game(iw_pc::Game game, const std::string& f, Args& args) {
    if (game != iw_pc::Game::CallOfDuty2) return std::nullopt;
    if (f == "Vec3Normalize" || f == "Vec2Normalize") {
        Bytes* v = args.buffer(0);
        if (!v) return Answer::fail("NULL vector");
        float length;
        if (f == "Vec3Normalize") {
            auto a = floats<3>(*v);
            length = iw_pc::shared::math::vec3_normalize(a);
            put_floats(*v, a);
        } else {
            auto a = floats<2>(*v);
            length = iw_pc::shared::math::vec2_normalize(a);
            put_floats(*v, a);
        }
        return Answer(Ret(length));
    }
    if (f == "Vec3NormalizeTo" || f == "VecToAngles") {  // v, out
        Bytes* v = args.buffer(0);
        Bytes* out = args.buffer(1);
        if (!v || !out) return Answer::fail("NULL vector");
        auto in = floats<3>(*v);
        auto o = floats<3>(*out);
        if (f == "VecToAngles") {
            iw_pc::shared::math::vec_to_angles(in, o);
            put_floats(*out, o);
            return nothing();
        }
        float length = iw_pc::shared::math::vec3_normalize_to(in, o);
        put_floats(*out, o);
        return Answer(Ret(length));
    }
    if (f == "vectoyaw") {
        Bytes* v = args.buffer(0);
        if (!v) return Answer::fail("NULL vector");
        return Answer(Ret(iw_pc::shared::math::vectoyaw(floats<3>(*v))));
    }
    if (f == "AngleNormalize180") return Answer(Ret(iw_pc::shared::math::angle_normalize180(args.f32(0))));
    if (f == "Jump_ReduceFriction") {  // ps, jump_slowdownEnable
        Bytes* b = args.buffer(0);
        if (!b) return Answer::fail("NULL ps");
        pm::MoveSettings settings;
        settings.jump_slowdown_enable = flag(args, 1);
        auto ps = from_bytes<pm::PlayerState>(*b);
        float r = pm::jump_reduce_friction(ps, settings).store();
        back(*b, ps);
        return Answer(Ret(r));
    }
    if (f == "PM_Friction") {  // ps, pml, stopspeed, friction, jump_slowdownEnable
        Bytes* b = args.buffer(0);
        Bytes* l = args.buffer(1);
        if (!b || !l) return Answer::fail("NULL buffer");
        pm::MoveSettings settings;
        settings.stop_speed = args.f32(2);
        settings.friction = args.f32(3);
        settings.jump_slowdown_enable = flag(args, 4);
        auto ps = from_bytes<pm::PlayerState>(*b);
        auto pml = from_bytes<pm::MoveLocal>(*l);
        pm::friction(ps, pml, settings);
        back(*b, ps);
        return nothing();
    }
    if (f == "PM_InertiaTurnTooSharp") {  // ps, dir, amount, inertiaMax, inertiaAngle, inertiaDebug
        Bytes* b = args.buffer(0);
        Bytes* d = args.buffer(1);
        if (!b || !d) return Answer::fail("NULL buffer");
        pm::MoveSettings settings;
        settings.inertia_max = args.f32(3);
        settings.inertia_angle = args.f32(4);
        settings.inertia_debug = flag(args, 5);
        auto ps = from_bytes<pm::PlayerState>(*b);
        return integer(pm::inertia_turn_too_sharp(ps, floats<3>(*d), args.f32(2), settings) ? 1 : 0);
    }
    if (f == "PM_ClampInertia") {  // ps, amount, dir, inertiaMax, inertiaAngle, inertiaDebug
        Bytes* b = args.buffer(0);
        Bytes* d = args.buffer(2);
        if (!b || !d) return Answer::fail("NULL buffer");
        pm::MoveSettings settings;
        settings.inertia_max = args.f32(3);
        settings.inertia_angle = args.f32(4);
        settings.inertia_debug = flag(args, 5);
        auto ps = from_bytes<pm::PlayerState>(*b);
        return Answer(Ret(pm::clamp_inertia(ps, args.f32(1), floats<3>(*d), settings)));
    }
    if (f == "PM_Accelerate") {  // wishdir, ps, pml, wishspeed, accel, stopspeed, inertiaMax, inertiaAngle, inertiaDebug
        Bytes* d = args.buffer(0);
        Bytes* b = args.buffer(1);
        Bytes* l = args.buffer(2);
        if (!d || !b || !l) return Answer::fail("NULL buffer");
        pm::MoveSettings settings;
        settings.stop_speed = args.f32(5);
        settings.inertia_max = args.f32(6);
        settings.inertia_angle = args.f32(7);
        settings.inertia_debug = flag(args, 8);
        auto ps = from_bytes<pm::PlayerState>(*b);
        auto pml = from_bytes<pm::MoveLocal>(*l);
        pm::accelerate(ps, pml, floats<3>(*d), args.f32(3), args.f32(4), settings);
        back(*b, ps);
        return nothing();
    }
    if (f == "PM_MoveScale") {  // ps, forward, right, up, player_spectateSpeedScale
        Bytes* b = args.buffer(0);
        if (!b) return Answer::fail("NULL ps");
        pm::MoveSettings settings;
        settings.spectate_speed_scale = args.f32(4);
        auto ps = from_bytes<pm::PlayerState>(*b);
        return Answer(Ret(pm::move_scale(ps, args.f32(1), args.f32(2), args.f32(3), settings).store()));
    }
    if (f == "PM_CmdScale") {  // cmd, ps, player_spectateSpeedScale
        Bytes* c = args.buffer(0);
        Bytes* b = args.buffer(1);
        if (!c || !b || c->size() < 0x1A) return Answer::fail("NULL buffer");
        pm::MoveSettings settings;
        settings.spectate_speed_scale = args.f32(2);
        auto ps = from_bytes<pm::PlayerState>(*b);
        return Answer(Ret(pm::cmd_scale(ps, c->data(), settings).store()));
    }
    if (f == "PM_DropTimers") {  // ps, pml
        Bytes* b = args.buffer(0);
        Bytes* l = args.buffer(1);
        if (!b || !l) return Answer::fail("NULL buffer");
        auto ps = from_bytes<pm::PlayerState>(*b);
        pm::drop_timers(ps, from_bytes<pm::MoveLocal>(*l));
        back(*b, ps);
        return nothing();
    }
    if (f == "PM_SetMovementDir") {  // pm, pml
        Bytes* b = args.buffer(0);
        Bytes* l = args.buffer(1);
        if (!b || !l) return Answer::fail("NULL buffer");
        auto pml = from_bytes<pm::MoveLocal>(*l);
        if (!with_pm(*b, [&](pm::Pmove& m) { pm::set_movement_dir(m, pml); })) return Answer::fail("short pm");
        return nothing();
    }
    if (f == "PM_ClipVelocity") {  // in, normal, out
        Bytes* v = args.buffer(0);
        Bytes* n = args.buffer(1);
        Bytes* o = args.buffer(2);
        if (!v || !n || !o) return Answer::fail("NULL vector");
        put_floats<3>(*o, pm::clip_velocity(floats<3>(*v), floats<3>(*n)));
        return nothing();
    }
    if (f == "PM_AddTouchEnt") {  // pm, entityNum
        Bytes* b = args.buffer(0);
        if (!b) return Answer::fail("NULL pm");
        std::int32_t number = args.i32(1);
        if (!with_pm(*b, [&](pm::Pmove& m) { pm::add_touch_ent(m, number); })) return Answer::fail("short pm");
        return nothing();
    }
    if (f == "Jump_GetStepHeight") {  // ps, origin, step, jump_height, jump_stepSize
        pm::MoveSettings settings;
        settings.jump_height = args.f32(3);
        settings.jump_step_size = args.f32(4);
        Bytes* b = args.buffer(0);
        Bytes* o = args.buffer(1);
        Bytes* st = args.buffer(2);
        if (!b || !o || !st) return Answer::fail("NULL buffer");
        auto step = floats<1>(*st);
        bool r = pm::jump_get_step_height(from_bytes<pm::PlayerState>(*b), floats<3>(*o), step[0], settings);
        put_floats<1>(*st, step);
        return integer(r ? 1 : 0);
    }
    if (f == "Jump_ClampVelocity") {  // ps, origin, jump_height
        pm::MoveSettings settings;
        settings.jump_height = args.f32(2);
        Bytes* b = args.buffer(0);
        Bytes* o = args.buffer(1);
        if (!b || !o) return Answer::fail("NULL buffer");
        auto ps = from_bytes<pm::PlayerState>(*b);
        pm::jump_clamp_velocity(ps, floats<3>(*o), settings);
        back(*b, ps);
        return nothing();
    }
    if (f == "PM_ShouldMakeFootsteps") {  // pm, player_footstepsThreshhold
        pm::MoveSettings settings;
        settings.footsteps_threshold = args.f32(1);
        Bytes* b = args.buffer(0);
        if (!b) return Answer::fail("NULL pm");
        bool r = false;
        if (!with_pm(*b, [&](pm::Pmove& m) { r = pm::should_make_footsteps(m, settings); })) return Answer::fail("short pm");
        return integer(r ? 1 : 0);
    }
    if (f == "PM_playerTrace") {  // pm, results, start, mins, maxs, end, passEntityNum, contentMask, answers, log
        std::array<std::array<float, 3>, 4> v;
        for (std::size_t k = 0; k < 4; ++k) {
            Bytes* b = args.buffer(2 + k);
            if (!b) return Answer::fail("NULL vector");
            v[k] = floats<3>(*b);
        }
        Bytes* b = args.buffer(0);
        Bytes* results = args.buffer(1);
        Bytes* log = args.buffer(9);
        if (!b || !results || !log) return Answer::fail("NULL buffer");
        CheckWorld world(args.bytes(8));
        iw_pc::game::trace::Trace r;
        if (!with_pm(*b, [&](pm::Pmove& m) { r = pm::player_trace(m, v[0], v[1], v[2], v[3], args.i32(6), args.i32(7), world); }))
            return Answer::fail("short pm");
        std::copy(r.bytes.begin(), r.bytes.end(), results->begin());
        *log = world.log.out;
        return nothing();
    }
    if (f == "PM_PermuteRestrictiveClipPlanes") {  // planes, velocity, planeCount, permutation
        Bytes* raw = args.buffer(0);
        Bytes* v = args.buffer(1);
        Bytes* slot = args.buffer(3);
        std::int32_t n = args.i32(2);
        if (!raw || !v || !slot || n < 1 || n > 8) return Answer::fail("bad arguments");
        std::vector<std::array<float, 3>> planes(static_cast<std::size_t>(n));
        for (std::size_t i = 0; i < planes.size(); ++i) std::memcpy(planes[i].data(), raw->data() + 12 * i, 12);
        std::vector<std::size_t> order(planes.size());
        float r = iw_pc::game::slide::permute_restrictive_clip_planes(floats<3>(*v), planes, order);
        for (std::size_t i = 0; i < order.size(); ++i) {
            auto o = static_cast<std::int32_t>(order[i]);
            std::memcpy(slot->data() + 4 * i, &o, 4);
        }
        return Answer(Ret(r));
    }
    if (f == "PM_SlideMove") {  // pm, pml, gravity, answers, log
        Bytes* b = args.buffer(0);
        Bytes* l = args.buffer(1);
        Bytes* log = args.buffer(4);
        if (!b || !l || !log) return Answer::fail("NULL buffer");
        auto pml = from_bytes<pm::MoveLocal>(*l);
        bool gravity = args.i32(2) != 0;
        CheckWorld world(args.bytes(3));
        bool r = false;
        if (!with_pm(*b, [&](pm::Pmove& m) { r = iw_pc::game::slide::slide_move(m, pml, gravity, world); }))
            return Answer::fail("short pm");
        back(*l, pml);
        *log = world.log.out;
        return integer(r ? 1 : 0);
    }
    if (f == "PM_AddEvent") {  // ps, event
        Bytes* b = args.buffer(0);
        if (!b) return Answer::fail("NULL ps");
        auto ps = from_bytes<pm::PlayerState>(*b);
        pm::add_event(ps, args.i32(1));
        back(*b, ps);
        return nothing();
    }
    if (f == "PM_FootstepType") {  // ps, pml
        Bytes* b = args.buffer(0);
        Bytes* l = args.buffer(1);
        if (!b || !l) return Answer::fail("NULL buffer");
        return integer(pm::footstep_type(from_bytes<pm::PlayerState>(*b), from_bytes<pm::MoveLocal>(*l)));
    }
    if (f == "PM_FootstepEvent") {  // pm, pml, old bob cycle, new, footstep, answers, log
        Bytes* b = args.buffer(0);
        Bytes* l = args.buffer(1);
        Bytes* log = args.buffer(6);
        if (!b || !l || !log) return Answer::fail("NULL buffer");
        auto pml = from_bytes<pm::MoveLocal>(*l);
        CheckWorld world(args.bytes(5));
        std::int32_t old_bob = args.i32(2), new_bob = args.i32(3);
        bool footstep = args.i32(4) != 0;
        if (!with_pm(*b, [&](pm::Pmove& m) { pm::footstep_event(m, pml, old_bob, new_bob, footstep, world); }))
            return Answer::fail("short pm");
        *log = world.log.out;
        return nothing();
    }
    if (f == "AngleVectors") {  // angles, forward, right, up
        Bytes* a = args.buffer(0);
        if (!a) return Answer::fail("NULL angles");
        std::array<std::optional<std::array<float, 3>>, 3> outs;
        for (std::size_t k = 0; k < 3; ++k)
            if (Bytes* b = args.buffer(1 + k)) outs[k] = floats<3>(*b);
        iw_pc::shared::math::angle_vectors(floats<3>(*a), outs[0] ? &*outs[0] : nullptr, outs[1] ? &*outs[1] : nullptr,
                                           outs[2] ? &*outs[2] : nullptr);
        for (std::size_t k = 0; k < 3; ++k)
            if (Bytes* b = args.buffer(1 + k)) put_floats<3>(*b, *outs[k]);
        return nothing();
    }
    if (f == "Vec3Lerp") {  // from, to, out, fraction
        Bytes* a = args.buffer(0);
        Bytes* b = args.buffer(1);
        Bytes* o = args.buffer(2);
        if (!a || !b || !o) return Answer::fail("NULL vector");
        put_floats<3>(*o, iw_pc::shared::math::vec3_lerp(floats<3>(*a), floats<3>(*b), args.f32(3)));
        return nothing();
    }
    if (f == "vectopitch") {
        Bytes* v = args.buffer(0);
        if (!v) return Answer::fail("NULL v");
        return Answer(Ret(iw_pc::shared::math::vectopitch(floats<3>(*v))));
    }
    if (f == "AngleSubtract") return Answer(Ret(iw_pc::shared::math::angle_subtract(args.f32(0), args.f32(1))));
    if (f == "AngleWrap360") return Answer(Ret(iw_pc::shared::math::angle_wrap360(args.f32(0))));
    if (f == "AngleWrap180") return Answer(Ret(iw_pc::shared::math::angle_wrap180(args.f32(0))));
    if (f == "AngleDelta") return Answer(Ret(iw_pc::shared::math::angle_delta(args.f32(0), args.f32(1))));
    if (f == "AddLeanToPosition") {  // position, yaw, leanFrac, roll, leanDist
        Bytes* b = args.buffer(0);
        if (!b) return Answer::fail("NULL position");
        auto position = floats<3>(*b);
        iw_pc::shared::math::add_lean_to_position(position, args.f32(1), args.f32(2), args.f32(3), args.f32(4));
        put_floats(*b, position);
        return nothing();
    }
    if (f == "PitchForYawOnNormal") {  // yaw, normal
        Bytes* b = args.buffer(1);
        if (!b) return Answer::fail("NULL normal");
        return Answer(Ret(iw_pc::shared::math::pitch_for_yaw_on_normal(args.f32(0), floats<3>(*b))));
    }
    if (f == "BG_CheckProne") {  // the 14 arguments, answers, log
        namespace pr = iw_pc::game::prone;
        pr::ProneQuery q;
        q.pass_entity = args.i32(0);
        Bytes* origin = args.buffer(1);
        Bytes* log = args.buffer(15);
        if (!origin || !log) return Answer::fail("NULL buffer");
        q.origin = floats<3>(*origin);
        q.size = args.f32(2);
        q.height = args.f32(3);
        q.yaw = args.f32(4);
        q.already_prone = args.i32(8) != 0;
        q.on_ground = args.i32(9) != 0;
        if (Bytes* n = args.buffer(10)) q.ground_normal = floats<3>(*n);
        q.handler = static_cast<std::uint8_t>(args.i32(11));
        q.check_type = args.i32(12);
        q.feet_distance = args.f32(13);
        std::array<std::optional<float>, 3> values;
        for (std::size_t k = 0; k < 3; ++k)
            if (Bytes* b = args.buffer(5 + k)) values[k] = floats<1>(*b)[0];
        pr::ProneAngles out{values[0] ? &*values[0] : nullptr, values[1] ? &*values[1] : nullptr,
                            values[2] ? &*values[2] : nullptr};
        CheckWorld world(args.bytes(14));
        bool r = pr::check_prone(q, out, world);
        for (std::size_t k = 0; k < 3; ++k)
            if (Bytes* b = args.buffer(5 + k)) put_floats<1>(*b, {*values[k]});
        *log = world.log.out;
        return integer(r ? 1 : 0);
    }
    if (f == "PM_VerifyPronePosition") {  // pm, start origin, start velocity, answers, log
        Bytes* b = args.buffer(0);
        Bytes* o = args.buffer(1);
        Bytes* v = args.buffer(2);
        Bytes* log = args.buffer(4);
        if (!b || !o || !v || !log) return Answer::fail("NULL buffer");
        CheckWorld world(args.bytes(3));
        bool r = false;
        auto origin = floats<3>(*o), velocity = floats<3>(*v);
        if (!with_pm(*b, [&](pm::Pmove& m) { r = iw_pc::game::prone::verify_prone_position(m, origin, velocity, world); }))
            return Answer::fail("short pm");
        *log = world.log.out;
        return integer(r ? 1 : 0);
    }
    if (f == "PM_StepSlideMove") {  // pm, pml, gravity, jump_height, jump_stepSize, player_footstepsThreshhold, answers, log
        Bytes* b = args.buffer(0);
        Bytes* l = args.buffer(1);
        Bytes* log = args.buffer(7);
        if (!b || !l || !log) return Answer::fail("NULL buffer");
        auto pml = from_bytes<pm::MoveLocal>(*l);
        pm::MoveSettings settings;
        settings.jump_height = args.f32(3);
        settings.jump_step_size = args.f32(4);
        settings.footsteps_threshold = args.f32(5);
        bool gravity = args.i32(2) != 0;
        CheckWorld world(args.bytes(6));
        if (!with_pm(*b, [&](pm::Pmove& m) { iw_pc::game::slide::step_slide_move(m, pml, gravity, settings, world); }))
            return Answer::fail("short pm");
        back(*l, pml);
        *log = world.log.out;
        return nothing();
    }
    if (f == "PM_AirMove") {  // pm, pml, the move's dvars, answers, log
        constexpr std::size_t k = 2 + kMoveDvarCount;
        Bytes* b = args.buffer(0);
        Bytes* l = args.buffer(1);
        Bytes* log = args.buffer(k + 1);
        if (!b || !l || !log) return Answer::fail("NULL buffer");
        auto pml = from_bytes<pm::MoveLocal>(*l);
        pm::MoveSettings settings = move_settings(args, 2);
        CheckWorld world(args.bytes(k));
        if (!with_pm(*b, [&](pm::Pmove& m) { iw_pc::game::moves::air_move(m, pml, settings, world); }))
            return Answer::fail("short pm");
        back(*l, pml);
        *log = world.log.out;
        return nothing();
    }
    if (f == "PM_DeadMove") {  // pml, ps
        Bytes* l = args.buffer(0);
        Bytes* b = args.buffer(1);
        if (!l || !b) return Answer::fail("NULL buffer");
        auto pml = from_bytes<pm::MoveLocal>(*l);
        auto ps = from_bytes<pm::PlayerState>(*b);
        iw_pc::game::moves::dead_move(ps, pml);
        back(*b, ps);
        return nothing();
    }
    if (f == "PM_FlyMove" || f == "PM_LadderMove") {  // pm, pml, the move's dvars, (scripts,) answers, log
        constexpr std::size_t k = 2 + kMoveDvarCount;
        bool ladder = f == "PM_LadderMove";
        std::size_t answers = ladder ? k + 1 : k;
        Bytes* b = args.buffer(0);
        Bytes* l = args.buffer(1);
        Bytes* log = args.buffer(answers + 1);
        if (!b || !l || !log) return Answer::fail("NULL buffer");
        auto pml = from_bytes<pm::MoveLocal>(*l);
        pm::MoveSettings settings = move_settings(args, 2);
        CheckWorld world(args.bytes(answers));
        if (ladder) world.scripts(args.bytes(k));
        if (!with_pm(*b, [&](pm::Pmove& m) {
                if (ladder) iw_pc::game::moves::ladder_move(m, pml, settings, world);
                else iw_pc::game::moves::fly_move(m, pml, settings, world);
            }))
            return Answer::fail("short pm");
        back(*l, pml);
        *log = world.log.out;
        return nothing();
    }
    if (f == "PM_WalkMove") {  // pm, pml, the move's dvars, weapons, scripts, answers, log
        constexpr std::size_t k = 2 + kMoveDvarCount;
        Bytes* b = args.buffer(0);
        Bytes* l = args.buffer(1);
        Bytes* log = args.buffer(k + 3);
        if (!b || !l || !log) return Answer::fail("NULL buffer");
        auto pml = from_bytes<pm::MoveLocal>(*l);
        pm::MoveSettings settings = move_settings(args, 2);
        CheckWorld world(args.bytes(k + 2));
        world.weapons(args.bytes(k)).scripts(args.bytes(k + 1));
        if (!with_pm(*b, [&](pm::Pmove& m) { iw_pc::game::moves::walk_move(m, pml, settings, world); }))
            return Answer::fail("short pm");
        back(*l, pml);
        *log = world.log.out;
        return nothing();
    }
    if (f == "PM_GetEffectiveStance") {  // ps
        Bytes* b = args.buffer(0);
        if (!b) return Answer::fail("NULL ps");
        return integer(pm::effective_stance(from_bytes<pm::PlayerState>(*b)));
    }
    if (f == "PM_GetViewHeightLerp") {  // pm, from, to
        Bytes* b = args.buffer(0);
        if (!b) return Answer::fail("NULL pm");
        float r = 0.0f;
        if (!with_pm(*b, [&](pm::Pmove& m) { r = pm::view_height_lerp(m, args.i32(1), args.i32(2)).store(); }))
            return Answer::fail("short pm");
        return Answer(Ret(r));
    }
    if (f == "PM_DamageScale_Walk") {  // damage timer, player_dmgtimer_maxTime, player_dmgtimer_minScale
        pm::MoveSettings settings;
        settings.dmg_timer_max_time = args.f32(1);
        settings.dmg_timer_min_scale = args.f32(2);
        return Answer(Ret(pm::damage_scale_walk(args.i32(0), settings).store()));
    }
    if (f == "PM_CmdScale_Walk") {  // pm, cmd, player_backSpeedScale, player_strafeSpeedScale, weapons
        Bytes* b = args.buffer(0);
        Bytes* c = args.buffer(1);
        if (!b || !c || c->size() < pm::kUsercmdSize) return Answer::fail("NULL buffer");
        pm::MoveSettings settings;
        settings.back_speed_scale = args.f32(2);
        settings.strafe_speed_scale = args.f32(3);
        CheckWorld world({});
        world.weapons(args.bytes(4));
        Bytes cmd = *c;
        float r = 0.0f;
        if (!with_pm(*b, [&](pm::Pmove& m) { r = pm::cmd_scale_walk(m, cmd.data(), settings, world).store(); }))
            return Answer::fail("short pm");
        return Answer(Ret(r));
    }
    if (f == "Jump_ApplySlowdown" || f == "Jump_GetLandFactor") {  // ps, jump_slowdownEnable
        Bytes* b = args.buffer(0);
        if (!b) return Answer::fail("NULL ps");
        pm::MoveSettings settings;
        settings.jump_slowdown_enable = flag(args, 1);
        auto ps = from_bytes<pm::PlayerState>(*b);
        if (f == "Jump_GetLandFactor") return Answer(Ret(iw_pc::game::jump::jump_get_land_factor(ps, settings).store()));
        iw_pc::game::jump::jump_apply_slowdown(ps, settings);
        back(*b, ps);
        return nothing();
    }
    if (f == "Jump_Start") {  // pm, pml, height, jump_slowdownEnable, jump_spreadAdd
        Bytes* b = args.buffer(0);
        Bytes* l = args.buffer(1);
        if (!b || !l) return Answer::fail("NULL buffer");
        pm::MoveSettings settings;
        settings.jump_slowdown_enable = flag(args, 3);
        settings.jump_spread_add = args.f32(4);
        float height = args.f32(2);
        auto pml = from_bytes<pm::MoveLocal>(*l);
        if (!with_pm(*b, [&](pm::Pmove& m) { iw_pc::game::jump::jump_start(m, pml, height, settings); }))
            return Answer::fail("short pm");
        back(*l, pml);
        return nothing();
    }
    if (f == "Jump_AddSurfaceEvent" || f == "Jump_PushOffLadder") {  // ps, pml (, jump_ladderPushVel)
        Bytes* b = args.buffer(0);
        Bytes* l = args.buffer(1);
        if (!b || !l) return Answer::fail("NULL buffer");
        auto ps = from_bytes<pm::PlayerState>(*b);
        auto pml = from_bytes<pm::MoveLocal>(*l);
        if (f == "Jump_AddSurfaceEvent") {
            iw_pc::game::jump::jump_add_surface_event(ps, pml);
        } else {
            pm::MoveSettings settings;
            settings.jump_ladder_push_vel = args.f32(2);
            iw_pc::game::jump::jump_push_off_ladder(ps, pml, settings);
        }
        back(*b, ps);
        return nothing();
    }
    if (f == "Jump_Check") {  // pm, pml, jump_slowdownEnable, jump_height, jump_spreadAdd, jump_ladderPushVel, scripts, log
        Bytes* b = args.buffer(0);
        Bytes* l = args.buffer(1);
        Bytes* log = args.buffer(7);
        if (!b || !l || !log) return Answer::fail("NULL buffer");
        pm::MoveSettings settings;
        settings.jump_slowdown_enable = flag(args, 2);
        settings.jump_height = args.f32(3);
        settings.jump_spread_add = args.f32(4);
        settings.jump_ladder_push_vel = args.f32(5);
        CheckWorld world({});
        world.scripts(args.bytes(6));
        auto pml = from_bytes<pm::MoveLocal>(*l);
        bool r = false;
        if (!with_pm(*b, [&](pm::Pmove& m) { r = iw_pc::game::jump::jump_check(m, pml, settings, world); }))
            return Answer::fail("short pm");
        back(*l, pml);
        *log = world.log.out;
        return integer(r ? 1 : 0);
    }
    if (f == "RoundFloatToInt") return integer(iw_pc::shared::math::round_float_to_int(args.f32(0)));
    if (f == "BG_CheckProneTurned") {  // ps, newProneYaw, handler, answers, log
        Bytes* b = args.buffer(0);
        Bytes* log = args.buffer(4);
        if (!b || !log) return Answer::fail("NULL buffer");
        CheckWorld world(args.bytes(3));
        auto ps = from_bytes<pm::PlayerState>(*b);
        bool r = iw_pc::game::prone::check_prone_turned(ps, args.f32(1), static_cast<std::uint8_t>(args.i32(2)), world);
        back(*b, ps);
        *log = world.log.out;
        return integer(r ? 1 : 0);
    }
    if (f == "PM_UpdateLean") {  // ps, msec, cmd, capsuleTrace (the trace function's address), answers, log
        Bytes* b = args.buffer(0);
        Bytes* c = args.buffer(2);
        Bytes* log = args.buffer(5);
        if (!b || !c || !log || c->size() < pm::kUsercmdSize) return Answer::fail("NULL buffer");
        int handler = trace_handler(args.i32(3));
        if (handler < 0) return Answer::fail("no trace function there");
        CheckWorld world(args.bytes(4));
        auto ps = from_bytes<pm::PlayerState>(*b);
        Bytes cmd = *c;
        iw_pc::game::view::update_lean(ps, args.f32(1), cmd.data(), static_cast<std::uint8_t>(handler), world);
        back(*b, ps);
        *log = world.log.out;
        return nothing();
    }
    if (f == "PM_UpdateViewAngles") {  // ps, msec, cmd, handler, the view's six dvars, answers, log
        Bytes* b = args.buffer(0);
        Bytes* c = args.buffer(2);
        Bytes* log = args.buffer(11);
        if (!b || !c || !log || c->size() < pm::kUsercmdSize) return Answer::fail("NULL buffer");
        pm::MoveSettings settings;
        view_settings(args, 4, settings);
        CheckWorld world(args.bytes(10));
        auto ps = from_bytes<pm::PlayerState>(*b);
        Bytes cmd = *c;
        iw_pc::game::view::update_view_angles(ps, args.f32(1), cmd.data(), static_cast<std::uint8_t>(args.i32(3)), settings, world);
        back(*b, ps);
        *log = world.log.out;
        return nothing();
    }
    if (f == "Mantle_CapView") {  // ps, mantle_enable, mantle_view_yawcap
        Bytes* b = args.buffer(0);
        if (!b) return Answer::fail("NULL ps");
        iw_pc::game::mantle::MantleSettings settings;
        settings.enable = flag(args, 1);
        settings.view_yaw_cap = args.f32(2);
        auto ps = from_bytes<pm::PlayerState>(*b);
        iw_pc::game::mantle::mantle_cap_view(ps, settings);
        back(*b, ps);
        return nothing();
    }
    namespace gr = iw_pc::game::ground;
    if (f == "BG_AddPredictableEventToPlayerstate") {  // event, eventParm, ps
        Bytes* b = args.buffer(2);
        if (!b) return Answer::fail("NULL ps");
        auto ps = from_bytes<pm::PlayerState>(*b);
        pm::add_predictable_event(args.i32(0), args.i32(1), ps);
        back(*b, ps);
        return nothing();
    }
    if (f == "PM_MediumLandingForSurface" || f == "PM_HardLandingForSurface") {  // pml
        Bytes* l = args.buffer(0);
        if (!l) return Answer::fail("NULL pml");
        auto pml = from_bytes<pm::MoveLocal>(*l);
        return integer(f == "PM_MediumLandingForSurface" ? gr::medium_landing_for_surface(pml)
                                                         : gr::hard_landing_for_surface(pml));
    }
    if (f == "PM_CrashLand") {  // ps, pml, bg_fallDamageMinHeight, bg_fallDamageMaxHeight, scripts, log
        Bytes* b = args.buffer(0);
        Bytes* l = args.buffer(1);
        Bytes* log = args.buffer(5);
        if (!b || !l || !log) return Answer::fail("NULL buffer");
        auto pml = from_bytes<pm::MoveLocal>(*l);
        auto ps = from_bytes<pm::PlayerState>(*b);
        CheckWorld world({});
        world.scripts(args.bytes(4));
        gr::crash_land(ps, pml, fall_settings(args, 2), world);
        back(*b, ps);
        *log = world.log.out;
        return nothing();
    }
    if (f == "PM_CorrectAllSolid") {  // pm, pml, trace, answers, log
        Bytes* b = args.buffer(0);
        Bytes* l = args.buffer(1);
        Bytes* t = args.buffer(2);
        Bytes* log = args.buffer(4);
        if (!b || !l || !t || !log) return Answer::fail("NULL buffer");
        auto pml = from_bytes<pm::MoveLocal>(*l);
        iw_pc::game::trace::Trace trace;
        std::copy(t->begin(), t->begin() + static_cast<std::ptrdiff_t>(std::min(t->size(), trace.bytes.size())),
                  trace.bytes.begin());
        CheckWorld world(args.bytes(3));
        bool r = false;
        if (!with_pm(*b, [&](pm::Pmove& m) { r = gr::correct_all_solid(m, pml, trace, world); }))
            return Answer::fail("short pm");
        back(*l, pml);
        std::copy(trace.bytes.begin(), trace.bytes.begin() + static_cast<std::ptrdiff_t>(std::min(t->size(), trace.bytes.size())),
                  t->begin());
        *log = world.log.out;
        return integer(r ? 1 : 0);
    }
    if (f == "PM_GroundTraceMissed" || f == "PM_GroundTrace") {
        // pm, pml, [bg_fallDamageMinHeight, bg_fallDamageMaxHeight,] scripts, answers, log
        bool full = f == "PM_GroundTrace";
        std::size_t k = full ? 4 : 2;
        Bytes* b = args.buffer(0);
        Bytes* l = args.buffer(1);
        Bytes* log = args.buffer(k + 2);
        if (!b || !l || !log) return Answer::fail("NULL buffer");
        auto pml = from_bytes<pm::MoveLocal>(*l);
        pm::MoveSettings settings = full ? fall_settings(args, 2) : pm::MoveSettings{};
        CheckWorld world(args.bytes(k + 1));
        world.scripts(args.bytes(k));
        if (!with_pm(*b, [&](pm::Pmove& m) {
                if (full) gr::ground_trace(m, pml, settings, world);
                else gr::ground_trace_missed(m, pml, world);
            }))
            return Answer::fail("short pm");
        back(*l, pml);
        *log = world.log.out;
        return nothing();
    }
    namespace dk = iw_pc::game::duck;
    if (f == "Jump_ActivateSlowdown" || f == "PM_ExitAimDownSight") {  // ps
        Bytes* b = args.buffer(0);
        if (!b) return Answer::fail("NULL ps");
        auto ps = from_bytes<pm::PlayerState>(*b);
        if (f == "Jump_ActivateSlowdown") iw_pc::game::jump::jump_activate_slowdown(ps);
        else pm::exit_aim_down_sight(ps);
        back(*b, ps);
        return nothing();
    }
    if (f == "PM_GetViewHeightLerpTime") return integer(dk::view_height_lerp_time(args.i32(0), args.i32(1)));
    if (f == "PM_ViewHeightTableLerp") {  // fraction, table (one of the game's, by address), the offset out
        Bytes* out = args.buffer(2);
        if (!out) return Answer::fail("NULL offset");
        std::span<const dk::ViewLerpWaypoint> table;
        switch (static_cast<std::uint32_t>(args.i32(1))) {
        case 0x005DB630u: table = dk::kStandToCrouch; break;
        case 0x005DB6A0u: table = dk::kCrouchToStand; break;
        case 0x005DB710u: table = dk::kCrouchToProne; break;
        case 0x005DB7B8u: table = dk::kProneToCrouch; break;
        default: return Answer::fail("no view height table there");
        }
        auto [height, offset] = dk::view_height_table_lerp(args.i32(0), table);
        put_floats<1>(*out, {offset});
        return Answer(Ret(height.store()));
    }
    if (f == "PM_ViewHeightAdjust" || f == "PM_CheckDuck") {
        // pm, pml, jump_height, jump_stepSize, player_footstepsThreshhold, [scripts,] answers, log
        bool duck = f == "PM_CheckDuck";
        std::size_t k = duck ? 6 : 5;
        Bytes* b = args.buffer(0);
        Bytes* l = args.buffer(1);
        Bytes* log = args.buffer(k + 1);
        if (!b || !l || !log) return Answer::fail("NULL buffer");
        auto pml = from_bytes<pm::MoveLocal>(*l);
        pm::MoveSettings settings = step_settings(args, 2);
        CheckWorld world(args.bytes(k));
        if (duck) world.scripts(args.bytes(5));
        if (!with_pm(*b, [&](pm::Pmove& m) {
                if (duck) dk::check_duck(m, pml, settings, world);
                else dk::view_height_adjust(m, pml, settings, world);
            }))
            return Answer::fail("short pm");
        back(*l, pml);
        *log = world.log.out;
        return nothing();
    }
    if (f == "SnapVector") {  // v
        Bytes* v = args.buffer(0);
        if (!v) return Answer::fail("NULL vector");
        auto a = floats<3>(*v);
        iw_pc::shared::math::snap_vector(a);
        put_floats(*v, a);
        return nothing();
    }
    if (f == "PM_NoclipMove" || f == "PM_UFOMove") {  // pm, pml, the move's dvars
        Bytes* b = args.buffer(0);
        Bytes* l = args.buffer(1);
        if (!b || !l) return Answer::fail("NULL buffer");
        auto pml = from_bytes<pm::MoveLocal>(*l);
        pm::MoveSettings settings = move_settings(args, 2);
        bool ufo = f == "PM_UFOMove";
        if (!with_pm(*b, [&](pm::Pmove& m) {
                if (ufo) iw_pc::game::moves::ufo_move(m, pml, settings);
                else iw_pc::game::moves::noclip_move(m, pml, settings);
            }))
            return Answer::fail("short pm");
        return nothing();
    }
    if (f == "PM_FoliageSounds") {  // pm, the five foliage dvars, answers, log
        Bytes* b = args.buffer(0);
        Bytes* log = args.buffer(7);
        if (!b || !log) return Answer::fail("NULL buffer");
        pm::MoveSettings settings;
        settings.foliage_min_speed = args.f32(1);
        settings.foliage_max_speed = args.f32(2);
        settings.foliage_slow_interval = args.i32(3);
        settings.foliage_fast_interval = args.i32(4);
        settings.foliage_reset_interval = args.i32(5);
        CheckWorld world(args.bytes(6));
        if (!with_pm(*b, [&](pm::Pmove& m) { pm::foliage_sounds(m, settings, world); })) return Answer::fail("short pm");
        *log = world.log.out;
        return nothing();
    }
    if (f == "PM_UpdateAdsSlowdown") {  // pm
        Bytes* b = args.buffer(0);
        if (!b) return Answer::fail("NULL pm");
        if (!with_pm(*b, [&](pm::Pmove& m) { pm::update_ads_slowdown(m); })) return Answer::fail("short pm");
        return nothing();
    }
    namespace wp = iw_pc::game::weapon;
    namespace an = iw_pc::game::anim;
    if (f == "BG_UpdateConditionValue") {  // client, condition, value, checkConversion, the client's animation state
        Bytes* a = args.buffer(4);
        if (!a) return Answer::fail("NULL animation state");
        auto info = from_bytes<an::ClientAnimInfo>(*a);
        an::update_condition_value(info, args.i32(1), args.i32(2), args.i32(3) != 0);
        back(*a, info);
        return nothing();
    }
    if (f == "BG_AnimUpdatePlayerStateConditions") {  // pm, definitions, the player's animation state
        Bytes* b = args.buffer(0);
        Bytes* a = args.buffer(2);
        if (!b || !a) return Answer::fail("NULL buffer");
        CheckWorld world({});
        world.defs(args.bytes(1)).anim_info(*a);
        if (!with_pm(*b, [&](pm::Pmove& m) { an::update_player_state_conditions(m, world); }))
            return Answer::fail("short pm");
        back(*a, world.anim_info());
        return nothing();
    }
    if (f == "BG_GetBinocularWeaponIndex") {  // definitions
        CheckWorld world({});
        world.defs(args.bytes(0));
        return integer(wp::binocular_weapon_index(world));
    }
    if (f == "PM_IsBinocularsUp") return integer(wp::binoculars_up(args.i32(0)) ? 1 : 0);
    if (f == "PM_IsAdsAllowed") {  // ps, pml, definitions
        Bytes* b = args.buffer(0);
        Bytes* l = args.buffer(1);
        if (!b || !l) return Answer::fail("NULL buffer");
        auto ps = from_bytes<pm::PlayerState>(*b);
        auto pml = from_bytes<pm::MoveLocal>(*l);
        CheckWorld world({});
        world.defs(args.bytes(2));
        return integer(wp::is_ads_allowed(ps, pml, world) ? 1 : 0);
    }
    if (f == "PM_UpdateAimDownSightFlag") {  // pm, pml, definitions, the player's animation state
        Bytes* b = args.buffer(0);
        Bytes* l = args.buffer(1);
        Bytes* a = args.buffer(3);
        if (!b || !l || !a) return Answer::fail("NULL buffer");
        auto pml = from_bytes<pm::MoveLocal>(*l);
        CheckWorld world({});
        world.defs(args.bytes(2)).anim_info(*a);
        if (!with_pm(*b, [&](pm::Pmove& m) { wp::update_aim_down_sight_flag(m, pml, world); }))
            return Answer::fail("short pm");
        back(*a, world.anim_info());
        return nothing();
    }
    if (f == "I_fclamp") return Answer(Ret(iw_pc::shared::math::i_fclamp(args.f32(0), args.f32(1), args.f32(2))));
    if (f == "PM_UpdateAimDownSightLerp" || f == "PM_UpdateAimSpreadScale") {
        // pm, pml, (player_scopeExitOnDamage, player_adsExitDelay | bg_aimSpreadMoveSpeedThreshold), definitions
        bool lerp = f == "PM_UpdateAimDownSightLerp";
        Bytes* b = args.buffer(0);
        Bytes* l = args.buffer(1);
        if (!b || !l) return Answer::fail("NULL buffer");
        auto pml = from_bytes<pm::MoveLocal>(*l);
        pm::MoveSettings settings;
        if (lerp) {
            settings.scope_exit_on_damage = flag(args, 2);
            settings.ads_exit_delay = args.i32(3);
        } else {
            settings.aim_spread_move_speed_threshold = args.f32(2);
        }
        CheckWorld world({});
        world.defs(args.bytes(lerp ? 4 : 3));
        if (!with_pm(*b, [&](pm::Pmove& m) {
                if (lerp) wp::update_aim_down_sight_lerp(m, pml, settings, world);
                else wp::update_aim_spread_scale(m, pml, settings, world);
            }))
            return Answer::fail("short pm");
        return nothing();
    }
    namespace ws = iw_pc::game::weapon_state;
    {
        using PsOnly = void (*)(pm::PlayerState&);
        static const std::pair<const char*, PsOnly> ps_only[] = {
            {"PM_Weapon_FinishWeaponRaise", ws::finish_weapon_raise}, {"PM_Weapon_FinishReloadEnd", ws::finish_reload_end},
            {"PM_Weapon_FinishRechamber", ws::finish_rechamber},     {"PM_Weapon_MeleeEnd", ws::melee_end},
            {"PM_Weapon_OffHandEmpty", ws::offhand_empty},           {"PM_ExitBinoculars", ws::exit_binoculars},
            {"PM_ResetWeaponState", ws::reset_weapon_state},         {"PM_Weapon_Idle", ws::weapon_idle}};
        for (const auto& [name, fn] : ps_only)
            if (f == name) {
                Bytes* b = args.buffer(0);
                if (!b) return Answer::fail("NULL ps");
                auto ps = from_bytes<pm::PlayerState>(*b);
                fn(ps);
                back(*b, ps);
                return nothing();
            }
        using PsDefs = void (*)(pm::PlayerState&, const iw_pc::game::trace::MoveHost&);
        static const std::pair<const char*, PsDefs> ps_defs[] = {
            {"PM_Weapon_MeleeFire", ws::melee_fire},           {"PM_Weapon_OffHandInit", ws::offhand_init},
            {"PM_Weapon_OffHandPrepare", ws::offhand_prepare}, {"PM_Weapon_OffHandHold", ws::offhand_hold},
            {"PM_Weapon_OffHandEnd", ws::offhand_end},         {"PM_Binoculars_DropWeapon", ws::binoculars_drop_weapon},
            {"PM_Binoculars_Raise", ws::binoculars_raise}};
        for (const auto& [name, fn] : ps_defs)
            if (f == name) {
                Bytes* b = args.buffer(0);
                if (!b) return Answer::fail("NULL ps");
                auto ps = from_bytes<pm::PlayerState>(*b);
                CheckWorld world({});
                world.defs(args.bytes(1));
                fn(ps, world);
                back(*b, ps);
                return nothing();
            }
    }
    if (f == "PM_Weapon_ResetUnlessBusy") {  // ps
        Bytes* b = args.buffer(0);
        if (!b) return Answer::fail("NULL ps");
        auto ps = from_bytes<pm::PlayerState>(*b);
        std::int32_t r = ws::reset_unless_busy(ps);
        back(*b, ps);
        return integer(r);
    }
    if (f == "PM_WeaponAmmoInClip" || f == "BG_GetFirstOffhandOfClass" || f == "BG_IsWeaponValid") {
        // ps, [class | weapon,] definitions
        Bytes* b = args.buffer(0);
        if (!b) return Answer::fail("NULL ps");
        auto ps = from_bytes<pm::PlayerState>(*b);
        CheckWorld world({});
        if (f == "PM_WeaponAmmoInClip") {
            world.defs(args.bytes(1));
            return integer(ws::ammo_in_clip(ps, world));
        }
        world.defs(args.bytes(2));
        if (f == "BG_GetFirstOffhandOfClass") return integer(ws::first_offhand_of_class(ps, args.i32(1), world));
        return integer(ws::is_weapon_valid(ps, args.i32(1), world) ? 1 : 0);
    }
    if (f == "PM_Binoculars_Lower" || f == "PM_Binoculars_ExitSights") {  // ps, player_adsExitDelay, definitions
        Bytes* b = args.buffer(0);
        if (!b) return Answer::fail("NULL ps");
        auto ps = from_bytes<pm::PlayerState>(*b);
        pm::MoveSettings settings;
        settings.ads_exit_delay = args.i32(1);
        CheckWorld world({});
        world.defs(args.bytes(2));
        if (f == "PM_Binoculars_Lower") ws::binoculars_lower(ps, settings, world);
        else ws::binoculars_exit_sights(ps, settings, world);
        back(*b, ps);
        return nothing();
    }
    if (f == "PM_Binoculars_DropBinoculars") {  // pm, definitions
        Bytes* b = args.buffer(0);
        if (!b) return Answer::fail("NULL pm");
        CheckWorld world({});
        world.defs(args.bytes(1));
        if (!with_pm(*b, [&](pm::Pmove& m) { ws::binoculars_drop(m, world); })) return Answer::fail("short pm");
        return nothing();
    }
    if (f == "PM_StartWeaponAnim" || f == "PM_ContinueWeaponAnim") {  // ps, animation
        Bytes* b = args.buffer(0);
        if (!b) return Answer::fail("NULL ps");
        auto ps = from_bytes<pm::PlayerState>(*b);
        if (f == "PM_StartWeaponAnim") ws::start_weapon_anim(ps, args.i32(1));
        else ws::continue_weapon_anim(ps, args.i32(1));
        back(*b, ps);
        return nothing();
    }
    if (f == "PM_Weapon_FireButtonReleased") {  // pm, delayedAction
        Bytes* b = args.buffer(0);
        if (!b) return Answer::fail("NULL pm");
        std::int32_t r = 0;
        if (!with_pm(*b, [&](pm::Pmove& m) { r = ws::fire_button_released(m, args.i32(1)); }))
            return Answer::fail("short pm");
        return integer(r);
    }
    if (f == "PM_Weapon_GrenadeCookOff") {  // ps, pml, definitions
        Bytes* b = args.buffer(0);
        Bytes* l = args.buffer(1);
        if (!b || !l) return Answer::fail("NULL buffer");
        auto ps = from_bytes<pm::PlayerState>(*b);
        auto pml = from_bytes<pm::MoveLocal>(*l);
        CheckWorld world({});
        world.defs(args.bytes(2));
        bool r = ws::grenade_cook_off(ps, pml, world);
        back(*b, ps);
        return integer(r ? 1 : 0);
    }
    if (f == "DiffTrack")
        return Answer(Ret(iw_pc::shared::math::diff_track(args.f32(0), args.f32(1), args.f32(2), args.f32(3))));
    if (f == "PM_UpdateHoldBreath") {  // pm, pml, the five breath dvars, definitions
        Bytes* b = args.buffer(0);
        Bytes* l = args.buffer(1);
        if (!b || !l) return Answer::fail("NULL buffer");
        auto pml = from_bytes<pm::MoveLocal>(*l);
        pm::MoveSettings settings;
        settings.breath_hold_time = args.f32(2);
        settings.breath_gasp_time = args.f32(3);
        settings.breath_gasp_scale = args.f32(4);
        settings.breath_hold_lerp = args.f32(5);
        settings.breath_gasp_lerp = args.f32(6);
        CheckWorld world({});
        world.defs(args.bytes(7));
        if (!with_pm(*b, [&](pm::Pmove& m) { wp::update_hold_breath(m, pml, settings, world); }))
            return Answer::fail("short pm");
        return nothing();
    }
    if (f == "PM_UpdatePronePitch") {  // pm, pml, answers, log
        Bytes* b = args.buffer(0);
        Bytes* l = args.buffer(1);
        Bytes* log = args.buffer(3);
        if (!b || !l || !log) return Answer::fail("NULL buffer");
        auto pml = from_bytes<pm::MoveLocal>(*l);
        CheckWorld world(args.bytes(2));
        if (!with_pm(*b, [&](pm::Pmove& m) { iw_pc::game::prone::update_prone_pitch(m, pml, world); }))
            return Answer::fail("short pm");
        *log = world.log.out;
        return nothing();
    }
    if (f == "PM_LeaveLadder") {  // ps
        Bytes* b = args.buffer(0);
        if (!b) return Answer::fail("NULL ps");
        auto ps = from_bytes<pm::PlayerState>(*b);
        iw_pc::game::moves::leave_ladder(ps);
        back(*b, ps);
        return nothing();
    }
    if (f == "PM_CheckLadderMove") {  // pm, pml, scripts, answers, log
        Bytes* b = args.buffer(0);
        Bytes* l = args.buffer(1);
        Bytes* log = args.buffer(4);
        if (!b || !l || !log) return Answer::fail("NULL buffer");
        auto pml = from_bytes<pm::MoveLocal>(*l);
        CheckWorld world(args.bytes(3));
        world.scripts(args.bytes(2));
        if (!with_pm(*b, [&](pm::Pmove& m) { iw_pc::game::moves::check_ladder_move(m, pml, world); }))
            return Answer::fail("short pm");
        back(*l, pml);
        *log = world.log.out;
        return nothing();
    }
    return std::nullopt;
}

}  // namespace pc_check
