// NGL on Vulkan: 2D quads (menus, HUD, text, movies), in the game's screen pixels.
#version 450

layout(location = 0) in vec2 position;      // screen pixels, (0, 0) top left
layout(location = 1) in vec2 uv;
layout(location = 2) in vec4 color;         // D3DCOLOR, read as B8G8R8A8: arrives as red, green, blue, alpha
layout(location = 3) in float textured;     // 1: the quad's picture times its color; 0: its color only

layout(push_constant) uniform Screen {
    vec2 size;                              // the game's screen in pixels
} screen;

layout(location = 0) out vec2 picture_uv;
layout(location = 1) out vec4 quad_color;
layout(location = 2) out float use_picture;

void main()
{
    picture_uv = uv;
    quad_color = color;
    use_picture = textured;
    gl_Position = vec4(position / screen.size * 2.0 - 1.0, 0.0, 1.0);
}
