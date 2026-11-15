// NGL on Vulkan: [acct2-fh] 2026-11-14 the window's CAS (ngl_vk.h NGL_VK_SCALE_CAS): AMD FidelityFX Contrast Adaptive
// Sharpening, after the algorithm of AMD's ffx_cas.h (FidelityFX CAS, MIT licence, Copyright (c) 2020 Advanced Micro
// Devices, Inc.), written again here in plain GLSL. Drawn at the size the picture is shown at in the window: each
// pixel's 3 x 3 neighbours are read from the target filtered (bilinear), one shown pixel apart, so it is the scaled
// picture that is sharpened (CAS's own 4 x 4 resampling isn't used). How much each pixel is sharpened follows its
// neighbours' contrast: the closer they come to black or white, the less, so edges don't ring.
#version 450

layout(location = 0) in vec2 picture_uv;    // 0..1 across the picture's place in the window
layout(location = 0) out vec4 color;

layout(set = 0, binding = 0) uniform sampler2D scene;   // the target (bilinear, clamped to its edges)

layout(push_constant) uniform Settings {
    vec2 step;      // one shown pixel in the target's 0..1 coordinates: 1 / the shown width and height
    float peak;     // the cross's most negative weight: -1 / mix(8, 5, sharpness), sharpness 0..1
} settings;

void main()
{
    vec2 s = settings.step;
    //  a b c
    //  d e f
    //  g h i
    vec3 a = texture(scene, picture_uv + vec2(-s.x, -s.y)).rgb;
    vec3 b = texture(scene, picture_uv + vec2(0.0, -s.y)).rgb;
    vec3 c = texture(scene, picture_uv + vec2(s.x, -s.y)).rgb;
    vec3 d = texture(scene, picture_uv + vec2(-s.x, 0.0)).rgb;
    vec3 e = texture(scene, picture_uv).rgb;
    vec3 f = texture(scene, picture_uv + vec2(s.x, 0.0)).rgb;
    vec3 g = texture(scene, picture_uv + vec2(-s.x, s.y)).rgb;
    vec3 h = texture(scene, picture_uv + vec2(0.0, s.y)).rgb;
    vec3 i = texture(scene, picture_uv + vec2(s.x, s.y)).rgb;

    // A soft minimum and maximum (0..2): the cross's, plus all nine's.
    vec3 low = min(min(min(d, e), min(f, b)), h);
    vec3 high = max(max(max(d, e), max(f, b)), h);
    low += min(min(min(low, a), min(c, g)), i);
    high += max(max(max(high, a), max(c, g)), i);

    // How far the neighbourhood stays from black and from white, over its brightness, shaped by a square root: 0
    // (no sharpening) next to a limit, 1 in the middle of the range.
    vec3 amount = sqrt(clamp(min(low, 2.0 - high) / max(high, 1.0 / 65536.0), 0.0, 1.0));

    // The cross's four weights (negative), and the result normalized by the weights' sum.
    vec3 w = amount * settings.peak;
    vec3 sharpened = (b * w + d * w + f * w + h * w + e) / (1.0 + 4.0 * w);

    color = vec4(clamp(sharpened, 0.0, 1.0), 1.0);
}
