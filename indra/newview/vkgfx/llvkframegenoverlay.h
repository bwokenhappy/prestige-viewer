// Frame generation world overlays: current jitter in BOTH clips, so the UV
// difference has the same unjittered prev-current convention as FSR/DLSS inputs.
#pragma once
#include <array>
#include <cmath>
#include <cstddef>
namespace llfgov {
// Previous clip Z is not used. Three row vectors leave room for texture-gradient
// scale, so guide alpha coverage matches display resolution without another UBO.
struct Clips {
    std::array<float,16> current;
    std::array<std::array<float,4>,3> previous; // x, y, w rows
    std::array<float,4> parameters;             // render/display x,y; alpha threshold; reserved
};
static_assert(sizeof(Clips)==128 && offsetof(Clips,parameters)==112,
              "overlay guide push fits Vulkan's minimum limit");
inline bool finite(const float* matrix, float jx, float jy) noexcept {
    if (!std::isfinite(jx) || !std::isfinite(jy)) return false;
    for (unsigned i=0;i<16;++i) if (!std::isfinite(matrix[i])) return false;
    return true;
}
inline Clips clips(const float* current, const float* reprojection, float jx, float jy,
                   float scaleX=1.0f, float scaleY=1.0f, float minAlpha=0.05f) noexcept {
    Clips out{};
    for (unsigned col=0;col<4;++col) {
        for (unsigned row=0;row<4;++row) out.current[col*4+row]=current[col*4+row];
        for (unsigned row=0;row<3;++row)
            for (unsigned k=0;k<4;++k)
                out.previous[row][col]+=reprojection[k*4+(row==2 ? 3 : row)]*current[col*4+k];
        out.current[col*4]+=jx*out.current[col*4+3];
        out.current[col*4+1]+=jy*out.current[col*4+3];
        out.previous[0][col]+=jx*out.previous[2][col];
        out.previous[1][col]+=jy*out.previous[2][col];
    }
    out.parameters[0]=scaleX; out.parameters[1]=scaleY; out.parameters[2]=minAlpha;
    return out;
}
} // namespace llfgov
