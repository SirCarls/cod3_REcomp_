// [rexglue-native] 2026-11-10 NGL on Vulkan: the sky glow added over the picture (ngl_vk_set_glow; glow_first.frag has
// the whole of it). Call of Duty 3's pixel shader B8709E700697B204 with blend 010E010E: the blurred sky times a
// strength, added to what is there (the pipeline adds; the target's alpha is kept).
#version 450

layout(location = 0) in vec2 picture_uv;
layout(location = 0) out vec4 color;

layout(set = 0, binding = 0) uniform sampler2D glow;            // the last blur's

layout(push_constant) uniform Push {
    vec4 strength;      // x: the strength; the rest unused
} push;

void main()
{
    color = vec4(texture(glow, picture_uv).rgb * push.strength.x, 0.0);
}
