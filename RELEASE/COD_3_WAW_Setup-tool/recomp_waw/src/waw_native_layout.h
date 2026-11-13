// waw - World at War's native renderer: where the 360 program keeps what a draw submits. [waw-decomp-waw] 2026-11-11
//
// The campaign program's (default.xex) layouts, each read in the decompile (dev/waw-decomp/x360_sp) and checked by a
// capture run (waw-decomp-waw-cap1, pel1a; dev/agents/waw-decomp/WAW-NATIVE.md has the map with its evidence). The
// names are IW3's (KisakCOD's and OpenAssetTools' field names, information only).

#pragma once

#include <cstdint>
#include <cstring>
#include <string>

namespace waw::native {

// The command buffer state (GfxCmdBufState), on the drawing thread's stack.
constexpr uint32_t kStateSamplerTextures = 0x88;  // + 4 x sampler slot: the texture bound (a pointer to an image's +4)
constexpr uint32_t kStatePrim = 0xF0;             // the prim state (below)
constexpr uint32_t kStateMaterial = 0x128;
constexpr uint32_t kStateTechniqueType = 0x12C;
constexpr uint32_t kStateTechnique = 0x130;
constexpr uint32_t kStatePass = 0x134;            // technique + 8 + 0x58 x pass index
constexpr uint32_t kStateTessIndices = 0x114C;    // the tess buffers' 16-bit indices
constexpr uint32_t kStateTessVertices = 0x1150;   // and their 32-byte vertices
constexpr uint32_t kStateBits = 0x1154;           // the pass's render state bits, two words (IW3's GFXS0, GFXS1)

// The prim state (state + 0xF0).
constexpr uint32_t kPrimDevice = 0x00;
constexpr uint32_t kPrimIndexBuffer = 0x04;
constexpr uint32_t kPrimDeclType = 0x08;  // the vertex declaration's type: 1 packed (models), 2 and up the world's
constexpr uint32_t kPrimStreams = 0x10;   // three streams of 12 bytes: +0 stride, +4 vertex buffer, +8 offset
constexpr uint32_t kPrimStreamBytes = 12;

// A technique: +0 its name, +4 flags (16 bits), +6 the pass count (16 bits), +8 the passes, 0x58 bytes each.
constexpr uint32_t kTechniquePasses = 0x08;
constexpr uint32_t kPassBytes = 0x58;
// A pass.
constexpr uint32_t kPassDeclaration = 0x00;    // the vertex declarations: +4 + 4 x type a Direct3D declaration
constexpr uint32_t kPassVertexShaders = 0x04;  // + 4 x the declaration's type: the vertex shader for that layout
constexpr uint32_t kPassPixelShader = 0x48;
constexpr uint32_t kPassPerPrimArgs = 0x4C;    // three counts (bytes): per prim, per object, stable
constexpr uint32_t kPassCustomSamplers = 0x4F;  // bit 0 the reflection probe (slot 1), bit 1 the primary lightmap
                                                // (slot 2), bit 2 the secondary lightmap (slot 3): 0x824420E0
constexpr uint32_t kPassArgs = 0x54;           // 8 bytes each: type, destination (16 bits each), then by type
// An argument's type (R_SetPassPixelShaderStableArguments 0x82437220 and the capture's log).
constexpr uint32_t kArgMaterialVertexConst = 0, kArgLiteralVertexConst = 1, kArgMaterialSampler = 2;
constexpr uint32_t kArgCodeVertexConst = 3, kArgCodeSampler = 4, kArgCodePixelConst = 5;
constexpr uint32_t kArgMaterialPixelConst = 6, kArgLiteralPixelConst = 7;
// The code constants seen so far (argument +4: the index, 16 bits; +6 the first row, +7 the row count).
constexpr uint32_t kCodeWorldMatrix = 0x6B;     // the object's placement, the eye taken away (rows of 4)
constexpr uint32_t kCodeViewProjection = 0x7B;  // the eye-relative view-projection
constexpr uint32_t kCodeSunPosition = 0x24;     // toward the sun (a unit vector; a pixel shader's)
constexpr uint32_t kCodeSunDiffuse = 0x25;      // the sun's color
constexpr uint32_t kCodeFogConsts = 0x2A;
constexpr uint32_t kCodeFogColor = 0x2B;
// [waw-decomp-waw] 2026-11-11 round 3: a model's place in the model lighting texture (vertex c8 in the capture; the
// PC's lit model vertex shaders call it baseLightingCoords), and that texture's number among the code textures
// ("$model_lighting", code texture 3, in sampler slot 4 in the capture).
constexpr uint32_t kCodeBaseLightingCoords = 0x3B;
constexpr uint32_t kCodeTextureModelLighting = 3;

// [waw-decomp-waw] 2026-11-11 round 3: the frame's views, as the front end fills them (0x82403400, which calls the
// film's 0x82402978 with the view as its first argument): the view is *(frontEndData + 0xDB838) + its index x
// 0x6DD0, the index kept at frontEndData + 0xDB830 (the count at + 0xDB834); frontEndData is the pointer at
// 0x84C5CA00. In a view, the film's three constants as 0x82402978 makes them for the post effect (postfx_color:
// colorBias, colorTintBase, colorTintDelta), and the film's own on switch (a byte).
constexpr uint32_t kFrontEndDataPointer = 0x84C5CA00u;
constexpr uint32_t kFrontEndViews = 0xDB838, kFrontEndViewIndex = 0xDB830, kFrontEndViewCount = 0xDB834;
constexpr uint32_t kViewBytes = 0x6DD0;
constexpr uint32_t kViewFilmEnable = 0x5769;
constexpr uint32_t kViewFilmColorBias = 0x6960, kViewFilmTintBase = 0x6970, kViewFilmTintDelta = 0x6980;

// A material.
constexpr uint32_t kMaterialTextureCount = 0x4B;  // a byte
constexpr uint32_t kMaterialConstantCount = 0x4C;
constexpr uint32_t kMaterialTechniqueSet = 0x50;
constexpr uint32_t kMaterialTextures = 0x54;  // 16 bytes each: +0 the name's hash, +7 the semantic, +0xC the image
constexpr uint32_t kMaterialTextureBytes = 16;
constexpr uint32_t kMaterialConstants = 0x58;  // 32 bytes each: +0 the hash, +4 the name (12), +0x10 four floats
constexpr uint32_t kSemanticColorMap = 2;
constexpr uint32_t kHashColorMap = 0xA0AB1041u;
// An image record: +4 its Direct3D texture.
constexpr uint32_t kImageTexture = 0x04;

// Direct3D's objects.
constexpr uint32_t kTextureFetch = 0x1C;       // a texture's six-word fetch constant
constexpr uint32_t kVertexBufferFetch = 0x18;  // a vertex buffer's: the address with type 3, then the size
// an index buffer's address: the indices' physical address itself. [waw-decomp-waw] 2026-11-11 round 3: its low bits
// are not a byte order (as this comment said until then): 16-bit indices need only 2-byte alignment, so the address
// is the word & 0x1FFFFFFE for them (& 0x1FFFFFFC for 32-bit ones; FirstIndexAddress in waw_native_side.cpp). Run
// waw-decomp-waw-r2c1 found the world's own index buffer at address word AD19F362.
constexpr uint32_t kIndexBufferAddress = 0x18;
constexpr uint32_t kIndexBufferBytes = 0x1C;
// The device keeps what the game set for the GPU (0x82352F00 sends the changed ones from here): the 32 fetch
// constants (six words a sampler slot; 0x823615A8 sends them as GPU registers 0x4800 on) and the shader constants.
constexpr uint32_t kDeviceFetchConstants = 0x480;
constexpr uint32_t kDeviceVertexConstants = 0x780;   // 16 bytes a register, c0 first
constexpr uint32_t kDevicePixelConstants = 0x1780;

inline uint32_t Load32(const uint8_t* base, uint32_t address) {
  uint32_t value;
  std::memcpy(&value, base + address, sizeof(value));
  return __builtin_bswap32(value);
}

inline float LoadF32(const uint8_t* base, uint32_t address) {
  const uint32_t bits = Load32(base, address);
  float value;
  std::memcpy(&value, &bits, sizeof(value));
  return value;
}

// Whether a guest address can be read: the heaps, the program's own image and data, or physical memory.
inline bool Readable(uint32_t address) {
  return (address >= 0x40000000u && address < 0x7FFF0000u) || (address >= 0x82000000u && address < 0x90000000u) ||
         (address >= 0xA0000000u && address < 0xFFFF0000u);
}

// The printable text at an address (3 to 127 characters), or "".
inline std::string GuestText(const uint8_t* base, uint32_t address) {
  if (!Readable(address) || !Readable(address + 128)) return "";
  const char* p = reinterpret_cast<const char*>(base + address);
  const size_t n = strnlen(p, 128);
  if (n < 3 || n == 128) return "";
  for (size_t i = 0; i < n; ++i) {
    if (p[i] < 0x20 || p[i] > 0x7E) return "";
  }
  return std::string(p, n);
}

// The name a record starts with (a material, a technique, a shader), or "?".
inline std::string NameOf(const uint8_t* base, uint32_t record) {
  if (!Readable(record)) return "?";
  const std::string name = GuestText(base, Load32(base, record));
  return name.empty() ? "?" : name;
}

}  // namespace waw::native
