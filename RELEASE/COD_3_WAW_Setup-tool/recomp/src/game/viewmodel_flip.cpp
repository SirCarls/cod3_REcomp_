// cod3 - [claude-9b] 2026-10-27 the weapon in the left hand, both programs, per local player (owner 2026-10-27
// 05:11: "add option to flip gun arm (basically just mirroring). This should be an option for a keybind and/or toggle
// box in f2. This will be related to gun x y and z positioning."). dev/agents/claude-9b-think/FINDINGS-VIEWMODEL-FLIP.md.
//
// The mirror itself is in viewmodel.cpp's gun placement hook: after the game writes the gun's axes, the left axis is
// turned around (the model mirrored through its own forward-up plane), so the gun, the hands drawn with it, the saved
// pose and the muzzle point all go to the left; "Right" (cod3_viewmodel_offset_x) then moves it outward. Not on a
// mounted gun, and only in a frame where the view model has its own scene.
//
// A mirrored model draws inside out unless its triangles' winding is read the other way, so this file turns the
// culling, only while the view model's own scene draws: the scene RenderViewModels builds (campaign 0x8242C428, cod3mp
// 0x824433C8) is marked by pointing its name at another spot of the same string ("ViewModels" in "RenderViewModels").
// The scene back end (campaign 0x8214F770, hooked in render_job_timing.cpp; cod3mp 0x82167688, hooked here) turns the
// device's current cull mode (clockwise <-> counter-clockwise, 2 <-> 6) when it starts drawing that scene and turns it
// back when it is done; in between, the D3D cull mode's set and get (campaign 0x822E6CD8 / 0x822E6CF8, cod3mp
// 0x822B5AA0 / 0x822B5AC0) swap the mode too. The get is turned so that a saved and restored mode doesn't come back
// turned twice. The scene lives in that frame's arena, so the mark is read on the worker exactly as the main thread
// left it.
//
// [claude-9b] 2026-10-27 Why the whole scene and not only the sets: build 063054 marked the scene every frame but
// turned one set a frame, and the gun stayed inside out. A surface's own culling (campaign 0x82141180, cod3mp
// 0x82158F38, called for each surface by 0x82143F70 / 0x8215BD78) returns at once in a scene with its own color
// target (scene+0x328), so the gun's surfaces set no cull mode at all: they draw with the one an earlier scene left
// on the device, which the set hook never saw. cod3_debug_viewmodel_flip counts what the draws in that scene see.
//
// [claude-9b] 2026-10-27 Build 080303 (test mv22): the device's cull mode (dev+0x28C8) read "none" for all ten gun
// surfaces a frame, yet the gun still drew inside out. The GPU's cull mode for those draws doesn't come from the
// device: each material pass carries precompiled register writes (pass+0xDC, built once by campaign 0x82155A90 ->
// 0x82154930, cod3mp 0x8216D988 -> 0x8216C828), copied into the command stream before the pass draws (campaign
// 0x821563A8, cod3mp 0x8216E2A0, called per pass from the surface draw 0x82143F70 / 0x8215BD78). The block at +0x40
// writes PA_SU_SC_MODE_CNTL (register 0x2205, value at +0x44: the material's cull, 2 -> cull front, 6 -> cull back),
// and the copy clears the device's dirty bit for that register, so the device's own value is not sent after it. In
// other scenes the surface's own culling (above) sets the device's mode after the copy and that one wins; in the view
// model's scene it returns at once, so the gun draws with its material's culling and the mirrored gun loses its outside
// faces. While the marked scene draws, the copy now goes out with PA_SU_SC_MODE_CNTL's FACE bit (bit 2, which winding
// is the front) turned, and the pass's block is put back right after: the mirrored triangles are read the other way,
// for the culling and anything else that depends on the facing. The copy is made at once on this thread, so other
// scenes and other players' unmirrored guns never see the turned value.

#include "game/viewmodel_flip.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <string>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>

#include "game/guest.h"

REXCVAR_DEFINE_BOOL(cod3_viewmodel_flip, false, "cod3/Game",
                    "The weapon in the left hand (mirrored), player 1; the key \"Flip weapon hand\" switches it")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(cod3_viewmodel_flip_2, false, "cod3/Game",
                    "The weapon in the left hand (mirrored), split-screen player 2")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(cod3_viewmodel_flip_3, false, "cod3/Game",
                    "The weapon in the left hand (mirrored), split-screen player 3")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(cod3_viewmodel_flip_4, false, "cod3/Game",
                    "The weapon in the left hand (mirrored), split-screen player 4")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(cod3_debug_viewmodel_flip, false, "cod3",
                    "Log once a second how the left-handed weapon's scene was marked and how often the culling was "
                    "turned for it (tests)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace {

namespace g = cod3::guest;

#if defined(COD3_MP)
constexpr uint32_t kActiveLocalClient = 0x82BCBE9C;
constexpr uint32_t kInteraction = 0x82C37070;    // + 4 client: the player's interaction; its +0 bit 0: a mounted gun
constexpr uint32_t kVmSceneName = 0x8207994C;    // "RenderViewModels": the view model scene's name (0x824433C8:
                                                 // lis -32248, addi -26292, stw +0x428; the image's only copy)
constexpr uint32_t kBuildScene = 0x82A8C12C;     // the scene being built (main thread)
constexpr uint32_t kRenderNode = 0x82A8C134;     // the scene being drawn (render job worker)
constexpr uint32_t kVmSceneTable = 0x82C9A2E4;   // + index x 0x70: 0 when the view model has its own scene
constexpr uint32_t kVmSceneIndex = 0x82C9A4A0;
constexpr uint32_t kDevice = 0x82A8C0FC;         // [claude-9b] the Direct3D device pointer (the cull set's r3)
#else
constexpr uint32_t kActiveLocalClient = 0x82A2A1C8;
constexpr uint32_t kInteraction = 0x82A59DA0;
constexpr uint32_t kVmSceneName = 0x8207A154;    // "RenderViewModels" (0x8242C428: lis -32248, addi -24236)
constexpr uint32_t kBuildScene = 0x8290E9A4;
constexpr uint32_t kRenderNode = 0x8290E9AC;
constexpr uint32_t kVmSceneFlag = 0x82AADA34;    // 0 when the view model has its own scene
constexpr uint32_t kDevice = 0x8290F24C;         // [claude-9b] the Direct3D device pointer (the cull set's r3)
#endif
constexpr uint32_t kVmMirroredName = kVmSceneName + 6;  // "ViewModels": the mark
constexpr uint32_t kSceneLastChild = 0x2FC;
constexpr uint32_t kSceneName = 0x428;
constexpr uint32_t kSceneTarget = 0x328;  // [claude-9b] the scene's own color target (0: none)
constexpr uint32_t kCullClockwise = 2;  // with 4 set: counter-clockwise (6)
// [claude-9b] The device's copy of PA_SU_SC_MODE_CNTL (bits 0-2: the cull mode, as the set 0x822E6CD8 writes it) and
// the low word of its 64-bit dirty mask (+0x10, big-endian): 0x40 makes the next draw send that register again.
constexpr uint32_t kDevCullCntl = 0x28C8;
constexpr uint32_t kDevDirtyLow = 0x14;
constexpr uint32_t kDirtyCullCntl = 0x40;
// [claude-9b] 2026-10-27 A material pass's precompiled register writes (see the top of the file): the pass's pointer to
// them, the PM4 type-0 header that writes PA_SU_SC_MODE_CNTL alone (+0x40), its value (+0x44), and the value's bits.
constexpr uint32_t kPassState = 0xDC;
constexpr uint32_t kStateScModeHeader = 0x40;
constexpr uint32_t kStateScModeCntl = 0x44;
constexpr uint32_t kScModeCntlRegister = 0x2205;  // the header: one register, PA_SU_SC_MODE_CNTL
constexpr uint32_t kScModeCull = 3;               // bit 0 cull front, bit 1 cull back
constexpr uint32_t kScModeFace = 4;               // bit 2: which winding is the front

bool g_mirrored[4] = {};  // main thread: the gun was mirrored, its scene not built yet

// [claude-9b] Scenes being drawn with the culling turned, on the render worker (the back end draws a scene's
// children inside it, so their cull mode sets are turned too and the device stays consistent).
thread_local int t_turn_depth = 0;

// [claude-9b] 2026-10-27 cod3_debug_viewmodel_flip: runs c9b-test-flip-on/-mp drew the mirrored gun inside out, so
// each step is counted (main thread: the marks; render worker: the cull mode calls).
struct FlipCounts {
  std::atomic<uint32_t> wanted{0}, marked{0}, no_parent{0}, other_name{0};
  std::atomic<uint32_t> sets{0}, sets_turned{0}, gets{0}, gets_turned{0};
  std::atomic<uint32_t> last_name{0}, last_node_name{0};
};
FlipCounts g_counts;

// [claude-9b] 2026-10-27 What the draws in the mirrored scene see (render worker; counted only with
// cod3_debug_viewmodel_flip, which the main thread copies here so a surface's draw reads one flag). Build 063054 turned
// one set a frame and the gun stayed inside out: these say whether the gun's surfaces draw in that scene, with which
// cull mode, and whether their own culling was skipped (the scene has its own target).
std::atomic<bool> g_debug{false};
struct DrawCounts {
  std::atomic<uint32_t> scenes{0}, scenes_turned{0};    // the mirrored scene drawn; with its cull mode turned
  std::atomic<uint32_t> surfaces{0};                    // surfaces drawn in it
  std::atomic<uint32_t> surface_mode[4] = {};           // the device's cull mode as each starts: none, 2, 6, other
  std::atomic<uint32_t> material{0}, material_skipped{0};  // their own culling; returned at once (own target)
  std::atomic<uint32_t> turned_mode[4] = {};            // the mode each turned set was given: none, 2, 6, other
  // [claude-9b] Material passes copied in the mirrored scene, how many had their facing turned, and their own culling
  // (PA_SU_SC_MODE_CNTL bits 0-1): none, front, back, both.
  std::atomic<uint32_t> passes{0}, passes_turned{0};
  std::atomic<uint32_t> pass_cull[4] = {};
};
DrawCounts g_draws;
// Surfaces drawn by scene name (the scene's +0x428), to find the gun's scene if it isn't the mirrored one.
constexpr int kSceneSlots = 8;
struct SceneSurfaces {
  std::atomic<uint32_t> name[kSceneSlots] = {};
  std::atomic<uint32_t> count[kSceneSlots] = {};
};
SceneSurfaces g_scene_surfaces;

int ModeSlot(uint32_t mode) { return mode == 0 ? 0 : mode == 2 ? 1 : mode == 6 ? 2 : 3; }

bool Valid(uint32_t p) { return p >= 0x10000 && (p & 3) == 0; }

bool AnyFlip() {
  return REXCVAR_GET(cod3_viewmodel_flip) || REXCVAR_GET(cod3_viewmodel_flip_2) ||
         REXCVAR_GET(cod3_viewmodel_flip_3) || REXCVAR_GET(cod3_viewmodel_flip_4);
}

// [claude-9b] 2026-10-27 The name's address is checked once against the guest's bytes: the first build had both
// addresses wrong (copied wrong into FINDINGS-VIEWMODEL-FLIP.md), so nothing was marked and the left-handed gun drew
// inside out (runs c9b-test-flip-on, -flip-mp). A wrong address now leaves the culling alone and says so in the log.
bool NameChecked(uint8_t* base) {
  static int state = 0;  // 0 not checked, 1 right, -1 wrong
  if (state == 0) {
    static constexpr char kName[] = "RenderViewModels";
    const bool right = std::memcmp(base + kVmSceneName, kName, sizeof(kName)) == 0;
    state = right ? 1 : -1;
    if (!right)
      REXLOG_WARN("cod3: viewmodel flip: no \"RenderViewModels\" at {:08x}; the left-handed weapon's culling is left "
                  "as the game's (it draws inside out)",
                  kVmSceneName);
  }
  return state > 0;
}

bool InMirroredScene(uint8_t* base) {
  const uint32_t node = g::ReadU32(base, kRenderNode);
  return Valid(node) && g::ReadU32(base, node + kSceneName) == kVmMirroredName;
}

// [claude-9b] The scenes surfaces were drawn in since the last log line ("none" if no surface), counts reset.
std::string SurfacesByScene(uint8_t* base) {
  std::string out;
  for (int i = 0; i < kSceneSlots; ++i) {
    const uint32_t name = g_scene_surfaces.name[i].load(std::memory_order_relaxed);
    const uint32_t count = g_scene_surfaces.count[i].exchange(0, std::memory_order_relaxed);
    if (name == 0 || count == 0) continue;
    if (!out.empty()) out += ", ";
    out += name >= 0x10000 ? g::ReadString(base, name, 40) : std::string("?");
    out += " " + std::to_string(count);
  }
  return out.empty() ? std::string("none") : out;
}

// After RenderViewModels: its scene is the last child of the scene being built.
void LogCounts(uint8_t* base) {
  static auto since = std::chrono::steady_clock::now();
  const auto now = std::chrono::steady_clock::now();
  if (now - since < std::chrono::seconds(1)) return;
  since = now;
  REXLOG_INFO("cod3: viewmodel flip: scenes to mark {}, marked {}, no scene being built {}, last child not the view "
              "model's {} (its name {:08x}); cull mode set {} ({} turned), get {} ({} turned); last drawn node's name "
              "{:08x} (mark {:08x})",
              g_counts.wanted.exchange(0), g_counts.marked.exchange(0), g_counts.no_parent.exchange(0),
              g_counts.other_name.exchange(0), g_counts.last_name.load(), g_counts.sets.exchange(0),
              g_counts.sets_turned.exchange(0), g_counts.gets.exchange(0), g_counts.gets_turned.exchange(0),
              g_counts.last_node_name.load(), kVmMirroredName);
  // [claude-9b] 2026-10-27 The draws in the mirrored scene (see DrawCounts).
  const uint32_t surfaces = g_draws.surfaces.exchange(0);
  const std::string by_scene = SurfacesByScene(base);
  REXLOG_INFO("cod3: viewmodel flip draws: mirrored scene drawn {} times ({} with the culling turned at its start); "
              "surfaces in it {} (cull mode as they start: none {}, 2 {}, 6 {}, other {}); per-surface culling {} "
              "({} skipped: the scene has its own target); turned sets by mode: none {}, 2 {}, 6 {}, other {}",
              g_draws.scenes.exchange(0), g_draws.scenes_turned.exchange(0), surfaces,
              g_draws.surface_mode[0].exchange(0), g_draws.surface_mode[1].exchange(0),
              g_draws.surface_mode[2].exchange(0), g_draws.surface_mode[3].exchange(0), g_draws.material.exchange(0),
              g_draws.material_skipped.exchange(0), g_draws.turned_mode[0].exchange(0),
              g_draws.turned_mode[1].exchange(0), g_draws.turned_mode[2].exchange(0),
              g_draws.turned_mode[3].exchange(0));
  // [claude-9b] 2026-10-27 The culling the GPU really gets in that scene: the material passes' own.
  REXLOG_INFO("cod3: viewmodel flip passes: material passes copied in the mirrored scene {} ({} with the facing "
              "turned); their own culling: none {}, front {}, back {}, both {}",
              g_draws.passes.exchange(0), g_draws.passes_turned.exchange(0), g_draws.pass_cull[0].exchange(0),
              g_draws.pass_cull[1].exchange(0), g_draws.pass_cull[2].exchange(0), g_draws.pass_cull[3].exchange(0));
  if (surfaces == 0)
    REXLOG_INFO("cod3: viewmodel flip draws: no surface drawn while the mirrored scene was current; surfaces by "
                "scene: {}",
                by_scene);
}

void MarkScene(uint8_t* base) {
  const bool debug = REXCVAR_GET(cod3_debug_viewmodel_flip);
  g_debug.store(debug, std::memory_order_relaxed);
  if (debug) LogCounts(base);
  const uint32_t client = g::ReadU32(base, kActiveLocalClient);
  if (client >= 4 || !g_mirrored[client]) return;
  g_mirrored[client] = false;
  if (!NameChecked(base)) return;
  if (debug) ++g_counts.wanted;
  const uint32_t parent = g::ReadU32(base, kBuildScene);
  if (!Valid(parent)) {
    if (debug) ++g_counts.no_parent;
    return;
  }
  const uint32_t scene = g::ReadU32(base, parent + kSceneLastChild);
  const uint32_t name = Valid(scene) ? g::ReadU32(base, scene + kSceneName) : 0;
  if (name == kVmSceneName) {
    g::WriteU32(base, scene + kSceneName, kVmMirroredName);
    if (debug) ++g_counts.marked;
  } else if (debug) {
    ++g_counts.other_name;
    g_counts.last_name.store(name, std::memory_order_relaxed);
  }
}

// The cull mode calls on the render worker (counted only with cod3_debug_viewmodel_flip).
void CountCull(uint8_t* base, bool set, bool turned) {
  if (!REXCVAR_GET(cod3_debug_viewmodel_flip)) return;
  ++(set ? g_counts.sets : g_counts.gets);
  if (turned) ++(set ? g_counts.sets_turned : g_counts.gets_turned);
  const uint32_t node = g::ReadU32(base, kRenderNode);
  if (Valid(node)) g_counts.last_node_name.store(g::ReadU32(base, node + kSceneName), std::memory_order_relaxed);
}

uint32_t Turned(uint32_t mode) { return (mode & kCullClockwise) ? (mode ^ 4u) : mode; }

// [claude-9b] Turns the device's current cull mode the way the set hook turns a new one, and marks the register to be
// sent again (as the set 0x822E6CD8 does), so the scene's next draw uses it. Turning twice gives the mode back. True
// if the mode changed (none stays none).
bool TurnDeviceCull(uint8_t* base) {
  const uint32_t dev = g::ReadU32(base, kDevice);
  if (!Valid(dev)) return false;
  const uint32_t cntl = g::ReadU32(base, dev + kDevCullCntl);
  const uint32_t turned = (cntl & ~7u) | Turned(cntl & 7u);
  if (turned == cntl) return false;
  g::WriteU32(base, dev + kDevCullCntl, turned);
  g::WriteU32(base, dev + kDevDirtyLow, g::ReadU32(base, dev + kDevDirtyLow) | kDirtyCullCntl);
  return true;
}

// [claude-9b] A model surface's draw (render worker; cod3_debug_viewmodel_flip only): which scene it is drawn in, and
// in the mirrored one the cull mode it starts with.
void CountSurface(uint8_t* base) {
  const uint32_t node = g::ReadU32(base, kRenderNode);
  if (!Valid(node)) return;
  const uint32_t name = g::ReadU32(base, node + kSceneName);
  if (name != 0) {
    for (int i = 0; i < kSceneSlots; ++i) {
      uint32_t slot = g_scene_surfaces.name[i].load(std::memory_order_relaxed);
      if (slot == 0 && g_scene_surfaces.name[i].compare_exchange_strong(slot, name, std::memory_order_relaxed))
        slot = name;
      if (slot == name) {
        g_scene_surfaces.count[i].fetch_add(1, std::memory_order_relaxed);
        break;
      }
    }
  }
  if (name != kVmMirroredName) return;
  ++g_draws.surfaces;
  const uint32_t dev = g::ReadU32(base, kDevice);
  ++g_draws.surface_mode[Valid(dev) ? ModeSlot(g::ReadU32(base, dev + kDevCullCntl) & 7u) : 3];
}

// [claude-9b] A surface's own culling (render worker; cod3_debug_viewmodel_flip only): in the mirrored scene, and
// whether it returns at once because the scene has its own target.
void CountMaterialCull(uint8_t* base) {
  if (!InMirroredScene(base)) return;
  ++g_draws.material;
  const uint32_t node = g::ReadU32(base, kRenderNode);
  if (g::ReadU32(base, node + kSceneTarget) != 0) ++g_draws.material_skipped;
}

void CountTurnedSet(uint32_t mode) {
  if (g_debug.load(std::memory_order_relaxed)) ++g_draws.turned_mode[ModeSlot(mode & 7u)];
}

// [claude-9b] 2026-10-27 Before a material pass's register writes are copied (render worker): in the mirrored scene,
// PA_SU_SC_MODE_CNTL's FACE bit in the pass's block is turned. Returns the value's address to put back after the copy
// (0: nothing turned) and the value in *saved. A block that doesn't hold that register write where it should is left
// alone.
uint32_t TurnPassState(uint8_t* base, uint32_t pass, uint32_t* saved) {
  if (!AnyFlip() || !Valid(pass) || !InMirroredScene(base)) return 0;
  const uint32_t state = g::ReadU32(base, pass + kPassState);
  if (!Valid(state)) return 0;  // none (the surface draw builds it before the copy, so it should be there)
  const bool debug = g_debug.load(std::memory_order_relaxed);
  if (debug) ++g_draws.passes;
  if (g::ReadU32(base, state + kStateScModeHeader) != kScModeCntlRegister) return 0;
  const uint32_t value = g::ReadU32(base, state + kStateScModeCntl);
  *saved = value;
  g::WriteU32(base, state + kStateScModeCntl, value ^ kScModeFace);
  if (debug) {
    ++g_draws.passes_turned;
    ++g_draws.pass_cull[value & kScModeCull];
  }
  return state + kStateScModeCntl;
}

}  // namespace

namespace cod3::viewmodel {

bool FlipFor(uint32_t client) {
  switch (client) {
    case 0: return REXCVAR_GET(cod3_viewmodel_flip);
    case 1: return REXCVAR_GET(cod3_viewmodel_flip_2);
    case 2: return REXCVAR_GET(cod3_viewmodel_flip_3);
    case 3: return REXCVAR_GET(cod3_viewmodel_flip_4);
    default: return false;
  }
}

bool OnMountedGun(uint8_t* base, uint32_t client) {
  if (client >= 4) return false;
  const uint32_t interaction = g::ReadU32(base, kInteraction + 4 * client);
  return Valid(interaction) && (g::ReadU32(base, interaction) & 1u) != 0;
}

bool OwnViewModelScene(uint8_t* base) {
#if defined(COD3_MP)
  const uint32_t index = g::ReadU32(base, kVmSceneIndex);
  if (index >= 16) return false;
  return g::ReadU32(base, kVmSceneTable + index * 0x70) == 0;
#else
  return g::ReadU32(base, kVmSceneFlag) == 0;
#endif
}

void MarkMirrored(uint32_t client) {
  if (client < 4) g_mirrored[client] = true;
}

// [claude-9b] 2026-10-27 Around each scene the back end draws: the marked one is drawn with the device's cull mode
// turned (its surfaces set none of their own), and the set and get hooks turn modes while inside it.
bool BeginScene(uint8_t* base) {
  if (!AnyFlip() || !InMirroredScene(base)) return false;
  ++t_turn_depth;
  const bool changed = TurnDeviceCull(base);
  if (g_debug.load(std::memory_order_relaxed)) {
    ++g_draws.scenes;
    if (changed) ++g_draws.scenes_turned;
  }
  return true;
}

void EndScene(uint8_t* base, bool turned) {
  if (!turned) return;
  TurnDeviceCull(base);
  --t_turn_depth;
}

}  // namespace cod3::viewmodel

#if defined(COD3_MP)
REX_EXTERN(__imp__sub_824433C8);
REX_HOOK_RAW(sub_824433C8) {  // RenderViewModels
  __imp__sub_824433C8(ctx, base);
  MarkScene(base);
}
REX_EXTERN(__imp__sub_822B5AA0);
REX_HOOK_RAW(sub_822B5AA0) {  // the cull mode's set (r3 the device, r4 the mode)
  const bool turn = t_turn_depth > 0;
  if (turn) {
    CountTurnedSet(ctx.r4.u32);
    ctx.r4.u64 = Turned(ctx.r4.u32);
  }
  CountCull(base, true, turn);
  __imp__sub_822B5AA0(ctx, base);
}
REX_EXTERN(__imp__sub_822B5AC0);
REX_HOOK_RAW(sub_822B5AC0) {  // the cull mode's get (r3: the mode)
  __imp__sub_822B5AC0(ctx, base);
  const bool turn = t_turn_depth > 0;
  if (turn) ctx.r3.u64 = Turned(ctx.r3.u32);
  CountCull(base, false, turn);
}
// [claude-9b] The scene back end (draws a scene's children, then its lists). The campaign's 0x8214F770 is hooked in
// render_job_timing.cpp, which calls the same two functions (a function can have only one hook).
REX_EXTERN(__imp__sub_82167688);
REX_HOOK_RAW(sub_82167688) {
  const bool turned = cod3::viewmodel::BeginScene(base);
  __imp__sub_82167688(ctx, base);
  cod3::viewmodel::EndScene(base, turned);
}
// [claude-9b] A model surface's draw (a render list item) and its own culling: counted only (tests).
REX_EXTERN(__imp__sub_8215BD78);
REX_HOOK_RAW(sub_8215BD78) {
  if (g_debug.load(std::memory_order_relaxed)) CountSurface(base);
  __imp__sub_8215BD78(ctx, base);
}
REX_EXTERN(__imp__sub_82158F38);
REX_HOOK_RAW(sub_82158F38) {
  if (g_debug.load(std::memory_order_relaxed)) CountMaterialCull(base);
  __imp__sub_82158F38(ctx, base);
}
// [claude-9b] 2026-10-27 A material pass's register writes copied into the command stream (r4 the pass): in the
// mirrored scene with the facing turned (see the top of the file).
REX_EXTERN(__imp__sub_8216E2A0);
REX_HOOK_RAW(sub_8216E2A0) {
  uint32_t saved = 0;
  const uint32_t turned = TurnPassState(base, ctx.r4.u32, &saved);
  __imp__sub_8216E2A0(ctx, base);
  if (turned) g::WriteU32(base, turned, saved);
}
#else
REX_EXTERN(__imp__sub_8242C428);
REX_HOOK_RAW(sub_8242C428) {  // RenderViewModels
  __imp__sub_8242C428(ctx, base);
  MarkScene(base);
}
REX_EXTERN(__imp__sub_822E6CD8);
REX_HOOK_RAW(sub_822E6CD8) {  // the cull mode's set (r3 the device, r4 the mode)
  const bool turn = t_turn_depth > 0;
  if (turn) {
    CountTurnedSet(ctx.r4.u32);
    ctx.r4.u64 = Turned(ctx.r4.u32);
  }
  CountCull(base, true, turn);
  __imp__sub_822E6CD8(ctx, base);
}
REX_EXTERN(__imp__sub_822E6CF8);
REX_HOOK_RAW(sub_822E6CF8) {  // the cull mode's get (r3: the mode)
  __imp__sub_822E6CF8(ctx, base);
  const bool turn = t_turn_depth > 0;
  if (turn) ctx.r3.u64 = Turned(ctx.r3.u32);
  CountCull(base, false, turn);
}
// [claude-9b] A model surface's draw (a render list item) and its own culling: counted only (tests).
REX_EXTERN(__imp__sub_82143F70);
REX_HOOK_RAW(sub_82143F70) {
  if (g_debug.load(std::memory_order_relaxed)) CountSurface(base);
  __imp__sub_82143F70(ctx, base);
}
REX_EXTERN(__imp__sub_82141180);
REX_HOOK_RAW(sub_82141180) {
  if (g_debug.load(std::memory_order_relaxed)) CountMaterialCull(base);
  __imp__sub_82141180(ctx, base);
}
// [claude-9b] 2026-10-27 A material pass's register writes copied into the command stream (r4 the pass): in the
// mirrored scene with the facing turned (see the top of the file).
REX_EXTERN(__imp__sub_821563A8);
REX_HOOK_RAW(sub_821563A8) {
  uint32_t saved = 0;
  const uint32_t turned = TurnPassState(base, ctx.r4.u32, &saved);
  __imp__sub_821563A8(ctx, base);
  if (turned) g::WriteU32(base, turned, saved);
}
#endif
