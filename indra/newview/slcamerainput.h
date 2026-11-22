/**
 * @file slcamerainput.h
 * @brief Camera mode's input decisions (#1572), pure and host-tested
 *        (scripts/check/test_camera_input.cpp). SLCameraMode routes events; these decide what
 *        they mean.
 *
 * Left: a click focuses (the AF point), a double-click on the same spot within the system's
 * double-click time focuses and shoots; the click that brought the window to the front and a drag
 * do nothing. Right: a click opens quick settings at the pointer, a drag orbits. The wheel's
 * target by modifiers (Alt, Ctrl, Shift), what is under the pointer and whether Controls is open.
 * Standard headers only.
 */

#ifndef SL_CAMERAINPUT_H
#define SL_CAMERAINPUT_H

namespace slcamin
{
enum class Left
{
    None,
    Focus,
    FocusAndShoot
};
struct ClickState
{
    double lastUpTime = -1.0;
    float  lastX = 0.f, lastY = 0.f;
    bool   swallowUp = false; // the press brought the window to the front: its release goes too
};

// A left press anywhere (the overlay or the picture), before anything else sees it. False: the
// press brought the window to the front and goes nowhere, nor does its release.
inline bool beginPress(ClickState& s, bool activation)
{
    s.swallowUp = activation;
    if (activation)
        s.lastUpTime = -1.0; // not the first half of a double-click either
    return !activation;
}
// Its release. False: swallowed with its press.
inline bool endPress(ClickState& s)
{
    const bool pass = !s.swallowUp;
    s.swallowUp     = false;
    return pass;
}

// A left button release on the picture. `travel`: how far the pointer moved since the press,
// `dblTime` / `slop`: the system's double-click time and drag threshold, `activation`: the press
// brought the viewer window to the front.
inline Left onLeftUp(ClickState& s, double t, float x, float y, float travel, double dblTime, float slop, bool activation)
{
    if (activation || travel > slop)
    {
        s.lastUpTime = -1.0;
        return Left::None;
    }
    const float dx = x - s.lastX, dy = y - s.lastY;
    if (s.lastUpTime >= 0.0 && t - s.lastUpTime <= dblTime && dx * dx + dy * dy <= slop * slop)
    {
        s.lastUpTime = -1.0; // a third click starts over
        return Left::FocusAndShoot;
    }
    s.lastUpTime = t;
    s.lastX      = x;
    s.lastY      = y;
    return Left::Focus;
}

enum class Right
{
    QuickSettings,
    OrbitEnd
};
inline Right onRightUp(float travel, float slop)
{
    return travel > slop ? Right::OrbitEnd : Right::QuickSettings;
}

// What the pointer is over (the overlay's elements that change the wheel's meaning).
enum class Over
{
    World,
    Cell,
    GridChip,
    TiltKnob
};
enum class Wheel
{
    Dolly,     // move in / out, as in play (over the picture, always)
    RearDial,  // exposure (Shift)
    Zoom,      // the zoom ring (Ctrl)
    Cell,      // the strip cell under the pointer
    GridNext,  // the next grid (over the Grid chip)
    TiltFine,  // fine tilt (over the tilt knob)
    Camera     // the normal camera wheel (Alt)
};
inline Wheel wheelTarget(bool controlsOpen, Over over, bool shift, bool ctrl, bool alt)
{
    if (alt)
        return Wheel::Camera;
    if (ctrl)
        return Wheel::Zoom;
    if (shift)
        return Wheel::RearDial;
    if (over == Over::GridChip)
        return Wheel::GridNext;
    if (over == Over::TiltKnob)
        return Wheel::TiltFine;
    if (over == Over::Cell)
        return Wheel::Cell;
    (void)controlsOpen; // over the picture the wheel moves the camera, Controls open or not
    return Wheel::Dolly;
}

// The camera's key commands stand back while text is edited or a popup / menu is open (there
// Enter, Space and the arrows act on the focused item).
inline bool commandTakesKey(bool textEditing, bool popupOpen)
{
    return !textEditing && !popupOpen;
}

// Which commands stand back: typing in a field keeps every key; an open popover or menu takes
// only Enter (its focused item; Space and the arrows are its own), so the letter commands (B, L,
// V, H, ...) keep working. The review card is not a menu: it blocks nothing.
enum class Command
{
    Shoot,  // Enter
    Letter  // every other camera command
};
inline bool commandTakesKeyFor(Command c, bool textEditing, bool menuOpen)
{
    if (textEditing)
        return false;
    return c == Command::Letter || !menuOpen;
}
// One Enter press reaches camera mode twice: as the key, then as its character (the viewer acts on
// Return when its character arrives). The character of a press camera mode handled does nothing,
// and neither does a held Enter's repeat.
struct ReturnState
{
    double at    = -1.0;  // when camera mode last handled an Enter
    bool   taken = false; // ...and its character has not arrived yet
};
inline bool takeReturn(ReturnState& s, double t, bool repeat)
{
    if (repeat)
        return false;
    if (s.taken && t - s.at < 0.15)
    {
        s.taken = false; // the same press's character
        return false;
    }
    s.taken = false;
    return true;
}
inline void returnHandled(ReturnState& s, double t)
{
    s.at    = t;
    s.taken = true;
}

// Space clicks the focused control, and Enter does while a popover, menu or card is open (with
// nothing open Enter is the shutter); only the overlay's own control, and only while it shows.
enum class NavKey
{
    Space,
    Enter
};
inline bool navActivatesFocus(NavKey k, bool focusOurs, bool focusVisible, bool menuOpen)
{
    return focusOurs && focusVisible && (k == NavKey::Space || menuOpen);
}
} // namespace slcamin

#endif // SL_CAMERAINPUT_H
