// NGL on Vulkan: 2D quads: the picture (opaque, X8R8G8B8, or with its alpha) times the quad's color, or the color
// alone.
#version 450

layout(location = 0) in vec2 picture_uv;
layout(location = 1) in vec4 quad_color;
layout(location = 2) in float use_picture;
layout(location = 0) out vec4 color;

layout(set = 0, binding = 0) uniform sampler2D picture;

void main()
{
    vec4 texel = vec4(1.0);
    // [rexglue-native-render] 2026-11-11 rn48l: NGL_VK_QUAD_ADD (4) on top of the mode only picks the pipeline (added)
    float mode = use_picture > 3.5 ? use_picture - 4.0 : use_picture;

    if (mode > 1.5)
        texel = texture(picture, picture_uv);
    else if (mode > 0.5)
        texel = vec4(texture(picture, picture_uv).rgb, 1.0);

    color = texel * quad_color;
}
