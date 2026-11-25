// [cod2-native] 2026-11-24 NGL on Vulkan: the IW engine's bloom, its first pass (ngl_vk_set_bloom). Call of Duty 2's
// glow_setup (its pixel shader 54872F98BF4CD183; the PC copy's glow_setup.hlsl): the picture shrunk into the glow
// target by four taps, each kept where its brightness dot(rgb, (0.299, 0.587, 0.114)) reaches the cutoff, as
// 1 + (rgb - 1) x rescale, times the share of it that is not sky (the game reads that from its picture's alpha; here
// the opaque depth says where nothing is drawn, as glow_first.frag); the four averaged, then drawn toward their own
// grey by the desaturation. Alpha: the sky's share.
#version 450

layout(location = 0) in vec2 picture_uv;
layout(location = 0) out vec4 color;

layout(set = 0, binding = 0) uniform sampler2D picture;         // the picture as it is now (the target's copy)
layout(set = 1, binding = 2) uniform sampler2D scene_depth;     // the shadow set's binding 2: the opaque depth

layout(push_constant) uniform Push {
    vec4 screen;        // the game's screen within the target: its corner and its size, in the target's 0..1
    vec4 setup;         // the game's glowSetup: x the cutoff, y the rescale (1 / (1 - cutoff)), w the desaturation;
                        // z (g4): 1 for the sky bleed's pass (the sky's color times its share), else 0
} push;

// 1 where nothing is drawn (ngl_vk's depth 1, the far end), 0 where something is
float sky(vec2 uv)
{
    ivec2 size = textureSize(scene_depth, 0);
    ivec2 at = clamp(ivec2((push.screen.xy + uv * push.screen.zw) * vec2(size)), ivec2(0), size - ivec2(1));

    return 1.0 - texelFetch(scene_depth, at, 0).r > 0.0 ? 0.0 : 1.0;
}

vec3 kept(vec2 uv)
{
    vec3 rgb = texture(picture, push.screen.xy + uv * push.screen.zw).rgb;

    if (dot(rgb, vec3(0.299, 0.587, 0.114)) < push.setup.x)
        return vec3(0.0);
    return clamp(1.0 + (rgb - 1.0) * push.setup.y, 0.0, 1.0);
}

void main()
{
    // the four taps a picture pixel either way from the small pixel's middle: with the picture's filtering they
    // cover the 4 x 4 picture pixels a quarter-size glow pixel stands for, as the game's do
    vec2 step = 1.0 / (vec2(textureSize(picture, 0)) * push.screen.zw);
    vec3 sum = vec3(0.0);
    float sky_share = 0.0;

    for (int i = 0; i < 4; i++) {
        vec2 at = picture_uv + step * vec2((i & 1) != 0 ? 1.0 : -1.0, (i & 2) != 0 ? 1.0 : -1.0);
        float s = sky(at);

        // [cod2-native] g4: setup.z 1, for the sky bleed: the sky's own color times its share, not the bright part
        if (push.setup.z > 0.5)
            sum += texture(picture, push.screen.xy + at * push.screen.zw).rgb * s;
        else
            sum += kept(at) * (1.0 - s);
        sky_share += s;
    }
    sum *= 0.25;
    if (push.setup.z > 0.5)
        color = vec4(sum, 0.25 * sky_share);
    else
        color = vec4(mix(sum, vec3(dot(sum, vec3(0.299, 0.587, 0.114))), push.setup.w), 0.25 * sky_share);
}
