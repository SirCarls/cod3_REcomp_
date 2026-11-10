// NGL on Vulkan: depth of field, one direction of the blur (Call of Duty 3's). The picture at a quarter of its size,
// blurred across or down by nine taps of a Gaussian (sigma about two taps); step is one tap, in picture coordinates.
#version 450

layout(location = 0) in vec2 picture_uv;
layout(location = 0) out vec4 color;

layout(set = 0, binding = 0) uniform sampler2D picture;

layout(push_constant) uniform Blur {
    vec2 step;
} blur;

void main()
{
    const float weight[5] = float[](0.2270270270, 0.1945945946, 0.1216216216, 0.0540540541, 0.0162162162);
    vec3 sum = texture(picture, picture_uv).rgb * weight[0];

    for (int i = 1; i < 5; i++) {
        sum += texture(picture, picture_uv + blur.step * float(i)).rgb * weight[i];
        sum += texture(picture, picture_uv - blur.step * float(i)).rgb * weight[i];
    }
    color = vec4(sum, 1.0);
}
