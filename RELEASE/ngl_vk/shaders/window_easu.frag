// NGL on Vulkan: [acct2-fh] 2026-11-14 the window's FSR 1 scaling pass (ngl_vk.h NGL_VK_SCALE_FSR): AMD FidelityFX
// Super Resolution 1's EASU ("edge adaptive spatial upsampling"), after the algorithm of AMD's ffx_fsr1.h (FidelityFX
// FSR 1, MIT licence, Copyright (c) 2021 Advanced Micro Devices, Inc.), written again here in plain GLSL with plain
// reads (no gathers, no approximate reciprocals). Each pixel of the bigger picture comes from the 12 source pixels
// around it:
//        b c
//      e f g h
//      i j k l
//        n o
// f is the source pixel up and left of the place, (pp) the place's fraction past f. The four nearest (f g j k) give a
// direction and how much of an edge there is, from their brightness; the 12 are then weighted by a Lanczos-2-like
// curve stretched along that edge, and the result is kept between the four nearest's smallest and largest values
// (no ringing). The pass may run more than once (ngl_vk_set_window_sharpness's FSR passes), each up to twice the size.
#version 450

layout(location = 0) out vec4 color;

layout(set = 0, binding = 0) uniform sampler2D source;  // the target, or the last pass's picture (read pixel by pixel)

layout(push_constant) uniform Sizes {
    vec2 from;      // the source's picture: its width and height in pixels, from the image's corner (0, 0)
    vec2 to;        // this pass's picture, drawn from the corner (0, 0) of the image drawn into
} sizes;

// A source pixel, its place kept inside the source's picture.
vec3 read(ivec2 at)
{
    return texelFetch(source, clamp(at, ivec2(0), ivec2(sizes.from) - 1), 0).rgb;
}

// Brightness times 2, as FSR has it: half red, all green, half blue.
float luma(vec3 c)
{
    return c.b * 0.5 + (c.r * 0.5 + c.g);
}

// One of the four nearest pixels' share of the direction and of the edge: w its bilinear weight; a above it, b left
// of it, c itself, d right of it, e below it.
void add_direction(inout vec2 dir, inout float len, float w, float a, float b, float c, float d, float e)
{
    // Across: the difference from left to right, and how much of an edge it is: 1 when both sides step the same way,
    // 0 when they step in turn (a thin line, not an edge), squared.
    float dc = d - c;
    float cb = c - b;
    float len_x = max(abs(dc), abs(cb));
    float dir_x = d - b;

    len_x = clamp(abs(dir_x) / max(len_x, 1.0 / 65536.0), 0.0, 1.0);
    dir.x += dir_x * w;
    len += len_x * len_x * w;

    // Down, the same.
    float ec = e - c;
    float ca = c - a;
    float len_y = max(abs(ec), abs(ca));
    float dir_y = e - a;

    len_y = clamp(abs(dir_y) / max(len_y, 1.0 / 65536.0), 0.0, 1.0);
    dir.y += dir_y * w;
    len += len_y * len_y * w;
}

// One of the 12 pixels into the weighted sum: off its place from the point drawn (in source pixels).
void add_tap(inout vec3 sum, inout float weight, vec2 off, vec2 dir, vec2 len2, float lob, float clip, vec3 c)
{
    // the offset turned into the edge's direction, then stretched (anisotropy)
    vec2 v = vec2(off.x * dir.x + off.y * dir.y, off.x * -dir.y + off.y * dir.x) * len2;
    // the distance squared, kept to the window's end (at a corner, two taps can lie outside it)
    float d2 = min(dot(v, v), clip);
    // Lanczos 2 without sin, sqrt or a division:
    //   (25/16 * (2/5 * x^2 - 1)^2 - (25/16 - 1)) * (lob * x^2 - 1)^2
    //    |_____________ base _______________|      |____ window ____|
    float base = 2.0 / 5.0 * d2 - 1.0;
    float window = lob * d2 - 1.0;
    float w;

    base *= base;
    window *= window;
    base = 25.0 / 16.0 * base - (25.0 / 16.0 - 1.0);
    w = base * window;
    sum += c * w;
    weight += w;
}

void main()
{
    // this pixel's center in the source's pixels, less half a pixel: f is the source pixel at its floor
    ivec2 here = ivec2(gl_FragCoord.xy);
    vec2 place = (vec2(here) + 0.5) * sizes.from / sizes.to - 0.5;
    vec2 fp = floor(place);
    vec2 pp = place - fp;
    ivec2 at = ivec2(fp);

    vec3 b = read(at + ivec2(0, -1)), c = read(at + ivec2(1, -1));
    vec3 e = read(at + ivec2(-1, 0)), f = read(at), g = read(at + ivec2(1, 0)), h = read(at + ivec2(2, 0));
    vec3 i = read(at + ivec2(-1, 1)), j = read(at + ivec2(0, 1)), k = read(at + ivec2(1, 1)), l = read(at + ivec2(2, 1));
    vec3 n = read(at + ivec2(0, 2)), o = read(at + ivec2(1, 2));

    float bL = luma(b), cL = luma(c), eL = luma(e), fL = luma(f), gL = luma(g), hL = luma(h);
    float iL = luma(i), jL = luma(j), kL = luma(k), lL = luma(l), nL = luma(n), oL = luma(o);

    // The direction and the amount of edge, bilinear over the four nearest:
    //   s t     f g
    //   u v     j k
    vec2 dir = vec2(0.0);
    float len = 0.0;

    add_direction(dir, len, (1.0 - pp.x) * (1.0 - pp.y), bL, eL, fL, gL, jL);
    add_direction(dir, len, pp.x * (1.0 - pp.y), cL, fL, gL, hL, kL);
    add_direction(dir, len, (1.0 - pp.x) * pp.y, fL, iL, jL, kL, nL);
    add_direction(dir, len, pp.x * pp.y, gL, jL, kL, lL, oL);

    // The direction made a unit (none, near zero: across)
    float dir_r = dot(dir, dir);

    if (dir_r < 1.0 / 32768.0) {
        dir = vec2(1.0, 0.0);
    } else {
        dir *= inversesqrt(dir_r);
    }
    // the edge from 0..2 to 0..1, shaped by a square
    len *= 0.5;
    len *= len;
    // the kernel stretched from 1 (across or down) to sqrt(2) (on a diagonal)
    float stretch = dot(dir, dir) / max(abs(dir.x), abs(dir.y));
    // the length after turning: x 1 to the stretch along an edge, y 1 to 0.5 (twice as wide) across it
    vec2 len2 = vec2(1.0 + (stretch - 1.0) * len, 1.0 - 0.5 * len);
    // the window's negative lobe moves from +-sqrt(2) to a little past 2 with the amount of edge
    float lob = 0.5 + ((1.0 / 4.0 - 0.04) - 0.5) * len;
    float clip = 1.0 / lob;

    // the four nearest's smallest and largest: the result is kept between them
    vec3 lowest = min(min(f, g), min(j, k));
    vec3 highest = max(max(f, g), max(j, k));

    vec3 sum = vec3(0.0);
    float weight = 0.0;

    add_tap(sum, weight, vec2(0.0, -1.0) - pp, dir, len2, lob, clip, b);
    add_tap(sum, weight, vec2(1.0, -1.0) - pp, dir, len2, lob, clip, c);
    add_tap(sum, weight, vec2(-1.0, 1.0) - pp, dir, len2, lob, clip, i);
    add_tap(sum, weight, vec2(0.0, 1.0) - pp, dir, len2, lob, clip, j);
    add_tap(sum, weight, vec2(0.0, 0.0) - pp, dir, len2, lob, clip, f);
    add_tap(sum, weight, vec2(-1.0, 0.0) - pp, dir, len2, lob, clip, e);
    add_tap(sum, weight, vec2(1.0, 1.0) - pp, dir, len2, lob, clip, k);
    add_tap(sum, weight, vec2(2.0, 1.0) - pp, dir, len2, lob, clip, l);
    add_tap(sum, weight, vec2(2.0, 0.0) - pp, dir, len2, lob, clip, h);
    add_tap(sum, weight, vec2(1.0, 0.0) - pp, dir, len2, lob, clip, g);
    add_tap(sum, weight, vec2(1.0, 2.0) - pp, dir, len2, lob, clip, o);
    add_tap(sum, weight, vec2(0.0, 2.0) - pp, dir, len2, lob, clip, n);

    color = vec4(min(highest, max(lowest, sum / weight)), 1.0);
}
