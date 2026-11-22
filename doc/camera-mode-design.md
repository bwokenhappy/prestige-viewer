# Camera mode: design decisions

Tickets: #1572 (epic), #1573, #1576–#1579, presets #1231. This document says why camera mode's
overlay is built the way it is. How it is tested: `doc/testplans/1572-camera-mode.md`.

Camera mode replaces the interface with a camera: a bar around a shutter, the camera's values in a
strip, a viewfinder (grid, level, AF point) and Photo Studio in a rail. Each section names a
decision, the reason for it, and what it costs.

## 1. One RmlUi document over a model with no interface

**Decision.** The overlay is one RmlUi document on the new interface kit (`slcameraview.cpp`). What
the camera does lives in `slcameramodel.cpp`, which knows nothing about the interface. The input
decisions (click, double-click, the press that activates the window, the wheel's target, which keys
a command takes) are pure functions in `slcamerainput.h`, the bar's layout in `slcamerabarlayout.h`;
both are unit-tested by the `camera-input` and `camera-bar-layout` checks.

**Why.** The hand-drawn overlay mixed drawing, hit testing and camera logic, so every change to the
look risked the camera. The kit gives camera mode the same type, tokens and themes as the rest of the
new interface, smooth shapes and text, and layout by RCSS instead of pixel arithmetic.

**Cost.** RmlUi reports most mistakes only as a log line: unbalanced markup is accepted, a reserved
data-model name (`size`) is refused, an unbound callback does nothing. The `camera-rml` check fails
the build on each of these for the camera document.

## 2. Any UI renderer, and a way out

**Decision.** Camera mode asks the RmlUi host to install at its first entry, whatever `UIRenderer`
is, and the host keeps it for the session. While camera mode is open its document is the only one
shown. If RmlUi is poisoned or the document cannot start, the snapshot key opens the classic snapshot
window instead; a device reset being recovered is waited out (10 s), not treated as a failure.

**Why.** Camera mode must work for everyone, including the default classic renderer, and a broken
overlay must never leave someone without a way to take a picture.

**Cost.** In `UIRenderer` 0 the host stays installed after the first use of camera mode (it records
nothing while camera mode is closed).

## 3. Mouse: the meanings play already has

**Decision.** A click focuses where it lands; a double-click on the same spot focuses and shoots; the
on-screen shutter is the half-press (hold to focus, let go to shoot, slide off to cancel). A
right-click opens the quick settings at the pointer; a right-drag turns the view. The middle button
stays voice. The wheel over the picture always moves the camera in or out; over a value it turns
that value; Shift turns exposure, Ctrl zoom. The press that brings the window to the front does
nothing.

**Why.** Left acts on what is under the pointer and right is its menu, as in play; a shot can cost
L$ or publish, so it needs a deliberate gesture. The first build made the wheel turn a marked value
while the controls were open; in use nobody could tell what the wheel would do, so it now always
moves the camera over the picture.

Tilt (the Dutch angle) has its own arc at the frame's right, -40 to +40 degrees, shown with the
controls: drag or click along it, the wheel over it turns half a degree, a double-click levels. A
first build put a small knob on the end of the level line instead; it was too hard to hit.

**Cost.** A double-click on the same spot shoots, so clicking twice "to make sure" takes a picture.

## 4. Keys: the mode owns its keys

**Decision.** Enter shoots, B focuses, L locks the focus, V previews depth while held, Q opens the
quick settings, R the shots, H hides the bar, F1 the mouse and key card, Esc backs out one level. A
key bound to a camera command is the camera's even with a menu open: it never falls through to a
gesture. Only a field being typed in keeps its keys. Menu shortcuts that would open a window behind
camera mode do nothing; those that act on the world, the avatar or the camera still work (a list in
`SLCameraMode::blocksMenuShortcut`, checked against the menu by `camera-input`). One Enter press acts
once although it reaches the viewer twice (the key, then its character), and a held key's repeats
never confirm anything.

**Why.** Letters fell through to gestures whenever a menu was open, and Ctrl+P and friends opened
windows nobody could see. Review moved from P to R because P read as Program on the mode dial; help
moved from ? to F1 because ? is Shift+/ only on some keyboard layouts.

**Cost.** In camera mode F1 is the help card, not Photo Studio (the rail is Photo Studio there).

## 5. Where the shot goes

**Decision.** Save to disk is the default. Save to inventory, Share to profile and Send by e-mail
ask on a card first (the price, the caption, the address); "Don't ask again" lasts until camera mode
closes. While a card waits, the shutter refuses a second shot. Bursts and brackets to a destination
other than the disk save to disk instead, and say so.

**Why.** A shot to the inventory costs L$ and a profile post is public: neither may happen without
the person seeing what it costs and where it goes. A burst would otherwise pay or post once per frame.

**Cost.** One more step for people who upload every shot; "Don't ask again" covers a session.

## 6. Showing state

**Decision.** A toggle that is easy to forget (focus lock, depth preview, a timer, burst or bracket)
shows as a named pill beside the help button. Tooltips open over or beside their control after
0.3 s, laid out as name, one line, scale and key. A greyed rail row says why it is greyed.

**Why.** In use, focus lock and timers were switched on by a key and then forgotten. Tooltips centred
on the screen were read as being about the wrong control.

## 7. Outside camera mode

The overlay's needs fixed things other RmlUi surfaces share:

- RmlUi fields turn the window's text input on while they are typed in (AltGr, dead keys and input
  methods type their characters), copy and paste through the viewer's clipboard, and show the cursor
  their RCSS asks for. The new interface's fields use the same code. The rule is in
  `doc/DEVELOPER_GUIDE.md` §5.
- The classic snapshot window's "Current Window" and camera mode's Window are the same choice: an
  unset size (`SnapshotLocalWidth` / `Height` 0) that follows the window.
- `UIKitTheme` (0 dark, 1 light) picks the kit's theme, for camera mode and the rest of the new
  interface.
- Camera mode's look is the kit's standard (#703): its controls (value cells, chips, tiles, the handle,
  capsules, setting rows, the tip card, bar buttons, menu rows, keycaps, the drawn icons) live in
  `llnewuikitstyle.cpp`, its icons are `<ic name="..."/>` templates, and its colours and type sizes are
  kit tokens. The document keeps its layout only; the `newui-kit` check holds every new-interface
  document to that.

## 8. Not in this change

Phase 3 (#1227–#1230) is separate. Camera mode's text fields take no input-method composition
window yet (characters arrive, the composition box is not positioned).
