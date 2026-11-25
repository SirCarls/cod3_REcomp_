#version 450
// [waw-decomp] Finest Hour port: the Xbox's fixed-function pixel stage, texture stage 0 (src/fh_draw.c).
//
// Constants (binding 1, ngl_vk_low's pixel constants), set per draw from the game's texture stage and render state
// arrays:
//   c0: stage 0's COLOROP, COLORARG1, COLORARG2, ALPHAOP
//   c1: stage 0's ALPHAARG1, ALPHAARG2, whether a texture is bound (1 or 0), [fh5] ALPHAKILL (texels of alpha 0
//       are dropped)
//   c2: the alpha test: on (1 or 0), its compare function (Vulkan's order: never, less, equal, less or equal,
//       greater, not equal, greater or equal, always), its reference (0..1), [fh11] above 0: the output alpha raised to it (the pain
//       effect's read-back picture, its persistence per frame scaled to the frame's length; src/fh_draw.c,
//       screen_blend_exponent)
//   c3: the texture factor color (D3DRS_TEXTUREFACTOR, state 148), red, green, blue, alpha
//   [acct2-fh] 2026-11-14, the fog (src/fh_draw.c, the top of the file, says how the game's Direct3D sets it up):
//   c4: on (1 or 0), the table mode (0 none: the input is the factor; 1 exp, 2 exp2, 3 linear: the input is the
//       distance), the start, the end
//   c5: the density; [fh5] stage 0's COLORARG0 and ALPHAARG0 (the third argument of MULTIPLYADD and LERP); [fh6] 1 to
//       divide the coordinates by q (a vertex program's oT0.w: the Xbox's 2D texture reads are projective), else 0
//   c6: the fog color, red, green, blue, unused
//   [fh4] 2026-11-24 c7: stage 0's coordinates scaled by (x, y) and moved by (z, w): a linear texture's are in its
//       texels on the Xbox, the back buffer's in the game's screen pixels (src/fh_draw.c, stage0_uv); (1, 1, 0, 0)
//       for the others
// The fog input (location 2) is the vertex's fog distance, or its factor without a table, interpolated.
// The operations are numbered as the Xbox's D3D8 numbers them, which differs from the PC's Direct3D 8 from 13 on
// ([fh5] 2026-11-24: the PC has 13 BLENDTEXTUREALPHA, 14 BLENDFACTORALPHA, 16 BLENDCURRENTALPHA; the Xbox moved
// BLENDCURRENTALPHA up to 13 and dropped nothing, so everything from 13 to 16 sits one place apart, and the PC's
// DOTPRODUCT3 24, MULTIPLYADD 25, LERP 26 are the Xbox's 22, 23, 24). This shader read 13 as BLENDTEXTUREALPHA
// before. The Xbox's: 1 DISABLE, 2 SELECTARG1, 3 SELECTARG2, 4 MODULATE, 5 MODULATE2X, 6 MODULATE4X, 7 ADD,
// 8 ADDSIGNED, 9 ADDSIGNED2X, 10 SUBTRACT, 11 ADDSMOOTH, 12 BLENDDIFFUSEALPHA, 13 BLENDCURRENTALPHA,
// 14 BLENDTEXTUREALPHA, 15 BLENDFACTORALPHA, 16 BLENDTEXTUREALPHAPM, 17 PREMODULATE, 18-21 the MODULATE..._ADD...
// pairs, 22 DOTPRODUCT3, 23 MULTIPLYADD, 24 LERP (25-26, bump mapping, and 17 are drawn as MODULATE for now; [fh6] 18-21 are written). The
// arguments are the same on both: 0 DIFFUSE, 1 CURRENT, 2 TEXTURE, 3 TFACTOR, 4 SPECULAR, 5 TEMP, with 0x10
// COMPLEMENT and 0x20 ALPHAREPLICATE.
layout(location = 0) in vec4 in_diffuse;
layout(location = 1) in vec3 in_uv0;      // [fh6] u, v, q
layout(location = 2) in float in_fog;

layout(set = 0, binding = 1) uniform PixelConstants {
    vec4 c[256];
} ps;

layout(set = 0, binding = 8) uniform sampler2D texture0;

layout(location = 0) out vec4 out_color;

vec4 argument(int which, vec4 texel)
{
    vec4 value;
    int source = which & 0xF;

    if (source == 2)
        value = texel;
    else if (source == 3)
        value = ps.c[3];
    else if (source == 4)
        value = vec4(0.0);
    else
        value = in_diffuse;     // DIFFUSE, and CURRENT, which at stage 0 is the diffuse color
    if ((which & 0x10) != 0)
        value = vec4(1.0) - value;
    if ((which & 0x20) != 0)
        value = vec4(value.a);
    return value;
}

// a, b: the first and second arguments; z: the third (ARG0); current: the color so far, at stage 0 the diffuse color.
vec4 combine(int op, vec4 a, vec4 b, vec4 z, vec4 texel, bool alpha)
{
    vec4 current = in_diffuse;

    if (op == 1)  return current;
    if (op == 2)  return a;
    if (op == 3)  return b;
    if (op == 5)  return a * b * 2.0;
    if (op == 6)  return a * b * 4.0;
    if (op == 7)  return a + b;
    if (op == 8)  return a + b - 0.5;
    if (op == 9)  return (a + b - 0.5) * 2.0;
    if (op == 10) return a - b;
    if (op == 11) return a + b * (vec4(1.0) - a);
    if (op == 12) return mix(b, a, in_diffuse.a);
    if (op == 13) return mix(b, a, current.a);
    if (op == 14) return mix(b, a, texel.a);
    if (op == 15) return mix(b, a, ps.c[3].a);
    if (op == 16) return a + b * (1.0 - texel.a);
    // [fh6] 18-21, on the color only (an alpha operation of these keeps the first argument): the first argument's
    // alpha (after its modifiers) times or plus the second
    if (op == 18) return alpha ? a : vec4(a.rgb + b.rgb * a.a, a.a);              // MODULATEALPHA_ADDCOLOR
    if (op == 19) return alpha ? a : vec4(a.rgb * b.rgb + a.a, a.a);              // MODULATECOLOR_ADDALPHA
    if (op == 20) return alpha ? a : vec4(a.rgb + b.rgb * (1.0 - a.a), a.a);      // MODULATEINVALPHA_ADDCOLOR
    if (op == 21) return alpha ? a : vec4((1.0 - a.rgb) * b.rgb + a.a, a.a);      // MODULATEINVCOLOR_ADDALPHA
    if (op == 22) return vec4(clamp(dot(a.rgb * 2.0 - 1.0, b.rgb * 2.0 - 1.0), 0.0, 1.0));
    if (op == 23) return z + a * b;
    if (op == 24) return mix(b, a, z);
    return a * b;               // MODULATE, and the operations not written yet
}

bool alpha_passes(float alpha, int compare, float reference)
{
    if (compare == 0) return false;
    if (compare == 1) return alpha < reference;
    if (compare == 2) return alpha == reference;
    if (compare == 3) return alpha <= reference;
    if (compare == 4) return alpha > reference;
    if (compare == 5) return alpha != reference;
    if (compare == 6) return alpha >= reference;
    return true;
}

// The fog factor, 1 meaning no fog (Direct3D's definitions; the NV2A's bias and scale give the same numbers).
float fog_factor(float d)
{
    int mode = int(ps.c[4].y);
    float start = ps.c[4].z, end = ps.c[4].w, density = ps.c[5].x;

    if (mode == 3)
        return end != start ? (end - d) / (end - start) : 1.0;
    if (mode == 1)
        return exp(-density * d);
    if (mode == 2)
        return exp(-(density * d) * (density * d));
    return d;
}

void main()
{
    ivec4 color_stage = ivec4(ps.c[0]);
    ivec4 alpha_stage = ivec4(ps.c[1]);
    vec2 uv0 = ps.c[5].w > 0.5 && in_uv0.z != 0.0 ? in_uv0.xy / in_uv0.z : in_uv0.xy;
    // [fh6] c1.z: 1 a texture, 0 none (white), 2 one set that could not be read here (0)
    vec4 texel = ps.c[1].z > 1.5 ? vec4(0.0)
               : ps.c[1].z > 0.5 ? texture(texture0, uv0 * ps.c[7].xy + ps.c[7].zw) : vec4(1.0);
    if (ps.c[1].w > 0.5 && texel.a == 0.0)
        discard;
    int color_arg0 = int(ps.c[5].y), alpha_arg0 = int(ps.c[5].z);
    vec4 color = combine(color_stage.x, argument(color_stage.y, texel), argument(color_stage.z, texel),
                         argument(color_arg0, texel), texel, false);
    vec4 alpha = combine(color_stage.w, argument(alpha_stage.x, texel), argument(alpha_stage.y, texel),
                         argument(alpha_arg0, texel), texel, true);
    vec4 result = clamp(vec4(color.rgb, alpha.a), 0.0, 1.0);

    if (ps.c[4].x > 0.5)
        result.rgb = mix(ps.c[6].rgb, result.rgb, clamp(fog_factor(in_fog), 0.0, 1.0));

    if (ps.c[2].x > 0.5 && !alpha_passes(result.a, int(ps.c[2].y), ps.c[2].z))
        discard;
    if (ps.c[2].w > 0.0)
        result.a = pow(clamp(result.a, 0.0, 1.0), ps.c[2].w);
    out_color = result;
}
