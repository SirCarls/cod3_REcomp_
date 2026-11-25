//! Climbing over obstacles (mantling; Call of Duty 4's `bg_mantle.cpp`),
//! Call of Duty 2's version (see [`super::pmove`] for the conventions).

use super::pmove::{ps, PlayerState};
use crate::net::msg::ftol2;
use crate::shared::math::{angle_normalize180, angle_wrap360};
use crate::x87::F24;

/// The mantle's settings (the server's dvars of these names).
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct MantleSettings {
    /// `mantle_enable` (1).
    pub enable: bool,
    /// `mantle_view_yawcap` (60, 0 to 180): how far the view may turn from
    /// the climb's direction.
    pub view_yaw_cap: f32,
}

impl Default for MantleSettings {
    fn default() -> Self {
        MantleSettings {
            enable: true,
            view_yaw_cap: 60.0,
        }
    }
}

/// `x` by `step` while `more(x)`, each step stored; no progress ends it
/// (where the original loops for ever: a cap of 0).
fn turn_while(mut x: f32, step: f32, more: impl Fn(f32) -> bool) -> f32 {
    while more(x) {
        let next = (F24::load(x) + F24::load(step)).store();
        if next == x {
            break;
        }
        x = next;
    }
    x
}

/// Keeps the view within `mantle_view_yawcap` of the climb's direction:
/// beyond it (the turn `delta` from the view to the climb's yaw, to (-180,
/// 180]), `delta` comes back by whole caps into [-cap, cap], the
/// command's angles move by it (`delta_angles` yaw + `ANGLE2SHORT(delta)`)
/// and the view's yaw becomes the climb's less the cap (`delta` above 0)
/// or plus it, wrapped to [0, 360). Nothing with `mantle_enable` off.
///
/// Original: `Mantle_CapView` at 0x00512B80 (CoD2MP_s.exe 1.3; Call of Duty 4's name; ps in esi)
pub fn mantle_cap_view(ps: &mut PlayerState, settings: &MantleSettings) {
    let l = F24::load;
    if !settings.enable {
        return;
    }
    let mantle_yaw = ps.float(ps::MANTLE_YAW);
    let mut delta = angle_normalize180((l(mantle_yaw) - l(ps.float(ps::VIEW_ANGLES + 4))).store());
    let cap = settings.view_yaw_cap;
    let least = (-l(cap)).store();
    // (`fcomp`, test ah,1: below -cap (or NaN); then test ah,0x41: above
    // the cap, or NaN; else nothing.)
    let below = delta < least || delta.is_nan() || least.is_nan();
    if !below && !(delta > cap || delta.is_nan() || cap.is_nan()) {
        return;
    }
    // (test ah,5: on while below; test ah,0x41: back while above.)
    delta = turn_while(delta, cap, |d| d < least);
    delta = turn_while(delta, -cap, |d| d > cap);
    let turn = ftol2((l(delta) * l(182.044_45)).0) as i32 & 0xFFFF;
    let yaw = ps.int(ps::DELTA_YAW).wrapping_add(turn);
    ps.set_int(ps::DELTA_YAW, yaw);
    // (`fcomp` 0, test ah,0x41: only above takes -cap.)
    let limit = if delta > 0.0 { least } else { cap };
    ps.set_float(
        ps::VIEW_ANGLES + 4,
        angle_wrap360((l(limit) + l(mantle_yaw)).store()),
    );
}
