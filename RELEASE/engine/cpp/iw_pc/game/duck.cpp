#include "iw_pc/game/duck.h"

#include <algorithm>
#include <array>

#include "iw_pc/game/jump.h"
#include "iw_pc/game/prone.h"
#include "iw_pc/game/slide.h"
#include "iw_pc/net/msg.h"
#include "iw_pc/shared/math.h"

namespace iw_pc::game::duck {

using x87::F24;
namespace pmv = iw_pc::game::pmove;
namespace ps = pmv::ps;
namespace pml_at = pmv::pml;
namespace pm_at = pmv::pm;
namespace pm_flags = pmv::pm_flags;
namespace tr = iw_pc::game::trace;
namespace math = iw_pc::shared::math;

const ViewLerpWaypoint kStandToCrouch[9] = {{0, 60.0f, 0},  {1, 59.5f, 0},  {4, 58.5f, 0},  {30, 56.0f, 0}, {80, 44.0f, 0},
                                            {90, 41.5f, 0}, {95, 40.5f, 0}, {100, 40.0f, 0}, {-1, 0.0f, 0}};
const ViewLerpWaypoint kCrouchToStand[9] = {{0, 40.0f, 0},  {5, 40.5f, 0},  {10, 41.5f, 0},  {20, 44.0f, 0}, {70, 56.0f, 0},
                                            {96, 58.5f, 0}, {99, 59.5f, 0}, {100, 60.0f, 0}, {-1, 0.0f, 0}};
const ViewLerpWaypoint kCrouchToProne[11] = {{0, 40.0f, 0},  {11, 38.0f, 0}, {22, 33.0f, 0}, {34, 25.0f, 0},
                                             {45, 16.0f, 0}, {50, 15.0f, 0}, {55, 16.0f, 0}, {70, 18.0f, 0},
                                             {90, 17.0f, 0}, {100, 11.0f, 0}, {-1, 0.0f, 0}};
const ViewLerpWaypoint kProneToCrouch[8] = {{0, 11.0f, 0},  {5, 10.0f, 0},  {30, 21.0f, 0}, {50, 25.0f, 0},
                                            {67, 31.0f, 0}, {83, 34.0f, 0}, {100, 40.0f, 0}, {-1, 0.0f, 0}};

namespace {

std::int32_t wrapping_sub(std::int32_t a, std::int32_t b) {
    return static_cast<std::int32_t>(static_cast<std::uint32_t>(a) - static_cast<std::uint32_t>(b));
}

std::span<const ViewLerpWaypoint> lerp_table(std::int32_t target, bool down) {
    if (target == pmv::kProneViewHeight) return kCrouchToProne;
    if (target == pmv::kCrouchViewHeight) return down ? std::span<const ViewLerpWaypoint>(kStandToCrouch)
                                                      : std::span<const ViewLerpWaypoint>(kProneToCrouch);
    return kCrouchToStand;
}

bool fits(pmv::Pmove& pm, float height, tr::MoveHost& host) {
    pm.set_real(pm_at::kMaxs + 8, height);
    auto origin = pm.ps.vec3(ps::kOrigin);
    auto mins = pm.vec3(pm_at::kMins), maxs = pm.vec3(pm_at::kMaxs);
    std::int32_t mask = pm.integer(pm_at::kTraceMask) & ~tr::kContentsBody;
    return !host.trace(pm.bytes[pm_at::kHandler], origin, mins, maxs, origin, pm.ps.integer(ps::kClientNum), mask)
                .all_solid();
}

void refused(pmv::Pmove& pm, std::int32_t event, std::int32_t parm) {
    if ((pm.integer(pm_at::kCmd + pmv::cmd::kButtons) & pmv::button::kNoStanceEvent) == 0)
        pmv::add_predictable_event(event, parm, pm.ps);
}

void set_stance_flags(pmv::PlayerState& p, bool prone, bool ducked) {
    std::int32_t flags = p.pm_flags() & ~(pm_flags::kProne | pm_flags::kDucked);
    if (prone) flags |= pm_flags::kProne;
    if (ducked) flags |= pm_flags::kDucked;
    p.set_integer(ps::kPmFlags, flags);
}

void play_torso_anim_zero(pmv::PlayerState& p, tr::MoveHost& host) {
    std::int32_t anim = p.integer(ps::kTorsoAnim);
    if ((anim & ~kAnimToggleBit) != 0) {
        p.set_integer(ps::kTorsoAnim, ~anim & kAnimToggleBit);
        p.set_integer(ps::kTorsoAnimDuration,
                      static_cast<std::int32_t>(static_cast<std::uint32_t>(host.anim_duration(0)) + 50u));
    }
}

}  // namespace

std::int32_t view_height_lerp_time(std::int32_t target, std::int32_t down) {
    if (target == pmv::kProneViewHeight) return 400;
    if (target == pmv::kCrouchViewHeight) return down != 0 ? 200 : 400;
    return 200;
}

std::pair<F24, float> view_height_table_lerp(std::int32_t fraction, std::span<const ViewLerpWaypoint> table) {
    auto l = F24::load;
    auto at = [&](const ViewLerpWaypoint& p) { return std::pair<F24, float>{l(p.view_height), F24::integer(p.offset).store()}; };
    if (fraction == 0) return at(table[0]);
    for (std::size_t k = 1; k < table.size(); ++k) {
        const ViewLerpWaypoint& p = table[k];
        if (p.fraction == -1) break;
        if (p.fraction == fraction) return at(p);
        if (p.fraction > fraction) {
            const ViewLerpWaypoint& prev = table[k - 1];
            F24 t = F24::integer(wrapping_sub(fraction, prev.fraction)) / F24::integer(wrapping_sub(p.fraction, prev.fraction));
            float offset = (F24::integer(wrapping_sub(p.offset, prev.offset)) * t + F24::integer(prev.offset)).store();
            F24 height = t * (l(p.view_height) - l(prev.view_height)) + l(prev.view_height);
            return {height, offset};
        }
    }
    return at(table[0]);
}

void view_height_adjust(pmv::Pmove& pm, pmv::MoveLocal& pml, const pmv::MoveSettings& settings, tr::MoveHost& host) {
    auto l = F24::load;
    pmv::PlayerState& p = pm.ps;
    std::int32_t target = p.integer(ps::kViewHeightTarget);
    F24 current = l(p.real(ps::kViewHeightCurrent));
    if (target == 0 || current.v == 0.0) {  // (only equal is none; NaN goes on)
        p.set_real(ps::kViewHeightCurrent, p.pm_type() == pmv::pm_type::kSpectator ? 0.0f : F24::integer(target).store());
        return;
    }
    float goal = F24::integer(target).store();
    // (fucompp with the integer's register, not the float kept)
    if (current.v == F24::integer(target).v && p.integer(ps::kViewHeightLerpTime) == 0) return;
    if (target != pmv::kProneViewHeight && target != pmv::kCrouchViewHeight && target != pmv::kStandViewHeight) {
        p.set_integer(ps::kViewHeightLerpTime, 0);
        F24 step = l(pml.frame_time()) * l(180.0f);
        if (current < l(goal)) {
            F24 up = step + current;
            p.set_real(ps::kViewHeightCurrent, up.store());
            if (up >= l(goal)) p.set_real(ps::kViewHeightCurrent, goal);  // (below, or NaN, keeps going)
        } else {
            F24 down = current - step;
            p.set_real(ps::kViewHeightCurrent, down.store());
            if (down <= l(goal)) p.set_real(ps::kViewHeightCurrent, goal);  // (above, or NaN, keeps going)
        }
        return;
    }

    std::int32_t fraction = 0;
    std::int32_t start = p.integer(ps::kViewHeightLerpTime);
    if (start != 0) {
        std::int32_t lerp_target = p.integer(ps::kViewHeightLerpTarget);
        std::int32_t down = p.integer(ps::kViewHeightLerpDown);
        std::int32_t time = view_height_lerp_time(lerp_target, down);
        fraction = static_cast<std::int32_t>(static_cast<std::uint32_t>(wrapping_sub(pm.server_time(), start)) * 100u) / time;
        if (fraction >= 100) {
            fraction = 100;
            p.set_real(ps::kViewHeightCurrent, F24::integer(lerp_target).store());
            p.set_integer(ps::kViewHeightLerpTime, 0);
            p.set_integer(ps::kViewHeightLerpPosAdj, 0);
        } else {
            fraction = std::max(fraction, 0);
            auto [height, offset] = view_height_table_lerp(fraction, lerp_table(lerp_target, down != 0));
            p.set_real(ps::kViewHeightCurrent, height.store());
            F24 last = l(p.real(ps::kViewHeightLerpPosAdj));
            if ((last - l(offset)).abs() > l(0.05f)) {  // (only above slides; NaN doesn't)
                auto saved = p.velocity();
                F24 change = l(offset) - last;
                if (p.integer(ps::kGroundEntityNum) == pmv::kEntityNone) change = change * l(0.5f);
                float speed = (change / l(pml.frame_time())).store();
                auto forward = pml.vec3(pml_at::kForward);
                std::array<float, 3> dir{forward[0], forward[1], 0.0f};
                math::vec3_normalize(dir);
                p.set_velocity({(l(dir[0]) * l(speed)).store(), (l(dir[1]) * l(speed)).store(), (l(dir[2]) * l(speed)).store()});
                slide::step_slide_move(pm, pml, true, settings, host);
                pm.ps.set_velocity(saved);
                pm.ps.set_real(ps::kViewHeightLerpPosAdj, offset);
            }
        }
    }

    if (p.integer(ps::kViewHeightLerpTime) != 0) {
        std::int32_t now_target = p.integer(ps::kViewHeightTarget);
        std::int32_t lerp_target = p.integer(ps::kViewHeightLerpTarget);
        std::int32_t down = p.integer(ps::kViewHeightLerpDown);
        bool wrong_way = (now_target < lerp_target && down == 0) || (now_target > lerp_target && down != 0);
        if (!wrong_way) return;
        std::int32_t rest = 100 - fraction;
        down ^= 1;
        p.set_integer(ps::kViewHeightLerpDown, down);
        std::int32_t next = lerp_target;
        if (down != 0) {
            if (lerp_target == pmv::kStandViewHeight) next = pmv::kCrouchViewHeight;
            else if (lerp_target == pmv::kCrouchViewHeight) next = pmv::kProneViewHeight;
        } else {
            if (lerp_target == pmv::kProneViewHeight) next = pmv::kCrouchViewHeight;
            else if (lerp_target == pmv::kCrouchViewHeight) next = pmv::kStandViewHeight;
        }
        p.set_integer(ps::kViewHeightLerpTarget, next);
        if (rest == 100) {
            p.set_real(ps::kViewHeightCurrent, F24::integer(next).store());
            p.set_integer(ps::kViewHeightLerpTime, 0);
            p.set_integer(ps::kViewHeightLerpPosAdj, 0);
            return;
        }
        std::int32_t time = view_height_lerp_time(next, down);
        auto gone = static_cast<std::int32_t>(
            static_cast<std::uint64_t>(net::ftol2((F24::integer(time) * F24::integer(rest) * l(0.01f)).v)));
        p.set_integer(ps::kViewHeightLerpTime, wrapping_sub(pm.server_time(), gone));
        auto [height, offset] = view_height_table_lerp(rest, lerp_table(next, down != 0));
        (void)height;
        p.set_real(ps::kViewHeightLerpPosAdj, offset);
        return;
    }

    target = p.integer(ps::kViewHeightTarget);
    goal = F24::integer(target).store();
    current = l(p.real(ps::kViewHeightCurrent));
    if (current.v == F24::integer(target).v) return;  // (only equal is there; NaN starts one)
    p.set_integer(ps::kViewHeightLerpTime, pm.server_time());
    std::int32_t lerp_target, down;
    if (target == pmv::kProneViewHeight) {
        lerp_target = current > l(40.0f) ? pmv::kCrouchViewHeight : pmv::kProneViewHeight;
        down = 1;
    } else if (target == pmv::kCrouchViewHeight) {
        lerp_target = pmv::kCrouchViewHeight;
        down = current > l(goal) ? 1 : 0;
    } else if (target == pmv::kStandViewHeight) {
        lerp_target = current < l(40.0f) ? pmv::kCrouchViewHeight : pmv::kStandViewHeight;
        down = 0;
    } else {
        return;
    }
    p.set_integer(ps::kViewHeightLerpDown, down);
    p.set_integer(ps::kViewHeightLerpTarget, lerp_target);
}

void check_duck(pmv::Pmove& pm, pmv::MoveLocal& pml, const pmv::MoveSettings& settings, tr::MoveHost& host) {
    auto l = F24::load;
    pm.set_integer(pm_at::kProneChange, 0);
    const std::size_t buttons_at = pm_at::kCmd + pmv::cmd::kButtons;
    if (pm.ps.pm_type() == pmv::pm_type::kSpectator) {
        pm.set_vec3(pm_at::kMins, {-8.0f, -8.0f, -8.0f});
        pm.set_vec3(pm_at::kMaxs, {8.0f, 8.0f, 16.0f});
        set_stance_flags(pm.ps, false, false);
        std::int32_t buttons = pm.integer(buttons_at);
        if ((buttons & pmv::button::kProne) != 0) {
            pm.set_integer(buttons_at, buttons & ~pmv::button::kProne);
            pmv::add_predictable_event(kEvStanceForceStand, 0, pm.ps);
        }
        pm.ps.set_integer(ps::kViewHeightTarget, 0);
        pm.ps.set_integer(ps::kViewHeightCurrent, 0);
        return;
    }
    bool was_prone = (pm.ps.pm_flags() & pm_flags::kProne) != 0;
    auto mins = pm.ps.vec3(ps::kMins), maxs = pm.ps.vec3(ps::kMaxs);
    pm.set_real(pm_at::kMins, mins[0]);
    pm.set_real(pm_at::kMins + 4, mins[1]);
    pm.set_real(pm_at::kMaxs, maxs[0]);
    pm.set_real(pm_at::kMaxs + 4, maxs[1]);
    pm.set_real(pm_at::kMins + 8, mins[2]);
    if (pm.ps.pm_type() >= pmv::pm_type::kDead) {
        pm.set_real(pm_at::kMaxs + 8, maxs[2]);
        pm.ps.set_integer(ps::kViewHeightTarget, kDeadViewHeight);
        view_height_adjust(pm, pml, settings, host);
        return;
    }

    std::int32_t e_flags = pm.ps.integer(ps::kEFlags);
    if ((e_flags & pmv::kEFlagsMounted) != 0) {
        bool a = (e_flags & 0x100) != 0, b = (e_flags & 0x200) != 0;
        set_stance_flags(pm.ps, a && !b, b && !a);
    } else if ((pm.ps.pm_flags() & pm_flags::kNoLean) == 0) {
        const std::int32_t stance_buttons = pmv::button::kProne | pmv::button::kCrouch;
        std::int32_t buttons = pm.integer(buttons_at);
        if ((pm.ps.pm_flags() & pm_flags::kLadder) != 0 && (buttons & stance_buttons) != 0) {
            pm.set_integer(buttons_at, buttons & ~stance_buttons);
            pmv::add_predictable_event(kEvStanceForceStand, 0, pm.ps);
        }
        buttons = pm.integer(buttons_at);
        std::int32_t flags = pm.ps.pm_flags();
        if ((buttons & pmv::button::kProne) != 0) {
            if ((flags & pm_flags::kProne) != 0) {
                set_stance_flags(pm.ps, true, false);
            } else if (pm.ps.integer(ps::kGroundEntityNum) != pmv::kEntityNone) {
                prone::ProneQuery q;
                q.pass_entity = pm.ps.integer(ps::kClientNum);
                q.origin = pm.ps.vec3(ps::kOrigin);
                q.size = pm.real(pm_at::kMaxs);
                q.height = kProneMaxsZ;
                q.yaw = pm.ps.real(ps::kViewAngles + 4);
                q.already_prone = false;
                q.on_ground = true;
                q.handler = pm.bytes[pm_at::kHandler];
                q.check_type = 0;
                q.feet_distance = 66.0f;
                float h = pm.ps.real(ps::kProneTorsoHeight), t = pm.ps.real(ps::kProneTorsoPitch),
                      w = pm.ps.real(ps::kProneWaistPitch);
                bool ok = prone::check_prone(q, prone::ProneAngles{&h, &t, &w}, host);
                pm.ps.set_real(ps::kProneTorsoHeight, h);
                pm.ps.set_real(ps::kProneTorsoPitch, t);
                pm.ps.set_real(ps::kProneWaistPitch, w);
                if (ok) {
                    set_stance_flags(pm.ps, true, false);
                } else if (pm.ps.integer(ps::kGroundEntityNum) != pmv::kEntityNone) {
                    std::int32_t f = pm.ps.pm_flags() | pm_flags::kProneViewTurned;
                    pm.ps.set_integer(ps::kPmFlags, f);
                    refused(pm, (f & pm_flags::kDucked) != 0 ? kEvStanceForceCrouch : kEvStanceForceStand, 0);
                }
            }
        } else if ((buttons & pmv::button::kCrouch) != 0) {
            if ((flags & pm_flags::kProne) != 0) {
                if (fits(pm, kCrouchMaxsZ, host)) {
                    host.anim_script_event(pm.ps, tr::kAnimEtProneToCrouch, false, false);  // (BG_AnimScriptEvent, inlined)
                    set_stance_flags(pm.ps, false, true);
                } else {
                    refused(pm, kEvStanceForceProne, 2);
                }
            } else {
                host.anim_script_event(pm.ps, tr::kAnimEtStandToCrouch, false, false);
                pm.ps.set_integer(ps::kPmFlags, pm.ps.pm_flags() | pm_flags::kDucked);
            }
        } else if ((flags & pm_flags::kProne) != 0) {
            if (fits(pm, maxs[2], host)) {
                host.anim_script_event(pm.ps, tr::kAnimEtProneToStand, false, false);
                set_stance_flags(pm.ps, false, false);
            } else if (fits(pm, kCrouchMaxsZ, host)) {
                set_stance_flags(pm.ps, false, true);
            } else {
                refused(pm, kEvStanceForceProne, 1);
            }
        } else if ((flags & pm_flags::kDucked) != 0) {
            if (fits(pm, maxs[2], host)) {
                host.anim_script_event(pm.ps, tr::kAnimEtCrouchToStand, false, false);
                pm.ps.set_integer(ps::kPmFlags, pm.ps.pm_flags() & ~pm_flags::kDucked);
            } else {
                refused(pm, kEvStanceForceCrouch, 1);
            }
        }
    }

    if (pm.ps.integer(ps::kViewHeightLerpTime) == 0) {
        std::int32_t target = pm.ps.integer(ps::kViewHeightTarget);
        if ((pm.ps.pm_flags() & pm_flags::kProne) != 0) {
            if (target == pmv::kStandViewHeight) {
                pm.ps.set_integer(ps::kViewHeightTarget, pmv::kCrouchViewHeight);
            } else if (target != pmv::kProneViewHeight) {
                pm.ps.set_integer(ps::kViewHeightTarget, pmv::kProneViewHeight);
                pm.set_integer(pm_at::kProneChange, 1);
                play_torso_anim_zero(pm.ps, host);
                jump::jump_activate_slowdown(pm.ps);
            }
        } else if (target == pmv::kProneViewHeight) {
            pm.ps.set_integer(ps::kViewHeightTarget, pmv::kCrouchViewHeight);
            pm.set_integer(pm_at::kProneChange, 1);
            play_torso_anim_zero(pm.ps, host);
        } else {
            pm.ps.set_integer(ps::kViewHeightTarget, (pm.ps.pm_flags() & pm_flags::kDucked) != 0 ? pmv::kCrouchViewHeight
                                                                                               : pmv::kStandViewHeight);
        }
    }
    view_height_adjust(pm, pml, settings, host);

    std::int32_t target = pm.ps.integer(ps::kViewHeightTarget);
    std::int32_t e = pm.ps.integer(ps::kEFlags) & ~(kEFlagsCrouching | kEFlagsProne);
    if (target == pmv::kCrouchViewHeight) {
        pm.set_real(pm_at::kMaxs + 8, kCrouchMaxsZ);
        pm.ps.set_integer(ps::kEFlags, e | kEFlagsCrouching);
        set_stance_flags(pm.ps, false, true);
    } else if (target == pmv::kProneViewHeight) {
        pm.set_real(pm_at::kMaxs + 8, kProneMaxsZ);
        pm.ps.set_integer(ps::kEFlags, e | kEFlagsProne);
        set_stance_flags(pm.ps, true, false);
    } else {
        pm.set_real(pm_at::kMaxs + 8, pm.ps.real(ps::kMaxs + 8));
        pm.ps.set_integer(ps::kEFlags, e);
        set_stance_flags(pm.ps, false, false);
    }

    if ((pm.ps.pm_flags() & pm_flags::kProne) == 0 || was_prone) return;
    const std::uint8_t* command = pm.cmd();
    if (command[pmv::cmd::kForwardMove] != 0 || command[pmv::cmd::kRightMove] != 0) {
        pm.ps.set_integer(ps::kPmFlags, pm.ps.pm_flags() & ~pm_flags::kAdsHold);
        pmv::exit_aim_down_sight(pm.ps);
    }
    std::uint8_t handler = pm.bytes[pm_at::kHandler];
    std::int32_t client = pm.ps.integer(ps::kClientNum);
    std::int32_t mask = pm.integer(pm_at::kTraceMask) & ~tr::kContentsBody;
    auto box_mins = pm.vec3(pm_at::kMins), box_maxs = pm.vec3(pm_at::kMaxs);
    auto origin = pm.ps.vec3(ps::kOrigin);
    std::array<float, 3> up{origin[0], origin[1], (l(origin[2]) + l(10.0f)).store()};
    tr::Trace t = host.trace(handler, origin, box_mins, box_maxs, up, client, mask);
    F24 f = l(t.fraction());
    std::array<float, 3> point{};
    for (std::size_t i = 0; i < 3; ++i) point[i] = ((l(up[i]) - l(origin[i])) * f + l(origin[i])).store();
    tr::Trace back = host.trace(handler, point, box_mins, box_maxs, origin, client, mask);
    auto settled = math::vec3_lerp(point, origin, back.fraction());
    pm.ps.set_vec3(ps::kOrigin, settled);
    pm.ps.set_real(ps::kProneDirection, pm.ps.real(ps::kViewAngles + 4));
    std::array<float, 3> below{settled[0], settled[1], (l(settled[2]) - l(0.25f)).store()};
    tr::Trace ground = host.trace(handler, settled, box_mins, box_maxs, below, client, mask);
    float pitch = 0.0f;
    if (!ground.start_solid() && l(ground.fraction()) < l(1.0f))  // (only below hit the ground; NaN didn't)
        pitch = math::pitch_for_yaw_on_normal(pm.ps.real(ps::kProneDirection), ground.normal());
    pm.ps.set_real(ps::kProneDirectionPitch, pitch);
    float view_pitch = pm.ps.real(ps::kViewAngles);
    F24 delta = l(math::angle_normalize180((l(pitch) - l(view_pitch)).store()));
    float base = pitch;
    if (delta < l(-45.0f)) base = (l(view_pitch) - l(45.0f)).store();
    else if (delta > l(45.0f)) base = (l(view_pitch) + l(45.0f)).store();
    pm.ps.set_real(ps::kPronePitchBase, base);
}

}  // namespace iw_pc::game::duck
