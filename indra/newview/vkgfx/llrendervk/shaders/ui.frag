#version 450
// LLRenderVK immediate-mode fragment shader.
//
// A single shader serves both textured and untextured batches. Untextured
// batches bind a 1x1 white texture, so `texture()*vColor == vColor`. This keeps
// the pipeline count down and mirrors gGL's "vertex color modulates the bound
// texture, and the default bind is white" behavior exactly.

layout(set = 0, binding = 0) uniform sampler2D uTex;

layout(location = 0) in vec4 vColor;
layout(location = 1) in vec2 vUV;

layout(location = 0) out vec4 outColor;

// HDR UI layer (#1451): a draw with blending off (BT_REPLACE) covers the world under it, so its
// coverage is 1 whatever its alpha. Off everywhere else (vk_pipeline_ui.h uiLayerOpaque).
layout(constant_id = 0) const bool kUiLayerOpaque = false;

void main() {
    vec4 c = texture(uTex, vUV) * vColor;
    outColor = vec4(c.rgb, kUiLayerOpaque ? 1.0 : c.a);
}
