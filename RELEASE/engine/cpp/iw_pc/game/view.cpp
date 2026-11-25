#include "iw_pc/game/view.h"

#include <cstring>

#include "iw_pc/game/mantle.h"
#include "iw_pc/game/prone.h"
#include "iw_pc/net/msg.h"
#include "iw_pc/shared/math.h"
#include "iw_pc/x87.h"

namespace iw_pc::game::view {

using x87::F24;
namespace pmv = iw_pc::game::pmove;

namespace {
constexpr std::int32_t kLeanMask = 0x02810011;

std::int32_t sign_bit_unit(float x) {
    std::int32_t bits;
    std::memcpy(&bits, &x, 4);
    return bits < 0 ? -1 : 1;
}
}  // namespace

void update_lean(pmv::PlayerState& ps, float msec, const std::uint8_t* cmd, std::uint8_t handler, trace::MoveHost& host) {
    namespace p = pmv::ps;
    auto l = F24::load;
    std::int32_t buttons;
    std::memcpy(&buttons, cmd + pmv::cmd::kButtons, 4);
    std::int32_t leaning = 0;
    std::int32_t kind = ps.pm_type();
    if ((buttons & (pmv::button::kLeanLeft | pmv::button::kLeanRight)) != 0 && (ps.pm_flags() & pmv::pm_flags::kNoLean) == 0 &&
        kind < pmv::pm_type::kDead && (ps.integer(p::kGroundEntityNum) != pmv::kEntityNone || kind == 1)) {
        if ((buttons & pmv::button::kLeanLeft) != 0) leaning = -1;
        if ((buttons & pmv::button::kLeanRight) != 0) leaning += 1;
    }
    if ((ps.integer(p::kEFlags) & 0x300) != 0) leaning = 0;
    float most = pmv::effective_stance(ps) == pmv::stance::kProne ? 0.25f : 0.5f;
    auto step = [&](float k) { return l(msec) * l(k) * l(most); };
    F24 lean = l(ps.real(p::kLeanf));
    F24 zero = l(0.0f);
    if (leaning == 0) {
        if (lean > zero) {
            lean = lean - step(0.0035714286f);
            if (lean < zero) lean = zero;
        } else if (lean < zero) {
            lean = lean + step(0.0035714286f);
            if (lean > zero) lean = zero;
        }
    } else if (leaning > 0) {
        if (lean < l(most)) lean = lean + step(0.0028571428f);
        if (lean > l(most)) lean = l(most);
    } else {
        F24 least = l((-l(most)).store());
        if (lean > least) lean = lean - step(0.0028571428f);
        if (lean < least) lean = least;
    }
    ps.set_real(p::kLeanf, lean.store());
    if (lean.v == 0.0) return;  // (only 0 needs no room; NaN does)
    std::int32_t side = sign_bit_unit(lean.store());
    auto o = ps.vec3(p::kOrigin);
    std::array<float, 3> start{o[0], o[1], (l(o[2]) + l(ps.real(p::kViewHeightCurrent))).store()};
    std::array<float, 3> end = start;
    shared::math::add_lean_to_position(end, ps.real(p::kViewAngles + 4), F24::integer(side).store(), 16.0f, 20.0f);
    trace::Trace t = host.trace(handler, start, {-8.0f, -8.0f, -8.0f}, {8.0f, 8.0f, 8.0f}, end, ps.integer(p::kClientNum), kLeanMask);
    F24 room = l(1.0f) - (l(1.0f) - l(t.fraction())).sqrt();
    float now = ps.real(p::kLeanf);
    if (l(now).abs() > room) ps.set_real(p::kLeanf, (F24::integer(sign_bit_unit(now)) * room).store());  // (NaN isn't cut)
}

namespace {
namespace sm = iw_pc::shared::math;

std::int32_t angle_to_short(float angle) {
    return static_cast<std::int32_t>(net::ftol2((F24::load(angle) * F24::load(182.04445f)).v)) & 0xFFFF;
}

float short_to_angle(std::int16_t s) {
    return (F24::load(F24::integer(s).store()) * F24::load(0.0054931640625f)).store();
}

std::int32_t command_angle(const std::uint8_t* cmd, std::size_t i) {
    std::int32_t v;
    std::memcpy(&v, cmd + pmv::cmd::kAngles + 4 * i, 4);
    return v;
}

std::int32_t add32(std::int32_t a, std::int32_t b) {
    return static_cast<std::int32_t>(static_cast<std::uint32_t>(a) + static_cast<std::uint32_t>(b));
}

std::int16_t view_short(const pmv::PlayerState& ps, const std::uint8_t* cmd, std::size_t i) {
    return static_cast<std::int16_t>(static_cast<std::uint16_t>(ps.integer(pmv::ps::kDeltaAngles + 4 * i)) +
                                     static_cast<std::uint16_t>(command_angle(cmd, i)));
}

void cap_view(pmv::PlayerState& ps, std::size_t i, float delta, float base, float cap, float (*wrap)(float)) {
    auto l = F24::load;
    if (!(delta > cap || -cap > delta)) return;  // (NaN stays)
    float excess = (delta > cap ? l(delta) - l(cap) : l(delta) + l(cap)).store();
    std::size_t at = pmv::ps::kDeltaAngles + 4 * i;
    ps.set_integer(at, add32(ps.integer(at), angle_to_short(excess)));
    F24 edge = excess > 0.0f ? l(base) - l(cap) : l(base) + l(cap);
    ps.set_real(pmv::ps::kViewAngles + 4 * i, wrap(edge.store()));
}

bool prone_fits(const pmv::PlayerState& ps, float yaw, std::uint8_t handler, trace::MoveHost& host) {
    namespace p = pmv::ps;
    prone::ProneQuery q;
    q.pass_entity = ps.integer(p::kClientNum);
    q.origin = ps.vec3(p::kOrigin);
    q.size = ps.real(p::kProneCheckSize);
    q.height = 30.0f;
    q.yaw = yaw;
    q.already_prone = true;
    q.on_ground = ps.integer(p::kGroundEntityNum) != pmv::kEntityNone;
    q.handler = handler;
    q.check_type = 0;
    q.feet_distance = 45.0f;
    return prone::check_prone(q, prone::ProneAngles{}, host);
}

/// The unit step toward delta when more than 1 away, else delta itself (which ends the walk).
float degree_step(float delta, bool& again) {
    again = F24::load(F24::load(delta).abs().store()) > F24::load(1.0f);  // (NaN doesn't)
    if (again) return delta > 0.0f ? 1.0f : -1.0f;
    return delta;
}

void prone_view(pmv::PlayerState& ps, float msec, const std::uint8_t* cmd, std::uint8_t handler, float old_yaw,
                float command_yaw, const pmv::MoveSettings& settings, trace::MoveHost& host) {
    namespace p = pmv::ps;
    auto l = F24::load;
    auto view_yaw = [&] { return ps.real(p::kViewAngles + 4); };
    bool turned = false;
    float delta = sm::angle_normalize180((l(ps.real(p::kProneDirection)) - l(view_yaw())).store());
    float edge = (l(settings.prone_yaw_cap) - l(5.0f)).store();
    bool moving = cmd[pmv::cmd::kForwardMove] != 0 || cmd[pmv::cmd::kRightMove] != 0;
    if (delta > edge || -edge > delta || (moving && delta != 0.0f)) {
        float step = (l(msec) * l(0.055000003f)).store();  // (the game's constant, a bit above 0.055f)
        float direction = ps.real(p::kProneDirection);
        float target = l(delta).abs().store() < step ? view_yaw()
                       : delta > 0.0f              ? (l(direction) - l(step)).store()
                                                   : (l(step) + l(direction)).store();
        bool more = true;
        bool fits = prone::check_prone_turned(ps, target, handler, host);
        while (!fits && more) {
            float back = sm::angle_normalize180((l(ps.real(p::kProneDirection)) - l(target)).store());
            bool again;
            float unit = degree_step(back, again);
            if (!again) turned = true;
            more = again;
            target = sm::angle_wrap360((l(target) + l(unit)).store());
            fits = prone::check_prone_turned(ps, target, handler, host);
        }
        if (fits) {
            if (prone_fits(ps, view_yaw(), handler, host) && prone_fits(ps, target, handler, host))
                ps.set_real(p::kProneDirection, target);
            else
                turned = true;
        }
    }

    delta = sm::angle_normalize180((l(ps.real(p::kProneDirection)) - l(view_yaw())).store());
    if (delta != 0.0f) {  // (NaN goes on)
        float test_yaw = ps.real(p::kProneDirection);
        bool again = true;
        for (;;) {
            bool ok = prone_fits(ps, test_yaw, handler, host);
            if (ok && prone::check_prone_turned(ps, test_yaw, handler, host)) {
                ps.set_real(p::kProneDirection, test_yaw);
                break;
            }
            if (!again) break;
            bool more;
            float unit = degree_step(delta, more);
            again = more;
            turned = true;
            ps.set_integer(p::kDeltaYaw, add32(ps.integer(p::kDeltaYaw), angle_to_short(unit)));
            float view = sm::angle_wrap360((l(unit) + l(view_yaw())).store());
            ps.set_real(p::kViewAngles + 4, view);
            delta = sm::angle_normalize180((l(ps.real(p::kProneDirection)) - l(view)).store());
            if (!ok) test_yaw = sm::angle_wrap360((l(test_yaw) + l(delta)).store());
        }
    }
    cap_view(ps, 1, delta, ps.real(p::kProneDirection), settings.prone_yaw_cap, sm::angle_wrap360);

    if (turned) {
        ps.set_integer(p::kPmFlags, ps.pm_flags() | pmv::pm_flags::kProneViewTurned);
        float back = sm::angle_normalize180((l(old_yaw) - l(view_yaw())).store());
        if (l(l(back).abs().store()) <= l(1.0f)) {  // (NaN doesn't ease)
            float against = sm::angle_delta(command_yaw, view_yaw());
            if (l(against) * l(back) > l(0.0f)) {
                float ease = (l(back) * l(0.98f)).store();
                ps.set_real(p::kViewAngles + 4, sm::angle_wrap360((l(ease) + l(view_yaw())).store()));
                ps.set_integer(p::kDeltaYaw, add32(ps.integer(p::kDeltaYaw), angle_to_short(ease)));
            }
        }
    }

    float base = ps.real(p::kPronePitchBase);
    float pitch = sm::angle_normalize180((l(base) - l(ps.real(p::kViewAngles))).store());
    cap_view(ps, 0, pitch, base, 45.0f, sm::angle_wrap180);
}
}  // namespace

void update_view_angles(pmv::PlayerState& ps, float msec, const std::uint8_t* cmd, std::uint8_t handler,
                        const pmv::MoveSettings& settings, trace::MoveHost& host) {
    namespace p = pmv::ps;
    auto l = F24::load;
    std::int32_t kind = ps.pm_type();
    if (kind == pmv::pm_type::kIntermission) return;
    if (kind >= pmv::pm_type::kDead) {
        if (ps.integer(p::kDeadYaw) == 999) {
            float yaw = short_to_angle(view_short(ps, cmd, 1));
            ps.set_integer(p::kDeadYaw, static_cast<std::int32_t>(net::ftol2(static_cast<double>(yaw))));
        }
        update_lean(ps, msec, cmd, handler, host);
        return;
    }
    float old_yaw = ps.real(p::kViewAngles + 4);
    std::int32_t up = angle_to_short(settings.view_pitch_up);
    std::int32_t down = angle_to_short(settings.view_pitch_down);
    std::int32_t pitch = view_short(ps, cmd, 0);
    if (pitch > down) {
        ps.set_integer(p::kDeltaAngles, static_cast<std::int32_t>(static_cast<std::uint32_t>(down) -
                                                                  static_cast<std::uint32_t>(command_angle(cmd, 0))));
        pitch = down;
    } else if (pitch < -up) {
        ps.set_integer(p::kDeltaAngles, static_cast<std::int32_t>(0u - static_cast<std::uint32_t>(add32(command_angle(cmd, 0), up))));
        pitch = -up;
    }
    ps.set_real(p::kViewAngles, short_to_angle(static_cast<std::int16_t>(pitch)));
    for (std::size_t i = 1; i < 3; ++i) ps.set_real(p::kViewAngles + 4 * i, short_to_angle(view_short(ps, cmd, i)));
    float command_yaw = ps.real(p::kViewAngles + 4);

    if ((ps.integer(p::kEFlags) & 0x300) != 0) {  // a mounted weapon: within its arcs
        for (std::size_t i = 0; i < 2; ++i) {
            float base = ps.real(p::kViewAngleClampBase + 4 * i);
            float view = ps.real(p::kViewAngles + 4 * i);
            std::int32_t s = static_cast<std::int32_t>(net::ftol2(((l(base) - l(view)) * l(182.04445f)).v)) & 0xFFFF;
            float delta = (l(F24::integer(s).store()) * l(0.0054931640625f)).store();
            if (delta > 180.0f) delta = (l(delta) - l(360.0f)).store();
            cap_view(ps, i, delta, base, ps.real(p::kViewAngleClampRange + 4 * i), sm::angle_wrap360);
        }
        return;
    }
    std::int32_t flags = ps.pm_flags();
    if ((flags & pmv::pm_flags::kMantle) != 0) {
        mantle::mantle_cap_view(ps, settings.mantle);
        return;
    }
    if ((flags & pmv::pm_flags::kLadder) != 0 && ps.integer(p::kGroundEntityNum) == pmv::kEntityNone &&
        settings.ladder_yaw_cap != 0.0f) {  // (NaN caps)
        float ladder_yaw = (l(sm::vectoyaw(ps.vec3(p::kLadderVec))) + l(180.0f)).store();
        float delta = sm::angle_normalize180((l(ladder_yaw) - l(ps.real(p::kViewAngles + 4))).store());
        cap_view(ps, 1, delta, ladder_yaw, settings.ladder_yaw_cap, sm::angle_wrap360);
    }
    if ((flags & pmv::pm_flags::kProne) != 0) prone_view(ps, msec, cmd, handler, old_yaw, command_yaw, settings, host);
    if (kind != pmv::pm_type::kNoclip && kind != pmv::pm_type::kUfo && kind != pmv::pm_type::kSpectator)
        update_lean(ps, msec, cmd, handler, host);
}

}  // namespace iw_pc::game::view
