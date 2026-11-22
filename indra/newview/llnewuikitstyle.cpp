// #703 / #1242 The new interface's widget kit: tokens, the component stylesheet and the control
// templates (see llnewuikitstyle.h). Standard C++ only: the offscreen lab links this file too.
#include "llnewuikitstyle.h"

#include <algorithm>
#include <cstring>
#include <string>

namespace
{
// The component stylesheet. Sizes in dp (UI units: they follow the UI size and the display
// scale). Spacing grid 4 dp; type scale caption 11 / body 13 / title 15 / display 20 ($font*).
// The look is camera mode's (#1572): floating surfaces with 10 / 14 dp corners, chips, value
// cells, keycaps, drawn icons, the tip card. Documents carry layout only.
//
// Order matters: a document's own <style> follows this one, and rules of equal specificity
// later in this sheet win over earlier ones. The camera mode section keeps camera mode's own
// order; the sections after it use class names of their own.
const char* kKit = R"rcss(
body { font-family: Inter; font-size: $fontBody; color: $text; font-effect: $textEffect; } /* inherited */
div, span, p { display: block; }
span { display: inline; }
.caption { font-size: $fontCaption; color: $muted; }
.title { font-size: $fontTitle; font-weight: bold; }
.muted { color: $muted; }
/* text that sits on the world (no surface under it): a scrim behind it and the outline */
.onworld { font-effect: $textOutline; }

.glass .bar, .glass .panel, .glass .popover { backdrop-filter: blur(12px); }
.spacer { flex: 1 1 0; }

/* btn: small text or icon buttons inside surfaces (panel heads, card actions) */
.btn { display: block; height: 22dp; line-height: 22dp; padding: 0 8dp; border-radius: $radius; color: $text;
       background-color: transparent; cursor: pointer; white-space: nowrap; }
.btn:hover { background-color: $hover; }
.btn:active { background-color: $pressed; }
.btn:focus { border: 1dp $accent; }
.btn.accent { background-color: $accent; color: $onAccent; font-effect: none; } /* opaque fill; dark text */

/* field: text inputs */
.field { display: block; box-sizing: border-box; height: 22dp; line-height: 22dp; padding: 0 8dp; border-radius: $radius; background-color: $field;
         border: 1dp $border; color: $text; }
.field:hover { border-color: $borderStrong; }
.field:focus { border-color: $accent; }
/* A rounded text input clips its text through a full-screen mask texture: the rounded box is
   a wrapper (.field), the input inside it is square and bare (rectangular scissor only). */
input.bare { display: block; width: 100%; height: 100%; font-family: Inter; font-size: $fontBody; color: $text;
             background-color: transparent; border-width: 0; cursor: text; }

/* panel / popover: surfaces */
.panel { background-color: $surface; border: 1dp $border; border-radius: $radiusPanel; }
.popover { position: absolute; background-color: $raised; border: 1dp $border; border-radius: $radiusPopover;
           border-top-color: $borderStrong; padding: 8dp; }
/* No box-shadow: RmlUi bakes it through full-screen layers (a 4K target spent the UI image
   budget on one popover). Depth comes from the scrim and the raised surface. */
.scrim { position: absolute; left: 0; top: 0; right: 0; bottom: 0; background-color: $scrim; }

/* segmented: a small choice of views (People / Nearby / Chats, aspect ratios) */
.segmented { display: flex; flex-direction: row; background-color: $field; border: 1dp $border; border-radius: $radius; padding: 2dp; gap: 2dp; }
.seg { flex: 1 1 0; text-align: center; height: 20dp; line-height: 20dp; border-radius: 4dp; color: $muted; cursor: pointer; }
.seg:hover { background-color: $hover; }
.seg.on { background-color: $selected; color: $text; }

/* toggle: a boolean setting */
.toggle { display: block; width: 30dp; height: 16dp; border-radius: 8dp; background-color: $chip; position: relative; cursor: pointer; }
.toggle .knob { position: absolute; left: 2dp; top: 2dp; width: 12dp; height: 12dp; border-radius: 6dp; background-color: $muted; }
.toggle.on { background-color: $accent; }
.toggle.on .knob { left: 16dp; background-color: $onAccent; }

/* slider (RmlUi range input) */
input.range { display: block; width: 120dp; height: 16dp; }
input.range slidertrack { height: 4dp; margin-top: 6dp; background-color: $chip; border-radius: 2dp; }
input.range sliderbar { width: 12dp; height: 12dp; margin-top: 2dp; border-radius: 6dp; background-color: $accent; }
input.range sliderbar:hover { background-color: $text; }

/* ---- camera mode's controls (#1572): the standard of the new interface, in its order ---- */
/* value cells: a value over its name; step arrows on the hovered cell (up = forward) */
.cell { display: inline-block; padding: 4dp 18dp 3dp 18dp; border-radius: $radiusControl; text-align: center; cursor: pointer;
        border-bottom: 2dp transparent; }
.cell:hover { background-color: $hover; }
.cell .v { font-size: $fontTitle; font-weight: bold; }
.cell .n { font-size: $fontCaption; color: $muted; }
.cell.off .v { color: $muted; font-weight: normal; }
.cell.marked { background-color: $selected; border-bottom-color: $accent; }
/* chips: round buttons on the world (grid, level, Photo Studio) */
.cbtn { height: 26dp; line-height: 26dp; padding: 0 12dp; border-radius: $radiusChip; background-color: $bar; border: 1dp $border;
        pointer-events: auto; cursor: pointer; white-space: nowrap; }
.cbtn:hover { background-color: $raised; }
.cbtn.on { border-color: $accent; }
/* tiles: a value over its name in a grid (quick settings) */
.qcell { width: 112dp; height: 48dp; padding-top: 4dp; border-radius: $radiusControl; background-color: $chip; text-align: center; cursor: pointer;
         border: 1dp transparent; }
.qcell:hover { background-color: $hover; border-color: $borderStrong; }
.qcell.marked { border-color: $accent; }
.qcell .v { font-size: $fontTitle; font-weight: bold; white-space: nowrap; overflow: hidden; }
.qcell .n { font-size: $fontCaption; color: $muted; }
/* in the cells' side padding, so they cost no width */
.cell, .qcell { position: relative; }
.cell .arr, .qcell .arr { position: absolute; top: 5dp; width: 16dp; height: 16dp; visibility: hidden; cursor: pointer;
                          text-align: center; line-height: 12dp; }
.cell .arr.up, .qcell .arr.up { left: 1dp; } .cell .arr.dn, .qcell .arr.dn { right: 1dp; }
.cell:hover .arr, .qcell:hover .arr { visibility: visible; }
.arr .chev { margin: 0; border-color: $muted; }
.arr:hover .chev { border-color: $accent; }
/* handle: a grip that brings a bar back or puts it away */
.handle { width: 48dp; height: 14dp; pointer-events: auto; cursor: pointer; }
.handle .grip { margin: 5dp 2dp 0 2dp; height: 4dp; border-radius: 2dp; background-color: $borderStrong; }
.handle:hover .grip { background-color: $text; }
/* capsule: a notice on the world (status, saved, hints, toasts) */
.capsule { display: inline-block; padding: 4dp 12dp; background-color: $bar; border: 1dp $border; border-radius: $radiusPill; }
.capsule.raised { background-color: $raised; }
.link { color: $accent; cursor: pointer; }
/* setting rows (Photo Studio's rail): a section, then label / control / value / back-to-default */
.rsec { height: 24dp; line-height: 24dp; color: $accent; cursor: pointer; }
.rsec .chev { margin: 0 8dp 0 2dp; border-color: $accent; }
.rrow { display: flex; flex-direction: row; align-items: center; height: 24dp; gap: 6dp; }
.rrow.dim { opacity: 0.45; }
.rrow .rl { width: 112dp; white-space: nowrap; overflow: hidden; text-overflow: ellipsis; color: $muted; }
.rrow .rv { width: 52dp; text-align: right; white-space: nowrap; overflow: hidden; }
/* back to the default: shown on rows whose setting was changed (a small circular arrow) */
.rreset { position: relative; width: 16dp; height: 16dp; flex: 0 0 16dp; visibility: hidden; cursor: pointer; }
.rreset.shown { visibility: visible; }
.rreset .ring { position: absolute; left: 2dp; top: 2dp; width: 12dp; height: 12dp; box-sizing: border-box; border-radius: 6dp;
                border: 2dp $muted; border-top-color: transparent; transform: rotate(-45deg); }
.rreset .tip { position: absolute; left: 1dp; top: 1dp; width: 5dp; height: 5dp; border-top: 2dp $muted; border-left: 2dp $muted; }
.rreset:hover .ring { border-color: $accent; border-top-color: transparent; }
.rreset:hover .tip { border-color: $accent; }
.rtrack { flex: 1 1 0; height: 14dp; cursor: pointer; }
.rtrack .tr { margin-top: 5dp; height: 4dp; border-radius: 2dp; background-color: $chip; }
.rtrack .fl { height: 4dp; border-radius: 2dp; background-color: $accent; }
.rchoice { flex: 1 1 0; display: flex; flex-direction: row; }
.rchoice .arr { width: 16dp; text-align: center; cursor: pointer; }
.rchoice .arr .chev { margin: 0; }
.rchoice .arr:hover .chev { border-color: $accent; }
.rchoice .cv { flex: 1 1 0; text-align: center; white-space: nowrap; overflow: hidden; cursor: pointer; }
.rchoice .cv:hover { color: $accent; }
.rbtns { flex: 1 1 0; display: flex; flex-direction: row; gap: 4dp; }
.rbtns .btn { flex: 1 1 0; text-align: center; border: 1dp $border; padding: 0 2dp; overflow: hidden; }
/* the tip card: a bold title, what it does, a low-to-high scale where there is one, what a click does */
.tipcard .tt { font-weight: bold; margin-bottom: 2dp; }
.tipcard .tw { color: $muted; }
/* the scale's ends at the two sides; when they do not fit side by side, the high end wraps under,
   still on the right (never past the card's edge) */
.tipcard .ts { display: flex; flex-direction: row; flex-wrap: wrap; margin-top: 6dp; column-gap: 12dp; row-gap: 2dp; }
.tipcard .ts .lo { flex: 0 0 auto; white-space: nowrap; }
.tipcard .ts .hi { flex: 0 0 auto; margin-left: auto; white-space: nowrap; }
.tipcard .ts .chev { margin: 0 6dp; }
.tipcard .tc { margin-top: 6dp; color: $muted; } .tipcard .tc .hk { margin-right: 6dp; }
.tipcard { display: block; padding: 8dp 12dp; background-color: $raised; border: 1dp $border; border-radius: $radiusControl;
           text-align: left; }
.numeric { font-family: DejaVu Sans Mono; }

/* bar buttons: a label (and its icon) in a floating bar */
.bb { position: absolute; top: 6dp; height: 44dp; line-height: 44dp; text-align: center; border-radius: $radiusControl; color: $text;
      cursor: pointer; white-space: nowrap; overflow: hidden; font-size: $fontTitle; }
.bb:hover { background-color: $hover; }
.bb, .cbtn, .cell, .opt, .seg, .qcell, .btn { tab-index: auto; }
.bb:focus, .cbtn:focus, .cell:focus, .opt:focus, .seg:focus, .qcell:focus { border-color: $accent; }
.bb:active { background-color: $pressed; }
.bb.on { background-color: $selected; }
.bb.small { font-size: $fontBody; }
/* icons, drawn (the faces lack these glyphs): chevron, close, download, sliders, more, ...
   Write them as <ic name="x"/> (LLNewUIKit::themed expands the parts). */
.chev { display: inline-block; vertical-align: middle; width: 6dp; height: 6dp; margin: 0 2dp 0 9dp;
        border-left: 1.5dp $muted; border-top: 1.5dp $muted; transform: rotate(45deg); }
.chev.down { transform: rotate(225deg); margin-top: -4dp; }
.chev.right { transform: rotate(135deg); }
.chev.left { transform: rotate(-45deg); }
.ic { display: inline-block; position: relative; vertical-align: middle; }
.ic div { position: absolute; background-color: $text; }
.ic-x { width: 12dp; height: 12dp; margin-right: 9dp; }
.ic-x .a { left: -1dp; top: 5.25dp; width: 14dp; height: 1.5dp; border-radius: 1dp; transform: rotate(45deg); }
.ic-x .b { left: -1dp; top: 5.25dp; width: 14dp; height: 1.5dp; border-radius: 1dp; transform: rotate(-45deg); }
.ic-dl { width: 14dp; height: 15dp; margin-right: 9dp; }
.ic-dl .stem { left: 6.25dp; top: 0; width: 1.5dp; height: 10dp; }
.ic-dl .head { left: 3.5dp; top: 4dp; width: 6dp; height: 6dp; background-color: transparent; border-right: 1.5dp $text; border-bottom: 1.5dp $text;
               transform: rotate(45deg); }
.ic-dl .base { left: 0; top: 13.5dp; width: 14dp; height: 1.5dp; }
/* destinations: inventory (a box), profile (a person), e-mail (an envelope) */
.ic-box { width: 14dp; height: 14dp; margin-right: 9dp; }
.ic-box .lid { left: 0; top: 0; width: 14dp; height: 4dp; box-sizing: border-box; background-color: transparent; border: 1.5dp $text;
               border-radius: 1dp; }
.ic-box .body { left: 1.5dp; top: 4dp; width: 11dp; height: 10dp; box-sizing: border-box; background-color: transparent;
                border: 1.5dp $text; border-top-width: 0; }
.ic-person { width: 14dp; height: 14dp; margin-right: 9dp; }
.ic-person .head { left: 4dp; top: 0; width: 6dp; height: 6dp; box-sizing: border-box; border-radius: 3dp; background-color: transparent;
                   border: 1.5dp $text; }
.ic-person .shoulders { left: 1dp; top: 8dp; width: 12dp; height: 7dp; box-sizing: border-box; background-color: transparent;
                        border: 1.5dp $text; border-bottom-width: 0; border-radius: 6dp 6dp 0 0; }
.ic-mail { width: 16dp; height: 12dp; margin-right: 9dp; }
.ic-mail .frame { left: 0; top: 0; width: 16dp; height: 12dp; box-sizing: border-box; background-color: transparent; border: 1.5dp $text;
                  border-radius: 2dp; }
.ic-mail .v1 { left: 1dp; top: 4dp; width: 8.5dp; height: 1.5dp; transform: rotate(33deg); }
.ic-mail .v2 { left: 6.5dp; top: 4dp; width: 8.5dp; height: 1.5dp; transform: rotate(-33deg); }
/* window: a frame with its title band */
.ic-win { width: 15dp; height: 13dp; margin-right: 10dp; }
.ic-win .frame { left: 0; top: 0; width: 15dp; height: 13dp; box-sizing: border-box; background-color: transparent; border: 1.5dp $text;
                 border-radius: 2dp; }
.ic-win .band { left: 0; top: 3.5dp; width: 15dp; height: 1.5dp; }
/* dock: a screen with the side the panel would move to filled in */
.ic-dock { width: 16dp; height: 12dp; }
.ic-dock .frame { left: 0; top: 0; width: 16dp; height: 12dp; box-sizing: border-box; background-color: transparent; border: 1.5dp $text;
                  border-radius: 2dp; }
.ic-dock .side { left: 10dp; top: 0; width: 6dp; height: 12dp; border-top-right-radius: 2dp; border-bottom-right-radius: 2dp; }
.ic-dock.toleft .side { left: 0; border-radius: 2dp 0 0 2dp; }
/* mouse: a rounded body with the button seam */
.ic-mouse { width: 12dp; height: 17dp; margin-right: 12dp; }
.ic-mouse .body { left: 0; top: 0; width: 12dp; height: 17dp; box-sizing: border-box; background-color: transparent; border: 1.5dp $muted;
                  border-radius: 6dp; }
.ic-mouse .seam { left: 5.25dp; top: 2dp; width: 1.5dp; height: 5dp; background-color: $muted; }
/* the part the row is about, lit: a button, the wheel (the seam), the middle press (a dot) */
.ic-mouse .bl, .ic-mouse .br { top: 1.5dp; width: 3.5dp; height: 6dp; background-color: transparent; }
.ic-mouse .bl { left: 1.5dp; border-top-left-radius: 4dp; } .ic-mouse .br { left: 7dp; border-top-right-radius: 4dp; }
.ic-mouse.left .bl, .ic-mouse.right .br { background-color: $accent; }
.ic-mouse.wheel .seam { left: 4.75dp; top: 1.5dp; width: 2.5dp; height: 7dp; border-radius: 1dp; background-color: $accent; }
.ic-mouse.middle .seam { left: 4dp; top: 2.5dp; width: 4dp; height: 4dp; border-radius: 2dp; background-color: $accent; }
/* the on-screen shutter, small */
.ic-shut { width: 16dp; height: 16dp; margin-right: 8dp;
           decorator: radial-gradient(circle closest-side, $text 55%, $text00 63%, $text00 74%, $text 81%, $text 91%, $text00 100%); }
.ic-sl { width: 16dp; height: 14dp; margin-right: 9dp; }
.ic-sl .l1 { left: 0; top: 2.25dp; width: 16dp; height: 1.5dp; } .ic-sl .l2 { left: 0; top: 10.25dp; width: 16dp; height: 1.5dp; }
.ic-sl .d1 { left: 3dp; top: 0; width: 6dp; height: 6dp; border-radius: 3dp; background-color: $bar; border: 1.5dp $text; box-sizing: border-box; }
.ic-sl .d2 { left: 8dp; top: 8dp; width: 6dp; height: 6dp; border-radius: 3dp; background-color: $bar; border: 1.5dp $text; box-sizing: border-box; }
.ic-more { width: 18dp; height: 4dp; }
.ic-more div { top: 0; width: 4dp; height: 4dp; border-radius: 2dp; }
.ic-more .a { left: 0; } .ic-more .b { left: 7dp; } .ic-more .c { left: 14dp; }
/* keycaps and the words beside them (hint rows, menus, tips) */
.hk { display: inline-block; padding: 0 5dp; border: 1dp $borderStrong; border-radius: $radius; color: $text; }
.ht { display: inline-block; color: $muted; margin: 0 6dp; }
.ht.end { margin-right: 14dp; }
.sep { height: 1dp; background-color: $border; margin: 10dp 0; }
/* menu rows: an icon or mark, the label (and a caption under it), a tick on the current one */
.opt { display: flex; flex-direction: row; align-items: center; padding: 6dp 8dp; border-radius: $radiusControl; cursor: pointer; }
.opt:hover { background-color: $hover; }
.opt.selected { background-color: $selected; }
.opt .col { flex: 1 1 0; }
.opt > .ic { margin-left: 2dp; }
.opt .tick { color: $accent; width: 16dp; }
.radio { width: 14dp; height: 14dp; border-radius: 7dp; border: 2dp $muted; margin-right: 10dp; }
.radio.on { border-color: $accent; background-color: $accent; }
.opt .note { color: $muted; font-size: $fontCaption; margin-left: 8dp; }
.trow { display: flex; flex-direction: row; align-items: center; padding: 6dp 8dp; cursor: pointer; }
.trow .label { flex: 1 1 0; }
/* a text box of several lines: its lines (and the caret), not a single-line field's */
textarea.bare { display: block; width: 100%; height: 100%; font-family: Inter; font-size: $fontBody; line-height: 18dp; color: $text;
                background-color: transparent; border-width: 0; cursor: text; }
.field.tall { height: 64dp; line-height: 18dp; padding: 5dp 8dp; }

/* ---- the rest of the new interface, on the same controls ---- */
/* floatbar: a floating surface for bars and grids, as camera mode's bar */
.floatbar { background-color: $bar; border: 1dp $border; border-radius: $radiusPill; pointer-events: auto; }
/* barwrap: a full-width line that centres its bar (the bar is an inline block) */
.barwrap { position: absolute; left: 0; right: 0; text-align: center; pointer-events: none; }
/* the main bar: the location and the command field, centred at the top (or bottom) edge */
.bar { display: inline-block; box-sizing: border-box; height: $barH; padding: 2dp; white-space: nowrap; text-align: left; }
.bar .chip, .bar .cbtn { display: inline-block; vertical-align: top; margin-right: 4dp; }
.bar .cbtn:last-child, .bar .chip:last-child { margin-right: 0; }
/* chip: a value shown as a chip (not a button) */
.chip { height: 26dp; line-height: 26dp; padding: 0 12dp; border-radius: $radiusChip; background-color: $chip; color: $text;
        white-space: nowrap; overflow: hidden; text-overflow: ellipsis; }
/* a field look-alike that opens something (the command surface): a chip with a search mark */
.cbtn.cmd { color: $muted; text-align: left; }
.cbtn.cmd .hk { margin-left: 10dp; line-height: 18dp; }
.ic-find { width: 13dp; height: 13dp; margin-right: 8dp; }
.ic-find .ring { left: 0; top: 0; width: 10dp; height: 10dp; box-sizing: border-box; border-radius: 5dp; background-color: transparent;
                 border: 1.5dp $muted; }
.ic-find .stem { left: 8dp; top: 9.5dp; width: 5dp; height: 1.5dp; border-radius: 1dp; background-color: $muted; transform: rotate(45deg); }
/* list rows of a menu-like list (the command surface): .opt rows with a kind tag and a keycap */
.list { display: block; } /* bounded rows; no overflow clip (a clip under rounded corners costs a mask) */
.opt .kind { font-size: $fontCaption; color: $accent; width: 56dp; white-space: nowrap; }
.opt .label { flex: 1 1 0; white-space: nowrap; overflow: hidden; text-overflow: ellipsis; }
.opt .path { color: $muted; font-size: $fontCaption; white-space: nowrap; margin-left: 8dp; }
.opt .hk { margin-left: 8dp; font-size: $fontCaption; }
/* keycaps side by side (Up Down) */
.hk + .hk { margin-left: 4dp; }
.keys { margin-top: 8dp; }

/* tool grid: the toolbar commands as icons in a floating grid (bottom centre) */
.tools { display: inline-block; padding: 4dp; text-align: left; white-space: normal; }
.tool { display: inline-block; position: relative; width: 40dp; height: 40dp; margin: 2dp; border-radius: $radiusControl; cursor: pointer;
        vertical-align: top; }
.tool:hover { background-color: $hover; }
.tool:active { background-color: $pressed; }
.tool.on { background-color: $selected; }
.tool.off { opacity: 0.45; cursor: default; }
.tool.off:hover { background-color: transparent; }
.tool .icon { position: absolute; left: 9dp; top: 9dp; width: 22dp; height: 22dp; image-color: $text; }
.tool.on .icon { image-color: $accent; }
/* the grid's grip: in the grid's top edge; a click locks the grid open (lit), another lets it go */
.tools .handle { display: block; margin: 0 auto 2dp auto; height: 10dp; }
.tools .handle .grip { margin-top: 3dp; }
.tools.locked .handle .grip { background-color: $accent; }
/* the pull-up tab: a small tab flush with the bottom edge, in a wider invisible area that opens the
   grid when the pointer reaches it */
.toolstab { display: inline-block; width: 120dp; height: 16dp; pointer-events: auto; cursor: pointer; }
.toolstab .tabface { margin: 6dp 24dp 0 24dp; height: 10dp; box-sizing: border-box; padding-top: 4dp; background-color: $bar;
                     border: 1dp $border; border-bottom-width: 0; border-radius: 6dp 6dp 0 0; }
.toolstab .grip { margin: 0 22dp; height: 3dp; border-radius: 2dp; background-color: $muted; }
.toolstab:hover .grip { background-color: $text; }

/* ---- windows (llnewuiwindow.h): a floating panel, its title bar (<whead/>) and resize grip (<wgrip/>) ----
   The surface is the panel token (>= 0.92 opaque in both themes): text never sits on raw glass.
   No overflow clip on the rounded window (a clip under rounded corners costs a mask); its lists
   clip in square viewports inside it. */
body.window { position: absolute; display: flex; flex-direction: column; box-sizing: border-box; background-color: $solid;
              border: 1dp $border; border-radius: $radiusPanel; pointer-events: auto; }
/* Translucent only where the blur backs it: without it (HDR, large targets) a 0.92 surface still
   shows the scene's detail through the text (seen at 4K), so the window is opaque. */
body.window.glass { background-color: $surface; backdrop-filter: blur(12px); }
body.window .popover { background-color: $solidRaised; }
body.window.active { border-color: $borderStrong; }
.wframe { display: flex; flex-direction: column; flex: 1 1 0; min-height: 0; } /* the data model's element: the whole window */
.whead { display: flex; flex-direction: row; align-items: center; flex: 0 0 30dp; height: 30dp; padding: 0 3dp 0 12dp; gap: 2dp;
         border-bottom: 1dp $border; }
.window.min .whead { border-bottom-width: 0; }
.wtitle { flex: 1 1 0; font-weight: bold; white-space: nowrap; overflow: hidden; text-overflow: ellipsis; color: $muted; }
.window.active .wtitle { color: $text; }
.wbtn { display: flex; align-items: center; justify-content: center; flex: 0 0 24dp; width: 24dp; height: 24dp; border-radius: $radius;
        cursor: pointer; tab-index: auto; }
.wbtn:hover { background-color: $hover; }
.wbtn:active { background-color: $pressed; }
.wbtn:focus { border: 1dp $accent; }
.wbtn .ic { margin: 0; }
.wbody { flex: 1 1 0; min-height: 0; display: flex; flex-direction: column; gap: 6dp; padding: 8dp; position: relative; }
.window.min .wbody, .window.min .wgrip { display: none; }
.wgrip { position: absolute; right: 1dp; bottom: 1dp; width: 20dp; height: 20dp; z-index: 20; pointer-events: auto; cursor: nwse-resize; }
.wgrip div { position: absolute; height: 1.5dp; background-color: $borderStrong; transform: rotate(-45deg); }
.wgrip .a { right: -1dp; bottom: 5dp; width: 12dp; } .wgrip .b { right: -1dp; bottom: 2dp; width: 6dp; }
.wgrip:hover div { background-color: $text; }
.ic-min { width: 12dp; height: 12dp; }
.ic-min .a { left: 0; top: 8dp; width: 12dp; height: 1.5dp; border-radius: 1dp; }
/* tabs: the views of a window (conversations, people's lists, inventory's) and their unread badge */
.tabs { display: flex; flex-direction: row; flex: 0 0 auto; gap: 2dp; border-bottom: 1dp $border; }
.tab { flex: 0 1 auto; height: 26dp; line-height: 26dp; padding: 0 10dp; color: $muted; cursor: pointer; white-space: nowrap;
       overflow: hidden; text-overflow: ellipsis; border-bottom: 2dp transparent; max-width: 180dp; tab-index: auto; }
.tab:hover { background-color: $hover; color: $text; }
.tab.on { color: $text; border-bottom-color: $accent; }
.tab:focus { color: $accent; }
.badge { display: inline-block; margin-left: 6dp; padding: 0 5dp; height: 16dp; line-height: 16dp; border-radius: 8dp;
         background-color: $accent; color: $onAccent; font-size: $fontCaption; font-weight: bold; }
/* a row of action buttons under a list (IM, Profile, Teleport ...) */
.actions { display: flex; flex-direction: row; flex-wrap: wrap; flex: 0 0 auto; gap: 4dp; padding-right: 16dp; }
.actions .btn { border: 1dp $border; }
.actions .btn.off { opacity: 0.45; cursor: default; }
.actions .btn.off:hover { background-color: transparent; }
/* a context menu (right-click, or the more button): .opt rows in a popover at the pointer */
.menu { position: absolute; z-index: 10; min-width: 180dp; padding: 4dp; }
.menu .opt { padding: 4dp 10dp; }
.menu .opt.off { opacity: 0.45; cursor: default; }
.menu .sep { margin: 4dp 0; }
/* where a list is in its length: a thin bar at the right of its viewport */
.vbar { position: absolute; right: 0; top: 0; bottom: 0; width: 4dp; }
.vthumb { position: absolute; right: 0; width: 4dp; border-radius: 2dp; background-color: $borderStrong; }
/* RmlUi otherwise omits the scissor when bottom-anchored content overflows upward:
   negative overflow does not increase scroll height. Always clip draw AND hit-test. */
.viewport { position: relative; flex: 1 1 0; min-height: 0; overflow: hidden; clip: always; } /* square: a rectangular scissor, no mask */
.vrow { position: absolute; left: 0; right: 0; height: 22dp; line-height: 22dp; padding: 0 8dp; white-space: nowrap; border-radius: $radius;
        overflow: hidden; cursor: pointer; display: flex; flex-direction: row; align-items: center; gap: 6dp; }
.vrow:hover { background-color: $hover; }
.vrow.selected { background-color: $selected; }
.vrow .label { flex: 1 1 0; white-space: nowrap; overflow: hidden; text-overflow: ellipsis; }
.vrow .path { color: $muted; font-size: $fontCaption; white-space: nowrap; }
.vrow .ico { width: 16dp; height: 16dp; flex: 0 0 16dp; }
.vrow .twist { width: 12dp; flex: 0 0 12dp; }
.vrow .twist .chev { margin: 0 0 0 2dp; }
.dot { width: 8dp; height: 8dp; border-radius: 4dp; background-color: $chip; flex: 0 0 8dp; }
.dot.on { background-color: $ok; }
.messages { flex: 1 1 0; display: flex; flex-direction: column; justify-content: flex-end; overflow: hidden; }
.msg { padding: 2dp 4dp; }
.msg .text { white-space: pre-wrap; }
.chatlink { color: $accent; text-decoration: underline; cursor: pointer; }
.msg .from { color: $accent; font-weight: bold; }
.msg .time { color: $muted; font-size: $fontCaption; }

/* ---- the status bar and its menus (llnewuistatus.cpp, llnewuimenus.cpp) ---- */
/* state text: good / warning / bad (frame rate, ping, packet loss); the frame generation rate in accent */
.good { color: $good; } .warn { color: $warn; } .bad { color: $bad; } .fgen { color: $accent; }
/* the status bar: the viewer's state in a small floating bar (top right), each part a segment */
.stats { display: inline-block; box-sizing: border-box; height: $barH; padding: 2dp; white-space: nowrap; text-align: left; }
.st { display: inline-block; vertical-align: top; height: 26dp; line-height: 26dp; padding: 0 7dp; border-radius: $radiusChip; cursor: pointer;
      white-space: nowrap; }
.st:hover { background-color: $hover; }
.st:active { background-color: $pressed; }
.st.on { background-color: $selected; }
.st.off { opacity: 0.45; }
.st.info { cursor: default; } .st.info:hover { background-color: transparent; }
.st .v { font-weight: bold; }
.st .u { font-size: $fontCaption; color: $muted; margin-left: 2dp; }
.st .u + .v { margin-left: 6dp; }
.st .sic { display: inline-block; vertical-align: middle; width: 16dp; height: 16dp; image-color: $text; }
.st .sic + .sic, .st .sic + .v, .st .v + .sic { margin-left: 5dp; }
.st .sic.warn { image-color: $warn; } .st .sic.bad { image-color: $bad; }
.st .ic-more { margin: 0 2dp; }
/* a menu of rows (the status bar's menus): a mark column, the label, its keys, a chevron to a submenu */
.scrim.clear { background-color: transparent; }
.menu { padding: 6dp; }
.menu .mhead { display: flex; flex-direction: row; align-items: center; height: 26dp; padding: 0 4dp 4dp 4dp; }
.menu .mhead .title { flex: 1 1 0; white-space: nowrap; overflow: hidden; text-overflow: ellipsis; }
.menu .opt { padding: 4dp 8dp; }
.menu .sep { margin: 4dp 0; }
.opt.off { opacity: 0.45; }
.opt .mk { width: 20dp; flex: 0 0 20dp; }
/* a check box: a toggle's state (menus, command surface) */
.ckb { position: relative; width: 12dp; height: 12dp; box-sizing: border-box; border: 1.5dp $borderStrong; border-radius: 3dp; }
.ckb.on { background-color: $accent; border-color: $accent; }
.ckb .t { position: absolute; left: 3.5dp; top: 0.5dp; width: 3.5dp; height: 7dp; border-right: 1.5dp $onAccent; border-bottom: 1.5dp $onAccent;
          transform: rotate(45deg); visibility: hidden; }
.ckb.on .t { visibility: visible; }
)rcss";

// The drawn icons' parts: <ic name="x"/> -> <span class="ic ic-x"><div class="a"></div>...</span>.
struct IconParts
{
    const char* name;
    const char* parts[4];
};
const IconParts kIcons[] = {
    { "x", { "a", "b" } },
    { "dl", { "stem", "head", "base" } },
    { "box", { "lid", "body" } },
    { "person", { "head", "shoulders" } },
    { "mail", { "frame", "v1", "v2" } },
    { "win", { "frame", "band" } },
    { "dock", { "frame", "side" } },
    { "mouse", { "body", "bl", "br", "seam" } },
    { "shut", {} },
    { "sl", { "l1", "l2", "d1", "d2" } },
    { "more", { "a", "b", "c" } },
    { "find", { "ring", "stem" } },
    { "min", { "a" } },
};

// The window frame's templates (llnewuiwindow.h): the framework handles their parts by id.
const std::pair<const char*, const char*> kFrameTemplates[] = {
    { "<whead/>", "<div id=\"whead\" class=\"whead\"><div class=\"wtitle\">{{title}}</div>"
                  "<div id=\"wmin\" class=\"wbtn\"><ic name=\"min\"/></div><div id=\"wclose\" class=\"wbtn\"><ic name=\"x\"/></div></div>" },
    { "<wgrip/>", "<div id=\"wgrip\" class=\"wgrip\"><div class=\"a\"></div><div class=\"b\"></div></div>" },
};

void replaceAll(std::string& s, const std::string& from, const std::string& to)
{
    for (size_t at = s.find(from); at != std::string::npos; at = s.find(from, at + to.size()))
        s.replace(at, from.size(), to);
}

void replaceTokens(std::string& s, const LLNewUIKit::Tokens& tokens)
{
    // Longest names first, so "$borderStrong" is not cut by "$border".
    LLNewUIKit::Tokens sorted = tokens;
    std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) { return a.first.size() > b.first.size(); });
    for (const auto& [name, value] : sorted)
        replaceAll(s, "$" + name, value);
}
} // namespace

LLNewUIKit::Tokens LLNewUIKit::tokens(Theme theme, bool glass)
{
    // Design note §4 (research/UI_NEW_UX_DESIGN_2026-11-05.md). Legibility rule: every surface
    // that carries text is opaque enough (>= 0.92 alpha) that all text tokens keep WCAG AA (4.5:1)
    // over the worst world behind it (white behind the dark theme, black behind the light one),
    // including the hover / pressed / selected / chip overlays on it. Glass adds a blur where it is
    // allowed; it never lowers the backing. Text on a surface has no outline (it only roughens
    // small text, camera mode #1572); text with no surface under it sits on a scrim and takes
    // .onworld ($textOutline). Ratios: the #703 legibility commit message.
    Tokens t;
    if (theme == Theme::Light)
        t = { { "surface", "#f5f6f8f5" }, { "raised", "#fffffffa" }, { "bar", "#f5f6f8f5" }, { "border", "#0000001a" },
              { "borderStrong", "#00000033" }, { "text", "#1b1d22" }, { "muted", "#4c535d" }, { "accent", "#1858b8" },
              { "onAccent", "#ffffff" }, { "hover", "#0000000d" }, { "pressed", "#00000017" }, { "selected", "#1f6fe01f" },
              { "field", "#ffffff" }, { "chip", "#0000000d" }, { "scrim", "#00000026" },
              // a solid, 6-digit surface colour for gradient discs ("$disc00": the same with alpha 0)
              { "disc", "#ffffff" },
              // fully opaque surfaces: windows where no blur backs the glass (llnewuiwindow.h)
              { "solid", "#f5f6f8ff" }, { "solidRaised", "#ffffffff" },
              // state text (status bar): good / warning / bad, >= 5:1 on the bar over black
              { "good", "#176b30" }, { "warn", "#7a4f00" }, { "bad", "#a4231b" } };
    else
        t = { { "surface", glass ? "#10141aeb" : "#16181dff" }, { "raised", glass ? "#181d26f0" : "#1e2128ff" },
              { "bar", glass ? "#10141aeb" : "#16181dff" }, { "border", "#ffffff14" }, { "borderStrong", "#ffffff2e" },
              { "text", "#e8eaed" }, { "muted", "#aab0b8" }, { "accent", "#6aafff" }, { "onAccent", "#0b1220" },
              { "hover", "#ffffff0f" }, { "pressed", "#ffffff1a" }, { "selected", "#4c9dff2b" },
              { "field", glass ? "#00000040" : "#0f1114ff" }, { "chip", "#ffffff10" }, { "scrim", "#00000059" },
              { "disc", "#181d26" }, { "solid", "#16181dff" }, { "solidRaised", "#1e2128ff" },
              // state text (status bar): good / warning / bad, >= 5.5:1 on the bar over white
              { "good", "#3ddc84" }, { "warn", "#ffc857" }, { "bad", "#ff8a80" } };
    t.push_back({ "textEffect", "none" });
    t.push_back({ "textOutline", "outline(1px #000000a0)" });
    // States that mean the same in every theme: in level / online, an error.
    t.push_back({ "ok", "#3ddc84" });
    t.push_back({ "error", "#ff8a80" });
    // The viewfinder draws on the world, in white whatever the theme.
    t.push_back({ "finderMask", "#00000080" });
    t.push_back({ "finderLine", "#ffffff59" });
    t.push_back({ "finderStrong", "#ffffffe6" });
    t.push_back({ "finderDot", "#ffffffcc" });
    t.push_back({ "finderClear", "#ffffff00" });
    t.push_back({ "finderHole", "#00000060" });
    t.push_back({ "finderCorner", "#ffffffb0" });
    t.push_back({ "flash", "#ffffff" });
    t.push_back({ "histogram", "#00000040" });
    // Radii and type.
    t.push_back({ "radius", "6dp" });
    t.push_back({ "radiusControl", "10dp" });
    t.push_back({ "radiusChip", "13dp" });
    t.push_back({ "radiusPill", "14dp" });
    t.push_back({ "radiusPanel", "10dp" });
    t.push_back({ "radiusPopover", "14dp" });
    t.push_back({ "fontCaption", "11dp" });
    t.push_back({ "fontBody", "13dp" });
    t.push_back({ "fontTitle", "15dp" });
    t.push_back({ "fontDisplay", "20dp" });
    t.push_back({ "barH", "32dp" });
    return t;
}

std::string LLNewUIKit::stylesheet(const Tokens& tokens)
{
    std::string s = kKit;
    replaceTokens(s, tokens);
    return s;
}

std::string LLNewUIKit::expandTemplates(const std::string& source)
{
    // The window frame's parts first (they contain icons).
    std::string rml = source;
    for (const auto& [from, to] : kFrameTemplates)
        replaceAll(rml, from, to);
    // <ic name="x" [attributes]/>: the attributes stay on the icon's span; a class is added to it.
    std::string out;
    out.reserve(rml.size() + rml.size() / 4);
    size_t from = 0;
    for (size_t at = rml.find("<ic "); at != std::string::npos; at = rml.find("<ic ", from))
    {
        const size_t end = rml.find("/>", at);
        if (end == std::string::npos)
            break;
        out.append(rml, from, at - from);
        std::string attrs = rml.substr(at + 4, end - at - 4);
        std::string name, cls;
        auto take = [&attrs](const char* key, std::string& value)
        {
            const std::string k = std::string(key) + "=\"";
            const size_t      a = attrs.find(k);
            if (a == std::string::npos || (a > 0 && attrs[a - 1] != ' '))
                return;
            const size_t b = attrs.find('"', a + k.size());
            if (b == std::string::npos)
                return;
            value = attrs.substr(a + k.size(), b - a - k.size());
            attrs.erase(a, b + 1 - a);
        };
        take("name", name);
        take("class", cls);
        const IconParts* icon = nullptr;
        for (const IconParts& i : kIcons)
            if (name == i.name)
                icon = &i;
        if (!icon)
        {
            // Unknown: left as written, so the mistake shows (and the newui-kit check names it).
            out.append(rml, at, end + 2 - at);
            from = end + 2;
            continue;
        }
        while (!attrs.empty() && attrs.back() == ' ')
            attrs.pop_back();
        while (!attrs.empty() && attrs.front() == ' ')
            attrs.erase(0, 1);
        out += "<span class=\"ic ic-" + name + (cls.empty() ? "" : " " + cls) + "\"" + (attrs.empty() ? "" : " " + attrs) + ">";
        for (const char* part : icon->parts)
            if (part)
                out += std::string("<div class=\"") + part + "\"></div>";
        out += "</span>";
        from = end + 2;
    }
    out.append(rml, from, std::string::npos);
    return out;
}

std::string LLNewUIKit::themed(const char* rml, const Tokens& tokens)
{
    std::string s = expandTemplates(rml);
    const size_t head = s.find("<head>");
    if (head != std::string::npos)
        s.insert(head + 6, "<style>" + stylesheet(tokens) + "</style>");
    replaceTokens(s, tokens);
    return s;
}

std::string LLNewUIKit::escapeText(const std::string& text)
{
    std::string out; out.reserve(text.size());
    for (char c : text)
        switch (c)
        {
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '&': out += "&amp;"; break;
            case '"': out += "&quot;"; break;
            case '{': out += "&#123;"; break;
            case '}': out += "&#125;"; break;
            default: out += c; break;
        }
    return out;
}
