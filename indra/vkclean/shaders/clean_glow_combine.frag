#version 450
#extension GL_GOOGLE_include_directive : require
// vkclean — glow composite (LL glowcombineF: scene + glow). Drawn with additive
// blending (ONE, ONE) onto the swapchain after the world passes, before HUDs.
layout(constant_id=0) const bool kDisplayLinear=false;
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 outColor;

// (#1608 LAYERED STEREO: a multiview pass -- the blurred glow a layer per view, read at this view's)
#include "clean_view.glsl"
layout(set = 0, binding = 0) uniform sampler2DArray uGlow;

void main()
{
    vec3 glow=texture(uGlow,vec3(v_uv,float(CLEAN_VIEW))).rgb;
    if(kDisplayLinear) glow=mix(pow((max(glow,vec3(0.0))+.055)/1.055,vec3(2.4)),glow/12.92,
                                lessThanEqual(glow,vec3(.04045)));
    outColor=vec4(glow,0.0);
}
