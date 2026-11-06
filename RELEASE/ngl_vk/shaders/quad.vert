// NGL on Vulkan: one triangle that covers the viewport, with picture coordinates 0..1 across it.
#version 450

layout(location = 0) out vec2 picture_uv;

void main()
{
    // vertices 0, 1, 2 at (0, 0), (2, 0), (0, 2): the part inside 0..1 is the whole viewport
    vec2 corner = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);

    picture_uv = corner;
    gl_Position = vec4(corner * 2.0 - 1.0, 0.0, 1.0);
}
