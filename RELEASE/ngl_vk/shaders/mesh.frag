// NGL on Vulkan: 3D meshes, with their base texture (or, without one, a color the draw's tint picks), lit by their
// lightmap, their corners' colors, or the scene's light (its ambient plus one to four directional lights, held to 1
// when the draw asks, as Call of Duty 3's vertex shaders saturate theirs; without the scene's, one fixed light from
// above: the games' worlds have z up). The normal is the corners' (the mesh's own space), or
// without them the face's, from the screen-space slopes of the mesh's own positions (it faces the eye; turned back
// under a mirroring world matrix); the light comes in already turned into the mesh's space. Alpha-tested
// draws keep only pixels whose alpha is above one half (the NV2A's alpha test at 0x80); the alpha goes out for the
// blended pipeline. Everything but the sky is fogged by the scene's fog (nglSetFog, nglSetFogColor).
//
// Two textures mixed (Big Red One's cdWorldBlend, its pixel shaders 0x0089F090 and 0x0089F270): the base texture and
// the second (set 1, at the lightmap's coordinates) mixed by the corners' alpha (cdOcean's, 0x0089E498: by the second's
// alpha too), then times a third (set 2, at the coordinates in the normal's place) when there is one, else unlit; the
// base texture's alpha is kept. A draw not mixed can be multiplied by a third texture the same way after its light
// (cdOcean's foam, times the map over the sea).
//
// The sun's shadow (Call of Duty 3's sun cascades, ngl_vk_set_sun_shadow): for a draw marked shadowed, the first light
// (or the fixed one) times how much of the sun reaches the pixel past the casters in the shadow map (sun_reaching).
//
// Shiny surfaces (Call of Duty 3's guns: an environment_Texture and a specular map): lit as usual, plus the
// environment map (set 1) as a sphere map at the normal turned toward the eye (0.5 + 0.5 x its x and y), times the
// mask (set 2, at the base texture's coordinates) when there is one.
#version 450

layout(set = 0, binding = 0) uniform sampler2D base_texture;
layout(set = 1, binding = 0) uniform sampler2D lightmap_texture;    // or the second texture mixed in
layout(set = 2, binding = 0) uniform sampler2D third_texture;
// The sun's shadow (ngl_vk_set_sun_shadow): the depth map, compared as it is read (1: no caster nearer the sun), and
// the frame's lookup (mesh.vert uses the matrix; here the bias and a texel)
layout(set = 3, binding = 0) uniform sampler2DShadow sun_shadow;
layout(set = 3, binding = 1) uniform Shadow {
    mat4 lookup;
    vec4 bias_texel;
} shadow;

layout(location = 0) in vec3 object_position;
layout(location = 1) flat in uint tint;
layout(location = 2) in vec2 texture_uv;
layout(location = 3) in vec2 lightmap_texture_uv;
layout(location = 4) in vec4 vertex_color;
layout(location = 5) in float fog_amount;
layout(location = 6) flat in vec3 fog_rgb;
layout(location = 7) flat in uvec4 light_toward;  // mesh.vert: each light's way toward it, 10-bit signed x, y, z
layout(location = 8) flat in uvec4 light_colors;  // and its color, 10-bit red, green, blue, 0 to 4
layout(location = 9) flat in vec3 ambient;
layout(location = 10) in vec3 vertex_normal;
layout(location = 11) flat in uint flags;       // ngl_vk.c's PUSH_*; bits 8-15 the draw's MODE_* bits
layout(location = 12) flat in vec3 eye_x;       // mesh.vert: the normal turned toward the eye is
layout(location = 13) flat in vec3 eye_y;       // (eye_x . n, eye_y . n, eye_w . n)
layout(location = 14) flat in vec3 eye_w;
layout(location = 15) in vec4 shadow_position;  // mesh.vert: in the shadow map's clip space

layout(location = 0) out vec4 color;

const uint ALPHA_TEST = 0x80000000u;        // ngl_vk.c's MESH_ALPHA_TEST
const uint TEXTURED = 0x40000000u;          // ngl_vk.c's MESH_TEXTURED
const uint SHADING_SHIFT = 28u;             // MESH_SHADING_SHIFT; the values are NGL_VK_SHADE_*
const uint SHADE_LIGHTMAP = 1u;
const uint SHADE_VERTEX_COLOR = 2u;
const uint SHADE_UNLIT = 3u;
const uint LIGHT_ONCE = 0x08000000u;       // MESH_LIGHT_ONCE: the lightmap or colors not doubled
const uint TINT = 0x07FFFFFFu;
const uint MODE_MIX = 1u;                   // the base texture and the second (set 1) mixed
const uint MODE_THIRD = 2u;                 // times the third (set 2) at the coordinates in the normal's place
const uint MODE_SHINY = 4u;                 // plus the environment map (set 1)
const uint MODE_MASKED = 8u;                // the environment map times the mask (set 2)
const uint MODE_BY_ALPHA = 16u;             // the mix also by the second's alpha
const uint SCENE_LIGHT = 1u;                // ngl_vk.c's PUSH_SCENE_LIGHT: the scene's lights, not the fixed one
const uint MIRRORED = 2u;                   // PUSH_MIRRORED: the world matrix's determinant is negative
const uint MORE_LIGHTS_SHIFT = 2u;          // PUSH_MORE_LIGHTS_SHIFT: how many lights after the first, 2 bits
const uint HELD = 16u;                      // PUSH_HELD: the light held to 1 before the texture
const uint SHADOWED = 32u;                  // PUSH_SHADOWED: the first light dimmed by the sun's shadow map
const uint MODE_SHIFT = 8u;                 // PUSH_MODE_SHIFT

// How much of the sun reaches this pixel, 0 to 1: four comparisons half a texel around it, averaged (each filtered
// where the GPU filters depth). Outside the map, or for a draw not shadowed, all of it. The map's v runs down where
// Direct3D's clip y runs up (mesh.vert turns y over for Vulkan).
float sun_reaching()
{
    if ((flags & SHADOWED) == 0u || shadow_position.w <= 0.0)
        return 1.0;
    vec3 p = shadow_position.xyz / shadow_position.w;
    vec2 uv = vec2(0.5 + 0.5 * p.x, 0.5 - 0.5 * p.y);
    if (any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0))) || p.z <= 0.0 || p.z >= 1.0)
        return 1.0;
    float depth = p.z - shadow.bias_texel.x;
    float t = 0.5 * shadow.bias_texel.y;
    return 0.25 * (textureLod(sun_shadow, vec3(uv + vec2(-t, -t), depth), 0.0) +
                   textureLod(sun_shadow, vec3(uv + vec2(t, -t), depth), 0.0) +
                   textureLod(sun_shadow, vec3(uv + vec2(-t, t), depth), 0.0) +
                   textureLod(sun_shadow, vec3(uv + vec2(t, t), depth), 0.0));
}

vec3 unpacked_toward(uint p)
{
    ivec3 v = ivec3(bitfieldExtract(int(p), 0, 10), bitfieldExtract(int(p), 10, 10), bitfieldExtract(int(p), 20, 10));

    return vec3(v) / 511.0;
}

vec3 unpacked_color(uint p)
{
    return vec3(float(p & 1023u), float((p >> 10) & 1023u), float((p >> 20) & 1023u)) * (4.0 / 1023.0);
}

vec3 tint_color(uint t)
{
    uint h = t * 2654435761u;               // spread the tint's bits

    return vec3(0.35) + 0.45 * vec3(float(h & 255u), float((h >> 8) & 255u), float((h >> 16) & 255u)) / 255.0;
}

// the game's textures repeat; the samplers clamp (for the quads), so the coordinates are wrapped here, with the
// unwrapped ones' derivatives so the wrap leaves no seam
vec4 wrapped(sampler2D s, vec2 uv)
{
    return textureGrad(s, fract(uv), dFdx(uv), dFdy(uv));
}

void main()
{
    uint shading = (tint >> SHADING_SHIFT) & 3u;
    uint mode = (flags >> MODE_SHIFT) & 255u;
    vec4 base = (tint & TEXTURED) != 0u ? wrapped(base_texture, texture_uv) : vec4(tint_color(tint & TINT), 1.0);
    // everything that takes derivatives comes before the alpha test's discard
    vec4 second = wrapped(lightmap_texture, lightmap_texture_uv);     // the lightmap, or the second texture mixed
    vec3 third = wrapped(third_texture, vertex_normal.xy).rgb;
    vec3 face_normal = ((flags & MIRRORED) != 0u ? -1.0 : 1.0) *
                       normalize(cross(dFdx(object_position), dFdy(object_position)));
    vec3 normal = dot(vertex_normal, vertex_normal) > 0.01 ? normalize(vertex_normal) : face_normal;
    vec3 light, shine = vec3(0.0);
    float alpha = base.a;

    if ((mode & MODE_SHINY) != 0u) {    // the same for every pixel of a draw, so the lookups' derivatives are sound
        vec3 toward_eye = vec3(dot(eye_x, normal), dot(eye_y, normal), dot(eye_w, normal));
        vec2 at = dot(toward_eye, toward_eye) > 0.0 ? normalize(toward_eye).xy : vec2(0.0);

        shine = texture(lightmap_texture, vec2(0.5) + vec2(0.5, -0.5) * at).rgb;
        if ((mode & MODE_MASKED) != 0u)
            shine *= wrapped(third_texture, texture_uv).rgb;
    }

    if ((tint & ALPHA_TEST) != 0u && alpha <= 128.0 / 255.0)
        discard;

    float doubled = (tint & LIGHT_ONCE) != 0u ? 1.0 : 2.0;

    if ((mode & MODE_MIX) != 0u) {
        base.rgb = mix(base.rgb, second.rgb, vertex_color.a * ((mode & MODE_BY_ALPHA) != 0u ? second.a : 1.0));
        light = vec3(1.0);
    } else if (shading == SHADE_LIGHTMAP) {
        light = doubled * second.rgb;
    } else if (shading == SHADE_VERTEX_COLOR) {
        light = doubled * vertex_color.rgb;
        alpha *= vertex_color.a;
    } else if (shading == SHADE_UNLIT) {
        light = vertex_color.rgb;
        alpha *= vertex_color.a;
    } else if ((flags & SCENE_LIGHT) != 0u) {
        uint count = 1u + ((flags >> MORE_LIGHTS_SHIFT) & 3u);
        float sun = sun_reaching();         // the first light is the sun

        light = ambient;
        for (uint i = 0u; i < count; i++) {
            vec3 toward = unpacked_toward(light_toward[i]);

            if (dot(toward, toward) > 0.0)
                light += max(dot(normal, normalize(toward)), 0.0) * unpacked_color(light_colors[i]) *
                         (i == 0u ? sun : 1.0);
        }
        if ((flags & HELD) != 0u)
            light = min(light, vec3(1.0));
    } else {
        vec3 toward = unpacked_toward(light_toward[0]);

        toward = dot(toward, toward) > 0.0 ? normalize(toward) : vec3(0.0);
        light = vec3(0.35 + 0.65 * abs(dot(normal, toward)) * sun_reaching());
    }
    if ((mode & MODE_THIRD) != 0u)
        light *= third;
    color = vec4(min(base.rgb * light + shine, vec3(1.0)), alpha);
    if (shading != SHADE_UNLIT)             // the sky and the background aren't fogged
        color.rgb = mix(color.rgb, fog_rgb, clamp(fog_amount, 0.0, 1.0));
}
