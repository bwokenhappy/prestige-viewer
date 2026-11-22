#version 450
#extension GL_GOOGLE_include_directive : require
// Final display transfer. uLinearNits: the tone-mapped world, Rec.709 absolute linear
// nits. uUiLayer (HDR, #1451): the UI, HUDs and overlays as the SDR pipelines drew them into
// a transparent RGBA8 layer (premultiplied sRGB-encoded colour, coverage in alpha),
// composited here at paper white; one pass, no later UI writes. SDR capture: WORLD ONLY.
// Never bind SDR-encoded scene/UI texels to uLinearNits.
layout(location=0) in vec2 v_uv;
layout(location=0) out vec4 outColor;
// Frame generation (#1517, HDR10 only; the pipeline with three attachments): the world alone in the
// output's encoding (what the generator gets), and the UI layer's own texel (the slot's copy of uUiLayer:
// premultiplied sRGB-encoded colour + coverage, nothing of this frame's world in it), which every generated
// frame composes over its own world as this pass does (encoding 4). Pipelines with one attachment discard them.
layout(location=1) out vec4 outHudless;
layout(location=2) out vec4 outUi;
layout(set=0,binding=0) uniform sampler2D uLinearNits;
layout(set=0,binding=1) uniform sampler2D uUiLayer;
// #1517 Task 7e: the slot's label atlas (name tags, chat bubbles, hover text; same encoding as uUiLayer)
// and, binding 3 (clean_display_labels.glsl), where each label is and how far it moved: drawn between the
// world and the interface. Read only with the 0x200 flag; otherwise valid stand-ins are bound, never read.
layout(set=0,binding=2) uniform sampler2D uLabelAtlas;
layout(push_constant) uniform DisplayOutput {
    uint encoding; // 0: SDR capture, 1: Rec.2020/PQ HDR10, 2: scRGB, 3: composited nits (snapshot source),
                   // 4: an encoded world frame + the UI layer (frame generation's composite);
                   // 5: an SDR world frame + the UI layer (#1536 SDR frame generation's composite: every
                   //    picture, the real one too; sRGB-encoded, no transfer math, no dither);
                   // + 0x100: the world is SDR-encoded (an SDR snapshot frame, #1451 G5)
                   // + 0x200: moving labels from uLabelAtlas, between the world and the interface (#1517 Task 7e)
    float paperWhiteNits;
    float peakNits;
    float dither;
    float labelPhase;  // encodings 4 and 5 with 0x200: this picture's moment, k / (n + 1) (1: the real frame)
                       // (encoding 4 reads paperWhiteNits and peakNits too: the real frame's; 5 neither)
} display;
#include "clean_display_common.glsl"
vec3 pq(vec3 nits) {
    vec3 y=pow(clamp(nits/10000.0,0.0,1.0),vec3(2610.0/16384.0));
    return pow((3424.0/4096.0+(2413.0/128.0)*y)/(1.0+(2392.0/128.0)*y),vec3(2523.0/32.0));
}
#include "clean_display_labels.glsl"
float outputNoise() {
    vec2 p=gl_FragCoord.xy;
    float a=fract(52.9829189*fract(dot(p,vec2(.06711056,.00583715))));
    float b=fract(52.9829189*fract(dot(p+vec2(1.618,2.414),vec2(.06711056,.00583715))));
    return a+b-1.0;
}
void main() {
    const uint encoding=display.encoding&0xFFu;
    if(encoding==4u) {
        // Frame generation (#1517): one picture of a batch -- a world frame in the output's encoding
        // (binding 0: a generated one, or the real one) and the slot's copy of the UI layer (binding 1),
        // composed as the real frame is: in nits at paper white, with 0x200 the labels between them, where
        // they were at this frame's moment (#1517 Task 7e). Pixels with neither keep the world texel.
        const vec3 world=texture(uLinearNits,v_uv).rgb;
        outColor=vec4(composeOverEncoded(world,texture(uUiLayer,v_uv),(display.encoding&0x200u)!=0u,
                                         ivec2(gl_FragCoord.xy),display.labelPhase,
                                         display.paperWhiteNits,display.peakNits),1.0);
        outHudless=vec4(0.0);
        outUi=vec4(0.0);
        return;
    }
    if(encoding==5u) {
        // #1536 SDR frame generation: the world (binding 0: the slot's SDR world, or a generated one) with the
        // labels at this picture's moment and the slot's UI layer (binding 1) over it, as the SDR swapchain
        // blends -- the same texels in the real picture and in every generated one.
        outColor=vec4(composeOverSdr(texture(uLinearNits,v_uv).rgb,texture(uUiLayer,v_uv),
                                     (display.encoding&0x200u)!=0u,ivec2(gl_FragCoord.xy),display.labelPhase),1.0);
        outHudless=vec4(0.0);
        outUi=vec4(0.0);
        return;
    }
    const bool sdrWorld=(display.encoding&0x100u)!=0u;
    vec3 nits=texture(uLinearNits,v_uv).rgb;
    if(any(isnan(nits))||any(isinf(nits))) nits=vec3(0.0);
    if(sdrWorld) nits=displaySdrWorldNits(nits,display.paperWhiteNits);
    float peak=clamp(display.peakNits,80.0,10000.0);
    nits=clamp(nits,0.0,peak);
    const vec3 worldNits=nits;   // (frame generation: the world alone, what the generator gets)
    // #1517 Task 7e: the moving labels at this real frame's moment (t = 1), between world and interface.
    vec3 labeled=worldNits;
    if(encoding!=0u&&(display.encoding&0x200u)!=0u)
        labeled=clamp(composeLabels(worldNits,ivec2(gl_FragCoord.xy),1.0,display.paperWhiteNits,false),0.0,peak);
    if(encoding!=0u)
        nits=clamp(displayComposeUi(labeled,texture(uUiLayer,v_uv),display.paperWhiteNits),0.0,peak);
    outHudless=vec4(0.0);
    outUi=vec4(0.0);
    if(encoding==3u) {
        // The snapshot source: nits, or (an SDR snapshot frame) the SDR-encoded composite.
        float white=clamp(display.paperWhiteNits,80.0,kDisplayReferenceWhiteMaxNits);
        outColor=vec4(sdrWorld ? encodeExt(nits/white) : nits,1.0);
    } else if(encoding==1u) {
        // Column-major Rec.709 -> Rec.2020, D65. Transfer is applied exactly once.
        mat3 to2020=mat3(.627404,.069097,.016391,
                         .329283,.919540,.088013,
                         .043313,.011362,.895595);
        vec3 encoded=pq(to2020*nits);
        vec3 hudless=pq(to2020*worldNits);
        if(display.dither>0.0) {
            const float n=outputNoise()/1023.0;
            encoded+=n; hudless+=n;
        }
        encoded=clamp(encoded,0.0,1.0);
        hudless=clamp(hudless,0.0,1.0);
        outColor=vec4(encoded,1.0);
        outHudless=vec4(hudless,1.0);
        // The interface layer for the generated frames: this frame's UI layer as it is (the labels are drawn
        // between world and interface in every picture, never carried in it).
        outUi=texture(uUiLayer,v_uv);
    } else if(encoding==2u) {
        outColor=vec4(nits/80.0,1.0);
    } else {
        // Dedicated world-only SDR capture target, never tone-map the UI layer.
        vec3 encoded=displayLinearToSrgb(displayNitsToSdrLinear(nits,display.paperWhiteNits,display.peakNits));
        if(display.dither>0.0) encoded+=outputNoise()/255.0;
        outColor=vec4(clamp(encoded,0.0,1.0),1.0);
    }
}
