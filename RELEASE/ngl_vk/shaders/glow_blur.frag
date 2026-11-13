// [rexglue-native] 2026-11-10 NGL on Vulkan: one of the sky glow's blurs (ngl_vk_set_glow; glow_first.frag has the
// whole of it). Call of Duty 3's pixel shader AC63ECD48A1A99BF with the four tap places its function 0x820D1A10 gives
// each corner: the mean of four taps times a gain. Plain: the taps in a plus around the pixel, each `tap` away. Leaning
// (toward the sun: its shafts of light): the taps along the line from the pixel to a point, 0, 1, 4 and 8 times `tap`
// of the way there. The game mixes the two by the lean, tap by tap.
#version 450

layout(location = 0) in vec2 picture_uv;
layout(location = 0) out vec4 color;

layout(set = 0, binding = 0) uniform sampler2D glow;            // the pass before this one

layout(push_constant) uniform Push {
    vec4 blur;          // x: the tap, in the glow's 0..1 (pass k: k times the first's); y: the gain; z: the lean, 0..1
    vec4 toward;        // xy: the point the taps lean toward, in the glow's 0..1
} push;

void main()
{
    const vec2 plus[4] = vec2[](vec2(1.0, 0.0), vec2(-1.0, 0.0), vec2(0.0, 1.0), vec2(0.0, -1.0));
    const float along[4] = float[](0.0, 1.0, 4.0, 8.0);
    vec2 to = push.toward.xy - picture_uv;
    vec4 sum = vec4(0.0);

    for (int i = 0; i < 4; i++)
        sum += texture(glow, picture_uv + mix(plus[i], to * along[i], push.blur.z) * push.blur.x);
    color = 0.25 * push.blur.y * sum;
}
