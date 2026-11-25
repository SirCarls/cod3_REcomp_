/* [acct2-bro] Finest Hour port: the Direct3D stand-ins that do part of the real function's work natively, because
 * the game can't go on without a real answer. Each is named in fh/config/stand_in_functions.tsv's stand_in_return
 * column and called by the generated stand-in (fh_stand_ins.c) with the arguments as the game passed them; what it
 * returns goes to the game in eax. Why each is needed, and the display modes offered: fh_d3d.c. */
#pragma once
#include <stdint.h>

/* Direct3D_GetAdapterModeCount(Adapter): how many display modes the adapter has. */
uint32_t fh_d3d_adapter_mode_count(uint32_t adapter);

/* Direct3D_EnumAdapterModes(Adapter, Mode, pMode): mode number Mode written to the D3DDISPLAYMODE at guest address
 * pMode. Returns S_OK (0), or D3DERR_INVALIDCALL for a mode that doesn't exist. */
uint32_t fh_d3d_enum_adapter_modes(uint32_t adapter, uint32_t mode, uint32_t out);

/* Direct3D_CreateDevice(..., pPresentationParameters, ppReturnedDeviceInterface): a zeroed 4 KB block of guest memory
 * written to *ppReturnedDeviceInterface as the device (always the same one), and the presentation parameters printed.
 * Returns S_OK (0). */
uint32_t fh_d3d_create_device(uint32_t present_params, uint32_t out);
/* [acct2-fh] 2026-11-13: what the game asked CreateDevice for: FullScreen_RefreshRateInHz and
 * FullScreen_PresentationInterval, as it passed them (both 0 before CreateDevice). For Swap's pacing (fh_present.c). */
void fh_d3d_presentation(uint32_t *refresh_hz, uint32_t *interval);

/* D3DDevice_GetRenderTarget2() and D3DDevice_GetDepthStencilSurface2(): the device's two surfaces (always the same
 * objects), made on first use at the back buffer's size with their own pixel memory. */
uint32_t fh_d3d_render_target(void);
uint32_t fh_d3d_depth_stencil_surface(void);

/* D3DDevice_GetBackBuffer2(BackBuffer): the render target, whichever buffer is asked for. */
uint32_t fh_d3d_back_buffer(uint32_t index);

/* D3DSurface_GetDesc(pThis, pDesc): the D3DSURFACE_DESC of one of the device's surfaces; zero sizes for any other. */
uint32_t fh_d3d_surface_get_desc(uint32_t surface, uint32_t desc);

/* D3DSurface_LockRect(pThis, pLockedRect, pRect, Flags): a device surface's pixels (at pRect's corner when given) and
 * pitch; any other surface gets the shared scratch buffer with a pitch of 0. Returns S_OK. */
uint32_t fh_d3d_surface_lock_rect(uint32_t surface, uint32_t locked, uint32_t rect);

/* 0x00221D30, D3D8's Lock2 (pResource, Flags), and 0x00221CE0, D3DVertexBuffer_Lock2 (pVB, Flags): the resource's
 * data address, its Data field with bit 31 set. */
uint32_t fh_d3d_resource_lock2(uint32_t resource, uint32_t flags);

/* 0x00221D60 (dst, src, count): copies count 32-bit words. Returns 0 (the real one returns nothing). */
uint32_t fh_d3d_copy_dwords(uint32_t dst, uint32_t src, uint32_t count);

/* [waw-decomp] 2026-11-09, phase 2. The render target's pixels as a host pointer, with its size, pitch and format, for
 * Swap to show (fh_present.c). Returns 0 before the game has asked for the render target. */
int fh_d3d_render_target_picture(const uint8_t **pixels, uint32_t *width, uint32_t *height, uint32_t *pitch,
                                 uint32_t *format);

/* How many times the game has locked the render target to write it by the CPU (its movie frames, XFONT text). */
uint32_t fh_d3d_render_target_locks(void);

/* Whether format (a D3DFORMAT number) is a 32-bit RGB one: 4 bytes a pixel, blue, green, red, then alpha or unused.
 * The device's surfaces are always linear here (fh_d3d.c makes them), whatever the number says. */
int fh_d3d_format_is_xrgb32(uint32_t format);

/* [waw-decomp] The other native answers the generated stand-ins call (fh_stand_ins.c includes only this header):
 * the device's state and draws, and Swap. */
#include "fh_d3d_state.h"
#include "fh_present.h"
#include "fh_pad.h"          /* [acct2-fh] 2026-11-13: XAPI's gamepad functions */
#include "fh_movie.h"        /* [acct2-fh] 2026-11-13: the Bink functions, the films through FFmpeg */
#include "fh_push.h"         /* [acct2-fh] 2026-11-13: D3DDevice_MakeSpace, the push buffer's constant loads */
#include "fh_vsh.h"          /* [acct2-fh] 2026-11-13: CreateVertexShader (the level's clones) */
#include "fh_aspect.h"       /* [acct2-fh] 2026-11-13: RenderWare's camera view window, Hor+ */
#include "fh_frame_clock.h"  /* [acct2-fh] 2026-11-14: the game's frame timer (0x000A44C0) */

#include "fh_sound_timing.h"     /* [r17] the DirectSound rows' native answers */
