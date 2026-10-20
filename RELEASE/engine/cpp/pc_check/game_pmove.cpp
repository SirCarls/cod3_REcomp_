// Requests for iw_pc::game::pmove and iw_pc::shared::math (the C++ version; Call of Duty 2;
// specs/game_pmove.py): the player state and the move's scratch state arrive as buffers of
// their bytes, the settings as their values.

#include <algorithm>
#include <cstring>
#include <optional>

#include "iw_pc/game/moves.h"
#include "iw_pc/game/pmove.h"
#include "iw_pc/game/prone.h"
#include "iw_pc/game/slide.h"
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

/// The check's world: each trace is logged as the original's stand-ins record it and answered with
/// the case's next result (nothing hit once they run out).
class CheckWorld : public iw_pc::game::trace::MoveHost {
public:
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
};

/// The settings a whole move reads, from the ten dvar arguments at first (specs/game_pmove.py MOVE_DVARS).
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
    if (f == "PM_AirMove") {  // pm, pml, the move's ten dvars, answers, log
        Bytes* b = args.buffer(0);
        Bytes* l = args.buffer(1);
        Bytes* log = args.buffer(13);
        if (!b || !l || !log) return Answer::fail("NULL buffer");
        auto pml = from_bytes<pm::MoveLocal>(*l);
        pm::MoveSettings settings = move_settings(args, 2);
        CheckWorld world(args.bytes(12));
        if (!with_pm(*b, [&](pm::Pmove& m) { iw_pc::game::moves::air_move(m, pml, settings, world); }))
            return Answer::fail("short pm");
        back(*l, pml);
        *log = world.log.out;
        return nothing();
    }
    if (f == "RoundFloatToInt") return integer(iw_pc::shared::math::round_float_to_int(args.f32(0)));
    return std::nullopt;
}

}  // namespace pc_check
