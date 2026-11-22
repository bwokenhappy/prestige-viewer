#version 450
// Both clips have this frame's SAME jitter. It cancels in prev_uv-current_uv,
// matching clean_fsr_inputs.comp; previous-frame jitter must not be added here.
layout(push_constant) uniform Push {
    mat4 current;
    vec4 previousX, previousY, previousW;
    vec4 parameters;
} pc;
layout(location=0) in vec3 aPos;
layout(location=1) in vec4 aColor;
layout(location=2) in vec2 aUV;
layout(location=0) out vec4 vColor;
layout(location=1) out vec2 vUV;
layout(location=2) out vec4 vCurrent;
layout(location=3) out vec4 vPrevious;
layout(location=4) flat out vec2 vTexelScale;
layout(location=5) flat out float vMinAlpha;
void main() {
    vColor=aColor; vUV=aUV;
    vCurrent=pc.current*vec4(aPos,1.0);
    vec4 p=vec4(aPos,1.0);
    vPrevious=vec4(dot(pc.previousX,p),dot(pc.previousY,p),0.0,dot(pc.previousW,p));
    vTexelScale=pc.parameters.xy; vMinAlpha=pc.parameters.z;
    gl_Position=vCurrent;
}
