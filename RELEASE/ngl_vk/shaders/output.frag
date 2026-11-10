// NGL on Vulkan: the target drawn into another image of the same device (Call of Duty 3's window, the presenter's
// guest output; ngl_vk_output) through the game's gamma table: each channel's 8-bit value v becomes that channel of
// the table's entry v, as the 360's display controller does with the 256 entries the game writes into it.
#version 450

layout(location = 0) in vec2 picture_uv;
layout(location = 0) out vec4 color;

layout(set = 0, binding = 0) uniform sampler2D scene;   // the target
layout(set = 1, binding = 0) uniform sampler2D table;   // 256 x 1: entry v's red, green and blue

void main()
{
    vec3 picture = texture(scene, picture_uv).rgb;
    ivec3 v = ivec3(clamp(picture * 255.0 + 0.5, 0.0, 255.0));

    color = vec4(texelFetch(table, ivec2(v.r, 0), 0).r, texelFetch(table, ivec2(v.g, 0), 0).g,
                 texelFetch(table, ivec2(v.b, 0), 0).b, 1.0);
}
