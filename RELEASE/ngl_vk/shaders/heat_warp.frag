// [rexglue-native-fx] 2026-11-11 fx1 NGL on Vulkan: the screen warp's color (Call of Duty 3's heat haze, its pixel shader
// DFE83B062BA63F7F: the frame's copy as it is, blended by the copy's alpha, which the game's mask quads wrote). Here the
// mask is apart from the picture: heat_mask holds 1 - the mask (cleared to 1, each mask quad multiplying by 1 - its
// alpha), read where the copy is read.
#version 450

layout(location = 0) in vec2 picture_uv;
layout(location = 0) out vec4 color;

layout(set = 0, binding = 0) uniform sampler2D picture;    // the target, copied (post_copy)
layout(set = 1, binding = 0) uniform sampler2D heat;       // 1 - the heat haze's mask (heat_mask)

void main()
{
    color = vec4(texture(picture, picture_uv).rgb, 1.0 - texture(heat, picture_uv).r);
}
