#version 450
// [acct2-fh] 2026-11-13: Finest Hour port: the game's NV2A vertex programs, run on the GPU (src/fh_draw.c, program
// draws; FH_VSH_GPU=0 runs them on the CPU instead, src/fh_vsh.c). One shader serves every program: it reads the
// program's instructions from the pixel constants and runs them for each vertex, the way fh_vsh_run does on the CPU.
// fh_vsh.c describes the instruction layout and the operations; this file follows fh_vsh_run step by step, so a
// change to one belongs in the other too.
//
// Inputs: up to eight of the program's v registers, at locations 0-7, each in the format its declaration gives
// (fh_draw.c, gpu_program_layout). Which register a location feeds is in the bool/loop block's words 0-7 (16: none).
// A register the declaration does not name reads (0, 0, 0, 0), as on the CPU.
// Vertex constants (binding 0): the NV2A's 192 constant registers (Direct3D's c-96..c95, c0 being register 96), with
// c-38 = 1 and c-37 = 0, so that a program's own screen mapping leaves oPos in clip space (fh_vsh_draw_begin says why).
// Pixel constants (binding 1): registers 0-3 are fixed.frag's; from register 16 on, one 128-bit instruction a register
// (its four words as the bits of four floats).
// Bool/loop block (binding 2): words 0-7 the registers of locations 0-7, word 8 the instruction count, word 9 bit 0:
// give the position its w (FH_VSH_W).
// The outputs are fixed.vert's: the position in Vulkan's clip space, oD0 as the diffuse color, oT0's first two, and
// [acct2-fh] 11-14 oFog.x as the fog distance (0 when the program doesn't write it, as on the CPU).

layout(location = 0) in vec4 in_v0;
layout(location = 1) in vec4 in_v1;
layout(location = 2) in vec4 in_v2;
layout(location = 3) in vec4 in_v3;
layout(location = 4) in vec4 in_v4;
layout(location = 5) in vec4 in_v5;
layout(location = 6) in vec4 in_v6;
layout(location = 7) in vec4 in_v7;

layout(set = 0, binding = 0) uniform VertexConstants {
    vec4 c[256];
} vs;

layout(set = 0, binding = 1) uniform PixelConstants {
    vec4 c[256];
} ps;

layout(set = 0, binding = 2) uniform BoolLoop {
    uvec4 w[10];
} bl;

layout(location = 0) out vec4 out_diffuse;
layout(location = 1) out vec3 out_uv0;     // [fh6] oT0's x, y and w (q): fixed.frag divides by q
layout(location = 2) out float out_fog;

const int CONSTANTS = 192;          // the NV2A's constant registers
const int FIRST_INSTRUCTION = 16;   // the pixel constant holding instruction 0
const uint MOST_INSTRUCTIONS = 136u;
const uint OUTPUT_POSITION = 0u, OUTPUT_DIFFUSE = 3u, OUTPUT_FOG = 5u, OUTPUT_TEXCOORD0 = 9u, OUTPUT_REGISTERS = 13u;
const uint TEMPS = 13u;             // r0-r11, and r12 (oPos as read)

vec4 temp[13];
vec4 outputs[13];
vec4 inputs[16];

uint field(uint word, int first, int count)
{
    return bitfieldExtract(word, first, count);
}

void set_input(uint reg, vec4 value)
{
    if (reg < 16u)
        inputs[reg] = value;
}

// An operand: source 1 a temp (r12 reads oPos; 13-15 read r0), 2 the input, 3 the constant (out of range: r0),
// 0 unused (r0).
vec4 operand_source(uint source, uint t, uint input_register, int index)
{
    if (source == 1u) {
        if (t == 12u)
            return outputs[OUTPUT_POSITION];
        return t < 12u ? temp[t] : temp[0];
    }
    if (source == 2u)
        return inputs[input_register];
    if (source == 3u)
        return index >= 0 && index < CONSTANTS ? vs.c[index] : temp[0];
    return temp[0];
}

// The swizzle's four 2-bit picks, x first (bits 7-6), then the negation.
vec4 swizzled(vec4 src, uint pick, uint negate)
{
    vec4 r = vec4(src[(pick >> 6) & 3u], src[(pick >> 4) & 3u], src[(pick >> 2) & 3u], src[pick & 3u]);

    return negate != 0u ? -r : r;
}

// Mask bits x y z w from bit 3 down.
vec4 masked(vec4 dst, vec4 value, uint mask)
{
    if ((mask & 8u) != 0u) dst.x = value.x;
    if ((mask & 4u) != 0u) dst.y = value.y;
    if ((mask & 2u) != 0u) dst.z = value.z;
    if ((mask & 1u) != 0u) dst.w = value.w;
    return dst;
}

float clamped_reciprocal(float x)
{
    float r = 1.0 / x;

    if (r > 0.0)
        return r < 5.42101e-20 ? 5.42101e-20 : (r > 1.884467e+19 ? 1.884467e+19 : r);
    return r > -5.42101e-20 ? -5.42101e-20 : (r < -1.884467e+19 ? -1.884467e+19 : r);
}

void main()
{
    float minus_infinity = uintBitsToFloat(0xFF800000u);
    int a0 = 0;
    uint count = min(bl.w[2].x, MOST_INSTRUCTIONS);

    for (int i = 0; i < 16; i++)
        inputs[i] = vec4(0.0);
    set_input(bl.w[0].x, in_v0);
    set_input(bl.w[0].y, in_v1);
    set_input(bl.w[0].z, in_v2);
    set_input(bl.w[0].w, in_v3);
    set_input(bl.w[1].x, in_v4);
    set_input(bl.w[1].y, in_v5);
    set_input(bl.w[1].z, in_v6);
    set_input(bl.w[1].w, in_v7);
    for (int i = 0; i < 13; i++) {
        temp[i] = vec4(0.0);
        outputs[i] = vec4(0.0);
    }
    outputs[OUTPUT_DIFFUSE] = vec4(1.0);
    outputs[OUTPUT_POSITION].w = 1.0;
    outputs[OUTPUT_TEXCOORD0].w = 1.0;

    for (uint i = 0u; i < count; i++) {
        // words 1-3 of the instruction (word 0 holds nothing the programs use)
        uvec4 w = floatBitsToUint(ps.c[FIRST_INSTRUCTION + int(i)]);
        uint mac = field(w.y, 21, 4), ilu = field(w.y, 25, 3), input_register = field(w.y, 9, 4);
        int index = int(field(w.y, 13, 8)) + (field(w.w, 1, 1) != 0u ? a0 : 0);
        uint temp_out = field(w.w, 20, 4);

        // the three operands, read before anything is written
        vec4 a = swizzled(operand_source(field(w.z, 26, 2), field(w.z, 28, 4), input_register, index),
                          field(w.y, 0, 8), field(w.y, 8, 1));
        vec4 b = swizzled(operand_source(field(w.z, 11, 2), field(w.z, 13, 4), input_register, index),
                          field(w.z, 17, 8), field(w.z, 25, 1));
        vec4 c = swizzled(operand_source(field(w.w, 28, 2), (field(w.z, 0, 2) << 2) | field(w.w, 30, 2), input_register, index),
                          field(w.z, 2, 8), field(w.z, 10, 1));
        vec4 mac_value = vec4(0.0), ilu_value = vec4(0.0);

        // the MAC (fh_draw.c sends no program with an operation 14 or 15)
        if (mac == 1u) {
            mac_value = a;
        } else if (mac == 2u) {
            mac_value = a * b;
        } else if (mac == 3u) {
            mac_value = a + c;
        } else if (mac == 4u) {
            mac_value = a * b + c;
        } else if (mac == 5u) {
            mac_value = vec4(a.x * b.x + a.y * b.y + a.z * b.z);
        } else if (mac == 6u) {
            mac_value = vec4(a.x * b.x + a.y * b.y + a.z * b.z + b.w);
        } else if (mac == 7u) {
            mac_value = vec4(a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w);
        } else if (mac == 8u) {
            mac_value = vec4(1.0, a.y * b.y, a.z, b.w);
        } else if (mac == 9u) {
            mac_value = vec4(a.x < b.x ? a.x : b.x, a.y < b.y ? a.y : b.y, a.z < b.z ? a.z : b.z, a.w < b.w ? a.w : b.w);
        } else if (mac == 10u) {
            mac_value = vec4(a.x > b.x ? a.x : b.x, a.y > b.y ? a.y : b.y, a.z > b.z ? a.z : b.z, a.w > b.w ? a.w : b.w);
        } else if (mac == 11u) {
            mac_value = vec4(a.x < b.x ? 1.0 : 0.0, a.y < b.y ? 1.0 : 0.0, a.z < b.z ? 1.0 : 0.0, a.w < b.w ? 1.0 : 0.0);
        } else if (mac == 12u) {
            mac_value = vec4(a.x >= b.x ? 1.0 : 0.0, a.y >= b.y ? 1.0 : 0.0, a.z >= b.z ? 1.0 : 0.0,
                             a.w >= b.w ? 1.0 : 0.0);
        } else if (mac == 13u) {
            a0 = int(floor(a.x + 1e-3));    // [fh10] a little over, as an index made by multiplying (3 x 0.99999) means 3
        }

        // the ILU, on operand C
        if (ilu == 1u) {
            ilu_value = c;
        } else if (ilu == 2u) {
            ilu_value = vec4(1.0 / c.x);
        } else if (ilu == 3u) {
            ilu_value = vec4(clamped_reciprocal(c.x));
        } else if (ilu == 4u) {
            ilu_value = vec4(1.0 / sqrt(abs(c.x)));
        } else if (ilu == 5u) {
            float f = floor(c.x);

            ilu_value = vec4(exp2(f), c.x - f, exp2(c.x), 1.0);
        } else if (ilu == 6u) {
            float m = abs(c.x), e = m > 0.0 ? floor(log2(m)) : minus_infinity;

            ilu_value = vec4(e, m > 0.0 ? m / exp2(e) : 1.0, m > 0.0 ? log2(m) : minus_infinity, 1.0);
        } else if (ilu == 7u) {     // lit
            float diffuse = c.x > 0.0 ? c.x : 0.0;
            float power = c.w < -127.9961 ? -127.9961 : (c.w > 127.9961 ? 127.9961 : c.w);

            ilu_value = vec4(1.0, diffuse, c.x > 0.0 && c.y > 0.0 ? pow(c.y, power) : 0.0, 1.0);
        }

        // the writes: temps (the ILU's to r1 when the MAC writes a temp too), then the o register
        uint mac_temp = temp_out % TEMPS, ilu_temp = mac != 0u ? 1u : temp_out % TEMPS;
        uint out_mask = field(w.w, 12, 4), out_index = field(w.w, 3, 8);

        if (mac != 0u && mac != 13u)
            temp[mac_temp] = masked(temp[mac_temp], mac_value, field(w.w, 24, 4));
        if (ilu != 0u)
            temp[ilu_temp] = masked(temp[ilu_temp], ilu_value, field(w.w, 16, 4));
        if (out_mask != 0u && field(w.w, 11, 1) != 0u && out_index < OUTPUT_REGISTERS)
            outputs[out_index] = masked(outputs[out_index], field(w.w, 2, 1) != 0u ? ilu_value : mac_value, out_mask);
        // a write to temp 12 is a write to oPos
        if (mac != 0u && mac != 13u && temp_out == 12u)
            outputs[OUTPUT_POSITION] = masked(outputs[OUTPUT_POSITION], mac_value, field(w.w, 24, 4));
        if (field(w.w, 0, 1) != 0u)
            break;
    }

    // oPos is x/w, y/w, z/w and w (fh_draw.c, program_vertex): back to clip space, as fixed.vert's
    vec4 p = outputs[OUTPUT_POSITION];
    float clip_w = (bl.w[2].y & 1u) != 0u ? p.w : 1.0;
    vec4 clip = vec4(p.xyz * clip_w, clip_w);

    clip.y = -clip.y;   // Direct3D's clip space has y up; Vulkan's framebuffer has y down
    gl_Position = clip;
    out_diffuse = clamp(outputs[OUTPUT_DIFFUSE], 0.0, 1.0);
    out_uv0 = outputs[OUTPUT_TEXCOORD0].xyw;
    out_fog = outputs[OUTPUT_FOG].x;
}
