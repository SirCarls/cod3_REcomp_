// waw - World at War's native renderer: Xbox 360 textures decoded to BGRA8, and the GPU's memory.
// [waw-decomp-waw] 2026-11-11
//
// For waw_native_side.cpp. The GPU's data (vertex and index buffers, textures) is named by physical addresses, as the
// Direct3D objects' fetch constants hold them.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace waw::native {

constexpr size_t kPhysicalBytes = 0x20000000u;  // the GPU's memory: every read stops at its end

// How many bytes can be read from a physical address on.
size_t PhysicalRoom(size_t physical);

// The GPU's data at a physical address.
const uint8_t* GpuPointer(const uint8_t* base, uint32_t physical);

// A big-endian word of the GPU's memory at a physical address (0 past its end), the same as a float, and a 16-bit
// index (big-endian).
uint32_t GpuU32(const uint8_t* base, uint32_t physical);
float GpuF32(const uint8_t* base, uint32_t physical);
uint32_t GpuU16(const uint8_t* base, uint32_t physical);

// A texture's top level from its six-word fetch constant, as rows of blue, green, red, alpha bytes. False (and why)
// when it can't be read.
bool DecodeFetch(const uint8_t* base, const std::array<uint32_t, 6>& fetch, std::vector<uint8_t>& bgra,
                 uint32_t& width, uint32_t& height, std::string& why);

// The texture's own smaller levels (1 to its last), the same way; empty when they can't be read (then the top level
// alone is used).
void DecodeLevelsFetch(const uint8_t* base, const std::array<uint32_t, 6>& fetch,
                       std::vector<std::vector<uint8_t>>& levels);

}  // namespace waw::native
