// [acct2-bro] 2026-11-23 Camo lane (the gloves): Black Ops first-person arms, and the glove skins of the player's own
// camo pack, drawn by the native renderer in place of the game's own arms. Off by default (cod3_arms empty).
//
// The files are ours, made by the setup tool from the player's own files: data/cod3/arms/<id>.c3arms (an arm model,
// ngl_arms.h) and data/cod3/gloves/<id>/<image>.c3camo (a skin's colour map per Black Ops colour image, the camo
// reader's format). They are read and checked on a thread of their own, never in a frame; a texture is made on the
// drawing thread at most one a frame; the arms show only when the model and every texture it wants are ready (a
// surface whose image the skin lacks, or can't be read, is drawn in one flat tint), so a change never stalls a frame
// and the arms never appear half made. Nothing is sent to other players and the game itself is unchanged: this file
// never reads the game's memory; ngl_side hands it what it read (GameArms).
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#if defined(COD3_NGL_VK) || defined(NGL_GLOVES_TEST)  // the drawing needs ngl_vk (F2's lists don't)
extern "C" {
#include "ngl_vk.h"
}
#define NGL_GLOVES_DRAWS 1
#endif
#include "game/ngl_arms.h"
#include "game/ngl_arms_rig.h"

namespace cod3::ngl_gloves {

// One front-scene skinned object, as ngl_side read it: the model, its skeleton and its colour map's own name (the
// arms are known by "arms" in it: w_usarms_new_c, w_canarms_new_c), and ngl_side's two readers, called only when
// wanted (the bones once per skeleton, the skin matrices once per object a frame).
struct GameArms {
  uint32_t object = 0, model = 0, skeleton = 0;
  std::string color_map;
  void* ctx = nullptr;
  bool (*read_bones)(void* ctx, std::vector<ngl_arms::GameBone>* out) = nullptr;
  bool (*read_skin)(void* ctx, size_t count, std::vector<ngl_arms::Mat4>* out) = nullptr;
};

// Cheap: is cod3_arms set (with the native renderer). The caller skips everything else when not.
bool On();

// The drawing thread, once a frame before the first front-scene draw: takes the settings, hands new files to the
// reading thread and makes at most one waiting texture.
void BeginFrame(uint64_t frame);

// The drawing thread: true when our arms stand for this object (the caller leaves the game's surface `d` out). Ours
// go into `draws` the first time in a frame, with `d`'s matrices, fog, light and split-screen part, if `room` allows.
#if defined(NGL_GLOVES_DRAWS)
bool DrawInstead(const GameArms& game, const ngl_vk_mesh_draw& d, size_t room, std::vector<ngl_vk_mesh_draw>* draws);
#endif

// ngl_vk's meshes and textures went (a level change): made again when wanted.
void Forget();

// F2 (its own thread): the arm models and glove skins found (id, name shown), read again by Rescan.
struct Choice {
  std::string id, name;
};
std::vector<Choice> ArmsList();
std::vector<Choice> GloveList();
void Rescan();

// F2: what happened, for its status line.
struct Counts {
  bool model_ready = false;
  int textures = 0, flat = 0, rigs = 0, refused = 0;
  std::string last_refusal;
};
Counts GetCounts();

}  // namespace cod3::ngl_gloves
