// [cod2-native] 2026-11-24 g4 NGL on Vulkan: the IW engine's sky bleed (ngl_vk_set_bloom's sky_bleed). Call of Duty
// 2's glow_apply_sky_bleed (the PC copy's glow_apply_sky_bleed.hlsl): where the blurred sky share (the glow's alpha)
// reaches, the screen drawn toward the sky's color by saturate(2 x share) x the strength. The game reads the color
// from its sky cube along the view; here it is the picture's own sky blurred by the same passes (bloom_first.frag
// with setup.z 1 keeps color x share), divided back by the share. Drawn with the source's alpha over the screen.
#version 450

layout(location = 0) in vec2 picture_uv;
layout(location = 0) out vec4 color;

layout(set = 0, binding = 0) uniform sampler2D glow;            // the last blur's: rgb the sky color x share, a share

layout(push_constant) uniform Push {
    vec4 bleed;         // x: the strength (the game's glowApply sky bleed intensity); the rest unused
} push;

void main()
{
    vec4 blurred = texture(glow, picture_uv);
    float share = blurred.a;

    color = vec4(share > 1.0 / 255.0 ? blurred.rgb / share : vec3(0.0),
                 share > 1.0 / 255.0 ? clamp(2.0 * share, 0.0, 1.0) * push.bleed.x : 0.0);
}
