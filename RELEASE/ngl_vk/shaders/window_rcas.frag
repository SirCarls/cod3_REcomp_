// NGL on Vulkan: [acct2-fh] 2026-11-14 the window's FSR 1 sharpening pass (ngl_vk.h NGL_VK_SCALE_FSR): AMD FidelityFX
// Super Resolution 1's RCAS ("robust contrast adaptive sharpening"), after the algorithm of AMD's ffx_fsr1.h (FidelityFX
// FSR 1, MIT licence, Copyright (c) 2021 Advanced Micro Devices, Inc.), written again here in plain GLSL. It runs on
// the scaled picture, at its own size, with the cross of 5 pixels:
//        b
//      d e f
//        h
// The sharpening lobe is as large as it can be without pushing the pixel past the cross's darkest or brightest value
// (so it never clips), at most 1/4 - 1/16, times the sharpness: 2 to the minus the stops it is reduced by.
#version 450

layout(location = 0) out vec4 color;

layout(set = 0, binding = 0) uniform sampler2D source;  // the scaled picture (read pixel by pixel)

layout(push_constant) uniform Settings {
    vec2 size;          // the picture's width and height in pixels, from the image's corner (0, 0)
    float stops;        // how much less sharp, 0..2: 0 the sharpest, 2 a quarter as sharp
} settings;

#define RCAS_LIMIT (0.25 - 1.0 / 16.0)

vec3 read(ivec2 at)
{
    return texelFetch(source, clamp(at, ivec2(0), ivec2(settings.size) - 1), 0).rgb;
}

void main()
{
    ivec2 here = ivec2(gl_FragCoord.xy);
    vec3 b = read(here + ivec2(0, -1));
    vec3 d = read(here + ivec2(-1, 0));
    vec3 e = read(here);
    vec3 f = read(here + ivec2(1, 0));
    vec3 h = read(here + ivec2(0, 1));

    // the ring's smallest and largest, each channel
    vec3 low = min(min(b, d), min(f, h));
    vec3 high = max(max(b, d), max(f, h));

    // The lobe each channel allows: so that e + 4 x lobe x (ring - e) stays above 0 (hit_low) and below 1 (hit_high).
    vec3 hit_low = min(low, e) / max(4.0 * high, 1.0 / 65536.0);
    vec3 hit_high = (1.0 - max(high, e)) / min(4.0 * low - 4.0, -1.0 / 65536.0);
    vec3 lobes = max(-hit_low, hit_high);
    float lobe = max(-RCAS_LIMIT, min(max(max(lobes.r, lobes.g), lobes.b), 0.0)) * exp2(-settings.stops);

    // the ring weighted by the lobe (negative), the pixel by 1, normalized
    vec3 sharpened = (lobe * (b + d + f + h) + e) / (4.0 * lobe + 1.0);

    color = vec4(clamp(sharpened, 0.0, 1.0), 1.0);
}
