# Test plan: camera mode on the new interface kit (#1572)

Camera mode's overlay is one RmlUi document (`slcameraview.cpp`) over a UI-free model
(`slcameramodel.cpp`). It replaces the hand-drawn overlay and runs whatever `UIRenderer` is set.
Run on Windows and Linux; record the GPU and the UI scale.

## Setup

- A region with an avatar, a few objects at different distances and some light.
- `SLCameraModeOverlay` on (the default). Snapshot (Ctrl+Shift+S, or the toolbar button) enters and
  leaves camera mode.
- Repeat the "Entering and leaving" and "Looks" sections with `UIRenderer` 0, 1 and 2, and with
  `UIKitTheme` 0 (dark) and 1 (light).

## Entering and leaving

1. Enter: the interface and HUDs hide, the bar and shutter show. With `UIRenderer` 0 (RmlUi never
   started this session) the overlay still shows.
2. Esc closes the innermost open thing (a popover, the card, a menu), then leaves. Leaving brings the
   interface and HUDs back exactly as they were, and the other new-interface surfaces return
   (`UIRenderer` 1 / 2).
3. Leave and enter again: no stale state (no open card, no held focus, no waiting shot, the toast gone).
4. If the overlay cannot start (the log says `camera overlay unavailable`), the classic snapshot
   window opens instead. A device-lost recovery must not do this (the overlay comes back within 10 s).

## Mouse

5. The click that brings the window to the front does nothing (no focus, no shot).
6. Click the picture: the AF point moves there and focuses. Double-click the same spot: it shoots.
   Two clicks far apart, or slower than the double-click time: no shot.
7. Drag the AF point: depth of field follows while dragging (not only on release).
8. Right-click without moving: quick settings at the pointer. Right-drag: the view turns; the avatar
   never turns.
9. Wheel over the picture: the camera moves in or out, Controls open or not. Wheel over a strip value:
   that value. Shift + wheel: exposure. Ctrl + wheel: zoom. Over the Grid chip: the next grid.
10. Hold the on-screen shutter: focus; let go over it: a shot; slide off: cancelled.
10b. Tilt (Controls open, Level on or off): click or drag anywhere along the arc at the frame's right;
    the wheel over it turns half a degree; double-click levels (the knob turns green at 0°).

## Keys

11. Enter shoots once per press, never twice. With "Save to inventory" chosen, Enter on the card
    confirms it and takes no second shot; holding Enter while shooting does not confirm the card.
12. B, L, V, Q, H, R and F1 do their job with a popover, the help card or the quick settings open, and
    never trigger a gesture bound to the same letter.
13. Typing in a field (custom resolution, preset name, e-mail fields) keeps every key; Enter leaves the
    field and does not shoot or send.
14. Menu shortcuts that open windows (Ctrl+P, Ctrl+I, Ctrl+M) do nothing and make no sound; F1 opens the
    help card, not Photo Studio. These still work: fullscreen, quit, Ctrl+Shift+S (leaves), the sky presets (Ctrl+Shift+Y
    etc.), zoom (Ctrl+8 / 9 / 0), freeze world, Home (fly), sit, always run, stop and resync animations.
15. After deleting a shot from the card's more menu, Space does not delete the next one.

## Looks

16. UI scale 1.0 and 1.5, `SLCameraModeOverlaySize` 1.0 and 1.5: nothing overlaps, nothing is cut off
    at 1280 wide, the strip is centred on the shutter, circles are smooth.
17. Tooltips open over or beside the hovered control (strip values, quick settings in both rows,
    Photo Studio rows, the Flash chip) after a short rest and stay on screen at the edges.
18. Focus lock, depth preview and a timer / burst / bracket show as pills beside the help button.
19. Resolution: each row can be picked; Window follows the window after a resize; Custom shows the
    fields. "As resolution" fits its aspect segment.
20. Grid: pointing at an option previews it; the Grid and Level chips open their menus under them.
21. Photo Studio rail: docks left or right, a changed row can be reset; the strip stays
    centred.

## The shot

22. A shot with everything open (strip, a popover, the card, the hints, the rail) contains no overlay
    pixels, in all three `UIRenderer` modes.
23. Save to disk: the file lands in the snapshot folder; the thumbnail and card show it; Show in folder
    opens it. Resolution, aspect, PNG / JPEG (quality) / BMP and post effects are honoured.
24. Save to inventory (costs L$): the card says the price and asks; Cancel uploads nothing. "Don't ask
    again" lasts until camera mode closes. With too little L$, the shot is refused with a message.
25. Share to profile and Send by e-mail: the card asks; an invalid address is refused; the last address
    is offered again, the message is not.
26. While a card waits, the shutter and a timer refuse a second shot with a notice.
27. Bursts and brackets with a non-disk destination save to disk and say so. Without a snapshot folder,
    the first single shot to disk picks it.
28. One shutter sound per shot, including inventory uploads; the classic snapshot window's inventory
    save still plays it.
29. Flash chip (the lightning bolt beside Level, top right, with Controls open): click it off (the bolt
    is struck through, no accent border). A shot to disk and one to inventory show no white flash; the
    shutter sound, the thumbnail and the "Saved" notice are unchanged. Restart the viewer: still off
    (`SLCameraModeFlash` false). Click it on: the flash is back, and it is never in the saved picture.
