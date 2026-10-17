#!/usr/bin/env python3
"""Call of Duty 3 (Xbox 360): the Direct3D device's state shadow (D3D9 2.0.3424, linked into default.xex).

Every Set* call writes the state into the device object and sets a "pending" bit; the next draw sends
the pending state to the GPU (0x822F3620 / 0x822F3A28). So at the moment of a draw call the device
object holds the complete state the GPU is about to get. This module records where each GPU register
lives in that object and which pending bit covers it, as read from the draw-time flush code; see
docs/COD3_D3D.md.

    python3 cod3_d3d_layout.py                 print the layout table
    python3 cod3_d3d_layout.py --reg 0x2200    where register 0x2200 is kept
    python3 cod3_d3d_layout.py --decode DUMP   decode a big-endian dump of the device object (0x5000 bytes;
                                               e.g. read from guest memory at *0x8290F24C)

Pending bits use LSB numbering (bit 0 = value 1) of the big-endian u64 at the given device offset.
"""
import argparse, struct, sys

DEVICE_POINTER = 0x8290F24C   # the game's global: IDirect3DDevice9* (written by Direct3D_CreateDevice)
DEVICE_SIZE = 0x5000

# Register names used below (from the Xenos register table; dword register indices).
REGISTER_NAMES = {
    0x2000: 'RB_SURFACE_INFO', 0x2001: 'RB_COLOR_INFO', 0x2002: 'RB_DEPTH_INFO', 0x2003: 'RB_COLOR1_INFO',
    0x2004: 'RB_COLOR2_INFO', 0x2005: 'RB_COLOR3_INFO', 0x2006: 'COHER_DEST_BASE_0', 0x2007: 'COHER_DEST_BASE_1',
    0x2008: 'COHER_DEST_BASE_2', 0x2009: 'COHER_DEST_BASE_3', 0x200A: 'COHER_DEST_BASE_4', 0x200B: 'COHER_DEST_BASE_5',
    0x200C: 'COHER_DEST_BASE_6', 0x200D: 'COHER_DEST_BASE_7', 0x200E: 'PA_SC_SCREEN_SCISSOR_TL',
    0x200F: 'PA_SC_SCREEN_SCISSOR_BR',
    0x2100: 'VGT_MAX_VTX_INDX', 0x2101: 'VGT_MIN_VTX_INDX', 0x2102: 'VGT_INDX_OFFSET',
    0x2103: 'VGT_MULTI_PRIM_IB_RESET_INDX', 0x2104: 'RB_COLOR_MASK', 0x2105: 'RB_BLEND_RED', 0x2106: 'RB_BLEND_GREEN',
    0x2107: 'RB_BLEND_BLUE', 0x2108: 'RB_BLEND_ALPHA', 0x2109: 'RB_FOG_COLOR_RED', 0x210A: 'RB_FOG_COLOR_GREEN',
    0x210B: 'RB_FOG_COLOR_BLUE', 0x210C: 'RB_STENCILREFMASK_BF', 0x210D: 'RB_STENCILREFMASK', 0x210E: 'RB_ALPHA_REF',
    0x210F: 'PA_CL_VPORT_XSCALE', 0x2110: 'PA_CL_VPORT_XOFFSET', 0x2111: 'PA_CL_VPORT_YSCALE',
    0x2112: 'PA_CL_VPORT_YOFFSET', 0x2113: 'PA_CL_VPORT_ZSCALE', 0x2114: 'PA_CL_VPORT_ZOFFSET',
    0x2180: 'SQ_PROGRAM_CNTL', 0x2181: 'SQ_CONTEXT_MISC', 0x2182: 'SQ_INTERPOLATOR_CNTL', 0x2183: 'SQ_WRAPPING_0',
    0x2184: 'SQ_WRAPPING_1',
    0x2200: 'RB_DEPTHCONTROL', 0x2201: 'RB_BLENDCONTROL0', 0x2202: 'RB_COLORCONTROL', 0x2203: 'RB_HIZCONTROL',
    0x2204: 'PA_CL_CLIP_CNTL', 0x2205: 'PA_SU_SC_MODE_CNTL', 0x2206: 'PA_CL_VTE_CNTL', 0x2207: 'VGT_CURRENT_BIN_ID_MIN',
    0x2208: 'RB_MODECONTROL', 0x2209: 'RB_BLENDCONTROL1', 0x220A: 'RB_BLENDCONTROL2', 0x220B: 'RB_BLENDCONTROL3',
    0x2280: 'PA_SU_POINT_SIZE', 0x2281: 'PA_SU_POINT_MINMAX', 0x2282: 'PA_SU_LINE_CNTL', 0x2283: 'PA_SC_LINE_STIPPLE',
    0x2284: 'VGT_OUTPUT_PATH_CNTL', 0x2285: 'VGT_HOS_CNTL', 0x2286: 'VGT_HOS_MAX_TESS_LEVEL',
    0x2287: 'VGT_HOS_MIN_TESS_LEVEL', 0x2288: 'VGT_HOS_REUSE_DEPTH', 0x2289: 'VGT_GROUP_PRIM_TYPE',
    0x228A: 'VGT_GROUP_FIRST_DECR', 0x228B: 'VGT_GROUP_DECR', 0x228C: 'VGT_GROUP_VECT_0_CNTL',
    0x228D: 'VGT_GROUP_VECT_1_CNTL', 0x228E: 'VGT_GROUP_VECT_0_FMT_CNTL', 0x228F: 'VGT_GROUP_VECT_1_FMT_CNTL',
    0x2290: 'UNKNOWN_2290', 0x2291: 'UNKNOWN_2291', 0x2292: 'PA_SC_MPASS_PS_CNTL', 0x2293: 'PA_SC_VIZ_QUERY',
    0x2294: 'VGT_ENHANCE',
    0x2300: 'PA_SC_LINE_CNTL', 0x2301: 'PA_SC_AA_CONFIG', 0x2302: 'PA_SU_VTX_CNTL', 0x2303: 'PA_CL_GB_VERT_CLIP_ADJ',
    0x2304: 'PA_CL_GB_VERT_DISC_ADJ', 0x2305: 'PA_CL_GB_HORZ_CLIP_ADJ', 0x2306: 'PA_CL_GB_HORZ_DISC_ADJ',
    0x2307: 'SQ_VS_CONST', 0x2308: 'SQ_PS_CONST', 0x2309: 'SQ_DEBUG_MISC_0', 0x230A: 'SQ_DEBUG_MISC_1',
    0x2312: 'PA_SC_AA_MASK', 0x2315: 'SQ_CF_PROGRAM_SIZE', 0x2316: 'VGT_VERTEX_REUSE_BLOCK_CNTL',
    0x2317: 'VGT_OUT_DEALLOC_CNTL', 0x2318: 'RB_COPY_CONTROL', 0x2319: 'RB_COPY_DEST_BASE', 0x231A: 'RB_COPY_DEST_PITCH',
    0x231B: 'RB_COPY_DEST_INFO', 0x231C: 'RB_HIZ_CLEAR', 0x231D: 'RB_DEPTH_CLEAR', 0x231E: 'RB_COLOR_CLEAR',
    0x231F: 'RB_COLOR_CLEAR_LO', 0x2320: 'RB_COPY_FUNC', 0x2321: 'RB_COPY_REF', 0x2322: 'RB_COPY_MASK',
    0x2323: 'RB_COPY_SURFACE_SLICE', 0x2324: 'RB_SAMPLE_COUNT_CTL', 0x2325: 'RB_SAMPLE_COUNT_ADDR',
    0x2380: 'PA_SU_POLY_OFFSET_FRONT_SCALE', 0x2381: 'PA_SU_POLY_OFFSET_FRONT_OFFSET',
    0x2382: 'PA_SU_POLY_OFFSET_BACK_SCALE', 0x2383: 'PA_SU_POLY_OFFSET_BACK_OFFSET', 0x2384: 'PA_CL_POINT_X_RAD',
    0x2385: 'PA_CL_POINT_Y_RAD', 0x2386: 'PA_CL_POINT_SIZE', 0x2387: 'PA_CL_POINT_CULL_RAD',
}

# Register groups: (first register, count, device offset, pending qword offset, LSB bit of the first register).
# Register first+k is at offset + 4k and its pending bit is first_bit - k.
REGISTER_GROUPS = [
    (0x2000, 16, 0x2800, 0x18, 38),
    (0x2100, 21, 0x284C, 0x18, 22),
    (0x2180, 5, 0x28A0, 0x10, 48),
    (0x2200, 12, 0x28B4, 0x10, 11),
    (0x2280, 21, 0x28E4, 0x18, 59),
    (0x2300, 38, 0x2938, 0x20, 37),
    (0x2380, 8, 0x29D0, 0x20, 45),
]

# Constant blocks: (what, GPU register base, registers per item, items, device offset, bytes per item,
#                   pending qword, LSB bit of item 0 (item k -> bit - k), items per bit)
CONSTANT_BLOCKS = [
    ('vertex shader float constant', 0x4000, 4, 256, 0x0700, 16, 0x00, 63, 4),
    ('pixel shader float constant', 0x4400, 4, 256, 0x1700, 16, 0x08, 63, 4),
    ('fetch constant (texture or vertex)', 0x4800, 6, 32, 0x0400, 24, 0x10, 43, 1),
    ('user clip plane', 0x2388, 4, 6, 0x27A0, 16, 0x20, 54, 1),
]
BOOL_LOOP = ('bool and loop constants', 0x4900, 40, 0x2700, 0x18, 1)   # one bit for all 40 registers
SHADER_BITS = (0x10, (49, 52))   # pending_10 bits 49-52: shaders, sent by 0x822FB4B0 (IM_LOAD, SQ_*)

# Device fields outside the GPU shadow (API-level state), as far as they are known.
OTHER_FIELDS = [
    (0x0000, 8, 'pending: vertex shader constants (bit 63-k = constants 4k..4k+3)'),
    (0x0008, 8, 'pending: pixel shader constants'),
    (0x0010, 8, 'pending: registers 0x2200-0x220B, fetch constants, 0x2180-0x2184, shaders'),
    (0x0018, 8, 'pending: bool/loop constants, registers 0x2100-0x2114, 0x2000-0x200F, 0x2280-0x2294'),
    (0x0020, 8, 'pending: registers 0x2300-0x2325, 0x2380-0x2387, clip planes'),
    (0x0028, 4, 'command buffer write pointer (points at the last word written)'),
    (0x002C, 4, 'command buffer reserve end'),
    (0x0030, 4, 'command buffer kick threshold (0x822F29B8 runs when the write pointer passes it)'),
    (0x0034, 4, 'device reference count'),
    (0x0038, 0x184, 'SetRenderState function table (index = state / 4; filled from 0x825C4008 by 0x822FDC58)'),
    (0x01BC, 0x50, 'SetSamplerState function table (from 0x825C4498)'),
    (0x020C, 0x184, 'GetRenderState function table'),
    (0x0390, 0x50, 'GetSamplerState function table'),
    (0x2A08, 4, 'owner thread (AcquireThreadOwnership)'),
    (0x2A10, 4, 'pointer to the GPU write-back block: [0] completed fence, [1] completed segment, [0xF] ring read pointer'),
    (0x2A1C, 4, 'next fence value (+2 per command segment)'),
    (0x2A2C, 4, 'last fence returned by InsertFence'),
    (0x2D10, 4, 'vertex declaration'),
    (0x2D38, 4, 'blend state as set (SrcBlend bits 0-4, BlendOp 5-7, DestBlend 8-12, alpha in the high half)'),
    (0x2D3C, 4, 'bit 31 AlphaBlendEnable, bit 30 SeparateAlphaBlendEnable'),
    (0x2D40, 4, 'ScissorTestEnable'),
    (0x2D44, 4, 'ColorWriteEnable as set'),
    (0x2D54, 4, 'ZEnable as set'),
    (0x2D58, 4, 'StencilEnable as set'),
    (0x2F84, 4, 'index buffer'),
    (0x2F88, 16, 'render targets 0-3'),
    (0x2F98, 4, 'depth/stencil surface'),
    (0x2F9C, 0x40, 'stream sources'),
    (0x3058, 0x18, 'viewport (D3DVIEWPORT9)'),
    (0x3070, 0x10, 'scissor rectangle'),
    (0x3080, 4, 'pixel shader'),
    (0x3084, 4, 'vertex shader'),
    (0x3354, 4, 'command buffer being recorded'),
    (0x3370, 0x34, 'display mode: width +0x3370, height +0x3374, refresh rate +0x3378, format +0x33A0'),
    (0x3510, 0x600, 'gamma ramp (D3DGAMMARAMP)'),
    (0x3B10, 4, 'vertical blank callback'),
    (0x4FC0, 4, 'creation flags (bits 24-29: hardware threads that get a Direct3D worker)'),
]


def register_name(reg):
    return REGISTER_NAMES.get(reg, f'reg_{reg:04X}')


def where(reg):
    """(device offset, pending qword offset, LSB bit, description) for a GPU register index, or None."""
    for first, n, off, pq, bit in REGISTER_GROUPS:
        if first <= reg < first + n:
            k = reg - first
            return off + 4 * k, pq, bit - k, register_name(reg)
    for what, base, per, items, off, size, pq, bit, per_bit in CONSTANT_BLOCKS:
        if base <= reg < base + per * items:
            item, d = divmod(reg - base, per)
            return off + item * size + 4 * d, pq, bit - item // per_bit, f'{what} {item} dword {d}'
    name, base, n, off, pq, bit = BOOL_LOOP
    if base <= reg < base + n:
        return off + 4 * (reg - base), pq, bit, f'{name} (register {reg:04X})'
    return None


def table():
    rows = []
    for first, n, off, pq, bit in REGISTER_GROUPS:
        for k in range(n):
            rows.append((off + 4 * k, f'{first + k:04X}', register_name(first + k), f'+{pq:02X} bit {bit - k}'))
    for what, base, per, items, off, size, pq, bit, per_bit in CONSTANT_BLOCKS:
        rows.append((off, f'{base:04X}-{base + per * items - 1:04X}', f'{items} x {what} ({size} bytes each)',
                     f'+{pq:02X} bits {bit}..{bit - (items - 1) // per_bit} ({per_bit} per bit)'))
    name, base, n, off, pq, bit = BOOL_LOOP
    rows.append((off, f'{base:04X}-{base + n - 1:04X}', name, f'+{pq:02X} bit {bit}'))
    rows.append((None, '', 'shaders (microcode, SQ_PROGRAM_CNTL, SQ_CONTEXT_MISC, SQ_INTERPOLATOR_CNTL)',
                 f'+{SHADER_BITS[0]:02X} bits {SHADER_BITS[1][0]}-{SHADER_BITS[1][1]}'))
    return sorted(rows, key=lambda r: (r[0] is None, r[0] or 0))


def decode(dump):
    """Register values (index -> u32) and pending masks from a big-endian device object dump."""
    if len(dump) < 0x3000:
        raise ValueError('dump too short for the device object')
    u32 = lambda o: struct.unpack_from('>I', dump, o)[0]
    regs = {}
    for first, n, off, pq, bit in REGISTER_GROUPS:
        for k in range(n):
            regs[first + k] = u32(off + 4 * k)
    for what, base, per, items, off, size, pq, bit, per_bit in CONSTANT_BLOCKS:
        for i in range(items):
            for d in range(per):
                regs[base + i * per + d] = u32(off + i * size + 4 * d)
    name, base, n, off, pq, bit = BOOL_LOOP
    for k in range(n):
        regs[base + k] = u32(off + 4 * k)
    pending = {o: struct.unpack_from('>Q', dump, o)[0] for o in (0, 8, 0x10, 0x18, 0x20)}
    return regs, pending


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--reg', help='GPU register index (hex)')
    ap.add_argument('--decode', metavar='DUMP', help='big-endian dump of the device object')
    a = ap.parse_args(argv)
    if a.reg:
        r = where(int(a.reg, 16))
        print('not in the shadow' if r is None else f'device+0x{r[0]:04X}, pending +0x{r[1]:02X} bit {r[2]} ({r[3]})')
        return 0
    if a.decode:
        regs, pending = decode(open(a.decode, 'rb').read())
        for o, v in pending.items():
            print(f'pending +0x{o:02X} = 0x{v:016X}')
        for reg, v in sorted(regs.items()):
            if reg < 0x4000:
                print(f'{reg:04X} {register_name(reg):32} 0x{v:08X}')
        return 0
    print('device offset  register(s)  what  pending bit')
    for off, reg, what, bit in table():
        print(f"{'' if off is None else f'+0x{off:04X}':>8}  {reg:>9}  {what}  {bit}")
    print('\nother device fields:')
    for off, size, what in OTHER_FIELDS:
        print(f'  +0x{off:04X} ({size:#x} bytes)  {what}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
