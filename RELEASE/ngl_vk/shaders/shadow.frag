// NGL on Vulkan: a shadow caster's pixels in the sun's shadow map (ngl_vk_set_sun_shadow), depth only. Alpha-tested
// casters keep only the pixels mesh.frag keeps (alpha above one half), so a tree's leaves cast their own shapes.
#version 450

layout(set = 0, binding = 0) uniform sampler2D base_texture;

layout(location = 1) flat in uint tint;     // mesh.vert: the draw's flags, passed as its first instance
layout(location = 2) in vec2 texture_uv;

const uint ALPHA_TEST = 0x80000000u;        // ngl_vk.c's MESH_ALPHA_TEST
const uint TEXTURED = 0x40000000u;          // ngl_vk.c's MESH_TEXTURED

void main()
{
    // the texture repeats and the sampler clamps: wrapped as mesh.frag does, with the unwrapped derivatives
    if ((tint & (ALPHA_TEST | TEXTURED)) == (ALPHA_TEST | TEXTURED) &&
        textureGrad(base_texture, fract(texture_uv), dFdx(texture_uv), dFdy(texture_uv)).a <= 128.0 / 255.0)
        discard;
}
