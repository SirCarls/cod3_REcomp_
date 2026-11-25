// [acct2-bro] 2026-11-22 Camo lane x531: see ngl_arms.h. The layout is the one arms530.py writes: a 64-byte header
// ("C3ARMS1\0", then header size 64, bone, surface and material counts, vertex and index totals, the payload's size
// and crc32, the model's name), then the bones (64 bytes each: name[32], parent i16, 2 bytes, quat f32[4], pos
// f32[3]), the materials (name[60], texture count u32, then 64 bytes a texture: semantic u8, 3 bytes, image[60]),
// the surfaces (material, vertex count, triangle count, flags; 52 bytes a vertex: pos, normal, uv, 4 bone bytes, 4
// weights; then u16 indices). Little-endian. Nothing is trusted before it is checked.
#include "ngl_arms.h"

#include <cmath>
#include <cstring>

namespace cod3::ngl_arms {
namespace {

class Reader {
 public:
  Reader(const uint8_t* data, size_t size) : data_(data), size_(size) {}
  bool Take(size_t n, const uint8_t** out) {
    if (n > size_ - at_) return false;
    *out = data_ + at_;
    at_ += n;
    return true;
  }
  size_t left() const { return size_ - at_; }

 private:
  const uint8_t* data_;
  size_t size_, at_ = 0;
};

uint32_t U32(const uint8_t* p) { return p[0] | p[1] << 8 | p[2] << 16 | static_cast<uint32_t>(p[3]) << 24; }
uint16_t U16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | p[1] << 8); }
float F32(const uint8_t* p) {
  const uint32_t v = U32(p);
  float f;
  std::memcpy(&f, &v, 4);
  return f;
}

// A fixed-size name: printable characters up to the first 0, which must be inside the field.
bool Name(const uint8_t* p, size_t size, std::string* out) {
  const void* end = std::memchr(p, 0, size);
  if (!end) return false;
  const size_t n = static_cast<const uint8_t*>(end) - p;
  for (size_t i = 0; i < n; ++i)
    if (p[i] < 0x20 || p[i] > 0x7E) return false;
  out->assign(reinterpret_cast<const char*>(p), n);
  return true;
}

bool Finite(const float* v, int n, float limit) {
  for (int i = 0; i < n; ++i)
    if (!std::isfinite(v[i]) || std::fabs(v[i]) > limit) return false;
  return true;
}

bool Fail(std::string* why, const char* text) {
  if (why) *why = text;
  return false;
}

}  // namespace

uint32_t Crc32(const uint8_t* data, size_t size) {
  static const std::array<uint32_t, 256> table = [] {  // made once, thread-safe (a function-local static)
    std::array<uint32_t, 256> t{};
    for (uint32_t i = 0; i < 256; ++i) {
      uint32_t c = i;
      for (int k = 0; k < 8; ++k) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
      t[i] = c;
    }
    return t;
  }();
  uint32_t c = 0xFFFFFFFFu;
  for (size_t i = 0; i < size; ++i) c = table[(c ^ data[i]) & 0xFF] ^ (c >> 8);
  return c ^ 0xFFFFFFFFu;
}

bool Parse(const uint8_t* data, size_t size, Model* out, std::string* why) {
  *out = Model{};
  if (!data || size < 64 || size > kMaxFileBytes) return Fail(why, "not a c3arms file (size)");
  if (std::memcmp(data, "C3ARMS1\0", 8) != 0) return Fail(why, "not a c3arms file");
  const uint32_t header = U32(data + 8), nbones = U32(data + 12), nsurfs = U32(data + 16), nmats = U32(data + 20);
  const uint32_t nverts = U32(data + 24), nidx = U32(data + 28), payload = U32(data + 32), crc = U32(data + 36);
  if (header != 64 || payload != size - 64) return Fail(why, "its size is not the one its header gives");
  if (Crc32(data + 64, payload) != crc) return Fail(why, "its checksum is wrong (a damaged file)");
  if (nbones == 0 || nbones > kMaxBones || nsurfs == 0 || nsurfs > kMaxSurfaces || nmats > kMaxSurfaces)
    return Fail(why, "its counts are out of range");
  if (!Name(data + 40, 24, &out->name)) return Fail(why, "its model name is not a name");
  Reader r(data + 64, payload);
  const uint8_t* p;
  out->bones.resize(nbones);
  for (uint32_t i = 0; i < nbones; ++i) {
    Bone& b = out->bones[i];
    if (!r.Take(64, &p) || !Name(p, 32, &b.name)) return Fail(why, "a bone is cut short or unnamed");
    b.parent = static_cast<int16_t>(U16(p + 32));
    if (b.parent < -1 || b.parent >= static_cast<int32_t>(i)) return Fail(why, "a bone's parent is not before it");
    for (int k = 0; k < 4; ++k) b.quat[k] = F32(p + 36 + 4 * k);
    for (int k = 0; k < 3; ++k) b.pos[k] = F32(p + 52 + 4 * k);
    const float n2 = b.quat[0] * b.quat[0] + b.quat[1] * b.quat[1] + b.quat[2] * b.quat[2] + b.quat[3] * b.quat[3];
    if (!Finite(b.quat, 4, 2.0f) || !Finite(b.pos, 3, 1e4f) || std::fabs(n2 - 1.0f) > 0.02f)
      return Fail(why, "a bone's pose is not a rotation and a position");
  }
  out->materials.resize(nmats);
  for (Material& m : out->materials) {
    if (!r.Take(64, &p) || !Name(p, 60, &m.name)) return Fail(why, "a material is cut short or unnamed");
    const uint32_t ntex = U32(p + 60);
    if (ntex > kMaxTextures) return Fail(why, "a material has too many textures");
    m.textures.resize(ntex);
    for (Texture& t : m.textures) {
      if (!r.Take(64, &p) || !Name(p + 4, 60, &t.image)) return Fail(why, "a texture is cut short or unnamed");
      t.semantic = p[0];
    }
  }
  uint64_t seen_verts = 0, seen_idx = 0;
  out->surfaces.resize(nsurfs);
  for (Surface& s : out->surfaces) {
    if (!r.Take(16, &p)) return Fail(why, "a surface is cut short");
    s.material = U32(p);
    const uint32_t vc = U32(p + 4), tc = U32(p + 8), flags = U32(p + 12);
    if (nmats == 0 || s.material >= nmats || vc == 0 || vc > kMaxVertices || tc == 0 || tc > 3u * kMaxVertices ||
        flags > 1)
      return Fail(why, "a surface's counts are out of range");
    s.rigid = flags & 1;
    if (r.left() / 52 < vc) return Fail(why, "a surface's vertices are cut short");
    s.vertices.resize(vc);
    for (Vertex& v : s.vertices) {
      r.Take(52, &p);
      for (int k = 0; k < 3; ++k) v.pos[k] = F32(p + 4 * k), v.normal[k] = F32(p + 12 + 4 * k);
      v.uv[0] = F32(p + 24), v.uv[1] = F32(p + 28);
      float total = 0.0f;
      for (int k = 0; k < 4; ++k) {
        v.bones[k] = p[32 + k];
        v.weights[k] = F32(p + 36 + 4 * k);
        if (!std::isfinite(v.weights[k]) || v.weights[k] < -0.0002f || v.weights[k] > 1.0002f ||
            (v.weights[k] > 0.0f && v.bones[k] >= nbones))
          return Fail(why, "a vertex's bones or weights are out of range");
        total += v.weights[k];
      }
      if (std::fabs(total - 1.0f) > 0.002f || !Finite(v.pos, 3, 1e4f) || !Finite(v.normal, 3, 2.0f) ||
          !Finite(v.uv, 2, 1e3f))
        return Fail(why, "a vertex is not a vertex");
    }
    if (r.left() / 6 < tc) return Fail(why, "a surface's triangles are cut short");
    s.indices.resize(3 * static_cast<size_t>(tc));
    for (uint16_t& i : s.indices) {
      r.Take(2, &p);
      i = U16(p);
      if (i >= vc) return Fail(why, "a triangle names a vertex past its surface's");
    }
    seen_verts += vc;
    seen_idx += 3ull * tc;
  }
  if (r.left() != 0) return Fail(why, "bytes left over after the last surface");
  if (seen_verts != nverts || seen_idx != nidx) return Fail(why, "its totals are not its surfaces' sums");
  if (why) why->clear();
  return true;
}

}  // namespace cod3::ngl_arms
