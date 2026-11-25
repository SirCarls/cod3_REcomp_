// [acct2-bro] 2026-11-23 Camo lane (the gloves): what and why: ngl_gloves.h. The probes x544-x549 (private, never
// landed) drew these arms from a debug setting, reading the files on the drawing thread; this is the same drawing
// with the files read on a thread of their own, the textures made one a frame, and the choice in F2.
#include "game/ngl_gloves.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <unordered_map>
#include <utility>

#if defined(NGL_GLOVES_TEST)
#include "gloves_stubs.h"
#else
#include <rex/cvar.h>
#include <rex/logging.h>
#endif
#include "game/ngl_camo.h"

#if !defined(NGL_GLOVES_TEST)
REXCVAR_DEFINE_STRING(cod3_arms, "", "cod3/Display",
                      "With cod3_native_vulkan, Black Ops first-person arms in place of the game's (an arm model the "
                      "setup tool made from your own Black Ops files: data/cod3/arms/<id>.c3arms; F2 > Camos). "
                      "Empty: the game's own arms (the default)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_STRING(cod3_gloves, "", "cod3/Display",
                      "With cod3_arms: a glove skin from your own camo pack on those arms (data/cod3/gloves/<id>/). "
                      "Empty: the arms in one flat colour")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
#endif

namespace cod3::ngl_gloves {
namespace {

constexpr uint32_t kTint = 0xB0B0B0u;          // the flat colour of a surface without a skin texture
constexpr uint32_t kCornerWhite = 0xFFFFFFFFu;  // the corners' colour (the tint or the texture gives the colour)
constexpr size_t kMostSkeletons = 4;            // split screen: up to 4 players' arms
constexpr size_t kMostTextures = 32;            // the arms' textures made in one level (choices changed included)
constexpr size_t kMostMeshes = 64;              // and their meshes (a model has at most 16 surfaces)

#if defined(NGL_GLOVES_TEST)
std::string ArmsSetting() { return TestArms(); }
std::string GlovesSetting() { return TestGloves(); }
std::filesystem::path DataFolder() { return TestFolder(); }
void Log(const std::string& s) { TestLog(s); }
#else
std::string ArmsSetting() { return REXCVAR_GET(cod3_arms); }
std::string GlovesSetting() { return REXCVAR_GET(cod3_gloves); }
std::filesystem::path DataFolder() { return cod3::ngl_camo::Folder().parent_path(); }  // data/cod3
constexpr int kMostLines = 64;  // log lines this session
std::atomic<int> g_lines{0};
void Log(const std::string& s) {
  if (g_lines.fetch_add(1, std::memory_order_relaxed) < kMostLines) REXLOG_INFO("cod3: gloves: {}", s);
}
#endif

// An id names a file: lower case letters, digits and "_" only, 1 to 64 of them (never a path).
bool IsId(const std::string& id) {
  return !id.empty() && id.size() <= 64 &&
         std::all_of(id.begin(), id.end(), [](char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                                                                 c == '_'; });
}

// An image's name as the setup tool's glove_pack writes it: lower case, without Black Ops' "~" and "-g" prefixes or
// a packer's "&~hash" / "~hash" ending (8 hex digits).
std::string ImageKey(const std::string& image) {
  std::string s;
  for (char c : image) s += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  const size_t dot = s.find_last_of('.');
  if (dot != std::string::npos && dot + 5 >= s.size()) s.resize(dot);
  while (!s.empty() && s[0] == '~') s.erase(0, 1);
  if (s.rfind("-g", 0) == 0) s.erase(0, 2);
  auto hex_tail = [&](size_t n) {
    if (s.size() < n) return false;
    for (size_t i = s.size() - 8; i < s.size(); ++i)
      if (!std::isxdigit(static_cast<unsigned char>(s[i]))) return false;
    return true;
  };
  if (s.size() > 10 && s[s.size() - 10] == '&' && s[s.size() - 9] == '~' && hex_tail(10)) s.resize(s.size() - 10);
  else if (s.size() > 9 && s[s.size() - 9] == '&' && hex_tail(9)) s.resize(s.size() - 9);
  else if (s.size() > 9 && s[s.size() - 9] == '~' && hex_tail(9)) s.resize(s.size() - 9);
  return s;
}

// Each surface's colour image key ("" when its material names none): its material's first semantic-2 image.
std::vector<std::string> SurfaceKeys(const ngl_arms::Model& model) {
  std::vector<std::string> keys(model.surfaces.size());
  for (size_t k = 0; k < model.surfaces.size(); ++k) {
    const uint32_t m = model.surfaces[k].material;
    if (m >= model.materials.size()) continue;
    for (const ngl_arms::Texture& t : model.materials[m].textures)
      if (t.semantic == 2 && !t.image.empty() && t.image != "?") {
        keys[k] = ImageKey(t.image);
        break;
      }
  }
  return keys;
}

bool ReadFile(const std::filesystem::path& path, std::vector<uint8_t>* out, std::string* why) {
  std::error_code ec, link_ec;
  const auto size = std::filesystem::file_size(path, ec);
  if (ec) return *why = "it can't be read (" + ec.message() + ")", false;
  if (size == 0 || size > ngl_arms::kMaxFileBytes || std::filesystem::is_symlink(path, link_ec))
    return *why = "too big, empty or not a plain file", false;
  out->resize(static_cast<size_t>(size));
  std::ifstream f(path, std::ios::binary);
  if (!f.read(reinterpret_cast<char*>(out->data()), std::streamsize(out->size())))
    return *why = "it can't be read (short)", false;
  return true;
}

std::mutex g_counts_mu;           // F2's status line (the drawing thread writes, F2's reads)
Counts g_counts;

#if defined(NGL_GLOVES_DRAWS)
// ---- the reading thread -------------------------------------------------------------------------------------------
struct Job {
  uint32_t generation = 0;
  std::string arms, gloves;
};
struct Map {                       // one colour map read and checked (ngl_camo::ReadColourLevels), or why not
  std::vector<std::vector<uint8_t>> levels;
  uint32_t width = 0, height = 0;
  std::string why;
};
struct Prepared {
  uint32_t generation = 0;
  bool ok = false;
  std::string why;                 // when not ok: why the model can't be used
  std::shared_ptr<ngl_arms::Model> model;
  std::unordered_map<std::string, Map> maps;  // image key -> its map (only the keys the model's surfaces want)
};

std::mutex g_mu;
std::condition_variable g_wake;
std::optional<Job> g_job;          // the latest wanted (an older one not started is replaced)
std::optional<Prepared> g_ready;   // the reader's latest result
std::unique_ptr<std::thread> g_reader;
bool g_stop = false;

Prepared Read(const Job& job) {
  Prepared p;
  p.generation = job.generation;
  const std::filesystem::path root = DataFolder();
  std::vector<uint8_t> data;
  auto model = std::make_shared<ngl_arms::Model>();
  if (!ReadFile(root / "arms" / (job.arms + ".c3arms"), &data, &p.why)) return p;
  if (!ngl_arms::Parse(data.data(), data.size(), model.get(), &p.why)) return p;
  p.ok = true;
  p.model = model;
  if (job.gloves.empty()) return p;
  for (const std::string& key : SurfaceKeys(*model)) {
    if (key.empty() || p.maps.count(key)) continue;
    Map& m = p.maps[key];
    if (!IsId(key)) {
      m.why = "its image name is not a plain name";
      continue;
    }
    const std::filesystem::path path = root / "gloves" / job.gloves / (key + ".c3camo");
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) {
      m.why = "the skin has no " + key;
      continue;
    }
    if (!cod3::ngl_camo::ReadColourLevels(path, &m.levels, &m.width, &m.height, &m.why)) m.levels.clear();
  }
  return p;
}

void ReaderMain() {
  for (;;) {
    Job job;
    {
      std::unique_lock<std::mutex> lock(g_mu);
      g_wake.wait(lock, [] { return g_stop || g_job.has_value(); });
      if (g_stop) return;
      job = std::move(*g_job);
      g_job.reset();
    }
    Prepared p = Read(job);
    std::lock_guard<std::mutex> lock(g_mu);
    g_ready = std::move(p);
  }
}

struct ReaderOwner {               // at the program's end: told to stop and waited for (it holds nothing else)
  ~ReaderOwner() {
    {
      std::lock_guard<std::mutex> lock(g_mu);
      g_stop = true;
    }
    g_wake.notify_all();
    if (g_reader && g_reader->joinable()) g_reader->join();
  }
} g_reader_owner;

void Ask(const Job& job) {
  std::lock_guard<std::mutex> lock(g_mu);
  g_job = job;
  if (!g_reader) g_reader = std::make_unique<std::thread>(ReaderMain);
  g_wake.notify_one();
}

// ---- the drawing thread's own (no lock) ---------------------------------------------------------------------------
struct SkeletonRig {
  uint32_t skeleton = 0;
  bool ok = false;
  size_t bones = 0;
  ngl_arms::Rig rig;
};
struct State {
  std::string arms, gloves;        // the settings this state is for
  uint32_t generation = 0;         // the job asked for them
  bool asked = false, ready = false, refused = false;
  std::shared_ptr<ngl_arms::Model> model;
  std::vector<std::string> keys;   // per surface
  std::unordered_map<std::string, Map> waiting;  // maps still to become textures (one a frame)
  std::unordered_map<std::string, uint32_t> textures;  // key -> texture (0: drawn flat)
  std::vector<uint32_t> surface_texture;
  std::vector<SkeletonRig> rigs;
  std::vector<uint32_t> arms_models;  // the models known as the arms by a colour map (x556: never "not the arms": one
                                      // model's surfaces have several colour maps, "-" among them)
  std::vector<uint32_t> meshes;
  std::vector<float> rest;
  bool no_room = false, cull_known = false, said_drawn = false;
  uint32_t cull = NGL_VK_CULL_NONE;
  uint64_t frame = ~0ull;
  std::vector<uint32_t> drawn;     // the objects whose arms are in this frame's draws
};
State g_state;
uint32_t g_generation = 0;
std::atomic<uint32_t> g_forgets{0};
uint32_t g_forgets_seen = 0;
size_t g_level_textures = 0, g_level_meshes = 0;  // made since the last Forget (ngl_vk frees them only then)

void SetCounts() {
  std::lock_guard<std::mutex> lock(g_counts_mu);
  g_counts.model_ready = g_state.ready;
  g_counts.textures = int(std::count_if(g_state.textures.begin(), g_state.textures.end(),
                                        [](const auto& t) { return t.second != 0; }));
  g_counts.flat = int(std::count(g_state.surface_texture.begin(), g_state.surface_texture.end(), 0u));
  g_counts.rigs = int(std::count_if(g_state.rigs.begin(), g_state.rigs.end(), [](const auto& r) { return r.ok; }));
}

void Refuse(const std::string& why) {
  g_state.refused = true;
  Log("arms " + g_state.arms + " not used: " + why);
  std::lock_guard<std::mutex> lock(g_counts_mu);
  ++g_counts.refused;
  g_counts.last_refusal = g_state.arms + ": " + why;
}

void Restart(const std::string& arms, const std::string& gloves) {
  g_state = State{};
  g_state.arms = arms, g_state.gloves = gloves;
  if (arms.empty()) return;
  if (!IsId(arms) || (!gloves.empty() && !IsId(gloves))) {
    Refuse("its name or its skin's is not a plain id");
    return;
  }
  g_state.generation = ++g_generation;
  g_state.asked = true;
  Ask(Job{g_state.generation, arms, gloves});
}

// which faces to drop, from our own triangles this frame (x545): those facing the eye counted by the way they wind
// on the screen; nine in ten one way (of 50 at least): the other way is dropped, else none.
uint32_t WindingCull(const ngl_arms::Model& model, const std::vector<float>& corners, const ngl_vk_mesh_draw& d) {
  double m[16];
  for (int r = 0; r < 4; ++r)
    for (int c = 0; c < 4; ++c) {
      m[4 * r + c] = 0;
      for (int k = 0; k < 4; ++k) m[4 * r + c] += double(d.world[4 * r + k]) * d.view_projection[4 * k + c];
    }
  const int cols[3] = {0, 1, 3};
  double a[3][3], b[3];
  for (int i = 0; i < 3; ++i) {
    for (int j = 0; j < 3; ++j) a[i][j] = m[4 * j + cols[i]];
    b[i] = -m[12 + cols[i]];
  }
  auto det3 = [](const double x[3][3]) {
    return x[0][0] * (x[1][1] * x[2][2] - x[1][2] * x[2][1]) - x[0][1] * (x[1][0] * x[2][2] - x[1][2] * x[2][0]) +
           x[0][2] * (x[1][0] * x[2][1] - x[1][1] * x[2][0]);
  };
  const double det = det3(a);
  int cw = 0, ccw = 0;
  if (std::fabs(det) > 1e-12 && std::isfinite(det)) {
    double eye[3];
    for (int j = 0; j < 3; ++j) {
      double x[3][3];
      for (int i = 0; i < 3; ++i)
        for (int k = 0; k < 3; ++k) x[i][k] = k == j ? b[i] : a[i][k];
      eye[j] = det3(x) / det;
    }
    size_t first = 0;
    for (const ngl_arms::Surface& s : model.surfaces) {
      for (size_t t = 0; t + 2 < s.indices.size(); t += 3) {
        double ndc[3][2], g[3] = {0, 0, 0}, n[3] = {0, 0, 0};
        bool ok = true;
        for (int v = 0; v < 3 && ok; ++v) {
          const float* c = &corners[(first + s.indices[t + v]) * NGL_VK_CORNER_FLOATS];
          double clip[4];
          for (int k = 0; k < 4; ++k) clip[k] = c[0] * m[k] + c[1] * m[4 + k] + c[2] * m[8 + k] + m[12 + k];
          if (!(clip[3] > 1e-4)) ok = false;
          ndc[v][0] = clip[0] / clip[3], ndc[v][1] = clip[1] / clip[3];
          for (int k = 0; k < 3; ++k) g[k] += c[k] / 3.0, n[k] += c[8 + k];
        }
        if (!ok) continue;
        const double area = (ndc[1][0] - ndc[0][0]) * (ndc[2][1] - ndc[0][1]) -
                            (ndc[2][0] - ndc[0][0]) * (ndc[1][1] - ndc[0][1]);
        const double facing = n[0] * (eye[0] - g[0]) + n[1] * (eye[1] - g[1]) + n[2] * (eye[2] - g[2]);
        if (!std::isfinite(area) || !std::isfinite(facing) || std::fabs(area) < 1e-12 || !(facing > 0)) continue;
        (area < 0 ? cw : ccw)++;
      }
      first += s.vertices.size();
    }
  }
  const int all = cw + ccw;
  Log("winding: of our triangles facing the eye " + std::to_string(cw) + " wind clockwise and " +
      std::to_string(ccw) + " the other way");
  if (all < 50) return NGL_VK_CULL_NONE;
  if (cw * 10 >= all * 9) return NGL_VK_CULL_CCW;
  if (ccw * 10 >= all * 9) return NGL_VK_CULL_CW;
  return NGL_VK_CULL_NONE;
}

const SkeletonRig* RigFor(const GameArms& game) {
  for (const SkeletonRig& r : g_state.rigs)
    if (r.skeleton == game.skeleton) return &r;
  if (g_state.rigs.size() >= kMostSkeletons || !game.read_bones) return nullptr;
  SkeletonRig r;
  r.skeleton = game.skeleton;
  std::vector<ngl_arms::GameBone> bones;
  std::string why;
  if (!game.read_bones(game.ctx, &bones)) why = "its bones can't be read";
  else r.ok = ngl_arms::MakeRig(*g_state.model, bones, &r.rig, &why);
  r.bones = bones.size();
  Log("rig on a skeleton of " + std::to_string(bones.size()) + " bones: " + (r.ok ? "made" : "not made: " + why));
  g_state.rigs.push_back(std::move(r));
  SetCounts();
  return &g_state.rigs.back();
}

bool MakeMeshes() {
  if (g_level_meshes + g_state.model->surfaces.size() > kMostMeshes) {
    Log("no room for more arms meshes in this level: the game's arms until the next one");
    g_state.no_room = true;
    return false;
  }
  ngl_arms::RestCorners(*g_state.model, NGL_VK_CORNER_FLOATS, 3, 7, kCornerWhite, 8, &g_state.rest);
  size_t first = 0;
  for (const ngl_arms::Surface& s : g_state.model->surfaces) {
    const uint32_t mesh = ngl_vk_mesh(&g_state.rest[first * NGL_VK_CORNER_FLOATS],
                                      static_cast<uint32_t>(s.vertices.size()), s.indices.data(),
                                      static_cast<uint32_t>(s.indices.size()));
    if (!mesh) {
      Log("no room for the arms' meshes: the game's arms until the next level");
      g_state.meshes.clear();
      g_state.no_room = true;
      return false;
    }
    g_state.meshes.push_back(mesh);
    ++g_level_meshes;
    first += s.vertices.size();
  }
  return true;
}

}  // namespace

bool On() { return !ArmsSetting().empty(); }

void BeginFrame(uint64_t frame) {
  const std::string arms = ArmsSetting(), gloves = GlovesSetting();
  const uint32_t forgets = g_forgets.load(std::memory_order_relaxed);
  if (arms != g_state.arms || gloves != g_state.gloves || forgets != g_forgets_seen) {
    if (forgets != g_forgets_seen) g_level_textures = g_level_meshes = 0;
    g_forgets_seen = forgets;
    Restart(arms, gloves);   // a change, or a level change: everything again from the files (a few hundred KB)
    SetCounts();
  }
  if (frame != g_state.frame) g_state.frame = frame, g_state.drawn.clear();
  if (!g_state.asked || g_state.refused) return;
  if (!g_state.model) {
    std::optional<Prepared> p;
    {
      std::lock_guard<std::mutex> lock(g_mu);
      if (g_ready && g_ready->generation == g_state.generation) p = std::move(g_ready), g_ready.reset();
    }
    if (!p) return;
    if (!p->ok) return Refuse(p->why.empty() ? "it can't be read" : p->why);
    g_state.model = p->model;
    g_state.keys = SurfaceKeys(*g_state.model);
    g_state.waiting = std::move(p->maps);
    Log("arms " + g_state.arms + ": read (" + std::to_string(g_state.model->bones.size()) + " bones, " +
        std::to_string(g_state.model->surfaces.size()) + " surfaces), " + std::to_string(g_state.waiting.size()) +
        " skin images wanted");
  }
  if (!g_state.waiting.empty()) {   // at most one texture a frame
    auto it = g_state.waiting.begin();
    uint32_t tex = 0;
    std::string why = it->second.why;
    if (!it->second.levels.empty()) {
      if (g_level_textures >= kMostTextures) {
        why = "no room for more arms textures in this level";
      } else {
        std::vector<const void*> ptrs;
        for (const auto& l : it->second.levels) ptrs.push_back(l.data());
        tex = ngl_vk_texture_levels(ptrs.data(), static_cast<uint32_t>(ptrs.size()), it->second.width,
                                    it->second.height);
        if (!tex) why = "ngl_vk has no room for its texture";
        else ++g_level_textures;
      }
    }
    Log("skin " + g_state.gloves + " image " + it->first + ": " +
        (tex ? "texture (" + std::to_string(it->second.width) + "x" + std::to_string(it->second.height) + ")"
             : "drawn flat: " + (why.empty() ? std::string("it can't be read") : why)));
    g_state.textures[it->first] = tex;
    g_state.waiting.erase(it);
  }
  if (!g_state.ready && g_state.waiting.empty()) {
    g_state.surface_texture.assign(g_state.model->surfaces.size(), 0);
    for (size_t k = 0; k < g_state.keys.size(); ++k) {
      auto t = g_state.textures.find(g_state.keys[k]);
      if (t != g_state.textures.end()) g_state.surface_texture[k] = t->second;
    }
    g_state.ready = true;
    SetCounts();
  }
}

bool DrawInstead(const GameArms& game, const ngl_vk_mesh_draw& d, size_t room, std::vector<ngl_vk_mesh_draw>* draws) {
  State& st = g_state;
  if (!st.ready || st.no_room || !game.model || !game.skeleton) return false;
  if (std::find(st.arms_models.begin(), st.arms_models.end(), game.model) == st.arms_models.end()) {
    // not (yet) known as the arms: multiplayer's front scene holds soldiers too, and the arms' own sleeve or glove
    // surface may come first with another colour map ("-"): the game's draw until one names the arms
    if (game.color_map.find("arms") == std::string::npos) return false;
    if (st.arms_models.size() >= 2 * kMostSkeletons) return false;
    st.arms_models.push_back(game.model);
    Log("a model is the first-person arms (its colour map \"" + game.color_map + "\")");
  }
  const SkeletonRig* r = RigFor(game);
  if (!r || !r->ok) return false;
  if (std::find(st.drawn.begin(), st.drawn.end(), game.object) != st.drawn.end()) return true;
  if (room < st.model->surfaces.size() || !game.read_skin) return false;
  if (st.meshes.empty() && !MakeMeshes()) return false;
  std::vector<ngl_arms::Mat4> skin;
  if (!game.read_skin(game.ctx, r->bones, &skin)) return false;
  thread_local std::vector<float> moved;
  moved = st.rest;
  if (!ngl_arms::SkinModel(*st.model, r->rig, skin, NGL_VK_CORNER_FLOATS, 8, &moved)) return false;
  std::vector<uint32_t> corners(st.model->surfaces.size());
  size_t first = 0;
  for (size_t k = 0; k < corners.size(); ++k) {   // all of them first: the game's arms when one can't be had
    const uint32_t count = static_cast<uint32_t>(st.model->surfaces[k].vertices.size());
    corners[k] = ngl_vk_frame_corners(&moved[first * NGL_VK_CORNER_FLOATS], count);
    if (!corners[k]) return false;
    first += count;
  }
  if (!st.cull_known) st.cull_known = true, st.cull = WindingCull(*st.model, moved, d);
  for (size_t k = 0; k < corners.size(); ++k) {
    // a plain lit draw: zero (every special shading off), then the game's matrices, fog and this object's light
    ngl_vk_mesh_draw ours{};
    ours.mesh = st.meshes[k];
    ours.corners = corners[k];
    ours.texture = k < st.surface_texture.size() ? st.surface_texture[k] : 0;
    ours.tint = ours.texture ? 0xFFFFFFu : kTint;
    ours.shading = NGL_VK_SHADE_LIGHT;
    ours.alpha = NGL_VK_ALPHA_OPAQUE;
    std::memcpy(ours.world, d.world, sizeof ours.world);
    std::memcpy(ours.view_projection, d.view_projection, sizeof ours.view_projection);
    std::memcpy(ours.fog, d.fog, sizeof ours.fog);
    ours.fog_color = d.fog_color;
    ours.lit = d.lit;
    std::memcpy(ours.ambient, d.ambient, sizeof ours.ambient);
    std::memcpy(ours.light_direction, d.light_direction, sizeof ours.light_direction);
    std::memcpy(ours.light_color, d.light_color, sizeof ours.light_color);
    ours.extra_light_count = d.extra_light_count;
    std::memcpy(ours.extra_light_direction, d.extra_light_direction, sizeof ours.extra_light_direction);
    std::memcpy(ours.extra_light_color, d.extra_light_color, sizeof ours.extra_light_color);
    ours.light_saturate = d.light_saturate;
    ours.sun_shadowed = d.sun_shadowed;
    ours.point_light_count = d.point_light_count;
    std::memcpy(ours.point_light, d.point_light, sizeof ours.point_light);
    ours.irradiance_rows = d.irradiance_rows;
    std::memcpy(ours.irradiance, d.irradiance, sizeof ours.irradiance);
    ours.scissor_x = d.scissor_x, ours.scissor_y = d.scissor_y;  // split screen: the player's part
    ours.scissor_width = d.scissor_width, ours.scissor_height = d.scissor_height;
    ours.cull = st.cull;
    draws->push_back(ours);
  }
  st.drawn.push_back(game.object);
  if (!st.said_drawn) {
    st.said_drawn = true;
    int textured = 0;
    for (uint32_t t : st.surface_texture) textured += t != 0;
    Log("arms " + st.arms + " drawn: " + std::to_string(corners.size()) + " surfaces (" + std::to_string(textured) +
        " with the skin's textures), " + std::to_string(moved.size() / NGL_VK_CORNER_FLOATS) + " corners");
  }
  return true;
}

void Forget() { g_forgets.fetch_add(1, std::memory_order_relaxed); }
#else   // no native renderer in this build: nothing to draw
}  // namespace
bool On() { return false; }
void BeginFrame(uint64_t) {}
void Forget() {}
#endif

// ---- F2 -----------------------------------------------------------------------------------------------------------
namespace {
std::mutex g_list_mu;
bool g_listed = false;
std::vector<Choice> g_arms_list, g_glove_list;

// [camo helper] 2026-11-23 x558: the folder listing and the skins' index.tsv are untrusted too: at most 4096 entries
// looked at, an index read only when it is a plain file of at most 1 MB, lines over 512 bytes skipped and a name's
// control bytes taken out (as ngl_camo's Rescan reads its index).
constexpr size_t kMostListed = 4096, kMostIndexBytes = 1u << 20, kMostIndexLine = 512;

void List() {
  if (g_listed) return;
  g_listed = true;
  g_arms_list.clear(), g_glove_list.clear();
  const std::filesystem::path root = DataFolder();
  std::error_code ec;
  size_t looked = 0;
  for (std::filesystem::directory_iterator it(root / "arms", ec), end;
       !ec && it != end && looked < kMostListed && g_arms_list.size() < 256; it.increment(ec), ++looked) {
    const std::filesystem::path& p = it->path();
    const std::string id = p.stem().string();
    if (p.extension() == ".c3arms" && IsId(id)) g_arms_list.push_back({id, id});
  }
  const std::filesystem::path index_path = root / "gloves" / "index.tsv";
  std::error_code size_ec, link_ec;
  const auto index_size = std::filesystem::file_size(index_path, size_ec);
  if (!size_ec && index_size <= kMostIndexBytes && !std::filesystem::is_symlink(index_path, link_ec)) {
    std::ifstream index(index_path);
    std::string line;
    while (g_glove_list.size() < 1024 && std::getline(index, line)) {
      if (!line.empty() && line.back() == '\r') line.pop_back();   // an index written with CR LF
      if (line.empty() || line[0] == '#' || line.size() > kMostIndexLine) continue;
      const size_t a = line.find('\t'), b = a == std::string::npos ? a : line.find('\t', a + 1);
      if (a == std::string::npos) continue;
      const std::string id = line.substr(0, a);
      if (!IsId(id)) continue;
      std::string name = line.substr(a + 1, b == std::string::npos ? std::string::npos : b - a - 1);
      name.erase(std::remove_if(name.begin(), name.end(),
                                [](char c) { return static_cast<uint8_t>(c) < 32 || c == 0x7F; }),
                 name.end());
      if (name.empty()) name = id;
      g_glove_list.push_back({id, name});
    }
  }
  std::sort(g_arms_list.begin(), g_arms_list.end(), [](const Choice& x, const Choice& y) { return x.id < y.id; });
}
}  // namespace

std::vector<Choice> ArmsList() {
  std::lock_guard<std::mutex> lock(g_list_mu);
  List();
  return g_arms_list;
}

std::vector<Choice> GloveList() {
  std::lock_guard<std::mutex> lock(g_list_mu);
  List();
  return g_glove_list;
}

void Rescan() {
  std::lock_guard<std::mutex> lock(g_list_mu);
  g_listed = false;
}

Counts GetCounts() {
  std::lock_guard<std::mutex> lock(g_counts_mu);
  return g_counts;
}

}  // namespace cod3::ngl_gloves
