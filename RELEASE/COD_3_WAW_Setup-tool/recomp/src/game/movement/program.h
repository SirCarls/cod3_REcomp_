// cod3 - the movement code's view of one game program (campaign or multiplayer).
//
// The movement code (game/movement/*) is shared by the campaign program
// (default.xex, this tree) and the multiplayer program (codmp_xenonf.xex,
// cod3mp). The two programs have the same player-movement code at different
// addresses, with some playerState fields 32 bytes apart. Everything the
// shared code reads or calls in the game is listed in one Program table per
// program:
//   program_sp.h  the campaign (built and tested here)
//   program_mp.h  the multiplayer program (for cod3mp; addresses from the
//                 disassembly, see dev/agents/movement/PLAN.md section 2.4)
// program_current.h picks the one this program is built with. No other
// movement file holds a game address.

#pragma once

#include <cstdint>

namespace cod3::movement {

// playerState_t field offsets (bytes from the playerState).
struct PlayerStateOffsets {
  uint32_t origin;              // vec3
  uint32_t velocity;            // vec3
  uint32_t command_time;        // int, the last usercmd's serverTime
  uint32_t pm_type;             // int: 0 normal, 2 noclip, 3 ufo, 4+ dead
  uint32_t pm_flags;            // int, PmFlag below
  uint32_t pm_time;             // int, ms
  uint32_t jump_time;           // int, serverTime of the last jump
  uint32_t jump_origin_z;       // float, where the jump started
  // int: what the game's player traces pass as the entity to skip
  // (PM_StepSlideMove's collision context +4). Not a client index: 16385
  // (0x4001) for the campaign's player (cloud run m4, 2026-10-16).
  uint32_t pass_entity;
  uint32_t view_angles;         // vec3 (pitch, yaw, roll)
  uint32_t binoculars_frac;     // float, 1.0 while looking through binoculars
  uint32_t view_height_target;  // int
  uint32_t prone_view_height;   // int
  uint32_t crouch_view_height;  // int
  uint32_t flags2;              // int; kFlags2NoMove: turret or vehicle use
  // Per-player speed scales, set at spawn (ClientSpawn); level scripts may
  // change them. The movement code multiplies the walk speed by them.
  uint32_t scale_ads;
  uint32_t scale_run;
  uint32_t scale_sprint;  // forward part only, while sprinting
  uint32_t scale_prone;
  uint32_t scale_crouch;
  uint32_t scale_strafe;
  uint32_t scale_back;
  uint32_t stamina;            // float, 0..1
  uint32_t last_sprint_time;   // int, serverTime of the last sprinting frame
  uint32_t event_sequence;     // int
  uint32_t events;             // int[4]
  uint32_t event_parms;        // int[4]
};

// pmove_t (the movement call's argument; the `pm` global points at it).
struct PmoveOffsets {
  uint32_t ps;         // playerState_t*
  uint32_t cmd;        // usercmd_t (48 bytes)
  uint32_t tracemask;  // int
  uint32_t mins;       // vec4, the player's box
  uint32_t maxs;       // vec4
  // int pmove_fixed / pmove_msec as ClientThink copies them from the dvars:
  // Pmove's own fixed-step loop (steps of pmove_msec). 0 = not known for
  // this program (CoD4's 125 fps steps stay off).
  uint32_t pmove_fixed;
  uint32_t pmove_msec;
  // PmoveSingle's other two PM_UpdateViewAngles arguments: a pointer into
  // pmove_t and an int read from it.
  uint32_t view_arg;
  uint32_t view_flag;
};

// usercmd_t field offsets (from the usercmd).
struct UsercmdOffsets {
  uint32_t server_time;  // int
  uint32_t buttons;      // int, Button below
  uint32_t forwardmove;  // signed char, -127..127
  uint32_t rightmove;    // signed char
  uint32_t upmove;       // signed char (jump >= 10)
};

// pml_t (the movement code's per-call locals, a global).
struct PmlOffsets {
  uint32_t forward;     // vec3, the view's forward vector
  uint32_t msec;        // int, this step's length
  uint32_t weapon_def;  // WeaponDef* of the current weapon
};

// trace_t, as PM_PlayerTrace fills it.
struct TraceOffsets {
  uint32_t endpos;     // vec3 (16-byte aligned)
  uint32_t normal;     // vec3 of the plane hit
  uint32_t fraction;   // float, 1.0 = nothing hit
  uint32_t allsolid;   // byte
  uint32_t startsolid; // byte
  uint32_t size;       // bytes to reserve
};

struct Program {
  const char* name;

  // --- game functions (guest addresses) -----------------------------------
  uint32_t in_key_down;          // IN_KeyDown(kbutton_t*, int key, int time)
  uint32_t in_key_up;            // IN_KeyUp(kbutton_t*, int key, int time)
  uint32_t in_holdbreath_down;   // IN_HoldbreathDown(key, time)
  uint32_t in_holdbreath_up;     // IN_HoldbreathUp(key, time)
  uint32_t sprint_breath_events; // PM_SprintBreathEvents(): breathing while out of breath
  uint32_t player_trace;         // PM_PlayerTrace(trace*, start, mins, maxs, end, context*)
  // PM_UpdateViewAngles(ps, usercmd*, pm + view_arg, msec, pm->view_flag);
  // reads the `pm` global. 0 = not known (CoD4's 125 fps steps stay off).
  uint32_t update_view_angles;
  uint32_t collision_context_vtable;  // player_collision_context_t::vftable
  // The two PM_ClipVelocity calls in PM_StepSlideMove's step down, as return
  // addresses (where CoD4 projects the velocity instead: bounces).
  uint32_t step_clip_return[2];

  // --- game data ------------------------------------------------------------
  uint32_t pm;                   // pmove_t* global (valid inside Pmove)
  uint32_t pml;                  // pml_t global
  uint32_t kbuttons;             // kbutton_t array (24 bytes each)
  uint32_t no_stamina_drain;     // int; never written by the game, honored as it does
  uint32_t client_active_index;  // int, the local client being handled
  uint32_t aim_toggle;           // int per local client: 1 = not aiming (toggle_cl_run)

  PlayerStateOffsets ps;
  PmoveOffsets pmove;
  UsercmdOffsets cmd;
  PmlOffsets pml_fields;
  TraceOffsets trace;

  // --- values ---------------------------------------------------------------
  uint32_t weapon_sprint_pose;   // WeaponDef: sprintMoveF/R/U (3 floats; the gun's offset while sprinting)
  int32_t weapon_stand_move_from_sprint;  // WeaponDef standMoveF/R/U, from sprintMoveF (debug log)
  float spawn_sprint_scale;      // ClientSpawn's sprint scale (forward)
  int default_alternate_movement;  // 2 in the campaign (the player is the host), 0 in multiplayer
  bool sprint_is_a_setting;      // campaign: cod3_sprint; multiplayer: always on
};

// The game's constants the shared code needs (the same in both programs).
inline constexpr uint32_t kKbuttonSize = 24;
inline constexpr int kSprintKbutton = 15;  // "+sprint" in the multiplayer program -> usercmd bit 2

namespace button {
inline constexpr uint32_t kAttack = 1u << 0;
inline constexpr uint32_t kSprint = 1u << 2;
inline constexpr uint32_t kAds = 1u << 3;
}  // namespace button

namespace pmf {
inline constexpr uint32_t kProne = 0x1;
inline constexpr uint32_t kDucked = 0x2;
inline constexpr uint32_t kJumpHeld = 0x8;
inline constexpr uint32_t kLadder = 0x10;
inline constexpr uint32_t kAds = 0x80;
inline constexpr uint32_t kJumping = 0x2000;  // landing slowdown pending
inline constexpr uint32_t kSprinting = 0x10000;
inline constexpr uint32_t kOutOfBreath = 0x20000;
}  // namespace pmf

inline constexpr uint32_t kFlags2NoMove = 0x106000;  // using a turret or a vehicle
inline constexpr int kEventSprintBreath = 163;       // the "out of breath" sound event
inline constexpr int kPmNormal = 0;
inline constexpr int kPmNoclip = 2;
inline constexpr int kPmDead = 4;

// Key ids the port uses as holders of a kbutton (real key numbers below 256
// the pad never sends; the keybinds code uses AUX1/AUX2).
inline constexpr int kKeyAux3 = 241;   // the sprint toggle's latch
inline constexpr int kKeyPadL3 = 220;  // JOY14: the pad's left stick click

}  // namespace cod3::movement
