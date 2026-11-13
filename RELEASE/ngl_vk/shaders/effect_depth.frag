// [rexglue-native] 2026-11-10 NGL on Vulkan: the effect layer's depth (ngl_vk.h effect_layer). Call of Duty 3 draws its
// smoke, dust and fire into a target of half the picture's width and height, tested against a depth buffer of that
// size that it fills from the picture's depth with one fetch a pixel (its pixel shader 2BE9B766ED8C769E, drawn over the
// whole small target before the effects): the small target's pixel (x, y) takes the picture's depth at (2x, 2y).
#version 450

layout(set = 1, binding = 2) uniform sampler2D scene_depth;     // the shadow set's binding 2: the opaque depth

void main()
{
    ivec2 last = textureSize(scene_depth, 0) - ivec2(1);

    gl_FragDepth = texelFetch(scene_depth, min(2 * ivec2(gl_FragCoord.xy), last), 0).r;
}
