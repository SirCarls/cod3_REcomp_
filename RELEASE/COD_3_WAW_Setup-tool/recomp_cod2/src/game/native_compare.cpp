// cod2 - the native renderer beside the emulated GPU, live in the window (game/native_compare.h).
// [waw-decomp] 2026-11-11, after Call of Duty 3's (recomp/src/render/native_compare.cpp, rexglue-native, 2026-11-09):
// the same drawer and keys, with Call of Duty 2's settings and the picture placed as the presenter letterboxes it.

#include "game/native_compare.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ui/graphics_provider.h>
#include <rex/ui/immediate_drawer.h>
#include <rex/ui/presenter.h>
#include <rex/ui/ui_drawer.h>
#include <rex/ui/ui_event.h>
#include <rex/ui/virtual_key.h>
#include <rex/ui/window.h>
#include <rex/ui/window_listener.h>

REXCVAR_DEFINE_BOOL(cod2_native_compare, false, "cod2/Debug",
                    "Compare the native renderer with the emulated GPU live in the window: the emulated picture left "
                    "of a line, the native one right of it (Ctrl+Home on or off; Ctrl+Left/Right move the line, Ctrl+Up "
                    "switches between all emulated and all native, Ctrl+Down puts it back in the middle). Draws the "
                    "native picture every frame (slower while on)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_DOUBLE(cod2_native_compare_split, 0.5, "cod2/Debug",
                      "With cod2_native_compare, where the line is, across the picture: 0 all native, 1 all emulated")
    .range(0.0, 1.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace cod2::compare {
namespace {

using Clock = std::chrono::steady_clock;

// The line's moves (Ctrl+Left / Right, with Shift)
constexpr double kStep = 0.05;
constexpr double kFineStep = 0.005;
// The strip at the top of the picture that says which side is which (emulated blue, native orange), shown for this
// long after the compare comes on or the line moves; the rest of the time nothing covers the picture but the line.
constexpr auto kStripShown = std::chrono::seconds(3);
constexpr float kStripHeight = 4.0f;
// The line: white, kLineWidth wide, on a black one kLineEdge wider on each side so it shows on light and dark alike
constexpr float kLineWidth = 2.0f;
constexpr float kLineEdge = 1.0f;
// ImmediateVertex colors are ImGui's (red in the low byte): alpha, blue, green, red
constexpr uint32_t kWhite = 0xFFFFFFFFu;
constexpr uint32_t kBlack = 0xFF000000u;
constexpr uint32_t kEmulatedBlue = 0xFFFF7828u;  // red 40, green 120, blue 255
constexpr uint32_t kNativeOrange = 0xFF2080FFu;  // red 255, green 128, blue 32

// A native picture on its way to the window: pixels with red in the low byte, already through the gamma table.
struct Frame {
  std::vector<uint32_t> pixels;
  uint32_t width = 0, height = 0;
  uint64_t number = 0;  // 1, 2, 3... in the order they were handed; 0: none
};

std::atomic<bool> g_attached{false};

// Hand's side (the thread that draws the native frames, one frame at a time)
Frame g_filling;
uint64_t g_handed = 0;
bool g_hand_logged = false;

// Between the two: the latest frame handed and not yet taken (or a buffer handed back to be filled again)
std::mutex g_ready_mutex;
Frame g_ready;

double Split() { return std::clamp(REXCVAR_GET(cod2_native_compare_split), 0.0, 1.0); }

// The keys' moves of the line (the drawer sees the change, from here or the console, and shows the strip)
void SetSplit(double split, const char* why) {
  split = std::clamp(split, 0.0, 1.0);
  rex::cvar::SetFlagByName("cod2_native_compare_split", std::to_string(split));
  REXLOG_INFO("cod2: native compare: {} ({:.1f}% emulated on the left, {:.1f}% native on the right)", why,
              split * 100.0, (1.0 - split) * 100.0);
}

void Quad(rex::ui::ImmediateVertex* v, float left, float top, float right, float bottom, float u0, float u1,
          uint32_t color) {
  const auto corner = [color](float x, float y, float u, float t) {
    rex::ui::ImmediateVertex c;
    c.x = x;
    c.y = y;
    c.u = u;
    c.v = t;
    c.color = color;
    return c;
  };
  v[0] = corner(left, top, u0, 0.0f);
  v[1] = corner(right, top, u1, 0.0f);
  v[2] = corner(left, bottom, u0, 1.0f);
  v[3] = corner(right, top, u1, 0.0f);
  v[4] = corner(right, bottom, u1, 1.0f);
  v[5] = corner(left, bottom, u0, 1.0f);
}

// Where a picture of width x height goes in a target: as large as fits at its own shape, in the middle (the
// presenter's letterbox)
struct PictureRect {
  uint32_t x = 0, y = 0, width = 0, height = 0;
};
PictureRect PictureInView(uint32_t target_width, uint32_t target_height, uint32_t width, uint32_t height) {
  PictureRect r;
  if (!target_width || !target_height || !width || !height) return r;
  // the wider of the two decides: fill the target's width, or its height
  if (uint64_t{target_width} * height <= uint64_t{target_height} * width) {
    r.width = target_width;
    r.height = static_cast<uint32_t>(uint64_t{target_width} * height / width);
  } else {
    r.height = target_height;
    r.width = static_cast<uint32_t>(uint64_t{target_height} * width / height);
  }
  r.x = (target_width - r.width) / 2;
  r.y = (target_height - r.height) / 2;
  return r;
}

// The presenter's drawer: the latest native picture right of the line
class CompareDrawer final : public rex::ui::UIDrawer {
 public:
  explicit CompareDrawer(std::unique_ptr<rex::ui::ImmediateDrawer> drawer) : drawer_(std::move(drawer)) {}

  // Before it goes (Detach): the texture first, while its drawer still knows the presenter's submissions, then the
  // drawer leaves the presenter
  void Release() {
    texture_.reset();
    if (drawer_) drawer_->SetPresenter(nullptr);
  }

  void Draw(rex::ui::UIDrawContext& context) override {
    if (!REXCVAR_GET(cod2_native_compare)) {
      if (texture_ || !taken_.pixels.empty()) {  // off: its memory goes
        texture_.reset();
        taken_ = Frame{};
        std::lock_guard<std::mutex> lock(g_ready_mutex);
        g_ready = Frame{};
      }
      was_on_ = false;
      return;
    }
    const double split_now = Split();
    if (!was_on_ || split_now != last_split_) {  // just on, or the line moved: which side is which, for a moment
      was_on_ = true;
      last_split_ = split_now;
      strip_until_ = Clock::now() + kStripShown;
    }
    {
      std::lock_guard<std::mutex> lock(g_ready_mutex);
      if (g_ready.number > shown_ && !g_ready.pixels.empty()) std::swap(g_ready, taken_);
    }
    if (taken_.number > shown_) {
      texture_ = drawer_->CreateTexture(taken_.width, taken_.height, rex::ui::ImmediateTextureFilter::kLinear, false,
                                        reinterpret_cast<const uint8_t*>(taken_.pixels.data()));
      shown_ = taken_.number;
      if (!logged_) {
        logged_ = true;
        REXLOG_INFO("cod2: native compare: the native picture ({}x{}) is shown right of the line, the emulated GPU's "
                    "left of it{}",
                    taken_.width, taken_.height, texture_ ? "" : " (no: the texture wasn't made)");
      }
    }
    const uint32_t target_width = context.render_target_width(), target_height = context.render_target_height();
    if (!texture_ || !target_width || !target_height) return;
    // where the presenter puts the game's picture: as large as fits, at the picture's own shape, in the middle
    // (letterboxed; the native picture is the game's size, so its shape is the game picture's)
    const PictureRect r = PictureInView(target_width, target_height, taken_.width, taken_.height);
    if (!r.width || !r.height) return;
    const float split = float(split_now);
    const float left = float(r.x), top = float(r.y), right = left + float(r.width), bottom = top + float(r.height);
    const float line_x = std::round(left + split * float(r.width));  // on a whole pixel
    rex::ui::ImmediateVertex v[30];
    int textured = 0, plain = 0;
    if (split < 1.0f) {  // the native picture right of the line, each pixel of it where the game's picture has it
      Quad(v, line_x, top, right, bottom, (line_x - left) / float(r.width), 1.0f, kWhite);
      textured = 6;
    }
    rex::ui::ImmediateVertex* p = v + textured;
    if (split > 0.0f && split < 1.0f) {  // the line
      const float half = kLineWidth * 0.5f;
      Quad(p + plain, line_x - half - kLineEdge, top, line_x + half + kLineEdge, bottom, 0.0f, 0.0f, kBlack);
      plain += 6;
      Quad(p + plain, line_x - half, top, line_x + half, bottom, 0.0f, 0.0f, kWhite);
      plain += 6;
    }
    if (Clock::now() < strip_until_) {  // which side is which, for a moment
      if (line_x > left) {
        Quad(p + plain, left, top, line_x, top + kStripHeight, 0.0f, 0.0f, kEmulatedBlue);
        plain += 6;
      }
      if (line_x < right) {
        Quad(p + plain, line_x, top, right, top + kStripHeight, 0.0f, 0.0f, kNativeOrange);
        plain += 6;
      }
    }
    if (!textured && !plain) return;
    drawer_->Begin(context, float(target_width), float(target_height));
    rex::ui::ImmediateDrawBatch batch;
    batch.vertices = v;
    batch.vertex_count = textured + plain;
    drawer_->BeginDrawBatch(batch);
    rex::ui::ImmediateDraw draw;
    draw.primitive_type = rex::ui::ImmediatePrimitiveType::kTriangles;
    if (textured) {
      draw.count = textured;
      draw.base_vertex = 0;
      draw.texture = texture_.get();
      drawer_->Draw(draw);
    }
    if (plain) {
      draw.count = plain;
      draw.base_vertex = textured;
      draw.texture = nullptr;
      drawer_->Draw(draw);
    }
    drawer_->EndDrawBatch();
    drawer_->End();
  }

 private:
  std::unique_ptr<rex::ui::ImmediateDrawer> drawer_;
  std::unique_ptr<rex::ui::ImmediateTexture> texture_;
  Frame taken_;         // the frame in texture_ (its buffer goes back to be filled again at the next swap)
  uint64_t shown_ = 0;  // its number
  bool was_on_ = false;
  double last_split_ = -1.0;
  Clock::time_point strip_until_{};
  bool logged_ = false;
};

// The keys (render/native_compare.h)
class CompareKeys final : public rex::ui::WindowInputListener {
 public:
  explicit CompareKeys(std::function<bool()> overlay_open) : overlay_open_(std::move(overlay_open)) {}

  void OnKeyDown(rex::ui::KeyEvent& e) override {
    if (!e.is_ctrl_pressed() || e.is_alt_pressed() || e.is_super_pressed()) return;
    if (overlay_open_ && overlay_open_()) return;  // the F2 window has the keys
    using rex::ui::VirtualKey;
    const VirtualKey key = e.virtual_key();
    if (key == VirtualKey::kHome) {
      e.set_handled(true);
      if (e.prev_state()) return;  // held down: once
      const bool on = !REXCVAR_GET(cod2_native_compare);
      rex::cvar::SetFlagByName("cod2_native_compare", on ? "true" : "false");
      REXLOG_INFO("cod2: native compare {} (Ctrl+Home)", on ? "on" : "off");
      return;
    }
    if (!REXCVAR_GET(cod2_native_compare)) return;
    const double split = Split();
    switch (key) {
      case VirtualKey::kLeft:
        SetSplit(split - (e.is_shift_pressed() ? kFineStep : kStep), "the line moved left");
        break;
      case VirtualKey::kRight:
        SetSplit(split + (e.is_shift_pressed() ? kFineStep : kStep), "the line moved right");
        break;
      case VirtualKey::kUp:
        if (e.prev_state()) break;  // held down: once
        SetSplit(split >= 1.0 ? 0.0 : 1.0, split >= 1.0 ? "all native" : "all emulated");
        break;
      case VirtualKey::kDown:
        SetSplit(0.5, "the line in the middle");
        break;
      default:
        return;
    }
    e.set_handled(true);
  }

 private:
  std::function<bool()> overlay_open_;
};

struct Overlay {
  rex::ui::Presenter* presenter = nullptr;
  std::unique_ptr<CompareDrawer> drawer;
  std::unique_ptr<CompareKeys> keys;
};
std::unique_ptr<Overlay> g_overlay;  // UI thread

// Below ImGui's drawer (64): the F2 window and the console stay on top
constexpr size_t kDrawerZOrder = 32;

}  // namespace

void Attach(rex::ui::Window* window, rex::ui::Presenter* presenter, rex::ui::GraphicsProvider* provider,
            size_t input_z_order, std::function<bool()> overlay_open) {
  if (g_overlay || !window || !presenter || !provider) {
    if (!g_overlay)
      REXLOG_INFO("cod2: native compare: not available ({})", !presenter ? "no presenter" : "no graphics provider");
    return;
  }
  std::unique_ptr<rex::ui::ImmediateDrawer> immediate = provider->CreateImmediateDrawer();
  if (!immediate) {
    REXLOG_WARN("cod2: native compare: not available (the provider made no immediate drawer)");
    return;
  }
  immediate->SetPresenter(presenter);
  auto overlay = std::make_unique<Overlay>();
  overlay->presenter = presenter;
  overlay->drawer = std::make_unique<CompareDrawer>(std::move(immediate));
  overlay->keys = std::make_unique<CompareKeys>(std::move(overlay_open));
  presenter->AddUIDrawerFromUIThread(overlay->drawer.get(), kDrawerZOrder);
  window->AddInputListener(overlay->keys.get(), input_z_order);
  g_overlay = std::move(overlay);
  g_attached.store(true, std::memory_order_release);
}

void Detach(rex::ui::Window* window) {
  if (!g_overlay) return;
  g_attached.store(false, std::memory_order_release);
  if (window) window->RemoveInputListener(g_overlay->keys.get());
  g_overlay->presenter->RemoveUIDrawerFromUIThread(g_overlay->drawer.get());
  g_overlay->drawer->Release();
  g_overlay.reset();
}

bool Wanted() { return g_attached.load(std::memory_order_acquire) && REXCVAR_GET(cod2_native_compare); }

void Hand(const uint8_t* rgba, uint32_t width, uint32_t height, const GammaTable& table) {
  if (!rgba || !width || !height || !Wanted()) return;
  const Clock::time_point start = Clock::now();
  // Each channel's value through the table, already in its byte of the word (as ngl_side.cpp's Show), so a pixel is
  // three look-ups and three ORs
  uint32_t words[3][256];
  for (uint32_t channel = 0; channel < 3; ++channel) {
    for (uint32_t i = 0; i < 256; ++i) words[channel][i] = uint32_t{table[channel][i]} << (8 * channel);
  }
  const size_t count = size_t{width} * height;
  g_filling.pixels.resize(count);
  uint32_t* out = g_filling.pixels.data();
  for (size_t i = 0; i < count; ++i) {
    uint32_t word;
    std::memcpy(&word, rgba + 4 * i, 4);  // bytes r, g, b, a: red in the low byte (little-endian)
    out[i] = words[0][word & 0xFF] | words[1][(word >> 8) & 0xFF] | words[2][(word >> 16) & 0xFF] | 0xFF000000u;
  }
  g_filling.width = width;
  g_filling.height = height;
  {
    std::lock_guard<std::mutex> lock(g_ready_mutex);
    g_filling.number = ++g_handed;
    std::swap(g_filling, g_ready);
  }
  if (!g_hand_logged) {
    g_hand_logged = true;
    REXLOG_INFO("cod2: native compare: the first native picture handed over ({}x{}, {:.2f} ms through the gamma "
                "table)",
                width, height, std::chrono::duration<double, std::milli>(Clock::now() - start).count());
  }
}

}  // namespace cod2::compare
