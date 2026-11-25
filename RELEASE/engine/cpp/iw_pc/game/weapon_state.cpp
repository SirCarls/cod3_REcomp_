#include "iw_pc/game/weapon_state.h"

#include "iw_pc/game/weapon.h"

namespace iw_pc::game::weapon_state {

namespace pmv = iw_pc::game::pmove;
namespace ps = pmv::ps;
namespace fl = pmv::pm_flags;
namespace def = weapon::def;

namespace {
constexpr std::int32_t kAnimToggleBit = 0x200;

// Where word `index` of the array at `base` is in the player state, if it is (see the Rust version).
bool word_at(std::size_t base, std::int32_t index, std::size_t& at) {
    std::int64_t a = static_cast<std::int64_t>(base) + static_cast<std::int64_t>(index) * 4;
    if (a < 0 || a + 4 > static_cast<std::int64_t>(pmv::kPlayerStateSize)) return false;
    at = static_cast<std::size_t>(a);
    return true;
}
std::int32_t word(const pmv::PlayerState& p, std::size_t base, std::int32_t index) {
    std::size_t at;
    return word_at(base, index, at) ? p.integer(at) : 0;
}
bool holds(const pmv::PlayerState& p, std::int32_t weapon) {
    return (static_cast<std::uint32_t>(word(p, ps::kWeapons, weapon >> 5)) & (std::uint32_t{1} << (weapon & 31))) != 0;
}
const weapon::WeaponDef& def_of(const trace::MoveHost& host, const pmv::PlayerState& p, std::size_t at) {
    return host.weapon_def(p.integer(at));
}
void raise_after(pmv::PlayerState& p, const trace::MoveHost& host) {
    if (p.integer(ps::kWeapon) == 0) {
        p.set_integer(ps::kWeaponTime, 0);
        p.set_integer(ps::kWeaponDelay, 1);
    } else {
        p.set_integer(ps::kWeaponTime, def_of(host, p, ps::kWeapon).integer(def::kQuickRaiseTime));
        p.set_integer(ps::kWeaponDelay, 0);
        start_weapon_anim(p, weap_anim::kQuickRaise);
    }
}
void reset(pmv::PlayerState& p) {
    p.set_integer(ps::kPmFlags, p.pm_flags() & ~(fl::kAdsHold | fl::kOffhand));
    std::int32_t st = p.integer(ps::kWeaponState);
    if (st >= 0x11 && st <= 0x16) exit_binoculars(p);
    p.set_integer(ps::kWeaponTime, 0);
    p.set_integer(ps::kWeaponDelay, 0);
    p.set_integer(ps::kWeaponState, 0);
    start_weapon_anim(p, weap_anim::kIdle);
}
std::int32_t add(std::int32_t a, std::int32_t b) {
    return static_cast<std::int32_t>(static_cast<std::uint32_t>(a) + static_cast<std::uint32_t>(b));
}
std::int32_t sub(std::int32_t a, std::int32_t b) {
    return static_cast<std::int32_t>(static_cast<std::uint32_t>(a) - static_cast<std::uint32_t>(b));
}
}  // namespace

void start_weapon_anim(pmv::PlayerState& p, std::int32_t anim) {
    if (p.pm_type() < pmv::pm_type::kDead) p.set_integer(ps::kWeapAnim, (~p.integer(ps::kWeapAnim) & kAnimToggleBit) | anim);
}

void continue_weapon_anim(pmv::PlayerState& p, std::int32_t anim) {
    if ((p.integer(ps::kWeapAnim) & ~kAnimToggleBit) != anim) start_weapon_anim(p, anim);
}

std::int32_t ammo_in_clip(const pmv::PlayerState& p, const trace::MoveHost& host) {
    return word(p, ps::kAmmoClip, def_of(host, p, ps::kWeapon).integer(def::kClipIndex));
}

std::int32_t first_offhand_of_class(const pmv::PlayerState& p, std::int32_t cls, const trace::MoveHost& host) {
    std::int32_t count = host.weapon_count();
    for (std::int32_t w = 1; w <= count; ++w) {
        const weapon::WeaponDef& d = host.weapon_def(w);
        if (d.integer(def::kOffhandClass) == cls && holds(p, w) &&
            add(word(p, ps::kAmmoClip, d.integer(def::kClipIndex)), word(p, ps::kAmmo, d.integer(def::kAmmoIndex))) != 0)
            return w;
    }
    return 0;
}

bool is_weapon_valid(const pmv::PlayerState& p, std::int32_t weapon, const trace::MoveHost& host) {
    bool valid = weapon >= 0 && weapon <= host.weapon_count();
    if (!holds(p, weapon)) valid = false;
    const weapon::WeaponDef& d = host.weapon_def(weapon);
    if (d.integer(def::kOffhandClass) == 0) {
        auto primary = [&](std::size_t k) { return static_cast<std::int32_t>(static_cast<std::int8_t>(p.bytes[ps::kPrimaryWeapons + k])); };
        if (primary(0) != weapon && primary(1) != weapon && d.integer(def::kAltWeaponIndex) != weapon) valid = false;
    }
    return valid;
}

void finish_weapon_raise(pmv::PlayerState& p) {
    p.set_integer(ps::kWeaponState, 0);
    start_weapon_anim(p, weap_anim::kIdle);
}

void finish_reload_end(pmv::PlayerState& p) {
    p.set_integer(ps::kWeaponState, 0);
    start_weapon_anim(p, weap_anim::kIdle);
}

void finish_rechamber(pmv::PlayerState& p) {
    p.set_integer(ps::kWeaponState, 0);
    continue_weapon_anim(p, weap_anim::kIdle);
}

void melee_end(pmv::PlayerState& p) {
    p.set_integer(ps::kWeaponState, 0);
    continue_weapon_anim(p, weap_anim::kIdle);
}

void melee_fire(pmv::PlayerState& p, const trace::MoveHost& host) {
    const weapon::WeaponDef& d = def_of(host, p, ps::kWeapon);
    std::int32_t time = sub(d.integer(def::kMeleeTime), d.integer(def::kMeleeDelay));
    if (p.integer(ps::kWeaponTime) < time) p.set_integer(ps::kWeaponTime, time);
    pmv::add_predictable_event(ev::kMelee, 0, p);
    p.set_integer(ps::kWeaponState, 0xB);
    std::int32_t flags = p.pm_flags();
    if (flags & fl::kProne) p.set_integer(ps::kPmFlags, flags | fl::kAdsHold);
}

void offhand_init(pmv::PlayerState& p, const trace::MoveHost& host) {
    std::int32_t time = def_of(host, p, ps::kWeapon).integer(def::kQuickDropTime);
    p.set_integer(ps::kWeaponState, 0xC);
    p.set_integer(ps::kWeaponTime, time);
    p.set_integer(ps::kPmFlags, p.pm_flags() & ~fl::kOffhand);
    p.set_integer(ps::kWeaponDelay, 0);
    pmv::add_predictable_event(ev::kAdsExit, 0, p);
    p.set_integer(ps::kPmFlags, p.pm_flags() & ~fl::kAds);
    start_weapon_anim(p, weap_anim::kQuickDrop);
}

void offhand_prepare(pmv::PlayerState& p, const trace::MoveHost& host) {
    std::int32_t offhand = p.integer(ps::kOffhandIndex);
    std::int32_t time = host.weapon_def(offhand).integer(def::kHoldFireTime);
    p.set_integer(ps::kWeaponState, 0xD);
    p.set_integer(ps::kWeaponTime, time);
    p.set_integer(ps::kPmFlags, p.pm_flags() | fl::kOffhand);
    p.set_integer(ps::kWeaponDelay, 0);
    pmv::add_predictable_event(ev::kOffhandPrepare, offhand & 0xFF, p);
    start_weapon_anim(p, weap_anim::kOffhandPrepare);
    std::int32_t flags = p.pm_flags();
    if (flags & fl::kProne) p.set_integer(ps::kPmFlags, flags | fl::kAdsHold);
}

void offhand_hold(pmv::PlayerState& p, const trace::MoveHost& host) {
    std::int32_t fuse = def_of(host, p, ps::kOffhandIndex).integer(def::kFuseTime);
    p.set_integer(ps::kWeaponTime, 0);
    p.set_integer(ps::kWeaponDelay, 0);
    p.set_integer(ps::kPmFlags, p.pm_flags() | fl::kOffhand);
    p.set_integer(ps::kWeaponState, 0xE);
    p.set_integer(ps::kGrenadeTimeLeft, fuse);
}

void offhand_end(pmv::PlayerState& p, const trace::MoveHost& host) {
    raise_after(p, host);
    std::int32_t flags = p.pm_flags() & ~(fl::kAdsHold | fl::kOffhand);
    p.set_integer(ps::kWeaponState, 0x10);
    p.set_integer(ps::kPmFlags, flags);
}

void offhand_empty(pmv::PlayerState& p) { pmv::add_predictable_event(ev::kOffhandEmpty, 0, p); }

void binoculars_drop_weapon(pmv::PlayerState& p, const trace::MoveHost& host) {
    std::int32_t time = def_of(host, p, ps::kWeapon).integer(def::kQuickDropTime);
    p.set_integer(ps::kWeaponState, 0x12);
    p.set_integer(ps::kWeaponTime, time);
    p.set_integer(ps::kWeaponDelay, 0);
    start_weapon_anim(p, weap_anim::kQuickDrop);
}

void binoculars_raise(pmv::PlayerState& p, const trace::MoveHost& host) {
    std::int32_t time = host.weapon_def(weapon::binocular_weapon_index(host)).integer(def::kAdsTransInTime);
    p.set_integer(ps::kPmFlags, p.pm_flags() | fl::kBinoculars);
    p.set_integer(ps::kWeaponState, 0x13);
    p.set_integer(ps::kWeaponTime, time);
    p.set_integer(ps::kWeaponDelay, 0);
    pmv::add_predictable_event(ev::kBinocularsRaise, 0, p);
    start_weapon_anim(p, weap_anim::kIdle);
}

void binoculars_lower(pmv::PlayerState& p, const pmv::MoveSettings& settings, const trace::MoveHost& host) {
    const weapon::WeaponDef& d = host.weapon_def(weapon::binocular_weapon_index(host));
    p.set_integer(ps::kWeaponState, 0x15);
    p.set_integer(ps::kWeaponTime, add(d.integer(def::kAdsTransOutTime), settings.ads_exit_delay));
    p.set_integer(ps::kWeaponDelay, 0);
    pmv::add_predictable_event(ev::kBinocularsLower, 0, p);
    start_weapon_anim(p, weap_anim::kIdle);
}

void binoculars_drop(pmv::Pmove& pm, const trace::MoveHost& host) {
    pmv::PlayerState& p = pm.ps;
    p.set_integer(ps::kPmFlags, p.pm_flags() & ~fl::kBinoculars);
    p.set_integer(ps::kWeaponState, 0x16);
    pmv::add_predictable_event(ev::kBinocularsDrop, 0, p);
    if (pm.bytes[pmv::pm::kCmd + pmv::cmd::kWeapon] == 0) {
        p.set_integer(ps::kWeaponTime, 0);
        p.set_integer(ps::kWeaponDelay, 1);
    } else {
        p.set_integer(ps::kWeaponTime, def_of(host, p, ps::kWeapon).integer(def::kQuickRaiseTime));
        p.set_integer(ps::kWeaponDelay, 0);
        start_weapon_anim(p, weap_anim::kQuickRaise);
    }
}

void binoculars_exit_sights(pmv::PlayerState& p, const pmv::MoveSettings& settings, const trace::MoveHost& host) {
    std::int32_t out = def_of(host, p, ps::kWeapon).integer(def::kAdsTransOutTime);
    pmv::add_predictable_event(ev::kAdsExit, 0, p);
    p.set_integer(ps::kPmFlags, p.pm_flags() & ~fl::kAds);
    p.set_integer(ps::kWeaponTime, 0);
    p.set_integer(ps::kWeaponDelay, add(out, settings.ads_exit_delay));
    p.set_integer(ps::kWeaponState, 0x11);
}

void exit_binoculars(pmv::PlayerState& p) {
    p.set_integer(ps::kWeaponPosFrac, 0);
    pmv::add_predictable_event(ev::kAdsExit, 0, p);
    p.set_integer(ps::kPmFlags, p.pm_flags() & ~(fl::kBinoculars | fl::kAds));
    pmv::add_predictable_event(ev::kBinocularsDrop, 0, p);
}

void reset_weapon_state(pmv::PlayerState& p) { reset(p); }

void weapon_idle(pmv::PlayerState& p) { reset(p); }

std::int32_t fire_button_released(pmv::Pmove& pm, std::int32_t delayed_action) {
    if ((pm.bytes[pmv::pm::kCmd + pmv::cmd::kButtons] & pmv::button::kAttack) != 0 || delayed_action != 0) return 0;
    pmv::PlayerState& p = pm.ps;
    if (p.integer(ps::kWeaponState) == weapon::state::kFiring) continue_weapon_anim(p, weap_anim::kIdle);
    p.set_integer(ps::kWeaponState, 0);
    return 1;
}

bool grenade_cook_off(pmv::PlayerState& p, const pmv::MoveLocal& pml, const trace::MoveHost& host) {
    if ((p.pm_flags() & fl::kOffhand) == 0) return false;
    const weapon::WeaponDef& d = def_of(host, p, ps::kOffhandIndex);
    if (d.integer(def::kWeapType) != kWeaptypeGrenade || p.integer(ps::kGrenadeTimeLeft) <= 0) return false;
    if (d.integer(def::kCookOffHold) != 0)
        p.set_integer(ps::kGrenadeTimeLeft, sub(p.integer(ps::kGrenadeTimeLeft), pml.integer(pmv::pml::kMsec)));
    if (p.integer(ps::kGrenadeTimeLeft) > 0) return false;
    std::size_t at;
    if (word_at(ps::kAmmoClip, d.integer(def::kClipIndex), at)) p.set_integer(at, sub(p.integer(at), 1));
    pmv::add_predictable_event(ev::kGrenadeCookedOff, 0, p);
    return true;
}

std::int32_t reset_unless_busy(pmv::PlayerState& p) {
    std::int32_t st = p.integer(ps::kWeaponState);
    if ((st >= 0 && st <= 2) || (st >= 4 && st <= 9)) return 1;
    if (st == 3 || (st >= 0xB && st <= 0x10)) return 0;
    reset_weapon_state(p);
    return 1;
}

}  // namespace iw_pc::game::weapon_state
