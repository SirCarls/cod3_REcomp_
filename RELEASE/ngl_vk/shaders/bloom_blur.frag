// [cod2-native] 2026-11-24 NGL on Vulkan: one of the IW engine's bloom blurs (ngl_vk_set_bloom). Call of Duty 2's
// filter_symmetric_N (its pixel shader AAAB69EB73A965B2 for N = 7; the PC copy's lib/filter_symmetric_small/_large):
// up to eight tap pairs, each at plus and minus its place, times its weight. The game draws them down, then across,
// with the places and weights in its pixel constants c8 on; the native side hands them over as they were.
#version 450

layout(location = 0) in vec2 picture_uv;
layout(location = 0) out vec4 color;

layout(set = 0, binding = 0) uniform sampler2D glow;            // the pass before this one

layout(push_constant) uniform Push {
    vec4 taps[8];       // each: xy its place in the glow's 0..1, w its weight; weight 0: no tap
} push;

void main()
{
    vec4 sum = vec4(0.0);

    for (int i = 0; i < 8; i++) {
        if (push.taps[i].w == 0.0)
            continue;
        sum += push.taps[i].w * (texture(glow, picture_uv + push.taps[i].xy) +
                                 texture(glow, picture_uv - push.taps[i].xy));
    }
    color = sum;
}
