#include "iw_pc/game/weapon.h"

#include <bit>

#include "iw_pc/game/anim.h"
#include "iw_pc/net/msg.h"
#include "iw_pc/shared/math.h"
#include "iw_pc/x87.h"

namespace iw_pc::game::weapon {

using x87::F24;
namespace pmv = iw_pc::game::pmove;
namespace ps = pmv::ps;
namespace fl = pmv::pm_flags;

namespace {
constexpr std::int32_t kOne = 0x3F800000;  // 1.0f's bits

bool sights_wanted(const pmv::PlayerState& p, const WeaponDef& d, std::int32_t flags) {
    std::int32_t st = p.integer(ps::kWeaponState);
    std::int32_t reload_left = static_cast<std::int32_t>(static_cast<std::uint32_t>(p.integer(ps::kWeaponTime)) -
                                                         static_cast<std::uint32_t>(d.integer(def::kReloadAdsTime)));
    bool blocked = d.integer(def::kSegmentedReload) == 0
                       ? st == state::kReloading && reload_left > 0
                       : (st >= state::kReloading && st <= 8) || (st == state::kReloadEnd && reload_left > 0);
    bool wanted = !blocked && !(d.integer(def::kRechamberWhileAds) == 0 && st == state::kRechambering) &&
                  (flags & fl::kAds) != 0;
    if (d.integer(def::kAdsFire) != 0 && p.integer(ps::kWeaponDelay) != 0 && st == state::kFiring) wanted = true;
    return wanted;
}

float short_to_angle(std::int32_t a) { return (F24::integer(a) * F24::load(0.0054931640625f)).store(); }

std::int32_t word_at(const std::uint8_t* b) {
    std::int32_t v;
    std::memcpy(&v, b, 4);
    return v;
}
}  // namespace

std::int32_t binocular_weapon_index(const trace::MoveHost& host) {
    std::int32_t count = host.weapon_count();
    for (std::int32_t w = 1; w <= count; ++w)
        if (host.weapon_def(w).integer(def::kWeapType) == kWeaptypeBinoculars) return w & 0xFF;
    return 0;
}

bool binoculars_up(std::int32_t state) { return state == 0x13 || state == 0x14; }

std::int32_t view_weapon(const pmv::PlayerState& p, const trace::MoveHost& host) {
    std::int32_t flags = p.pm_flags();
    if (flags & fl::kBinoculars) return binocular_weapon_index(host);
    if (flags & fl::kOffhand) return p.integer(ps::kOffhandIndex);
    return p.integer(ps::kWeapon);
}

bool is_ads_allowed(const pmv::PlayerState& p, const pmv::MoveLocal& pml, const trace::MoveHost& host) {
    std::int32_t type = p.pm_type();
    if (type == 1) {
        if (pml.integer(pmv::pml::kAlmostGroundPlane) != 0) return false;
    } else if (type >= 2 && type <= 7) {
        return false;
    }
    if (host.weapon_def(view_weapon(p, host)).integer(def::kAimDownSight) == 0) return false;
    std::int32_t st = p.integer(ps::kWeaponState);
    if ((st >= 0xC && st <= 0x10) || st == 0xA || st == 0xB || st == 1 || st == 2) return false;
    return !(st >= 0x11 && st <= 0x16) || binoculars_up(st);
}

void update_aim_down_sight_flag(pmv::Pmove& pm, const pmv::MoveLocal& pml, trace::MoveHost& host) {
    std::int32_t flags = pm.ps.pm_flags() & ~fl::kAds;
    pm.ps.set_integer(ps::kPmFlags, flags);
    bool allowed = is_ads_allowed(pm.ps, pml, host);
    bool held = (pm.integer(pmv::pm::kCmd + pmv::cmd::kButtons) & pmv::button::kAds) != 0;
    std::int32_t st = pm.ps.integer(ps::kWeaponState);
    if ((held || st == 0x13 || st == 0x14) && allowed) {
        if (flags & fl::kProne) {
            std::int32_t old = pm.integer(pmv::pm::kOldCmd + pmv::cmd::kButtons);
            const std::uint8_t* c = pm.cmd();
            if ((old & pmv::button::kAds) == 0 || (c[pmv::cmd::kForwardMove] == 0 && c[pmv::cmd::kRightMove] == 0))
                pm.ps.set_integer(ps::kPmFlags, flags | fl::kAds | fl::kAdsHold);
        } else {
            pm.ps.set_integer(ps::kPmFlags, flags | fl::kAds);
        }
    }
    std::int32_t ads = (pm.ps.pm_flags() & fl::kAds) != 0 ? 1 : 0;
    anim::update_condition_value(host.client_anim_info(pm.ps.integer(ps::kClientNum)), anim::kAnimCondWeaponPosition,
                                 ads, true);
}

void update_aim_down_sight_lerp(pmv::Pmove& pm, const pmv::MoveLocal& pml, const pmv::MoveSettings& settings,
                                const trace::MoveHost& host) {
    auto l = F24::load;
    std::int32_t flags = pm.ps.pm_flags();
    std::int32_t now = pm.server_time();
    const WeaponDef& d = host.weapon_def(view_weapon(pm.ps, host));
    pmv::PlayerState& p = pm.ps;
    if (settings.scope_exit_on_damage && p.integer(ps::kDamageCount) != 0 && d.integer(def::kOverlayReticle) != 0) {
        pmv::exit_aim_down_sight(p);
        p.set_integer(ps::kWeaponPosFrac, 0);
        p.set_integer(ps::kAdsDelayTime, 0);
        return;
    }
    if (d.integer(def::kAimDownSight) == 0) {
        p.set_integer(ps::kWeaponPosFrac, 0);
        p.set_integer(ps::kAdsDelayTime, 0);
        return;
    }
    bool up = sights_wanted(p, d, flags);
    bool at_sights = p.integer(ps::kWeaponPosFrac) == kOne;  // (the bits, as integers)
    std::int32_t delay = settings.ads_exit_delay;
    if (at_sights && !up && delay > 0) {
        if (p.integer(ps::kAdsDelayTime) == 0)
            p.set_integer(ps::kAdsDelayTime,
                          static_cast<std::int32_t>(static_cast<std::uint32_t>(now) + static_cast<std::uint32_t>(delay)));
        if (p.integer(ps::kAdsDelayTime) > now) up = true;
        else p.set_integer(ps::kAdsDelayTime, 0);
    } else {
        p.set_integer(ps::kAdsDelayTime, 0);
    }
    if (up) {
        if (p.integer(ps::kWeaponPosFrac) == kOne) return;
    } else if (l(p.real(ps::kWeaponPosFrac)).v == 0.0) {  // (only 0 stays; NaN moves)
        return;
    }
    F24 msec = F24::integer(pml.integer(pmv::pml::kMsec));
    F24 frac = l(p.real(ps::kWeaponPosFrac));
    F24 moved = up ? msec * l(d.real(def::kOoPosAnimLengthIn)) + frac : frac - msec * l(d.real(def::kOoPosAnimLengthOut));
    p.set_real(ps::kWeaponPosFrac, moved.store());
    p.set_real(ps::kWeaponPosFrac, shared::math::i_fclamp(p.real(ps::kWeaponPosFrac), 0.0f, 1.0f));
}

void update_aim_spread_scale(pmv::Pmove& pm, const pmv::MoveLocal& pml, const pmv::MoveSettings& settings,
                             const trace::MoveHost& host) {
    auto l = F24::load;
    const WeaponDef& d = host.weapon_def(pm.ps.integer(ps::kWeapon));
    float frame_time = pml.frame_time();
    F24 rate = l(d.real(def::kHipSpreadDecayRate));
    float decay;
    F24 add = l(0.0f);
    bool in_air = pm.ps.integer(ps::kGroundEntityNum) == pmv::kEntityNone && pm.ps.pm_type() != 1;
    if (rate.v == 0.0) {
        decay = 1.0f;
    } else {
        std::int32_t e_flags = pm.ps.integer(ps::kEFlags);
        F24 r = rate;
        if (in_air) r = r * l(0.5f);
        else if (e_flags & 8) r = r * l(d.real(def::kHipSpreadProneDecay));
        else if (e_flags & 4) r = r * l(d.real(def::kHipSpreadDuckedDecay));
        decay = (r * l(frame_time)).store();
        if (pm.ps.integer(ps::kWeaponPosFrac) != kOne) {  // (the bits, as integers)
            float turn = d.real(def::kHipSpreadTurnAdd);
            if (l(turn).v != 0.0) {  // (only 0 adds nothing; NaN adds)
                float per_second = (l(1.0f) / l(frame_time)).store();
                float sum = 0.0f;
                const std::uint8_t* c = pm.cmd();
                const std::uint8_t* old = pm.bytes.data() + pmv::pm::kOldCmd;
                for (std::size_t i = 0; i < 2; ++i) {
                    float now_a = short_to_angle(word_at(c + pmv::cmd::kAngles + 4 * i));
                    float before = short_to_angle(word_at(old + pmv::cmd::kAngles + 4 * i));
                    F24 turned = l(shared::math::angle_subtract(now_a, before)).abs();
                    add = turned * l(turn) * l(0.01f) * l(per_second) + l(sum);
                    sum = add.store();
                }
            }
            float move_add = d.real(def::kHipSpreadMoveAdd);
            if (l(move_add).v != 0.0) {  // (only 0 adds nothing; NaN adds)
                const std::uint8_t* c = pm.cmd();
                if (c[pmv::cmd::kForwardMove] != 0 || c[pmv::cmd::kRightMove] != 0) {
                    auto v = pm.ps.vec3(ps::kVelocity);
                    F24 speed2 = l(v[0]) * l(v[0]) + l(v[1]) * l(v[1]);
                    F24 threshold = l(settings.aim_spread_move_speed_threshold);
                    if ((threshold * threshold).v < speed2.v) add = add + l(move_add);  // (only below; NaN doesn't)
                }
            }
            if (in_air) add = add + l(1.28f) + l(1.28f);
            add = add * l(frame_time);
        }
    }
    F24 scale = (add - l(decay)) * l(255.0f) + l(pm.ps.real(ps::kAimSpreadScale));
    pm.ps.set_real(ps::kAimSpreadScale, scale.store());
    if (scale.v < 0.0) pm.ps.set_real(ps::kAimSpreadScale, 0.0f);  // (NaN stays)
    else if (scale.v > 255.0) pm.ps.set_real(ps::kAimSpreadScale, 255.0f);
}

void update_hold_breath(pmv::Pmove& pm, const pmv::MoveLocal& pml, const pmv::MoveSettings& settings,
                        const trace::MoveHost& host) {
    auto l = F24::load;
    std::int32_t flags = pm.ps.pm_flags();
    std::int32_t w = view_weapon(pm.ps, host);
    auto ms = [&](float s) {
        return static_cast<std::int32_t>(static_cast<std::uint64_t>(net::ftol2((l(s) * l(1000.0f)).v)));
    };
    std::int32_t hold = ms(settings.breath_hold_time), gasp = ms(settings.breath_gasp_time);
    const WeaponDef& d = host.weapon_def(w);
    bool held = (pm.integer(pmv::pm::kCmd + pmv::cmd::kButtons) & pmv::button::kHoldBreath) != 0;
    pmv::PlayerState& p = pm.ps;
    if (hold <= 0) {
        p.set_integer(ps::kPmFlags, flags & ~fl::kHoldBreath);
        p.set_real(ps::kHoldBreathScale, 1.0f);
        p.set_integer(ps::kHoldBreathTimer, 0);
        return;
    }
    bool at_sights = p.integer(ps::kWeaponPosFrac) == kOne;  // (the bits, as integers)
    if (at_sights && d.integer(def::kOverlayReticle) != 0 && d.integer(def::kWeapClass) != 9 && held) {
        if (p.integer(ps::kHoldBreathTimer) == 0) p.set_integer(ps::kPmFlags, flags | fl::kHoldBreath);
    } else {
        p.set_integer(ps::kPmFlags, flags & ~fl::kHoldBreath);
    }
    flags = p.pm_flags();
    auto msec = static_cast<std::uint32_t>(pml.integer(pmv::pml::kMsec));
    bool holding = (flags & fl::kHoldBreath) != 0;
    auto t = static_cast<std::uint32_t>(p.integer(ps::kHoldBreathTimer));
    auto timer = static_cast<std::int32_t>(holding ? t + msec : t - msec);
    p.set_integer(ps::kHoldBreathTimer, timer < 0 ? 0 : timer);
    auto total = static_cast<std::int32_t>(static_cast<std::uint32_t>(gasp) + static_cast<std::uint32_t>(hold));
    if (holding && p.integer(ps::kHoldBreathTimer) > hold) {
        p.set_integer(ps::kHoldBreathTimer, total);
        p.set_integer(ps::kPmFlags, flags & ~fl::kHoldBreath);
    }
    F24 target;
    float rate;
    if (p.pm_flags() & fl::kHoldBreath) {
        target = l(0.0f);
        rate = settings.breath_hold_lerp;
    } else {
        F24 part = F24::integer(p.integer(ps::kHoldBreathTimer)) / F24::integer(total);
        target = part * (l(settings.breath_gasp_scale) - l(1.0f)) + l(1.0f);
        rate = settings.breath_gasp_lerp;
    }
    float goal = ((target - l(1.0f)) * l(p.real(ps::kWeaponPosFrac)) + l(1.0f)).store();
    p.set_real(ps::kHoldBreathScale,
               shared::math::diff_track(goal, p.real(ps::kHoldBreathScale), rate, pml.frame_time()));
}

}  // namespace iw_pc::game::weapon
