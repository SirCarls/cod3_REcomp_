//! The animation state the movement keeps for each player: the conditions
//! the animation scripts test (`bg_animation_mp.c`'s `clientConditions`).
//! Call of Duty 2 keeps them in each client's animation state, which the
//! host holds ([`super::trace::MoveHost::client_anim_info`]).

use super::pmove::{button, cmd, ps, Pmove, E_FLAGS_ADS, E_FLAGS_MOUNTED};
use super::trace::MoveHost;
use super::weapon::{self, def};
use crate::x87::F24;

/// Bytes of a client's animation state (Call of Duty 2's `clientInfo_t`
/// in the animation state, as far as the movement knows it).
pub const CLIENT_ANIM_INFO_SIZE: usize = 0x4B8;

/// Where the conditions start in it: two words each.
pub const CONDITIONS: usize = 0x45C;

/// The conditions (`animScriptConditionTypes_t`; Call of Duty 4's names,
/// whose order Call of Duty 2's table at 0x005D8DD0 follows).
pub const ANIM_COND_PLAYERANIMTYPE: i32 = 0;
pub const ANIM_COND_WEAPONCLASS: i32 = 1;
pub const ANIM_COND_MOUNTED: i32 = 2;
pub const ANIM_COND_MOVETYPE: i32 = 3;
pub const ANIM_COND_UNDERHAND: i32 = 4;
pub const ANIM_COND_CROUCHING: i32 = 5;
pub const ANIM_COND_FIRING: i32 = 6;
pub const ANIM_COND_WEAPON_POSITION: i32 = 7;
pub const ANIM_COND_STRAFING: i32 = 8;
/// How many there are (`NUM_ANIM_CONDITIONS`).
pub const NUM_ANIM_CONDITIONS: usize = 9;

/// A condition kept as bits, one per value (`ANIM_CONDTYPE_BITFLAGS`), or
/// as the value (`ANIM_CONDTYPE_VALUE`).
pub const ANIM_CONDTYPE_BITFLAGS: i32 = 0;
pub const ANIM_CONDTYPE_VALUE: i32 = 1;

/// Each condition's kind: the first words of the game's
/// `animConditionsTable` (0x005D8DD0, 8 bytes each; the data the game
/// starts with, which nothing changes).
pub const ANIM_CONDITION_TYPES: [i32; NUM_ANIM_CONDITIONS] = [0, 0, 1, 0, 1, 1, 1, 1, 1];

/// A client's animation state: the game's structure as its bytes.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ClientAnimInfo {
    pub bytes: Vec<u8>,
}

impl Default for ClientAnimInfo {
    fn default() -> Self {
        ClientAnimInfo {
            bytes: vec![0; CLIENT_ANIM_INFO_SIZE],
        }
    }
}

impl ClientAnimInfo {
    /// The state from its bytes (zeros after the end of a shorter slice).
    pub fn from_bytes(b: &[u8]) -> Self {
        let mut info = ClientAnimInfo::default();
        let n = b.len().min(CLIENT_ANIM_INFO_SIZE);
        info.bytes[..n].copy_from_slice(&b[..n]);
        info
    }

    pub fn int(&self, at: usize) -> i32 {
        let b = &self.bytes[at..at + 4];
        i32::from_le_bytes([b[0], b[1], b[2], b[3]])
    }

    pub fn set_int(&mut self, at: usize, v: i32) {
        self.bytes[at..at + 4].copy_from_slice(&v.to_le_bytes());
    }

    /// Condition `condition`'s two words.
    pub fn condition(&self, condition: i32) -> [i32; 2] {
        let at = condition_at(condition);
        [self.int(at), self.int(at + 4)]
    }
}

fn condition_at(condition: i32) -> usize {
    (CONDITIONS as i64 + i64::from(condition) * 8) as usize
}

/// A condition of the client's animation state set to `value`: with
/// `check_conversion`, a condition kept as bits gets its two words cleared
/// and bit `value` set (the bit's word counted from the condition's first;
/// the original writes outside the state for a value outside it, which the
/// port leaves alone); else the first word is the value.
///
/// Original: `BG_UpdateConditionValue` at 0x004F9360 (CoD2MP_s.exe 1.3; Wolfenstein: Enemy Territory's name; the client in eax, the condition in edx, the value in ecx, checkConversion on the stack)
pub fn update_condition_value(
    info: &mut ClientAnimInfo,
    condition: i32,
    value: i32,
    check_conversion: bool,
) {
    let at = condition_at(condition);
    let bits = usize::try_from(condition)
        .ok()
        .and_then(|c| ANIM_CONDITION_TYPES.get(c))
        .is_some_and(|&t| t == ANIM_CONDTYPE_BITFLAGS);
    if check_conversion && bits {
        info.set_int(at + 4, 0);
        info.set_int(at, 0);
        let word = at as i64 + i64::from(value >> 5) * 4;
        if let Ok(word) = usize::try_from(word) {
            if word + 4 <= CLIENT_ANIM_INFO_SIZE {
                let v = info.int(word) | 1 << (value & 31);
                info.set_int(word, v);
            }
        }
    } else {
        info.set_int(at, value);
    }
}

/// The conditions the player's state gives this move: the weapon in
/// hand's `playerAnimType` and `weapClass`, aiming down the sights (the
/// eFlags bit), on a mounted weapon, the view pitched down (`UNDERHAND`),
/// firing (each as [`update_condition_value`] with conversion).
///
/// Original: `BG_AnimUpdatePlayerStateConditions` at 0x004F94B0 (CoD2MP_s.exe 1.3; Wolfenstein: Enemy Territory's name; pm on the stack)
pub fn update_player_state_conditions(pm: &Pmove, host: &mut dyn MoveHost) {
    let ps = &pm.ps;
    let weapon = weapon::view_weapon(ps, host);
    let (anim_type, class) = {
        let d = host.weapon_def(weapon);
        (d.int(def::PLAYER_ANIM_TYPE), d.int(def::WEAP_CLASS))
    };
    let e_flags = ps.int(ps::E_FLAGS);
    // (`fcomp` 0, test ah,0x41, jne: only above is underhand; NaN isn't.)
    let underhand = F24::load(ps.float(ps::VIEW_ANGLES)) > F24::load(0.0);
    let firing = pm.cmd()[cmd::BUTTONS] as i32 & button::ATTACK != 0;
    let info = host.client_anim_info(ps.int(ps::CLIENT_NUM));
    update_condition_value(info, ANIM_COND_PLAYERANIMTYPE, anim_type, true);
    update_condition_value(info, ANIM_COND_WEAPONCLASS, class, true);
    let ads = i32::from(e_flags & E_FLAGS_ADS != 0);
    update_condition_value(info, ANIM_COND_WEAPON_POSITION, ads, true);
    let mounted = i32::from(e_flags & E_FLAGS_MOUNTED != 0);
    update_condition_value(info, ANIM_COND_MOUNTED, mounted, true);
    update_condition_value(info, ANIM_COND_UNDERHAND, i32::from(underhand), true);
    update_condition_value(info, ANIM_COND_FIRING, i32::from(firing), true);
}
