// RenderVKGIDebug 11 legend fixture (scripts/check/checks/gi_lamp_why.py): indra/vkclean/shaders/clean_gi_lamp_why.h
// included as-is. Every reason has its own colour, a ray without GLW_PASSED shows nothing, the priority is
// FIX > SELF > EMIT > FACING > ALPHA > CLEAN, and the bits fit the queue's result word (w >> 8, below VQ_RES_*).
#include <cstdio>
#include <cstdint>

typedef uint32_t uint;
struct vec3
{
    float x, y, z;
    vec3(float a, float b, float c) : x(a), y(b), z(c) {}
    explicit vec3(float a) : x(a), y(a), z(a) {}
};
static vec3 operator*(vec3 a, float s) { return vec3(a.x * s, a.y * s, a.z * s); }
static bool eq(vec3 a, vec3 b) { return a.x == b.x && a.y == b.y && a.z == b.z; }

#include "clean_gi_lamp_why.h"

static int g_fail = 0;
#define CHECK(c, m) do { if (!(c)) { std::printf("FAIL: %s\n", m); ++g_fail; } } while (0)

int main()
{
    const uint reasons[6] = {0u, GLW_ALPHA, GLW_FACING, GLW_EMIT, GLW_SELF, GLW_FIX};
    vec3 col[6] = {vec3(0.f), vec3(0.f), vec3(0.f), vec3(0.f), vec3(0.f), vec3(0.f)};
    for (int i = 0; i < 6; ++i)
    {
        col[i] = giLampWhyColour(reasons[i] | GLW_PASSED);
        CHECK(!eq(col[i], vec3(0.f)), "a passed ray must show a colour");
        CHECK(eq(giLampWhyColour(reasons[i]), vec3(0.f)), "a ray without GLW_PASSED must show nothing");
    }
    for (int i = 0; i < 6; ++i)
        for (int j = i + 1; j < 6; ++j)
            CHECK(!eq(col[i], col[j]), "every reason needs its own colour");
    // priority: each reason wins over every lower one
    for (int i = 1; i < 6; ++i)
        for (int j = 0; j < i; ++j)
            CHECK(eq(giLampWhyColour(reasons[i] | reasons[j] | GLW_PASSED), col[i]), "priority FIX > SELF > EMIT > FACING > ALPHA > CLEAN");
    CHECK((GLW_MASK & (GLW_PASSED | GLW_FIX | GLW_ALPHA | GLW_FACING | GLW_SELF | GLW_EMIT)) ==
              (GLW_PASSED | GLW_FIX | GLW_ALPHA | GLW_FACING | GLW_SELF | GLW_EMIT), "GLW_MASK covers every bit");
    CHECK((GLW_MASK << 8u) >> 8u == GLW_MASK && GLW_MASK < (1u << 24), "the bits fit the queue result word above VQ_RES_*");
    // giLampSpeckColour (view 11): an outlier at full intensity, everything else black or faint
    const vec3 red(1.f, 0.f, 0.f), green(0.f, 1.f, 0.f);
    for (int i = 0; i < 6; ++i)
    {
        const uint why = reasons[i] | GLW_PASSED;
        CHECK(eq(giLampSpeckColour(true, 0u, why), col[i]), "a fresh outlier with lamp light shows its reason at full");
        CHECK(eq(giLampSpeckColour(true, 1u, why), green) && eq(giLampSpeckColour(true, 2u, why), green),
              "a reused outlier shows green, whatever its texel's fresh hit did");
        const vec3 f = giLampSpeckColour(false, 0u, why);
        CHECK(f.x <= 0.2f && f.y <= 0.2f && f.z <= 0.2f && !eq(f, vec3(0.f)), "a non-outlier lamp texel is faint");
        CHECK(!eq(col[i], red) && !eq(col[i], green), "the reason colours stay apart from red / green");
    }
    CHECK(eq(giLampSpeckColour(true, 0u, 0u), red), "a fresh outlier without lamp light shows red");
    CHECK(eq(giLampSpeckColour(false, 0u, 0u), vec3(0.f)) && eq(giLampSpeckColour(false, 2u, 0u), vec3(0.f)),
          "everything else is black");
    if (g_fail) { std::printf("%d failure(s)\n", g_fail); return 1; }
    std::printf("ok\n");
    return 0;
}
