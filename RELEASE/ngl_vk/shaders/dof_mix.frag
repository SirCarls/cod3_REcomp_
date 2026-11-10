// NGL on Vulkan: depth of field and blur, the blurred picture over the sharp one (Call of Duty 3's). The opaque
// meshes' depth (0 near to 1 far) picks how much: by depth from near_depth (none) to far_depth (all), at least
// blanket everywhere, and none where the depth is under keep_depth (what is drawn in front of the world). Blended by
// the alpha written here.
#version 450

layout(location = 0) in vec2 picture_uv;
layout(location = 0) out vec4 color;

layout(set = 0, binding = 0) uniform sampler2D blurred;
layout(set = 1, binding = 2) uniform sampler2D scene_depth;     // the shadow set's binding 2: the opaque depth

layout(push_constant) uniform Focus {
    float near_depth;
    float far_depth;
    float keep_depth;
    float blanket;
} focus;

void main()
{
    float depth = texelFetch(scene_depth, ivec2(gl_FragCoord.xy), 0).r;
    float by_depth = clamp((depth - focus.near_depth) / max(focus.far_depth - focus.near_depth, 1e-6), 0.0, 1.0);
    float amount = depth < focus.keep_depth ? 0.0 : max(focus.blanket, by_depth);

    color = vec4(texture(blurred, picture_uv).rgb, amount);
}
