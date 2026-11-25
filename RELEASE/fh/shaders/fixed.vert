#version 450
// [waw-decomp] Finest Hour port: the Xbox's fixed-function vertex stage, for the game's FVF draws (src/fh_draw.c).
//
// The vertex: position (x, y, z floats), diffuse color (a D3DCOLOR: its bytes are blue, green, red, alpha, read here
// as R8G8B8A8_UNORM and put back in order below) and one texture coordinate pair.
// Constants (binding 0, ngl_vk_low's vertex constants): c0-c3 are the rows of world x view x projection, as Direct3D
// multiplies them (a row vector times the matrix).
// [acct2-fh] 2026-11-13: the position has four parts. FVF draws give three (x, y, z) and Vulkan supplies w = 1, so
// they are unchanged; vertex program draws (fh_draw.c, program_vertices) give the program's clip-space x, y, z, w with
// an identity c0-c3, so Vulkan clips them and interpolates with perspective.
// [acct2-fh] 2026-11-14: the fog distance for fixed.frag (src/fh_draw.c says how the game's fog works): c4.x 0, the
// eye's depth, which is the clip-space w (the fixed-function draws); c4.x 1, the vertex's own (location 3: a program's
// oFog.x as fh_draw.c's program_vertices wrote it, 0 for the vertices already on the screen).
// [fh8] 2026-11-24: Direct3D's fixed-function lighting (fh_draw.c, lighting_constants, lists c5-c66). With c5.x 1 the
// vertex color is the emissive color, plus the ambient color times the global ambient and each light's ambient, plus
// the diffuse color times each light's diffuse and the normal's cosine to the light; point and spot lights fade by
// 1 / (a0 + a1 d + a2 d^2) and end at their range, spot lights fade between their cones. The alpha is the diffuse
// color's. Specular is not added (no second color yet).
layout(location = 0) in vec4 in_position;
layout(location = 1) in vec4 in_diffuse_bytes;
layout(location = 2) in vec2 in_uv0;
layout(location = 3) in float in_fog;
layout(location = 4) in vec3 in_normal;

layout(set = 0, binding = 0) uniform VertexConstants {
    vec4 c[256];
} vs;

layout(location = 0) out vec4 out_diffuse;
layout(location = 1) out vec3 out_uv0;     // [fh6] u, v, q: q is 1 here (fixed.frag divides by it)
layout(location = 2) out float out_fog;

const int LIGHT_AT = 19;

vec4 lit(vec4 vertex_color)
{
    vec3 p = (in_position.x * vs.c[7] + in_position.y * vs.c[8] + in_position.z * vs.c[9] +
              in_position.w * vs.c[10]).xyz;
    vec3 n = in_normal.x * vs.c[11].xyz + in_normal.y * vs.c[12].xyz + in_normal.z * vs.c[13].xyz;
    vec4 material_diffuse = vs.c[6].x > 0.5 ? vertex_color : vs.c[15];
    vec4 material_ambient = vs.c[6].y > 0.5 ? vertex_color : vs.c[16];
    vec4 material_emissive = vs.c[6].w > 0.5 ? vertex_color : vs.c[18];
    vec3 ambient = vs.c[14].rgb;
    vec3 diffuse = vec3(0.0);
    int lights = int(vs.c[5].z + 0.5);

    if (vs.c[5].y > 0.5 && dot(n, n) > 0.0)
        n = normalize(n);
    for (int i = 0; i < 8; i++) {
        if (i >= lights)
            break;
        int at = LIGHT_AT + i * 6;
        vec4 position = vs.c[at + 2], direction = vs.c[at + 3], fade = vs.c[at + 4], cone = vs.c[at + 5];
        vec3 to_light;
        float strength = 1.0;

        if (position.w > 2.5) {                         // directional
            to_light = -direction.xyz;
        } else {                                        // point (1) or spot (2)
            vec3 d = position.xyz - p;
            float distance = length(d);

            if (distance > direction.w)
                continue;
            to_light = distance > 0.0 ? d / distance : vec3(0.0, 0.0, 1.0);
            strength = 1.0 / max(fade.x + fade.y * distance + fade.z * distance * distance, 1e-6);
            if (position.w > 1.5) {
                float rho = dot(-to_light, direction.xyz);

                if (rho <= cone.y)
                    strength = 0.0;
                else if (rho < cone.x)
                    strength *= pow(clamp((rho - cone.y) / max(cone.x - cone.y, 1e-6), 0.0, 1.0),
                                    max(fade.w, 1e-6));
            }
        }
        ambient += strength * vs.c[at + 1].rgb;
        diffuse += strength * max(dot(n, to_light), 0.0) * vs.c[at].rgb;
    }
    return clamp(vec4(material_emissive.rgb + material_ambient.rgb * ambient + material_diffuse.rgb * diffuse,
                      material_diffuse.a), 0.0, 1.0);
}

void main()
{
    vec4 clip = in_position.x * vs.c[0] + in_position.y * vs.c[1] + in_position.z * vs.c[2] + in_position.w * vs.c[3];

    out_fog = vs.c[4].x > 0.5 ? in_fog : clip.w;
    clip.y = -clip.y;   // Direct3D's clip space has y up; Vulkan's framebuffer has y down
    gl_Position = clip;
    out_diffuse = vs.c[5].x > 0.5 ? lit(in_diffuse_bytes.zyxw) : in_diffuse_bytes.zyxw;
    out_uv0 = vec3(in_uv0, 1.0);
}
