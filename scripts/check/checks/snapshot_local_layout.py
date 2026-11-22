"""Regression guard: nothing in the snapshot Disk panel hides behind its Save / Cancel buttons.

panel_snapshot_local.xml is laid out 380 px tall, but floater_snapshot.xml's panel_container
shows it 276 px tall (follows="all"): the bottom-following Save and Cancel buttons move up by
the difference while the top-following check boxes stay put. "Also save an HDR copy" (shown
only on an HDR display) ended up under the buttons, unclickable. The floater's status line
(status_hr and the "Saved" text) is drawn over the panel's last 16 px, so the buttons cannot
move down either: the options above have to fit over them.

Checks every top-following control placed with an absolute top="..." (the rows from the Name
line down) against the buttons' top in the shown panel, the buttons against the status line,
and that the check box labels fit their width at the default font (about 6 px a character).
"""
import re
import xml.etree.ElementTree as ET

NAME = "snapshot-local-layout"
STAGE = "source"
DESCRIPTION = "snapshot Disk panel: no option under the Save / Cancel buttons at the shown height"
GUARDS = ("A snapshot option (the HDR copy, the holdout mattes) drawn behind the Save / Cancel "
          "buttons, or its label cut off, because the panel is shown shorter than it is laid out.")

PANEL = "indra/newview/skins/default/xui/en/panel_snapshot_local.xml"
FLOATER = "indra/newview/skins/default/xui/en/floater_snapshot.xml"
GAP = 3             # px between the lowest option and the buttons
CHAR_PX = 6.2       # SansSerifSmall, a generous average
CHECK_BOX_PX = 20   # the box and its gap before the label


def run(ctx, result):
    panel = ET.parse(ctx.path(PANEL)).getroot()
    floater = ET.parse(ctx.path(FLOATER)).getroot()
    container = floater.find(".//panel_container[@name='panel_container']")
    if container is None:
        result.fail("%s: no panel_container" % FLOATER)
        return
    shown_h = int(container.get("height"))
    laid_h = int(panel.get("height"))
    status = floater.find(".//view_border[@name='status_hr']")
    # status_hr follows the container with a negative top_pad: drawn that far inside the panel
    status_y = shown_h + int(status.get("top_pad", "0")) if status is not None else shown_h

    buttons = [c for c in panel if c.get("name") in ("save_btn", "cancel_btn")]
    absolute = [int(c.get("top")) for c in buttons if c.get("top")]
    if not absolute or any("bottom" not in (c.get("follows") or "") for c in buttons):
        result.fail("%s: save_btn / cancel_btn must follow the bottom with a top=\"...\"" % PANEL)
        return
    button_top = absolute[0] - (laid_h - shown_h)
    button_bottom = button_top + max(int(c.get("height", "0")) for c in buttons)
    if button_bottom + GAP > status_y:
        result.fail("%s: the Save / Cancel buttons end at %d, into the floater's status line at %d"
                    % (PANEL, button_bottom, status_y))

    lowest = 0
    for c in panel:
        follows = c.get("follows") or ""
        if c.get("name") in ("save_btn", "cancel_btn") or not c.get("top") or "top" not in follows:
            continue
        bottom = int(c.get("top")) + int(c.get("height", "0"))
        lowest = max(lowest, bottom)
        if bottom + GAP > button_top:
            result.fail("%s: %s ends at %d, under the Save / Cancel buttons at %d in the %d px tall "
                        "panel the snapshot floater shows" % (PANEL, c.get("name"), bottom, button_top, shown_h))
        if c.tag == "check_box" and c.get("label") and c.get("width"):
            need = CHECK_BOX_PX + len(c.get("label")) * CHAR_PX
            if need > int(c.get("width")):
                result.fail("%s: %s label \"%s\" needs ~%d px, has %s (cut off)"
                            % (PANEL, c.get("name"), c.get("label"), need, c.get("width")))
    result.summary = ("options end at %d, buttons %d..%d, status line %d (panel shown %d of %d px)"
                      % (lowest, button_top, button_bottom, status_y, shown_h, laid_h))
