#version 450
// The result, sampled at the same size with a point sampler: a copy, drawn.
layout(binding = 0) uniform sampler2D src;
layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 o;
void main()
{
    o = texture(src, uv);
}
