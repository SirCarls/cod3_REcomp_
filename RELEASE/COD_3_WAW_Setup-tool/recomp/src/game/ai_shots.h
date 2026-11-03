// cod3 - [claude-9b-think] 2026-11-02 UNTESTED cod3_debug_ai_shots (campaign): each AI shot near the player, with
// its guest call stack, what decided it, its aim error and its trace (ai_shots.cpp).
#pragma once

#include <cstdint>

struct PPCContext;

namespace cod3::ai_shots {

// Around Bullet_Fire (0x82567A58; its hook is in ai_census.cpp): an AI shot with the player within 2000 units is
// followed through its trace and logged when it ends.
class BulletScope {
 public:
  BulletScope(PPCContext& ctx, uint8_t* base);
  ~BulletScope();
  BulletScope(const BulletScope&) = delete;
  BulletScope& operator=(const BulletScope&) = delete;

 private:
  uint8_t* base_;
  bool active_;
  bool any_;  // [r2] counted by cod3_debug_ai_spawns (any AI shot)
};

// After 0x823DBAC8(actor) returned hit (1) or miss (0) (its hook is in ai_aim.cpp).
void OnHitOrMiss(uint8_t* base, uint32_t actor, uint32_t result);

// [r1] Around one usercmd (ClientThink_real's hook in ai_census.cpp): the 10 s lines, and what runs inside a
// usercmd is told apart from what runs in the game update.
class UsercmdScope {
 public:
  explicit UsercmdScope(uint8_t* base);
  ~UsercmdScope();
  UsercmdScope(const UsercmdScope&) = delete;
  UsercmdScope& operator=(const UsercmdScope&) = delete;
};

// [r1] G_Damage(target) (its hook is in ai_census.cpp): the followed shot hurt the player.
void OnDamage(uint8_t* base, uint32_t target);

// [r2] 2026-11-02 cod3_debug_ai_spawns: once a game update (the script scheduler's hook in ai_census.cpp), the
// sentients by team: alive, arrived, left.
void EachUpdate(uint8_t* base);

// [r3] 2026-11-02 cod3_debug_ai_accuracy: around 0x823ED9B8 (ActorCmd_Shoot / ShootAt; its hook is in ai_aim.cpp),
// what became of the shot command.
class ShootScope {
 public:
  ShootScope(PPCContext& ctx, uint8_t* base);
  ~ShootScope();
  ShootScope(const ShootScope&) = delete;
  ShootScope& operator=(const ShootScope&) = delete;

 private:
  bool active_;
};

// [r3] After 0x823D7E18 (Pmove's sample of the player for the AI; its hook is in ai_aim.cpp): `same_update` when the
// newest sample already had this update's level.time, `refreshed` when cod3_fix_ai_player_history took it again.
void OnPlayerSample(uint8_t* base, bool same_update, bool refreshed);

}  // namespace cod3::ai_shots
