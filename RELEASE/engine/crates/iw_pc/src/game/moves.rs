//! The kinds of move (`bg_pmove.c`'s `PM_*Move`): how a command's
//! forward and right moves become velocity in the air, on the ground, on a
//! ladder ..., Call of Duty 2's versions (see [`super::pmove`] for the
//! conventions).

use super::pmove::{
    accelerate, cmd_scale, friction, pml, set_movement_dir, MoveLocal, MoveSettings, Pmove,
    USERCMD_SIZE,
};
use super::slide::step_slide_move;
use super::trace::MoveHost;
use crate::shared::math::vec3_normalize;
use crate::x87::F24;

/// A command's forward and right moves (signed bytes at 0x18 and 0x19) as
/// `float`s.
fn moves(pm: &Pmove) -> (f32, f32) {
    let cmd = pm.cmd();
    (f32::from(cmd[0x18] as i8), f32::from(cmd[0x19] as i8))
}

/// The move in the air: friction, then the command's moves along the
/// view's level forward and right (normalized), at the command's scale
/// ([`cmd_scale`]), accelerated at 1 (`pm_airaccelerate`); on a surface
/// the velocity first slides along it; then the step slide move with
/// gravity, and the movement direction.
///
/// Original: `PM_AirMove` at 0x00515FA0 (CoD2MP_s.exe 1.3; Quake III's name; pml in eax, pm on the stack)
pub fn air_move(
    pm: &mut Pmove,
    pml: &mut MoveLocal,
    settings: &MoveSettings,
    host: &mut dyn MoveHost,
) {
    let l = F24::load;
    friction(&mut pm.ps, pml, settings);
    let (fmove, smove) = moves(pm);
    let cmd: [u8; USERCMD_SIZE] = pm.cmd().try_into().expect("a command's bytes");
    let scale = cmd_scale(&pm.ps, &cmd, settings).store();

    let mut forward = pml.vec3(pml::FORWARD);
    let mut right = pml.vec3(pml::RIGHT);
    forward[2] = 0.0;
    right[2] = 0.0;
    vec3_normalize(&mut forward);
    vec3_normalize(&mut right);
    pml.set_vec3(pml::FORWARD, forward);
    pml.set_vec3(pml::RIGHT, right);

    let mut wish = [0f32; 3];
    for i in 0..2 {
        wish[i] = (l(smove) * l(right[i]) + l(fmove) * l(forward[i])).store();
    }
    let wish_speed = (F24::load(vec3_normalize(&mut wish)) * l(scale)).store();
    accelerate(&mut pm.ps, pml, &wish, wish_speed, 1.0, settings);

    if pml.int(pml::GROUND_PLANE) != 0 {
        // Along the surface (its own order of the dot product).
        let n = pml.vec3(pml::GROUND_NORMAL);
        let v = pm.ps.velocity();
        let backoff = l(n[0]) * l(v[0]) + l(v[2]) * l(n[2]) + l(v[1]) * l(n[1]);
        let change = -(backoff - backoff.abs() * l(0.001));
        let mut out = v;
        for i in 0..3 {
            out[i] = (change * l(n[i]) + l(v[i])).store();
        }
        pm.ps.set_velocity(out);
    }

    step_slide_move(pm, pml, true, settings, host);
    set_movement_dir(pm, pml);
}
