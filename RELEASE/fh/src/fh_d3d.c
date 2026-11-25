/* [acct2-bro] Finest Hour port: native answers for the Direct3D stand-ins that need one. See fh_d3d.h.
 *
 * Which functions these are (first boots, 2026-11-08 16:34-16:41): the game asks Direct3D for its display modes while
 * RenderWare's Xbox driver opens. 0x001CEC40 (the driver's "open", case 0 of its device function at 0x001CFA90) calls
 * 0x0022D5F0 with one argument and gives up when it returns 0, then 0x0022D600 with (0) for a count, then 0x0022D6A0
 * with (0, mode, a 20-byte buffer) for each mode. Those are the D3D8 calls Direct3DCreate8(SDKVersion),
 * Direct3D_GetAdapterModeCount(Adapter) and Direct3D_EnumAdapterModes(Adapter, Mode, D3DDISPLAYMODE *): the same
 * argument counts and the same 20-byte D3DDISPLAYMODE (Width, Height, RefreshRate, Flags, Format). The driver's
 * "start" (case 2, 0x001CEDC0) then calls 0x00220D20 with (0, 1, 0, 0x40, the presentation parameters at 0x003F0F18,
 * the device pointer's address 0x00406EE4): Direct3D_CreateDevice(Adapter, D3DDEVTYPE_HAL, no window,
 * D3DCREATE_HARDWARE_VERTEXPROCESSING, D3DPRESENT_PARAMETERS *, IDirect3DDevice8 **).
 *
 * How the game picks its mode (0x0012CBA0, then 0x001CEC40). It reads mode 0 into the mode it will ask for, keeps
 * mode 0's format, sets 640 x 480, sets the refresh rate from the console's video standard (60 Hz; 50 Hz for PAL
 * without PAL-60) and ORs D3DPRESENTFLAG bits into mode 0's flags from the console's video flags: always 0x100
 * (10X11PIXELASPECTRATIO), 0x10 (WIDESCREEN) with widescreen on, 0x40 (PROGRESSIVE) for 480p, and 0x20 (INTERLACED)
 * with video flag 0x2. The driver then takes the first mode whose width, height, format class and refresh rate match
 * and whose flags are exactly the asked flags masked with 0x1F0. With no such mode RwEngineOpen fails. The game does
 * not check that (0x000A4940 goes on whatever 0x000A4720 returns): it creates its first world anyway, and
 * RwFreeListAlloc (0x001C2E60) loops forever on the world plugin's freelist, which RwEngineStart never made (boot 4:
 * the one mode offered had flags 0).
 *
 * So the list below has 640 x 480 in 32-bit color with every flag combination the game can ask for, at 60 and 50 Hz,
 * and mode 0 is the plain 4:3 one (flags 0x100), so the game's ORs give what the console settings say. Phase 1 draws
 * nothing, so the mode only has to let the game go on. The native renderer draws at the PC's own resolution whatever
 * mode the game has; the widescreen flag will matter then (it sets the game's aspect ratio), and which one the game
 * gets will come from the port's settings. */
/*
 * The surfaces (boot 5, 2026-11-08 16:58): with the device made, the driver asks for the device's render target and
 * depth buffer (0x0021FB60, 0x0021FB80, no arguments, the result in eax; 0x001CEE68 keeps them as its frame and depth
 * rasters) and the game starts its frame loop. Each frame it draws text twice with the XDK's XFONT (0x00138F9A, called
 * from 0x0012A8B0 and 0x0012C664): D3DSurface_GetDesc(surface, D3DSURFACE_DESC *) at 0x00223EA0, then
 * D3DSurface_LockRect(surface, D3DLOCKED_RECT *, RECT *, flags) at 0x00223EC0, then the glyphs are written by the CPU
 * through the locked rectangle's pBits and Pitch with the desc's format, width and height. With the logging stand-ins
 * both structures were left as whatever was on the stack, so the text went wherever that pointed. Now the render
 * target and depth buffer are real 640 x 480 surfaces (the size and formats from CreateDevice's presentation
 * parameters) with their own pixel memory, GetDesc describes them, and LockRect hands out their pixels. Any other
 * surface gets an empty desc and a pitch of 0 into the shared scratch buffer. Phase 1 still shows nothing; the
 * pixels can be written to a file (FH_FRAME_DUMP, below) to see what the game draws there.
 */
/*
 * Textures (boot 6, 2026-11-08 17:06): with the effects image found, the game opened COD.PAK and read its loading
 * banner's texture dictionary (shell\fend\INSTANCE\xbox\LDBANNER.TXD, 0x000B3850). RenderWare's Xbox driver puts
 * each texture in contiguous memory (XPhysicalAlloc, through the kernel) and locks it for the pixels at 0x001C8260,
 * which calls 0x00221D30 (resource, flags) and keeps what comes back in eax as the pixel address. That function is
 * D3D8's Lock2 for a resource's data: unless flags has 0xA0 it waits for the GPU to be done with the resource
 * (0x002253C0), then returns the resource's Data field (offset 4) with bit 31 set, the contiguous address. With the
 * stand-in's 0 the pixels went to address 0, the dictionary came back empty, and RwTexDictionaryDestroy (0x001C4DB0)
 * then went round its texture list forever. Now Lock2 returns what the real one does (there is no GPU to wait for).
 * The D3D section's word copier next to it (0x00221D60: dst, src, count of 32-bit words; rep movsd and 64-byte MMX
 * blocks) does its copy too, since anything the game copies with it is data, not drawing.
 *
 * The front end (boot 13, 17:39) locks a vertex buffer every frame through 0x00221CE0, D3DVertexBuffer_Lock2 (pVB,
 * Flags): the same answer, Data with bit 31 set, after waiting for the GPU unless Flags has 0xA0. With the stand-in's
 * 0 the front end was writing its vertices at guest address 0. Both rows use fh_d3d_resource_lock2.
 */
#include "fh_d3d.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "recomp_types.h"           /* MEM32: guest memory */
#include "xbox_memory_layout.h"     /* xbox_HeapAlloc */
#include "fh_log.h"                 /* fh_stand_in_scratch */
#include "fh_draw.h"                /* fh_draw_resource_locked */
#include "fh_host.h"                /* [waw-decomp/fh2] 11-23: UTF-8 paths */

#define D3D_OK                  0u
#define D3DERR_INVALIDCALL      0x8876086Cu
#define D3DERR_OUTOFVIDEOMEMORY 0x8876017Cu
#define D3DFMT_LIN_X8R8G8B8     0x1Eu   /* the Xbox D3D8's linear 32-bit format without alpha */
#define D3DFMT_LIN_D24S8        0x2Eu   /* linear 24-bit depth, 8-bit stencil */
#define D3DRTYPE_SURFACE        1u
#define D3DUSAGE_RENDERTARGET   0x1u
#define D3DUSAGE_DEPTHSTENCIL   0x2u

/* D3DPRESENTFLAG bits the game asks for (Xbox D3D8) */
#define FLAG_WIDESCREEN         0x010u
#define FLAG_INTERLACED         0x020u
#define FLAG_PROGRESSIVE        0x040u
#define FLAG_10X11_PIXELS       0x100u  /* 10X11PIXELASPECTRATIO: standard-definition 4:3 */

#define DEVICE_BYTES            4096u
#define SURFACE_BYTES           64u     /* a D3DSurface object; the game only passes its address back */
#define MAX_SIDE                4096u
#define FRAME_DUMP_AT_DEFAULT   300u    /* FH_FRAME_DUMP: written when the render target is locked this many times */

struct display_mode {
    uint32_t width, height, refresh_rate, flags, format;
};

/* [acct2-fh] 2026-11-13: the presentation the game asked CreateDevice for (fh_d3d_presentation, fh_present.c's
 * pacing). 0 until CreateDevice: the pacing then holds frames to 60 a second. */
static uint32_t present_refresh_hz, present_interval;

#define MODES_AT(hz) \
    { 640, 480, hz, FLAG_10X11_PIXELS,                                      D3DFMT_LIN_X8R8G8B8 }, \
    { 640, 480, hz, FLAG_10X11_PIXELS | FLAG_WIDESCREEN,                    D3DFMT_LIN_X8R8G8B8 }, \
    { 640, 480, hz, FLAG_10X11_PIXELS | FLAG_INTERLACED,                    D3DFMT_LIN_X8R8G8B8 }, \
    { 640, 480, hz, FLAG_10X11_PIXELS | FLAG_INTERLACED | FLAG_WIDESCREEN,  D3DFMT_LIN_X8R8G8B8 }, \
    { 640, 480, hz, FLAG_10X11_PIXELS | FLAG_PROGRESSIVE,                   D3DFMT_LIN_X8R8G8B8 }, \
    { 640, 480, hz, FLAG_10X11_PIXELS | FLAG_PROGRESSIVE | FLAG_WIDESCREEN, D3DFMT_LIN_X8R8G8B8 }

static const struct display_mode modes[] = {
    MODES_AT(60),       /* mode 0 first: the one the game starts from */
    MODES_AT(50),
};
#define MODE_COUNT  ((uint32_t)(sizeof modes / sizeof modes[0]))

/* A surface the device owns: the guest object handed to the game, and its pixels in guest memory. */
struct surface {
    uint32_t object, pixels;
    uint32_t width, height, pitch, format, usage;
    uint32_t locks;
};

static uint32_t device;                 /* guest address of the device object handed to the game */
static struct surface render_target, depth_buffer;
static uint32_t back_width = 640, back_height = 480;
static uint32_t back_format = D3DFMT_LIN_X8R8G8B8, depth_format = D3DFMT_LIN_D24S8;

static uint32_t guest_zalloc(uint32_t bytes, uint32_t align)
{
    uint32_t p = xbox_HeapAlloc(bytes, align);

    if (p)
        memset((void *)XBOX_PTR(p), 0, bytes);
    else
        fprintf(stderr, "[fh] no guest memory for %u bytes\n", bytes);
    return p;
}

/* The render target or depth buffer, made on first use with the size CreateDevice was given. */
static uint32_t device_surface(struct surface *s, uint32_t format, uint32_t usage)
{
    if (!s->object) {
        s->width = back_width;
        s->height = back_height;
        s->pitch = back_width * 4;      /* both formats are 32 bits a pixel */
        s->format = format;
        s->usage = usage;
        s->object = guest_zalloc(SURFACE_BYTES, 16);
        s->pixels = guest_zalloc(s->pitch * s->height, 4096);
    }
    return s->object;
}

static struct surface *find_surface(uint32_t object)
{
    if (object && object == render_target.object)
        return &render_target;
    if (object && object == depth_buffer.object)
        return &depth_buffer;
    return NULL;
}

/* FH_FRAME_DUMP=FILE.ppm: the render target's pixels written once, as a binary PPM, when it has been locked
 * FH_FRAME_DUMP_AT times (default 300). Nothing clears it in phase 1, so it holds everything the CPU drew there. */
static void maybe_dump_frame(const struct surface *s)
{
    static int done;
    const char *path = fh_getenv_utf8("FH_FRAME_DUMP");
    const char *at = getenv("FH_FRAME_DUMP_AT");
    uint32_t when = at && *at ? (uint32_t)strtoul(at, NULL, 10) : FRAME_DUMP_AT_DEFAULT;
    FILE *f;

    if (done || !path || !*path || s->locks != when || !s->pixels)
        return;
    done = 1;
    f = fh_fopen(path, "wb");
    if (!f) {
        fprintf(stderr, "[fh] frame dump: cannot write %s\n", path);
        return;
    }
    fprintf(f, "P6\n%u %u\n255\n", s->width, s->height);
    for (uint32_t y = 0; y < s->height; y++) {
        const uint8_t *row = (const uint8_t *)XBOX_PTR(s->pixels + y * s->pitch);
        for (uint32_t x = 0; x < s->width; x++) {
            const uint8_t rgb[3] = { row[4 * x + 2], row[4 * x + 1], row[4 * x + 0] };   /* X8R8G8B8 is B, G, R, X */
            fwrite(rgb, 1, 3, f);
        }
    }
    fclose(f);
    fprintf(stderr, "[fh] frame dump: %s (%u x %u, after %u locks)\n", path, s->width, s->height, s->locks);
}

uint32_t fh_d3d_adapter_mode_count(uint32_t adapter)
{
    return adapter == 0 ? MODE_COUNT : 0;
}

uint32_t fh_d3d_enum_adapter_modes(uint32_t adapter, uint32_t mode, uint32_t out)
{
    const struct display_mode *m;

    if (adapter != 0 || mode >= MODE_COUNT || out == 0)
        return D3DERR_INVALIDCALL;
    m = &modes[mode];
    MEM32(out + 0) = m->width;
    MEM32(out + 4) = m->height;
    MEM32(out + 8) = m->refresh_rate;
    MEM32(out + 12) = m->flags;
    MEM32(out + 16) = m->format;
    return D3D_OK;
}

uint32_t fh_d3d_create_device(uint32_t present_params, uint32_t out)
{
    if (out == 0)
        return D3DERR_INVALIDCALL;
    if (!device) {
        device = guest_zalloc(DEVICE_BYTES, 16);
        if (!device) {
            MEM32(out) = 0;
            return D3DERR_OUTOFVIDEOMEMORY;
        }
    }
    /* D3DPRESENT_PARAMETERS: BackBufferWidth, BackBufferHeight, BackBufferFormat, BackBufferCount (0-12),
     * AutoDepthStencilFormat at 36, Flags at 40, FullScreen_RefreshRateInHz at 44,
     * FullScreen_PresentationInterval at 48 ([acct2-fh] 11-13: kept for Swap's pacing, fh_present.c). */
    if (present_params) {
        uint32_t w = MEM32(present_params), h = MEM32(present_params + 4);

        if (w && h && w <= MAX_SIDE && h <= MAX_SIDE && !render_target.object) {
            back_width = w;
            back_height = h;
        }
        if (MEM32(present_params + 8))
            back_format = MEM32(present_params + 8);
        if (MEM32(present_params + 36))
            depth_format = MEM32(present_params + 36);
        present_refresh_hz = MEM32(present_params + 44);
        present_interval = MEM32(present_params + 48);
        fprintf(stderr, "[fh] Direct3D_CreateDevice: back buffer %u x %u, format 0x%X, %u buffer(s); depth format "
                "0x%X; flags 0x%X, %u Hz, presentation interval 0x%X\n", MEM32(present_params),
                MEM32(present_params + 4), MEM32(present_params + 8), MEM32(present_params + 12),
                MEM32(present_params + 36), MEM32(present_params + 40), present_refresh_hz, present_interval);
    }
    MEM32(out) = device;
    return D3D_OK;
}

void fh_d3d_presentation(uint32_t *refresh_hz, uint32_t *interval)
{
    *refresh_hz = present_refresh_hz;
    *interval = present_interval;
}

uint32_t fh_d3d_render_target(void)
{
    return device_surface(&render_target, back_format, D3DUSAGE_RENDERTARGET);
}

uint32_t fh_d3d_depth_stencil_surface(void)
{
    return device_surface(&depth_buffer, depth_format, D3DUSAGE_DEPTHSTENCIL);
}

/* D3DDevice_GetBackBuffer2 (0x0021F7E0, boot 10): the game's movie player (0x0012CF60) asks for back buffer 0 and
 * locks it to write each Bink frame by the CPU. The real one returns one of the device's buffers ([device + 0x1A14 +
 * 4 * i]; -1 is the front buffer) with a reference added. There is one buffer here: the render target. */
uint32_t fh_d3d_back_buffer(uint32_t index)
{
    (void)index;
    return fh_d3d_render_target();
}

uint32_t fh_d3d_surface_get_desc(uint32_t object, uint32_t desc)
{
    const struct surface *s = find_surface(object);

    if (!desc)
        return D3DERR_INVALIDCALL;
    /* D3DSURFACE_DESC: Format, Type, Usage, Size, MultiSampleType, Width, Height */
    MEM32(desc + 0) = s ? s->format : 0;
    MEM32(desc + 4) = D3DRTYPE_SURFACE;
    MEM32(desc + 8) = s ? s->usage : 0;
    MEM32(desc + 12) = s ? s->pitch * s->height : 0;
    MEM32(desc + 16) = 0;
    MEM32(desc + 20) = s ? s->width : 0;
    MEM32(desc + 24) = s ? s->height : 0;
    return D3D_OK;
}

uint32_t fh_d3d_surface_lock_rect(uint32_t object, uint32_t locked, uint32_t rect)
{
    struct surface *s = find_surface(object);
    uint32_t bits;

    if (!locked)
        return D3DERR_INVALIDCALL;
    if (!s || !s->pixels) {
        MEM32(locked + 0) = 0;                          /* Pitch */
        MEM32(locked + 4) = fh_stand_in_scratch();      /* pBits */
        return D3D_OK;
    }
    s->locks++;
    maybe_dump_frame(s);
    bits = s->pixels;
    if (rect) {                                         /* RECT: left, top, right, bottom */
        uint32_t left = MEM32(rect), top = MEM32(rect + 4);

        if (left < s->width && top < s->height)
            bits += top * s->pitch + left * 4;
    }
    MEM32(locked + 0) = s->pitch;
    MEM32(locked + 4) = bits;
    return D3D_OK;
}

uint32_t fh_d3d_resource_lock2(uint32_t resource, uint32_t flags)
{
    (void)flags;        /* the real one waits for the GPU unless flags has 0xA0; there is none to wait for */
    /* [acct2-fh] 2026-11-14: whoever locks a texture or a palette writes it next (RenderWare's raster lock,
     * 0x001C8260, when it loads a texture dictionary): the drawing layer lets go of what it made from it. */
    fh_draw_resource_locked(resource);
    return resource ? MEM32(resource + 4) | 0x80000000u : 0;
}

uint32_t fh_d3d_copy_dwords(uint32_t dst, uint32_t src, uint32_t count)
{
    if (dst && src && count)
        memmove((void *)XBOX_PTR(dst), (const void *)XBOX_PTR(src), (size_t)count * 4);
    return 0;
}

/* [waw-decomp] 2026-11-09: what Swap shows (fh_present.c). The render target is the one surface the game's CPU draws
 * into (its movie frames and XFONT text) and, once the native draws exist, the one they draw into. */
int fh_d3d_render_target_picture(const uint8_t **pixels, uint32_t *width, uint32_t *height, uint32_t *pitch,
                                 uint32_t *format)
{
    if (!render_target.object || !render_target.pixels)
        return 0;
    *pixels = (const uint8_t *)XBOX_PTR(render_target.pixels);
    *width = render_target.width;
    *height = render_target.height;
    *pitch = render_target.pitch;
    *format = render_target.format;
    return 1;
}

uint32_t fh_d3d_render_target_locks(void)
{
    return render_target.locks;
}

int fh_d3d_format_is_xrgb32(uint32_t format)
{
    /* swizzled A8R8G8B8 and X8R8G8B8, linear A8R8G8B8 and X8R8G8B8: the Xbox D3D8's 32-bit RGB formats (the game asks
     * for 0x1E, LIN_X8R8G8B8: CreateDevice's log) */
    return format == 0x06u || format == 0x07u || format == 0x12u || format == D3DFMT_LIN_X8R8G8B8;
}
