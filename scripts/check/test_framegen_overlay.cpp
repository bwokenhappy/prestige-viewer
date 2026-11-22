#include "llvkframegenoverlay.h"
#include <cassert>
#include <cstdio>
#include <limits>
using Matrix=std::array<float,16>;
Matrix identity() { Matrix m{}; m[0]=m[5]=m[10]=m[15]=1; return m; }
std::array<float,4> point(const Matrix& m, const std::array<float,4>& p) {
    std::array<float,4> out{};
    for (unsigned r=0;r<4;++r) for(unsigned c=0;c<4;++c) out[r]+=m[c*4+r]*p[c];
    return out;
}
std::array<float,4> prior(const llfgov::Clips& c,const std::array<float,4>& p) {
    std::array<float,4> out{};
    for (unsigned col=0;col<4;++col) {
        out[0]+=c.previous[0][col]*p[col]; out[1]+=c.previous[1][col]*p[col];
        out[3]+=c.previous[2][col]*p[col];
    }
    return out;
}
void close(float a,float b) { assert(std::abs(a-b)<2e-6f); }
int main() {
    unsigned checks=0;
    for (unsigned width : {853u,1920u,3840u})
     for (float distance : {2.f,5.f,20.f,200.f})
      for (float jitter : {-0.4f,0.f,0.4f})
        for (float camera : {-0.1f,0.f,0.1f}) {
            Matrix projection=identity(); projection[10]=0; projection[14]=0.1f; projection[11]=1; projection[15]=0;
            Matrix reproject=identity(); reproject[8]=camera/0.1f;
            auto c=llfgov::clips(projection.data(),reproject.data(),2*jitter/float(width),-2*jitter/(float(width)*9.f/16.f));
            auto p=point(c.current,{0.2f,0.1f,distance,1.f});
            auto prev=prior(c,{0.2f,0.1f,distance,1.f});
            close((prev[0]/prev[3]-p[0]/p[3])*0.5f,camera/(2*distance));
            close((prev[1]/prev[3]-p[1]/p[3])*0.5f,0.f);
            // A floating wall's camera translation must use ITS depth, not the
            // farther floor's. Translation is in world/view space before projection.
            Matrix world=projection; world[12]+=camera;
            auto old=point(world,{0.2f,0.1f,distance,1.f});
            close((old[0]/old[3]-(0.2f/distance))*0.5f,camera/(2*distance));
            if (camera!=0.0f) {
                const float backgroundMotion=camera/(4*distance);
                assert(std::abs((prev[0]/prev[3]-p[0]/p[3])*0.5f-backgroundMotion)>1e-6f);
            }
            ++checks;
        }
    Matrix m=identity();
    const auto reduced=llfgov::clips(m.data(),m.data(),0,0,1.f/3.f,1.f/3.f);
    close(reduced.parameters[0],1.f/3.f); close(reduced.parameters[1],1.f/3.f);
    assert(llfgov::finite(m.data(),0,0));
    m[3]=std::numeric_limits<float>::quiet_NaN(); assert(!llfgov::finite(m.data(),0,0));
    m=identity(); assert(!llfgov::finite(m.data(),std::numeric_limits<float>::infinity(),0));
    std::printf("Framegen overlay clip/jitter/depth fixtures: %u PASS\n",checks);
}
