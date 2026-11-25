// [acct2-bro] 2026-11-22 Camo lane x531 (the gloves, the game's side, a prototype; not in the build yet): reading
// our own .c3arms files (written by the setup tool from the player's own Black Ops arm models, results x530) into
// memory, for the native renderer to draw in place of the game's first-person arms. The game never reads a Black
// Ops file. A file is untrusted input: every count and size is checked against the file's own length and its
// crc32 before anything is used, and a file that fails is refused once with a reason (the game's own arms are drawn).
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace cod3::ngl_arms {

constexpr uint32_t kMaxBones = 128, kMaxSurfaces = 16, kMaxVertices = 65535, kMaxTextures = 32;
constexpr uint32_t kMaxFileBytes = 32u << 20;  // the largest seen is about 0.45 MB

struct Bone {
  std::string name;
  int16_t parent = -1;     // -1 for a root; always before this bone
  float quat[4] = {};      // the bind pose in the model's space (a unit quaternion) ...
  float pos[3] = {};       // ... and position
};

struct Texture {
  uint8_t semantic = 0;    // Black Ops' texture semantic (2 colour, 5 normal, 8 specular, ...)
  std::string image;       // its image name ("" when the setup tool could not read it)
};

struct Material {
  std::string name;
  std::vector<Texture> textures;
};

struct Vertex {
  float pos[3], normal[3], uv[2];
  uint8_t bones[4];        // indices into the model's bones
  float weights[4];        // they add up to 1
};

struct Surface {
  uint32_t material = 0;
  bool rigid = false;
  std::vector<Vertex> vertices;
  std::vector<uint16_t> indices;  // three per triangle, each below vertices.size()
};

struct Model {
  std::string name;
  std::vector<Bone> bones;
  std::vector<Material> materials;
  std::vector<Surface> surfaces;
};

// The whole file in memory in, a model out; false and why (one short sentence) if anything is wrong.
bool Parse(const uint8_t* data, size_t size, Model* out, std::string* why);

// The crc32 the file's header carries (zlib's polynomial), over the bytes after the header.
uint32_t Crc32(const uint8_t* data, size_t size);

}  // namespace cod3::ngl_arms
