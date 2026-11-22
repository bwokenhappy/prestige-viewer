/**
 * @file slposedialctrl.h
 * @brief Pose Studio (#500): half-ring rotation dial with a pointer,
 *        registered as @c sl_pose_dial.
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

#ifndef SL_POSEDIALCTRL_H
#define SL_POSEDIALCTRL_H

#include "lluictrl.h"
#include "lluicolor.h"

#include <boost/signals2.hpp>

/**
 * The value (degrees) runs left to right over the upper half ring, min_value at the
 * left end, max_value at the right, zero marked with a tick. Press anywhere on the dial
 * and turn around its centre: the value moves by the turn from where it was (a click
 * alone changes nothing; past the ends, below the centre line, the turn carries on),
 * a tenth as fast with Shift held (SLPoseDial::dragValue, slposedialmath.h). A value past
 * an end (set from a decoded pose) is kept: the pointer sits at the end and a drag only
 * stops it going further out. The press signal fires first on every press, so the owner can
 * set the live value; the commit signal fires continuously while dragging (local preview)
 * and the release signal once when the button comes up, or the capture is lost, after a
 * change. Clicking the small centre hub resets to zero (a commit only if it was not zero).
 * Scroll wheel nudges by one snap step (1 degree when snapping is off, x10 with shift).
 */
class SLPoseDialCtrl : public LLUICtrl
{
public:
    struct Params : public LLInitParam::Block<Params, LLUICtrl::Params>
    {
        Optional<F32>         min_value;
        Optional<F32>         max_value;
        Optional<LLUIColor>   ring_color;
        Optional<LLUIColor>   track_color;
        Optional<LLUIColor>   pointer_color;
        Optional<LLUIColor>   text_color;
        Optional<S32>         ring_thickness;

        Params();
    };

    SLPoseDialCtrl(const Params& p);
    ~SLPoseDialCtrl() override = default;

    void setValue(const LLSD& value) override;
    LLSD getValue() const override { return LLSD(mValue); }
    F32 getValueF32() const { return mValue; }

    void setSnap(F32 step_degrees) { mSnap = llmax(0.f, step_degrees); }
    void setLabel(const std::string& label) { mLabel = label; }   // #1566: "X local", "X world", ...
    // Changes the span (per bone). The value is kept as set even outside the span (a decoded
    // pose can lie past a dial's end): the pointer is drawn at the end, the number as it is.
    void setRange(F32 min_value, F32 max_value);
    bool isDragging() const { return mDragging; }

    typedef boost::signals2::signal<void(SLPoseDialCtrl*)> release_signal_t;
    boost::signals2::connection setReleaseCallback(const release_signal_t::slot_type& cb) { return mReleaseSignal.connect(cb); }
    // Fires on every press, before the dial reads its value: the owner can set the live
    // value there (setValue) and the drag or hub reset starts from it (#1459).
    boost::signals2::connection setPressCallback(const release_signal_t::slot_type& cb) { return mPressSignal.connect(cb); }

    void draw() override;
    bool handleMouseDown(S32 x, S32 y, MASK mask) override;
    bool handleMouseUp(S32 x, S32 y, MASK mask) override;
    bool handleHover(S32 x, S32 y, MASK mask) override;
    bool handleDoubleClick(S32 x, S32 y, MASK mask) override;
    bool handleScrollWheel(S32 x, S32 y, S32 clicks) override;
    void onMouseCaptureLost() override;

private:
    void geometry(F32& cx, F32& cy, F32& radius) const;
    F32 valueToAngle(F32 value) const;
    bool onHub(S32 x, S32 y) const;
    F32 applySnap(F32 value, F32 lo, F32 hi) const;
    void setFromUser(F32 value, F32 lo, F32 hi);
    void resetToZero();

    F32 mMin;
    F32 mMax;
    F32 mValue = 0.f;
    F32 mSnap = 0.f;
    bool mDragging = false;
    bool mMoved = false;            // past the click slop
    bool mChanged = false;          // the value changed during this drag
    bool mHasAngle = false;         // mLastAngle is known (the press was off the centre)
    F32 mLastAngle = 0.f;           // pointer angle at the last drag step (radians)
    F32 mDragValue = 0.f;           // unsnapped value the drag accumulates
    F32 mDragLo = 0.f;              // this drag's limits: the range, widened to its start value
    F32 mDragHi = 0.f;
    S32 mPressX = 0;
    S32 mPressY = 0;
    std::string mLabel;
    S32 mRingThickness;
    LLUIColor mRingColor;
    LLUIColor mTrackColor;
    LLUIColor mPointerColor;
    LLUIColor mTextColor;
    release_signal_t mReleaseSignal;
    release_signal_t mPressSignal;
};

#endif // SL_POSEDIALCTRL_H
