#version 450
layout(set=0,binding=0) uniform sampler2D uTex;
layout(set=1,binding=0) uniform sampler2D uDepth;
layout(set=2,binding=0) uniform sampler2D uMotion;
layout(constant_id=0) const int kCompare=7; // VkCompareOp, ALWAYS when depth testing is off
layout(location=0) in vec4 vColor;
layout(location=1) in vec2 vUV;
layout(location=2) in vec4 vCurrent;
layout(location=3) in vec4 vPrevious;
layout(location=4) flat in vec2 vTexelScale;
layout(location=5) flat in float vMinAlpha;
layout(location=0) out float outDepth;
layout(location=1) out vec2 outMotion;
void main() {
    // Use the display pass's mip footprint, not the coarser guide viewport's.
    vec4 color=textureGrad(uTex,vUV,dFdx(vUV)*vTexelScale.x,dFdy(vUV)*vTexelScale.y)*vColor;
    float coverage=vMinAlpha<0.0 ? max(max(color.r,color.g),color.b) : color.a;
    if (coverage<abs(vMinAlpha)) discard;
    ivec2 q=clamp(ivec2(gl_FragCoord.xy),ivec2(0),textureSize(uDepth,0)-1);
    float d=texelFetch(uDepth,q,0).r, z=gl_FragCoord.z;
    // Match the facade's raster depth test, including its camera-facing surface
    // bias. The guide images are separate copies: the upscaler's own remain read-only.
    float tolerance=max(1e-9,abs(d)*0.00002);
    if (kCompare==0 || ((kCompare==1 || kCompare==3) && z>d+tolerance) ||
        ((kCompare==4 || kCompare==6) && z<d-tolerance) ||
        (kCompare==2 && abs(z-d)>tolerance) || (kCompare==5 && abs(z-d)<=tolerance)) discard;
    if (abs(vCurrent.w)<1e-8 || vPrevious.w<=1e-8) discard;
    vec2 mv=(vPrevious.xy/vPrevious.w-vCurrent.xy/vCurrent.w)*0.5;
    // Surface-attached selection / physics overlays inherit the object's motion;
    // floating parcel walls and edit axes keep their own camera parallax.
    ivec2 motionPixel=q;
    float best=abs(z-d);
    // A one-pixel wire edge can sit just outside its object's render-size depth
    // footprint. Search only this fixed 3x3 neighbourhood for the matching surface.
    for (int y=-1;y<=1;++y) for (int x=-1;x<=1;++x) {
        if (x==0 && y==0) continue;
        ivec2 tap=clamp(q+ivec2(x,y),ivec2(0),textureSize(uDepth,0)-1);
        float difference=abs(z-texelFetch(uDepth,tap,0).r);
        if (difference<best) { best=difference; motionPixel=tap; }
    }
    if (best<=max(1e-7,abs(z)*0.0001)) mv=texelFetch(uMotion,motionPixel,0).xy;
    if (any(isnan(mv)) || any(isinf(mv)) || isnan(z) || isinf(z)) discard;
    outDepth=z; outMotion=clamp(mv,vec2(-2.0),vec2(2.0));
}
