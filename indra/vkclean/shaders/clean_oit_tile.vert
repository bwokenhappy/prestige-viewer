#version 450
// vkclean -- WBOIT RESOLVE TILES (#1410; clean_oit_tiles.comp lists them, llcleanalphaoitvk.cpp
// draws them): one 8 x 8 pixel quad per touched tile (instance), clamped to the targets' extent,
// in place of the resolve's fullscreen triangle. The resolve FS reads only gl_FragCoord.
layout(set = 2, binding = 4, r16f) uniform readonly image2DArray uOitReveal;   // (#1608: a layer per view; the size)
layout(std430, set = 2, binding = 6) readonly buffer OitTiles { uint cmd[4]; uint tiles[]; };

void main()
{
    const uint t = tiles[gl_InstanceIndex];
    const vec2 size = vec2(imageSize(uOitReveal).xy);
    // two triangles: (0,0) (1,0) (0,1) / (0,1) (1,0) (1,1)
    const uint v = uint(gl_VertexIndex);
    const vec2 corner = vec2(v == 1u || v == 4u || v == 5u ? 1.0 : 0.0, v == 2u || v == 3u || v == 5u ? 1.0 : 0.0);
    const vec2 p = min((vec2(t & 0xFFFFu, t >> 16u) + corner) * 8.0, size);
    gl_Position = vec4(p / size * 2.0 - 1.0, 0.0, 1.0);
}
