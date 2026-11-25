#include "iw_pc/game/pmove.h"

#include "iw_pc/net/msg.h"
#include "iw_pc/shared/math.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace iw_pc::game::pmove {

using x87::F24;

F24 jump_reduce_friction(PlayerState& ps, const MoveSettings& settings) {
    std::int32_t time = ps.integer(ps::kPmTime);
    if (time > 1800) {
        ps.set_integer(ps::kPmFlags, ps.pm_flags() & ~pm_flags::kJumping);
        ps.set_integer(ps::kJumpOriginZ, 0);
        return F24::load(1.0f);
    }
    if (!settings.jump_slowdown_enable) return F24::load(1.0f);
    if (time >= 1700) return F24::load(2.5f);
    return F24::integer(time) * F24::load(0.00088235294f) + F24::load(1.0f);
}

void friction(PlayerState& ps, const MoveLocal& pml, const MoveSettings& settings) {
    auto v = ps.velocity();
    bool walking = pml.walking();
    F24 vx = F24::load(v[0]), vy = F24::load(v[1]), vz = walking ? F24::load(0.0f) : F24::load(v[2]);
    F24 speed = (vy * vy + vx * vx + vz * vz).sqrt();
    float stored = speed.store();
    if (speed < F24::load(1.0f)) {  // (fcomp 1, jp: NaN goes on)
        ps.set_velocity({0.0f, 0.0f, 0.0f});
        return;
    }
    std::int32_t flags = ps.pm_flags();
    F24 frame_time = F24::load(pml.frame_time());
    F24 drop = F24::load(0.0f);
    if (walking && (pml.integer(pml::kGroundSurfaceFlags) & kSurfSlick) == 0 && (flags & pm_flags::kTimeKnockback) == 0) {
        float control = stored < settings.stop_speed ? settings.stop_speed : stored;
        F24 scaled = F24::load(control);
        if (flags & pm_flags::kSlide) scaled = F24::load(control) * F24::load(0.3f);
        else if (flags & pm_flags::kJumping) scaled = jump_reduce_friction(ps, settings) * F24::load(control);
        drop = F24::load(settings.friction) * frame_time * scaled;
    }
    if (ps.pm_type() == pm_type::kSpectator) drop = drop + F24::load(stored) * frame_time * F24::load(5.0f);
    F24 new_speed = F24::load(stored) - drop;
    if (new_speed < F24::load(0.0f)) new_speed = F24::load(0.0f);  // (NaN stays)
    F24 scale = new_speed / F24::load(stored);
    v = ps.velocity();
    ps.set_velocity({(scale * F24::load(v[0])).store(), (scale * F24::load(v[1])).store(), (scale * F24::load(v[2])).store()});
}

bool inertia_turn_too_sharp(const PlayerState& ps, const std::array<float, 3>& dir, float amount, const MoveSettings& settings) {
    auto v = ps.velocity();
    F24 a = F24::load(amount);
    std::array<float, 2> now{(a * F24::load(dir[0]) + F24::load(v[0])).store(), (a * F24::load(dir[1]) + F24::load(v[1])).store()};
    std::array<float, 2> old{ps.real(ps::kOldVelocity), ps.real(ps::kOldVelocity + 4)};
    shared::math::vec2_normalize(old);
    shared::math::vec2_normalize(now);
    F24 cosine = F24::load(old[0]) * F24::load(now[0]) + F24::load(old[1]) * F24::load(now[1]);
    return !(cosine >= F24::load(settings.inertia_angle));  // (below, or NaN)
}

float clamp_inertia(const PlayerState& ps, float amount, const std::array<float, 3>& dir, const MoveSettings& settings) {
    if (ps.pm_type() == pm_type::kNoclip || settings.inertia_max >= amount) return amount;
    F24 ox = F24::load(ps.real(ps::kOldVelocity)), oy = F24::load(ps.real(ps::kOldVelocity + 4));
    if (ox * ox + oy * oy < F24::dbl(0.0001)) return amount;
    return inertia_turn_too_sharp(ps, dir, amount, settings) ? settings.inertia_max : amount;
}

void accelerate(PlayerState& ps, const MoveLocal& pml, const std::array<float, 3>& wish_dir, float wish_speed, float accel,
                const MoveSettings& settings) {
    auto v = ps.velocity();
    F24 vx = F24::load(v[0]), vy = F24::load(v[1]), vz = F24::load(v[2]);
    F24 dx = F24::load(wish_dir[0]), dy = F24::load(wish_dir[1]), dz = F24::load(wish_dir[2]);
    F24 ws = F24::load(wish_speed), frame_time = F24::load(pml.frame_time()), ac = F24::load(accel);
    if (ps.pm_flags() & pm_flags::kLadder) {
        float wz = (ws * dz).store();
        std::array<float, 3> push{(ws * dx - vx).store(), (ws * dy - vy).store(), (F24::load(wz) - vz).store()};
        float push_len = shared::math::vec3_normalize(push);
        F24 can_push = ws * frame_time * ac;
        if (can_push > F24::load(push_len)) can_push = F24::load(push_len);
        ps.set_velocity({(F24::load(push[0]) * can_push + vx).store(), (F24::load(push[1]) * can_push + vy).store(),
                         (F24::load(push[2]) * can_push + vz).store()});
        return;
    }
    F24 current = vz * dz + vy * dy + dx * vx;
    F24 add = ws - current;
    if (add <= F24::load(0.0f)) return;  // (NaN goes on)
    float control = wish_speed < settings.stop_speed ? settings.stop_speed : wish_speed;
    F24 amount = F24::load(control) * frame_time * ac;
    float stored = (amount > add ? add : amount).store();
    F24 clamped = F24::load(clamp_inertia(ps, stored, wish_dir, settings));
    ps.set_velocity({(clamped * dx + vx).store(), (clamped * dy + vy).store(), (clamped * dz + vz).store()});
}

namespace {

F24 scale_for_state(const PlayerState& ps, F24 scale, const MoveSettings& settings) {
    float lean = ps.real(ps::kLeanf);
    if ((ps.pm_flags() & pm_flags::kSlowed) || lean != 0.0f) scale = scale * F24::load(0.4f);  // (NaN slows too)
    switch (ps.pm_type()) {
        case pm_type::kNoclip: scale = scale * F24::load(3.0f); break;
        case pm_type::kUfo: scale = scale * F24::load(6.0f); break;
        case pm_type::kSpectator: scale = scale * F24::load(settings.spectate_speed_scale); break;
        default: break;
    }
    return scale;
}

}  // namespace

F24 move_scale(const PlayerState& ps, float forward, float right, float up, const MoveSettings& settings) {
    F24 max = F24::load(std::fabs(forward));
    for (float c : {right, up})
        if (F24::load(std::fabs(c)) > max) max = F24::load(std::fabs(c));
    if (max.v == 0.0) return F24::load(0.0f);
    F24 speed = F24::integer(ps.integer(ps::kSpeed)) * max;
    F24 f = F24::load(forward), r = F24::load(right), u = F24::load(up);
    F24 total = (f * f + r * r + u * u).sqrt();
    return scale_for_state(ps, speed / (total * F24::load(127.0f)), settings);
}

F24 cmd_scale(const PlayerState& ps, const std::uint8_t* cmd, const MoveSettings& settings) {
    std::int32_t forward = static_cast<std::int8_t>(cmd[0x18]), right = static_cast<std::int8_t>(cmd[0x19]);
    F24 total = F24::integer(forward * forward + right * right).sqrt();
    std::int32_t max = std::max(std::abs(forward), std::abs(right));
    if (max == 0) return F24::load(0.0f);
    F24 speed = F24::integer(ps.integer(ps::kSpeed)) * F24::integer(max);
    return scale_for_state(ps, speed / (total * F24::load(127.0f)), settings);
}

namespace {
std::int32_t wrap_sub(std::int32_t a, std::int32_t b) {
    return static_cast<std::int32_t>(static_cast<std::uint32_t>(a) - static_cast<std::uint32_t>(b));
}
}  // namespace

std::int32_t effective_stance(const PlayerState& ps) {
    switch (ps.integer(ps::kViewHeightTarget)) {
    case kCrouchViewHeight: return stance::kCrouch;
    case kProneViewHeight: return stance::kProne;
    default: return stance::kStand;
    }
}

F24 view_height_lerp(const Pmove& pm, std::int32_t from, std::int32_t to) {
    const PlayerState& ps = pm.ps;
    std::int32_t start = ps.integer(ps::kViewHeightLerpTime);
    if (start == 0) return F24::load(0.0f);
    std::int32_t target = ps.integer(ps::kViewHeightLerpTarget);
    bool down = ps.integer(ps::kViewHeightLerpDown) != 0;
    if (from != -1 && to != -1) {
        if (to != target) return F24::load(0.0f);
        if (to == kCrouchViewHeight) {
            bool counts = from == kProneViewHeight ? !down : from == kStandViewHeight ? down : false;
            if (!counts) return F24::load(0.0f);
        }
    }
    std::int32_t duration = target == kProneViewHeight ? 400 : target == kCrouchViewHeight && !down ? 400 : 200;
    F24 frac = F24::integer(wrap_sub(pm.server_time(), start)) / F24::integer(duration);
    if (frac < F24::load(0.0f)) return F24::load(0.0f);  // (fcom 0, test ah,5: below)
    if (frac > F24::load(1.0f)) return F24::load(1.0f);
    return frac;
}

F24 damage_scale_walk(std::int32_t damage_timer, const MoveSettings& settings) {
    if (damage_timer == 0) return F24::load(1.0f);
    F24 max_time = F24::load(settings.dmg_timer_max_time);
    if (max_time.v == 0.0) return F24::load(1.0f);  // (fucompp, jp: only equal; NaN goes on)
    return F24::load(-1.0f) / max_time * F24::load(settings.dmg_timer_min_scale) * F24::integer(damage_timer) +
           F24::load(1.0f);
}

namespace {
/// cmd_scale_walk's share for the stance (see the Rust version's stance_scale).
F24 stance_scale(const Pmove& pm, float scale) {
    auto l = F24::load;
    const PlayerState& ps = pm.ps;
    std::int32_t stance = effective_stance(ps);
    F24 lerp = view_height_lerp(pm, kCrouchViewHeight, kProneViewHeight);
    if (lerp.v != 0.0) return ((l(1.0f) - lerp) * l(0.65f) + lerp * l(0.15f)) * l(scale);  // (NaN blends)
    std::int32_t start = ps.integer(ps::kViewHeightLerpTime);
    if (start != 0 && ps.integer(ps::kViewHeightLerpTarget) == kCrouchViewHeight &&
        ps.integer(ps::kViewHeightLerpDown) == 0) {
        F24 frac = F24::integer(wrap_sub(pm.server_time(), start)) * l(0.0025f);
        if (frac >= l(0.0f)) {
            bool blend = true;
            F24 t = frac;
            if (frac > l(1.0f)) t = l(1.0f);
            else if (frac.v == 0.0) blend = false;
            if (blend) return ((l(1.0f) - t) * l(0.15f) + t * l(0.65f)) * l(scale);
        }
    }
    if (stance == stance::kProne) return l(scale) * l(0.15f);
    if (stance == stance::kCrouch) return l(scale) * l(0.65f);
    return l(scale);
}
}  // namespace

F24 cmd_scale_walk(const Pmove& pm, const std::uint8_t* cmd, const MoveSettings& settings, const trace::MoveHost& host) {
    auto l = F24::load;
    const PlayerState& ps = pm.ps;
    std::int32_t forward = static_cast<std::int8_t>(cmd[cmd::kForwardMove]);
    std::int32_t right = static_cast<std::int8_t>(cmd[cmd::kRightMove]);
    F24 total = F24::integer(forward * forward + right * right).sqrt();
    F24 f = (forward < 0 ? F24::integer(forward) * l(settings.back_speed_scale) : F24::integer(forward)).abs();
    F24 r = (F24::integer(right) * l(settings.strafe_speed_scale)).abs();
    F24 diff = f - r;
    F24 max = diff < l(0.0f) || diff.is_nan() ? l(r.store()) : f;  // (test ah,1: below or NaN)
    if (max.v == 0.0) return l(0.0f);
    F24 speed = F24::integer(ps.integer(ps::kSpeed)) * max;
    float scale = (speed / (total * l(127.0f))).store();
    float lean = ps.real(ps::kLeanf);
    if ((ps.pm_flags() & pm_flags::kSlowed) != 0 || lean != 0.0f || std::isnan(lean)) scale = (l(scale) * l(0.4f)).store();
    F24 result = ps.pm_type() == pm_type::kNoclip ? l(scale) * l(3.0f)
                 : ps.pm_type() == pm_type::kUfo  ? l(scale) * l(6.0f)
                                                  : stance_scale(pm, scale);
    std::int32_t weapon = ps.integer(ps::kWeapon);
    if (weapon != 0) {
        float weapon_scale = host.weapon_move_speed_scale(weapon);
        if (weapon_scale > 0.0f) result = result * l(weapon_scale);  // (only above; NaN doesn't)
    }
    std::int32_t buttons;
    std::memcpy(&buttons, cmd + cmd::kButtons, 4);
    if ((buttons & button::kSlow) != 0) result = result * l(0.4f);
    return result;
}

void drop_timers(PlayerState& ps, const MoveLocal& pml) {
    std::int32_t msec = pml.integer(pml::kMsec);
    std::int32_t time = ps.integer(ps::kPmTime);
    if (time != 0) {
        if (msec < time) {
            ps.set_integer(ps::kPmTime, time - msec);
        } else {
            ps.set_integer(ps::kPmFlags, ps.pm_flags() & ~(pm_flags::kJumping | pm_flags::kTimeKnockback | pm_flags::kSlide));
            ps.set_integer(ps::kPmTime, 0);
        }
    }
    for (std::size_t at : {ps::kLegsTimer, ps::kTorsoTimer}) {
        std::int32_t timer = ps.integer(at);
        if (timer > 0) {
            std::int32_t left = static_cast<std::int32_t>(static_cast<std::uint32_t>(timer) - static_cast<std::uint32_t>(msec));
            ps.set_integer(at, left < 0 ? 0 : left);
        }
    }
}

namespace {

/// ftol2(AngleNormalize180(angle)): whole degrees in (-180, 180].
std::int32_t whole_degrees(float angle) {
    return static_cast<std::int32_t>(net::ftol2(static_cast<double>(shared::math::angle_normalize180(angle))));
}

/// Clamped to 90 either way, as the signed byte the original keeps.
std::int32_t clamp_dir(std::int32_t degrees) {
    std::int32_t magnitude = degrees < 0 ? static_cast<std::int32_t>(0u - static_cast<std::uint32_t>(degrees)) : degrees;
    std::int32_t d = magnitude > 90 ? (degrees <= 0 ? -90 : 90) : degrees;
    return static_cast<std::int8_t>(d);
}

std::int32_t relative_dir(float angle, float yaw) { return clamp_dir(whole_degrees((F24::load(angle) - F24::load(yaw)).store())); }

/// The direction from the last move; false: 0.
bool moved_dir(const PlayerState& ps, const std::uint8_t* cmd, const MoveLocal& pml, float yaw, std::int32_t& out) {
    auto origin = ps.vec3(ps::kOrigin);
    auto before = pml.vec3(pml::kPreviousOrigin);
    std::array<float, 3> moved{};
    for (std::size_t i = 0; i < 3; ++i) moved[i] = (F24::load(origin[i]) - F24::load(before[i])).store();
    auto forward = static_cast<std::int8_t>(cmd[0x18]);
    if (forward == 0 && cmd[0x19] == 0) return false;
    if (ps.integer(ps::kGroundEntityNum) == kEntityNone) return false;
    F24 x = F24::load(moved[0]), y = F24::load(moved[1]), z = F24::load(moved[2]);
    float squared = (z * z + x * x + y * y).store();
    float length = F24::load(squared).sqrt().store();
    if (length == 0.0f) return false;  // (fucompp with 0: only equal stops; NaN goes on)
    // (fcomp, test ah,5, jp: on only when 5 frame times are below the length; NaN stops)
    if (!(F24::load(pml.frame_time()) * F24::load(5.0f) < F24::load(length))) return false;
    std::array<float, 3> dir{};
    shared::math::vec3_normalize_to(moved, dir);
    std::array<float, 3> unit = dir;  // (in place in the original: it reads before it writes)
    shared::math::vec_to_angles(unit, dir);
    std::int32_t degrees = whole_degrees((F24::load(dir[1]) - F24::load(yaw)).store());
    if (forward < 0) degrees = whole_degrees((F24::load(F24::integer(degrees).store()) + F24::load(180.0f)).store());
    out = clamp_dir(degrees);
    return true;
}

}  // namespace

void set_movement_dir(Pmove& pm, const MoveLocal& pml) {
    PlayerState& ps = pm.ps;
    const std::uint8_t* cmd = pm.cmd();
    float yaw = ps.real(ps::kViewAngles + 4);
    std::int32_t flags = ps.pm_flags();
    std::int32_t dir = 0;
    if ((flags & pm_flags::kProne) != 0 && (ps.integer(ps::kEFlags) & kEFlagsProneFree) == 0) {
        dir = relative_dir(ps.real(ps::kProneDirection), yaw);
    } else if ((flags & pm_flags::kLadder) != 0) {
        float ladder = (F24::load(shared::math::vectoyaw(ps.vec3(ps::kLadderVec))) + F24::load(180.0f)).store();
        dir = relative_dir(ladder, yaw);
    } else if (!moved_dir(ps, cmd, pml, yaw, dir)) {
        dir = 0;
    }
    ps.set_integer(ps::kMovementDir, dir);
}

std::array<float, 3> clip_velocity(const std::array<float, 3>& v, const std::array<float, 3>& normal) {
    F24 x = F24::load(v[0]), y = F24::load(v[1]), z = F24::load(v[2]);
    F24 a = F24::load(normal[0]), b = F24::load(normal[1]), c = F24::load(normal[2]);
    F24 backoff = x * a + y * b + c * z;
    F24 change = -(backoff - backoff.abs() * F24::load(0.001f));
    return {(change * a + x).store(), (change * b + y).store(), (change * c + z).store()};
}

void add_touch_ent(Pmove& pm, std::int32_t number) {
    if (number == kEntityWorld) return;
    std::int32_t count = pm.integer(pm::kNumTouch);
    if (count < 0 || count >= kMaxTouch) return;  // (a count the game never has: left alone)
    for (std::int32_t i = 0; i < count; ++i)
        if (pm.integer(pm::kTouchEnts + 4 * static_cast<std::size_t>(i)) == number) return;
    pm.set_integer(pm::kTouchEnts + 4 * static_cast<std::size_t>(count), number);
    pm.set_integer(pm::kNumTouch, count + 1);
}

bool jump_get_step_height(const PlayerState& ps, const std::array<float, 3>& origin, float& step, const MoveSettings& settings) {
    F24 top = F24::load(ps.real(ps::kJumpOriginZ)) + F24::load(settings.jump_height);
    if (!(top > F24::load(origin[2]))) return false;  // (fcomp, test ah,0x41, jne: on only when above; NaN stops)
    step = settings.jump_step_size;
    float stored = top.store();
    if (F24::load(settings.jump_step_size) + F24::load(origin[2]) > F24::load(stored))
        step = (F24::load(stored) - F24::load(origin[2])).store();
    return true;
}

void jump_clamp_velocity(PlayerState& ps, const std::array<float, 3>& origin, const MoveSettings& settings) {
    F24 z = F24::load(ps.real(ps::kOrigin + 8));
    if (!(z - F24::load(origin[2]) > F24::load(0.0f))) return;  // (only above goes on; NaN doesn't)
    F24 left = F24::load(settings.jump_height) + F24::load(ps.real(ps::kJumpOriginZ)) - z;
    constexpr std::size_t kVelocityZ = ps::kVelocity + 8;
    if (left < F24::load(0.1f)) {  // (fcom 0.1, test ah,5, jp: below stops the jump; NaN goes on)
        ps.set_real(kVelocityZ, 0.0f);
        return;
    }
    F24 most = (F24::integer(ps.integer(ps::kGravity)) * (left + left)).sqrt();
    if (most < F24::load(ps.real(kVelocityZ))) ps.set_real(kVelocityZ, most.store());
}

bool should_make_footsteps(const Pmove& pm, const MoveSettings& settings) {
    std::int32_t target = pm.ps.integer(ps::kViewHeightTarget);
    if (target == kCrouchViewHeight || target == kProneViewHeight) return false;
    if ((pm.ps.pm_flags() & pm_flags::kSlowed) != 0) return false;
    return F24::load(pm.real(pm::kXySpeed)) >= F24::load(settings.footsteps_threshold);  // (below or NaN: none)
}

trace::Trace player_trace(Pmove& pm, const std::array<float, 3>& start, const std::array<float, 3>& mins,
                          const std::array<float, 3>& maxs, const std::array<float, 3>& end, std::int32_t pass_entity,
                          std::int32_t content_mask, trace::MoveHost& host) {
    trace::Trace results = host.trace(pm.bytes[pm::kHandler], start, mins, maxs, end, pass_entity, content_mask);
    if (results.start_solid() && (results.contents() & trace::kContentsBody) != 0) {
        add_touch_ent(pm, results.entity_num());
        pm.set_integer(pm::kTraceMask, pm.integer(pm::kTraceMask) & ~trace::kContentsBody);
        results = host.trace(pm.bytes[pm::kHandler], start, mins, maxs, end, pass_entity,
                             content_mask & ~trace::kContentsBody);
    }
    return results;
}

void add_event(PlayerState& ps, std::int32_t event) {
    if (event == 0) return;
    std::int32_t sequence = ps.integer(ps::kEventSequence);
    auto slot = static_cast<std::size_t>(sequence & (kMaxPsEvents - 1));
    ps.set_integer(ps::kEvents + 4 * slot, event & 0xFF);
    ps.set_integer(ps::kEventParms + 4 * slot, 0);
    ps.set_integer(ps::kEventSequence, static_cast<std::int32_t>(static_cast<std::uint32_t>(sequence) + 1u));
}

void add_predictable_event(std::int32_t event, std::int32_t parm, PlayerState& ps) {
    if (event == 0) return;
    std::int32_t sequence = ps.integer(ps::kEventSequence);
    auto slot = static_cast<std::size_t>(sequence & (kMaxPsEvents - 1));
    ps.set_integer(ps::kEvents + 4 * slot, event & 0xFF);
    ps.set_integer(ps::kEventParms + 4 * slot, parm & 0xFF);
    ps.set_integer(ps::kEventSequence, static_cast<std::int32_t>(static_cast<std::uint32_t>(sequence) + 1u));
}

void foliage_sounds(Pmove& pm, const MoveSettings& settings, trace::MoveHost& host) {
    auto l = x87::F24::load;
    using x87::F24;
    F24 speed = l(pm.real(pm::kXySpeed));
    F24 min = l(settings.foliage_min_speed);
    std::int32_t last = pm.ps.integer(ps::kFoliageSoundTime);
    auto add = [](std::int32_t a, std::int32_t b) {
        return static_cast<std::int32_t>(static_cast<std::uint32_t>(a) + static_cast<std::uint32_t>(b));
    };
    if (speed < min) {  // (only below is too slow; NaN isn't)
        if (add(settings.foliage_reset_interval, last) < pm.server_time()) pm.ps.set_integer(ps::kFoliageSoundTime, 0);
        return;
    }
    F24 fraction = (speed - min) / (l(settings.foliage_max_speed) - min);
    if (fraction > l(1.0f)) fraction = l(1.0f);  // (only above is capped; NaN isn't)
    std::int32_t slow = settings.foliage_slow_interval;
    std::int32_t diff = static_cast<std::int32_t>(static_cast<std::uint32_t>(settings.foliage_fast_interval) -
                                                  static_cast<std::uint32_t>(slow));
    auto interval = static_cast<std::int32_t>(
        static_cast<std::uint64_t>(net::ftol2((F24::integer(diff) * fraction + F24::integer(slow)).v)));
    if (add(interval, last) >= pm.server_time()) return;
    auto box_mins = pm.vec3(pm::kMins), box_maxs = pm.vec3(pm::kMaxs);
    std::array<float, 3> mins{(l(box_mins[0]) * l(0.75f)).store(), (l(box_mins[1]) * l(0.75f)).store(),
                              (l(box_mins[2]) * l(0.75f)).store()};
    std::array<float, 3> maxs{(l(box_maxs[0]) * l(0.75f)).store(), (l(box_maxs[1]) * l(0.75f)).store(),
                              (l(box_maxs[2]) * l(0.9f)).store()};
    auto origin = pm.ps.vec3(ps::kOrigin);
    trace::Trace t = player_trace(pm, origin, mins, maxs, origin, pm.ps.integer(ps::kClientNum), kContentsFoliage, host);
    if (t.start_solid()) {
        add_predictable_event(kEvFoliageSound, 0, pm.ps);
        pm.ps.set_integer(ps::kFoliageSoundTime, pm.server_time());
    }
}

void update_ads_slowdown(Pmove& pm) {
    std::int32_t flags = pm.ps.pm_flags() & ~pm_flags::kSlowed;
    pm.ps.set_integer(ps::kPmFlags, flags);
    std::int32_t state = pm.ps.integer(ps::kWeaponState);
    if (pm.ps.pm_type() < pm_type::kDead && (pm.integer(pm::kCmd + cmd::kButtons) & button::kAds) != 0 &&
        (flags & pm_flags::kProne) == 0 && (flags & pm_flags::kAds) != 0 && !(state >= 5 && state <= 9))
        pm.ps.set_integer(ps::kPmFlags, flags | pm_flags::kSlowed);
}

void exit_aim_down_sight(PlayerState& ps) {
    add_predictable_event(kEvAdsExit, 0, ps);
    ps.set_integer(ps::kPmFlags, ps.pm_flags() & ~pm_flags::kAds);
}

std::int32_t footstep_type(const PlayerState& ps, const MoveLocal& pml) {
    std::int32_t flags = pml.integer(pml::kGroundSurfaceFlags);
    if ((flags & kSurfNoSteps) != 0) return 0;
    std::int32_t surface = (flags >> 20) & 0x1F;
    if (surface == 0) return 0;
    std::int32_t f = ps.pm_flags();
    if ((f & pm_flags::kProne) != 0) return surface + kEvFootstepProne;
    float lean = ps.real(ps::kLeanf);
    if ((f & pm_flags::kSlowed) != 0 || lean != 0.0f || std::isnan(lean)) return surface + kEvFootstepWalk;  // (NaN walks)
    return surface + kEvFootstepRun;
}

void footstep_event(Pmove& pm, const MoveLocal& pml, std::int32_t old_bob, std::int32_t new_bob, bool footstep,
                    trace::MoveHost& host) {
    auto plus64 = [](std::int32_t b) { return static_cast<std::uint32_t>(b) + 64u; };
    if (((plus64(old_bob) ^ plus64(new_bob)) & 0x80u) == 0) return;
    if (pm.ps.integer(ps::kGroundEntityNum) != kEntityNone) {
        if (footstep) add_event(pm.ps, footstep_type(pm.ps, pml));
        return;
    }
    if (!footstep || (pm.ps.pm_flags() & pm_flags::kLadder) == 0) return;
    auto mins = pm.vec3(pm::kMins), maxs = pm.vec3(pm::kMaxs);
    F24 six = F24::load(6.0f);
    std::array<float, 3> small_mins{(F24::load(mins[0]) + six).store(), (F24::load(mins[1]) + six).store(), 8.0f};
    float top = F24::load(maxs[2]) < F24::load(8.0f) ? 8.0f : maxs[2];  // (only below 8 is raised; NaN stays)
    std::array<float, 3> small_maxs{(F24::load(maxs[0]) - six).store(), (F24::load(maxs[1]) - six).store(), top};
    auto origin = pm.ps.vec3(ps::kOrigin);
    auto ladder = pm.ps.vec3(ps::kLadderVec);
    std::array<float, 3> end{};
    for (std::size_t i = 0; i < 3; ++i) end[i] = (F24::load(origin[i]) - F24::load(ladder[i]) * F24::load(31.0f)).store();
    std::int32_t mask = pm.integer(pm::kTraceMask) & ~(trace::kContentsBody | 0x00010000);
    trace::Trace tr = player_trace(pm, origin, small_mins, small_maxs, end, pm.ps.integer(ps::kClientNum), mask, host);
    std::int32_t surface = (tr.integer(trace::at::kSurfaceFlags) >> 20) & 0x1F;
    if (tr.fraction() == 1.0f || surface == 0) surface = kLadderSurfaceType;
    add_event(pm.ps, surface + kEvFootstepRun);
}

}  // namespace iw_pc::game::pmove
