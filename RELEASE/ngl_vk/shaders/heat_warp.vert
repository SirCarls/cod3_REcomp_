// [rexglue-native-fx] 2026-11-11 fx1 NGL on Vulkan: the screen warp (ngl_vk_set_screen_warp; Call of Duty 3's heat haze, its vertex
// shader 9F3B80D28ED167A9, whose placing ngl_side has already done with the game's own constants): each corner where it
// goes on the game's screen (-1 to 1, y down) and where it reads the picture's copy (the target's 0 to 1).
#version 450

layout(location = 0) in vec2 position;
layout(location = 1) in vec2 uv;
layout(location = 0) out vec2 picture_uv;

void main()
{
    picture_uv = uv;
    gl_Position = vec4(position, 0.0, 1.0);
}
