// [rexglue-native] 2026-11-10 NGL on Vulkan: the effect layer blended back over the picture (ngl_vk.h effect_layer),
// as Call of Duty 3's pixel shader 8E6F505DEE7CDF9E does it (read from its microcode; its numbers from the game's own
// draw: c0 = 0.003 0.06 -0.2 0.02, c1 = 1/1280 1/720 640 360, c255 = 1 -1 0.25 0.7; drawn with ONE, 1 - its alpha).
//
// The layer is half the picture's width and height; its pixel (x, y) was drawn for the picture's pixel (2x, 2y). For
// the picture's pixel p, with j = p / 2 and o = p's odd bits, the game fetches the layer four times, unfiltered:
// r1 at j, r2 at j + (o.x, 0), r3 at j + (0, o.y), r4 at j + o, and takes their mean. So an even pixel takes its own
// layer pixel and an odd one the mean of the two (or four) it lies between.
//
// Where something is there (the largest of max(red, alpha) over the four is above 0.2) and the picture's depth has an
// edge, it takes one fetch whole instead, so smoke doesn't bleed across a wall's edge: with m = max(red, alpha) of each
// fetch, `other` is the fetch whose m differs most from r1's (r2, r3 or r4, the later winning a tie) and `way` its
// side ((1, -1), (-1, 1) or (1, 1)); the picture's depth is read at p - o (a), at p + way x o (b) and at p; where a's
// differs from p's by more than 6% and by more than b's does, the pixel takes `other`; where b's differs by more than
// 6% and by more than a's, it takes r1. The depths compared are the game's, which run backward (1 near): 1 - ours.
#version 450

layout(location = 0) in vec2 picture_uv;
layout(location = 0) out vec4 color;

layout(set = 0, binding = 0) uniform sampler2D layer;           // the effect layer (premultiplied color, coverage)
layout(set = 1, binding = 2) uniform sampler2D scene_depth;     // the shadow set's binding 2: the opaque depth

const float SOMETHING_THERE = 0.2;      // -c0.z
const float DEPTH_EDGE = 0.06;          // c0.y

float game_depth(ivec2 at)
{
    ivec2 last = textureSize(scene_depth, 0) - ivec2(1);

    return 1.0 - texelFetch(scene_depth, clamp(at, ivec2(0), last), 0).r;
}

void main()
{
    ivec2 p = ivec2(gl_FragCoord.xy), j = p >> 1, o = p & 1;
    ivec2 last = textureSize(layer, 0) - ivec2(1);
    vec4 r1 = texelFetch(layer, min(j, last), 0);
    vec4 r2 = texelFetch(layer, min(j + ivec2(o.x, 0), last), 0);
    vec4 r3 = texelFetch(layer, min(j + ivec2(0, o.y), last), 0);
    vec4 r4 = texelFetch(layer, min(j + o, last), 0);
    vec4 mean = (r1 + r2 + r3 + r4) * 0.25;
    vec4 m = vec4(max(r1.r, r1.a), max(r2.r, r2.a), max(r3.r, r3.a), max(r4.r, r4.a));

    color = mean;
    if (max(max(m.x, m.y), max(m.z, m.w)) > SOMETHING_THERE && (o.x | o.y) != 0) {
        vec3 apart = abs(m.yzw - m.xxx);
        float most = max(apart.x, max(apart.y, apart.z));
        vec4 other = r2;
        ivec2 way = ivec2(1, -1);

        if (apart.y == most) {
            other = r3;
            way = ivec2(-1, 1);
        }
        if (apart.z == most) {
            other = r4;
            way = ivec2(1, 1);
        }
        float here = game_depth(p), a = game_depth(p - o), b = game_depth(p + way * o);
        float a_off, b_off;     // how far each is from the pixel's own, as a share of it

        if (here > 0.0) {
            a_off = abs(a / here - 1.0);
            b_off = abs(b / here - 1.0);
        } else {                // the far end (the game divides by 0 there: an endless share, or none)
            a_off = a > 0.0 ? 1e30 : 0.0;
            b_off = b > 0.0 ? 1e30 : 0.0;
        }
        if (a_off > DEPTH_EDGE && a_off > b_off)
            color = other;
        if (b_off > DEPTH_EDGE && b_off > a_off)
            color = r1;
    }
}
