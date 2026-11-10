/* ngl_vk's device, target and frame, for a lower layer that records frames itself. [acct2-bro, 2026-11-08]
 *
 * ngl_vk.h draws NGL's own things with ngl_vk's shaders and stays free of Vulkan types, for Big Red One's C code. A
 * renderer that brings its own pipelines (acct2-waw's ngl_vk_low.h: World at War, Call of Duty 4, and Call of Duty 2
 * next) uses this header instead. It records into ngl_vk's command buffer, draws into ngl_vk's target, and lets
 * ngl_vk submit and show the frame: through ngl_vk's own window, or ngl_vk_output for the SDK presenter's image.
 *
 * Use:
 *   1. ngl_vk_init() (ngl_vk's own device) or ngl_vk_init_on_device() (the SDK presenter's device), as for ngl_vk.h.
 *      On a borrowed device, also ngl_vk_set_device_features() with the features its owner enabled.
 *   2. ngl_vk_backend() for the handles. Make pipelines, buffers and images on `device` with them.
 *   3. Each frame, instead of ngl_vk_draw_quads() or ngl_vk_draw_picture() (never both in one frame):
 *        ngl_vk_begin_frame()  the command buffer to record into. It has waited for the last frame (its fence), so
 *                              the last frame's memory may be reused. `target` starts in
 *                              VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL and `depth` in
 *                              VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, both with undefined contents (clear
 *                              or fully draw them).
 *        ...draw into target (or your own images, then copy into it)...
 *        put target in `target_layout_at_end`, with a barrier from your last writes to transfer reads
 *        ngl_vk_end_frame()    copies the target out (to the host unless ngl_vk_set_readback(0)) and into the window,
 *                              submits under the owner's queue lock on a borrowed device, and shows the frame.
 *      ngl_vk_output() then works as after any ngl_vk frame.
 *   4. Never submit to `queue` yourself, and never wait on it: ngl_vk owns the frame's fence. On a borrowed device the
 *      queue belongs to its owner and is used only through ngl_vk.
 *
 * Threads: every call on the thread that draws (the one ngl_vk's frames run on). */
#ifndef NGL_VK_BACKEND_H
#define NGL_VK_BACKEND_H

#include <stdint.h>
#include <vulkan/vulkan.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ngl_vk_backend's struct has enabled_features (ngl_vk_low.c reads it when this is defined). */
#define NGL_VK_BACKEND_FEATURES 1

struct ngl_vk_backend {
    VkInstance instance;
    VkPhysicalDevice gpu;
    VkDevice device;
    VkQueue queue;                      /* see 4. above */
    uint32_t queue_family;
    uint32_t width, height;             /* the target's and the depth's size */
    VkImage target, depth;              /* the frame's color (what is shown) and depth */
    VkImageView target_view, depth_view;
    VkFormat target_format, depth_format;   /* VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_D32_SFLOAT */
    VkImageLayout target_layout_at_end; /* the target's layout at ngl_vk_end_frame(): TRANSFER_SRC_OPTIMAL */
    uint32_t frames_in_flight;          /* 1: ngl_vk_begin_frame() waits for the last frame */
    VkPhysicalDeviceFeatures enabled_features;  /* none on ngl_vk's own device; a borrowed device's as its owner set */
};

/* [f2-menus] Something drawn over the finished picture (Big Red One's F2 window): fn(commands, user) at the end of
 * every frame, after the last pass and before the target is copied out and shown, the target in
 * target_layout_at_end (which fn leaves it in). Record only; never submit. NULL: nothing. */
typedef void (*ngl_vk_overlay_fn)(VkCommandBuffer commands, void *user);
void ngl_vk_set_overlay(ngl_vk_overlay_fn fn, void *user);

/* 0 with *out filled once ngl_vk is up (ngl_vk_init or ngl_vk_init_on_device); -1 before. */
int ngl_vk_backend(struct ngl_vk_backend *out);

/* The features the borrowed device was made with (NULL: none). ngl_vk can't ask a device for them. */
void ngl_vk_set_device_features(const VkPhysicalDeviceFeatures *features);

/* A frame recorded by the caller. VK_NULL_HANDLE when ngl_vk isn't up, a frame is already begun, or Vulkan failed. */
VkCommandBuffer ngl_vk_begin_frame(void);

/* The frame begun by ngl_vk_begin_frame() submitted and shown. 0, or -1 when none was begun or Vulkan failed. */
int ngl_vk_end_frame(void);

#ifdef __cplusplus
}
#endif

#endif
