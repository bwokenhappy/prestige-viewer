#version 450
layout(set=0,binding=0) uniform sampler2D uTex;
layout(push_constant) uniform Push {
    layout(offset=80) float minAlpha;
    float whiteNits; // zero: legacy SDR world bytes; positive: linear HDR world nits
} pc;
layout(location=0) in vec4 vColor;
layout(location=1) in vec2 vUV;
layout(location=0) out vec4 outColor;
vec3 linearize(vec3 c) {
    return mix(c/12.92,pow(max((c+0.055)/1.055,vec3(0.0)),vec3(2.4)),greaterThan(c,vec3(0.04045)));
}
void main() {
    vec4 c=texture(uTex,vUV)*vColor;
    if (any(isnan(c)) || any(isinf(c)) || c.a<pc.minAlpha) discard;
    c.a=clamp(c.a,0.0,1.0);
    if (pc.whiteNits>0.0) c.rgb=linearize(clamp(c.rgb,vec3(0.0),vec3(1.0)))*pc.whiteNits;
    outColor=c;
}
