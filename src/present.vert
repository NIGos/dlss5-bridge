#version 450
// One fullscreen triangle from the vertex index, no vertex buffer. Clip space
// is written in D3D's convention (y up) because ReShade's Vulkan command list
// flips the viewport it is handed, so uv (0,0) lands at the top-left as it
// does on every other backend.
layout(location = 0) out vec2 uv;
void main()
{
    uv = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    gl_Position = vec4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, 0.0, 1.0);
}
