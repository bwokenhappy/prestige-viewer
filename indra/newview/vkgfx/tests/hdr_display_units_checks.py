#!/usr/bin/env python3
"""Compile extracted production HDR tone/final helpers and test their unit contract.

Requires a real GLM include directory. No viewer link or GPU. The fixture replaces
only external grading (disabled here) and GLSL vector syntax with GLM equivalents.
"""
import argparse
from pathlib import Path
import re
import subprocess

p = argparse.ArgumentParser()
p.add_argument('--out', type=Path, required=True)
p.add_argument('--source-root', type=Path, default=Path(__file__).resolve().parents[4])
p.add_argument('--glm', type=Path, required=True)
a = p.parse_args()
root = a.source_root
tone = (root / 'indra/vkclean/shaders/clean_tonemap.frag').read_text()
final = (root / 'indra/vkclean/shaders/clean_final.frag').read_text()
common = (root / 'indra/vkclean/shaders/clean_display_common.glsl').read_text()
sphere = (root / 'indra/vkclean/shaders/clean_rlv_sphere.frag').read_text()

def function(source, name):
    m = re.search(r'^(?:vec3|float) ' + name + r'\([^\n]*\)\s*\{', source, re.M)
    assert m, name
    end = source.index('{', m.start()) + 1
    depth = 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[m.start():end]

def cpp(source):
    # GLSL floating literals are floats; unsuffixed C++ literals are doubles.
    return re.sub(r'(?<![\w.])(\d*\.\d+(?:e[-+]?\d+)?|\d+e[-+]?\d+)(?![\w.])', r'\1f', source)

statement = re.findall(r'if\(kDisplayLinear && pc\.p4\.w>0\.0\) c=([^;]+);', tone)[-1]
last = final[final.rindex('    vec3 display='):final.rindex('    outColor=')]
last = last.replace('diff.rgb', 'relative')
code = r'''
#include <glm/glm.hpp>
#include <cassert>
#include <cmath>
#include <initializer_list>
using namespace glm;
struct { vec4 p1{0},p4{0},p5{0}; } pc;
struct { vec4 misc{0}; } grade;
struct { vec4 res_frame{0}, dither{0}; } fx;
bool kDisplayLinear=true;
const vec3 CG_LUMA(.2126f,.7152f,.0722f);
const float kDisplayReferenceWhiteMaxNits=1000.f; // clean_display_common.glsl
vec3 gradeLinear(vec3 v) { return v; } // grading disabled in these cases
'''
code += cpp(function(tone, 'displayDecodeSrgb')) + '\n'
code += cpp(function(tone, 'hdrHeadroomWeight')) + '\n'
code += cpp(function(tone, 'displayHeadroom')) + '\n'
code += cpp(function(final, 'displayRange')) + '\n'
code += cpp(function(common, 'encodeExt')) + '\n'
code += cpp(function(common, 'decodeExt')) + '\n'
code += cpp(function(common, 'displayComposeUi')) + '\n'
code += cpp(function(common, 'displaySdrWorldNits')) + '\n'
# #1451: the RLVa sphere's Blend and the Lightbox look effects run in the SDR encoding with HDR on.
code += 'bool gSphereLinear=false; bool sphereLinear() { return gSphereLinear; }\n'
code += cpp(function(sphere, 'sphereBlend')) + '\n'
code += 'bool gLookOn=true; bool lookOn() { return gLookOn; }\n'
code += ('// A stand-in for the effects: non-linear in the value, as vignette, grain and CVD are.\n'
         'vec3 applyLook(vec3 c,vec2 uv,vec2) { return c*(.35f+.5f*uv.x)+vec3(.03f,-.02f,.01f)*uv.y+c*c*.1f; }\n')
code += cpp(function(final, 'finalLook')) + '\n'
code += 'vec3 toneOutput(vec3 c,vec3 scene) { vec3 c_scene=scene; if(kDisplayLinear && pc.p4.w>0.f) c=' + cpp(statement) + '; return c; }\n'
code += 'vec3 finalOutput(vec3 relative) {\n' + cpp(last) + '\nreturn display; }\n'
code += r'''
bool closeColour(vec3 a,vec3 b,float e=.002f) { return length(a-b)<e; }
void setup(float white,float peak) {
 pc.p4.w=peak/white;pc.p5.x=white;pc.p5.y=6;
 fx.res_frame.w=white;fx.dither.w=peak/white;
}
float lumaOf(vec3 c) { return dot(c,CG_LUMA); }
// #1451 G3: share of the room above paper white that a grey scene of exposed luma L adds
// (the tone curve's white point, p5.y, is 6 in setup).
float shoulderGain(vec3 encoded,float L,float ratio) {
 return (lumaOf(toneOutput(encoded,vec3(L)))-lumaOf(displayDecodeSrgb(encoded)))/(ratio-1.f);
}
void weightChecks() {
 // g(L): 0 to diffuse white, flat there, >= 0.9 at the tone curve's white point W, monotonic.
 for(float W:{2.f,6.f,16.f}) {
  assert(hdrHeadroomWeight(1.f,W)==0.f && hdrHeadroomWeight(.5f,W)==0.f);
  assert(hdrHeadroomWeight(1.f+1e-3f,W)/1e-3f<.01f);
  assert(hdrHeadroomWeight(W,W)>=.9f);
  float previous=0.f;
  for(float L=1.f;L<=64.f;L+=1.f/64.f) {
   const float g=hdrHeadroomWeight(L,W);
   assert(g+1e-7f>=previous && g<=1.f);previous=g;
  }
 }
}
void shoulderChecks() {
 const vec3 grey(.5f,.5f,.5f),colour(.46f,.32f,.2f);
 for(float ratio:{1.f,1.25f,5.7f,12.f}) {   // 1.25: a 400-nit panel at a 320-nit white
  setup(80.f,80.f*ratio);
  for(vec3 encoded:{grey,colour}) {
   const vec3 base=displayDecodeSrgb(encoded);
   assert(closeColour(toneOutput(encoded,vec3(1.f)),base,1e-6f));
   // Continuous where the SDR curve hands over to the headroom.
   assert(std::abs(lumaOf(toneOutput(encoded,vec3(1.f+1e-4f)))-lumaOf(toneOutput(encoded,vec3(1.f-1e-4f))))<1e-3f);
   float previous=lumaOf(base);
   for(float L=1.f;L<=64.f;L+=1.f/64.f) {
    const vec3 out=toneOutput(encoded,vec3(L));
    // Never inverts (relative tolerance: the per-channel peak bound plateaus in float noise).
    assert(lumaOf(out)>=previous*(1.f-1e-6f)-1e-6f);previous=lumaOf(out);
    assert(max(out.r,max(out.g,out.b))<=ratio+1e-5f);          // never past the panel's peak
    if(ratio==1.f) assert(closeColour(out,base,1e-6f));          // no headroom: the SDR result
   }
  }
  if(ratio>1.f) {
   assert(shoulderGain(grey,1.f+1e-3f,ratio)/1e-3f<.01f);   // flat at diffuse white: no ring in a gradient
   assert(shoulderGain(grey,6.f,ratio)>=.9f);              // most of the headroom by the curve's white point
  }
 }
}
// #1451 G1: the UI layer (premultiplied sRGB-encoded colour, coverage) over the world at paper
// white, blended in the sRGB curve continued past 1.
vec3 srgbEncode(vec3 x) { // the SDR swapchain's transfer, x in [0, 1]
 return mix(1.055f*pow(x,vec3(1.f/2.4f))-.055f,12.92f*x,lessThanEqual(x,vec3(.0031308f)));
}
void compositeChecks() {
 float previous=-1.f;
 for(float x=0.f;x<=12.f;x+=1.f/256.f) {
  const vec3 e=encodeExt(vec3(x));
  assert(std::abs(decodeExt(e).r-x)<=1e-5f*max(1.f,x));   // round trip
  assert(e.r>previous);previous=e.r;                        // monotonic
  if(x<=1.f) assert(std::abs(e.r-srgbEncode(vec3(x)).r)<1e-6f); // the sRGB curve itself up to white
 }
 for(float x=-1.f;x<0.f;x+=1.f/64.f) assert(encodeExt(vec3(x)).r<0.f && std::abs(decodeExt(encodeExt(vec3(x))).r-x)<1e-5f);
 const float white=80.f;
 for(vec4 ui:{vec4(0.f),vec4(.05f,.05f,.05f,.05f),vec4(.1f,.15f,.3f,.6f),vec4(.2f,.2f,.2f,1.f),vec4(.4f,.1f,.0f,.0f)}) {
  for(float x=0.f;x<=1.f;x+=1.f/64.f) for(vec3 world:{vec3(x),vec3(x,x*.5f,x*.25f)}) {
   // SDR: the UI blended over the sRGB-encoded world in the swapchain (clamped at 1).
   const vec3 sdr=min(vec3(ui)+srgbEncode(world)*(1.f-ui.a),vec3(1.f));
   const vec3 hdr=displayComposeUi(world*white,ui,white)/white;
   const vec3 encoded=encodeExt(hdr);
   assert(std::abs(encoded.r-sdr.r)<.5f/255.f && std::abs(encoded.g-sdr.g)<.5f/255.f && std::abs(encoded.b-sdr.b)<.5f/255.f);
  }
  if(ui==vec4(0.f)) for(float x=0.f;x<=12.f;x+=.25f) // an empty layer leaves the world untouched
   assert(closeColour(displayComposeUi(vec3(x*white),ui,white),vec3(x*white),1e-3f*max(1.f,x*white)));
  // Continuous where the world crosses white under the UI, and no clip over a highlight.
  const vec3 below=displayComposeUi(vec3((1.f-1e-4f)*white),ui,white),above=displayComposeUi(vec3((1.f+1e-4f)*white),ui,white);
  assert(length(below-above)<.05f);   // the world itself moves .016 nits across the step
  float last=-1.f;
  for(float x=0.f;x<=12.f;x+=1.f/32.f) {
   const float n=displayComposeUi(vec3(x*white),ui,white).g;
   assert(n+1e-4f>=last);last=n;
   if(ui.a<1.f && x>1.f) assert(n>displayComposeUi(vec3(white),ui,white).g);
  }
 }
}
// #1451 G5: a snapshot frame's world is SDR-encoded; the encode shows it at paper white and the
// shot's composite (written back with encodeExt) is exactly SDR's UI-over-world.
void sdrWorldChecks() {
 for(float white:{80.f,203.f,480.f})
  for(vec4 ui:{vec4(0.f),vec4(.1f,.15f,.3f,.6f),vec4(.4f,.1f,.0f,.0f),vec4(.2f,.2f,.2f,1.f)})
   for(float e=0.f;e<=1.f;e+=1.f/255.f) {
    const vec3 world(e,e*.7f,e*.3f);
    const vec3 nits=displaySdrWorldNits(world,white);
    assert(closeColour(encodeExt(nits/white),world,1e-5f));                       // the SDR value, shown at white
    const vec3 shot=encodeExt(displayComposeUi(nits,ui,white)/white);
    const vec3 sdr=min(vec3(ui)+world*(1.f-ui.a),vec3(1.f));
    assert(std::abs(shot.r-sdr.r)<.5f/255.f && std::abs(shot.g-sdr.g)<.5f/255.f && std::abs(shot.b-sdr.b)<.5f/255.f);
   }
 assert(closeColour(displaySdrWorldNits(vec3(2.f,-1.f,.5f),80.f),vec3(80.f,0.f,displaySdrWorldNits(vec3(.5f),80.f).b),1e-3f));
}
// #1451: RLVa @setsphere Blend (mode 0) mixes the frame toward its colour in the SDR encoding. With
// HDR the frame is relative linear (1 = paper white): the blend must show what SDR shows, the
// partial-strength gradient included, and leave the highlights above white to the strength.
void sphereChecks() {
 for(vec3 colour:{vec3(0.f),vec3(.5f),vec3(.8f,.1f,.1f),vec3(1.f)})
  for(float s=0.f;s<=1.f;s+=1.f/16.f)
   for(float e=0.f;e<=1.f;e+=1.f/64.f) {
    const vec3 frame(e,e*.6f,e*.2f);
    gSphereLinear=false; const vec3 sdr=sphereBlend(frame,colour,s);
    gSphereLinear=true;  const vec3 hdr=sphereBlend(decodeExt(frame),colour,s);
    assert(closeColour(encodeExt(hdr),sdr,1e-4f));
   }
 gSphereLinear=true;
 for(float x:{1.5f,4.f,10.f}) {
  assert(closeColour(sphereBlend(vec3(x),vec3(.3f),0.f),vec3(x),1e-4f*x));   // strength 0: untouched
  assert(closeColour(sphereBlend(vec3(x),vec3(.3f),1.f),decodeExt(vec3(.3f)),1e-5f));
  float last=1e9f;   // toward a darker colour: never brightens, no ring on the way
  for(float s=0.f;s<=1.f;s+=1.f/32.f) { const float g=sphereBlend(vec3(x),vec3(.2f),s).g; assert(g<=last+1e-5f); last=g; }
 }
 gSphereLinear=false;
}
// #1451: the Lightbox look effects (vignette, ADS vignette, CVD compensation, grain, dither, preview)
// were made for the SDR encoding; with HDR they run on the relative-linear frame encoded at paper
// white and decoded after, so the result is SDR's. Nothing is converted while they are all off.
void finalLookChecks() {
 for(float white:{80.f,203.f,480.f}) {
  setup(white,white*4.f);
  for(float e=0.f;e<=1.f;e+=1.f/64.f) for(vec2 uv:{vec2(0.f),vec2(.3f,.9f),vec2(1.f,.5f)}) {
   const vec3 encoded(e,e*.7f,e*.3f);
   kDisplayLinear=false; const vec3 sdr=finalLook(encoded,uv,vec2(0.f));
   kDisplayLinear=true;  const vec3 hdr=finalLook(decodeExt(encoded),uv,vec2(0.f));
   assert(closeColour(encodeExt(hdr),sdr,1e-4f));
   // A snapshot frame under HDR (no paper white in the block) stays SDR.
   fx.res_frame.w=0.f; assert(closeColour(finalLook(encoded,uv,vec2(0.f)),sdr,1e-6f)); fx.res_frame.w=white;
  }
  gLookOn=false;
  for(float x:{0.f,.18f,1.f,3.5f}) assert(finalLook(vec3(x),vec2(.5f),vec2(0.f))==vec3(x));   // off: untouched
  gLookOn=true;
 }
 kDisplayLinear=true;
}
int main() {
 sphereChecks();
 finalLookChecks();
 sdrWorldChecks();
 compositeChecks();
 weightChecks();
 shoulderChecks();
 const vec3 encoded(.46f,.32f,.2f);
 const vec3 base=displayDecodeSrgb(encoded);
 for(float white:{203.f,350.f}) for(float peak:{409.f,1000.f,1350.f}) {
  if(peak<white) continue;
  setup(white,peak);
  // Sane coloured midtones must neither whiten nor acquire white squared.
  vec3 nits=finalOutput(toneOutput(encoded,vec3(.2f)));
  assert(closeColour(nits,base*white));assert(nits.r>nits.g && nits.g>nits.b);
  // No discontinuous jump when the scene first enters HDR headroom.
  vec3 below=toneOutput(encoded,vec3(1.f-.000001f));
  vec3 above=toneOutput(encoded,vec3(1.f+.000001f));
  assert(closeColour(below,above));
  float previous=dot(base,CG_LUMA);
  for(float scene:{1.f,1.01f,2.f,10.f,100.f}) {
   vec3 relative=toneOutput(encoded,vec3(scene));
   float lum=dot(relative,CG_LUMA);
   assert(lum+.00001f>=previous);previous=lum;
   assert(max(relative.r,max(relative.g,relative.b))<=peak/white+.00001f);
   assert(std::abs(relative.r/relative.g-base.r/base.g)<.0001f);
   assert(max(finalOutput(relative).r,max(finalOutput(relative).g,finalOutput(relative).b))<=peak+.01f);
  }
 }
 kDisplayLinear=false;setup(350,409);
 assert(closeColour(toneOutput(encoded,vec3(.2f)),encoded));
 assert(closeColour(finalOutput(encoded),encoded));
}
'''
a.out.mkdir(parents=True, exist_ok=True)
src = a.out / 'hdr_display_units.cpp'
src.write_text(code)
exe = a.out / 'hdr_display_units'
subprocess.run(['c++','-std=c++20','-O2','-Wall','-Wextra','-Werror','-I'+str(a.glm),str(src),'-o',str(exe)],check=True)
subprocess.run([str(exe)],check=True)
print('production HDR display units PASS')
