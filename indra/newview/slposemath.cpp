/**
 * @file slposemath.cpp
 * @brief Pose Studio (#500): the OP Pose HUD dial model and the compact
 *        quaternion quantisation used by pose sync.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Slipstream Viewer Source Code
 * Copyright (C) 2026, Slipstream Viewer contributors
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;
 * version 2.1 of the License only.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 * $/LicenseInfo$
 */

#include "llviewerprecompiledheaders.h"

#include "slposemath.h"
#include "slposedialmath.h"

#include "llmath.h"
#include "llsdutil.h"
#include "llsdutil_math.h"

#include <cmath>

namespace
{
// The generator's math lives in slposedialmath.h (dependency-free, checked on its own).
using namespace SLPoseGen;

HQ fromLL(const LLQuaternion& q)
{
    HQ h;
    h.w = q.mQ[VW]; h.x = q.mQ[VX]; h.y = q.mQ[VY]; h.z = q.mQ[VZ];
    return h;
}

Model genModel(const SLBoneModel& m)
{
    Model g;
    g.axis = m.mAxis;
    g.leg = m.mLeg;
    g.base = fromLL(m.mBase);
    for (S32 i = 0; i < 3; ++i)
    {
        g.pre[i] = (double)m.mPre.mV[i];
        g.offset[i] = (double)m.mOffset.mV[i];
        g.invert[i] = (double)m.mInvert.mV[i];
    }
    return g;
}
}

// static
SLBoneModel SLBoneModel::fromLLSD(const LLSD& sd, const SLBoneModel& defaults)
{
    SLBoneModel m = defaults;
    if (sd.has("axis"))
        m.mAxis = llclamp((S32)sd["axis"].asInteger(), 0, 2);
    if (sd.has("leg"))
        m.mLeg = sd["leg"].asInteger() != 0;
    if (sd.has("pre"))
        m.mPre.setValue(sd["pre"]);
    if (sd.has("base"))
    {
        m.mBase = ll_quaternion_from_sd(sd["base"]);
        m.mBase.normalize();
    }
    if (sd.has("offset"))
        m.mOffset.setValue(sd["offset"]);
    if (sd.has("invert"))
    {
        m.mInvert.setValue(sd["invert"]);
        for (S32 i = 0; i < 3; ++i)
            m.mInvert.mV[i] = (m.mInvert.mV[i] < 0.f) ? -1.f : 1.f;
    }
    if (sd.has("min"))
        m.mMin.setValue(sd["min"]);
    if (sd.has("max"))
        m.mMax.setValue(sd["max"]);
    if (sd.has("steps"))
        m.mSteps.setValue(sd["steps"]);
    m.mFollowers.clear();
    if (sd.has("followers"))
    {
        for (const LLSD& f : llsd::inArray(sd["followers"]))
        {
            Follower follower;
            follower.mJoint = f["joint"].asString();
            follower.mInvert = f["invert"].asInteger() != 0;
            if (!follower.mJoint.empty())
                m.mFollowers.push_back(follower);
        }
    }
    return m;
}

LLQuaternion SLBoneModel::toQuat(const SLPoseAngles& dial, bool follower_invert) const
{
    const HQ q = dialToQuat(genModel(*this), (double)dial.mX, (double)dial.mY, (double)dial.mZ, follower_invert);
    LLQuaternion out((F32)q.x, (F32)q.y, (F32)q.z, (F32)q.w);
    out.normalize();
    return out;
}

SLPoseAngles SLBoneModel::fromQuat(const LLQuaternion& rotation, const SLPoseAngles& reference) const
{
    HQ target = fromLL(rotation);
    if (target.w < 0.0)
    {
        target.w = -target.w; target.x = -target.x; target.y = -target.y; target.z = -target.z;
    }

    // The current dial values still produce this rotation: keep them. This also covers the
    // generator's singular points (e.g. a 180-degree swing), where many dial triples give
    // the same rotation and a fresh decomposition would jump.
    if (angleBetweenDeg(fromLL(toQuat(reference)), target) <= 0.05)
        return reference;

    // Reference in applied generator angles (radians).
    double ref[3];
    for (S32 i = 0; i < 3; ++i)
        ref[i] = degToRad((double)mInvert.mV[i] * ((double)reference.get(i) - (double)mOffset.mV[i]));
    // The swing direction's phi at the reference (kept at the poles, where it is undefined).
    const double ref_phi = (mAxis == 2) ? ref[0] : ref[2];

    // Pre-images of the w rebuild: the target itself, and the same x,y,z with -w.
    std::vector<HQ> pre_images(1, target);
    if (target.w > 1.0e-9)
    {
        HQ alt = target;
        alt.w = -alt.w;
        pre_images.push_back(alt);
    }

    // The pre-rotations the forward model can have used.
    std::vector<HQ> pres(1, eulerQ(degToRad(mPre.mV[VX]), degToRad(mPre.mV[VY]), degToRad(mPre.mV[VZ])));
    if (mLeg)
    {
        pres.push_back(eulerQ(0.0, PI_D * 1.5, 0.0));
        pres.push_back(eulerQ(0.0, -PI_D / 2.0, 0.0));
    }

    const HQ base_inv = conjQ(fromLL(mBase));
    const DV r = refAxis(mAxis);

    // Candidates are ranked: rotation reproduced and every dial inside its range, then
    // reproduced but outside a range, then not reproduced; within a rank the one nearest to
    // reference (or, unreproduced, the smallest error) wins. The range rank keeps a decode
    // from picking an equivalent triple the dials cannot show (e.g. wrist y 140 as -160).
    bool have = false;
    S32 best_rank = 0;
    double best_score = 0.0;
    SLPoseAngles best = reference;

    for (const HQ& image : pre_images)
    {
        for (const HQ& pre : pres)
        {
            // image = base * pre * R  ->  R = pre^-1 * base^-1 * image
            const HQ R = hmulRaw(conjQ(pre), hmulRaw(base_inv, image));
            const DV pos = rotateV(R, r);
            const double theta = acos(llclamp(pos.z, -1.0, 1.0));
            const bool pole = sin(theta) < 1.0e-4;
            const double phi = pole ? ref_phi : atan2(pos.y, pos.x);

            const HQ T = hmulRaw(R, conjQ(swingQ(r, pos)));
            const double twist = 2.0 * atan2(T.x * pos.x + T.y * pos.y + T.z * pos.z, T.w);

            const double thetas[3] = { theta, -theta, 2.0 * PI_D - theta };
            const double phis[3] = { phi, phi + PI_D, phi - PI_D };
            for (S32 v = 0; v < 3; ++v)
            {
                for (S32 k1 = -1; k1 <= 1; ++k1)
                {
                    for (S32 k2 = -1; k2 <= 1; ++k2)
                    {
                        const double ph = phis[v] + 2.0 * PI_D * k1;
                        const double tw = twist + 2.0 * PI_D * k2;
                        const double th = thetas[v];
                        double g[3];
                        if (mAxis == 0) { g[0] = tw; g[1] = th; g[2] = ph; }
                        else if (mAxis == 1) { g[0] = th; g[1] = tw; g[2] = ph; }
                        else
                        {
                            const double ax = ph, ay = th;
                            double add = 0.0;
                            if (mLeg)
                                add = (ay < PI_D / 2.0) ? ax * sin(ay) : ax - ax * cos(ay);
                            g[0] = ax; g[1] = ay; g[2] = tw - add;
                        }

                        SLPoseAngles dial;
                        for (S32 i = 0; i < 3; ++i)
                            dial.set(i, (F32)(g[i] * 180.0 / PI_D * mInvert.mV[i] + mOffset.mV[i]));

                        const double err = angleBetweenDeg(fromLL(toQuat(dial)), target);
                        const bool valid = err <= 0.05;
                        double dist = 0.0;
                        bool in_range = true;
                        for (S32 i = 0; i < 3; ++i)
                        {
                            dist += fabs(dial.get(i) - reference.get(i));
                            in_range = in_range && dial.get(i) >= mMin.mV[i] - 0.01f && dial.get(i) <= mMax.mV[i] + 0.01f;
                        }
                        const S32 rank = valid ? (in_range ? 2 : 1) : 0;
                        const double score = valid ? dist : err;
                        if (!have || rank > best_rank || (rank == best_rank && score < best_score))
                        {
                            have = true;
                            best_rank = rank;
                            best_score = score;
                            best = dial;
                        }
                    }
                }
            }
        }
    }
    return best;
}

// static
LLQuaternion SLBoneModel::mirror(const LLQuaternion& rotation)
{
    // Reflection through the X-Z plane (Y is avatar-left in SL): same as FSJointState::reflectRotation.
    return LLQuaternion(-rotation.mQ[VX], rotation.mQ[VY], -rotation.mQ[VZ], rotation.mQ[VW]);
}

namespace SLPoseQuant
{
namespace
{
constexpr F32 COMPONENT_RANGE = 0.70710678f; // 1/sqrt(2): bound of the three smallest components
constexpr U32 COMPONENT_MAX = 32767;         // 15 bits
constexpr char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

S32 b64Value(char c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '-') return 62;
    if (c == '_') return 63;
    return -1;
}
}

void packQuat(const LLQuaternion& rotation, U8 out[6])
{
    LLQuaternion q(rotation);
    q.normalize();

    S32 largest = 0;
    for (S32 i = 1; i < 4; ++i)
    {
        if (llabs(q.mQ[i]) > llabs(q.mQ[largest]))
            largest = i;
    }
    const F32 sign = (q.mQ[largest] < 0.f) ? -1.f : 1.f;

    U64 bits = (U64)largest;
    for (S32 i = 0; i < 4; ++i)
    {
        if (i == largest)
            continue;
        const F32 c = llclamp(q.mQ[i] * sign / COMPONENT_RANGE, -1.f, 1.f);
        const U32 v = (U32)ll_round((c * 0.5f + 0.5f) * (F32)COMPONENT_MAX);
        bits = (bits << 15) | (U64)(v & COMPONENT_MAX);
    }
    // 2 + 45 = 47 bits, stored in 48.
    for (S32 i = 5; i >= 0; --i)
    {
        out[i] = (U8)(bits & 0xFF);
        bits >>= 8;
    }
}

LLQuaternion unpackQuat(const U8 in[6])
{
    U64 bits = 0;
    for (S32 i = 0; i < 6; ++i)
        bits = (bits << 8) | (U64)in[i];

    F32 comps[3];
    for (S32 i = 2; i >= 0; --i)
    {
        const U32 v = (U32)(bits & COMPONENT_MAX);
        bits >>= 15;
        comps[i] = (((F32)v / (F32)COMPONENT_MAX) * 2.f - 1.f) * COMPONENT_RANGE;
    }
    const S32 largest = (S32)(bits & 0x3);

    LLQuaternion q;
    F32 sum = 0.f;
    S32 c = 0;
    for (S32 i = 0; i < 4; ++i)
    {
        if (i == largest)
            continue;
        q.mQ[i] = comps[c++];
        sum += q.mQ[i] * q.mQ[i];
    }
    q.mQ[largest] = sqrtf(llmax(0.f, 1.f - sum));
    q.normalize();
    return q;
}

std::string encode64(const U8* data, size_t len)
{
    std::string out;
    out.reserve((len * 4 + 2) / 3);
    size_t i = 0;
    for (; i + 3 <= len; i += 3)
    {
        const U32 v = ((U32)data[i] << 16) | ((U32)data[i + 1] << 8) | (U32)data[i + 2];
        out += B64[(v >> 18) & 63];
        out += B64[(v >> 12) & 63];
        out += B64[(v >> 6) & 63];
        out += B64[v & 63];
    }
    const size_t rest = len - i;
    if (rest == 1)
    {
        const U32 v = (U32)data[i] << 16;
        out += B64[(v >> 18) & 63];
        out += B64[(v >> 12) & 63];
    }
    else if (rest == 2)
    {
        const U32 v = ((U32)data[i] << 16) | ((U32)data[i + 1] << 8);
        out += B64[(v >> 18) & 63];
        out += B64[(v >> 12) & 63];
        out += B64[(v >> 6) & 63];
    }
    return out;
}

bool decode64(const std::string& text, std::string& out)
{
    out.clear();
    out.reserve(text.size() * 3 / 4);
    U32 acc = 0;
    S32 nbits = 0;
    for (char ch : text)
    {
        const S32 v = b64Value(ch);
        if (v < 0)
            return false;
        acc = (acc << 6) | (U32)v;
        nbits += 6;
        if (nbits >= 8)
        {
            nbits -= 8;
            out += (char)((acc >> nbits) & 0xFF);
        }
    }
    return true;
}
}
