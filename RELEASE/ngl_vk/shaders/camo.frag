// [acct2-bro] 2026-11-22 Camo lane: the camo pass's color (ngl_vk.h camo_texture; src/ngl_vk.c make_camo, draw_camo).
// The factor the picture under it is multiplied by: mix(1, the camo's color / its mean color, strength), so where
// the pattern is as bright as its mean the gun keeps its own color and light, and the pattern's colors show over
// its shading. The pipeline blends DST_COLOR x out + SRC_COLOR x behind (the picture x 2 x out), so the factor goes
// out halved: 0 to 2 fits the UNORM target. The camo is wrapped here (the textures' sampler clamps to the edge),
// read with the unwrapped coordinates' slopes so its level doesn't jump at the seams.
#version 450

layout(set = 0, binding = 0) uniform sampler2D camo_map;

layout(location = 0) in vec2 camo_uv;
layout(location = 1) flat in vec4 strength_mean;   // the strength (0 to 1), then the mean red, green and blue

layout(location = 0) out vec4 color;

void main()
{
    vec3 camo = textureGrad(camo_map, fract(camo_uv), dFdx(camo_uv), dFdy(camo_uv)).rgb;
    vec3 factor = mix(vec3(1.0), camo / max(strength_mean.yzw, vec3(1.0 / 255.0)), clamp(strength_mean.x, 0.0, 1.0));
    color = vec4(clamp(factor, 0.0, 2.0) * 0.5, 1.0);
}
