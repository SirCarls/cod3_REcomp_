// cod3 - the multiplayer program (codmp_xenonf.xex) for the movement code
// (program.h). For cod3mp (the multiplayer port, built from the same sources
// by recomp/mp/CMakeLists.txt, which doesn't list the movement files yet):
// the campaign never uses this table, but compiles it so it stays in step
// with program.h.
//
// Read in the disassembly of xex/codmp.pe (dev/agents/movement/PLAN.md section 2.4,
// evidence/mp_*.txt). The multiplayer program has the campaign's movement
// code; its playerState has 32 more bytes before the speed scales. A 0 is
// not found yet: cod3mp fills it in before using the part that needs it
// (each use in the shared code says which field it reads).

#pragma once

#include "game/movement/program.h"

namespace cod3::movement {

inline constexpr Program kMultiplayer = {
    .name = "multiplayer",
    .in_key_down = 0x82525B10,         // IN_SprintDown 0x825273A8 tail-calls it with kbuttons + 360
    .in_key_up = 0x82523530,           // IN_SprintUp 0x82525810 tail-calls it
    .in_holdbreath_down = 0,           // to find (cod3mp's +holdbreath)
    .in_holdbreath_up = 0,             // to find
    .sprint_breath_events = 0x8248EB28,
    .player_trace = 0x824A1828,        // called by PM_StepSlideMove 0x824A87E0 like the campaign's
    .update_view_angles = 0,  // to find (PM_UpdateViewAngles is 0x824AC818; its arguments not checked)
    .collision_context_vtable = 0x82065E50,
    .step_clip_return = {0x824A8C14, 0x824A8D7C},  // PM_ClipVelocity 0x824879F8 calls in the step move
    .pm = 0x82B922C8,
    .pml = 0x82C74880,
    .kbuttons = 0x82B53E20,
    .no_stamina_drain = 0x82B91B74,
    .client_active_index = 0,          // to find
    .aim_toggle = 0,                   // to find (the client's toggle_cl_run array)
    .ps =
        {
            .origin = 0,
            .velocity = 16,
            .command_time = 32,
            .pm_type = 36,
            .pm_flags = 44,
            .pm_time = 48,
            .jump_time = 112,
            .jump_origin_z = 116,
            .pass_entity = 0,          // to find
            .view_angles = 0,          // to find
            .binoculars_frac = 0,      // to find
            .view_height_target = 216,
            .prone_view_height = 1108,
            .crouch_view_height = 1112,
            .flags2 = 240,
            .scale_ads = 1124,
            .scale_run = 1128,
            .scale_sprint = 1132,
            .scale_prone = 1136,
            .scale_crouch = 1140,
            .scale_strafe = 1144,
            .scale_back = 1148,
            .stamina = 1168,
            .last_sprint_time = 1172,
            .event_sequence = 244,
            .events = 248,
            .event_parms = 264,
        },
    // tracemask / box / pmove_fixed / the view-angle arguments: to find
    // (0 keeps CoD4's 125 fps steps off in cod3mp).
    .pmove = {.ps = 0,
              .cmd = 4,
              .tracemask = 0,
              .mins = 0,
              .maxs = 0,
              .pmove_fixed = 0,
              .pmove_msec = 0,
              .view_arg = 0,
              .view_flag = 0},
    .cmd = {.server_time = 0, .buttons = 4, .forwardmove = 24, .rightmove = 25, .upmove = 26},
    .pml_fields = {.forward = 0, .msec = 40, .weapon_def = 176},
    .trace = {.endpos = 0, .normal = 16, .fraction = 32, .allsolid = 60, .startsolid = 61, .size = 128},
    .weapon_sprint_pose = 0,  // to find (the multiplayer WeaponDef)
    .weapon_stand_move_from_sprint = 0,
    .spawn_sprint_scale = 1.9167f,     // ClientSpawn 0x82593608 (constant 0x82085AC0)
    .default_alternate_movement = 0,   // a server's default: everyone moves as on the console
    .sprint_is_a_setting = false,      // multiplayer always has sprint
};

}  // namespace cod3::movement
