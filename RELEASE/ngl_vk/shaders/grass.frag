// NGL on Vulkan: Call of Duty 3's grass, its pixel shader translated (shader_670333283B688C66): the blade texture
// (the game's tf1) times the color the vertex shader made; pixels outside the blade's 0..1 cut away.
#version 450

layout(set = 2, binding = 0) uniform sampler2D blade_texture;   // tf1

layout(location = 0) in vec4 vertex_color;  // r0
layout(location = 1) in vec2 blade_uv;      // r1

layout(location = 0) out vec4 color;

void main()
{
    if (any(lessThan(blade_uv, vec2(0.0))) || any(lessThan(vec2(1.0) - blade_uv, vec2(0.0))))   // kill_gt x2
        discard;
    vec3 blade = texture(blade_texture, blade_uv).xyz;      // 6
    color = vec4(clamp(blade * vertex_color.xyz, 0.0, 1.0), vertex_color.w);   // 7
}
