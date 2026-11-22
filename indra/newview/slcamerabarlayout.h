/**
 * @file slcamerabarlayout.h
 * @brief Camera mode's quick-shot bar geometry (#1572), pure and host-tested
 *        (scripts/check/test_camera_bar_layout.cpp).
 *
 * [Leave] [Save to disk] (shutter) [1920 x 1080 . PNG] [Controls]: fixed, mirrored widths (outer
 * 104 dp, inner 168 dp), the 46 dp shutter with 12 dp each side, 6 dp padding, and the shutter's
 * centre exactly on the window's centre line. Labels that do not fit shrink one type step (13 to
 * 11 dp) before they truncate; the widths never change. Standard headers only.
 */

#ifndef SL_CAMERABARLAYOUT_H
#define SL_CAMERABARLAYOUT_H

namespace slcambar
{
constexpr float kOuter = 104.f, kInner = 168.f, kShutter = 46.f, kShutterGap = 12.f, kPad = 6.f;
constexpr float kTitle = 15.f, kBody = 13.f, kCaption = 11.f; // the kit's type sizes (dp)

struct Box
{
    float x = 0.f, w = 0.f;
};
// Window pixels from the window's left edge (`dp`: pixels per UI unit).
struct Bar
{
    float left = 0.f, width = 0.f;
    Box   leave, saveTo, shutter, size, controls;
};

inline Bar layoutBar(float windowW, float dp)
{
    Bar b;
    b.width     = (2 * kPad + 2 * kOuter + 2 * kInner + kShutter + 2 * kShutterGap) * dp;
    // From the centre line outwards, so the shutter is centred whatever the rounding of the rest.
    const float c = windowW * 0.5f;
    b.shutter     = { c - kShutter * dp * 0.5f, kShutter * dp };
    b.saveTo      = { b.shutter.x - kShutterGap * dp - kInner * dp, kInner * dp };
    b.leave       = { b.saveTo.x - kOuter * dp, kOuter * dp };
    b.size        = { b.shutter.x + b.shutter.w + kShutterGap * dp, kInner * dp };
    b.controls    = { b.size.x + b.size.w, kOuter * dp };
    b.left        = b.leave.x - kPad * dp;
    return b;
}

// 0: the label fits at its size; 1: it fits one type step smaller; 2: truncate at the smaller size.
// `textW` is measured at `size` (the bar's labels: title 15 to body 13 dp; elsewhere body to caption).
inline int labelStep(float textW, float boxW, float size = kBody, float smaller = kCaption)
{
    if (textW <= boxW)
        return 0;
    return textW * smaller / size <= boxW ? 1 : 2;
}

// The hint row: for 8 s after it was shown (entering, opening Controls).
inline bool hintVisible(double now, double shownAt)
{
    const double d = now - shownAt;
    return d >= 0.0 && d < 8.0;
}
} // namespace slcambar

#endif // SL_CAMERABARLAYOUT_H
