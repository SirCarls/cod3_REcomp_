#include "iw_pc/game/mantle.h"

#include <cmath>

#include "iw_pc/net/msg.h"
#include "iw_pc/shared/math.h"
#include "iw_pc/x87.h"

namespace iw_pc::game::mantle {

using x87::F24;

namespace {
/// x by step while more(x), each step stored; no progress ends it (where the original loops for ever).
template <class More>
float turn_while(float x, float step, More more) {
    while (more(x)) {
        float next = (F24::load(x) + F24::load(step)).store();
        if (next == x) break;
        x = next;
    }
    return x;
}
}  // namespace

void mantle_cap_view(pmove::PlayerState& ps, const MantleSettings& settings) {
    namespace p = pmove::ps;
    auto l = F24::load;
    if (!settings.enable) return;
    float mantle_yaw = ps.real(p::kMantleYaw);
    float delta = shared::math::angle_normalize180((l(mantle_yaw) - l(ps.real(p::kViewAngles + 4))).store());
    float cap = settings.view_yaw_cap;
    float least = (-l(cap)).store();
    bool below = delta < least || std::isnan(delta) || std::isnan(least);
    if (!below && !(delta > cap || std::isnan(delta) || std::isnan(cap))) return;
    delta = turn_while(delta, cap, [&](float d) { return d < least; });
    delta = turn_while(delta, -cap, [&](float d) { return d > cap; });
    auto turn = static_cast<std::int32_t>(net::ftol2((l(delta) * l(182.04445f)).v)) & 0xFFFF;
    ps.set_integer(p::kDeltaYaw,
                   static_cast<std::int32_t>(static_cast<std::uint32_t>(ps.integer(p::kDeltaYaw)) + static_cast<std::uint32_t>(turn)));
    float limit = delta > 0.0f ? least : cap;
    ps.set_real(p::kViewAngles + 4, shared::math::angle_wrap360((l(limit) + l(mantle_yaw)).store()));
}

}  // namespace iw_pc::game::mantle
