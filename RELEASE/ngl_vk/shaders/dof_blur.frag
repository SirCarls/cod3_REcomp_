// NGL on Vulkan: depth of field, one direction of the blur (Call of Duty 3's). The picture at a quarter of its size,
// blurred across or down by nine taps of a Gaussian (sigma about two taps); step is one tap, in picture coordinates.
// [acct2-bro] 2026-11-13 rn64c: kind 1 is Call of Duty 3's own depth of field blur, whole in one pass (the taps its
// 0x82146E90 makes for its pixel shader 019C0B5CF349676A: the 13 texels where |x| + |y| <= 2, each weighted
// exp(-(x^2 + y^2) / 2) over the sum of the 13); step is then one texel across and one down. kind 2: a plain copy.
#version 450

layout(location = 0) in vec2 picture_uv;
layout(location = 0) out vec4 color;

layout(set = 0, binding = 0) uniform sampler2D picture;

layout(push_constant) uniform Blur {
    vec2 step;
    float kind;             // rn64c: 0 the line of nine taps, 1 the game's 13 taps, 2 a plain copy
    float not_used;
} blur;

void main()
{
    if (blur.kind > 1.5) {      // rn64c: a copy
        color = vec4(texture(picture, picture_uv).rgb, 1.0);
        return;
    }
    if (blur.kind > 0.5) {      // rn64c: the game's 13 taps
        vec3 taps = vec3(0.0);
        float total = 0.0;

        for (int y = -2; y <= 2; y++) {
            for (int x = -2; x <= 2; x++) {
                if (abs(x) + abs(y) > 2)
                    continue;
                float share = exp(-0.5 * float(x * x + y * y));
                taps += texture(picture, picture_uv + blur.step * vec2(float(x), float(y))).rgb * share;
                total += share;
            }
        }
        color = vec4(taps / total, 1.0);
        return;
    }
    const float weight[5] = float[](0.2270270270, 0.1945945946, 0.1216216216, 0.0540540541, 0.0162162162);
    vec3 sum = texture(picture, picture_uv).rgb * weight[0];

    for (int i = 1; i < 5; i++) {
        sum += texture(picture, picture_uv + blur.step * float(i)).rgb * weight[i];
        sum += texture(picture, picture_uv - blur.step * float(i)).rgb * weight[i];
    }
    color = vec4(sum, 1.0);
}
