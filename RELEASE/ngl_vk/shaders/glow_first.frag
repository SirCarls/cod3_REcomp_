// [rexglue-native] 2026-11-10 NGL on Vulkan: the sky glow's first pass (ngl_vk_set_glow). Call of Duty 3 lets its sky
// glow over what stands before it: each frame, after its tint, it draws the picture into a 256 x 256 target keeping
// only the sky (its pixel shader C8894F59B8CA31E6): the picture's color at the small pixel's middle, times the share of
// four depth fetches (at that place and half a small pixel to the right, down, and both) that hold nothing drawn.
// Everything else comes out black. Then glow_blur.frag four times and glow_add.frag over the picture.
#version 450

layout(location = 0) in vec2 picture_uv;
layout(location = 0) out vec4 color;

layout(set = 0, binding = 0) uniform sampler2D picture;         // the picture after the tint (the target's copy)
layout(set = 1, binding = 2) uniform sampler2D scene_depth;     // the shadow set's binding 2: the opaque depth

layout(push_constant) uniform Push {
    vec4 screen;        // the game's screen within the target: its corner and its size, in the target's 0..1
} push;

// 1 where nothing is drawn (the game's depth 0: ngl_vk's 1, the far end), 0 where something is
float sky(vec2 uv)
{
    ivec2 size = textureSize(scene_depth, 0);
    ivec2 at = clamp(ivec2((push.screen.xy + uv * push.screen.zw) * vec2(size)), ivec2(0), size - ivec2(1));

    return 1.0 - texelFetch(scene_depth, at, 0).r > 0.0 ? 0.0 : 1.0;
}

void main()
{
    vec2 half_pixel = 0.5 * fwidth(picture_uv);
    float share = 0.25 * (sky(picture_uv) + sky(picture_uv + vec2(half_pixel.x, 0.0)) +
                          sky(picture_uv + vec2(0.0, half_pixel.y)) + sky(picture_uv + half_pixel));

    color = texture(picture, push.screen.xy + picture_uv * push.screen.zw) * share;
}
