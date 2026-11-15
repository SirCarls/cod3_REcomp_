// NGL on Vulkan: the target drawn into another image of the same device (Call of Duty 3's window, the presenter's
// guest output; ngl_vk_output) through the game's gamma table: each channel's 8-bit value v becomes that channel of
// the table's entry v, as the 360's display controller does with the 256 entries the game writes into it.
#version 450

layout(location = 0) in vec2 picture_uv;
layout(location = 0) out vec4 color;

layout(set = 0, binding = 0) uniform sampler2D scene;   // the target
layout(set = 1, binding = 0) uniform sampler2D table;   // 256 x 1: entry v's red, green and blue

// [acct2-bro] 2026-11-12 rn61a: supersampling. When the target is N times larger each way than the image drawn
// into (Call of Duty 3's cod3_native_antialiasing), a pixel is the mean of the N x N target pixels it covers, before
// the table. N comes from how far the target's place moves from one pixel to the next: 1 (the pixel itself, as
// before) to 4.
void main()
{
    ivec2 size = textureSize(scene, 0);
    vec2 place = picture_uv * vec2(size);                   // this pixel's center, in target pixels
    vec2 across = fwidth(picture_uv) * vec2(size);          // target pixels to the next pixel, each way
    int n = clamp(int(floor(max(across.x, across.y) + 0.5)), 1, 4);
    vec3 picture;

    if (n == 1) {
        picture = texture(scene, picture_uv).rgb;
    } else {
        ivec2 first = ivec2(floor(place - 0.5 * float(n) + 0.5));
        vec3 sum = vec3(0.0);

        for (int y = 0; y < n; y++)
            for (int x = 0; x < n; x++)
                sum += texelFetch(scene, clamp(first + ivec2(x, y), ivec2(0), size - 1), 0).rgb;
        picture = sum / float(n * n);
    }
    ivec3 v = ivec3(clamp(picture * 255.0 + 0.5, 0.0, 255.0));

    color = vec4(texelFetch(table, ivec2(v.r, 0), 0).r, texelFetch(table, ivec2(v.g, 0), 0).g,
                 texelFetch(table, ivec2(v.b, 0), 0).b, 1.0);
}
