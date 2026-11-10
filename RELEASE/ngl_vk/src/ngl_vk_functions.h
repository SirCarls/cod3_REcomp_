/* ngl_vk's Vulkan functions, found at run time. [rexglue-native] 2026-11-09
 *
 * ngl_vk doesn't link the Vulkan loader. It opens the system's loader when it starts (libvulkan.so.1 on Linux,
 * vulkan-1.dll on Windows, which comes with the graphics driver, or Wine's own), so:
 *   - the Windows build made on Linux needs no import library for it (llvm-mingw has none);
 *   - a PC without Vulkan only goes without the native renderer: ngl_vk_init fails, and the game says so and draws
 *     with its GPU emulation.
 * It is the same loader the SDK and SDL open, so handles they made (ngl_vk_init_on_device's instance and device, a
 * window's surface) work with the functions found here.
 *
 * Every Vulkan function ngl_vk.c calls is a pointer with the function's own name (vulkan.h is included without its
 * prototypes, VK_NO_PROTOTYPES), so the calls in ngl_vk.c read as plain Vulkan. They are found in three steps, each
 * through the one before:
 *   1. ngl_vk_open_loader(): the loader, its vkGetInstanceProcAddr, and the functions needed before there is an
 *      instance (vkCreateInstance). Once per program; it logs the loader it found, or why there is none.
 *   2. ngl_vk_load_instance(instance): the instance's functions, once there is an instance (ngl_vk's own, or the
 *      owner's when ngl_vk draws on someone else's device).
 *   3. ngl_vk_load_device(device): the device's functions, through vkGetDeviceProcAddr, which gives the driver's own
 *      entry points rather than the loader's.
 * A missing function stops ngl_vk from starting, except the window extensions' (a surface, a swapchain): those are
 * used only with a window of ngl_vk's own, whose instance and device are made with the extensions on.
 *
 * Only ngl_vk.c includes this file. The pointers are static, so no other code (the SDK, a lower layer that links the
 * loader itself) can ever find them in place of the loader's own functions. */
#ifndef NGL_VK_FUNCTIONS_H
#define NGL_VK_FUNCTIONS_H

#include <stdio.h>
#include <string.h>

#include <vulkan/vulkan.h>

#ifndef VK_NO_PROTOTYPES
#error "ngl_vk_functions.h: define VK_NO_PROTOTYPES before the first #include <vulkan/vulkan.h>"
#endif

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#define NGL_VK_LOADER_FILE "vulkan-1.dll"
#else
#include <dlfcn.h>
#define NGL_VK_LOADER_FILE "libvulkan.so.1"
#endif

/* ---------------------------------------------------------------------------------------------------------------
 * The functions, by where they come from. A new Vulkan call in ngl_vk.c goes into one of these lists (the compiler
 * says which name is missing: without prototypes, a function not listed is undeclared). */

/* Before there is an instance: from vkGetInstanceProcAddr(NULL, ...) */
#define NGL_VK_GLOBAL_FUNCTIONS(F) \
    F(vkCreateInstance)

/* The instance's: their first parameter is the instance or a GPU */
#define NGL_VK_INSTANCE_FUNCTIONS(F) \
    F(vkDestroyInstance) \
    F(vkEnumeratePhysicalDevices) \
    F(vkGetPhysicalDeviceProperties) \
    F(vkGetPhysicalDeviceQueueFamilyProperties) \
    F(vkGetPhysicalDeviceMemoryProperties) \
    F(vkGetPhysicalDeviceFormatProperties) \
    F(vkEnumerateDeviceExtensionProperties) \
    F(vkCreateDevice) \
    F(vkGetDeviceProcAddr)

/* VK_KHR_surface, for a window of ngl_vk's own (may be missing otherwise) */
#define NGL_VK_SURFACE_FUNCTIONS(F) \
    F(vkDestroySurfaceKHR) \
    F(vkGetPhysicalDeviceSurfaceSupportKHR) \
    F(vkGetPhysicalDeviceSurfaceCapabilitiesKHR) \
    F(vkGetPhysicalDeviceSurfaceFormatsKHR) \
    F(vkGetPhysicalDeviceSurfacePresentModesKHR)

/* The device's: their first parameter is the device, a queue or a command buffer */
#define NGL_VK_DEVICE_FUNCTIONS(F) \
    /* the device and its queue */ \
    F(vkDestroyDevice) \
    F(vkGetDeviceQueue) \
    F(vkDeviceWaitIdle) \
    F(vkQueueSubmit) \
    F(vkQueueWaitIdle) \
    /* command buffers, fences and semaphores */ \
    F(vkCreateCommandPool) \
    F(vkDestroyCommandPool) \
    F(vkAllocateCommandBuffers) \
    F(vkResetCommandBuffer) \
    F(vkBeginCommandBuffer) \
    F(vkEndCommandBuffer) \
    F(vkCreateFence) \
    F(vkDestroyFence) \
    F(vkWaitForFences) \
    F(vkResetFences) \
    F(vkCreateSemaphore) \
    F(vkDestroySemaphore) \
    /* memory, buffers and images */ \
    F(vkAllocateMemory) \
    F(vkFreeMemory) \
    F(vkMapMemory) \
    F(vkUnmapMemory) \
    F(vkCreateBuffer) \
    F(vkDestroyBuffer) \
    F(vkGetBufferMemoryRequirements) \
    F(vkBindBufferMemory) \
    F(vkCreateImage) \
    F(vkDestroyImage) \
    F(vkGetImageMemoryRequirements) \
    F(vkBindImageMemory) \
    F(vkCreateImageView) \
    F(vkDestroyImageView) \
    F(vkCreateSampler) \
    F(vkDestroySampler) \
    /* passes, pipelines and descriptor sets */ \
    F(vkCreateShaderModule) \
    F(vkDestroyShaderModule) \
    F(vkCreateRenderPass) \
    F(vkDestroyRenderPass) \
    F(vkCreateFramebuffer) \
    F(vkDestroyFramebuffer) \
    F(vkCreateDescriptorSetLayout) \
    F(vkDestroyDescriptorSetLayout) \
    F(vkCreateDescriptorPool) \
    F(vkDestroyDescriptorPool) \
    F(vkAllocateDescriptorSets) \
    F(vkUpdateDescriptorSets) \
    F(vkCreatePipelineLayout) \
    F(vkDestroyPipelineLayout) \
    F(vkCreateGraphicsPipelines) \
    F(vkDestroyPipeline) \
    /* recording */ \
    F(vkCmdBeginRenderPass) \
    F(vkCmdEndRenderPass) \
    F(vkCmdBindPipeline) \
    F(vkCmdBindDescriptorSets) \
    F(vkCmdBindVertexBuffers) \
    F(vkCmdBindIndexBuffer) \
    F(vkCmdPushConstants) \
    F(vkCmdSetViewport) \
    F(vkCmdSetScissor) \
    F(vkCmdDraw) \
    F(vkCmdDrawIndexed) \
    F(vkCmdPipelineBarrier) \
    F(vkCmdCopyBuffer) \
    F(vkCmdCopyBufferToImage) \
    F(vkCmdCopyImage) \
    F(vkCmdCopyImageToBuffer) \
    F(vkCmdBlitImage) \
    F(vkCmdClearColorImage)

/* VK_KHR_swapchain, for a window of ngl_vk's own (may be missing otherwise) */
#define NGL_VK_SWAPCHAIN_FUNCTIONS(F) \
    F(vkCreateSwapchainKHR) \
    F(vkDestroySwapchainKHR) \
    F(vkGetSwapchainImagesKHR) \
    F(vkAcquireNextImageKHR) \
    F(vkQueuePresentKHR)

/* ---------------------------------------------------------------------------------------------------------------
 * The pointers: one for each function above, named as the function (PFN_vkCreateBuffer vkCreateBuffer, ...) */

#define NGL_VK_POINTER(name) static PFN_##name name;
static PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr;
NGL_VK_GLOBAL_FUNCTIONS(NGL_VK_POINTER)
NGL_VK_INSTANCE_FUNCTIONS(NGL_VK_POINTER)
NGL_VK_SURFACE_FUNCTIONS(NGL_VK_POINTER)
NGL_VK_DEVICE_FUNCTIONS(NGL_VK_POINTER)
NGL_VK_SWAPCHAIN_FUNCTIONS(NGL_VK_POINTER)
#undef NGL_VK_POINTER

/* How a pointer is found: through the instance (VK_NULL_HANDLE for the global ones), or through the device */
#define NGL_VK_FROM_INSTANCE(name) name = (PFN_##name)vkGetInstanceProcAddr(instance, #name);
#define NGL_VK_FROM_DEVICE(name) name = (PFN_##name)vkGetDeviceProcAddr(device, #name);
/* The first of a list's functions that wasn't found, into missing */
#define NGL_VK_NEEDED(name) if (!name && !missing) missing = #name;

/* The loader: open (library set), or tried and not there (tried set, library NULL) */
static struct {
    void *library;
    int tried;
} ngl_vk_loader_state;

/* Step 1: the system's Vulkan loader and its vkGetInstanceProcAddr, then the global functions. 0 when they are there.
 * The loader stays open until the program ends (the SDK and SDL keep it open too). */
static int ngl_vk_open_loader(void)
{
    VkInstance instance = VK_NULL_HANDLE;   /* the global functions are asked for without an instance */
    const char *missing = NULL;
    char why[160] = "";

    if (ngl_vk_loader_state.library)
        return 0;
    if (ngl_vk_loader_state.tried)
        return -1;
    ngl_vk_loader_state.tried = 1;
#if defined(_WIN32)
    {
        HMODULE library = LoadLibraryA(NGL_VK_LOADER_FILE);

        if (!library) {
            snprintf(why, sizeof why, "it can't be loaded (Windows error %lu)", (unsigned long)GetLastError());
            goto no_loader;
        }
        /* GetProcAddress gives a FARPROC; through void (*)(void), the plain function pointer type, to Vulkan's */
        vkGetInstanceProcAddr =
            (PFN_vkGetInstanceProcAddr)(void (*)(void))GetProcAddress(library, "vkGetInstanceProcAddr");
        ngl_vk_loader_state.library = (void *)library;
    }
#else
    {
        void *library = dlopen(NGL_VK_LOADER_FILE, RTLD_NOW | RTLD_LOCAL);
        void *symbol;

        if (!library) {
            const char *error = dlerror();

            snprintf(why, sizeof why, "it can't be opened (%s)", error ? error : "no reason given");
            goto no_loader;
        }
        /* dlsym gives a function's address as a void *; POSIX guarantees it converts, copied as its bytes */
        symbol = dlsym(library, "vkGetInstanceProcAddr");
        memcpy(&vkGetInstanceProcAddr, &symbol, sizeof vkGetInstanceProcAddr);
        ngl_vk_loader_state.library = library;
    }
#endif
    if (!vkGetInstanceProcAddr) {
        snprintf(why, sizeof why, "it has no vkGetInstanceProcAddr");
        goto no_loader;
    }
    NGL_VK_GLOBAL_FUNCTIONS(NGL_VK_FROM_INSTANCE)
    NGL_VK_GLOBAL_FUNCTIONS(NGL_VK_NEEDED)
    if (missing) {
        snprintf(why, sizeof why, "it has no %s", missing);
        goto no_loader;
    }
    fprintf(stderr, "[ngl_vk] Vulkan loader: %s (opened at run time)\n", NGL_VK_LOADER_FILE);
    return 0;

no_loader:
    /* the library (if it opened) stays loaded: it may be the SDK's too, and nothing here calls into it again */
    ngl_vk_loader_state.library = NULL;
    vkGetInstanceProcAddr = NULL;
    fprintf(stderr, "[ngl_vk] no Vulkan loader: %s %s; ngl_vk can't start\n", NGL_VK_LOADER_FILE, why);
    return -1;
}

/* Step 2: the instance's functions (and the surface's, when the instance has them). 0 when the needed ones are there. */
static int ngl_vk_load_instance(VkInstance instance)
{
    const char *missing = NULL;

    if (ngl_vk_open_loader() || !instance)
        return -1;
    NGL_VK_INSTANCE_FUNCTIONS(NGL_VK_FROM_INSTANCE)
    NGL_VK_SURFACE_FUNCTIONS(NGL_VK_FROM_INSTANCE)
    NGL_VK_INSTANCE_FUNCTIONS(NGL_VK_NEEDED)
    if (missing) {
        fprintf(stderr, "[ngl_vk] the Vulkan instance has no %s\n", missing);
        return -1;
    }
    return 0;
}

/* Step 3: the device's functions (and the swapchain's, when the device has them), straight from its driver. After
 * ngl_vk_load_instance (vkGetDeviceProcAddr). 0 when the needed ones are there. */
static int ngl_vk_load_device(VkDevice device)
{
    const char *missing = NULL;

    if (!vkGetDeviceProcAddr || !device)
        return -1;
    NGL_VK_DEVICE_FUNCTIONS(NGL_VK_FROM_DEVICE)
    NGL_VK_SWAPCHAIN_FUNCTIONS(NGL_VK_FROM_DEVICE)
    NGL_VK_DEVICE_FUNCTIONS(NGL_VK_NEEDED)
    if (missing) {
        fprintf(stderr, "[ngl_vk] the Vulkan device has no %s\n", missing);
        return -1;
    }
    return 0;
}

#undef NGL_VK_FROM_INSTANCE
#undef NGL_VK_FROM_DEVICE
#undef NGL_VK_NEEDED

#endif
