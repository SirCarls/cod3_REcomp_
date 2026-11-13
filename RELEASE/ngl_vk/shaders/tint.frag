// NGL on Vulkan: a color tint over the 3D (Call of Duty 3's post effect 1, its pixel shader's "index", "orange" and
// "exposure"): each pixel moves toward its brightness times a color, by an amount. Not where the opaque depth is under
// keep_depth: the scenes drawn in front of the world, which the game draws after its post effects. Then post effect 2
// (shader_377AC015A488B262): the color times a brightness, to a power. Then post effect 4 (shader_1A3598F9A2CE3A59):
// a share of the last frame's picture, a motion blur. Before that, post effect 3 (shader_8B289D78B550EBBD): a color
// look-up, red and green from a table at (red, green), blue from it at (blue, red).
// [rexglue-native] 2026-11-10 rn41b: and post effect 7 (ngl_vk_set_film), the blur, grain and gray of a concussion,
// which the game draws as two passes over its screen copy after the others (pixel shaders 470878A4FB9D8A6F, then
// D7B45AAE96CFCD55): see film_at and main. ngl_vk draws it in a draw of its own after this one, with this pass's other
// numbers neutral and keep_depth below any depth (the game's concussion blurs its gun too). rn41g: the motion blur too,
// in a draw of its own before post effect 7's; both before the quads (the game's crosshair stays sharp).
#version 450

layout(location = 0) in vec2 picture_uv;
layout(location = 0) out vec4 color;

layout(set = 0, binding = 0) uniform sampler2D scene;           // the target, copied
layout(set = 1, binding = 2) uniform sampler2D scene_depth;     // the shadow set's binding 2: the opaque depth
layout(set = 2, binding = 0) uniform sampler2D last_frame;      // the last frame's picture, after this pass
layout(set = 3, binding = 0) uniform sampler2D lookup;          // the color look-up's table (ngl_vk_set_color_lookup)

layout(push_constant) uniform Tint {
    vec4 weights;       // rgb: the brightness's weights (times the exposure); a: the amount
    vec4 tone;          // rgb: the color; a: keep_depth
    vec4 curve;         // x: the brightness, y: the power (1, 1: none); z: the last frame's share (0: none);
                        // w: 1 the color look-up (0: none)
    vec4 film;          // post effect 7's first pass, its c0: x how far apart its taps are, y the grain's share, z how
                        // far toward the gray, w its time
    vec4 film_gray;     // its c1: the gray's color
    vec4 film_move;     // x its c2's scale on the color before the gray (0: no post effect 7); y, z the second pass's
                        // move of the picture; w the moved picture's share
    vec4 after;         // [rexglue-native] 2026-11-11 rn47g: x above 0: this draw is the first pass of post effect 4
                        // as the game draws it (ngl_vk_set_afterimage), x how far in from every edge it reads
    vec4 tweak;         // [waw-decomp] 2026-11-11: w above 0.5: the film tweak (ngl_vk_set_film_tweak) in place of
                        // the tint, the brightness and the color look-up, rgb its delta; then weights is its bias
                        // and tone.rgb its base
} tint;

// The picture after post effects 1 to 4 at a pixel; front: it is a scene drawn in front of the world (left alone)
vec3 toned_at(ivec2 at, out bool front)
{
    vec3 picture = texelFetch(scene, at, 0).rgb;
    float depth = texelFetch(scene_depth, at, 0).r;
    front = depth < tint.tone.a;
    if (front)
        return picture;
    // [waw-decomp] 2026-11-11: Call of Duty 4's and World at War's film (their PC copies' postfx_color.hlsl, compiled
    // ps_3_0_2acda88e: c7 colorBias, c5 colorTintBase, c6 colorTintDelta), alone in its frame's draw
    if (tint.tweak.w > 0.5) {
        float lum = dot(picture, vec3(0.299, 0.587, 0.114));
        return tint.weights.rgb + (tint.tone.rgb + tint.tweak.rgb * lum) * (picture * tint.weights.a + lum);
    }
    vec3 toned = mix(picture, dot(picture, tint.weights.rgb) * tint.tone.rgb, tint.weights.a);
    toned = pow(max(toned * tint.curve.x, vec3(0.0)), vec3(tint.curve.y));
    if (tint.curve.w > 0.5) {
        vec3 c = clamp(toned, 0.0, 1.0);
        toned = vec3(texture(lookup, c.rg).rg, texture(lookup, vec2(c.b, c.r)).b);
    }
    return mix(toned, texelFetch(last_frame, at, 0).rgb, tint.curve.z);
}

ivec2 pixel_of(vec2 uv)
{
    ivec2 size = textureSize(scene, 0);
    return clamp(ivec2(floor(uv * vec2(size))), ivec2(0), size - 1);
}

// Post effect 7's first pass at uv (0 to 1 across the picture), from 470878A4FB9D8A6F read instruction by instruction
// with its literals (c252 96.744675 81.23949 -0.5 0.2, c253 81.23949 114.0927 0 1, c254 0.11 0.3 0.59 0.14159, c255
// 81.23949 17.348024 37.34984 59.39484): five taps of the picture, each at a place hashed from uv and the time (a seed
// stepped 0.14159 a tap) up to film.x / 2 away and mixed with three hashed grains (0 to 1) by film.y, averaged; then
// moved by film.z toward the gray of (that x film_move.x) times film_gray. A tap on a scene kept in front (none, as
// ngl_vk draws it) takes beside instead.
vec3 film_at(vec2 uv, vec3 beside)
{
    const vec3 k252 = vec3(96.744675, 81.23949, -0.5);
    const vec2 k253 = vec2(81.23949, 114.0927);
    const vec4 k255 = vec4(81.23949, 17.348024, 37.34984, 59.39484);
    float seed = tint.film.w;
    vec3 sum = vec3(0.0);
    for (int tap = 0; tap < 5; tap++) {
        // the tap's place (its instructions 9-39)
        float u = seed + uv.x, v = seed + uv.y;
        float a = fract(u * k253.x + v * k253.y);
        float m = k255.y * fract(a * k253.x + v * k253.y);
        float d = fract(m + v * k252.x + a * k252.y);
        float e = k255.z * d;
        float f = k255.w * fract(m + v * k255.w + a * k255.x + e);
        float g = fract(a * k255.x + m + e + f);
        float h = k255.x * g;
        float w = fract(h + m + e + f);
        float h2 = w * k255.y + h;
        float z = fract(h2 + e + f);
        float h3 = z * k255.z + h2;
        float x = fract(h3 + f);
        vec4 place = vec4(g, w, x, z) + k252.z;
        vec4 at = place * tint.film.x + uv.xyyy;
        bool front;
        vec3 picture = toned_at(pixel_of(at.xy), front);
        if (front)
            picture = beside;
        // its grains, hashed from the place (its instructions 43-69)
        float p = fract(dot(at.zwyx, k255.wzyx));
        float s1 = fract(at.z * k255.w + at.w * k255.z + at.y * k255.y + p * k255.x);
        float t1 = p * k255.x + s1 * k255.y;
        float e2 = k255.z * fract(at.z * k255.w + at.w * k255.z + t1);
        float s2 = t1 + e2;
        float f2 = k255.w * fract(at.z * k255.w + s2);
        float ny = fract(s2 + f2);
        float h4 = k255.x * ny;
        float nx = fract(s1 * k255.y + h4 + e2 + f2);
        float h5 = nx * k255.y + h4;
        float nz = fract(h5 + e2 + f2);
        // red takes the grain ny, green nx, blue nz
        sum += picture * (1.0 - tint.film.y) + vec3(ny, nx, nz) * tint.film.y;
        seed += 0.14159;
    }
    vec3 average = sum * 0.2;
    float gray = dot(average * tint.film_move.x, vec3(0.3, 0.59, 0.11));
    return mix(average, gray * tint.film_gray.rgb, tint.film.z);
}

void main()
{
    ivec2 at = ivec2(gl_FragCoord.xy);
    // [rexglue-native] 2026-11-11 rn47g: post effect 4's first pass, the one the game draws into its kept picture
    // (its pixel shader: fac1 x Diffuse_Map + fac2 x Diffuse2_Map; its corners' coordinates drawn in by `after.x` on
    // every side, decomp 0x821472C8): the picture so far and the kept picture (set 2 here), both read that far in and
    // filtered, mixed by curve.z. ngl_vk copies the result into the kept picture and then draws the second pass, which
    // is this shader's plain mix with the last frame (toned_at) with the kept picture on set 2.
    if (tint.after.x > 0.0) {
        vec2 uv = (vec2(at) + 0.5) / vec2(textureSize(scene, 0));
        vec2 inside = tint.after.x + uv * (1.0 - 2.0 * tint.after.x);
        color = vec4(mix(texture(scene, inside).rgb, texture(last_frame, inside).rgb, tint.curve.z), 1.0);
        return;
    }
    bool front;
    vec3 toned = toned_at(at, front);
    // post effect 7's second pass (D7B45AAE96CFCD55): its first pass here and at a place moved by film_move.yz
    // (wrapped), mixed by film_move.w
    if (!front && tint.film_move.x > 0.0) {
        vec2 uv = (vec2(at) + 0.5) / vec2(textureSize(scene, 0));
        vec3 here = film_at(uv, toned);
        vec3 moved = film_at(fract(uv + tint.film_move.yz), toned);
        toned = mix(here, moved, tint.film_move.w);
    }
    color = vec4(toned, 1.0);
}
