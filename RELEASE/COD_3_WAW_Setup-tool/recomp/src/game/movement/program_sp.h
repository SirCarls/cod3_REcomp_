// cod3 - the campaign program (default.xex) for the movement code (program.h).
//
// Every address was read in the disassembly (notes in dev/agents/movement/;
// research/symbols.toml for the named ones). The campaign moves players only
// on its server: Pmove 0x82496978 has one caller, ClientThink_real
// 0x8256C260; the client game predicts view angles only.

#pragma once

#include "game/movement/program.h"

namespace cod3::movement {

inline constexpr Program kCampaign = {
    .name = "campaign",
    .in_key_down = 0x8250BB50,
    .in_key_up = 0x8250A110,
    .in_holdbreath_down = 0x8250CD30,
    .in_holdbreath_up = 0x8250CCF8,
    .sprint_breath_events = 0x82471320,
    .player_trace = 0x8248BE90,
    .update_view_angles = 0x8248DF18,  // PmoveSingle 0x824960D0 calls it first
    .collision_context_vtable = 0x820678D8,
    .step_clip_return = {0x8248FB70, 0x8248FCD8},
    .pm = 0x829F1040,
    .pml = 0x82A922B0,
    .kbuttons = 0x829C8E68,
    .no_stamina_drain = 0x829F08EC,
    .client_active_index = 0x82A2A1C8,
    .aim_toggle = 0x829C3E78,
    .ps =
        {
            .origin = 0,
            .velocity = 16,
            .command_time = 32,
            .pm_type = 36,
            .pm_flags = 44,
            .pm_time = 48,
            .jump_time = 116,
            .jump_origin_z = 120,
            .pass_entity = 128,
            .view_angles = 172,
            .binoculars_frac = 140,
            .view_height_target = 184,
            .prone_view_height = 1076,
            .crouch_view_height = 1080,
            .flags2 = 208,
            .scale_ads = 1092,
            .scale_run = 1096,
            .scale_sprint = 1100,
            .scale_prone = 1104,
            .scale_crouch = 1108,
            .scale_strafe = 1112,
            .scale_back = 1116,
            .stamina = 1136,
            .last_sprint_time = 1140,
            .event_sequence = 212,
            .events = 216,
            .event_parms = 232,
        },
    // pmove_fixed / pmove_msec: Pmove 0x82496978's loop (ClientThink_real
    // keeps pmove_msec within 8..33); view_arg / view_flag: PmoveSingle's call
    // of PM_UpdateViewAngles (r5 = pm + 52, r7 = pm->+328).
    .pmove = {.ps = 0,
              .cmd = 4,
              .tracemask = 100,
              .mins = 272,
              .maxs = 288,
              .pmove_fixed = 312,
              .pmove_msec = 316,
              .view_arg = 52,
              .view_flag = 328},
    .cmd = {.server_time = 0, .buttons = 4, .forwardmove = 24, .rightmove = 25, .upmove = 26},
    .pml_fields = {.forward = 0, .msec = 40, .weapon_def = 176},
    .trace = {.endpos = 0, .normal = 16, .fraction = 32, .allsolid = 60, .startsolid = 61, .size = 128},
    .weapon_sprint_pose = 1260,
    .weapon_stand_move_from_sprint = 24,  // standMoveF at +1284 (CG_WeaponMove 0x82437490)
    .spawn_sprint_scale = 1.6f,
    .default_alternate_movement = 2,
    .sprint_is_a_setting = true,
};

}  // namespace cod3::movement
