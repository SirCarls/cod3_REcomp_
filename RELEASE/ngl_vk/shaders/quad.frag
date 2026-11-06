// NGL on Vulkan: the picture, opaque (the X8R8G8B8 pictures' fourth byte is unused).
#version 450

layout(location = 0) in vec2 picture_uv;
layout(location = 0) out vec4 color;

layout(set = 0, binding = 0) uniform sampler2D picture;

void main()
{
    color = vec4(texture(picture, picture_uv).rgb, 1.0);
}
