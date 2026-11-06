// NGL on Vulkan: 3D meshes, with their base texture, lit by their lightmap, their corners' colors or the scene's light.
#version 450

layout(location = 0) in vec3 position;      // the mesh's own space
layout(location = 1) in vec2 uv;            // the base texture's coordinates
layout(location = 2) in vec2 lightmap_uv;   // the lightmap's (cdWorld)
layout(location = 3) in vec4 color;         // a D3DCOLOR, read as blue, green, red, alpha bytes: rgba here
layout(location = 4) in vec3 normal;        // the mesh's own space; 0, 0, 0: none

// Direct3D's row-vector matrices: the world-view-projection row by row, read into a GLSL mat4 as its transpose, so
// matrix * vector here is Direct3D's vector * matrix.
layout(push_constant) uniform Draw {
    mat4 world_view_projection;
    vec3 fog;               // start and end (distances from the eye), the amount at the end; end <= start: no fog
    uint fog_color;         // red, green, blue bytes, then the amount before the start as a byte
    vec3 ambient;
    uint flags;             // ngl_vk.c's PUSH_*: the scene's light or a fixed one, mirrored, how many more lights,
                            // held to 1; bits 8-15 the draw's MESH_MODE_* bits
    uvec4 toward;           // the way toward each light in the mesh's own space, x, y, z 10 bits each, signed
    uvec4 light_colors;     // each light's color, red, green, blue 10 bits each, 0 to 4
} draw;

layout(location = 0) out vec3 object_position;  // the mesh's own space: the fragment shader's normal comes from it
layout(location = 1) flat out uint tint;
layout(location = 2) out vec2 texture_uv;
layout(location = 3) out vec2 lightmap_texture_uv;
layout(location = 4) out vec4 vertex_color;
layout(location = 5) out float fog_amount;
layout(location = 6) flat out vec3 fog_rgb;
layout(location = 7) flat out uvec4 light_toward;
layout(location = 8) flat out uvec4 light_colors;
layout(location = 9) flat out vec3 ambient;
layout(location = 10) out vec3 vertex_normal;
layout(location = 11) flat out uint flags;
layout(location = 12) flat out vec3 eye_x;     // the world-view-projection's rows for x, y and w, each of unit length:
layout(location = 13) flat out vec3 eye_y;     // a normal in the mesh's space turned toward the eye (shiny surfaces)
layout(location = 14) flat out vec3 eye_w;
layout(location = 15) out vec4 shadow_position;   // in the sun's shadow map's clip space (mesh.frag's sun_reaching)

// The sun's shadow (ngl_vk_set_sun_shadow): the camera's clip space to the light's, Direct3D's row by row like the
// world-view-projection; then the bias and a texel (read by mesh.frag)
layout(set = 3, binding = 1) uniform Shadow {
    mat4 lookup;
    vec4 bias_texel;
} shadow;

void main()
{
    uint c = draw.fog_color;

    object_position = position;
    flags = draw.flags;
    light_toward = draw.toward;
    light_colors = draw.light_colors;
    vertex_normal = normal;
    {   // the rows of the matrix's first three columns: GLSL's m * v here, so row r is (m[0][r], m[1][r], m[2][r])
        mat4 m = draw.world_view_projection;
        vec3 x = vec3(m[0][0], m[1][0], m[2][0]), y = vec3(m[0][1], m[1][1], m[2][1]);
        vec3 w = vec3(m[0][3], m[1][3], m[2][3]);

        eye_x = dot(x, x) > 0.0 ? normalize(x) : vec3(0.0);
        eye_y = dot(y, y) > 0.0 ? normalize(y) : vec3(0.0);
        eye_w = dot(w, w) > 0.0 ? normalize(w) : vec3(0.0);
    }
    ambient = draw.ambient;
    tint = uint(gl_InstanceIndex);          // the draw's tint, shading and flags, passed as its first instance
    texture_uv = uv;
    lightmap_texture_uv = lightmap_uv;
    vertex_color = color;
    gl_Position = draw.world_view_projection * vec4(position, 1.0);
    shadow_position = shadow.lookup * gl_Position;      // only meaningful for draws placed with the camera given
    // linear fog by the distance from the eye (clip w is the view's depth)
    fog_amount = draw.fog.y > draw.fog.x
                 ? mix(float(c >> 24) / 255.0, draw.fog.z,
                       clamp((gl_Position.w - draw.fog.x) / (draw.fog.y - draw.fog.x), 0.0, 1.0))
                 : 0.0;
    fog_rgb = vec3(float(c & 255u), float((c >> 8) & 255u), float((c >> 16) & 255u)) / 255.0;
    gl_Position.y = -gl_Position.y;         // Direct3D's clip space has y up, Vulkan's down; z is 0..1 in both
}
