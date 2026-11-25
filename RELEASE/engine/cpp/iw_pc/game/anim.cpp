#include "iw_pc/game/anim.h"

#include "iw_pc/game/weapon.h"
#include "iw_pc/x87.h"

namespace iw_pc::game::anim {

namespace {
std::int64_t condition_at(std::int32_t condition) { return static_cast<std::int64_t>(kConditions) + condition * 8LL; }
}  // namespace

void update_condition_value(ClientAnimInfo& info, std::int32_t condition, std::int32_t value, bool check_conversion) {
    std::int64_t at = condition_at(condition);
    bool bits = condition >= 0 && static_cast<std::size_t>(condition) < kNumAnimConditions &&
                kAnimConditionTypes[condition] == kAnimCondTypeBitflags;
    if (check_conversion && bits) {
        info.set_integer(static_cast<std::size_t>(at + 4), 0);
        info.set_integer(static_cast<std::size_t>(at), 0);
        std::int64_t word = at + static_cast<std::int64_t>(value >> 5) * 4;
        if (word >= 0 && word + 4 <= static_cast<std::int64_t>(kClientAnimInfoSize)) {
            auto w = static_cast<std::size_t>(word);
            info.set_integer(w, static_cast<std::int32_t>(static_cast<std::uint32_t>(info.integer(w)) |
                                                          (std::uint32_t{1} << (value & 31))));
        }
    } else {
        info.set_integer(static_cast<std::size_t>(at), value);
    }
}

void update_player_state_conditions(const pmove::Pmove& pm, trace::MoveHost& host) {
    namespace ps = pmove::ps;
    const pmove::PlayerState& p = pm.ps;
    std::int32_t w = weapon::view_weapon(p, host);
    const weapon::WeaponDef& d = host.weapon_def(w);
    std::int32_t anim_type = d.integer(weapon::def::kPlayerAnimType), cls = d.integer(weapon::def::kWeapClass);
    std::int32_t e_flags = p.integer(ps::kEFlags);
    bool underhand = x87::F24::load(p.real(ps::kViewAngles)).v > 0.0;  // (only above; NaN isn't)
    bool firing = (pm.cmd()[pmove::cmd::kButtons] & pmove::button::kAttack) != 0;
    ClientAnimInfo& info = host.client_anim_info(p.integer(ps::kClientNum));
    update_condition_value(info, kAnimCondPlayerAnimType, anim_type, true);
    update_condition_value(info, kAnimCondWeaponClass, cls, true);
    update_condition_value(info, kAnimCondWeaponPosition, (e_flags & pmove::kEFlagsAds) != 0 ? 1 : 0, true);
    update_condition_value(info, kAnimCondMounted, (e_flags & pmove::kEFlagsMounted) != 0 ? 1 : 0, true);
    update_condition_value(info, kAnimCondUnderhand, underhand ? 1 : 0, true);
    update_condition_value(info, kAnimCondFiring, firing ? 1 : 0, true);
}

}  // namespace iw_pc::game::anim
