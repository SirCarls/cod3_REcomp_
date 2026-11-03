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
    // [claude-9b-think] 2026-11-02 UNTESTED PmoveSingle 0x824B39B8 calls it as the campaign's 0x824960D0 calls
    // 0x8248DF18: r3 = pm->ps, r4 = pm + 4 (the usercmd), r5 = pm + 52, r6 = pml (0x82C74880) + 40 (msec),
    // r7 = pm->+328 (call at 0x824B3E78).
    .update_view_angles = 0x824AC818,
    .collision_context_vtable = 0x82065E50,
    .step_clip_return = {0x824A8C14, 0x824A8D7C},  // PM_ClipVelocity 0x824879F8 calls in the step move
    .pm = 0x82B922C8,
    .pml = 0x82C74880,
    .kbuttons = 0x82B53E20,
    .no_stamina_drain = 0x82B91B74,
    // [claude-9b-think] 2026-11-02 UNTESTED mp Com_Frame 0x82557D60 runs CL_Frame 0x8252E2E0 once per active local
    // player with this = the player (FINDINGS-MPTEST.md B3); menus_mp.cpp reads it as kActiveClient. The campaign's
    // is 0x82A2A1C8. Valid on the client side only: the game's ClientThink runs outside that loop (see
    // mp_fn::kMpClientThink below for the controller of a Pmove call).
    .client_active_index = 0x82BCBE9C,
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
            // [claude-9b-think] 2026-11-02 UNTESTED the campaign's 140 plus this playerState's 32 extra bytes (as
            // view_height_target 184 -> 216 and flags2 208 -> 240); the stage-2 log prints it (0 to 1 while aiming)
            // so T1 confirms it before any rule reads it.
            .binoculars_frac = 172,
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
    // [claude-9b-think] 2026-11-02 UNTESTED Pmove is 0x824B4270 (callers 0x82592B98 and 0x82592CD0, the game's
    // ClientThink side only: the client game has no movement prediction, it replays the view angles alone in
    // 0x82465000 / 0x82464D38). Its loop is the campaign's 0x82496978 instruction for instruction: pm + 312
    // pmove_fixed, pm + 316 pmove_msec (the step's cap), 666 ms per PmoveSingle without it, at most 1000 ms a call.
    // Differences: no 50 ms limit for pm_flags 0x80000, and pm_flags 0x100000 sends the whole call to 0x824AA438
    // (the hooks must leave such a call to the game). pm + 100 is the tracemask (PmoveSingle clears 0x02000000 in it,
    // as the campaign's); the box (mins / maxs) is not checked yet and stays 0.
    .pmove = {.ps = 0,
              .cmd = 4,
              .tracemask = 100,
              .mins = 0,
              .maxs = 0,
              .pmove_fixed = 312,
              .pmove_msec = 316,
              .view_arg = 52,
              .view_flag = 328},
    .cmd = {.server_time = 0, .buttons = 4, .forwardmove = 24, .rightmove = 25, .upmove = 26},
    .pml_fields = {.forward = 0, .msec = 40, .weapon_def = 176},
    .trace = {.endpos = 0, .normal = 16, .fraction = 32, .allsolid = 60, .startsolid = 61, .size = 128},
    .weapon_sprint_pose = 0,  // to find (the multiplayer WeaponDef)
    .weapon_stand_move_from_sprint = 0,
    .spawn_sprint_scale = 1.9167f,     // ClientSpawn 0x82593608 (constant 0x82085AC0)
    .default_alternate_movement = 0,   // a server's default: everyone moves as on the console
    .sprint_is_a_setting = false,      // multiplayer always has sprint
};

// [claude-9b-think] 2026-11-02 UNTESTED The multiplayer program's movement functions the stage-2 hooks attach to
// (hooks_mp.cpp; the campaign's hooks_sp.cpp names its own directly). Found by their callers, which match the
// campaign's one for one (generated code, 2026-11-02):
//   PM_UpdateSprint   0x8248E970  6 calls from PmoveSingle 0x824B39B8 (campaign 0x824655B0: 5 from 0x824960D0),
//                                 each third in the block 0x824A7770, 0x82480AC0, it (campaign 0x82488D20, 0x82465718, it)
//   PM_UpdateStamina  0x82495980  6 calls from PmoveSingle, each after 0x8248EBC8 (campaign 0x82477F28 after 0x824657E8)
//   PM_CheckJump      0x82495B58  callers 0x824A9D28 and 0x824AB0C8 (campaign 0x82478100: 0x82490E78 and 0x82492038)
//   PM_AirMove        0x824A9798  callers 0x824A9D28, 0x824AB0C8 and PmoveSingle, last before PM_GroundTrace
//                                 (campaign 0x82490B70: 0x82490E78, 0x82492038 and 0x824960D0)
//   ClientThink_real  0x82592CD0  r3 = the entity; + 512: its gclient (+ 1932 == 2: connected); calls Pmove itself
//                                 and through 0x82592B98: every Pmove call of a local player runs inside it
//   local players' entities: ReadU32(ReadU32(0x82BCC7FC) + 4 * (controller + 1)) (FINDINGS-MPTEST.md A,
//                                 cod3_test_autojoin's TeamNow)
namespace mp_fn {
inline constexpr uint32_t kPmUpdateSprint = 0x8248E970;
inline constexpr uint32_t kPmUpdateStamina = 0x82495980;
inline constexpr uint32_t kPmCheckJump = 0x82495B58;
inline constexpr uint32_t kPmAirMove = 0x824A9798;
inline constexpr uint32_t kMpClientThink = 0x82592CD0;
inline constexpr uint32_t kLocalEntities = 0x82BCC7FC;
}  // namespace mp_fn

}  // namespace cod3::movement
