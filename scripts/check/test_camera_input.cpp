// Tests for indra/newview/slcamerainput.h: camera mode's input decisions (#1572). A click focuses, a
// double-click on the same spot within the double-click time focuses and shoots, the click that brings the
// window to the front does nothing, a drag is not a click, right-click opens quick settings and a right-drag
// orbits, the wheel's target by state and modifiers, and typing keeps its keys.
// Built and run by scripts/check/checks/camera_input.py.
#include "slcamerainput.h"

#include <cstdio>

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); ++failures; } } while (0)

using namespace slcamin;

static const double kDbl  = 0.5;
static const float  kSlop = 4.f;

static void activationClickIgnored()
{
    ClickState s;
    CHECK(onLeftUp(s, 1.0, 100, 100, 0, kDbl, kSlop, true) == Left::None);
    // ...and it does not count as the first half of a double-click.
    CHECK(onLeftUp(s, 1.1, 100, 100, 0, kDbl, kSlop, false) == Left::Focus);
}

// The press that brings the window to the front goes nowhere: not to the overlay's buttons (the
// shutter, Leave) and not to the picture; its release is swallowed with it.
static void activationPressGoesNowhere()
{
    ClickState s;
    CHECK(!beginPress(s, true));
    CHECK(s.swallowUp);
    CHECK(!endPress(s)); // its release: swallowed, and it clears the flag
    CHECK(!s.swallowUp);
    CHECK(beginPress(s, false));
    CHECK(endPress(s));
}

static void singleClickFocuses()
{
    ClickState s;
    CHECK(onLeftUp(s, 1.0, 10, 10, 0, kDbl, kSlop, false) == Left::Focus);
}

static void doubleClickNeedsSameSpotAndTime()
{
    {
        ClickState s;
        CHECK(onLeftUp(s, 1.0, 100, 100, 0, kDbl, kSlop, false) == Left::Focus);
        CHECK(onLeftUp(s, 1.0 + kDbl + 0.01, 100, 100, 0, kDbl, kSlop, false) == Left::Focus); // too late
    }
    {
        ClickState s;
        CHECK(onLeftUp(s, 1.0, 100, 100, 0, kDbl, kSlop, false) == Left::Focus);
        CHECK(onLeftUp(s, 1.1, 100 + 2 * kSlop, 100, 0, kDbl, kSlop, false) == Left::Focus); // too far
    }
    {
        ClickState s;
        CHECK(onLeftUp(s, 1.0, 100, 100, 0, kDbl, kSlop, false) == Left::Focus);
        CHECK(onLeftUp(s, 1.2, 101, 102, 0, kDbl, kSlop, false) == Left::FocusAndShoot);
        // A third click right after the shot starts over: it focuses, it does not shoot again.
        CHECK(onLeftUp(s, 1.3, 101, 102, 0, kDbl, kSlop, false) == Left::Focus);
    }
}

static void dragIsNotAClick()
{
    ClickState s;
    CHECK(onLeftUp(s, 1.0, 100, 100, kSlop + 1, kDbl, kSlop, false) == Left::None);
    CHECK(onLeftUp(s, 1.1, 100, 100, 0, kDbl, kSlop, false) == Left::Focus); // not a double with the drag
}

static void rightClickVsOrbit()
{
    CHECK(onRightUp(0, kSlop) == Right::QuickSettings);
    CHECK(onRightUp(kSlop, kSlop) == Right::QuickSettings);
    CHECK(onRightUp(kSlop + 1, kSlop) == Right::OrbitEnd);
}

static void wheelTable()
{
    CHECK(wheelTarget(false, Over::World, false, false, false) == Wheel::Dolly);
    // Over the picture the wheel always moves the camera, Controls open or not; a value changes
    // only when the pointer is on it.
    CHECK(wheelTarget(true, Over::World, false, false, false) == Wheel::Dolly);
    CHECK(wheelTarget(true, Over::Cell, false, false, false) == Wheel::Cell);
    CHECK(wheelTarget(false, Over::Cell, false, false, false) == Wheel::Cell);
    CHECK(wheelTarget(false, Over::GridChip, false, false, false) == Wheel::GridNext);
    CHECK(wheelTarget(true, Over::TiltKnob, false, false, false) == Wheel::TiltFine);
    CHECK(wheelTarget(false, Over::World, true, false, false) == Wheel::RearDial);
    CHECK(wheelTarget(true, Over::Cell, true, false, false) == Wheel::RearDial);
    CHECK(wheelTarget(false, Over::World, false, true, false) == Wheel::Zoom);
    CHECK(wheelTarget(false, Over::World, true, true, false) == Wheel::Zoom); // ctrl beats shift
    CHECK(wheelTarget(true, Over::GridChip, false, false, true) == Wheel::Camera);
    CHECK(wheelTarget(false, Over::World, false, true, true) == Wheel::Camera); // alt beats ctrl
}

static void textEditingKeepsKeys()
{
    CHECK(commandTakesKey(false, false));
    CHECK(!commandTakesKey(true, false));
    CHECK(!commandTakesKey(false, true));
    CHECK(!commandTakesKey(true, true));
}

// A popover or menu takes Enter, Space and the arrows for its items; the camera's letter commands
// (B, L, V, H) keep working, and the review card blocks nothing. Typing in a field keeps every key.
static void lettersWorkWithMenuOpen()
{
    CHECK(commandTakesKeyFor(Command::Letter, false, true));
    CHECK(!commandTakesKeyFor(Command::Shoot, false, true)); // Enter: the menu's focused item
    CHECK(commandTakesKeyFor(Command::Shoot, false, false));
    CHECK(!commandTakesKeyFor(Command::Letter, true, false)); // typing keeps its keys
    CHECK(!commandTakesKeyFor(Command::Shoot, true, false));
}

// One Enter press reaches camera mode twice (the key, then its character): the second arrival does
// nothing, so Enter in a field (it lets go of the field) or on the send card (it sends) never also
// shoots. A held Enter's repeats do nothing: a card that opens under a held Enter is not confirmed.
static void enterIsOnePress()
{
    ReturnState s;
    CHECK(takeReturn(s, 10.0, false));
    returnHandled(s, 10.0);
    CHECK(!takeReturn(s, 10.0, false)); // its character
    CHECK(takeReturn(s, 10.6, false));  // the next press
    CHECK(!takeReturn(s, 11.0, true));  // a repeat
    ReturnState u;                      // a press camera mode let go: its character goes on
    CHECK(takeReturn(u, 12.0, false));
    CHECK(takeReturn(u, 12.0, false));
}

// Space (and Enter with a menu open) clicks the focused control only while it shows: a control
// hidden since it was clicked (the card's Delete) keeps RmlUi's focus, and must not fire again.
static void hiddenFocusNeverClicks()
{
    CHECK(navActivatesFocus(NavKey::Space, true, true, false));
    CHECK(!navActivatesFocus(NavKey::Space, true, false, false));
    CHECK(!navActivatesFocus(NavKey::Enter, true, false, true));
    CHECK(navActivatesFocus(NavKey::Enter, true, true, true));
    CHECK(!navActivatesFocus(NavKey::Enter, true, true, false)); // nothing open: Enter is the shutter
    CHECK(!navActivatesFocus(NavKey::Space, false, true, false)); // not the overlay's
}

int main()
{
    activationClickIgnored();
    activationPressGoesNowhere();
    singleClickFocuses();
    doubleClickNeedsSameSpotAndTime();
    dragIsNotAClick();
    rightClickVsOrbit();
    wheelTable();
    textEditingKeepsKeys();
    lettersWorkWithMenuOpen();
    enterIsOnePress();
    hiddenFocusNeverClicks();
    if (failures)
        std::printf("%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
