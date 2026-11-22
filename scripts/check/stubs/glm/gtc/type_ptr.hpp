// Host-test stand-in for glm/gtc/type_ptr.hpp: see ../glm.hpp.
#pragma once
#include "../glm.hpp"

namespace glm
{
inline mat4 make_mat4(const float* p)
{
    mat4 m;
    float* o = &m.c[0].v[0];
    for (int i = 0; i < 16; ++i) o[i] = p[i];
    return m;
}
inline const float* value_ptr(const mat4& m) { return &m.c[0].v[0]; }
} // namespace glm
