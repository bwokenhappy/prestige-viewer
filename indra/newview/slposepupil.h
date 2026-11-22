/**
 * @file slposepupil.h
 * @brief Pupil eyes (#1567): reads an ik_chains.xml eye set of type "pupil" (eyes that look by
 *        sliding the bone over the eye and turning it flat to it, the Open Pony's
 *        mFaceNoseLeft / Right) into SLPoseIKMath::PupilBone.
 *        Shared by Pose Studio's eye IK (slposeik.cpp) and the live look-at (slposelookat.cpp)
 *        so both read the same numbers.
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

#ifndef SL_POSE_PUPIL_H
#define SL_POSE_PUPIL_H

#include "llsd.h"
#include "slposeikmath.h"

#include <cmath>
#include <string>

namespace SLPosePupil
{
// "x" / "y" / "z" (or 0 / 1 / 2) -> axis index; anything else keeps fallback.
inline int readAxis(const LLSD& value, int fallback)
{
    if (value.isInteger())
    {
        const int axis = value.asInteger();
        return (axis >= 0 && axis <= 2) ? axis : fallback;
    }
    const std::string name = value.asString();
    if (name == "x" || name == "X") return 0;
    if (name == "y" || name == "Y") return 1;
    if (name == "z" || name == "Z") return 2;
    return fallback;
}

inline double readReal(const LLSD& value, double fallback, double lo, double hi)
{
    if (!value.isReal() && !value.isInteger())
        return fallback;
    const double v = value.asReal();
    return std::isfinite(v) ? std::min(std::max(v, lo), hi) : fallback;
}

inline double readSign(const LLSD& value, double fallback)
{
    if (!value.isReal() && !value.isInteger())
        return fallback;
    return value.asReal() < 0.0 ? -1.0 : 1.0;
}

// One bone's entry of an eye set's "pupil" map (see ik_chains.xml for the fields).
inline SLPoseIKMath::PupilBone readBone(const LLSD& entry)
{
    SLPoseIKMath::PupilBone p;
    auto vec = [](const LLSD& v, double lo, double hi)
    {
        if (!v.isArray() || v.size() < 3)
            return SLPoseIKMath::V3();
        return SLPoseIKMath::V3(readReal(v[0], 0.0, lo, hi), readReal(v[1], 0.0, lo, hi), readReal(v[2], 0.0, lo, hi));
    };
    if (entry["model"].asString() == "shell")
    {
        p.mModel = SLPoseIKMath::PupilBone::SHELL;
        p.mPivot = vec(entry["pivot"], -1.0, 1.0);
        p.mArm = readReal(entry["arm"], 0.0, 0.0, 1.0);
        p.mShellCenter = vec(entry["shell_center"], -5.0, 5.0);
        // The shell: three semi-axis vectors (an ellipsoid), or one radius (a sphere).
        const LLSD& axes = entry["shell_axes"];
        if (axes.isArray() && axes.size() >= 3)
        {
            for (int i = 0; i < 3; ++i)
                p.mShellAxes[i] = vec(axes[i], -5.0, 5.0);
        }
        else
        {
            const double radius = readReal(entry["shell_radius"], 0.0, 0.0, 5.0);
            p.mShellAxes[0] = SLPoseIKMath::V3(radius, 0, 0);
            p.mShellAxes[1] = SLPoseIKMath::V3(0, radius, 0);
            p.mShellAxes[2] = SLPoseIKMath::V3(0, 0, radius);
        }
        p.mAimOffset = vec(entry["aim_offset"], -1.0, 1.0);
        p.mAimCenter = vec(entry["aim_center"], -1.0, 1.0);
        p.mAxis = vec(entry["axis"], -1.0, 1.0);
    }
    p.mHAxis = readAxis(entry["h_axis"], p.mHAxis);
    p.mHSign = readSign(entry["h_sign"], p.mHSign);
    p.mVAxis = readAxis(entry["v_axis"], p.mVAxis);
    p.mVSign = readSign(entry["v_sign"], p.mVSign);
    p.mGainH = readReal(entry["gain_h"], p.mGainH, 0.0, 0.01);
    p.mGainV = readReal(entry["gain_v"], p.mGainV, 0.0, 0.01);
    if (entry["range"].isArray() && entry["range"].size() >= 2)
    {
        p.mRangeH = readReal(entry["range"][0], p.mRangeH, 0.0, 0.2);
        p.mRangeV = readReal(entry["range"][1], p.mRangeV, 0.0, 0.2);
    }
    p.mVergence = readReal(entry["vergence"], p.mVergence, 0.0, 0.05);
    p.mInward = readSign(entry["inward"], p.mInward);
    p.mCenter = vec(entry["center"], -0.2, 0.2);
    return p;
}

// An eye set is a pupil set when its "type" says so; its bones' entries (by joint name) go
// into out in the order of joints (a joint without an entry gets the default: no movement).
template <typename Joints, typename Out>
inline bool readSet(const LLSD& set, const Joints& joints, Out& out)
{
    out.clear();
    if (set["type"].asString() != "pupil")
        return false;
    for (const std::string& name : joints)
        out.push_back(readBone(set["pupil"][name]));
    return true;
}
}

#endif // SL_POSE_PUPIL_H
