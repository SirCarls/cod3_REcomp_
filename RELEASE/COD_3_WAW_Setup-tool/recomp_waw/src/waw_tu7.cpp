// waw - World at War's title update 7, the parts the map packs' zombie maps need, redone as our own code on the disc
// program. [map-compat]
//
// Owner, 2026-11-11 09:33: "We need to re-create the title update so it won't be required." His rule of 11-09 17:28
// stands: the port keeps running the disc's program, and what the update did is written again here, in our own words,
// behind settings of its own. No update file is needed, loaded or shipped. What the update changed and why each piece
// is here: dev/agents/map-compat/TU7.md (the update was studied on the PC only).
//
// Step 1, the script builtins. The three map-pack zombie maps (Verrückt, Shi No Numa, Der Riese) call 15 builtins the
// disc doesn't have; a call to an unknown builtin stops the level's scripts from compiling. Each one gets a guest
// address from the runtime (a thunk to the host function below), and the four lookups the script compiler makes (server
// functions and methods, client-script functions and methods) find our names before the disc's tables. The builtins
// work through the disc's own script functions (parameters, results, errors), called as the game calls them.
//
// The perks are kept on the host side, not in guest structures: the update appended its perk word to playerState_s
// (+0x20A8, where the disc keeps the word ClientScr_SetSessionState uses), so they live here, per player, cleared at
// map start and when a player spawns. The per-player visibility mask is the disc's own: gentity_s +0x120/+0x124,
// which the disc's setvisibletoplayer() (GScr_setvisibletoplayer 0x821D37E8) already sets and its snapshot code
// (0x822C1E98) already honors; the update's two new builtins write the same two words, and so do ours.
//
// Further down: step 3, Speed Cola's and Double Tap's timing in the weapon code; step 4, the electric shock and
// teleporter screen effects (the server commands setelectrified/settransported send, and the client showing them).
//
// [acct2-mods] 2026-11-14 22:30, taken over on the owner's word (to the question whether to take over map-compat's
// title update 7 and map-pack work: "yes. We need to replicate that patch without actually needing it."):
//   - a parameter's text was read wrongly: 0x8232C508 is the disc's Scr_GetString (it gets the parameter's string id
//     with 0x8232C318 and returns its text through 0x8231BAC8 itself); the draft passed that text to 0x8231BAC8 again
//     as if it were an id, so every perk name read as garbage. Script::String now calls 0x8232C508 alone.
//   - two builtins PC mods' scripts call and neither the disc nor the update has (dev/agents/acct2-mods/NOTES.md,
//     20:20-20:45): the entity method isnotarget() and the client scripts' getsubstr(). spawncollision() is still to
//     write (done 2026-11-23, below).
//   - 22:50: three client-script builtins Shi No Numa's and Der Riese's client scripts call and the draft's list
//     missed (NOTES.md 22:35-22:45): getcurrentweapon, getweaponammoclip, isthrowinggrenade. Each does for the local
//     player's predicted playerState what the disc's own player method of the same name does for a player's on the
//     server (PlayerCmd_getCurrentWeapon 0x821AA8C0, PlayerCmd_GetWeaponAmmoClip 0x821ADC88,
//     PlayerCmd_IsThrowingGrenade 0x821AD590); the playerState is one structure on both sides.
//   - 23:10: Shi No Numa's client scripts still stopped at "unknown function" with every name known: the patch
//     zone's clientscripts/_callbacks.csc calls playsound as a method (ent playsound(...)), and the disc has it only
//     as a function. The compiler looks functions and methods up in different tables (p0/gsccheck.py now reads each
//     call with its form). Read that way, the three maps' client scripts lack four more: the method playsound, the
//     functions getvisionsetnaked and playfxontag and the method linkto (the last three Der Riese's). playsound and
//     getvisionsetnaked are written here from the disc's own code; playfxontag and linkto are checked and do
//     nothing yet (the effect isn't played, the entity doesn't follow), so that the level compiles and runs.
//   - 23:40: playfxontag plays its effect: the disc's own player of an effect on an entity's tag (0x8210E840, the
//     one its entity event for the server's playfxontag() calls) with the client script's effect, entity and tag.
//     linkto is still checked only.
//
// [waw-decomp/mods2] 2026-11-23: spawncollision(), the third PC builtin two community maps call (Das Herrenhaus,
// Clinic Of Evil), written from the PC game's own (dev/waw-decomp/pc_sp, GScr_spawncollision) with the disc's spawn
// functions; see Builtin_SpawnCollision.

#include <array>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/memory.h>
#include <rex/system.h>
#include <rex/system/function_dispatcher.h>

#include "generated/default/waw_pch.h"

REXCVAR_DEFINE_BOOL(waw_tu7_builtins, true, "waw",
    "Title update 7's script builtins, redone (hasperk, setperk, setinvisibletoplayer, getaivelocity, the client "
    "scripts' isads...): the map packs' zombie maps call them; without them their scripts don't compile");

REXCVAR_DEFINE_BOOL(waw_tu7_perk_effects, true, "waw",
    "Title update 7's perk effects in the weapon code, redone: Speed Cola (specialty_fastreload) shortens reloads and "
    "Double Tap (specialty_rof) speeds up firing, for a player the scripts gave the perk with setperk()");
REXCVAR_DEFINE_DOUBLE(waw_tu7_perk_reload_multiplier, 0.5, "waw",
    "Speed Cola: the share of a reload's time it takes (title update 7's perk_weapReloadMultiplier, 0.5 there; "
    "0 reloads at once)");
REXCVAR_DEFINE_DOUBLE(waw_tu7_perk_rate_multiplier, 0.75, "waw",
    "Double Tap: the share of the time between shots it takes (title update 7's perk_weapRateMultiplier, 0.75 there; "
    "0 fires at once)");

REXCVAR_DEFINE_BOOL(waw_tu7_client_effects, true, "waw",
    "Title update 7's electric shock and teleporter screen effects, redone: the zombie maps' electric traps "
    "(setelectrified) and Der Riese's teleporters (settransported) tell the player's client to show them; off, the "
    "client prints its unknown-command line instead, as the disc does");

// The game's script functions these builtins use (the disc program's addresses; instance 0 is the server's scripts,
// 1 the client scripts').
DECLARE_REX_FUNC(sub_82326400);  // Scr_GetNumParam(inst)
DECLARE_REX_FUNC(sub_8232BE10);  // Scr_GetInt(index, inst)
DECLARE_REX_FUNC(sub_8232C208);  // Scr_GetFloat(index, inst): f1
DECLARE_REX_FUNC(sub_8232C508);  // Scr_GetString(index, inst): the parameter's text (its id, then SL_ConvertToString)
DECLARE_REX_FUNC(sub_82326758);  // Scr_AddString(text, inst)
DECLARE_REX_FUNC(sub_821FDEC8);  // Scr_GetEntity(index): the gentity (server)
DECLARE_REX_FUNC(sub_82326418);  // Scr_AddInt(value, inst)
DECLARE_REX_FUNC(sub_82326918);  // Scr_AddVector(vector, inst)
DECLARE_REX_FUNC(sub_82326D10);  // Scr_Error(message, inst): leaves the builtin (longjmp to the VM's error frame)
DECLARE_REX_FUNC(sub_82214B28);  // the actor's velocity (actor, out vector)
DECLARE_REX_FUNC(sub_822EED00);  // the local client's predicted playerState (localClientNum)
DECLARE_REX_FUNC(sub_822476F0);  // SV_GameSendServerCommand(clientNum, type, text)
DECLARE_REX_FUNC(sub_821005D0);  // CG_Burn(localClientNum, ms): the flame screen effect starts
DECLARE_REX_FUNC(sub_82100660);  // the flame screen effect ends (localClientNum): it fades out
DECLARE_REX_FUNC(sub_82158FB8);  // DB_FindXAssetHeader(type, name, 1, -1)
DECLARE_REX_FUNC(sub_82155EF0);  // DB_IsXAssetDefault(type, name)
DECLARE_REX_FUNC(sub_82208AC0);  // BG_FindWeaponIndexForName(name): the weapon's index, 0 for none
DECLARE_REX_FUNC(sub_820EBEA0);  // the clip a weapon index loads from (its index into the playerState's clip counts)
DECLARE_REX_FUNC(sub_8232C748);  // Scr_GetVector(index, out vector, inst)
// CG_PlaySoundAliasByName(localClientNum, entity number, origin, alias): the sound's id
DECLARE_REX_FUNC(sub_822F0050);
DECLARE_REX_FUNC(sub_8232C950);  // Scr_GetEntityRef(out, index, inst): the parameter's scr_entref_t (its address)
DECLARE_REX_FUNC(sub_8232C3D0);  // Scr_GetConstLowercaseString(index, inst): the parameter's string id, lower case
DECLARE_REX_FUNC(sub_8231BAC8);  // SL_ConvertToString(id, inst): a string id's text
// SL_FindLowercaseString(text, inst): the id of a string the game already has, 0 if none
DECLARE_REX_FUNC(sub_8231BF90);
// an effect played on an entity's tag (localClientNum, effect, entity number, tag name id)
DECLARE_REX_FUNC(sub_8210E840);
// spawncollision()'s: the disc's own spawn path, as its spawn() builtin (GScr_Spawn 0x821D2960) uses it.
DECLARE_REX_FUNC(sub_8232C318);  // Scr_GetConstString(index, inst): the parameter's string id
DECLARE_REX_FUNC(sub_82204220);  // G_Spawn(): a free gentity
DECLARE_REX_FUNC(sub_8231CDC8);  // Scr_SetString(field, id, inst): stores a string id with its reference
DECLARE_REX_FUNC(sub_822037C8);  // G_SetModel(ent, name): the model's index into ent +0x198 (an error if unknown)
DECLARE_REX_FUNC(sub_821FDBF0);  // G_CallSpawnEntity(ent): runs the spawn function for the entity's class
DECLARE_REX_FUNC(sub_821FDE18);  // Scr_AddEntity(ent): returns the entity to the script

namespace {

constexpr uint32_t kServer = 0;
constexpr uint32_t kClient = 1;

// g_entities on the disc (Scr_GetEntity 0x821FDEC8: entnum * 0x378 + 0x82918F00) and the gentity fields used here;
// the update's builtins read the same fields at the same offsets (its gentity is the disc's).
constexpr uint32_t kGEntities = 0x82918F00;
constexpr uint32_t kGEntitySize = 0x378;
constexpr uint32_t kMaxEntities = 1024;
constexpr uint32_t kEntityType = 0x4;           // s.eType
constexpr uint32_t kEntityFlags = 0x8;          // s.lerp.eFlags
constexpr uint32_t kEntityWeapon = 0xE0;        // s.weapon
constexpr uint32_t kEntityOwner = 0x178;        // the owner's handle: its entity number + 1, 0 for none
constexpr uint32_t kEntityClient = 0x180;       // gclient_t, 0 for non-players
constexpr uint32_t kEntityActor = 0x188;        // actor_s, 0 for non-actors
constexpr uint32_t kEntityInUse = 0x11D;        // r.inuse (a byte)
constexpr uint32_t kEntityExplodeTime = 0x1C4;  // a missile's detonation time (level time, ms)
constexpr uint32_t kEntityHiddenFrom = 0x120;   // two words: a bit per client (0-31, 32-63) the entity isn't sent to
constexpr uint32_t kTypeMissile = 4;
// The entity's game flags and the "enemies don't see me" bit in them (Cmd_Notarget_f 0x821B1888 flips it).
constexpr uint32_t kEntityGameFlags = 0x1B4;
constexpr uint32_t kGameFlagNoTarget = 0x4;
// The longest text getsubstr() returns, with its end (the disc's own getsubstr, 0x821DBFF0, has the same room).
constexpr uint32_t kSubStrSize = 1024;
constexpr uint32_t kFlagEyes = 0x20000;  // the update's zombie-eyes flag in eFlags (its server haseyes() sets it)

// gclient_t starts with its playerState_s: clientNum at +0xF8, weaponstate at +0x108 (the weapon is at +0x104).
constexpr uint32_t kClientNum = 0xF8;
constexpr uint32_t kClientWeaponState = 0x108;
// weaponstate 1-4: raising, raising for the alternate mode, dropping, quick dropping (the update's test).
constexpr uint32_t kWeaponStateFirstSwitching = 1;
constexpr uint32_t kWeaponStateLastSwitching = 4;
// playerState_s fields the disc's own player methods read (the addresses are those methods'):
constexpr uint32_t kPlayerWeapon = 0x104;          // the weapon in hand, 0 for none (0x821AA930)
constexpr uint32_t kPlayerOtherWeapon = 0x20F4;    // a byte: the weapon named when none is in hand (0x821AA934)
constexpr uint32_t kPlayerWeaponState = 0x108;     // as kClientWeaponState
constexpr uint32_t kPlayerClipCounts = 0x5FC;      // one word per clip index: rounds in the clip (0x821ADD18)
constexpr uint32_t kWeaponStateFirstGrenade = 16;  // 16-21: a grenade being held or thrown (0x821AD604)
constexpr uint32_t kWeaponStateLastGrenade = 21;
constexpr uint32_t kMaxWeapons = 256;              // the game's weapon pool

// The weapon definitions, one pointer per weapon index (as waw_tu_fixes.cpp reads them), and the fields the update's
// resetmissiledetonationtime() reads: whether the grenade goes off on a timer, its fuse for players and for AI (ms).
constexpr uint32_t kWeaponDefs = 0x824CE208;
constexpr uint32_t kWeaponDefTimedDetonation = 0x6B0;
constexpr uint32_t kWeaponDefFuseTime = 0x4D4;
constexpr uint32_t kWeaponDefAiFuseTime = 0x4D8;

// level.time on the disc (GScr_GetTime 0x821D1CE0 returns it).
constexpr uint32_t kLevelTime = 0x82AA358C;

// The local client's playerState_s: fWeaponPosFrac (how far the weapon is raised to the sights), as the update's
// client-script isads() reads it.
constexpr uint32_t kPlayerAdsFraction = 0x110;

// The client game, one cg_t per local client: *kCgArray + localClientNum * kCgSize.
constexpr uint32_t kCgArray = 0x844C5AD0;
constexpr uint32_t kCgSize = 0x15DA38;
constexpr uint32_t kMaxLocalClients = 2;  // the campaign program's (its loops over local clients stop at 2: 0x8215AE60)
// cg_t: a naked vision set's name, 64 bytes; the function at 0x8212DCDC copies it there (0x8212DD18) and loads that
// set. Where its text comes from was not followed.
constexpr uint32_t kCgVisionNameNaked = 0x15B2B0;
constexpr uint32_t kVisionNameSize = 64;
constexpr uint32_t kVisionModeNaked = 0;  // the mode CScr_visionsetnaked passes CG_VisionSetStartLerp_To (0x822F9834)

// The entities client scripts call methods on, as the disc's own client methods find them (CScr_getorigin
// 0x822FC488): scr_entref_t comes in r3, its entity number in bits 63-48, its class in bits 47-32 and the local
// client in bits 31-16. Numbers below 1024 are the game's client entities (an array per local client), 1024 and up
// the client scripts' own "fake" entities (spawnfakeent(): 1024 per local client). Both keep their origin at +36.
constexpr uint32_t kClientEntities = 0x844C8258;  // one pointer per local client: its centity_t array
constexpr uint32_t kClientEntitySize = 712;
constexpr uint32_t kFakeEntities = 0x8433D160;    // (localClientNum * 1024 + number - 1024) * kFakeEntitySize from here
constexpr uint32_t kFakeEntitySize = 784;
constexpr uint32_t kFakeEntitiesPerClient = 1024;
constexpr uint32_t kClientEntityOrigin = 36;
// A sound of no entity: what the disc's playsound() function passes (0x822FA02C).
constexpr uint32_t kEntityNumWorld = 1022;

// The effects client scripts loaded (loadfx() gives their numbers, 1-195): one pointer each in the client game's
// static data, as the disc's playfx() finds them (0x822F8D3C): *kCgsArray + 4 * (localClientNum * kCgsWords +
// kCgsEffects + number).
constexpr uint32_t kCgsArray = 0x844C5AB4;
constexpr uint32_t kCgsWords = 8913;
constexpr uint32_t kCgsEffects = 683;
constexpr int32_t kScriptEffectsEnd = 196;

// The perks, in the update's order (multiplayer's 32 names; title update 7 brought them to the campaign program for
// the zombie maps' perk machines). Our bit for a perk is its place here.
constexpr const char* kPerkNames[] = {
    "specialty_gpsjammer",         "specialty_reconnaissance", "specialty_bulletaccuracy", "specialty_fastreload",
    "specialty_rof",               "specialty_holdbreath",     "specialty_bulletpenetration",
    "specialty_grenadepulldeath",
    "specialty_pistoldeath",       "specialty_quieter",        "specialty_longersprint",   "specialty_detectexplosive",
    "specialty_explosivedamage",   "specialty_exposeenemy",    "specialty_bulletdamage",   "specialty_extraammo",
    "specialty_twoprimaries",      "specialty_armorvest",      "specialty_fraggrenade",    "specialty_specialgrenade",
    "specialty_flakjacket",        "specialty_pin_back",       "specialty_greased_barrings", "specialty_water_cooled",
    "specialty_shades",            "specialty_gas_mask",       "specialty_fireproof",      "specialty_ordinance",
    "specialty_boost",             "specialty_leadfoot",       "specialty_quickrevive",    "specialty_altmelee",
};
static_assert(sizeof(kPerkNames) / sizeof(kPerkNames[0]) == 32);

// Host-side state (see the top of the file).
std::array<std::atomic<uint32_t>, kMaxEntities> g_perks{};  // per player entity: one bit per perk

uint32_t LoadBe32(const uint8_t* base, uint32_t address) {
  uint32_t value;
  std::memcpy(&value, base + address, sizeof(value));
  return __builtin_bswap32(value);
}

uint32_t LoadBe16(const uint8_t* base, uint32_t address) {
  uint16_t value;
  std::memcpy(&value, base + address, sizeof(value));
  return __builtin_bswap16(value);
}

void StoreBe32(uint8_t* base, uint32_t address, uint32_t value) {
  value = __builtin_bswap32(value);
  std::memcpy(base + address, &value, sizeof(value));
}

float LoadBeFloat(const uint8_t* base, uint32_t address) {
  const uint32_t bits = LoadBe32(base, address);
  float value;
  std::memcpy(&value, &bits, sizeof(value));
  return value;
}

// A guest string, copied lowercase into `out` (at most size - 1 characters); false for null or too long.
bool ReadLower(const uint8_t* base, uint32_t address, char* out, size_t size) {
  if (address == 0) return false;
  for (size_t i = 0; i < size; ++i) {
    const char c = static_cast<char>(base[address + i]);
    out[i] = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (c == '\0') return true;
  }
  return false;
}

int PerkIndex(const char* name) {
  for (int i = 0; i < 32; ++i) {
    if (std::strcmp(kPerkNames[i], name) == 0) return i;
  }
  return -1;
}

// Guest memory made once (the main thread makes the first script lookup): the text of script errors, a vector for
// results, the text of the server commands setelectrified/settransported send (the server copies it at once, as
// Scr_Error copies its message, so one buffer each is enough) and the names of the two effects' materials.
uint32_t g_error_text = 0;
uint32_t g_vector = 0;
uint32_t g_command_text = 0;
uint32_t g_substr_text = 0;   // guest room for getsubstr()'s result
uint32_t g_none_text = 0;     // "none" in guest memory: getcurrentweapon()'s answer without a weapon
uint32_t g_entref = 0;        // guest room for one scr_entref_t (Scr_GetEntityRef writes it there)
uint32_t g_spawn_vectors = 0;  // guest room for spawncollision()'s origin and angles (two vectors)
constexpr uint32_t kErrorTextSize = 256;
constexpr uint32_t kCommandTextSize = 32;
constexpr const char* kEffectMaterialNames[] = {"zombie_electric_shock_overlay", "zombie_transporter_overlay"};
std::array<uint32_t, 2> g_effect_material_names{};

// One builtin call: the context the VM called us with, and the script instance. Parameters and results go through
// the disc's functions. A builtin is a function the VM calls like any other (not a hook inside one), so the game's
// functions are called directly: they make their frames below r1, as the disc's own builtins' callees do.
struct Script {
  PPCContext& ctx;
  uint8_t* base;
  uint32_t inst;

  uint32_t NumParams() {
    ctx.r3.u64 = inst;
    sub_82326400(ctx, base);
    return ctx.r3.u32;
  }
  int32_t Int(uint32_t index) {
    ctx.r3.u64 = index;
    ctx.r4.u64 = inst;
    sub_8232BE10(ctx, base);
    return static_cast<int32_t>(ctx.r3.u32);
  }
  float Float(uint32_t index) {
    ctx.r3.u64 = index;
    ctx.r4.u64 = inst;
    sub_8232C208(ctx, base);
    return static_cast<float>(ctx.f1.f64);
  }
  uint32_t String(uint32_t index) {  // the parameter's text (guest address)
    ctx.r3.u64 = index;
    ctx.r4.u64 = inst;
    sub_8232C508(ctx, base);
    return ctx.r3.u32;
  }
  void Vector(uint32_t index, uint32_t out) {  // the parameter's three numbers, into guest memory at out
    ctx.r3.u64 = index;
    ctx.r4.u64 = out;
    ctx.r5.u64 = inst;
    sub_8232C748(ctx, base);
  }
  uint32_t Entity(uint32_t index) {  // server scripts only
    ctx.r3.u64 = index;
    sub_821FDEC8(ctx, base);
    return ctx.r3.u32;
  }
  void AddInt(int32_t value) {
    ctx.r3.u64 = static_cast<uint32_t>(value);
    ctx.r4.u64 = inst;
    sub_82326418(ctx, base);
  }
  void AddString(uint32_t text) {  // a guest string; the game copies it
    ctx.r3.u64 = text;
    ctx.r4.u64 = inst;
    sub_82326758(ctx, base);
  }
  void AddVector(uint32_t vector) {
    ctx.r3.u64 = vector;
    ctx.r4.u64 = inst;
    sub_82326918(ctx, base);
  }
  // Raises a script error. Scr_Error doesn't come back (it longjmps to the VM's error frame), so callers return right
  // after it and hold nothing that needs cleaning up when it is called.
  void Error(const char* format, ...) {
    char text[kErrorTextSize];
    va_list args;
    va_start(args, format);
    std::vsnprintf(text, sizeof(text), format, args);
    va_end(args);
    std::memcpy(base + g_error_text, text, std::strlen(text) + 1);
    ctx.r3.u64 = g_error_text;
    ctx.r4.u64 = inst;
    sub_82326D10(ctx, base);
  }
};

// The entity a server method was called on (`ent foo()`): scr_entref_t comes in the high half of r3, its entity number
// in bits 63-48 and its class in bits 47-32 (0: an entity). 0 after a script error.
uint32_t MethodEntity(Script& s) {
  const uint64_t ref = s.ctx.r3.u64;
  const uint32_t entnum = static_cast<uint32_t>(ref >> 48);
  const uint32_t classnum = static_cast<uint32_t>((ref >> 32) & 0xFFFF);
  if (classnum != 0 || entnum >= kMaxEntities) {
    s.Error("not an entity");
    return 0;
  }
  return kGEntities + entnum * kGEntitySize;
}

uint32_t EntityNumber(uint32_t ent) { return (ent - kGEntities) / kGEntitySize; }

// The player a server method was called on: its gclient_t, 0 after a script error.
uint32_t MethodPlayer(Script& s, uint32_t* entnum) {
  const uint32_t ent = MethodEntity(s);
  if (ent == 0) return 0;
  const uint32_t client = LoadBe32(s.base, ent + kEntityClient);
  *entnum = EntityNumber(ent);
  if (client == 0) {
    s.Error("entity %u is not a player", *entnum);
    return 0;
  }
  return client;
}

// The perk named by the first parameter: its bit, -1 after a script error.
int PerkParam(Script& s) {
  char name[64];
  if (!ReadLower(s.base, s.String(0), name, sizeof(name))) {
    s.Error("perk name missing or too long");
    return -1;
  }
  const int index = PerkIndex(name);
  if (index < 0) s.Error("unknown perk \"%s\"", name);
  return index;
}

void CountUse(const char* name, std::atomic<uint32_t>& uses) {
  const uint32_t n = uses.fetch_add(1, std::memory_order_relaxed) + 1;
  if (n <= 3 || n % 1000 == 0) REXLOG_INFO("waw_tu7: {} called ({}x)", name, n);
}

#define TU7_USES(name)                     \
  static std::atomic<uint32_t> uses{0}; \
  CountUse(name, uses)

// --- Server builtins ---------------------------------------------------------------------------------------------

// bbprint(...): the update's "blackbox" telemetry for Treyarch. Nothing to send it to here: it does nothing.
void Builtin_BbPrint(PPCContext& ctx, uint8_t* base) {
  (void)ctx;
  (void)base;
  TU7_USES("bbprint");
}

// player hasperk(name): 1 when the player has the perk.
void Builtin_HasPerk(PPCContext& ctx, uint8_t* base) {
  TU7_USES("hasperk");
  Script s{ctx, base, kServer};
  uint32_t entnum = 0;
  if (MethodPlayer(s, &entnum) == 0) return;
  const int perk = PerkParam(s);
  if (perk < 0) return;
  s.AddInt((g_perks[entnum].load(std::memory_order_relaxed) >> perk) & 1);
}

// player setperk(name) / player unsetperk(name).
void SetPerk(PPCContext& ctx, uint8_t* base, bool on) {
  Script s{ctx, base, kServer};
  uint32_t entnum = 0;
  if (MethodPlayer(s, &entnum) == 0) return;
  const int perk = PerkParam(s);
  if (perk < 0) return;
  if (on) {
    g_perks[entnum].fetch_or(1u << perk, std::memory_order_relaxed);
  } else {
    g_perks[entnum].fetch_and(~(1u << perk), std::memory_order_relaxed);
  }
}

void Builtin_SetPerk(PPCContext& ctx, uint8_t* base) {
  TU7_USES("setperk");
  SetPerk(ctx, base, true);
}

void Builtin_UnsetPerk(PPCContext& ctx, uint8_t* base) {
  TU7_USES("unsetperk");
  SetPerk(ctx, base, false);
}

// player isswitchingweapons(): 1 while the weapon is being raised or put away.
void Builtin_IsSwitchingWeapons(PPCContext& ctx, uint8_t* base) {
  TU7_USES("isswitchingweapons");
  Script s{ctx, base, kServer};
  uint32_t entnum = 0;
  const uint32_t client = MethodPlayer(s, &entnum);
  if (client == 0) return;
  const uint32_t state = LoadBe32(base, client + kClientWeaponState);
  s.AddInt(state >= kWeaponStateFirstSwitching && state <= kWeaponStateLastSwitching ? 1 : 0);
}

// ent setvisibletoall(): visible to every player again (the disc's mask cleared; its snapshots then send it to all).
void Builtin_SetVisibleToAll(PPCContext& ctx, uint8_t* base) {
  TU7_USES("setvisibletoall");
  Script s{ctx, base, kServer};
  const uint32_t ent = MethodEntity(s);
  if (ent == 0) return;
  StoreBe32(base, ent + kEntityHiddenFrom, 0);
  StoreBe32(base, ent + kEntityHiddenFrom + 4, 0);
}

// ent setinvisibletoplayer(player [, invisible]): hidden from that player (or shown again with 0): that player's bit in
// the disc's mask, which its snapshot code already reads.
void Builtin_SetInvisibleToPlayer(PPCContext& ctx, uint8_t* base) {
  TU7_USES("setinvisibletoplayer");
  Script s{ctx, base, kServer};
  const uint32_t ent = MethodEntity(s);
  if (ent == 0) return;
  const uint32_t player = s.Entity(0);
  const uint32_t client = player ? LoadBe32(base, player + kEntityClient) : 0;
  if (client == 0) {
    s.Error("setinvisibletoplayer: the parameter is not a player");
    return;
  }
  const uint32_t client_num = LoadBe32(base, client + kClientNum);
  const bool invisible = s.NumParams() > 1 ? s.Int(1) != 0 : true;
  if (client_num >= 64) return;
  const uint32_t word = ent + kEntityHiddenFrom + (client_num >> 5) * 4;
  const uint32_t bit = 1u << (client_num & 31);
  const uint32_t mask = LoadBe32(base, word);
  StoreBe32(base, word, invisible ? (mask | bit) : (mask & ~bit));
}

// player setelectrified(seconds) / player settransported(seconds): the update tells that player's client to show the
// electric shock or the teleporter effect for that long, with a reliable server command "%c %i": '(' or ')' and the
// milliseconds. Ours sends the same command; the client side (step 4, below) shows it.
constexpr char kCommandElectrified = '(';
constexpr char kCommandTransported = ')';
constexpr uint32_t kServerCommandReliable = 1;

void ClientEffect(PPCContext& ctx, uint8_t* base, char command) {
  Script s{ctx, base, kServer};
  const float seconds = s.Float(0);
  if (!(seconds > 0.0f)) {
    s.Error("Time must be positive");
    return;
  }
  uint32_t entnum = 0;
  if (MethodPlayer(s, &entnum) == 0) return;
  if (base[kGEntities + entnum * kGEntitySize + kEntityInUse] == 0) return;
  const int32_t ms = static_cast<int32_t>(seconds * 1000.0f);
  char text[kCommandTextSize];
  std::snprintf(text, sizeof(text), "%c %i", command, ms);
  std::memcpy(base + g_command_text, text, std::strlen(text) + 1);
  ctx.r3.u64 = entnum;
  ctx.r4.u64 = kServerCommandReliable;
  ctx.r5.u64 = g_command_text;
  sub_822476F0(ctx, base);
}

void Builtin_SetElectrified(PPCContext& ctx, uint8_t* base) {
  TU7_USES("setelectrified");
  ClientEffect(ctx, base, kCommandElectrified);
}

void Builtin_SetTransported(PPCContext& ctx, uint8_t* base) {
  TU7_USES("settransported");
  ClientEffect(ctx, base, kCommandTransported);
}

// grenade resetmissiledetonationtime(): a grenade on a timer starts its fuse again from now (the monkey bomb). The fuse
// is its weapon's player fuse when a player threw it, otherwise its AI fuse.
void Builtin_ResetMissileDetonationTime(PPCContext& ctx, uint8_t* base) {
  TU7_USES("resetmissiledetonationtime");
  Script s{ctx, base, kServer};
  const uint32_t ent = MethodEntity(s);
  if (ent == 0) return;
  if (LoadBe32(base, ent + kEntityType) != kTypeMissile) return;
  const uint32_t weapon = LoadBe32(base, ent + kEntityWeapon);
  if (weapon == 0) return;
  const uint32_t def = LoadBe32(base, kWeaponDefs + weapon * 4);
  if (def == 0 || LoadBe32(base, def + kWeaponDefTimedDetonation) == 0) return;
  const uint32_t owner = static_cast<uint32_t>(base[ent + kEntityOwner]) << 8 | base[ent + kEntityOwner + 1];
  bool by_player = false;
  if (owner != 0 && owner <= kMaxEntities) {
    by_player = LoadBe32(base, kGEntities + (owner - 1) * kGEntitySize + kEntityClient) != 0;
  }
  const uint32_t fuse = LoadBe32(base, def + (by_player ? kWeaponDefFuseTime : kWeaponDefAiFuseTime));
  StoreBe32(base, ent + kEntityExplodeTime, LoadBe32(base, kLevelTime) + fuse);
}

// actor getaivelocity(): the actor's velocity (the hellhounds' move and pain scripts use it).
void Builtin_GetAiVelocity(PPCContext& ctx, uint8_t* base) {
  TU7_USES("getaivelocity");
  Script s{ctx, base, kServer};
  const uint32_t ent = MethodEntity(s);
  if (ent == 0) return;
  const uint32_t actor = LoadBe32(base, ent + kEntityActor);
  if (actor == 0) {
    s.Error("entity %u is not an actor", EntityNumber(ent));
    return;
  }
  std::memset(base + g_vector, 0, 12);
  ctx.r3.u64 = actor;
  ctx.r4.u64 = g_vector;
  sub_82214B28(ctx, base);
  s.AddVector(g_vector);
}

// ent haseyes(on): the update's server method sets or clears the entity's eyes flag, which the client scripts'
// haseyes() reads. (The map packs' server scripts don't call it; it is here because the update has it.)
void Builtin_ServerHasEyes(PPCContext& ctx, uint8_t* base) {
  TU7_USES("haseyes (server)");
  Script s{ctx, base, kServer};
  const uint32_t ent = MethodEntity(s);
  if (ent == 0) return;
  if (s.NumParams() != 1) {
    s.Error("haseyes takes one parameter");
    return;
  }
  const bool on = s.Int(0) != 0;
  const uint32_t flags = LoadBe32(base, ent + kEntityFlags);
  StoreBe32(base, ent + kEntityFlags, on ? (flags | kFlagEyes) : (flags & ~kFlagEyes));
}

// ent isnotarget(): 1 when the entity is marked "not a target" (the notarget cheat, or a script's setting). The PC
// game has it; the disc and the update don't. PC mods' zombie scripts ask it of players.
void Builtin_IsNotarget(PPCContext& ctx, uint8_t* base) {
  TU7_USES("isnotarget");
  Script s{ctx, base, kServer};
  const uint32_t ent = MethodEntity(s);
  if (ent == 0) return;
  s.AddInt((LoadBe32(base, ent + kEntityGameFlags) & kGameFlagNoTarget) != 0 ? 1 : 0);
}

// The gentity fields spawncollision() fills before the class's spawn function runs. The disc's entity field table
// (0x820165D0) and its spawn() have them where the PC game has them: classname +0x1A0, targetname +0x1A8 (string
// ids), model +0x198 (an index), origin +0x160 and angles +0x16C (three floats each).
constexpr uint32_t kEntityClassname = 0x1A0;
constexpr uint32_t kEntityTargetname = 0x1A8;
constexpr uint32_t kEntityModel = 0x198;
constexpr uint32_t kEntitySpawnOrigin = 0x160;
constexpr uint32_t kEntitySpawnAngles = 0x16C;
constexpr uint32_t kScrConstScriptModel = 0x8300DEE6;  // scr_const.script_model: the id of "script_model"
constexpr uint32_t kFlagCollisionOnly = 0x200;          // the eFlags bit the PC's spawncollision() sets
constexpr uint32_t kXAssetXModel = 5;  // the xmodel asset type (the disc's model register 0x824001D0 asks for 5)

// spawncollision(model, targetname, origin, angles): a "script_model" entity with that model's collision, for a
// map's scripts to block a way; the entity is returned. The PC game added it in its last patch; neither the disc nor
// the update has it, and two of the community maps call it. Done as the PC game's own builtin does it (its
// GScr_spawncollision, read in dev/waw-decomp/pc_sp): the four parameters, a free entity of class script_model with
// the model, targetname, origin and angles, the class's spawn function (the disc's SP_script_model gives it the
// model's collision and links it), then eFlags bit 0x200. An empty model name is an error, as on the PC; a model
// the scripts didn't precache is the disc's own error (G_SetModel). A model that isn't in any loaded zone (a mod's
// own models aren't converted yet) gets the game's default model: the call still works, without that model's
// collision, and says so in the log once.
void Builtin_SpawnCollision(PPCContext& ctx, uint8_t* base) {
  TU7_USES("spawncollision");
  Script s{ctx, base, kServer};
  const uint32_t model = s.String(0);
  if (model == 0 || base[model] == '\0') {
    s.Error("SpawnCollision: the collision model name is empty");
    return;
  }
  ctx.r3.u64 = 1;
  ctx.r4.u64 = s.inst;
  sub_8232C318(ctx, base);
  const uint32_t targetname = ctx.r3.u32;
  s.Vector(2, g_spawn_vectors);
  s.Vector(3, g_spawn_vectors + 12);

  sub_82204220(ctx, base);
  const uint32_t ent = ctx.r3.u32;
  ctx.r3.u64 = ent + kEntityClassname;
  ctx.r4.u64 = LoadBe16(base, kScrConstScriptModel);
  ctx.r5.u64 = s.inst;
  sub_8231CDC8(ctx, base);
  ctx.r3.u64 = ent;
  ctx.r4.u64 = model;
  sub_822037C8(ctx, base);
  if (LoadBe16(base, ent + kEntityModel) == 0) {
    s.Error("SpawnCollision: collision model name %s is not valid", reinterpret_cast<const char*>(base + model));
    return;
  }
  ctx.r3.u64 = ent + kEntityTargetname;
  ctx.r4.u64 = targetname;
  ctx.r5.u64 = s.inst;
  sub_8231CDC8(ctx, base);
  std::memcpy(base + ent + kEntitySpawnOrigin, base + g_spawn_vectors, 12);
  std::memcpy(base + ent + kEntitySpawnAngles, base + g_spawn_vectors + 12, 12);
  ctx.r3.u64 = ent;
  sub_821FDBF0(ctx, base);
  StoreBe32(base, ent + kEntityFlags, LoadBe32(base, ent + kEntityFlags) | kFlagCollisionOnly);

  static std::atomic<bool> default_noted{false};
  ctx.r3.u64 = kXAssetXModel;
  ctx.r4.u64 = model;
  sub_82155EF0(ctx, base);
  if ((ctx.r3.u32 & 0xFF) != 0 && !default_noted.exchange(true)) {
    REXLOG_WARN("waw_tu7: spawncollision: model {} isn't in any loaded zone; the entity has no collision",
                reinterpret_cast<const char*>(base + model));
  }
  ctx.r3.u64 = ent;
  sub_821FDE18(ctx, base);
}

// --- Client-script builtins --------------------------------------------------------------------------------------

// The local client's predicted playerState for a client-script builtin whose first parameter is the local client
// number; 0 when the game has none for it.
uint32_t LocalPlayerState(Script& s) {
  s.ctx.r3.u64 = static_cast<uint32_t>(s.Int(0));
  sub_822EED00(s.ctx, s.base);
  return s.ctx.r3.u32;
}

// getcurrentweapon(localClientNum): the name of the weapon that local player holds, "none" without one. As the
// disc's player method: the weapon in hand, else the playerState's other weapon byte, else "none".
void Builtin_ClientGetCurrentWeapon(PPCContext& ctx, uint8_t* base) {
  TU7_USES("getcurrentweapon (client scripts)");
  Script s{ctx, base, kClient};
  if (s.NumParams() != 1) {
    s.Error("getcurrentweapon takes one parameter, the local client");
    return;
  }
  const uint32_t ps = LocalPlayerState(s);
  uint32_t weapon = 0;
  if (ps != 0) {
    weapon = LoadBe32(base, ps + kPlayerWeapon);
    if (weapon == 0) weapon = base[ps + kPlayerOtherWeapon];
  }
  uint32_t name = 0;
  if (weapon != 0 && weapon < kMaxWeapons) {
    const uint32_t def = LoadBe32(base, kWeaponDefs + 4 * weapon);
    if (def != 0) name = LoadBe32(base, def);  // the definition starts with its name
  }
  s.AddString(name != 0 ? name : g_none_text);
}

// getweaponammoclip(localClientNum, weapon name): the rounds in that weapon's clip for the local player. A weapon
// the game doesn't know gives 0 (the disc's server method raises a script error there; a client script that asks
// about a weapon the level doesn't have should go on).
void Builtin_ClientGetWeaponAmmoClip(PPCContext& ctx, uint8_t* base) {
  TU7_USES("getweaponammoclip (client scripts)");
  Script s{ctx, base, kClient};
  if (s.NumParams() != 2) {
    s.Error("getweaponammoclip takes the local client and a weapon name");
    return;
  }
  const uint32_t ps = LocalPlayerState(s);
  const uint32_t name = s.String(1);
  int32_t rounds = 0;
  if (ps != 0 && name != 0) {
    ctx.r3.u64 = name;
    sub_82208AC0(ctx, base);
    const uint32_t weapon = ctx.r3.u32;
    if (weapon != 0 && weapon < kMaxWeapons) {
      ctx.r3.u64 = weapon;
      sub_820EBEA0(ctx, base);
      const uint32_t clip = ctx.r3.u32;
      if (clip < kMaxWeapons) rounds = static_cast<int32_t>(LoadBe32(base, ps + kPlayerClipCounts + 4 * clip));
    }
  }
  s.AddInt(rounds);
}

// isthrowinggrenade(localClientNum): 1 while that local player holds or throws a grenade (weaponstate 16-21).
void Builtin_ClientIsThrowingGrenade(PPCContext& ctx, uint8_t* base) {
  TU7_USES("isthrowinggrenade (client scripts)");
  Script s{ctx, base, kClient};
  if (s.NumParams() != 1) {
    s.Error("isthrowinggrenade takes one parameter, the local client");
    return;
  }
  const uint32_t ps = LocalPlayerState(s);
  const uint32_t state = ps != 0 ? LoadBe32(base, ps + kPlayerWeaponState) : 0;
  s.AddInt(state >= kWeaponStateFirstGrenade && state <= kWeaponStateLastGrenade ? 1 : 0);
}

// getsubstr(text, start[, end]) for client scripts: the characters from start up to (not including) end, or to the
// text's end. The disc has it for server scripts only (0x821DBFF0); the PC game's client scripts have it too, and PC
// mods use it. A start past the text's end gives the empty string.
void Builtin_ClientGetSubStr(PPCContext& ctx, uint8_t* base) {
  TU7_USES("getsubstr (client scripts)");
  Script s{ctx, base, kClient};
  const uint32_t count = s.NumParams();
  if (count < 2 || count > 3) {
    s.Error("getsubstr takes a text, a start and an optional end");
    return;
  }
  const uint32_t text = s.String(0);
  const int32_t start = s.Int(1);
  const int32_t end = count == 3 ? s.Int(2) : INT32_MAX;
  if (text == 0 || start < 0) {
    s.Error("getsubstr: no text, or a negative start");
    return;
  }
  const char* from = reinterpret_cast<const char*>(base + text);
  char* to = reinterpret_cast<char*>(base + g_substr_text);
  uint32_t written = 0;
  int32_t at = 0;
  while (at < start && from[at] != '\0') ++at;  // a start past the end stops at the end
  for (; at < end && from[at] != '\0'; ++at) {
    if (written + 1 >= kSubStrSize) {
      s.Error("getsubstr: string too long");
      return;
    }
    to[written++] = from[at];
  }
  to[written] = '\0';
  s.AddString(g_substr_text);
}

// isads(localClientNum): 1 while that local player's weapon is raised towards the sights (the Wunderwaffe's scripts).
void Builtin_IsAds(PPCContext& ctx, uint8_t* base) {
  TU7_USES("isads");
  Script s{ctx, base, kClient};
  if (s.NumParams() != 1) {
    s.Error("isads takes one parameter, the local client");
    return;
  }
  const int32_t local_client = s.Int(0);
  ctx.r3.u64 = static_cast<uint32_t>(local_client);
  sub_822EED00(ctx, base);
  const uint32_t ps = ctx.r3.u32;
  s.AddInt(ps != 0 && LoadBeFloat(base, ps + kPlayerAdsFraction) > 0.0f ? 1 : 0);
}

// playviewmodelfx(localClientNum, fx, tag): an effect bolted to a tag of that local player's viewmodel (the
// Wunderwaffe's glow). Step 6 plays it; until then the call is checked and does nothing.
void Builtin_PlayViewmodelFx(PPCContext& ctx, uint8_t* base) {
  TU7_USES("playviewmodelfx");
  Script s{ctx, base, kClient};
  if (s.NumParams() != 3) {
    s.Error("playviewmodelfx takes three parameters: local client, effect, tag");
    return;
  }
}

// ent haseyes(): the client scripts' test of the eyes flag (the zombie eye glow). Step 7 reads it from the client's
// entity; until then it answers 0, so no eyes are drawn.
void Builtin_ClientHasEyes(PPCContext& ctx, uint8_t* base) {
  TU7_USES("haseyes (client)");
  Script s{ctx, base, kClient};
  s.AddInt(0);
}

// The entity a client method was called on: its number and local client from the scr_entref_t in r3 (see
// kClientEntities), and where the game keeps it; 0 when there is no such entity.
struct ClientEntity {
  uint32_t number;
  uint32_t local_client;
  uint32_t address;
};

ClientEntity MethodClientEntity(const uint8_t* base, uint64_t ref) {
  ClientEntity e{static_cast<uint32_t>(ref >> 48), static_cast<uint32_t>((ref >> 16) & 0xFFFF), 0};
  if (e.local_client >= kMaxLocalClients) return e;
  if (e.number < kMaxEntities) {
    const uint32_t entities = LoadBe32(base, kClientEntities + e.local_client * 4);
    if (entities != 0) e.address = entities + e.number * kClientEntitySize;
  } else if (e.number < kMaxEntities + kFakeEntitiesPerClient) {
    e.address = kFakeEntities + (e.local_client * kFakeEntitiesPerClient + e.number - kMaxEntities) * kFakeEntitySize;
  }
  return e;
}

// ent playsound(localClientNum, alias[, origin]): the sound alias played as that entity's sound; the sound's id.
// The disc has playsound only as a function, playsound(localClientNum, alias, origin), which plays the alias at the
// origin as a sound of no entity (CScr playsound 0x822F9FE0: CG_PlaySoundAliasByName with entity 1022). The method
// does the same with the entity: its own number when it is one of the game's client entities (the sound is then that
// entity's), 1022 for a client script's fake entity (the sound system knows no such numbers); at the origin given,
// or at the entity's own without one.
void Builtin_ClientPlaySound(PPCContext& ctx, uint8_t* base) {
  const uint64_t ref = ctx.r3.u64;  // read first: every call below uses r3
  TU7_USES("playsound (client method)");
  Script s{ctx, base, kClient};
  const uint32_t count = s.NumParams();
  if (count < 2 || count > 3) {
    s.Error("playsound on an entity takes the local client, the sound alias and, if wanted, an origin");
    return;
  }
  const ClientEntity ent = MethodClientEntity(base, ref);
  if (ent.address == 0) {
    s.Error("playsound: no such client entity");
    return;
  }
  const int32_t local_client = s.Int(0);
  const uint32_t alias = s.String(1);
  if (count == 3) {
    s.Vector(2, g_vector);
  } else {
    std::memcpy(base + g_vector, base + ent.address + kClientEntityOrigin, 12);
  }
  ctx.r3.u64 = static_cast<uint32_t>(local_client);
  ctx.r4.u64 = ent.number < kMaxEntities ? ent.number : kEntityNumWorld;
  ctx.r5.u64 = g_vector;
  ctx.r6.u64 = alias;
  sub_822F0050(ctx, base);
  s.AddInt(static_cast<int32_t>(ctx.r3.u32));
}

// The naked vision set each local client was last given, by name. The disc keeps the name only when the server's
// level sets it (cg_t +0x15B2B0); a set given by a client script (visionsetnaked()) is loaded and its name dropped.
// So the name is noted where the client scripts' sets pass: CG_VisionSetStartLerp_To (the hook is below the table).
std::mutex g_vision_mutex;
char g_vision_naked[kMaxLocalClients][kVisionNameSize] = {};

// getvisionsetnaked(localClientNum): the name of that local client's naked vision set, "" if none was set (Der
// Riese's teleporter scripts keep it to put it back after their own).
void Builtin_ClientGetVisionSetNaked(PPCContext& ctx, uint8_t* base) {
  TU7_USES("getvisionsetnaked");
  Script s{ctx, base, kClient};
  if (s.NumParams() != 1) {
    s.Error("getvisionsetnaked takes one parameter, the local client");
    return;
  }
  const int32_t local_client = s.Int(0);
  char text[kVisionNameSize] = {};
  if (local_client >= 0 && static_cast<uint32_t>(local_client) < kMaxLocalClients) {
    {
      std::lock_guard<std::mutex> lock(g_vision_mutex);
      std::memcpy(text, g_vision_naked[local_client], kVisionNameSize);
    }
    const uint32_t cgs = LoadBe32(base, kCgArray);
    if (text[0] == '\0' && cgs != 0) {  // none noted yet: the name the server's level set
      std::memcpy(text, base + cgs + static_cast<uint32_t>(local_client) * kCgSize + kCgVisionNameNaked,
                  kVisionNameSize - 1);
    }
  }
  text[kVisionNameSize - 1] = '\0';
  std::memcpy(base + g_substr_text, text, kVisionNameSize);
  s.AddString(g_substr_text);
}

// playfxontag(localClientNum, fx, entity, tag): an effect played on a tag of an entity, staying with it (Der
// Riese's client scripts). The disc has it for server scripts only: the server sends an entity event, and the client
// game's handler (CG_EntityEvent, 0x82111598) calls 0x8210E840 with the local client, the effect, the entity's
// number and the tag's name id. That function finds the tag's bone on the entity's model and plays the effect on it
// (on the entity itself when the model has no such bone). This builtin calls it with the client script's own
// values, each read as the disc's client builtins read theirs: the effect as playfx() does (0x822F91DC), the entity
// as isalive() does (Scr_GetEntityRef, 0x822FB9EC), the tag's name id as gettagorigin() does (0x822FC80C-0x822FC81C).
void Builtin_ClientPlayFxOnTag(PPCContext& ctx, uint8_t* base) {
  TU7_USES("playfxontag (client)");
  Script s{ctx, base, kClient};
  if (s.NumParams() != 4) {
    s.Error("playfxontag takes four parameters: local client, effect, entity, tag");
    return;
  }
  const int32_t local_client = s.Int(0);
  const int32_t fx = s.Int(1);
  if (local_client < 0 || static_cast<uint32_t>(local_client) >= kMaxLocalClients) {
    s.Error("playfxontag: no local client %d", local_client);
    return;
  }
  if (fx <= 0 || fx >= kScriptEffectsEnd) {
    s.Error("playfxontag: invalid effect id %d", fx);
    return;
  }
  ctx.r3.u64 = g_entref;
  ctx.r4.u64 = 2;
  ctx.r5.u64 = kClient;
  sub_8232C950(ctx, base);
  const uint32_t ref = ctx.r3.u32;
  const uint32_t entnum = ref != 0 ? (static_cast<uint32_t>(base[ref]) << 8) | base[ref + 1] : kMaxEntities;
  const uint32_t classnum = ref != 0 ? (static_cast<uint32_t>(base[ref + 2]) << 8) | base[ref + 3] : 1;
  if (classnum != 0) {
    s.Error("playfxontag: the third parameter is not an entity");
    return;
  }
  ctx.r3.u64 = 3;
  ctx.r4.u64 = kClient;
  sub_8232C3D0(ctx, base);  // the tag's string id among the client scripts' strings
  ctx.r4.u64 = kClient;
  sub_8231BAC8(ctx, base);  // its text
  ctx.r4.u64 = 0;
  sub_8231BF90(ctx, base);  // the id the models' bones are named by (0: no model has a bone of that name)
  const uint32_t tag = ctx.r3.u32;
  if (entnum >= kMaxEntities) return;  // a client script's fake entity has no model and no tags: nothing to play on
  const uint32_t cgs = LoadBe32(base, kCgsArray);
  const uint32_t effect =
      cgs != 0 ? LoadBe32(base, cgs + 4 * (static_cast<uint32_t>(local_client) * kCgsWords + kCgsEffects +
                                           static_cast<uint32_t>(fx)))
               : 0;
  if (effect == 0) return;  // not loaded: the disc's playfx() would play nothing either
  ctx.r3.u64 = static_cast<uint32_t>(local_client);
  ctx.r4.u64 = effect;
  ctx.r5.u64 = entnum;
  ctx.r6.u64 = tag;
  sub_8210E840(ctx, base);
}

// ent linkto(parent[, tag]): a client entity made to follow another (Der Riese's teleporter scripts, once). Not
// done yet: the call is checked and does nothing, so the entity stays where it is.
void Builtin_ClientLinkTo(PPCContext& ctx, uint8_t* base) {
  TU7_USES("linkto (client)");
  Script s{ctx, base, kClient};
  const uint32_t count = s.NumParams();
  if (count < 1 || count > 4) {
    s.Error("linkto takes the entity to follow and, if wanted, a tag and offsets");
    return;
  }
}

// --- The table and the four lookups ------------------------------------------------------------------------------

enum Kind : uint32_t { kServerFunction, kServerMethod, kClientFunction, kClientMethod };

struct Builtin {
  const char* name;
  Kind kind;
  PPCFunc* host;
  uint32_t thunk;       // its guest address (0 until made)
  uint32_t guest_name;  // the name, in guest memory, handed back to the compiler
};

Builtin g_builtins[] = {
    {"bbprint", kServerFunction, &Builtin_BbPrint, 0, 0},
    {"hasperk", kServerMethod, &Builtin_HasPerk, 0, 0},
    {"setperk", kServerMethod, &Builtin_SetPerk, 0, 0},
    {"unsetperk", kServerMethod, &Builtin_UnsetPerk, 0, 0},
    {"isswitchingweapons", kServerMethod, &Builtin_IsSwitchingWeapons, 0, 0},
    {"setvisibletoall", kServerMethod, &Builtin_SetVisibleToAll, 0, 0},
    {"setinvisibletoplayer", kServerMethod, &Builtin_SetInvisibleToPlayer, 0, 0},
    {"setelectrified", kServerMethod, &Builtin_SetElectrified, 0, 0},
    {"settransported", kServerMethod, &Builtin_SetTransported, 0, 0},
    {"resetmissiledetonationtime", kServerMethod, &Builtin_ResetMissileDetonationTime, 0, 0},
    {"getaivelocity", kServerMethod, &Builtin_GetAiVelocity, 0, 0},
    {"haseyes", kServerMethod, &Builtin_ServerHasEyes, 0, 0},
    {"isnotarget", kServerMethod, &Builtin_IsNotarget, 0, 0},  // the PC game's, not the update's
    {"spawncollision", kServerFunction, &Builtin_SpawnCollision, 0, 0},  // the PC game's, not the update's
    // Client scripts only: the server scripts' isads() is maps\_utility's own script function, which a server builtin
    // of the same name would hide (the update registers it for client scripts only too).
    {"isads", kClientFunction, &Builtin_IsAds, 0, 0},
    {"playviewmodelfx", kClientFunction, &Builtin_PlayViewmodelFx, 0, 0},
    {"haseyes", kClientMethod, &Builtin_ClientHasEyes, 0, 0},
    {"getsubstr", kClientFunction, &Builtin_ClientGetSubStr, 0, 0},  // the PC game's, not the update's
    // The update's, missing from the first list (Shi No Numa's and Der Riese's client scripts):
    {"getcurrentweapon", kClientFunction, &Builtin_ClientGetCurrentWeapon, 0, 0},
    {"getweaponammoclip", kClientFunction, &Builtin_ClientGetWeaponAmmoClip, 0, 0},
    {"isthrowinggrenade", kClientFunction, &Builtin_ClientIsThrowingGrenade, 0, 0},
    // The update's too, found by reading each call with its form (function or method):
    {"playsound", kClientMethod, &Builtin_ClientPlaySound, 0, 0},  // the disc's playsound is a function only
    {"getvisionsetnaked", kClientFunction, &Builtin_ClientGetVisionSetNaked, 0, 0},
    {"playfxontag", kClientFunction, &Builtin_ClientPlayFxOnTag, 0, 0},
    {"linkto", kClientMethod, &Builtin_ClientLinkTo, 0, 0},              // checked, not done yet
};

std::mutex g_setup_mutex;
std::atomic<bool> g_ready{false};
bool g_failed = false;

// Thunks and guest strings, made at the first lookup. False if the runtime couldn't give them (then nothing is added
// and the compiler reports the unknown builtin as it did before).
bool Setup(uint8_t* base) {
  if (g_ready.load(std::memory_order_acquire)) return true;
  std::lock_guard<std::mutex> lock(g_setup_mutex);
  if (g_ready.load(std::memory_order_relaxed)) return true;
  if (g_failed) return false;
  auto* kernel = rex::system::kernel_state();
  auto* memory = kernel ? kernel->memory() : nullptr;
  auto* dispatcher = kernel ? kernel->function_dispatcher() : nullptr;
  if (memory == nullptr || dispatcher == nullptr) {
    g_failed = true;
    REXLOG_ERROR("waw_tu7: no runtime to make the builtins' addresses; title update 7's builtins are off");
    return false;
  }
  g_error_text = memory->SystemHeapAlloc(kErrorTextSize);
  g_vector = memory->SystemHeapAlloc(16);
  g_command_text = memory->SystemHeapAlloc(kCommandTextSize);
  g_substr_text = memory->SystemHeapAlloc(kSubStrSize);
  g_none_text = memory->SystemHeapAlloc(8);
  g_entref = memory->SystemHeapAlloc(8);
  g_spawn_vectors = memory->SystemHeapAlloc(24);
  if (g_none_text != 0) std::memcpy(base + g_none_text, "none", 5);
  bool names_ok = true;
  for (size_t i = 0; i < g_effect_material_names.size(); ++i) {
    const size_t size = std::strlen(kEffectMaterialNames[i]) + 1;
    g_effect_material_names[i] = memory->SystemHeapAlloc(static_cast<uint32_t>(size));
    if (g_effect_material_names[i] == 0) {
      names_ok = false;
      break;
    }
    std::memcpy(base + g_effect_material_names[i], kEffectMaterialNames[i], size);
  }
  if (g_error_text == 0 || g_vector == 0 || g_command_text == 0 || g_substr_text == 0 || g_none_text == 0 ||
      g_entref == 0 || g_spawn_vectors == 0 || !names_ok) {
    g_failed = true;
    REXLOG_ERROR("waw_tu7: no guest memory for the builtins; title update 7's builtins are off");
    return false;
  }
  for (auto& b : g_builtins) {
    b.thunk = dispatcher->AllocateThunk(b.host, 0);
    const size_t size = std::strlen(b.name) + 1;
    b.guest_name = memory->SystemHeapAlloc(static_cast<uint32_t>(size));
    if (b.thunk == 0 || b.guest_name == 0) {
      g_failed = true;
      REXLOG_ERROR("waw_tu7: couldn't make {}'s address; title update 7's builtins are off", b.name);
      return false;
    }
    std::memcpy(base + b.guest_name, b.name, size);
  }
  REXLOG_INFO("waw_tu7: {} builtins the disc lacks are ready (title update 7's and three of the PC game's; "
              "first at 0x{:08X})",
              sizeof(g_builtins) / sizeof(g_builtins[0]), g_builtins[0].thunk);
  g_ready.store(true, std::memory_order_release);
  return true;
}

// The compiler's lookup (const char** name in r3, int* type in r4; the handler is returned in r3): one of our names
// gets our handler and our copy of the name, type 0 (not a developer builtin), and the disc's lookup is skipped.
bool Lookup(PPCContext& ctx, uint8_t* base, Kind kind) {
  if (!REXCVAR_GET(waw_tu7_builtins)) return false;
  const uint32_t name_slot = ctx.r3.u32;
  const uint32_t type_slot = ctx.r4.u32;
  if (name_slot == 0 || type_slot == 0) return false;
  char name[64];
  if (!ReadLower(base, LoadBe32(base, name_slot), name, sizeof(name))) return false;
  for (auto& b : g_builtins) {
    if (b.kind != kind || std::strcmp(b.name, name) != 0) continue;
    if (!Setup(base)) return false;
    StoreBe32(base, name_slot, b.guest_name);
    StoreBe32(base, type_slot, 0);
    ctx.r3.u64 = b.thunk;
    return true;
  }
  return false;
}

}  // namespace

// The four lookups, each at its entry with return_on_true: server functions 0x821E90A0 (EmitCall's, instance 0),
// server methods 0x821EAC90 (EmitMethod's; it tries the actor, sentient, player, script entity, vehicle and hud element
// tables, then its own), client-script functions CScr_GetFunction 0x822FC1D0, client-script methods CScr_GetMethod
// 0x822FF1C8.
bool waw_tu7_server_function_hook(PPCContext& ctx, uint8_t* base) { return Lookup(ctx, base, kServerFunction); }
bool waw_tu7_server_method_hook(PPCContext& ctx, uint8_t* base) { return Lookup(ctx, base, kServerMethod); }
bool waw_tu7_client_function_hook(PPCContext& ctx, uint8_t* base) { return Lookup(ctx, base, kClientFunction); }
bool waw_tu7_client_method_hook(PPCContext& ctx, uint8_t* base) { return Lookup(ctx, base, kClientMethod); }

// CG_VisionSetStartLerp_To(localClientNum, mode, style, name, duration in ms): 1 when the vision set was loaded and
// its change started. The client scripts' visionsetnaked() passes here (0x822F983C, mode 0); the name is noted for
// getvisionsetnaked(). Known limit: the function at 0x8212DCDC, which copies the server's name into cg_t and loads
// the set itself, doesn't pass here, so after a client script's set a later one of the server's isn't noted.
REX_EXTERN(__imp__sub_8212DBB0);
REX_HOOK_RAW(sub_8212DBB0) {
  const uint32_t local_client = ctx.r3.u32;
  const uint32_t mode = ctx.r4.u32;
  const uint32_t name = ctx.r6.u32;
  char text[kVisionNameSize] = {};
  if (mode == kVisionModeNaked && local_client < kMaxLocalClients && name != 0) {
    for (uint32_t i = 0; i + 1 < kVisionNameSize; ++i) {
      text[i] = static_cast<char>(base[name + i]);
      if (text[i] == '\0') break;
    }
  }
  __imp__sub_8212DBB0(ctx, base);
  if (text[0] != '\0' && (ctx.r3.u32 & 0xFF) != 0) {
    std::lock_guard<std::mutex> lock(g_vision_mutex);
    std::memcpy(g_vision_naked[local_client], text, kVisionNameSize);
  }
}

namespace {
void RestoreFlameMaterial(uint8_t* base);  // step 4, below
}  // namespace

// G_InitGame 0x821C0220, at its entry: a new map, no perks, and the flame effect's own material back in place (an
// electric or teleporter one from the last map's zone must not outlive it).
void waw_tu7_init_game_hook(PPCContext& ctx, uint8_t* base) {
  (void)ctx;
  for (uint32_t i = 0; i < kMaxEntities; ++i) g_perks[i].store(0, std::memory_order_relaxed);
  RestoreFlameMaterial(base);
}

// ClientSpawn 0x8230C240, at its entry (r3 the player's entity): a player spawns with no perks, as with the update's
// perk word, which the spawn's playerState reset cleared.
void waw_tu7_client_spawn_hook(PPCContext& ctx, uint8_t* base) {
  (void)base;
  const uint32_t ent = ctx.r3.u32;
  if (ent < kGEntities || ent >= kGEntities + kMaxEntities * kGEntitySize) return;
  const uint32_t entnum = (ent - kGEntities) / kGEntitySize;
  g_perks[entnum].store(0, std::memory_order_relaxed);
}

// --- Step 3: perk effects in the weapon code ----------------------------------------------------------------------
// PM_Weapon_WeaponTimeAdjust 0x820ED850 counts the weapon's timers down by this frame's milliseconds: r25 is loaded
// from pml->msec at 0x820ED8A8 and then taken off ps->weaponTime (+0x40) and ps->weaponDelay (+0x44). The update
// replaced the function with one that first divides those milliseconds by perk_weapReloadMultiplier while the weapon
// is reloading (weaponstate 7-11) and the player has specialty_fastreload, or by perk_weapRateMultiplier while it is
// firing or rechambering (weaponstate 5-6) and the player has specialty_rof (rounded to the nearest millisecond; a
// multiplier of 0 ends both timers at once). The hook stands at 0x820ED8AC, right after the load, with r31 = ps, and
// does the same to r25. The movement code runs for the server and for the client's prediction alike; both read the
// same perks here, so they agree.
namespace {
constexpr uint32_t kPlayerWeaponTime = 0x40;
constexpr uint32_t kPlayerWeaponDelay = 0x44;
constexpr uint32_t kPerkFastReload = 3;  // specialty_fastreload
constexpr uint32_t kPerkRateOfFire = 4;  // specialty_rof
}  // namespace

void waw_tu7_weapon_time_hook(PPCContext& ctx, uint8_t* base) {
  if (!REXCVAR_GET(waw_tu7_perk_effects)) return;
  const uint32_t ps = ctx.r31.u32;
  if (ps == 0) return;
  const uint32_t client_num = LoadBe32(base, ps + kClientNum);
  if (client_num >= kMaxEntities) return;
  const uint32_t perks = g_perks[client_num].load(std::memory_order_relaxed);
  if (perks == 0) return;
  const uint32_t state = LoadBe32(base, ps + kClientWeaponState);
  double multiplier;
  if (state >= 7 && state <= 11 && ((perks >> kPerkFastReload) & 1)) {
    multiplier = REXCVAR_GET(waw_tu7_perk_reload_multiplier);
  } else if ((state == 5 || state == 6) && ((perks >> kPerkRateOfFire) & 1)) {
    multiplier = REXCVAR_GET(waw_tu7_perk_rate_multiplier);
  } else {
    return;
  }
  const int32_t msec = static_cast<int32_t>(ctx.r25.u32);
  int32_t adjusted;
  if (multiplier == 0.0) {
    const int32_t time = static_cast<int32_t>(LoadBe32(base, ps + kPlayerWeaponTime));
    const int32_t delay = static_cast<int32_t>(LoadBe32(base, ps + kPlayerWeaponDelay));
    adjusted = time < delay ? delay : time;
  } else {
    adjusted = static_cast<int32_t>(std::floor(static_cast<double>(msec) / multiplier + 0.5));
  }
  ctx.r25.u64 = static_cast<uint32_t>(adjusted);
}

// --- Step 4: the electric shock and teleporter screen effects ------------------------------------------------------
// What the update does on the client (read on the PC, 2026-11-11 12:47-13:00): CG_DeployServerCommand switches on a
// command's first character; the update added '(' and ')'. Each reads its milliseconds (Cmd_Argv(1), atoi) and starts
// its effect when they are not 0, ends it when they are ("Player %d electrified for %d (ms)...", "... transporter
// sick ..."). The two effects are copies of the disc's flame effect (CG_Burn 0x821005D0, its end 0x82100660, its
// settings 0x8212B830): the same r_flameFX_* settings, the same fade, the same timer, the same full-screen pass in the
// renderer (the disc's 0x82438DB0; the update has it three times). The only thing that differs is the pass's material:
// "zombie_electric_shock_overlay" and "zombie_transporter_overlay" in place of the flame's
// "flamethrowerfx_color_distort_overlay_bloom". The map packs bring those two materials in their _patch zones (electric
// shock: all three; teleporter: Der Riese's).
//
// So ours runs the disc's flame effect for them with the right material standing in the renderer's flame slot
// (0x84A84C78, filled from its post-effect table at 0x820A5DA0) while it shows, and puts the flame's material back once
// the effect has faded out. One difference: the update keeps the three effects apart, so a player could burn and be
// electrified at once; here one shows at a time (the last command wins, and the disc's own burn command, 'W', takes the
// slot back). In the map packs no one burns, so that never comes up there.
namespace {

constexpr int32_t kCommandBurn = 'W';  // the disc's own: the flame effect
constexpr uint32_t kCmdArgvList = 100;  // in the command arguments (r31): argv per nesting level
constexpr uint32_t kFlameMaterialSlot = 0x84A84C78;
constexpr uint32_t kAssetMaterial = 6;  // Material_RegisterHandle 0x8240F480 asks DB_FindXAssetHeader for type 6
constexpr uint32_t kCgFlameActive = 0xAD204;  // byte: the flame effect shows (cleared when its fade-out ends)

std::atomic<uint32_t> g_flame_original{0};  // the slot's own material while ours stands in it, 0 otherwise
std::atomic<int> g_effect_showing{-1};       // 0 electric shock, 1 teleporter, -1 none
std::atomic<uint32_t> g_effect_cg{0};        // the cg_t of the local client it shows for

void RestoreFlameMaterial(uint8_t* base) {
  const uint32_t original = g_flame_original.exchange(0, std::memory_order_relaxed);
  if (original != 0) StoreBe32(base, kFlameMaterialSlot, original);
  g_effect_showing.store(-1, std::memory_order_relaxed);
  g_effect_cg.store(0, std::memory_order_relaxed);
}

// atoi() on a guest string (what the update's cases call on the argument).
int32_t GuestAtoi(const uint8_t* base, uint32_t text) {
  if (text == 0) return 0;
  uint32_t i = text;
  while (base[i] == ' ' || base[i] == '\t') ++i;
  bool negative = false;
  if (base[i] == '-' || base[i] == '+') negative = base[i++] == '-';
  int64_t value = 0;
  while (base[i] >= '0' && base[i] <= '9' && value < 0x7FFFFFFF) value = value * 10 + (base[i++] - '0');
  if (value > 0x7FFFFFFF) value = 0x7FFFFFFF;
  return static_cast<int32_t>(negative ? -value : value);
}

// The effect's material, 0 when its zone isn't loaded (then the default material would come back, and a full-screen
// pass of that is worse than no effect).
uint32_t EffectMaterial(PPCContext& ctx, uint8_t* base, int effect) {
  const uint32_t name = g_effect_material_names[effect];
  if (name == 0) return 0;
  ctx.r3.u64 = kAssetMaterial;
  ctx.r4.u64 = name;
  ctx.r5.u64 = 1;
  ctx.r6.u64 = ~0ull;
  sub_82158FB8(ctx, base);
  const uint32_t material = ctx.r3.u32;
  if (material == 0) return 0;
  ctx.r3.u64 = kAssetMaterial;
  ctx.r4.u64 = name;
  sub_82155EF0(ctx, base);
  return (ctx.r3.u32 & 0xFF) != 0 ? 0 : material;
}

void StartEffect(PPCContext& ctx, uint8_t* base, uint32_t local_client, int effect, int32_t ms) {
  const uint32_t material = EffectMaterial(ctx, base, effect);
  if (material == 0) {
    static std::atomic<uint32_t> missing{0};
    if (missing.fetch_add(1, std::memory_order_relaxed) < 3) {
      REXLOG_WARN("waw_tu7: {} isn't loaded (its map pack's _patch zone brings it); no screen effect",
                  kEffectMaterialNames[effect]);
    }
    return;
  }
  uint32_t expected = 0;
  g_flame_original.compare_exchange_strong(expected, LoadBe32(base, kFlameMaterialSlot), std::memory_order_relaxed);
  StoreBe32(base, kFlameMaterialSlot, material);
  g_effect_showing.store(effect, std::memory_order_relaxed);
  g_effect_cg.store(LoadBe32(base, kCgArray) + local_client * kCgSize, std::memory_order_relaxed);
  ctx.r3.u64 = local_client;
  ctx.r4.u64 = static_cast<uint32_t>(ms);
  sub_821005D0(ctx, base);
}

void EndEffect(PPCContext& ctx, uint8_t* base, uint32_t local_client, int effect) {
  if (g_effect_showing.load(std::memory_order_relaxed) != effect) return;  // not showing: nothing to end
  ctx.r3.u64 = local_client;
  sub_82100660(ctx, base);  // fades out; the material goes back when it has (the frame hook)
}

}  // namespace

// CG_DeployServerCommand 0x82124AE0 at 0x82124B58, the switch's bound test (bt cr6.gt: past '}' is "unknown command"),
// with r9 the command's first character, r28 argc, r31 the command arguments, r11 their nesting level * 4, r24 the
// local client. Our two commands are handled here; then r9 = 0 sends the switch to its entry 0, the function's own
// return (where '\0', 'B', 'M', 'P' and 'n' go), and cr6 says "in range" again (the calls may have changed it).
void waw_tu7_client_command_hook(PPCContext& ctx, uint8_t* base) {
  const int32_t command = static_cast<int32_t>(ctx.r9.u32);
  if (command == kCommandBurn) {  // the disc's own flame effect takes its slot back
    if (g_flame_original.load(std::memory_order_relaxed) != 0) RestoreFlameMaterial(base);
    return;
  }
  if (command != kCommandElectrified && command != kCommandTransported) return;
  if (!REXCVAR_GET(waw_tu7_client_effects)) return;
  const int effect = command == kCommandElectrified ? 0 : 1;
  const uint32_t local_client = ctx.r24.u32;
  int32_t ms = 0;
  if (static_cast<int32_t>(ctx.r28.u32) > 1) {
    const uint32_t argv = LoadBe32(base, ctx.r31.u32 + kCmdArgvList + ctx.r11.u32);
    if (argv != 0) ms = GuestAtoi(base, LoadBe32(base, argv + 4));
  }
  if (Setup(base)) {
    if (ms != 0) {
      StartEffect(ctx, base, local_client, effect, ms);
    } else {
      EndEffect(ctx, base, local_client, effect);
    }
  }
  ctx.r9.u64 = 0;
  ctx.cr6.lt = 1;
  ctx.cr6.gt = 0;
  ctx.cr6.eq = 0;
}

// CG_DrawActiveFrame 0x8212C8A8 at 0x8212D058, right after it has run the flame effect's fade-out (r31 the local
// client's cg_t): once the effect is gone for the client ours showed for, the flame's own material goes back.
void waw_tu7_client_frame_hook(PPCContext& ctx, uint8_t* base) {
  if (g_flame_original.load(std::memory_order_relaxed) == 0) return;
  const uint32_t cg = ctx.r31.u32;
  if (cg == 0 || cg != g_effect_cg.load(std::memory_order_relaxed)) return;
  if (base[cg + kCgFlameActive] != 0) return;
  RestoreFlameMaterial(base);
}

// For the later steps (the Bowie knife): what the builtins keep.
bool WawTu7HasPerk(uint32_t entnum, uint32_t perk) {
  return entnum < kMaxEntities && perk < 32 && ((g_perks[entnum].load(std::memory_order_relaxed) >> perk) & 1) != 0;
}
