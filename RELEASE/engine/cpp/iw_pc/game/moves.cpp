#include "iw_pc/game/moves.h"

#include "iw_pc/game/slide.h"
#include "iw_pc/shared/math.h"

namespace iw_pc::game::moves {

using x87::F24;
namespace pmv = iw_pc::game::pmove;

void air_move(pmv::Pmove& pm, pmv::MoveLocal& pml, const pmv::MoveSettings& settings, trace::MoveHost& host) {
    namespace ml = pmv::pml;
    auto l = F24::load;
    pmv::friction(pm.ps, pml, settings);
    const std::uint8_t* cmd = pm.cmd();
    float fmove = static_cast<float>(static_cast<std::int8_t>(cmd[0x18]));
    float smove = static_cast<float>(static_cast<std::int8_t>(cmd[0x19]));
    float scale = pmv::cmd_scale(pm.ps, cmd, settings).store();

    auto forward = pml.vec3(ml::kForward), right = pml.vec3(ml::kRight);
    forward[2] = 0.0f;
    right[2] = 0.0f;
    shared::math::vec3_normalize(forward);
    shared::math::vec3_normalize(right);
    pml.set_vec3(ml::kForward, forward);
    pml.set_vec3(ml::kRight, right);

    std::array<float, 3> wish{};
    for (std::size_t i = 0; i < 2; ++i) wish[i] = (l(smove) * l(right[i]) + l(fmove) * l(forward[i])).store();
    float wish_speed = (l(shared::math::vec3_normalize(wish)) * l(scale)).store();
    pmv::accelerate(pm.ps, pml, wish, wish_speed, 1.0f, settings);

    if (pml.integer(ml::kGroundPlane) != 0) {  // along the surface (its own order of the dot product)
        auto n = pml.vec3(ml::kGroundNormal);
        auto v = pm.ps.velocity();
        F24 backoff = l(n[0]) * l(v[0]) + l(v[2]) * l(n[2]) + l(v[1]) * l(n[1]);
        F24 change = -(backoff - backoff.abs() * l(0.001f));
        std::array<float, 3> out{};
        for (std::size_t i = 0; i < 3; ++i) out[i] = (change * l(n[i]) + l(v[i])).store();
        pm.ps.set_velocity(out);
    }

    slide::step_slide_move(pm, pml, true, settings, host);
    pmv::set_movement_dir(pm, pml);
}

}  // namespace iw_pc::game::moves
