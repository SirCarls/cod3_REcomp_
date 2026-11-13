// cod2 - the native renderer as the game's own picture, with the GPU emulation off (cod2_native_vulkan). [waw-decomp]
// 2026-11-11 4z61. See native_window.h. Ported from Call of Duty 3's recomp/src/game/ngl_side.cpp (StartNativeVulkan,
// WindowPresenter, Feed, Painter, Show, ShowOnGpu, HoldQueue), which rexglue-native and acct2-bro wrote and measured
// (side120 to side222): the same steps, with Call of Duty 2's names.

#include "game/native_window.h"

#include <chrono>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/system/gpu_plugin.h>

#if defined(COD2_NGL_VK)
extern "C" {
#include "ngl_vk.h"
}
#include <rex/system/interfaces/graphics.h>
#include <rex/system/kernel_state.h>
#include <rex/ui/presenter.h>
#include <rex/ui/vulkan/presenter.h>
#include <rex/ui/vulkan/provider.h>
#include <rex/ui/vulkan/util.h>
#endif

REXCVAR_DEFINE_BOOL(cod2_native_vulkan, false, "cod2/Display",
                    "Draw the game with the native Vulkan renderer (in development): the Xbox 360 GPU emulation is "
                    "off, and every frame is drawn from the game's own draw lists. Not everything is drawn yet "
                    "(no model shadows, no glow); a restart applies it")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DECLARE(std::string, cod2_graphics_api);  // cod2_app.cpp

namespace cod2::native {
namespace {

bool g_native = false;  // set once, before the game starts (StartNativeVulkan)

#if defined(COD2_NGL_VK)
// The window's presenter, and its Vulkan device in device when the window is drawn by Vulkan (else nullptr)
rex::ui::Presenter* WindowPresenter(const rex::ui::vulkan::VulkanDevice*& device) {
  auto* kernel = rex::system::kernel_state();
  auto* runtime = kernel ? kernel->emulator() : nullptr;
  auto* graphics = runtime ? runtime->graphics_system() : nullptr;
  auto* provider = graphics ? dynamic_cast<rex::ui::vulkan::VulkanProvider*>(graphics->provider()) : nullptr;
  device = provider ? provider->vulkan_device() : nullptr;
  return graphics ? graphics->presenter() : nullptr;
}

// A Vulkan handle as the 64-bit value ngl_vk.h takes (a pointer on 64-bit builds)
template <typename Handle>
uint64_t HandleBits(Handle handle) {
  if constexpr (std::is_pointer_v<Handle>) {
    return static_cast<uint64_t>(reinterpret_cast<uintptr_t>(handle));
  } else {
    return static_cast<uint64_t>(handle);
  }
}

// ngl_vk on the window's own device submits to the window's graphics queue, which the SDK guards with a mutex
// (VulkanDevice::AcquireQueue), so ngl_vk holds it around each use: taken and let go on the same thread.
thread_local std::optional<rex::ui::vulkan::VulkanDevice::Queue::Acquisition> t_queue_hold;

void* HoldQueue(void* user) {
  const auto* device = static_cast<const rex::ui::vulkan::VulkanDevice*>(user);
  t_queue_hold.emplace(device->AcquireQueue(device->queue_family_graphics_compute(), 0));
  return t_queue_hold->queue();
}

void ReleaseQueue(void*) { t_queue_hold.reset(); }

bool g_on_gpu = false;  // ngl_vk draws on the window's device
uint32_t g_width = 0, g_height = 0;  // ngl_vk's picture
int g_gpu_failures = 0;  // hand-overs on the GPU that failed in a row

// The presenter paints a picture on the thread that hands it over, and for Call of Duty 3 that held the game's thread
// for about a third of a second a frame (its side120b). So the tail of each hand-over (RefreshGuestOutput's
// defer_tail: the completion signal, the mailbox and the paint) runs on a thread of its own, and a picture that comes
// while the last one is still being painted isn't handed over: the window drops it, the game never waits for it.
class Painter {
 public:
  bool Busy() {
    std::lock_guard<std::mutex> lock(mutex_);
    return busy_;
  }

  // Runs tail on the painting thread (started the first time)
  void Run(rex::ui::Presenter::GuestOutputRefreshTail tail) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (!started_) {
        std::thread([this] { Loop(); }).detach();  // for the program's life (see g_painter)
        started_ = true;
      }
      tail_ = std::move(tail);
      busy_ = true;
    }
    wake_.notify_one();
  }

 private:
  void Loop() {
    for (;;) {
      rex::ui::Presenter::GuestOutputRefreshTail tail;
      {
        std::unique_lock<std::mutex> lock(mutex_);
        wake_.wait(lock, [this] { return static_cast<bool>(tail_); });
        tail = std::move(tail_);
        tail_ = nullptr;
      }
      tail();
      std::lock_guard<std::mutex> lock(mutex_);
      busy_ = false;
    }
  }

  std::mutex mutex_;
  std::condition_variable wake_;
  rex::ui::Presenter::GuestOutputRefreshTail tail_;
  bool busy_ = false, started_ = false;
};
Painter* g_painter = new Painter;  // never destroyed: its thread may still be painting when the program ends

// What reached the window: counted for the log (one line every 600 pictures)
struct Shown {
  bool failed = false;  // the window can't be given pictures: nothing more is tried
  uint64_t handed = 0, dropped = 0;
};
Shown g_shown;

void CountHandOver(bool refreshed, const char* how) {
  ++g_shown.handed;
  if (g_shown.handed == 1 || (!refreshed && g_shown.handed < 5)) {
    REXLOG_INFO("cod2: native Vulkan: the native picture {} in the window ({}x{}, {}, through the game's gamma "
                "table)", refreshed ? "shown" : "not taken by the presenter", g_width, g_height, how);
  }
  if (g_shown.handed % 600 == 0) {
    REXLOG_INFO("cod2: native Vulkan: {} pictures handed to the window, {} dropped while the last was painted",
                g_shown.handed, g_shown.dropped);
  }
}

// Through the CPU: an upload buffer on the window's device, filled with the picture (as the window takes it:
// A2B10G10R10, each 8-bit value widened) and copied into the window's picture on its graphics queue; the copy is
// waited for before the buffer is filled again.
struct Feed {
  const rex::ui::vulkan::VulkanDevice* device = nullptr;
  VkCommandPool pool = VK_NULL_HANDLE;
  VkCommandBuffer commands = VK_NULL_HANDLE;
  VkFence fence = VK_NULL_HANDLE;
  VkBuffer buffer = VK_NULL_HANDLE;
  VkDeviceMemory memory = VK_NULL_HANDLE;
  uint32_t memory_type = UINT32_MAX;
  uint32_t* mapped = nullptr;
  VkDeviceSize bytes = 0;
  bool pending = false;  // the fence waits on the last copy
};
Feed g_feed;

// The upload buffer and the rest, made once for the window's device (false: they couldn't be)
bool FeedReady(Feed& f, const rex::ui::vulkan::VulkanDevice* device, VkDeviceSize bytes) {
  if (f.device) return f.device == device && f.bytes >= bytes;
  const auto& dfn = device->functions();
  const VkDevice vk_device = device->device();
  VkCommandPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
  pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;  // begun again each frame
  pool_info.queueFamilyIndex = device->queue_family_graphics_compute();
  if (dfn.vkCreateCommandPool(vk_device, &pool_info, nullptr, &f.pool) != VK_SUCCESS) return false;
  VkCommandBufferAllocateInfo allocate_info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
  allocate_info.commandPool = f.pool;
  allocate_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  allocate_info.commandBufferCount = 1;
  if (dfn.vkAllocateCommandBuffers(vk_device, &allocate_info, &f.commands) != VK_SUCCESS) return false;
  VkFenceCreateInfo fence_info{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
  if (dfn.vkCreateFence(vk_device, &fence_info, nullptr, &f.fence) != VK_SUCCESS) return false;
  VkBufferCreateInfo buffer_info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
  buffer_info.size = bytes;
  buffer_info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
  buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  if (dfn.vkCreateBuffer(vk_device, &buffer_info, nullptr, &f.buffer) != VK_SUCCESS) return false;
  VkMemoryRequirements requirements;
  dfn.vkGetBufferMemoryRequirements(vk_device, f.buffer, &requirements);
  f.memory_type =
      rex::ui::vulkan::util::ChooseHostMemoryType(device->memory_types(), requirements.memoryTypeBits, false);
  if (f.memory_type == UINT32_MAX) return false;
  VkMemoryAllocateInfo memory_info{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  memory_info.allocationSize = requirements.size;
  memory_info.memoryTypeIndex = f.memory_type;
  if (dfn.vkAllocateMemory(vk_device, &memory_info, nullptr, &f.memory) != VK_SUCCESS) return false;
  if (dfn.vkBindBufferMemory(vk_device, f.buffer, f.memory, 0) != VK_SUCCESS) return false;
  void* mapped = nullptr;
  if (dfn.vkMapMemory(vk_device, f.memory, 0, VK_WHOLE_SIZE, 0, &mapped) != VK_SUCCESS) return false;
  f.mapped = static_cast<uint32_t*>(mapped);
  f.device = device;
  f.bytes = bytes;
  return true;
}
#endif  // COD2_NGL_VK

}  // namespace

bool StartNativeVulkan(rex::RuntimeConfig& config) {
  if (!REXCVAR_GET(cod2_native_vulkan)) return false;
#if defined(COD2_NGL_VK)
  if (config.graphics || config.gpu_plugin.empty()) {
    REXLOG_WARN("cod2: native Vulkan: {}; the emulated GPU draws",
                config.graphics ? "the graphics already started" : "no GPU plugin (cod2_no_gpu)");
    return false;
  }
#if defined(_WIN32)
  // The native picture is drawn with Vulkan (the host backend's window is always Vulkan's, and ngl_vk draws on its
  // device), so with Direct3D 12 chosen it stays off and the game keeps the API it was given.
  if (REXCVAR_GET(cod2_graphics_api) == "d3d12") {
    REXLOG_WARN("cod2: native Vulkan: needs the Vulkan graphics API, and cod2_graphics_api is d3d12; the emulated GPU "
                "draws, with Direct3D 12");
    return false;
  }
#endif
  // ngl_vk opens the system's Vulkan loader itself; without one the native picture stays off
  if (!ngl_vk_loader()) {
    REXLOG_WARN("cod2: native Vulkan: no Vulkan loader on this PC (ngl_vk can't start); the emulated GPU draws");
    return false;
  }
  // The host backend reads and answers the game's GPU commands as the emulated GPU does, but draws nothing and hands
  // nothing to the window: the picture is the native side's.
  config.graphics = rex::system::LoadGpuPlugin(config.gpu_plugin, "host");
  if (!config.graphics) {
    REXLOG_ERROR("cod2: native Vulkan: the GPU plugin's host backend didn't load; the emulated GPU draws");
    return false;
  }
  g_native = true;
  REXLOG_INFO("cod2: native Vulkan: the GPU emulation is off; the native renderer draws every frame into the window");
  return true;
#else
  (void)config;
  REXLOG_WARN("cod2: native Vulkan: this build has no ngl_vk (Vulkan headers or glslc missing); the emulated GPU "
              "draws");
  return false;
#endif
}

bool NativeVulkanOn() { return g_native; }

#if defined(COD2_NGL_VK)
int StartNglVkForWindow(uint32_t width, uint32_t height) {
  g_width = width;
  g_height = height;
  const rex::ui::vulkan::VulkanDevice* device = nullptr;
  WindowPresenter(device);
  if (device && ngl_vk_init_on_device(width, height, device->vulkan_instance()->instance(), device->physical_device(),
                                      device->device(), device->queue_family_graphics_compute(), 0, HoldQueue,
                                      ReleaseQueue, const_cast<void*>(static_cast<const void*>(device))) == 0) {
    g_on_gpu = true;
    REXLOG_INFO("cod2: native Vulkan: ngl_vk draws on the window's own Vulkan device; pictures go to it on the GPU");
    return 0;
  }
  REXLOG_WARN("cod2: native Vulkan: ngl_vk can't draw on the window's Vulkan device ({}); it uses one of its own, and "
              "each picture goes to the window through the CPU",
              device ? "ngl_vk_init_on_device failed" : "the window isn't drawn by Vulkan");
  return ngl_vk_init(width, height);
}

bool PictureOnGpu() { return g_on_gpu; }

void ShowOnGpu(const uint8_t table[3][256]) {
  if (g_shown.failed) return;
  if (g_painter->Busy()) {  // the last picture is still being painted
    ++g_shown.dropped;
    return;
  }
  using VulkanPresenter = rex::ui::vulkan::VulkanPresenter;
  const rex::ui::vulkan::VulkanDevice* device = nullptr;
  rex::ui::Presenter* presenter = WindowPresenter(device);
  if (!presenter || !device) {
    g_shown.failed = true;
    REXLOG_WARN("cod2: native Vulkan: the native picture can't be shown in the window ({})",
                !presenter ? "no presenter" : "the presenter isn't Vulkan's");
    return;
  }
  uint32_t words[256];  // red in the low byte, as ngl_vk_output takes it
  for (uint32_t i = 0; i < 256; ++i) {
    words[i] = uint32_t{table[0][i]} | uint32_t{table[1][i]} << 8 | uint32_t{table[2][i]} << 16 | 0xFF000000u;
  }
  bool called = false, drawn = false;
  const bool refreshed = presenter->RefreshGuestOutput(
      g_width, g_height, 16, 9, [&](rex::ui::Presenter::GuestOutputRefreshContext& context) -> bool {
        auto& vulkan_context = static_cast<VulkanPresenter::VulkanGuestOutputRefreshContext&>(context);
        called = true;
        context.SetIs8bpc(true);
        const uint32_t after = vulkan_context.image_ever_written_previously()
                                   ? uint32_t{VulkanPresenter::kGuestOutputInternalStageMask}
                                   : 0u;  // never used: nothing to wait for
        drawn = ngl_vk_output(HandleBits(vulkan_context.image()), HandleBits(vulkan_context.image_view()),
                              VulkanPresenter::kGuestOutputFormat, g_width, g_height, after,
                              VulkanPresenter::kGuestOutputInternalLayout,
                              VulkanPresenter::kGuestOutputInternalStageMask,
                              VulkanPresenter::kGuestOutputInternalAccessMask, words) == 0;
        return drawn;
      },
      [](rex::ui::Presenter::GuestOutputRefreshTail tail) { g_painter->Run(std::move(tail)); });
  if (called && !drawn && ++g_gpu_failures == 3) {
    g_shown.failed = true;
    REXLOG_WARN("cod2: native Vulkan: handing the picture to the window on the GPU failed 3 times; nothing more is "
                "shown");
  } else if (drawn) {
    g_gpu_failures = 0;
  }
  CountHandOver(refreshed, "handed over on the GPU");
}

void ShowPixels(const uint8_t* rgba, uint32_t width, uint32_t height, const uint8_t table[3][256]) {
  if (g_shown.failed || !rgba || !width || !height) return;
  if (g_painter->Busy()) {  // the last picture is still being painted
    ++g_shown.dropped;
    return;
  }
  const rex::ui::vulkan::VulkanDevice* device = nullptr;
  rex::ui::Presenter* presenter = WindowPresenter(device);
  const VkDeviceSize bytes = VkDeviceSize{width} * height * 4;
  if (!presenter || !device || !FeedReady(g_feed, device, bytes)) {
    g_shown.failed = true;
    REXLOG_WARN("cod2: native Vulkan: the native picture can't be shown in the window ({})",
                !presenter ? "no presenter" : !device ? "the presenter isn't Vulkan's" : "no upload buffer");
    return;
  }
  const auto& dfn = device->functions();
  const VkDevice vk_device = device->device();
  if (g_feed.pending) {  // the last copy is done before its buffer is filled again
    dfn.vkWaitForFences(vk_device, 1, &g_feed.fence, VK_TRUE, UINT64_MAX);
    dfn.vkResetFences(vk_device, 1, &g_feed.fence);
    g_feed.pending = false;
  }
  // Each channel's 10-bit word in place, made once a picture, so a pixel is three look-ups and three ORs
  const auto widen = [](uint32_t v) { return v << 2 | v >> 6; };  // 8 bits to 10
  uint32_t words[3][256];
  for (uint32_t channel = 0; channel < 3; ++channel) {
    for (uint32_t i = 0; i < 256; ++i) words[channel][i] = widen(table[channel][i]) << (10 * channel);
  }
  constexpr uint32_t kOpaque = 3u << 30;
  const size_t pixels = size_t{width} * height;
  uint32_t* out = g_feed.mapped;
  for (size_t i = 0; i < pixels; ++i) {
    uint32_t rgba_word;
    std::memcpy(&rgba_word, rgba + 4 * i, 4);  // bytes r, g, b, a: little-endian, r in the low byte
    out[i] = words[0][rgba_word & 0xFF] | words[1][(rgba_word >> 8) & 0xFF] | words[2][(rgba_word >> 16) & 0xFF] |
             kOpaque;
  }
  rex::ui::vulkan::util::FlushMappedMemoryRange(device, g_feed.memory, g_feed.memory_type);
  using VulkanPresenter = rex::ui::vulkan::VulkanPresenter;
  const bool refreshed = presenter->RefreshGuestOutput(
      width, height, 16, 9, [&](rex::ui::Presenter::GuestOutputRefreshContext& context) -> bool {
        auto& vulkan_context = static_cast<VulkanPresenter::VulkanGuestOutputRefreshContext&>(context);
        context.SetIs8bpc(true);
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        if (dfn.vkBeginCommandBuffer(g_feed.commands, &begin) != VK_SUCCESS) return false;
        VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        barrier.srcAccessMask = 0;  // all of it is written: what was there goes (after the presenter's last read)
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = vulkan_context.image();
        barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        dfn.vkCmdPipelineBarrier(g_feed.commands,
                                 vulkan_context.image_ever_written_previously()
                                     ? VulkanPresenter::kGuestOutputInternalStageMask
                                     : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                 VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
        VkBufferImageCopy region{};
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageExtent = {width, height, 1};
        dfn.vkCmdCopyBufferToImage(g_feed.commands, g_feed.buffer, vulkan_context.image(),
                                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;  // then as the presenter reads it
        barrier.dstAccessMask = VulkanPresenter::kGuestOutputInternalAccessMask;
        barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.newLayout = VulkanPresenter::kGuestOutputInternalLayout;
        dfn.vkCmdPipelineBarrier(g_feed.commands, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                 VulkanPresenter::kGuestOutputInternalStageMask, 0, 0, nullptr, 0, nullptr, 1,
                                 &barrier);
        if (dfn.vkEndCommandBuffer(g_feed.commands) != VK_SUCCESS) return false;
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &g_feed.commands;
        auto queue = device->AcquireQueue(device->queue_family_graphics_compute(), 0);
        if (dfn.vkQueueSubmit(queue.queue(), 1, &submit, g_feed.fence) != VK_SUCCESS) return false;
        g_feed.pending = true;
        return true;
      },
      [](rex::ui::Presenter::GuestOutputRefreshTail tail) { g_painter->Run(std::move(tail)); });
  CountHandOver(refreshed, "copied up through the CPU");
}
#else
int StartNglVkForWindow(uint32_t, uint32_t) { return -1; }
bool PictureOnGpu() { return false; }
void ShowOnGpu(const uint8_t[3][256]) {}
void ShowPixels(const uint8_t*, uint32_t, uint32_t, const uint8_t[3][256]) {}
#endif  // COD2_NGL_VK

}  // namespace cod2::native
