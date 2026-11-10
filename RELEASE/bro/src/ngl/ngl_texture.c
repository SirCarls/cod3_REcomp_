/* [acct2-bro] Big Red One port: the part of nglTexture_Lock the game's own code depends on.
 *
 * nglTexture_Lock(texture, flags) (0x0006A820) locks level 0 of the texture's Direct3D texture (at +0x30) into the
 * texture's own D3DLOCKED_RECT, pitch at +0x1C and pixel pointer at +0x20, and returns 1. Callers then write through
 * that pointer: the movie player (0x000CE210) decodes every frame straight into a locked texture. The stand-in left
 * the rectangle as it was (zero, in the zeroed block nglGetBackBufferTexture hands out), so the movie wrote its first
 * frame from guest address 0 upward, over the game's own code, and the decoder's next switch jump read pixels where
 * its jump table had been.
 *
 * Until NGL is rewritten on Vulkan, every lock gets the same 8 MB of guest memory (a 1024 x 1024 texture at 32 bits
 * is 4 MB) and a pitch of width x 4 bytes; what is written there is thrown away. A texture with no size recorded at
 * +0x10 / +0x14 (the stand-in back buffer) is taken as the 640 x 480 screen the stand-ins report.
 *
 * Not done: the real function also sets up XGraphics' swizzler (Swizzler_Init) for swizzled textures; nothing is
 * drawn yet, so nothing reads swizzled pixels back.
 *
 * nglGetBackBufferTexture (0x0006AAE0) hands out the stand-in back buffer: its zeroed block with the screen's size
 * written at +0x10 / +0x14. The game's viewport code (0x00061B70) takes a scene's size from its render target (scene
 * +0x2F8) unless that texture's flags (+0x28) say "the screen"; with a size of 0 it divided by zero, every scene's
 * viewport (+0x338) came out NaN, and so did every projection nglUpdateScene made from it (run 102). That size wasn't
 * the one missing (run 103): the scenes had no render target at all (+0x2F8 zero, run 104), because nglSetRenderTarget
 * (0x00073A70) was a stand-in that did nothing. It only records the target in the current scene (scene +0x2F8, and
 * +0x08 = 6), so it is done here as the game does it.
 *
 * For checking what the game writes before anything is drawn: BRO_DUMP_LOCK=<n> BRO_DUMP_PATH=<file.ppm> saves the
 * lock buffer, as the n-th lock finds it (what the lock before it wrote), as a picture, taking the pixels as 32-bit
 * X8R8G8B8 in rows of the pitch. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "recomp_types.h"
#include "xbox_memory_layout.h"     /* xbox_HeapAlloc: guest memory */
#include "ngl_texture.h"
#include "ngl_log.h"                /* ngl_stand_in_block */

/* nglTexture fields (from nglTexture_Lock itself) */
#define TEXTURE_WIDTH        0x10u
#define TEXTURE_HEIGHT       0x14u
#define TEXTURE_LOCK_PITCH   0x1Cu
#define TEXTURE_LOCK_BITS    0x20u

#define LOCK_BYTES           (8u << 20)
#define BYTES_PER_PIXEL      4u
#define SCREEN_WIDTH         640u
#define SCREEN_HEIGHT        480u

#define NGL_VBLANK_COUNT     0x009028E4u    /* see ngl_frame_clock.c */
#define NGL_CURRENT_SCENE    0x0090259Cu    /* the scene being set up */
#define SCENE_STATE          0x08u
#define SCENE_RENDER_TARGET  0x2F8u

static uint32_t lock_buffer;
static uint32_t last_width, last_height;   /* the size the last lock handed out */
static uint32_t last_texture;              /* and the nglTexture it was for */

/* Save what the last lock's caller wrote, if this is the lock BRO_DUMP_LOCK names. */
static void dump_if_asked(unsigned long lock_number)
{
    const char *number = getenv("BRO_DUMP_LOCK");
    const char *path = getenv("BRO_DUMP_PATH");
    const uint8_t *pixels;
    unsigned long nonblack = 0;
    FILE *out;
    uint32_t x, y;

    if (!number || !path || strtoul(number, NULL, 0) != lock_number || !last_width)
        return;
    if ((uint64_t)last_width * BYTES_PER_PIXEL * last_height > LOCK_BYTES)
        return;
    out = fopen(path, "wb");
    if (!out) {
        fprintf(stderr, "[ngl] nglTexture_Lock: can't write %s\n", path);
        return;
    }
    pixels = (const uint8_t *)XBOX_PTR(lock_buffer);
    fprintf(out, "P6\n%u %u\n255\n", last_width, last_height);
    for (y = 0; y < last_height; y++) {
        for (x = 0; x < last_width; x++) {
            const uint8_t *p = pixels + ((size_t)y * last_width + x) * BYTES_PER_PIXEL;
            uint8_t rgb[3] = { p[2], p[1], p[0] };

            nonblack += (rgb[0] | rgb[1] | rgb[2]) != 0;
            fwrite(rgb, 1, sizeof rgb, out);
        }
    }
    fclose(out);
    fprintf(stderr, "[ngl] nglTexture_Lock: lock %lu, saved %ux%u to %s (%lu of %u pixels not black), vblank %u\n",
            lock_number, last_width, last_height, path, nonblack, last_width * last_height, MEM32(NGL_VBLANK_COUNT));
}

void ngl_texture_lock(uint32_t texture)
{
    static int reported;
    static unsigned long locks;
    uint32_t width, height;

    if (!texture)
        return;
    if (!lock_buffer) {
        lock_buffer = xbox_HeapAlloc(LOCK_BYTES, 4096);
        if (!lock_buffer) {
            fprintf(stderr, "[ngl] no guest memory for texture locks\n");
            return;
        }
        memset((void *)XBOX_PTR(lock_buffer), 0, LOCK_BYTES);
    }

    width = MEM32(texture + TEXTURE_WIDTH);
    height = MEM32(texture + TEXTURE_HEIGHT);
    if (!width || !height) {
        width = SCREEN_WIDTH;
        height = SCREEN_HEIGHT;
    }
    if ((uint64_t)width * BYTES_PER_PIXEL * height > LOCK_BYTES && reported++ < 4)
        fprintf(stderr, "[ngl] nglTexture_Lock: texture 0x%08X is %ux%u, more than the %u MB lock buffer\n",
                texture, width, height, LOCK_BYTES >> 20);
    if (++locks == 1)
        fprintf(stderr, "[ngl] nglTexture_Lock: first lock, texture 0x%08X, %ux%u\n", texture, width, height);
    dump_if_asked(locks);
    last_width = width;
    last_height = height;
    last_texture = texture;

    MEM32(texture + TEXTURE_LOCK_PITCH) = width * BYTES_PER_PIXEL;
    MEM32(texture + TEXTURE_LOCK_BITS) = lock_buffer;
}

uint32_t ngl_texture_last_locked(void)
{
    return last_texture;
}

int ngl_texture_last_lock(uint32_t *buffer, uint32_t *width, uint32_t *height, uint32_t *pitch)
{
    if (!lock_buffer || !last_width || (uint64_t)last_width * BYTES_PER_PIXEL * last_height > LOCK_BYTES)
        return 0;
    *buffer = lock_buffer;
    *width = last_width;
    *height = last_height;
    *pitch = last_width * BYTES_PER_PIXEL;
    return 1;
}

uint32_t ngl_back_buffer_texture(void)
{
    uint32_t texture = ngl_stand_in_block(NGL_NGLGETBACKBUFFERTEXTURE);

    if (texture && !MEM32(texture + TEXTURE_WIDTH)) {
        MEM32(texture + TEXTURE_WIDTH) = SCREEN_WIDTH;
        MEM32(texture + TEXTURE_HEIGHT) = SCREEN_HEIGHT;
    }
    return texture;
}

uint32_t ngl_set_render_target(uint32_t texture)
{
    uint32_t scene = MEM32(NGL_CURRENT_SCENE);

    if (scene) {
        MEM32(scene + SCENE_RENDER_TARGET) = texture;
        MEM32(scene + SCENE_STATE) = 6;
    }
    return 0;
}
