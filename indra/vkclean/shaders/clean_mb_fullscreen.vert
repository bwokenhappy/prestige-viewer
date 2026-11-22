#version 450
// vkclean -- MBOIT (clean_mboit.glsl): the cutoff and resolve passes' fullscreen triangle.
void main()
{
    const vec2 p = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
