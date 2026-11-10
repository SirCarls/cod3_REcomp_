/* Big Red One port: what xboxrecomp's kernel library expects from the parts we leave out.
 *
 * The port draws through NGL on Vulkan (../../../ngl_vk) and plays sound through the game's own sound layer on a PC
 * audio library, so xboxrecomp's GPU push-buffer player (nv2a_pb_scan.c, nv2a_pb_exec.c, nv2a_vsh_interp.c,
 * nv2a_combiner.c), its audio-chip emulation (src/apu) and its framebuffer window and movie player (src/video) are
 * not built. The kernel library still calls a few of their functions; these are the do-nothing versions.
 *
 * Nothing here pretends to be a GPU or an audio chip: each function reports "nothing to do". If the game ever writes
 * GPU commands, nothing reads them. That only happens if a Direct3D call slips past the replacements: NGL's
 * functions and every Direct3D function called from outside NGL are stand-ins (bro/config/stand_in_functions.tsv)
 * that log the call and return. */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

/* ── GPU command stream (xboxrecomp src/kernel/nv2a_pb_*.c) ───────────────────────────────────────────────────── */

/* 0: no DMA walk, so the caller takes its plain path. */
int nv2a_pb_dma_walk(void) { return 0; }

void nv2a_pb_resync(uint32_t get_phys) { (void)get_phys; }

void nv2a_pb_scan(uint32_t put_phys) { (void)put_phys; }

void nv2a_pb_scan_segment(uint32_t start_va, uint32_t end_va) { (void)start_va; (void)end_va; }

void nv2a_pb_scan_report(void) {}

/* 0: flip pacing off. Frames are paced by the Vulkan swapchain instead. */
int nv2a_pb_exec_flip_pacing(void) { return 0; }

/* 0: no flip is ever held. */
int nv2a_pb_exec_flip_poll(void) { return 0; }

/* ── Audio chip (xboxrecomp src/apu) ──────────────────────────────────────────────────────────────────────────── */

/* false: the fault is not the audio chip's, so the kernel's own handler goes on (USB, then the usual error). */
#if defined(_WIN32)
#include <windows.h>
/* The audio chip's state, which the kernel's Windows memory setup checks for: there is none. */
void *g_apu_state = NULL;

bool apu_hook_handle_mmio(PCONTEXT ctx, uintptr_t fault_addr, uint32_t fault_xbox_va, int is_write)
{
    (void)ctx; (void)fault_addr; (void)fault_xbox_va; (void)is_write;
    return false;
}
#else
bool apu_hook_handle_mmio_posix(void *ucv, uintptr_t fault_addr, uint32_t fault_xbox_va, int is_write)
{
    (void)ucv; (void)fault_addr; (void)fault_xbox_va; (void)is_write;
    return false;
}
#endif

/* The audio chip's debug recording; there is none to close. */
int apu_wav_close_nowait(int wait_ms) { (void)wait_ms; return 0; }

/* ── Framebuffer window and movies (xboxrecomp src/video) ─────────────────────────────────────────────────────── */

/* The window is ngl_vk's swapchain; the kernel's framebuffer window is not used. */
void xbox_FramebufferWindowStart(void) {}

void xbox_FramebufferWindowSet(uint32_t fb_va, uint32_t pitch) { (void)fb_va; (void)pitch; }

void xbox_FramebufferWindowSetTitle(const uint16_t *name, int max_chars) { (void)name; (void)max_chars; }

/* Movies come in phase 6 (roadmap); until then a movie request is skipped and logged once. */
int xbox_VideoIsPlaying(void) { return 0; }

int xbox_VideoPlayFile(const char *host_path)
{
    static int told;
    if (!told) {
        fprintf(stderr, "[bro] movies are not played yet (%s skipped)\n", host_path ? host_path : "?");
        told = 1;
    }
    return 0;
}
