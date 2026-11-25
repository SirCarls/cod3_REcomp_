// [acct2-bro] 2026-11-22 Camo lane x534 (the gloves, the game's side, a prototype; not in the build yet): putting a
// Black Ops arm model (ngl_arms::Model, read from our own .c3arms file) on Call of Duty 3's first-person arm bones.
// Pure math on numbers handed in: nothing here reads game memory or files, so it is tested on its own (rig_test).
//
// Call of Duty 3's arms are a 3ds Max Biped ("Bip01 L UpperArm", "Bip01 R Finger12", ...); Black Ops names its arm
// bones j_<part>_le / _ri (j_shoulder_le, j_index_ri_2, ...). Bones are paired by those names (PairKey); a Black Ops
// bone with no pair rides its nearest paired ancestor. One similarity transform (scale, rotation, offset) takes the
// Black Ops model's space to Call of Duty 3's arms model's space; it is fitted once from the paired bones' rest and
// bind positions (least squares, Umeyama). Each frame, a Black Ops vertex v (in its model's space) goes where Call of
// Duty 3's own skinning would put a vertex at v x G x C of the paired bone: out = sum of w x (v G C) S, S the bone's
// model to bind inverse times its pose this frame (the matrix ngl_side's SurfaceSkin folds), C the segment's
// placement onto the game's (MakeRig). Row vectors, 4x4, as there.
// Refused (false and why) when too few bones pair, the fit is degenerate, or any number is not finite: then the
// game's own arms are drawn.
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "ngl_arms.h"

namespace cod3::ngl_arms {

using Mat4 = std::array<float, 16>;  // row vectors: p' = p M, the translation in [12..14]

// One of Call of Duty 3's arm bones as the game holds it: its name, parent (-1 for a root) and the model's space to
// the bone's (the inverse bind matrix, the skeleton record's +0x40).
struct GameBone {
  std::string name;
  int parent = -1;
  Mat4 inverse_bind{};
};

// "L UpperArm", "R Finger12" for both namings, "" for a bone that pairs with nothing.
std::string PairKey(const std::string& bone_name);

struct Rig {
  std::vector<int> game_bone;   // for each Black Ops bone: the Call of Duty 3 bone it rides
  int pairs = 0;                // how many Black Ops bones paired by name
  Mat4 align{};                 // G: Black Ops model space to Call of Duty 3's arms model space
  std::vector<Mat4> place;      // for each Black Ops bone: G, then its segment laid onto the game's (C); see MakeRig
  float scale = 1.0f, worst = 0.0f;  // G's scale, and the largest paired joint distance after it (model units)
  float fit_scale = 1.0f;       // x545: the global fit's own scale (G's is the paired segments' median length ratio)
  bool by_shape = false;        // x540: paired by the skeleton's shape (the game's bones had too few names)
};

// The rig for `model` on `game` (the game's bones, in its own order). Needs at least 6 pairs (both hands' main
// bones), by name or, when the game's bones have too few names (Call of Duty 3's have none at run time), by the
// skeleton's shape (x540: hands, arms and fingers found from the tree); false and why otherwise. `reanchor`: each
// limb segment is also turned about its joint onto the game's segment, stretched along it to its length (at most 2
// times) and its joint put on the game's (for rest poses and proportions that differ); off: G alone for every bone.
bool MakeRig(const Model& model, const std::vector<GameBone>& game, Rig* out, std::string* why, bool reanchor = true);

// One frame: `skin[i]` = game bone i's inverse bind times its pose (as many as `game` had). Writes, for each vertex
// of each surface in order, its position and normal into `out` (`stride` floats a vertex: position at 0..2, normal
// at `normal_at`..+2; a buffer of the right size keeps its other floats). False if a matrix is missing or not
// finite (then use none of it).
bool SkinModel(const Model& model, const Rig& rig, const std::vector<Mat4>& skin, size_t stride, size_t normal_at,
               std::vector<float>* out);

// The model's corners at rest, `stride` floats each: position, normal, uv at `uv_at`, and the 4 color bytes (ngl_vk's
// D3DCOLOR) in the float at `color_at`; everything else 0. For ngl_vk_mesh (made once), and as the buffer SkinModel
// then moves each frame (it writes the positions and normals only, so the uv and color stay).
void RestCorners(const Model& model, size_t stride, size_t uv_at, size_t color_at, uint32_t color, size_t normal_at,
                 std::vector<float>* out);

// Helpers, public for the test: a * b; the bone's origin in the model's space from its inverse bind.
Mat4 Mul(const Mat4& a, const Mat4& b);
std::array<float, 3> OriginOf(const Mat4& inverse_bind);

}  // namespace cod3::ngl_arms
