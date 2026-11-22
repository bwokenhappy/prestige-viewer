#version 450
// vkclean -- DISPLAY DEPTH for a scaled frame (render scale, #181). The selection
// outline, LL's 3D overlays and the HUD pass run at the display resolution and depth-test
// against this. The world depth is render-sized AND jittered (TAA); a plain nearest
// upscale moves every depth edge by the jitter each frame (up to display / render pixels),
// so the overlays' hidden parts flickered. Here each display pixel takes the depth of
// the render pixel that sampled its UNJITTERED centre: render pixel q shows the scene at
// q + 0.5 - jitter, so the one covering render position pr is floor(pr + jitter).
// A fullscreen depth-only draw (VS: clean_glow_combine.vert; depth test ALWAYS, write on)
// into the display depth: the same texel per pixel as the old clean_depth_up.comp, whose
// display-sized storage buffer + copy it replaces (D32: the value is stored unchanged).

layout(set = 0, binding = 0) uniform sampler2D uDepth;              // world depth (texelFetch)

layout(push_constant) uniform Push {
    ivec2 dsize;    // display extent
    ivec2 rsize;    // render extent
    vec2  ratio;    // render / display
    vec2  jitter;   // this frame's jitter, render pixels (x right, y down)
} pc;

void main()
{
    const ivec2 p = ivec2(gl_FragCoord.xy);
    const vec2  pr = (vec2(p) + 0.5) * pc.ratio;
    const ivec2 q  = clamp(ivec2(floor(pr + pc.jitter)), ivec2(0), pc.rsize - 1);
    gl_FragDepth = texelFetch(uDepth, q, 0).r;
}
