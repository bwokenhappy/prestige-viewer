// Standalone (#1459): g++ -std=c++17 -Wall -Wextra -Werror slposedialmath_test.cpp -o <scratch>/dial_test && <scratch>/dial_test
// Checks the Pose Studio dial math in slposedialmath.h: the HUD generator's forward model has
// no 180-degree flip at the sphere's pole (leg-template bones at Y = +90), the pole nudge
// changes nothing anywhere else, and a dial drag turns the value relative to where it was
// pressed (no jump on click, no pinning below the centre line, Shift = a tenth).
#include "../slposedialmath.h"
#include <cassert>
#include <cmath>
#include <cstdio>
#include <initializer_list>

namespace
{
using namespace SLPoseGen;

// bone_axes.xml settings (Y range in the comment).
Model legModel(double pre_y, double invert_x)
{
    Model m;
    m.axis = 2;
    m.leg = true;
    m.pre[1] = pre_y;
    m.offset[1] = pre_y;
    m.invert[0] = invert_x;
    return m;
}

const Model ELBOW_L = legModel(-90.0, 1.0);     // Y -90..90
const Model SHOULDER_L = legModel(-90.0, -1.0); // Y -110..110
const Model WRIST_L = legModel(-30.0, -1.0);    // Y -20..140
const Model DEFAULT_M;                          // X twist, Y -180..180

double stepDeg(const Model& m, double x0, double y0, double z0, double x1, double y1, double z1)
{
    return angleBetweenDeg(dialToQuat(m, x0, y0, z0), dialToQuat(m, x1, y1, z1));
}

bool sameBits(const HQ& a, const HQ& b)
{
    return a.w == b.w && a.x == b.x && a.y == b.y && a.z == b.z;
}

// Y swept over the range: every 0.25 degree step moves the bone by little more than that.
void checkContinuous(const char* name, const Model& m, double y_min, double y_max)
{
    const double xs[] = { -15.0, 0.0, 15.0 };
    const double zs[] = { -15.0, 0.0, 15.0 };
    for (double x : xs)
    {
        for (double z : zs)
        {
            for (double y = y_min; y < y_max; y += 0.25)
            {
                const double step = stepDeg(m, x, y, z, x, y + 0.25, z);
                if (step > 1.0)
                {
                    printf("FAIL %s: x %.2f z %.2f, y %.2f -> %.2f turns the bone %.2f degrees\n", name, x, y, y + 0.25, z, step);
                    assert(false);
                }
            }
        }
    }
}

bool near(float a, float b)
{
    return std::fabs(a - b) < 1.0e-3f;
}

float deg(float d)
{
    return d * (float)PI_D / 180.f;
}

void checkDialDrag()
{
    using namespace SLPoseDial;
    // Pressing and not moving changes nothing, wherever the press was.
    assert(near(dragValue(37.f, deg(150.f), deg(150.f), -180.f, 180.f, false), 37.f));
    // The upper half ring spans the whole range: on a +/-180 dial a 10 degree clockwise
    // turn (toward the max end, on the right) is +20; counter-clockwise is -20.
    assert(near(dragValue(0.f, deg(90.f), deg(80.f), -180.f, 180.f, false), 20.f));
    assert(near(dragValue(0.f, deg(90.f), deg(100.f), -180.f, 180.f, false), -20.f));
    // A +/-90 dial: 10 degrees of turn is 10.
    assert(near(dragValue(5.f, deg(30.f), deg(20.f), -90.f, 90.f, false), 15.f));
    // Shift: a tenth.
    assert(near(dragValue(0.f, deg(90.f), deg(80.f), -180.f, 180.f, true), 2.f));
    // Below the centre line the turn carries on (no pin to an end): from 170 degrees to -170
    // (past the left end, below the line) is 20 degrees counter-clockwise, -40 on +/-180.
    assert(near(dragValue(-100.f, deg(170.f), deg(-170.f), -180.f, 180.f, false), -140.f));
    // ... and on the right, from 10 to -10 is 20 clockwise.
    assert(near(dragValue(50.f, deg(10.f), deg(-10.f), -180.f, 180.f, false), 90.f));
    // Clamped at the ends, so turning back responds at once.
    const float at_end = dragValue(170.f, deg(90.f), deg(60.f), -180.f, 180.f, false);
    assert(near(at_end, 180.f));
    assert(near(dragValue(at_end, deg(60.f), deg(65.f), -180.f, 180.f, false), 170.f));
    // A drag that starts outside the range (a pose decoded past the dial's end) starts where
    // it is: only turning further outward is stopped, turning back moves at once.
    float lo = 0.f, hi = 0.f;
    dragLimits(200.f, -180.f, 180.f, lo, hi);
    assert(near(lo, -180.f) && near(hi, 200.f));
    assert(near(dragValue(200.f, deg(90.f), deg(85.f), -180.f, 180.f, false, lo, hi), 200.f));  // outward: held
    assert(near(dragValue(200.f, deg(90.f), deg(95.f), -180.f, 180.f, false, lo, hi), 190.f));  // inward: moves
    // The turn's scale stays the dial's own (a +/-15 dial: 10 degrees of turn is 1.667).
    dragLimits(-75.f, -15.f, 15.f, lo, hi);
    assert(near(lo, -75.f) && near(hi, 15.f));
    assert(near(dragValue(-75.f, deg(90.f), deg(80.f), -15.f, 15.f, false, lo, hi), -75.f + 30.f / 18.f));
    assert(near(dragValue(-75.f, deg(90.f), deg(100.f), -15.f, 15.f, false, lo, hi), -75.f));  // outward: held
    // Inside the range the limits are the range.
    dragLimits(10.f, -90.f, 90.f, lo, hi);
    assert(near(lo, -90.f) && near(hi, 90.f));
    // The pointer is drawn at the end stop for a value past it.
    assert(near(shownValue(200.f, -180.f, 180.f), 180.f));
    assert(near(shownValue(-75.f, -15.f, 15.f), -15.f));
    assert(near(shownValue(12.f, -15.f, 15.f), 12.f));

    // The pointer angle: undefined right at the centre (the drag waits there).
    float a = 0.f;
    assert(!pointerAngle(50.f, 18.f, 50.f, 16.f, 4.f, a));
    assert(pointerAngle(50.f, 40.f, 50.f, 16.f, 4.f, a) && near(a, deg(90.f)));
    assert(pointerAngle(20.f, 6.f, 50.f, 16.f, 4.f, a) && a < deg(-90.f));
}
}

int main()
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    // The flip the ticket hit: elbow Y dragged to its end stop (90), or a 5/10/15/45 snap on a
    // shoulder / hip / collar landing on 90.
    printf("elbow Y 89.99 -> 90: %.3f deg\n", stepDeg(ELBOW_L, 0, 89.99, 0, 0, 90, 0));
    printf("shoulder Y 90 -> 90.01: %.3f deg\n", stepDeg(SHOULDER_L, 0, 90, 0, 0, 90.01, 0));
    assert(stepDeg(ELBOW_L, 0, 89.99, 0, 0, 90, 0) < 0.05);
    assert(stepDeg(SHOULDER_L, 0, 90, 0, 0, 90.01, 0) < 0.05);
    assert(stepDeg(SHOULDER_L, 10, 89.99, -5, 10, 90, -5) < 0.05);

    checkContinuous("elbow", ELBOW_L, -90.0, 90.0);
    checkContinuous("shoulder", SHOULDER_L, -110.0, 110.0);
    checkContinuous("wrist", WRIST_L, -20.0, 140.0);

    // Off the pole the result is exactly the generator's (bit for bit), on it within the nudge.
    for (const Model* m : { &ELBOW_L, &SHOULDER_L, &WRIST_L, &DEFAULT_M })
    {
        for (double y = -110.0; y <= 140.0; y += 0.5)
        {
            for (double x = -30.0; x <= 30.0; x += 7.5)
            {
                const double z = 0.5 * x;
                const HQ raw = forwardApplied(*m, degToRad(m->invert[0] * (x - m->offset[0])), degToRad(m->invert[1] * (y - m->offset[1])),
                                              degToRad(m->invert[2] * (z - m->offset[2])), false);
                const HQ got = dialToQuat(*m, x, y, z);
                const bool pole = m->leg && std::fabs(m->invert[1] * (y - m->offset[1]) - 180.0) < 1.0e-6;
                if (pole)
                    assert(angleBetweenDeg(got, dialToQuat(*m, x, y - 0.001, z)) < 0.01);
                else
                    assert(sameBits(raw, got));
            }
        }
    }

    // The default model (fingers, head, spine) never flipped: unchanged at its poles.
    assert(stepDeg(DEFAULT_M, 0, 89.99, 0, 0, 90, 0) < 0.05);
    assert(stepDeg(DEFAULT_M, 0, -90, 0, 0, -89.99, 0) < 0.05);

    checkDialDrag();

    printf("slposedialmath_test: ok\n");
    return 0;
}
