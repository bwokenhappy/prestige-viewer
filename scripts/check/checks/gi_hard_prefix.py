"""Execute the GLSL sphere predicates as C++ against the real host publisher.

Compare hard-only prefix lookup, all-sphere strength, and segment invalidation with
unpartitioned queries for empty/soft/hard/mixed lists, padding and global overflow.
"""
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

NAME = 'gi-hard-prefix'
STAGE = 'source'
DESCRIPTION = 'optional GI hard prefix preserves hard rejection, soft strength and segment invalidation'
GUARDS = 'Soft spheres must never cause hard rejection; metadata must never become overflow; hard changes cannot be lost.'


def function(text, name):
    m = re.search(r'(?:bool|uint) '+name+r'\([^)]*\)\s*\{', text)
    if not m:
        raise ValueError('missing shader function '+name)
    end = m.end(); depth = 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}'); end += 1
    return text[m.start():end].replace('out float strength','float& strength').replace('.xyz','.xyz()')


def run(ctx, result):
    shader = Path(ctx.path('indra/vkclean/shaders/clean_gi_common.glsl')).read_text()
    functions = '\n'.join(function(shader,n) for n in ('giSphereHitS','giSphereHit','giSceneChanged','giSegmentChanged'))
    compiler = os.environ.get('CXX') or shutil.which('c++')
    if not compiler:
        result.fail('C++ compiler required for sphere semantic equivalence'); return
    prelude = r'''
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>
#include "llcleanchangespheres.h"
using uint = uint32_t;
using std::min; using std::max; using std::abs;
#define GIR_PREC
#define GI_SPHERES_MAX 64u
struct vec3 { float x,y,z; };
struct vec4 { float x,y,z,w; vec3 xyz() const { return {x,y,z}; } };
struct uvec4 { uint x,y,z,w; };
vec3 operator-(vec3 a,vec3 b){return {a.x-b.x,a.y-b.y,a.z-b.z};}
vec3 operator+(vec3 a,vec3 b){return {a.x+b.x,a.y+b.y,a.z+b.z};}
vec3 operator*(vec3 a,float b){return {a.x*b,a.y*b,a.z*b};}
float dot(vec3 a,vec3 b){return a.x*b.x+a.y*b.y+a.z*b.z;}
float clamp(float v,float a,float b){return min(max(v,a),b);}
struct Header { uvec4 sph_n{}; vec4 sph[64]{}; } gi;
'''
    # GLSL's unadorned decimal literals are floats; preserve that in the C++ fixture.
    functions = re.sub(r'(?<![\w.])(\d+\.\d*(?:e[-+]?\d+)?|\d+e[-+]?\d+)(?![\w.])',r'\1f',functions)
    fixture = r'''
int main() {
    std::mt19937 rng(1604);
    std::uniform_real_distribution<float> pos(-256.f,256.f), rad(.01f,256.f);
    uint cases=0;
    for(uint n=0;n<=64;++n) for(uint mode=0;mode<4;++mode) {
        std::array<LLCleanChangeSpheres::Sphere,64> src{};
        Header before{}; before.sph_n.x=n;
        for(uint i=0;i<n;++i) {
            const float sign=mode==0 ? -1.f : mode==1 ? 1.f : (rng()&1u ? 1.f:-1.f);
            float xyz[3]={pos(rng),pos(rng),pos(rng)};
            const float radius=mode==3 && i%7==0 ? -0.f : sign*rad(rng);
            src[i]=LLCleanChangeSpheres::make(xyz,radius);
            before.sph[i]={xyz[0],xyz[1],xyz[2],radius};
        }
        const auto saved=src;
        float packed[64][4]{};
        uint hard=LLCleanChangeSpheres::writeHardFirst(src.data(),n,packed);
        if(std::memcmp(src.data(),saved.data(),sizeof(src))) return 1;
        Header after{};after.sph_n.x=n;
        std::memcpy(after.sph,packed,sizeof(packed));
        uint expected=0;
        for(uint i=0;i<n;++i) expected+=src[i].w>=0.f;
        if(hard!=expected) return 2;
        // Check every byte survives, with stable relative ordering within each class.
        uint out=0;
        for(uint kind=0;kind<2;++kind)for(uint i=0;i<n;++i) {
            if((src[i].w<0.f)!=(kind!=0))continue;
            if(std::memcmp(&before.sph[i],&after.sph[out++],sizeof(vec4)))return 3;
        }
        for(uint ov=0;ov<2;++ov) for(uint q=0;q<256;++q) {
            before.sph_n.y=ov;after.sph_n.y=ov|((hard+1u)<<1u);
            vec3 p{pos(rng),pos(rng),pos(rng)},b{pos(rng),pos(rng),pos(rng)};
            if(n && q<n){ p=before.sph[q].xyz(); if(q&1u)p.x+=abs(before.sph[q].w); }
            float padding=q%3==0 ? 0.f : q%3==1 ? .87f : 12.f;
            gi=before;float oldStrength;uint oldHit=giSphereHitS(p,padding,oldStrength);
            bool oldSegment=giSegmentChanged(p,b);
            gi=after;float newStrength;uint newHit=giSphereHitS(p,padding,newStrength);
            if(giSceneChanged(p,padding)!=(oldHit==2u) || newHit!=oldHit ||
               (oldHit!=2u && oldStrength!=newStrength) || giSegmentChanged(p,b)!=oldSegment) {
                std::fprintf(stderr,"mismatch n=%u mode=%u overflow=%u query=%u\n",n,mode,ov,q);return 4;
            }
            ++cases;
        }
    }
    std::printf("%u shader/host equivalence cases passed\n",cases);
}
'''
    with tempfile.TemporaryDirectory() as tmp:
        source=Path(tmp)/'sphere.cpp';exe=Path(tmp)/'sphere'
        source.write_text(prelude+functions+fixture)
        build=subprocess.run([compiler,'-std=c++17','-O2','-Wall','-Wextra','-I',ctx.path('indra/newview'),str(source),'-o',str(exe)],capture_output=True,text=True)
        if build.returncode:result.fail(build.stdout+build.stderr);return
        check=subprocess.run([str(exe)],capture_output=True,text=True)
        if check.returncode:result.fail(check.stdout+check.stderr+' exit='+str(check.returncode));return
    result.summary=check.stdout.strip()
