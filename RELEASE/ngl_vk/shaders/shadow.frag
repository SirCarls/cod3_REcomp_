// NGL on Vulkan: a shadow caster's pixels in the sun's shadow map (ngl_vk_set_sun_shadow), depth only. Alpha-tested
// casters keep only the pixels mesh.frag keeps (alpha above one half, or at the draw's own cut: [acct2-bro] rn115a),
// so a tree's leaves cast their own shapes.
#version 450

layout(set = 0, binding = 0) uniform sampler2D base_texture;

layout(location = 1) flat in uint tint;     // mesh.vert: the draw's flags, passed as its first instance
layout(location = 2) in vec2 texture_uv;
layout(location = 8) flat in uvec4 light_colors;    // [acct2-bro] 2026-11-22 rn115a: the draw's own cut in the
layout(location = 11) flat in uint flags;           // fourth's place (mesh.frag), and how many lights it has

const uint ALPHA_TEST = 0x80000000u;        // ngl_vk.c's MESH_ALPHA_TEST
const uint TEXTURED = 0x40000000u;          // ngl_vk.c's MESH_TEXTURED
const uint MORE_LIGHTS_SHIFT = 2u;          // ngl_vk.c's PUSH_MORE_LIGHTS_SHIFT

void main()
{
    // [acct2-bro] 2026-11-22 rn115a: the alpha test's edge as mesh.frag's (the draw's own cut, else one half)
    bool own_cut = ((flags >> MORE_LIGHTS_SHIFT) & 3u) < 3u && light_colors[3] != 0u;
    float cut_edge = own_cut ? uintBitsToFloat(light_colors[3]) - 0.5 / 255.0 : 128.0 / 255.0;

    // the texture repeats and the sampler clamps: wrapped as mesh.frag does, with the unwrapped derivatives
    if ((tint & (ALPHA_TEST | TEXTURED)) == (ALPHA_TEST | TEXTURED) &&
        textureGrad(base_texture, fract(texture_uv), dFdx(texture_uv), dFdy(texture_uv)).a <= cut_edge)
        discard;
}
