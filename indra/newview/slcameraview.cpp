/**
 * @file slcameraview.cpp
 * @brief Camera mode's overlay on the new interface kit (see slcameraview.h).
 *
 * One document, data model "camera". Layout from slcamerabarlayout.h (dp = UI units), values and
 * actions from SLCameraModel; this file only presents them and forwards input.
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
#include "slcameraview.h"

#include "slcamerabarlayout.h"
#include "slcameraexposure.h"
#include "slcamerainput.h"
#include "slcameramode.h"
#include "slcameramodel.h"
#include "slcamerapresets.h"
#include "slcamerarail.h"
#include "slcamerareview.h"
#include "slkeycommands.h"
#include "prestigecameraisolate.h"   // <Prestige> isolate your subject
#include "prestigecamerashot.h" // <Prestige> setting up the shot (chips, the light, where the camera stands)

#include "llfile.h"
#include "llfloaterpreference.h"
#include "llfloaterreg.h"
#include "llimview.h"
#include "llframetimer.h"
#include "llrmluiatlas.h"
#include "vkgfx/llrmluivk.h"
#include "llnewui.h"
#include "llnewuikit.h"
#include "llrender.h"
#include "llsnapshotmodel.h"
#include "lltrans.h"
#include "llui.h"
#include "llview.h"
#include "llviewercontrol.h"
#include "llviewerwindow.h"
#include "vkgfx/vk_ui_texture_host.h" // slUIMarkExternalLayer

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Core.h>
#include <RmlUi/Core/DataModelHandle.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/ElementUtilities.h>
#include <RmlUi/Core/Input.h>
#include <climits>
#include <cstdlib>
#include <new>
#include <string>
#include <vector>

namespace
{
// Layout only; every colour, radius and type size comes from the kit's tokens ($name).
const char* kCameraRml = R"rml(<rml><head><title>camera</title><style>
body { position: absolute; left: 0; top: 0; right: 0; bottom: 0; pointer-events: none; }
/* the viewfinder: crop mask, grid, level, tilt knob, AF point (placed from C++ in dp) */
.mask { position: absolute; background-color: $finderMask; }
.gl { position: absolute; background-color: $finderLine; }
.gl.c { background-color: $finderStrong; }
#level { position: absolute; height: 2dp; }
#level .seg { position: absolute; top: 0; height: 2dp; background-color: $finderStrong; }
#level.ok .seg { background-color: $ok; }
/* the tilt (Dutch angle): an arc at the frame's right, -40..+40 degrees. The ring is a gradient (smooth
   edges) clipped to the arc by its band (clip: always -- RmlUi clips only overflowing layout otherwise,
   and absolutely placed children never overflow). Boxes along the arc take the drag and the wheel. */
#tiltband { position: absolute; overflow: hidden; clip: always; pointer-events: none; }
.thit { position: absolute; pointer-events: auto; cursor: pointer; }
#tiltring { position: absolute; pointer-events: none; }
.tdot { position: absolute; pointer-events: none; decorator: radial-gradient(circle closest-side, $finderDot 60%, $finderClear 100%); }
#tiltlabel { position: absolute; font-size: $fontCaption; white-space: nowrap; pointer-events: none; }
#tiltlabel .pill { display: inline-block; padding: 0 6dp; background-color: $bar; border-radius: $radius; }
#tiltknob { position: absolute; width: 18dp; height: 18dp; pointer-events: auto; cursor: pointer;
            decorator: radial-gradient(circle closest-side, $finderHole 62%, $finderStrong 72%, $finderStrong 86%, $finderClear 100%); }
#tiltknob.ok { decorator: radial-gradient(circle closest-side, $finderHole 62%, $ok 72%, $ok 86%, $ok00 100%); }
#tiltknob:hover { decorator: radial-gradient(circle closest-side, $finderHole 62%, $accent 72%, $accent 86%, $accent00 100%); }
#levelinfo { position: absolute; font-size: $fontCaption; white-space: nowrap; }
#levelinfo .pill { display: inline-block; padding: 0 6dp; background-color: $bar; border-radius: $radius; }
#af { position: absolute; width: 40dp; height: 40dp; box-sizing: border-box; border: 2dp $finderStrong; border-radius: $radius; }
#af.held { border-color: $accent; }
/* Controls: the strip above the bar and the chips top-right */
#stripwrap { position: absolute; left: 0; right: 0; bottom: 94dp; text-align: center; }
#strip { display: inline-block; padding: 4dp; background-color: $bar; border: 1dp $border; border-radius: $radiusPill;
         pointer-events: auto; white-space: nowrap; }
#chips { position: absolute; right: 24dp; top: 24dp; display: flex; flex-direction: row; gap: 8dp; }
#gridpop { top: 58dp; width: 250dp; pointer-events: auto; } /* left: under its chip (placeChipMenus) */
#quick { width: 480dp; padding: 8dp; pointer-events: auto; }
#qgrid { display: flex; flex-direction: row; flex-wrap: wrap; gap: 6dp; }
#quick .keys { margin-top: 8dp; }
.cell.more { vertical-align: top; padding-top: 16dp; padding-bottom: 14dp; }
/* the bar out of the frame (board 27): only the shutter and the handle stay */
#handle { position: absolute; left: 50%; margin-left: -24dp; bottom: 8dp; } /* .handle */
#bar.away { background-color: transparent; border-color: transparent; pointer-events: none; }
#bar.away .bb { display: none; }
#bar.away #shutter { pointer-events: auto; }
#notice { position: absolute; left: 0; right: 0; top: 24dp; text-align: center; }
#noticepill .t { font-weight: bold; margin-right: 8dp; }
#readout { position: absolute; left: 50%; margin-left: -90dp; width: 180dp; bottom: 100dp; padding: 6dp 10dp; text-align: center;
           background-color: $bar; border: 1dp $border; border-radius: $radiusControl; }
#readout .v { font-size: $fontDisplay; font-weight: bold; margin-left: 6dp; }
#readout .caption { margin-top: 2dp; }
#readout .ticks { display: flex; flex-direction: row; justify-content: space-between; align-items: flex-end; height: 12dp; margin: 4dp 10dp 0 10dp; }
#readout .tk { width: 2dp; height: 6dp; background-color: $muted; }
#readout .tk.major { height: 10dp; }
#readout .tk.on { height: 12dp; width: 3dp; background-color: $accent; }
/* the shutter alone (bar out of the frame) is a little larger */
#bar.away #shutter { transform: scale(1.25); }
/* the bar at the top edge */
.top #bar { bottom: auto; top: 28dp; }
.top #handle { bottom: auto; top: 8dp; }
.top #stripwrap { bottom: auto; top: 94dp; }
.top #hints { bottom: auto; top: 96dp; }
.top #hints.up { top: 162dp; }
.top .pop { bottom: auto; top: 100dp; }
.top #readout { bottom: auto; top: 100dp; }
.top #notice, .top #saved { top: auto; bottom: 28dp; }
.top #toast { top: auto; bottom: 64dp; }
/* after the shot (board 29) */
#saved { position: absolute; left: 0; right: 0; top: 24dp; text-align: center; }
#savedpill { pointer-events: auto; }
#savedpill .link { margin-left: 10dp; }
/* the session's shots: the thumbnails, and the card above them in the same panel (board 29) */
#review { left: 24dp; bottom: 28dp; padding: 8dp; pointer-events: auto; }
#review.open { width: 300dp; } /* the shot, the histogram and the rows: one width, centred */
#thumbs { display: flex; flex-direction: row; gap: 6dp; align-items: center; }
#review.open #thumbs { margin-top: 8dp; padding-top: 8dp; border-top: 1dp $border; }
.thumb { width: 64dp; height: 36dp; border-radius: 4dp; border: 2dp transparent; cursor: pointer; }
.thumb.sel { border-color: $accent; }
#thumbs .more { color: $muted; font-size: $fontCaption; padding: 0 4dp; }
#thumbs .allshots { margin-left: 8dp; white-space: nowrap; }
#card { position: relative; }
#card .shot { display: block; width: 300dp; border-radius: $radius; background-color: $chip; } /* (a placeholder while the preview loads) */
#hist { display: flex; flex-direction: row; align-items: flex-end; height: 36dp; margin: 6dp 0; background-color: $histogram; border-radius: 4dp; }
#hist .b { flex: 1 1 0; background-color: $muted; }
#card .meta { display: flex; flex-direction: row; }
#card .meta .t { flex: 1 1 0; font-weight: bold; }
#card .acts { display: flex; flex-direction: row; gap: 6dp; margin-top: 6dp; }
#card .acts .btn { border: 1dp $border; }
#card .acts .spacer { flex: 1 1 0; }
#cardmenu { right: 0; bottom: 34dp; width: 140dp; padding: 4dp; }
/* popovers, menus and cards draw above the rail and the strip */
.popover { z-index: 10; }
/* popovers placed from C++ stay in the document (hidden), so they can be placed before they show */
.popover.toggled { display: none; } .popover.toggled.shown { display: block; }
#rail { z-index: 5; }
/* the Photo Studio rail */
/* Plain positioning, no flex: a scrolling flex child is sized to almost nothing by RmlUi. */
#rail { position: absolute; left: 0; top: 70dp; bottom: 100dp; width: 300dp; box-sizing: border-box; pointer-events: auto; }
#rail.right { left: auto; right: 0; }
.top #rail { top: 100dp; bottom: 28dp; }
#rail .rhead { position: absolute; left: 8dp; right: 8dp; top: 6dp; height: 28dp; display: flex; flex-direction: row; align-items: center; }
#rail .rhead .title { flex: 1 1 0; }
#railbody { position: absolute; left: 8dp; right: 8dp; top: 40dp; bottom: 44dp; overflow-y: auto; }
#rail .studio { position: absolute; left: 8dp; right: 8dp; bottom: 8dp; text-align: center; border: 1dp $border; }
/* The strip, hints and tips stay centred on the window (in line with the bar); when one of them
   would run under the rail, the rail stops above them instead. */
#rail.short { bottom: 200dp; }
.top #rail.short { top: 200dp; bottom: 28dp; }
/* the help for the strip cell under the pointer */
/* centred over the value or button it is about (left set from C++), inside the window */
#tipwrap { position: absolute; width: 420dp; bottom: 162dp; visibility: hidden; } /* always there: placed before it shows */
#tipwrap.shown { visibility: visible; }
.top #tipwrap { bottom: auto; top: 162dp; }
#tipwrap.low { bottom: 96dp; } .top #tipwrap.low { bottom: auto; top: 96dp; }
/* the drive's countdown or progress, the shutter's flash, unread messages */
#drive { position: absolute; left: 0; right: 0; top: 22%; text-align: center; }
#drivepill { padding: 8dp 18dp; font-size: $fontDisplay; font-weight: bold; }
#flash { position: absolute; background-color: $flash; display: none; }
/* what is switched on and easy to forget: focus locked, depth preview, a timer / burst / bracket, unread */
#status { position: absolute; left: 68dp; top: 36dp; height: 24dp; display: flex; flex-direction: row; align-items: center; gap: 6dp; } /* centred on the ? beside it */
#status .st { font-size: $fontCaption; padding: 3dp 8dp; background-color: $bar; border: 1dp $border; border-radius: $radiusControl; white-space: nowrap; }
#status .st.accent { color: $accent; border-color: $accent; }
#af .lk { position: absolute; left: -10dp; top: 40dp; width: 56dp; text-align: center; font-size: $fontCaption; color: $accent; display: none; }
#af.locked .lk { display: block; }
#af.locked { border-color: $accent; border-width: 3dp; }
/* <Prestige> focus confirmation: green when the focus is achieved, red when it failed; the group
   focus's markers on each chosen person's eyes */
#af.ok { border-color: $ok; }
#af.fail { border-color: $error; }
.gm { position: absolute; display: none; box-sizing: border-box; border: 2dp $finderStrong; border-radius: 7dp; }
.gm.ok { border-color: $ok; }
/* </Prestige> */
/* saved camera setups */
/* saved camera setups, in the rail's camera section */
.setups { margin-top: 6dp; }
.setups .opt { padding: 3dp 6dp; }
.setups .opt .x { color: $muted; min-width: 18dp; text-align: center; }
.setups .opt .x:hover { color: $text; }
.prow { display: flex; flex-direction: row; gap: 6dp; margin-top: 6dp; }
.prow .field { flex: 1 1 0; }
.prow .btn { border: 1dp $border; }
.pv { position: relative; width: 40dp; height: 24dp; border: 1dp $muted; border-radius: 3dp; margin-right: 12dp; }
.pv div { position: absolute; background-color: $muted; }
.pv .v1 { left: 33%; top: 0; bottom: 0; width: 1dp; } .pv .v2 { left: 66%; top: 0; bottom: 0; width: 1dp; }
.pv .h1 { top: 33%; left: 0; right: 0; height: 1dp; } .pv .h2 { top: 66%; left: 0; right: 0; height: 1dp; }
.pv .g1 { left: 38%; top: 0; bottom: 0; width: 1dp; } .pv .g2 { left: 62%; top: 0; bottom: 0; width: 1dp; }
.pv .g3 { top: 38%; left: 0; right: 0; height: 1dp; } .pv .g4 { top: 62%; left: 0; right: 0; height: 1dp; }
.pv .d1 { left: -3dp; top: 11dp; width: 46dp; height: 1dp; transform: rotate(31deg); }
.pv .d2 { left: -3dp; top: 11dp; width: 46dp; height: 1dp; transform: rotate(-31deg); }
.pv .cx { left: 15dp; top: 11dp; width: 10dp; height: 1dp; } .pv .cy { left: 19dp; top: 7dp; width: 1dp; height: 10dp; }
.corner { position: absolute; width: 22dp; height: 22dp; }
#c-tl { left: 24dp; top: 24dp; border-left: 2dp $finderCorner; border-top: 2dp $finderCorner; }
#c-tr { right: 24dp; top: 24dp; border-right: 2dp $finderCorner; border-top: 2dp $finderCorner; }
#c-bl { left: 24dp; bottom: 24dp; border-left: 2dp $finderCorner; border-bottom: 2dp $finderCorner; }
#c-br { right: 24dp; bottom: 24dp; border-right: 2dp $finderCorner; border-bottom: 2dp $finderCorner; }
#helpbtn { position: absolute; left: 36dp; top: 36dp; width: 24dp; height: 24dp; line-height: 24dp; text-align: center;
           decorator: radial-gradient(circle closest-side, $disc 88%, $disc00 100%); pointer-events: auto; cursor: pointer; }
#helpbtn:hover { decorator: radial-gradient(circle closest-side, $accent 88%, $accent00 100%); color: $onAccent; }
#bar { position: absolute; bottom: 28dp; height: 58dp; box-sizing: border-box; background-color: $bar; border: 1dp $border;
       border-radius: $radiusPill; pointer-events: auto; }
#helpbtn { tab-index: auto; } #helpbtn:focus { border-color: $accent; } /* .bb and the other controls: the kit */
/* the confirmation / address card for a shot to a destination other than the disk */
#sendcard { left: 50%; margin-left: -190dp; width: 380dp; bottom: 100dp; pointer-events: auto; }
.top #sendcard { bottom: auto; top: 100dp; }
#sendcard .title { margin-bottom: 4dp; }
#sendcard .frow { margin-top: 8dp; }
#sendcard .frow .field { margin-top: 2dp; }
#sendcard .trow { margin-top: 6dp; padding: 4dp 0; }
#sendcard .err { color: $error; margin-top: 6dp; }
#sendcard .acts { display: flex; flex-direction: row; gap: 6dp; margin-top: 10dp; }
#sendcard .acts .spacer { flex: 1 1 0; }
#sendcard .acts .btn { border: 1dp $border; }
/* the histogram's scale */
#histscale { display: flex; flex-direction: row; font-size: $fontCaption; color: $muted; margin-top: -2dp; margin-bottom: 4dp; }
#histscale .l { flex: 1 1 0; } #histscale .r { flex: 1 1 0; text-align: right; }
/* the flash: a lightning bolt (two slants and the step between them); off, struck through */
.ic-flash { width: 12dp; height: 15dp; }
.ic-flash .a { left: 1.75dp; top: 3dp; width: 8.5dp; height: 1.5dp; transform: rotate(-62deg); }
.ic-flash .b { left: 3.5dp; top: 6.75dp; width: 6dp; height: 1.5dp; }
.ic-flash .c { left: 2.75dp; top: 10.5dp; width: 8.5dp; height: 1.5dp; transform: rotate(-62deg); }
.ic-flash .s { left: -2dp; top: 6.75dp; width: 16dp; height: 1.5dp; transform: rotate(45deg); display: none; }
.ic-flash.off .s { display: block; }
.ic-flash.off .a, .ic-flash.off .b, .ic-flash.off .c { background-color: $muted; }
/* The shutter's ring and disc are one radial gradient: the RmlUi backend has no edge
   anti-aliasing, a gradient (a shader) gives the circles soft edges. ($text00: the token with
   alpha 0 -- the text tokens are 6-digit colours.) */
#shutter { position: absolute; top: 5dp; width: 46dp; height: 46dp; cursor: pointer;
           decorator: radial-gradient(circle closest-side, $text 71%, $text00 75%, $text00 84%, $text 88%, $text 95%, $text00 100%); }
#shutter:hover { opacity: 0.88; }
#shutter.down { decorator: radial-gradient(circle closest-side, $accent 71%, $accent00 75%, $text00 84%, $text 88%, $text 95%, $text00 100%); }
#hints { position: absolute; left: 0; right: 0; bottom: 96dp; text-align: center; }
#hints.up { bottom: 162dp; }
#hintpill { padding: 4dp 10dp; }
#toast { position: absolute; left: 0; right: 0; top: 64dp; text-align: center; } /* under the saved / bar notices */
#toastpill { padding: 6dp 14dp; }
#help { left: 36dp; top: 70dp; width: 540dp; padding: 16dp 20dp; pointer-events: auto; }
#help .title { margin-bottom: 10dp; }
.mrow { display: flex; flex-direction: row; min-height: 24dp; line-height: 18dp; padding: 3dp 0; }
.mrow .name { width: 150dp; font-weight: bold; }
.mrow .ic-mouse { margin-top: 1dp; }
#help .link:hover { text-decoration: underline; }
.mrow .what { flex: 1 1 0; color: $muted; }
#keys { display: flex; flex-direction: row; flex-wrap: wrap; }
.krow { width: 33%; height: 26dp; line-height: 26dp; white-space: nowrap; }
.krow .hk { margin-right: 6dp; }
#help .foot { color: $muted; font-size: $fontCaption; }
.pop { bottom: 100dp; pointer-events: auto; }
#savepop { width: 320dp; }
#sizepop { width: 330dp; }
#sizepop .seg { flex: 1 1 auto; padding: 0 6dp; } /* "As resolution" is wider than a ratio */
.pop .caption { margin: 6dp 4dp 4dp 4dp; }
.custom { display: flex; flex-direction: row; align-items: center; gap: 6dp; padding: 4dp 8dp 4dp 32dp; }
.custom .field { width: 80dp; }
.qrow { display: flex; flex-direction: row; align-items: center; gap: 8dp; padding: 6dp 8dp; }
/* <Prestige> isolate your subject: its menu under its chip, the picked subjects' marks, the picking banner */
#isopop { top: 58dp; width: 320dp; pointer-events: auto; } /* left: under its chip (placeChipMenus) */
#isopop .caption { margin: 6dp 4dp 4dp 4dp; }
.isomark { position: absolute; display: none; box-sizing: border-box; border: 2dp $accent; border-radius: 11dp; }
#isopick { position: absolute; left: 0; right: 0; top: 104dp; text-align: center; }
.top #isopick { top: auto; bottom: 104dp; }
#help .foot.iso { margin-bottom: 6dp; }
/* <Prestige> setting up the shot: HUDs, poser, freeze, quiet, sky (chips left of the chips; right: placed
   from C++), the light above the shutter (placed from C++ above the bar, the strip and the hints), renaming setups */
#shotchips { position: absolute; right: 340dp; top: 24dp; display: flex; flex-direction: row; gap: 8dp; }
#lightbtn { position: absolute; left: 50%; margin-left: -40dp; width: 80dp; bottom: 94dp; height: 28dp; line-height: 28dp;
            text-align: center; white-space: nowrap; background-color: $bar; border: 1dp $border; border-radius: $radiusPill;
            pointer-events: auto; cursor: pointer; }
#lightbtn:hover { border-color: $accent; }
#lightbtn.on { color: $accent; border-color: $accent; }
.setups .opt .rn { color: $muted; margin-right: 8dp; display: none; }
.setups .opt:hover .rn { display: block; }
.setups .opt .rn:hover { color: $text; }
.setups .segmented { margin-top: 4dp; }
/* </Prestige> */
</style></head><body><div data-model="camera" data-class-top="barTop">
<div id="m-t" class="mask"></div><div id="m-b" class="mask"></div><div id="m-l" class="mask"></div><div id="m-r" class="mask"></div>
<div id="g-v1" class="gl"></div><div id="g-v2" class="gl"></div><div id="g-h1" class="gl"></div><div id="g-h2" class="gl"></div>
<div id="g-d1" class="gl"></div><div id="g-d2" class="gl"></div><div id="g-cx" class="gl c"></div><div id="g-cy" class="gl c"></div>
<div id="af"><div class="lk">{{lockMark}}</div></div>
<!-- <Prestige> group focus markers -->
<div id="gm-0" class="gm"></div><div id="gm-1" class="gm"></div><div id="gm-2" class="gm"></div><div id="gm-3" class="gm"></div>
<div id="gm-4" class="gm"></div><div id="gm-5" class="gm"></div><div id="gm-6" class="gm"></div><div id="gm-7" class="gm"></div>
<!-- </Prestige> -->
<div id="level"><div id="lv-l" class="seg"></div><div id="lv-r" class="seg"></div></div>
<div id="tiltband"><div id="tiltring"></div><div class="tdot" id="td-0"></div><div class="tdot" id="td-1"></div><div class="tdot" id="td-2"></div><div class="tdot" id="td-3"></div><div class="tdot" id="td-4"></div><div class="tdot" id="td-5"></div><div class="tdot" id="td-6"></div><div class="tdot" id="td-7"></div><div class="tdot" id="td-8"></div></div>
<div id="tilthit"><div class="thit" id="th-0"></div><div class="thit" id="th-1"></div><div class="thit" id="th-2"></div><div class="thit" id="th-3"></div><div class="thit" id="th-4"></div><div class="thit" id="th-5"></div><div class="thit" id="th-6"></div><div class="thit" id="th-7"></div></div>
<div id="tiltknob"></div><div id="tiltlabel"></div>
<div id="levelinfo"></div>
<div id="flash"></div>
<div id="c-tl" class="corner"></div><div id="c-tr" class="corner"></div>
<div id="c-bl" class="corner"></div><div id="c-br" class="corner"></div>
<div id="helpbtn" data-event-click="help()">?</div>
<div id="status"><div class="st" data-for="st : statuses" data-class-accent="st.end">{{st.key}}</div></div>
<div id="drive" data-if="drive != ''"><div id="drivepill" class="capsule">{{drive}}</div></div>
<div id="tipwrap" data-class-shown="tip != '' && !barAway" data-class-low="!controlsOpen"><div id="tippill" class="tipcard">
  <div class="tt">{{tip}}</div><div class="tw">{{tipWhat}}</div>
  <div class="ts" data-if="tipLow != ''"><div class="lo"><span class="chev left"></span>{{tipLow}}</div><div class="hi">{{tipHigh}}<span class="chev right"></span></div></div>
  <div class="tc" data-if="tipClick != ''"><span class="hk">{{clickWord}}</span>{{tipClick}}</div>
</div></div>
<div id="toast" data-if="toast != ''"><div id="toastpill" class="capsule raised">{{toast}}</div></div>
<div id="hints" data-if="hintShown && tip == ''" data-class-up="controlsOpen"><div id="hintpill" class="capsule"><span data-for="h : hints"><span class="hk">{{h.key}}</span><span class="ht" data-class-end="h.end">{{h.text}}</span></span></div></div>
<div id="stripwrap" data-if="controlsOpen && !barAway"><div id="strip">
  <div class="cell" data-for="c : cells" data-attr-id="c.id" data-class-marked="c.marked" data-class-off="c.off" data-event-click="cell(it_index)">
    <div class="v"><span class="arr up" data-event-click="cellstep(it_index, 1)"><span class="chev"></span></span>{{c.value}}<span data-if="c.unit != ''"> {{c.unit}}</span><span class="arr dn" data-event-click="cellstep(it_index, -1)"><span class="chev down"></span></span></div><div class="n">{{c.name}}</div></div>
  <div id="cellmore" class="cell more" data-event-click="qmenu()"><ic name="more"/></div>
</div></div>
<div id="quick" class="popover toggled" data-class-shown="popup == 'quick'">
  <div id="qgrid"><div class="qcell" data-for="q : qcells" data-attr-id="q.id" data-class-marked="q.marked" data-event-click="qnext(it_index)">
    <div class="v"><span class="arr up" data-event-click="qstep(it_index, 1)"><span class="chev"></span></span>{{q.value}}<span class="arr dn" data-event-click="qstep(it_index, -1)"><span class="chev down"></span></span></div><div class="n">{{q.name}}</div></div></div>
  <div class="keys"><span data-for="h : qhints"><span class="hk">{{h.key}}</span><span class="ht end">{{h.text}}</span></span></div>
</div>
<div id="chips" data-if="controlsOpen && !barAway">
  <div id="gridchip" class="cbtn" data-class-on="popup == 'grid'" data-event-click="pop('grid')">{{gridTitle}}<span class="chev" data-class-down="popup != 'grid'"></span></div>
  <div id="levelchip" class="cbtn" data-class-on="levelOn" data-event-click="level()">{{levelOn ? '✓  ' : ''}}{{levelLabel}}</div>
  <div id="flashchip" class="cbtn" data-class-on="flashOn" data-event-click="flash()"><span class="ic ic-flash" data-class-off="!flashOn"><div class="a"></div><div class="b"></div><div class="c"></div><div class="s"></div></span></div>
  <div id="studiochip" class="cbtn" data-class-on="studioOn" data-event-click="studio()">{{studioLabel}}</div>
  <!-- <Prestige> isolate your subject -->
  <div id="isochip" class="cbtn" data-class-on="isoMode != 0" data-event-click="pop('iso')">{{isoChip}}<span class="chev" data-class-down="popup != 'iso'"></span></div>
  <!-- <Prestige> AE-L -->
  <div id="aelchip" class="cbtn" data-class-on="aeLocked" data-event-click="ael()">{{aelLabel}}</div>
  <!-- </Prestige> -->
</div>
<div id="gridpop" class="popover toggled" data-class-shown="popup == 'grid'">
  <div class="opt" data-for="g : grids" data-attr-id="g.id" data-class-selected="g.on" data-event-click="grid(it_index)">
    <div class="pv"><div class="v1" data-if="it_index == 1 || it_index == 2"></div><div class="v2" data-if="it_index == 1 || it_index == 2"></div>
      <div class="h1" data-if="it_index == 1 || it_index == 2"></div><div class="h2" data-if="it_index == 1 || it_index == 2"></div>
      <div class="d1" data-if="it_index == 2"></div><div class="d2" data-if="it_index == 2"></div>
      <div class="g1" data-if="it_index == 3"></div><div class="g2" data-if="it_index == 3"></div>
      <div class="g3" data-if="it_index == 3"></div><div class="g4" data-if="it_index == 3"></div>
      <div class="cx" data-if="it_index == 4"></div><div class="cy" data-if="it_index == 4"></div></div>
    <div class="col">{{g.label}}</div><div class="tick" data-if="g.on">✓</div></div>
</div>
<!-- <Prestige> isolate your subject: its menu, the picked subjects' marks (placed from C++), picking -->
<div id="isopop" class="popover toggled" data-class-shown="popup == 'iso'">
  <div class="caption">{{isoTitle}}</div>
  <div class="opt" data-for="o : isoModes" data-class-selected="o.on" data-event-click="isomode(it_index)">
    <div class="col"><div>{{o.label}}</div><div class="caption">{{o.note}}</div></div><div class="tick" data-if="o.on">✓</div></div>
  <div class="sep" data-if="isoMode == 2"></div>
  <div class="opt" data-if="isoMode == 2" data-event-click="isopick()"><div class="col">{{isoPickLabel}}</div></div>
  <div class="opt" data-if="isoMode == 2 && isoCount > 0" data-event-click="isoclear()"><div class="col">{{isoClearLabel}}</div></div>
  <div class="sep"></div>
  <div class="caption">{{isoHelp}}</div>
</div>
<div class="isomark" id="iso-m0"></div><div class="isomark" id="iso-m1"></div><div class="isomark" id="iso-m2"></div><div class="isomark" id="iso-m3"></div>
<div class="isomark" id="iso-m4"></div><div class="isomark" id="iso-m5"></div><div class="isomark" id="iso-m6"></div><div class="isomark" id="iso-m7"></div>
<div id="isopick" data-if="isoPicking"><div class="capsule raised">{{isoPickText}}</div></div>
<!-- <Prestige> setting up the shot: HUDs in the viewfinder, the poser, Freeze World, the quiet shutter, the sky -->
<div id="shotchips" data-if="controlsOpen && !barAway">
  <div id="hudchip" class="cbtn" data-class-on="shotHuds" data-event-click="shothuds()">{{shotHudsLabel}}</div>
  <div id="poserchip" class="cbtn" data-class-on="shotPoser" data-event-click="shotposer()">{{shotPoserLabel}}</div>
  <div id="freezechip" class="cbtn" data-class-on="shotFrozen" data-event-click="shotfreeze()">{{shotFreezeLabel}}</div>
  <div id="quietchip" class="cbtn" data-class-on="shotQuiet" data-event-click="shotquiet()">{{shotQuietLabel}}</div>
  <div id="skychip" class="cbtn" data-class-on="shotSkyPhoto" data-event-click="shotsky()">{{shotSkyLabel}}</div>
</div>
<div id="lightbtn" data-if="!barAway" data-class-on="shotLight" data-event-click="shotlight()">{{shotLightLabel}}</div>
<!-- </Prestige> -->
<div id="notice" data-if="noticeShown"><div id="noticepill" class="capsule"><span class="t">{{noticeTitle}}</span><span class="ht">{{noticeText}}</span><span class="hk">{{toggleKey}}</span><span class="ht">{{noticeEnd}}</span></div></div>
<div id="readout" data-if="readoutShown"><div><span class="caption">{{readoutName}}</span><span class="v">{{readoutValue}}</span></div>
  <div class="ticks" data-if="readoutTicks.size > 0"><div class="tk" data-for="k : readoutTicks" data-class-on="k.on" data-class-major="k.major"></div></div>
  <div class="caption">{{readoutHint}}</div></div>
<div id="saved" data-if="savedShown"><div id="savedpill" class="capsule">✓  {{savedLabel}}<span class="link" data-if="savedDisk" data-event-click="folder()">{{folderLabel}}</span></div></div>
<div id="review" class="popover" data-if="thumbs.size > 0 && !barAway" data-class-open="cardOpen">
  <div id="card" data-if="cardOpen">
  <img class="shot" data-if="cardSrc != ''" data-attr-src="cardSrc" data-style-height="cardH"/>
  <div class="shot" data-if="cardSrc == ''" data-style-height="cardH"></div>
  <div id="hist"><div class="b" data-for="b : bars" data-style-height="b.h"></div></div>
  <div id="histscale"><div class="l">{{histDark}}</div><div class="r">{{histBright}}</div></div>
  <div class="meta"><div class="t">{{cardTitle}}</div><div class="caption">{{cardMeta}}</div></div>
  <div class="caption">{{cardLabel}}</div>
  <div class="acts"><div class="btn" data-event-click="folder()">{{folderLabel}}</div>    <div class="spacer"></div><div class="btn" data-event-click="cardmore()"><ic name="more"/></div></div>
  <div id="cardmenu" class="popover" data-if="cardMore"><div class="opt" data-event-click="del()"><div class="col">{{deleteLabel}}</div></div></div>
  </div>
  <div id="thumbs">
  <div class="tw" data-for="t : thumbs"><img class="thumb" data-attr-src="t.src" data-class-sel="t.sel" data-event-click="thumb(t.index)"/></div>
  <div class="more" data-if="moreShots != ''">{{moreShots}}</div>
  <div class="allshots" data-if="reviewKey != '' && !narrow"><span class="hk">{{reviewKey}}</span><span class="ht">{{allShotsLabel}}</span></div>
  </div>
</div>
<div id="rail" class="panel" data-if="controlsOpen && studioOn && !barAway" data-class-right="railRight" data-class-short="railShort">
  <div class="rhead"><div class="title">{{studioLabel}}</div>
    <div class="btn" data-event-click="railside()"><ic name="dock" data-class-toleft="railRight"/></div>
    <div class="btn" data-event-click="studio()"><ic name="x"/></div></div>
  <div id="railbody">
    <div data-for="sec : rail">
      <div class="rsec" data-event-click="railsec(it_index)"><span class="chev" data-class-down="sec.open" data-class-right="!sec.open"></span>{{sec.title}}</div>
      <div data-if="sec.open">
        <div class="rrow" data-for="r : sec.rows" data-attr-id="r.id" data-class-dim="!r.enabled">
          <div class="rl" data-if="r.kind != 3">{{r.label}}</div>
          <div class="rtrack" data-if="r.kind == 0" data-attr-id="r.track"><div class="tr"><div class="fl" data-style-width="r.w"></div></div></div>
          <div class="toggle" data-if="r.kind == 1" data-class-on="r.on" data-event-click="railturn(r.id, 1)"><div class="knob"></div></div>
          <div class="rchoice" data-if="r.kind == 2"><div class="arr" data-event-click="railturn(r.id, -1)"><span class="chev left"></span></div><div class="cv" data-event-click="railturn(r.id, 1)">{{r.value}}</div>
            <div class="arr" data-event-click="railturn(r.id, 1)"><span class="chev right"></span></div></div>
          <div class="rbtns" data-if="r.kind == 3"><div class="btn" data-for="b : r.buttons" data-event-click="railpress(r.id, it_index)">{{b}}</div></div>
          <div class="rv" data-if="r.kind == 0 || r.kind == 1">{{r.value}}</div>
          <div class="rreset" data-if="r.kind != 3" data-class-shown="r.changed" data-event-click="railreset(r.id)"><div class="ring"></div><div class="tip"></div></div>
        </div>
        <div class="setups" data-if="sec.setups">
          <div class="caption">{{setupsTitle}}</div>
          <div class="caption" data-if="presets.size == 0">{{presetNone}}</div>
          <div class="opt" data-for="p : presets" data-event-click="presetload(it_index)"><div class="col">{{p.label}}</div>
            <!-- <Prestige> --><div class="rn" data-event-click="presetren(it_index)">{{presetRenameLabel}}</div><!-- </Prestige> -->
            <div class="x" data-event-click="presetdel(it_index)"><span data-if="p.on">{{deleteAsk}}</span>
              <ic name="x" data-if="!p.on"/></div></div>
          <div class="prow"><div class="field"><input id="presetname" class="bare" type="text" maxlength="40" data-value="presetName"/></div>
            <div class="btn" data-if="presetRenaming == ''" data-event-click="presetsave()">{{presetSaveLabel}}</div><!-- <Prestige> data-if: Rename instead while renaming -->
            <div class="btn" data-if="presetRenaming != ''" data-event-click="presetrengo()">{{presetRenameLabel}}</div><!-- </Prestige> --></div>
          <!-- <Prestige> where the camera stands, saved with the setup -->
          <div class="caption">{{anchorTitle}}</div>
          <div class="segmented"><div class="seg" data-for="a : anchors" data-class-on="a.on" data-event-click="shotanchor(it_index)">{{a.label}}</div></div>
          <div class="caption" data-if="anchorNote != ''">{{anchorNote}}</div>
          <!-- </Prestige> -->
        </div>
      </div>
    </div>
  </div>
  <div class="btn studio" data-event-click="openstudio()">{{openStudioLabel}}</div>
</div>
<div id="handle" class="handle"><div class="grip"></div></div>
<div id="bar" data-class-away="barAway">
  <div id="leave" class="bb" data-event-click="leave()"><span class="lbl"><ic name="x"/>{{leaveLabel}}</span></div>
  <div id="saveto" class="bb" data-class-on="popup == 'saveto'" data-event-click="pop('saveto')"><span class="lbl"><span data-if="dest == 0"><ic name="dl"/></span><span data-if="dest == 1"><ic name="box"/></span><span data-if="dest == 2"><ic name="person"/></span><span data-if="dest == 3"><ic name="mail"/></span>{{saveLabel}}</span><span class="chev" data-class-down="popup == 'saveto'"></span></div>
  <div id="shutter" data-class-down="shutterDown"></div>
  <div id="size" class="bb" data-class-on="popup == 'size'" data-event-click="pop('size')"><span class="lbl">{{sizeLabel}}</span><span class="chev" data-class-down="popup == 'size'"></span></div>
  <div id="controls" class="bb" data-class-on="controlsOpen" data-event-click="controls()"><span class="lbl"><ic name="sl"/>{{controlsLabel}}</span></div>
</div>
<div id="savepop" class="popover pop toggled" data-class-shown="popup == 'saveto'">
  <div class="opt" data-class-selected="dest == 0" data-event-click="dest(0)"><ic name="dl"/>
    <div class="col"><div>{{destDisk}}</div><div class="caption">{{diskNote}}</div></div><div class="tick" data-if="dest == 0">✓</div></div>
  <div class="opt" data-class-selected="dest == 1" data-event-click="dest(1)"><ic name="box"/>
    <div class="col"><div>{{destInventory}}</div><div class="caption">{{inventoryNote}}</div></div><div class="tick" data-if="dest == 1">✓</div></div>
  <div class="opt" data-class-selected="dest == 2" data-event-click="dest(2)"><ic name="person"/>
    <div class="col"><div>{{destProfile}}</div><div class="caption">{{profileNote}}</div></div><div class="tick" data-if="dest == 2">✓</div></div>
  <div class="opt" data-class-selected="dest == 3" data-event-click="dest(3)"><ic name="mail"/>
    <div class="col"><div>{{destEmail}}</div><div class="caption">{{emailNote}}</div></div><div class="tick" data-if="dest == 3">✓</div></div>
  <div class="sep"></div>
  <div class="opt" data-event-click="classic()"><ic name="win"/>
    <div class="col">{{classicLabel}}</div></div>
</div>
<div id="sendcard" class="popover" data-if="sendOpen">
  <div class="title">{{sendTitle}}</div>
  <div class="caption">{{sendText}}</div>
  <div class="frow" data-if="sendDest == 2"><div class="caption">{{captionLabel}}</div>
    <div class="field"><input id="sendcaption" class="bare" type="text" maxlength="250" data-value="sendCaption"/></div></div>
  <div class="trow" data-if="sendDest == 2" data-event-click="sendloc()"><div class="label">{{locationLabel}}</div>
    <div class="toggle" data-class-on="sendLocation"><div class="knob"></div></div></div>
  <div data-if="sendDest == 3">
    <div class="frow"><div class="caption">{{toLabel}}</div><div class="field"><input id="sendto" class="bare" type="text" maxlength="250" data-value="sendTo"/></div></div>
    <div class="frow"><div class="caption">{{subjectLabel}}</div><div class="field"><input id="sendsubject" class="bare" type="text" maxlength="100" data-value="sendSubject"/></div></div>
    <div class="frow"><div class="caption">{{messageLabel}}</div><div class="field tall"><textarea id="sendmessage" class="bare" data-value="sendMessage"></textarea></div></div>
  </div>
  <div class="trow" data-if="sendDest == 1 || sendDest == 2" data-event-click="sendask()"><div class="label">{{dontAskLabel}}</div>
    <div class="toggle" data-class-on="sendDontAsk"><div class="knob"></div></div></div>
  <div class="caption err" data-if="sendError != ''">{{sendError}}</div>
  <div class="acts"><div class="spacer"></div><div class="btn" data-event-click="sendcancel()">{{cancelLabel}}</div>
    <div class="btn accent" data-event-click="sendgo()">{{sendGoLabel}}</div></div>
</div>
<div id="sizepop" class="popover pop toggled" data-class-shown="popup == 'size'">
  <div class="caption">{{aspectTitle}}</div>
  <div class="segmented"><div class="seg" data-for="a : aspects" data-class-on="a.on" data-event-click="aspect(it_index)">{{a.label}}</div></div>
  <div class="caption">{{sizeTitle}}</div>
  <div class="opt" data-for="r : sizes" data-class-selected="r.on" data-event-click="pickSize(it_index)">
    <div class="radio" data-class-on="r.on"></div><div>{{r.label}}</div><div class="note">{{r.note}}</div></div>
  <div class="custom" data-if="customOn">
    <div class="field"><input id="customw" class="bare" type="text" maxlength="5" data-value="customW"/></div><div class="by">×</div>
    <div class="field"><input id="customh" class="bare" type="text" maxlength="5" data-value="customH"/></div></div>
  <div class="caption">{{typeTitle}}</div>
  <div class="segmented"><div class="seg" data-for="f : formats" data-class-on="f.on" data-event-click="format(it_index)">{{f.label}}</div></div>
  <div class="caption" data-if="isoMode != 0">{{isoPngNote}}</div><!-- <Prestige> isolate: PNG -->
  <div class="qrow" data-if="jpeg"><div>{{qualityTitle}}</div><input class="range" type="range" min="10" max="100" step="1" data-value="quality"/><div>{{quality}}</div></div>
  <div class="sep"></div>
  <div class="trow" data-event-click="post()"><div class="label">{{postTitle}}</div><div class="toggle" data-class-on="post"><div class="knob"></div></div></div>
</div>
<div id="help" class="popover" data-if="helpOpen">
  <div class="title">{{helpTitle}}</div>
  <div class="mrow" data-for="m : helpMouse">
    <ic name="mouse" data-if="m.icon != 'shutter'" data-class-left="m.icon == 'left'" data-class-right="m.icon == 'right'"
          data-class-wheel="m.icon == 'wheel'" data-class-middle="m.icon == 'middle'"/>
    <ic name="shut" data-if="m.icon == 'shutter'"/>
    <div class="name">{{m.key}}</div><div class="what">{{m.text}}</div></div>
  <div class="sep"></div>
  <div id="keys"><div class="krow" data-for="k : helpKeys"><span class="hk">{{k.key}}</span>{{k.text}}</div></div>
  <div class="sep"></div>
  <div class="foot iso">{{isoHelpLine}}</div><!-- <Prestige> isolate your subject -->
  <div class="foot">{{helpFooter}} <span class="link" data-event-click="changekeys()">{{changeKeysLabel}}</span></div>
</div>
</div></body></rml>)rml";

// A key (or mouse gesture) and what it does; `end` closes a group in the hint row.
struct KeyText
{
    std::string key, text;
    bool        end = false;
    std::string icon; // help card: which part of the mouse ("left", "right", "wheel", "middle") or "shutter"
};

// A choice of a segmented control or a list: its label, a note, whether it is the current one.
struct Choice
{
    std::string label, note;
    bool        on = false;
    std::string id; // an element id when the document needs one (the grid menu's rows)
};

// A strip cell: a camera value as the model shows it (`id`: "cell-N", N its index in kCells).
struct StripCell
{
    std::string id, value, unit, name;
    bool        off = false, marked = false;
};
const SLCameraModel::Value kCells[] = { SLCameraModel::Value::Mode,  SLCameraModel::Value::Shutter, SLCameraModel::Value::Aperture,
                                        SLCameraModel::Value::EV,    SLCameraModel::Value::ISO,     SLCameraModel::Value::WB,
                                        SLCameraModel::Value::Focal, SLCameraModel::Value::Focus,   SLCameraModel::Value::Drive };
constexpr size_t kCellCount = sizeof(kCells) / sizeof(kCells[0]);

// A thumbnail of the session's shots (`index` into SLCameraReview) and a histogram bar.
struct Thumb
{
    std::string src;
    int         index = 0;
    bool        sel   = false;
    bool operator==(const Thumb& o) const { return src == o.src && index == o.index && sel == o.sel; }
};
struct Bar
{
    std::string h; // "37%"
};
// A tick of the readout's dial.
struct Tick
{
    bool on = false, major = false;
};
constexpr int kTicks = 13;
constexpr size_t kMaxThumbs = 3; // board 29; four reach the bar on a 1280-wide window

// The Photo Studio rail as the document shows it (`id`: "rrow-S-R", `track`: "rtrack-S-R").
struct RailRow
{
    std::string id, track, label, value, w;
    int         kind = 0;
    bool        on = false, enabled = true, changed = false;
    std::string why; // greyed: why, as its tooltip
    std::vector<std::string> buttons;
};
struct RailSec
{
    std::string          title;
    bool                 open = false, setups = false; // setups: the saved camera setups go under it
    std::vector<RailRow> rows;
};

// The viewfinder as last placed (re-placed only when one of these changes).
struct Finder
{
    LLRect frame;
    U32    grid = U32(-1);
    bool   level = false, levelOk = false, held = false, locked = false, tilt = false;
    S32    roll10 = INT_MIN, pitch10 = INT_MIN; // tenths of a degree
    F32    afX = -1.f, afY = -1.f;
    S32    rootH = 0;
};

// Size presets (the "Window" row and "Custom..." around them).
const S32 kSizes[][2] = { { 1920, 1080 }, { 2560, 1440 }, { 3840, 2160 } };

struct View
{
    Rml::Context*         context  = nullptr;
    Rml::ElementDocument* document = nullptr;
    Rml::DataModelHandle  model;
    bool                  shown = false, changed = true;
    float                 density = 0.f;
    U32                   markedFrame = U32(-1);
    U32  theme = 0; // UIKitTheme the document was loaded with
    // layout
    S32  rootW = 0, rootH = 0;
    // dp per UI unit: SLCameraModeOverlaySize (1 = the UI size, like the rest of the kit).
    // W / H: the window in dp.
    float k = 1.f, W = 0.f, H = 0.f;
    bool fitPending = true;
    // input
    S32           lastX = S32_MIN, lastY = S32_MIN;
    Rml::Element* lastHover = nullptr;
    bool          pressedInDoc = false; // a left press went to the document (so does its release)
    bool          textInput = false;    // the window's text input is on for a field being typed in
    // model
    std::string           changeKeysLabel;
    std::string           leaveLabel, saveLabel, sizeLabel, controlsLabel, toast, helpTitle, helpFooter;
    bool                  controlsOpen = false, hintShown = false, helpOpen = false, shutterDown = false;
    std::vector<KeyText>  hints, helpMouse, helpKeys;
    // popovers: "" (none), "saveto", "size"
    std::string           popup, diskNote, classicLabel, aspectTitle, sizeTitle, typeTitle, qualityTitle, postTitle;
    std::vector<Choice>   aspects, sizes, formats;
    bool                  customOn = false, jpeg = false, post = true, customChosen = false;
    int                   customW = 0, customH = 0, quality = 75;
    // Controls
    std::vector<StripCell> cells;
    std::vector<Choice>    grids;
    std::string            gridTitle, levelLabel, studioLabel;
    bool                   levelOn = true, studioOn = false, tilting = false, flashOn = true;
    Finder                 finder;
    std::vector<StripCell> qcells;
    std::vector<KeyText>   qhints;
    int                    qSel = 0;
    // the bar out of the frame
    bool                   barHidden = false, barTop = false, barAway = false, peeking = false, noticeShown = false, readoutShown = false;
    bool                   handleDown = false;
    S32                    handleDownY = 0;
    F64                    hiddenAt = -100.0, readoutAt = -100.0;
    SLCameraModel::Value   readoutValue_ = SLCameraModel::Value::Mode;
    std::string            noticeTitle, noticeText, noticeEnd, toggleKey, readoutName, readoutValue, readoutHint;
    std::vector<Tick>      readoutTicks;
    int                    readoutTick = -2;
    // after the shot
    std::vector<Thumb>     thumbs;
    std::vector<Bar>       bars;
    std::vector<U32>       exposed; // shots whose textures RmlUi may hold
    bool                   savedShown = false, cardOpen = false, cardMore = false;
    U32                    cardSerial = 0;
    std::vector<RailSec>   rail;
    std::vector<Choice>    presets;
    std::vector<KeyText>   statuses; // text; end: accent (unread)
    std::string            lockMark;
    std::string            setupsTitle, tipWhat, tipLow, tipHigh, tipClick, clickWord;
    F64                    tipSince = 0.0; // the pointer reached the cell (tips wait half a second)
    int                    tipCell  = -1;
    std::string            presetArmed, deleteAsk; // a first click on x arms the delete
    std::string            presetNone, presetSaveLabel, presetName, tip, drive, ims;
    F64                    nextImPoll = 0.0;
    std::string            railSignature, openStudioLabel;
    bool                   railRight = true, railShort = false;
    int                    railDragSec = -1, railDragRow = -1;
    F64                    railNext = 0.0; // the rail's rows re-read at 4 Hz (choices list folders)
    int                    gridPreview = -1; // the grid under the pointer in the grid menu, shown in the frame
    std::string            reviewKey, allShotsLabel;
    // destinations (phase 2) and the send card
    int                    dest = 0, sendDest = 0;
    bool                   sendOpen = false, sendLocation = false, sendDontAsk = false;
    std::string            destDisk, destInventory, destProfile, destEmail, inventoryNote, profileNote, emailNote;
    std::string            sendTitle, sendText, sendError, sendCaption, sendTo, sendSubject, sendMessage, sendGoLabel;
    std::string            captionLabel, locationLabel, toLabel, subjectLabel, messageLabel, dontAskLabel, cancelLabel;
    std::string            histDark, histBright;
    bool                   savedDisk = true;
    U32                    savedShownTo = 0;
    bool                   narrow = false; // the window is too narrow for the thumbnails' key hint
    std::string            moreShots, cardSrc, cardH, cardTitle, cardMeta, cardLabel, savedLabel, folderLabel, deleteLabel;
    F64                   hintSince = 0.0, nextRefresh = 0.0;
    // <Prestige> isolate your subject
    int                    isoMode = 0, isoCount = 0, isoMarks = 0;
    bool                   isoPicking = false;
    std::vector<Choice>    isoModes;
    std::string            isoChip, isoTitle, isoPickLabel, isoClearLabel, isoHelp, isoPickText, isoPngNote, isoHelpLine;
    std::string            isoMarkSignature;
    // <Prestige> AE-L (its chip), the AF point's confirmation and the group markers shown
    bool                  aeLocked = false;
    std::string           aelLabel;
    U32                   afConfirm = 0;
    size_t                markersShown = 0;
    // </Prestige>
};
View gView;

// <Prestige> setting up the shot (prestigecamerashot.h): what the overlay shows of it
struct ShotView
{
    bool                huds = false, poser = false, frozen = false, quiet = false, skyPhoto = true, light = false;
    std::string         hudsLabel, poserLabel, freezeLabel, quietLabel, skyLabel, lightLabel;
    std::vector<Choice> anchors;   // with me / here / an object
    std::string         anchorTitle, anchorNote;
    std::string         renaming, renameLabel; // the setup being renamed ("" none)
};
ShotView gShot;
// </Prestige>

F64 now() { return LLTimer::getTotalSeconds(); }

void markChanged() { gView.changed = true; }

void refreshPresets(bool suggestName);

std::string str(const char* name)
{
    return LLTrans::getString(std::string("CameraMode") + name);
}

std::string key(const char* command)
{
    return SLKeyCommands::instance().getBindingString(command);
}

int rmlModifiers(MASK mask)
{
    int m = 0;
    if (mask & MASK_CONTROL)
        m |= Rml::Input::KM_CTRL;
    if (mask & MASK_SHIFT)
        m |= Rml::Input::KM_SHIFT;
    if (mask & MASK_ALT)
        m |= Rml::Input::KM_ALT;
    return m;
}

// The element `e` or one of its ancestors has this id.
bool within(Rml::Element* e, const char* id)
{
    for (; e; e = e->GetParentNode())
        if (e->GetId() == id)
            return true;
    return false;
}

void dirty(const char* variable)
{
    if (gView.model)
        gView.model.DirtyVariable(variable);
    markChanged();
}

template <typename T> void set(T& field, const T& value, const char* variable)
{
    if (field == value)
        return;
    field = value;
    dirty(variable);
}

// The hint row: what the mouse does in the state the user is in.
void buildHints()
{
    View& v = gView;
    v.hints.clear();
    if (v.controlsOpen)
    {
        // Board 24: what the wheel and the right button do with Controls open.
        v.hints.push_back({ str("MouseWheel"), str("HintMove"), true });
        v.hints.push_back({ str("KeyShift"), "" });
        v.hints.push_back({ str("MouseWheel"), str("HintExposure"), true });
        v.hints.push_back({ str("KeyCtrl"), "" });
        v.hints.push_back({ str("MouseWheel"), str("HintZoom"), true });
        v.hints.push_back({ str("MouseWheelOver"), str("HintThatValue"), true });
        v.hints.push_back({ str("MouseRightClick"), str("HintQuick") });
    }
    else
    {
        v.hints.push_back({ str("MouseClick"), str("HintClick"), true });
        v.hints.push_back({ str("MouseDoubleClick"), str("HintOr") });
        v.hints.push_back({ key("camera_shoot"), str("HintShoot"), true });
        v.hints.push_back({ str("MouseRightDrag"), str("HintTurn"), true });
        v.hints.push_back({ str("MouseWheel"), str("HintMove"), true });
        v.hints.push_back({ key("camera_exit"), str("HintLeave") });
    }
    dirty("hints");
}

void buildHelp()
{
    View& v = gView;
    v.helpTitle = str("HelpTitle");
    v.changeKeysLabel = str("ChangeKeys");
    v.helpFooter = str("HelpFooter");
    // Each row's icon shows the part it is about: the left or right button, the wheel, the middle
    // press, or the on-screen shutter.
    v.helpMouse = { { str("MouseClick"), str("HelpClick"), false, "left" },
                    { str("MouseDoubleClick"), str("HelpDoubleClick"), false, "left" },
                    // <Prestige> group focus
                    { str("MouseShiftClick"), str("HelpShiftClick"), false, "left" },
                    // </Prestige>
                    { str("MouseHold"), str("HelpHold"), false, "shutter" },
                    { str("MouseRightDrag"), str("HelpRightDrag"), false, "right" },
                    { str("MouseRightClick"), str("HelpRightClick"), false, "right" },
                    { str("MouseWheel"), str("HelpWheel"), false, "wheel" },
                    { str("MouseShiftWheel"), str("HelpShiftWheel"), false, "wheel" },
                    { str("MouseCtrlWheel"), str("HelpCtrlWheel"), false, "wheel" },
                    { str("MouseAltWheel"), str("HelpAltWheel"), false, "wheel" },
                    { str("MouseMiddle"), str("HelpMiddle"), false, "middle" } };
    const std::pair<const char*, const char*> keys[] = {
        { "camera_shoot", "KeyShoot" },      { "camera_af_on", "KeyFocus" },       { "camera_af_lock", "KeyLock" },
        { "camera_dof_preview", "KeyDepth" }, { "camera_q_menu", "KeyQuick" },      { "camera_review", "KeyReview" },
        { "camera_toggle_bar", "KeyBar" },    { "camera_help", "KeyHelp" },         { "camera_exit", "KeyExit" },
        // <Prestige> AE-L
        { "camera_ae_lock", "KeyExposureLock" },
        // </Prestige>
    };
    v.helpKeys.clear();
    for (const auto& [command, text] : keys)
    {
        const std::string k = key(command);
        if (!k.empty()) // an unbound command has nothing to show
            v.helpKeys.push_back({ k, str(text) });
    }
    dirty("helpMouse");
    dirty("helpKeys");
    dirty("helpTitle");
    dirty("helpFooter");
}

std::string sizeText()
{
    static LLCachedControl<S32> s_format(gSavedSettings, "SnapshotFormat", 0);
    static const char* const formats[] = { "PNG", "JPEG", "BMP" };
    S32 w = 0, h = 0;
    SLCameraModel::outputSize(w, h);
    LLStringUtil::format_map_t args;
    args["[W]"]      = llformat("%d", w);
    args["[H]"]      = llformat("%d", h);
    args["[FORMAT]"] = PrestigeCameraIsolate::enabled() ? "PNG" : formats[llclamp((S32)s_format, 0, 2)];   // <Prestige> isolate: PNG
    std::string s = str("SizeLabel");
    LLStringUtil::format(s, args);
    return s;
}

// The folder shots go to (its last two parts), or that the first shot asks for one.
std::string diskNote()
{
    const std::string dir = gSavedPerAccountSettings.getString("SnapshotBaseDir");
    if (dir.empty())
        return str("DiskAsks");
    std::string d = dir;
    while (!d.empty() && (d.back() == '/' || d.back() == '\\'))
        d.pop_back();
    const size_t last = d.find_last_of("/\\");
    const size_t prev = last == std::string::npos || last == 0 ? std::string::npos : d.find_last_of("/\\", last - 1);
    std::string tail = prev == std::string::npos ? d : d.substr(prev + 1);
    for (char& c : tail)
        if (c == '/' || c == '\\')
            c = '>';
    LLStringUtil::replaceString(tail, ">", " › ");
    return tail;
}

// The size popover's lists, from the model; dirty only what changed.
void refreshSizePopover()
{
    View& v = gView;
    std::vector<Choice> aspects, sizes, formats;
    const U32 aspect = SLCameraModel::aspect();
    for (U32 i = 0; i < SLCameraExposure::ASPECT_COUNT; ++i)
        aspects.push_back({ i == 0 ? str("AspectAsSize") : std::string(SLCameraExposure::aspectLabel(i)), "", i == aspect });
    S32 bw = 0, bh = 0, ww = 0, wh = 0;
    SLCameraModel::outputBase(bw, bh);
    SLCameraModel::windowSize(ww, wh);
    // One row is on: Window when the size follows the window, else the preset it equals, else Custom.
    const bool follows = SLCameraModel::outputFollowsWindow();
    bool preset = false;
    sizes.push_back({ str("SizeWindow"), llformat("%d × %d", ww, wh), !v.customChosen && follows });
    preset |= sizes.back().on;
    for (const auto& sz : kSizes)
    {
        sizes.push_back({ llformat("%d × %d", sz[0], sz[1]), sz[0] >= 3840 ? str("SizeSlow") : std::string(),
                          !v.customChosen && !follows && bw == sz[0] && bh == sz[1] });
        preset |= sizes.back().on;
    }
    sizes.push_back({ str("SizeCustom"), "", !preset });
    const U32 format = SLCameraModel::fileFormat();
    static const char* const names[] = { "PNG", "JPEG", "BMP" };
    for (U32 i = 0; i < 3; ++i)
        formats.push_back({ names[i], "", i == format });
    auto same = [](const std::vector<Choice>& a, const std::vector<Choice>& b)
    {
        if (a.size() != b.size())
            return false;
        for (size_t i = 0; i < a.size(); ++i)
            if (a[i].label != b[i].label || a[i].note != b[i].note || a[i].on != b[i].on)
                return false;
        return true;
    };
    if (!same(aspects, v.aspects))
    {
        v.aspects = aspects;
        dirty("aspects");
    }
    if (!same(sizes, v.sizes))
    {
        v.sizes = sizes;
        dirty("sizes");
    }
    if (!same(formats, v.formats))
    {
        v.formats = formats;
        dirty("formats");
    }
    set(v.customOn, sizes.back().on, "customOn");
    set(v.jpeg, format == 1, "jpeg");
    set(v.post, SLCameraModel::postEffects(), "post");
    // The fields follow the setting unless one is being typed in.
    if (!SLCameraView::textEditing())
    {
        set(v.customW, (int)bw, "customW");
        set(v.customH, (int)bh, "customH");
    }
    set(v.quality, (int)SLCameraModel::jpegQuality(), "quality");
}

// The bar's destination button and the save-to menu's rows, from the model.
void refreshDestination()
{
    View& v = gView;
    const int dest = (int)SLCameraModel::destination();
    static const char* const labels[] = { "SaveToDisk", "SaveToInventory", "ShareToProfile", "SendByEmail" };
    set(v.dest, dest, "dest");
    const std::string label = str(labels[dest]);
    if (label != v.saveLabel)
    {
        v.saveLabel  = label;
        v.fitPending = true;
        dirty("saveLabel");
    }
    LLStringUtil::format_map_t args;
    args["[COST]"] = llformat("%d", SLCameraModel::inventoryCost());
    set(v.inventoryNote, LLTrans::getString("CameraModeInventoryNote", args), "inventoryNote");
}

void closeSendCard()
{
    View& v = gView;
    SLCameraModel::discardPending(); // (nothing when it was sent)
    if (v.context)
        if (Rml::Element* focus = v.context->GetFocusElement())
            focus->Blur();
    set(v.sendOpen, false, "sendOpen");
    set(v.sendError, std::string(), "sendError");
}

// The card's main button (and Enter): send the waiting shot, or say what is missing.
void sendGo()
{
    View& v = gView;
    LLSD params;
    params["dont_ask"] = v.sendDontAsk;
    params["caption"]  = v.sendCaption;
    params["location"] = v.sendLocation;
    params["to"]       = v.sendTo;
    params["subject"]  = v.sendSubject;
    params["message"]  = v.sendMessage;
    const std::string error = SLCameraModel::commitPending(params);
    set(v.sendError, error, "sendError");
    if (error.empty())
        closeSendCard();
}

// A shot to the inventory or the profile asks first (until "don't ask again this session"); an
// e-mail asks for the address every time.
void openSendCard(U32 dest, S32 cost)
{
    View& v = gView;
    static const char* const titles[] = { "", "SendInventoryTitle", "SendProfileTitle", "SendEmailTitle" };
    static const char* const texts[]  = { "", "SendInventoryText", "SendProfileText", "SendEmailText" };
    static const char* const gos[]    = { "", "SendInventoryGo", "SendProfileGo", "SendEmailGo" };
    const U32 d = llmin(dest, 3u);
    LLStringUtil::format_map_t args;
    args["[COST]"] = llformat("%d", cost);
    set(v.sendDest, (int)d, "sendDest");
    set(v.sendTitle, str(titles[d]), "sendTitle");
    set(v.sendText, LLTrans::getString(std::string("CameraMode") + texts[d], args), "sendText");
    set(v.sendGoLabel, str(gos[d]), "sendGoLabel");
    set(v.sendError, std::string(), "sendError");
    set(v.sendDontAsk, false, "sendDontAsk");
    // Each card starts clean: no caption, message or location from the last one (a personal
    // message never goes to the next person); only the last address is offered again.
    set(v.sendCaption, std::string(), "sendCaption");
    set(v.sendLocation, false, "sendLocation");
    set(v.sendMessage, std::string(), "sendMessage");
    set(v.sendSubject, str("EmailSubjectDefault"), "sendSubject");
    if (d == SLCameraModel::DEST_EMAIL)
        set(v.sendTo, SLCameraModel::lastEmailTo(), "sendTo");
    set(v.helpOpen, false, "helpOpen");
    set(v.popup, std::string(), "popup");
    set(v.sendOpen, true, "sendOpen");
}

void openPopup(const std::string& which)
{
    View& v = gView;
    if (which == "size")
    {
        v.customChosen = false;
        refreshSizePopover();
    }
    else if (which == "saveto")
        set(v.diskNote, diskNote(), "diskNote");

    if (!which.empty())
        set(v.helpOpen, false, "helpOpen");
    if (which.empty() && v.context)
        if (Rml::Element* focus = v.context->GetFocusElement())
            focus->Blur(); // a field being typed in lets go
    set(v.popup, which, "popup");
}

// The strip's cells and the chips, from the model; dirty only what changed.
void refreshControls()
{
    View& v = gView;
    std::vector<StripCell> cells;
    cells.reserve(kCellCount);
    for (size_t i = 0; i < kCellCount; ++i)
    {
        const SLCameraModel::Cell c = SLCameraModel::cell(kCells[i]);
        cells.push_back({ llformat("cell-%d", (int)i), c.value, c.unit, c.name, c.off, c.marked });
    }
    bool same = cells.size() == v.cells.size();
    for (size_t i = 0; same && i < cells.size(); ++i)
        same = cells[i].value == v.cells[i].value && cells[i].unit == v.cells[i].unit && cells[i].name == v.cells[i].name
               && cells[i].off == v.cells[i].off && cells[i].marked == v.cells[i].marked;
    if (!same)
    {
        v.cells = std::move(cells);
        dirty("cells");
    }
    const U32 grid = SLCameraModel::grid();
    LLStringUtil::format_map_t args;
    args["[NAME]"] = LLTrans::getString(llformat("CameraModeGrid%u", grid));
    set(v.gridTitle, LLTrans::getString("CameraModeGridTitle", args), "gridTitle");
    std::vector<Choice> grids;
    for (U32 i = 0; i < SLCameraExposure::GRID_COUNT; ++i)
        grids.push_back({ LLTrans::getString(llformat("CameraModeGrid%u", i)), "", i == grid, llformat("gridopt-%u", i) });
    bool sameGrids = grids.size() == v.grids.size();
    for (size_t i = 0; sameGrids && i < grids.size(); ++i)
        sameGrids = grids[i].on == v.grids[i].on && grids[i].label == v.grids[i].label;
    if (!sameGrids)
    {
        v.grids = grids;
        dirty("grids");
    }
    set(v.levelOn, SLCameraModel::level(), "levelOn");
    set(v.studioOn, SLCameraRail::open(), "studioOn");
    set(v.flashOn, SLCameraModel::flashOn(), "flashOn");
    // <Prestige> AE-L
    set(v.aeLocked, SLCameraModel::exposureLocked(), "aeLocked");
    // </Prestige>
}

// The quick settings grid: the Q menu's items, as value + plain word (mode and ISO as the strip
// shows them).
void refreshQuick()
{
    View& v = gView;
    const std::vector<std::string>& items = SLCameraModel::qItems();
    std::vector<StripCell> cells;
    for (size_t i = 0; i < items.size(); ++i)
    {
        StripCell c;
        c.id = llformat("qcell-%d", (int)i);
        if (i == 0 || i == 1)
        {
            const SLCameraModel::Cell m = SLCameraModel::cell(i == 0 ? SLCameraModel::Value::Mode : SLCameraModel::Value::ISO);
            c.value = m.value;
            c.name  = m.name;
        }
        else
        {
            c.value = SLCameraModel::qValue((S32)i);
            c.name  = LLTrans::getString(llformat("CameraModeQName%d", (int)i));
        }
        c.marked = (int)i == v.qSel;
        cells.push_back(c);
    }
    bool same = cells.size() == v.qcells.size();
    for (size_t i = 0; same && i < cells.size(); ++i)
        same = cells[i].value == v.qcells[i].value && cells[i].name == v.qcells[i].name && cells[i].marked == v.qcells[i].marked;
    if (!same)
    {
        v.qcells = std::move(cells);
        dirty("qcells");
    }
}

// Opens the quick settings with its top-left at (left, top) dp, kept inside the window.
void showQuick(float left, float top)
{
    View& v = gView;
    if (v.qhints.empty())
    {
        v.qhints = { { str("MouseWheel"), str("QChange") }, { str("MouseClick"), str("QNext") }, { key("camera_exit"), str("QClose") } };
        dirty("qhints");
    }
    refreshQuick();
    openPopup("quick");
    const float w = 480.f, h = 150.f;
    if (Rml::Element* e = v.document->GetElementById("quick"))
    {
        e->SetProperty("left", llformat("%.1fdp", llclamp(left, 8.f, llmax(8.f, v.W - w - 8.f))));
        e->SetProperty("top", llformat("%.1fdp", llclamp(top, 8.f, llmax(8.f, v.H - h - 8.f))));
    }
}

std::string shotSource(U32 serial, const char* kind)
{
    return llformat("llext:camshot/%u/%s", serial, kind);
}

// The review's index of a shot, or -1.
int shotIndex(U32 serial)
{
    for (size_t i = 0; i < SLCameraReview::count(); ++i)
        if (SLCameraReview::serial(i) == serial)
            return (int)i;
    return -1;
}

// "llext:camshot/<serial>/thumb|preview": the session's shots as RmlUi textures. RmlUi owns
// them (its own upload, released with Rml::ReleaseTexture): an engine texture sampled in place
// can be re-uploaded or freed by texture streaming while the overlay still draws it.
Rml::TextureHandle shotTexture(LLRmlUiVK& vk, Rml::Vector2i& size, const std::string& source)
{
    unsigned serial = 0;
    char     kind[16] = {};
    if (std::sscanf(source.c_str(), "llext:camshot/%u/%15s", &serial, kind) != 2)
        return 0;
    const int i = shotIndex(serial);
    if (i < 0)
        return 0;
    const LLImageRaw* raw = std::string(kind) == "preview" ? SLCameraReview::preview((size_t)i) : SLCameraReview::thumbnail((size_t)i);
    if (!raw || !raw->getData() || raw->getWidth() <= 0 || raw->getHeight() <= 0)
        return 0;
    const S32 w = raw->getWidth(), h = raw->getHeight(), comps = raw->getComponents();
    std::vector<Rml::byte> rgba;
    try
    {
        rgba.resize((size_t)w * (size_t)h * 4);
    }
    catch (const std::bad_alloc&)
    {
        return 0;
    }
    // LLImageRaw rows run bottom-up; RmlUi wants them top-down, RGBA (opaque).
    const U8* src = raw->getData();
    for (S32 y = 0; y < h; ++y)
    {
        const U8*  row = src + (size_t)(h - 1 - y) * (size_t)w * (size_t)comps;
        Rml::byte* out = rgba.data() + (size_t)y * (size_t)w * 4;
        for (S32 x = 0; x < w; ++x, row += comps, out += 4)
        {
            out[0] = row[0];
            out[1] = comps >= 3 ? row[1] : row[0];
            out[2] = comps >= 3 ? row[2] : row[0];
            out[3] = 255;
        }
    }
    size = { int(w), int(h) };
    return vk.GenerateTexture(Rml::Span<const Rml::byte>(rgba.data(), rgba.size()), size);
}

void releaseShot(U32 serial)
{
    Rml::ReleaseTexture(shotSource(serial, "thumb"));
    Rml::ReleaseTexture(shotSource(serial, "preview"));
}

void fillCard(int i)
{
    View& v = gView;
    if (i < 0)
    {
        set(v.cardOpen, false, "cardOpen");
        return;
    }
    v.cardSerial = SLCameraReview::serial((size_t)i);
    S32 w = 0, h = 0;
    SLCameraReview::imageSize((size_t)i, w, h);
    const std::string& p = SLCameraReview::path((size_t)i);
    const size_t dot = p.find_last_of('.');
    std::string type = dot == std::string::npos ? std::string() : p.substr(dot + 1);
    LLStringUtil::toUpper(type);
    set(v.cardSrc, SLCameraReview::preview((size_t)i) ? shotSource(v.cardSerial, "preview") : std::string(), "cardSrc");
    set(v.cardH, llformat("%.1fdp", 300.f * (float)h / (float)llmax(w, 1)), "cardH");
    LLStringUtil::format_map_t args;
    args["[TIME]"] = SLCameraReview::timeTaken((size_t)i);
    set(v.cardTitle, LLTrans::getString("CameraModeShotTitle", args), "cardTitle");
    set(v.cardMeta, type.empty() ? llformat("%d × %d", w, h) : llformat("%d × %d · %s", w, h, type.c_str()), "cardMeta");
    set(v.cardLabel, SLCameraReview::label((size_t)i), "cardLabel");
    std::vector<Bar> bars;
    if (const F32* hist = SLCameraReview::histogram((size_t)i))
        for (size_t b = 0; b < SLCameraReview::HIST_BINS; ++b)
            bars.push_back({ llformat("%d%%", llclamp(ll_round(hist[b] * 100.f), 1, 100)) });
    v.bars = std::move(bars);
    dirty("bars");
    v.cardMore = false;
    dirty("cardMore");
    set(v.cardOpen, true, "cardOpen");
}

// The thumbnails (the newest that fit, then "+N") and the card, from the review; textures of
// shots that left the review are let go.
void refreshReview()
{
    View& v = gView;
    const size_t n = SLCameraReview::count();
    std::vector<U32> live;
    for (size_t i = 0; i < n; ++i)
        live.push_back(SLCameraReview::serial(i));
    for (U32 serial : v.exposed)
        if (std::find(live.begin(), live.end(), serial) == live.end())
            releaseShot(serial);
    v.exposed = live;
    std::vector<Thumb> thumbs;
    size_t shown = 0;
    for (size_t k = 0; k < n && shown < kMaxThumbs; ++k)
    {
        const size_t i = n - 1 - k; // newest first, laid out oldest to newest
        if (!SLCameraReview::thumbnail(i))
            continue;
        thumbs.insert(thumbs.begin(), { shotSource(SLCameraReview::serial(i), "thumb"), (int)i,
                                        v.cardOpen && SLCameraReview::serial(i) == v.cardSerial });
        ++shown;
    }
    if (thumbs != v.thumbs)
    {
        v.thumbs = std::move(thumbs);
        dirty("thumbs");
    }
    set(v.moreShots, n > shown ? llformat("+%d", (int)(n - shown)) : std::string(), "moreShots");
    if (v.cardOpen && shotIndex(v.cardSerial) < 0)
        fillCard(n ? (int)n - 1 : -1); // its shot went: the newest, or none
}

// Show in folder / Delete when the file is gone: say so.
bool fileThere(int i)
{
    const std::string& p = i >= 0 ? SLCameraReview::path((size_t)i) : std::string();
    if (!p.empty() && LLFile::isfile(p))
        return true;
    SLCameraModel::toast(LLTrans::getString("CameraModeFileGone"));
    return false;
}

// The saved camera setups and a name to save the next one under.
void refreshPresets(bool suggestName)
{
    View& v = gView;
    std::vector<Choice> list;
    for (const std::string& name : SLCameraPresets::list())
        list.push_back({ name, "", name == v.presetArmed });
    v.presets = std::move(list);
    dirty("presets");
    if (suggestName)
    {
        for (int i = 1;; ++i)
        {
            LLStringUtil::format_map_t args;
            args["[N]"] = llformat("%d", i);
            const std::string name = LLTrans::getString("CameraModePresetDefault", args);
            bool taken = false;
            for (const Choice& c : v.presets)
                taken |= c.label == name;
            if (!taken)
            {
                set(v.presetName, name, "presetName");
                break;
            }
        }
    }
}

std::string presetMessage(const char* name, const std::string& preset)
{
    LLStringUtil::format_map_t args;
    args["[NAME]"] = preset;
    return LLTrans::getString(std::string("CameraMode") + name, args);
}

// <Prestige> setting up the shot: the chips, the light and where the camera stands, from
// PrestigeCameraShot; the light above the shutter, clear of the bar, the strip and the hints.
void placeLight()
{
    View& v = gView;
    if (!v.document || v.density <= 0.f || v.barAway)
        return;
    Rml::Element* light = v.document->GetElementById("lightbtn");
    if (!light)
        return;
    const float px = v.density * v.k;
    float edge = v.barTop ? 0.f : v.H;
    bool  any  = false;
    for (const char* id : { "bar", "strip", "hintpill" })
    {
        Rml::Element* e = v.document->GetElementById(id);
        if (!e || e->GetOffsetHeight() <= 0.f)
            continue;
        const float top = e->GetAbsoluteTop() / px, bottom = top + e->GetOffsetHeight() / px;
        edge = v.barTop ? llmax(edge, bottom) : llmin(edge, top);
        any  = true;
    }
    if (!any)
        return;
    const std::string placed = v.barTop ? llformat("t%.1f", edge + 8.f) : llformat("b%.1f", v.H - edge + 8.f);
    if (light->GetAttribute<Rml::String>("data-placed", "") == placed)
        return;
    light->SetAttribute("data-placed", placed);
    light->SetProperty(v.barTop ? "top" : "bottom", llformat("%.1fdp", v.barTop ? edge + 8.f : v.H - edge + 8.f));
    light->SetProperty(v.barTop ? "bottom" : "top", "auto");
    markChanged();
}

// The setup chips in the chips' row, left of them (the rail docks under the row); with the bar at
// the top edge (it would run into it) under the chips, above the rail.
void placeShotChips()
{
    View& v = gView;
    if (!v.document || v.density <= 0.f)
        return;
    Rml::Element* row   = v.document->GetElementById("shotchips");
    Rml::Element* chips = v.document->GetElementById("chips");
    if (!row || !chips || chips->GetOffsetWidth() <= 0.f)
        return;
    const float px    = v.density * v.k;
    const float right = v.barTop ? 24.f : v.W - chips->GetAbsoluteLeft() / px + 8.f;
    const float top   = v.barTop ? (chips->GetAbsoluteTop() + chips->GetOffsetHeight()) / px + 8.f : 24.f;
    const std::string placed = llformat("%.1f|%.1f", right, top);
    if (row->GetAttribute<Rml::String>("data-placed", "") == placed)
        return;
    row->SetAttribute("data-placed", placed);
    row->SetProperty("right", llformat("%.1fdp", right));
    row->SetProperty("top", llformat("%.1fdp", top));
    markChanged();
}

void refreshShot()
{
    ShotView& s = gShot;
    if (s.hudsLabel.empty())
    {
        s.hudsLabel   = str("ShotHUDs");
        s.poserLabel  = str("ShotPoser");
        s.freezeLabel = str("ShotFreeze");
        s.quietLabel  = str("ShotQuiet");
        s.lightLabel  = str("ShotLight");
        s.anchorTitle = str("AnchorTitle");
        s.renameLabel = str("PresetRename");
        for (const char* name : { "shotHudsLabel", "shotPoserLabel", "shotFreezeLabel", "shotQuietLabel", "shotLightLabel",
                                  "anchorTitle", "presetRenameLabel" })
            dirty(name);
    }
    set(s.huds, PrestigeCameraShot::showHUDs(), "shotHuds");
    set(s.poser, PrestigeCameraShot::toolOpen(PrestigeCameraShot::Tool::Poser), "shotPoser");
    set(s.light, PrestigeCameraShot::toolOpen(PrestigeCameraShot::Tool::Light), "shotLight");
    set(s.frozen, PrestigeCameraShot::worldFrozen(), "shotFrozen");
    set(s.quiet, PrestigeCameraShot::quiet(), "shotQuiet");
    const bool photo = PrestigeCameraShot::envForPhotoOnly();
    set(s.skyPhoto, photo, "shotSkyPhoto");
    set(s.skyLabel, str(photo ? "ShotSkyPhoto" : "ShotSkyKeep"), "shotSkyLabel");
    static const char* const anchors[] = { "AnchorAvatar", "AnchorPlace", "AnchorObject" };
    const S32 a = PrestigeCameraShot::anchor();
    bool same = s.anchors.size() == (size_t)PrestigeCameraShot::ANCHOR_COUNT;
    for (S32 i = 0; same && i < PrestigeCameraShot::ANCHOR_COUNT; ++i)
        same = s.anchors[(size_t)i].on == (i == a);
    if (!same)
    {
        s.anchors.clear();
        for (S32 i = 0; i < PrestigeCameraShot::ANCHOR_COUNT; ++i)
            s.anchors.push_back({ str(anchors[(size_t)i]), "", i == a });
        dirty("anchors");
    }
    set(s.anchorNote, PrestigeCameraShot::anchorNote(), "anchorNote");
    placeLight();
    placeShotChips();
}
// </Prestige>

// What the strip cell under the pointer does, in plain words: the strings' base name.
const char* tipFor(SLCameraModel::Value value)
{
    switch (value)
    {
        case SLCameraModel::Value::Mode:
        {
            static const char* const modes[] = { "TipModeP", "TipModeAv", "TipModeTv", "TipModeM" };
            return modes[llmin(SLCameraModel::exposureMode(), 3u)];
        }
        case SLCameraModel::Value::Shutter: return "TipShutter";
        case SLCameraModel::Value::Aperture: return "TipAperture";
        case SLCameraModel::Value::EV: return SLCameraModel::exposureMode() == SLCameraExposure::MODE_MANUAL ? "TipMeter" : "TipEV";
        case SLCameraModel::Value::ISO: return "TipISO";
        case SLCameraModel::Value::WB: return "TipWB";
        case SLCameraModel::Value::Focal: return "TipFocal";
        case SLCameraModel::Value::Focus: return "TipFocus";
        case SLCameraModel::Value::Drive: return "TipDrive";
        default: return "";
    }
}

// The rail's rows explain themselves too: their tip by the row's label (or its first button).
const char* railTip(const RailRow& r)
{
    static const std::pair<const char*, const char*> keys[] = {
        { "Look", "TipRailLook" },           { "Colour grade", "TipRailColourGrade" }, { "Contrast", "TipRailContrast" },
        { "Saturation", "TipRailSaturation" }, { "Vibrance", "TipRailVibrance" },     { "Brightness", "TipRailBrightness" },
        { "Tint", "TipRailTint" },           { "Hold the sky", "TipRailHoldSky" },    { "Time of day", "TipRailTimeOfDay" },
        { "Sunrise", "TipRailSkyPresets" },  { "Aurora", "TipRailAurora" },           { "Haze start", "TipRailHazeStart" },
        { "Haze opacity", "TipRailHazeOpacity" }, { "Haze: automatic", "TipRailHazeAuto" }, { "Sun shafts", "TipRailSunShafts" },
        { "Water mist", "TipRailWaterMist" }, { "Effects (FX)", "TipRailEffects" },   { "FX preset", "TipRailFxPreset" },
        { "Bloom", "TipRailBloom" },         { "Lens flare", "TipRailLensFlare" },    { "Vignette", "TipRailVignette" },
        { "Film grain", "TipRailFilmGrain" }, { "Sharpen", "TipRailSharpen" },        { "Shadows", "TipRailShadows" },
        { "Ambient occlusion", "TipRailAmbientOcclusion" }, { "God rays", "TipRailGodRays" },
        { "Draw distance", "TipRailDrawDistance" }, { "Avatars in shot", "TipRailAvatarsInShot" }, { "View", "TipRailView" },
        { "Rear", "TipRailCameraViews" },
    };
    const std::string& label = r.label.empty() && !r.buttons.empty() ? r.buttons.front() : r.label;
    for (const auto& [l, k] : keys)
        if (label == l)
            return k;
    return "";
}

// A tip part ("Title", "What", "Low", "High", "Click"); empty when that tip has none.
std::string tipPart(const char* base, const char* part)
{
    std::string out;
    LLStringUtil::format_map_t args;
    args["[DEPTH_KEY]"] = key("camera_dof_preview");
    args["[SHOOT_KEY]"] = key("camera_shoot");
    if (*base && !LLTrans::findString(out, std::string("CameraMode") + base + part, args))
        out.clear();
    return out;
}

// "rrow-3-2" / "rtrack-3-2" -> section 3, row 2.
bool railIds(const std::string& id, int& section, int& row)
{
    const size_t dash = id.find('-');
    return dash != std::string::npos && std::sscanf(id.c_str() + dash + 1, "%d-%d", &section, &row) == 2;
}

// The rail's sections and rows from SLCameraRail (only while it shows); dirty when anything changed.
void refreshRail()
{
    View& v = gView;
    set(v.railRight, SLCameraRail::dockedRight(), "railRight");
    std::vector<RailSec> rail;
    std::string sig;
    for (size_t si = 0; si < SLCameraRail::sectionCount(); ++si)
    {
        RailSec sec;
        sec.title  = SLCameraRail::sectionTitle(si);
        sec.open   = SLCameraRail::sectionOpen(si);
        sec.setups = sec.open && si == SLCameraRail::cameraSection();
        sig += sec.title + (sec.open ? "+" : "-");
        if (sec.open)
        {
            const std::vector<SLCameraRail::RowData> rows = SLCameraRail::rows(si);
            for (size_t ri = 0; ri < rows.size(); ++ri)
            {
                const SLCameraRail::RowData& d = rows[ri];
                RailRow r;
                r.id      = llformat("rrow-%d-%d", (int)si, (int)ri);
                r.track   = llformat("rtrack-%d-%d", (int)si, (int)ri);
                r.label   = d.label;
                r.value   = d.value;
                r.w       = llformat("%d%%", ll_round(d.fraction * 100.f));
                r.kind    = d.kind;
                r.on      = d.on;
                r.enabled = d.enabled;
                r.changed = d.changed;
                r.why     = d.why;
                r.buttons = d.buttons;
                sig += r.label + "|" + r.value + "|" + r.w + (r.on ? "1" : "0") + (r.enabled ? "e" : "d") + (r.changed ? "c" : "");
                for (const std::string& b : r.buttons)
                    sig += b;
                sec.rows.push_back(std::move(r));
            }
        }
        rail.push_back(std::move(sec));
    }
    if (sig != v.railSignature)
    {
        v.railSignature = sig;
        v.rail          = std::move(rail);
        dirty("rail");
    }
}

// The rail stops above the strip, hints and tips when one of them would run under it (they stay
// centred on the window, in line with the bar).
void fitRail()
{
    View& v = gView;
    Rml::Element* rail = v.document->GetElementById("rail");
    if (!rail)
        return;
    const float railL = rail->GetAbsoluteLeft(), railR = railL + rail->GetOffsetWidth();
    bool under = false;
    for (const char* id : { "strip", "hintpill", "tippill" })
        if (Rml::Element* e = v.document->GetElementById(id))
        {
            const float l = e->GetAbsoluteLeft(), r = l + e->GetOffsetWidth();
            if (e->GetOffsetWidth() > 0.f && r > railL && l < railR)
                under = true;
        }
    set(v.railShort, under, "railShort");
}

// A slider follows the pointer across its track.
void railDragTo(S32 x)
{
    View& v = gView;
    Rml::Element* track = v.document ? v.document->GetElementById(llformat("rtrack-%d-%d", v.railDragSec, v.railDragRow)) : nullptr;
    if (!track || v.density <= 0.f)
        return;
    const float left  = track->GetAbsoluteLeft();
    const float width = track->GetClientWidth();
    if (width <= 0.f)
        return;
    SLCameraRail::setFraction((size_t)v.railDragSec, (size_t)v.railDragRow, ((float)x * v.density - left) / width);
}

void place(const char* id, float left, float top, float w, float h)
{
    if (Rml::Element* e = gView.document->GetElementById(id))
    {
        e->SetProperty("display", "block");
        e->SetProperty("left", llformat("%.2fdp", left));
        e->SetProperty("top", llformat("%.2fdp", top));
        e->SetProperty("width", llformat("%.2fdp", w));
        e->SetProperty("height", llformat("%.2fdp", h));
    }
}

void hide(const char* id)
{
    if (Rml::Element* e = gView.document->GetElementById(id))
        e->SetProperty("display", "none");
}

// <Prestige> focus confirmation on the AF point (green: achieved, red: failed), and a small marker
// on each person of a group focus (their eyes, inside the frame; green once the focus is achieved).
void updatePrestigeFocus()
{
    View& v = gView;
    if (!v.document)
        return;
    const U32 confirm = SLCameraModel::focusConfirm();
    if (Rml::Element* e = v.document->GetElementById("af"))
    {
        e->SetClass("ok", confirm == 2);
        e->SetClass("fail", confirm == 3);
    }
    if (confirm != v.afConfirm)
    {
        v.afConfirm = confirm;
        markChanged();
    }
    std::vector<SLCameraModel::GroupMarker> marks;
    SLCameraModel::groupMarkers(marks);
    const LLRect fr = SLCameraModel::frame();
    size_t shown = 0;
    for (const SLCameraModel::GroupMarker& m : marks)
    {
        if (shown >= 8)
            break;
        if (!fr.pointInRect(ll_round(m.x), ll_round(m.y)))
            continue;
        const std::string id = llformat("gm-%u", (U32)shown);
        place(id.c_str(), m.x / v.k - 7.f, v.H - m.y / v.k - 7.f, 14.f, 14.f);
        if (Rml::Element* e = v.document->GetElementById(id))
            e->SetClass("ok", confirm == 2);
        ++shown;
    }
    for (size_t i = shown; i < v.markersShown; ++i)
        hide(llformat("gm-%u", (U32)i).c_str());
    if (shown > 0 || v.markersShown > 0)
        markChanged();
    v.markersShown = shown;
}
// </Prestige>

// The crop mask, grid, level, tilt knob and AF point from the model, in dp from the top-left.
void updateFinder()
{
    View& v = gView;
    Finder f;
    f.frame = SLCameraModel::frame();
    // Quick shot is the picture alone (board 21): the grid, level and tilt come with Controls.
    f.grid  = !v.controlsOpen ? (U32)SLCameraExposure::GRID_OFF
              : v.gridPreview >= 0 ? (U32)v.gridPreview : SLCameraModel::grid();
    f.level = v.controlsOpen && SLCameraModel::level();
    f.tilt  = v.controlsOpen;
    f.held  = SLCameraModel::focusHeld();
    F32 roll = 0.f, pitch = 0.f;
    SLCameraModel::levelAngles(roll, pitch);
    f.roll10  = ll_round(roll * RAD_TO_DEG * 10.f);
    f.pitch10 = ll_round(pitch * RAD_TO_DEG * 10.f);
    f.levelOk = fabsf(roll) < SLCameraModel::levelTolerance();
    SLCameraModel::afPoint(f.afX, f.afY);
    f.locked = SLCameraModel::focusLocked();
    f.rootH = ll_round(v.H * 100.f);
    const Finder& o = v.finder;
    if (f.frame == o.frame && f.grid == o.grid && f.level == o.level && f.levelOk == o.levelOk && f.held == o.held && f.locked == o.locked && f.tilt == o.tilt
        && f.roll10 == o.roll10 && f.pitch10 == o.pitch10 && f.afX == o.afX && f.afY == o.afY && f.rootH == o.rootH)
        return;
    v.finder = f;
    markChanged();
    // The frame comes in UI units (y up); the document wants dp from the top-left.
    const float W = v.W, H = v.H, k = v.k;
    const float l = (float)f.frame.mLeft / k, t = H - (float)f.frame.mTop / k, w = (float)f.frame.getWidth() / k,
                h = (float)f.frame.getHeight() / k;
    // The crop: what is outside the shot is dimmed.
    place("m-t", 0.f, 0.f, W, llmax(0.f, t));
    place("m-b", 0.f, t + h, W, llmax(0.f, H - t - h));
    place("m-l", 0.f, t, llmax(0.f, l), h);
    place("m-r", l + w, t, llmax(0.f, W - l - w), h);
    // The grid.
    for (const char* id : { "g-v1", "g-v2", "g-h1", "g-h2", "g-d1", "g-d2", "g-cx", "g-cy" })
        hide(id);
    auto vline = [&](const char* id, float at) { place(id, l + w * at, t, 1.f, h); };
    auto hline = [&](const char* id, float at) { place(id, l, t + h * at, w, 1.f); };
    switch (f.grid)
    {
        case SLCameraExposure::GRID_THIRDS:
        case SLCameraExposure::GRID_THIRDS_DIAGONALS:
            vline("g-v1", 1.f / 3.f);
            vline("g-v2", 2.f / 3.f);
            hline("g-h1", 1.f / 3.f);
            hline("g-h2", 2.f / 3.f);
            if (f.grid == SLCameraExposure::GRID_THIRDS_DIAGONALS)
            {
                const float diag = sqrtf(w * w + h * h), deg = atan2f(h, w) * RAD_TO_DEG;
                for (const auto& [id, sign] : { std::make_pair("g-d1", 1.f), std::make_pair("g-d2", -1.f) })
                {
                    place(id, l + w * 0.5f - diag * 0.5f, t + h * 0.5f, diag, 1.f);
                    if (Rml::Element* e = v.document->GetElementById(id))
                        e->SetProperty("transform", llformat("rotate(%.3fdeg)", sign * deg));
                }
            }
            break;
        case SLCameraExposure::GRID_GOLDEN:
        {
            const float g = 0.381966f; // 1 - 1/phi
            vline("g-v1", g);
            vline("g-v2", 1.f - g);
            hline("g-h1", g);
            hline("g-h2", 1.f - g);
            break;
        }
        case SLCameraExposure::GRID_CENTRE:
        {
            const float arm = llmin(w, h) * 0.04f;
            place("g-cx", l + w * 0.5f - arm, t + h * 0.5f, 2.f * arm, 1.f);
            place("g-cy", l + w * 0.5f, t + h * 0.5f - arm, 1.f, 2.f * arm);
            break;
        }
        default:
            break;
    }
    // The AF point.
    place("af", l + f.afX * w - 20.f, t + (1.f - f.afY) * h - 20.f, 40.f, 40.f);
    if (Rml::Element* e = v.document->GetElementById("af"))
        e->SetClass("held", f.held);
    if (Rml::Element* e = v.document->GetElementById("af"))
        e->SetClass("locked", f.locked);
    if (!f.tilt)
    {
        for (const char* id : { "level", "levelinfo", "tiltband", "tilthit", "tiltknob", "tiltlabel" })
            hide(id);
        return;
    }
    // The tilt (Dutch angle): an arc at the frame's right, -40..+40 degrees, the knob at the roll
    // (level: straight right of the centre). The band along the arc takes the drag and the wheel:
    // a big target, shown with Controls whether Level is on or not (tilt is not only for levelling).
    {
        const float cx = l + w * 0.5f, cy = t + h * 0.5f, r = llmin(w, h) * 0.42f;
        const float lim = SLCameraModel::tiltLimit(), rs = llclamp(roll, -lim, lim);
        const float bl = cx + r * cosf(lim) - 14.f, bt = cy - r * sinf(lim) - 12.f;
        const float bw = cx + r + 16.f - bl, bh = 2.f * (r * sinf(lim) + 12.f);
        place("tiltband", bl, bt, bw, bh);
        // The ring: a gradient 1.4 dp wide on a circle of radius r, clipped by the band.
        const float R = r + 3.f;
        place("tiltring", cx - R - bl, cy - R - bt, 2.f * R, 2.f * R);
        if (Rml::Element* e = v.document->GetElementById("tiltring"))
            e->SetProperty("decorator", llformat("radial-gradient(circle closest-side, #ffffff00 %.3f%%, #ffffffa6 %.3f%%, #ffffffa6 %.3f%%, #ffffff00 %.3f%%)",
                                                 100.f * (r - 1.6f) / R, 100.f * (r - 0.7f) / R, 100.f * (r + 0.7f) / R, 100.f * (r + 1.6f) / R));
        // Where it takes the pointer: a box around each 10-degree stretch of the arc, about 18 dp
        // either side of the line (not the whole rectangle the arc spans).
        place("tilthit", 0.f, 0.f, 0.f, 0.f);
        for (int i = 0; i < 8; ++i)
        {
            const float a0 = (float)(i * 10 - 40) * DEG_TO_RAD, a1 = a0 + 10.f * DEG_TO_RAD, pad = 18.f;
            const float x0 = cx + r * cosf(a0), y0 = cy - r * sinf(a0), x1 = cx + r * cosf(a1), y1 = cy - r * sinf(a1);
            place(llformat("th-%d", i).c_str(), llmin(x0, x1) - pad, llmin(y0, y1) - pad, std::fabs(x1 - x0) + 2.f * pad,
                  std::fabs(y1 - y0) + 2.f * pad);
        }
        // Marks every 10 degrees; level (0) is bigger.
        for (int i = 0; i <= 8; ++i)
        {
            const float a = (float)(i * 10 - 40) * DEG_TO_RAD, d = i == 4 ? 9.f : 5.f;
            place(llformat("td-%d", i).c_str(), cx + r * cosf(a) - bl - d * 0.5f, cy - r * sinf(a) - bt - d * 0.5f, d, d);
        }
        // Screen y is down: a positive roll (left side up) puts the knob above the centre line.
        const float kx = cx + r * cosf(rs), ky = cy - r * sinf(rs);
        place("tiltknob", kx - 9.f, ky - 9.f, 18.f, 18.f);
        if (Rml::Element* e = v.document->GetElementById("tiltknob"))
            e->SetClass("ok", f.roll10 == 0);
        if (Rml::Element* e = v.document->GetElementById("tiltlabel"))
        {
            place("tiltlabel", kx + 14.f, ky - 8.f, 110.f, 16.f);
            LLStringUtil::format_map_t args;
            args["[ANGLE]"] = f.roll10 == 0 ? std::string("0°") : llformat("%+.1f°", f.roll10 / 10.f);
            e->SetInnerRML("<span class=\"pill\">" + LLTrans::getString("CameraModeTiltLabel", args) + "</span>");
        }
    }
    // The level: the horizon through the centre at the camera's roll (Level on).
    const float half = llmin(w, h) * 0.18f, gap = 10.f;
    if (!f.level)
    {
        hide("level");
        hide("levelinfo");
        return;
    }
    place("level", l + w * 0.5f - half, t + h * 0.5f - 1.f, 2.f * half, 2.f);
    place("lv-l", 0.f, 0.f, half - gap, 2.f);
    place("lv-r", half + gap, 0.f, half - gap, 2.f);
    if (Rml::Element* e = v.document->GetElementById("level"))
    {
        // Screen y is down: a positive roll (left side up) turns the line counter-clockwise.
        e->SetProperty("transform", llformat("rotate(%.2fdeg)", -roll * RAD_TO_DEG));
        e->SetClass("ok", f.levelOk);
    }
    if (Rml::Element* e = v.document->GetElementById("levelinfo"))
    {
        place("levelinfo", l + w * 0.5f + half + 22.f, t + h * 0.5f - 8.f, 140.f, 16.f);
        // The pitch, in words (the tilt is on the arc): gone once the camera looks straight ahead.
        LLStringUtil::format_map_t args;
        args["[ANGLE]"] = llformat("%.1f°", std::abs(f.pitch10) / 10.f);
        e->SetInnerRML(std::abs(f.pitch10) < 5 ? std::string()
                                               : "<span class=\"pill\">" + LLTrans::getString(f.pitch10 > 0 ? "CameraModeLookingUp" : "CameraModeLookingDown", args)
                                                     + "</span>");
    }
}

// The chips' menus open under their chip, right-aligned to it, and stay in the space that is
// left: up to the rail when it is open on the right, else up to the window's edge.
void placeChipMenus()
{
    View& v = gView;
    const std::pair<const char*, const char*> menus[] = { { "gridpop", "gridchip" }, { "isopop", "isochip" } };   // <Prestige> + isolate
    for (const auto& [menuId, chipId] : menus)
    {
        Rml::Element* menu = v.document->GetElementById(menuId);
        Rml::Element* chip = v.document->GetElementById(chipId);
        if (!menu || !chip || v.density <= 0.f)
            continue;
        const float px    = v.density * v.k; // pixels per dp
        const float right = (chip->GetAbsoluteLeft() + chip->GetOffsetWidth()) / px;
        const float width = menu->GetOffsetWidth() > 0.f ? menu->GetOffsetWidth() / px : 260.f;
        const bool  rail  = v.controlsOpen && v.studioOn && !v.barAway && v.railRight;
        const float limit = (rail ? v.W - 300.f : v.W) - 8.f;
        const float left  = llclamp(llmin(right, limit) - width, 8.f, llmax(8.f, limit - width));
        const std::string value = llformat("%.1fdp", left);
        if (menu->GetAttribute<Rml::String>("data-placed", "") != value)
        {
            menu->SetAttribute("data-placed", value);
            menu->SetProperty("left", value);
            markChanged();
        }
    }
}

// While the grid menu is open, the frame shows the grid under the pointer.
void previewGrid()
{
    View& v = gView;
    int preview = -1;
    if (v.popup == "grid" && v.context)
        for (Rml::Element* e = v.context->GetHoverElement(); e; e = e->GetParentNode())
            if (e->GetId().rfind("gridopt-", 0) == 0)
            {
                preview = std::atoi(e->GetId().c_str() + 8);
                break;
            }
    if (preview >= (int)SLCameraExposure::GRID_COUNT)
        preview = -1;
    if (preview != v.gridPreview)
    {
        v.gridPreview = preview; // updateFinder sees the change (the grid is part of its state)
        markChanged();
    }
}

void setBox(const char* id, float x, float w)
{
    if (Rml::Element* e = gView.document->GetElementById(id))
    {
        e->SetProperty("left", llformat("%.2fdp", x));
        e->SetProperty("width", llformat("%.2fdp", w));
    }
}

// The bar from slcamerabarlayout.h, in dp (= UI units: the document's dp ratio is the UI scale).
void layout()
{
    View& v = gView;
    const slcambar::Bar b = slcambar::layoutBar(v.W, 1.f);
    setBox("bar", b.left, b.width);
    // Children are placed inside the bar's 1 dp border: their x is from the bar's padding edge.
    const float in = b.left + 1.f;
    setBox("leave", b.leave.x - in, b.leave.w);
    setBox("saveto", b.saveTo.x - in, b.saveTo.w);
    setBox("shutter", b.shutter.x - in, b.shutter.w);
    setBox("size", b.size.x - in, b.size.w);
    setBox("controls", b.controls.x - in, b.controls.w);
    // The popovers stand over their buttons, inside the window.
    const float W = v.W;
    setBox("savepop", llclamp(b.saveTo.x + b.saveTo.w * 0.5f - 160.f, 8.f, llmax(8.f, W - 328.f)), 320.f);
    setBox("sizepop", llclamp(b.size.x + b.size.w * 0.5f - 165.f, 8.f, llmax(8.f, W - 338.f)), 330.f);
    v.fitPending = true;
    markChanged();
}

// Labels that do not fit shrink one type step before they truncate (7.2); the widths never change.
void fitLabels()
{
    View& v = gView;
    if (!v.fitPending || v.density <= 0.f)
        return;
    const slcambar::Bar b = slcambar::layoutBar(v.W, 1.f);
    const std::pair<const char*, float> boxes[] = { { "leave", b.leave.w }, { "saveto", b.saveTo.w }, { "size", b.size.w },
                                                    { "controls", b.controls.w } };
    const std::string texts[] = { v.leaveLabel, v.saveLabel, v.sizeLabel, v.controlsLabel };
    for (size_t i = 0; i < 4; ++i)
    {
        Rml::Element* button = v.document->GetElementById(boxes[i].first);
        Rml::Element* label  = button ? button->GetFirstChild() : nullptr;
        if (!label)
            continue;
        const int px = Rml::ElementUtilities::GetStringWidth(label, texts[i]);
        if (px <= 0)
            return; // fonts not laid out yet: next frame
        // Measured at the current size: back to the bar's size (title) when it is shrunk now.
        const float title = (float)px / (v.density * v.k) * (button->IsClassSet("small") ? slcambar::kTitle / slcambar::kBody : 1.f);
        // Room for the padding, an icon and a chevron.
        const int   step  = slcambar::labelStep(title, boxes[i].second - 60.f, slcambar::kTitle, slcambar::kBody);
        button->SetClass("small", step >= 1);
    }
    v.fitPending = false;
    markChanged();
}

// Pointer to RmlUi (only real moves); true when it is over one of the document's elements.
bool toRml(S32 x, S32 y, MASK mask)
{
    View& v = gView;
    if (!v.shown || !v.context || v.density <= 0.f)
        return false;
    if (x != v.lastX || y != v.lastY)
    {
        v.lastX = x;
        v.lastY = y;
        v.context->ProcessMouseMove(int((float)x * v.density), int((float)(v.rootH - y) * v.density), rmlModifiers(mask));
    }
    Rml::Element* hover = v.context->GetHoverElement();
    if (hover != v.lastHover)
    {
        v.lastHover = hover;
        markChanged(); // :hover styles
    }
    return hover && hover != v.document && hover->GetOwnerDocument() == v.document;
}

// The kit's theme (UIKitTheme: 0 dark, 1 light). Dark uses the glass tokens (0.92 alpha
// surfaces, no blur): the camera looks through its bar.
Rml::ElementDocument* loadDocument(Rml::Context* context)
{
    View& v = gView;
    v.theme = gSavedSettings.getU32("UIKitTheme") == 1 ? 1u : 0u;
    // Every text of this document sits on a surface (>= 0.92 alpha): no outline (the kit's rule).
    const LLNewUIKit::Tokens tokens = LLNewUIKit::tokens(v.theme == 1 ? LLNewUIKit::Theme::Light : LLNewUIKit::Theme::Dark, true);
    return context->LoadDocumentFromMemory(LLNewUIKit::themed(kCameraRml, tokens), "slcamera.rml");
}

// A theme switch: the document again under the new tokens; the data model and every state stay.
void retheme()
{
    View& v = gView;
    Rml::ElementDocument* fresh = loadDocument(v.context);
    if (!fresh)
        return; // keep the old look rather than lose the overlay
    v.document->Close();
    v.document = fresh;
    v.document->Show(Rml::ModalFlag::None, Rml::FocusFlag::None);
    v.rootW = v.rootH = 0; // lay out again (positions are set from C++)
    v.finder    = Finder();
    v.lastHover = nullptr;
    v.lastX = v.lastY = S32_MIN;
    if (v.model)
        v.model.DirtyAllVariables();
    markChanged();
}

// <Prestige> isolate your subject (prestigecameraisolate.h): its chip, menu and picking banner,
// from the model; dirty only what changed.
void refreshIsolate()
{
    View& v = gView;
    namespace PCI = PrestigeCameraIsolate;
    const int mode = (int)PCI::mode(), count = (int)PCI::pickedCount();
    set(v.isoMode, mode, "isoMode");
    set(v.isoCount, count, "isoCount");
    set(v.isoPicking, PCI::picking(), "isoPicking");
    LLStringUtil::format_map_t args;
    args["[COUNT]"] = llformat("%d", count);
    args["[KEY]"]   = key("camera_exit");
    static const char* const chips[] = { "CameraModeIsolateChipOff", "CameraModeIsolateChipMine", "CameraModeIsolateChipPicked" };
    set(v.isoChip, LLTrans::getString(chips[llclamp(mode, 0, 2)], args), "isoChip");
    set(v.isoPickLabel, str(v.isoPicking ? "IsolateStopPicking" : "IsolatePick"), "isoPickLabel");
    set(v.isoPickText, LLTrans::getString("CameraModeIsolatePickBanner", args), "isoPickText");
    std::vector<Choice> modes;
    for (int i = 0; i < (int)PCI::MODE_COUNT; ++i)
        modes.push_back({ str(llformat("IsolateMode%d", i).c_str()), str(llformat("IsolateNote%d", i).c_str()), i == mode, std::string() });
    bool same = modes.size() == v.isoModes.size();
    for (size_t i = 0; same && i < modes.size(); ++i)
        same = modes[i].label == v.isoModes[i].label && modes[i].note == v.isoModes[i].note && modes[i].on == v.isoModes[i].on;
    if (!same)
    {
        v.isoModes = std::move(modes);
        dirty("isoModes");
    }
}

// The picked subjects' marks: a ring just over each one's head (none with Isolate off or on
// "My avatar"). Re-placed only when one moved, the window or the document changed.
void updateIsolateMarks()
{
    View& v = gView;
    constexpr int kMarks = 8;   // (the document has iso-m0 .. iso-m7)
    constexpr float kRing = 22.f;
    std::vector<std::pair<S32, S32>> points;
    PrestigeCameraIsolate::markPoints(points);
    std::string signature = llformat("%p|%.3f|%d|", (void*)v.document, v.k, v.rootH);
    for (const auto& [x, y] : points)
        signature += llformat("%d,%d;", x, y);
    if (signature == v.isoMarkSignature)
        return;
    v.isoMarkSignature = signature;
    for (int i = 0; i < kMarks; ++i)
    {
        const std::string id = llformat("iso-m%d", i);
        if (i < (int)points.size())
            place(id.c_str(), (float)points[(size_t)i].first / v.k - kRing * 0.5f, v.H - (float)points[(size_t)i].second / v.k - kRing * 0.5f,
                  kRing, kRing);
        else
            hide(id.c_str());
    }
    markChanged();
}
// </Prestige>

bool createDocument(Rml::Context* context)
{
    View& v = gView;
    v.context = context;
    // The kit's faces: the new interface (UIRenderer 2) loaded them into this context already.
    if (!LLNewUI::instance().active())
        LLNewUIKit::loadFonts();
    try
    {
        Rml::DataModelConstructor m = context->CreateDataModel("camera");
        if (!m)
            return false;
        if (auto s = m.RegisterStruct<KeyText>())
        {
            s.RegisterMember("key", &KeyText::key);
            s.RegisterMember("text", &KeyText::text);
            s.RegisterMember("end", &KeyText::end);
            s.RegisterMember("icon", &KeyText::icon);
        }
        m.RegisterArray<std::vector<KeyText>>();
        if (auto c = m.RegisterStruct<Choice>())
        {
            c.RegisterMember("label", &Choice::label);
            c.RegisterMember("note", &Choice::note);
            c.RegisterMember("on", &Choice::on);
            c.RegisterMember("id", &Choice::id);
        }
        m.RegisterArray<std::vector<Choice>>();
        if (auto c = m.RegisterStruct<StripCell>())
        {
            c.RegisterMember("id", &StripCell::id);
            c.RegisterMember("value", &StripCell::value);
            c.RegisterMember("unit", &StripCell::unit);
            c.RegisterMember("name", &StripCell::name);
            c.RegisterMember("off", &StripCell::off);
            c.RegisterMember("marked", &StripCell::marked);
        }
        m.RegisterArray<std::vector<StripCell>>();
        m.Bind("cells", &v.cells);
        m.Bind("grids", &v.grids);
        m.Bind("gridTitle", &v.gridTitle);
        m.Bind("levelLabel", &v.levelLabel);
        m.Bind("studioLabel", &v.studioLabel);
        m.Bind("levelOn", &v.levelOn);
        m.Bind("studioOn", &v.studioOn);
        m.Bind("flashOn", &v.flashOn);
        // <Prestige> AE-L
        m.Bind("aeLocked", &v.aeLocked);
        m.Bind("aelLabel", &v.aelLabel);
        // </Prestige>
        // A cell's arrows: one step either way (the cell's click keeps its own action). The
        // values that switch an effect or reset on a click step their value; the others step
        // their named stops (modes, lenses, focus and drive modes).
        m.BindEventCallback("cellstep", [](Rml::DataModelHandle, Rml::Event& event, const Rml::VariantList& args)
                            {
                                event.StopPropagation(); // not the cell's own click
                                const int i = args.size() >= 2 ? args[0].Get<int>() : -1;
                                if (i < 0 || i >= (int)kCellCount)
                                    return;
                                const SLCameraModel::Value value = kCells[i];
                                const int dir = args[1].Get<int>();
                                switch (value)
                                {
                                    case SLCameraModel::Value::Mode:
                                    case SLCameraModel::Value::Focal:
                                    case SLCameraModel::Value::Focus:
                                    case SLCameraModel::Value::Drive: SLCameraModel::step(value, dir); break;
                                    default: SLCameraModel::turnValue(value, dir); break;
                                }
                                refreshControls();
                            });
        m.BindEventCallback("qstep", [](Rml::DataModelHandle, Rml::Event& event, const Rml::VariantList& args)
                            {
                                event.StopPropagation();
                                if (args.size() < 2)
                                    return;
                                gView.qSel = args[0].Get<int>();
                                SLCameraModel::qTurn(gView.qSel, args[1].Get<int>());
                                refreshQuick();
                            });
        m.BindEventCallback("cell", [](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList& args)
                            {
                                const int i = args.empty() ? -1 : args[0].Get<int>();
                                if (i < 0 || i >= (int)kCellCount)
                                    return;
                                const SLCameraModel::Value value = kCells[i];
                                // Shutter, aperture and white balance: their effect on / off.
                                if (value == SLCameraModel::Value::Shutter || value == SLCameraModel::Value::Aperture
                                    || value == SLCameraModel::Value::WB)
                                    SLCameraModel::toggle(value);
                                else if (value == SLCameraModel::Value::EV || value == SLCameraModel::Value::ISO)
                                    SLCameraModel::reset(value); // back to 0 EV / ISO 100, as the old dials did
                                else
                                    SLCameraModel::step(value, 1);
                                refreshControls();
                            });
        m.BindEventCallback("grid", [](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList& args)
                            {
                                if (!args.empty())
                                    SLCameraModel::setGrid((U32)args[0].Get<int>());
                                openPopup(std::string());
                                refreshControls();
                            });
        m.BindEventCallback("level", [](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&)
                            {
                                SLCameraModel::setLevel(!SLCameraModel::level());
                                refreshControls();
                            });
        m.BindEventCallback("flash", [](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&)
                            {
                                SLCameraModel::setFlashOn(!SLCameraModel::flashOn());
                                refreshControls();
                            });
        // <Prestige> AE-L
        m.BindEventCallback("ael", [](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&)
                            {
                                SLCameraModel::toggleExposureLock();
                                refreshControls();
                            });
        // </Prestige>
        m.BindEventCallback("studio", [](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&)
                            {
                                SLCameraRail::setOpen(!SLCameraRail::open());
                                refreshControls();
                            });
        if (auto t = m.RegisterStruct<Thumb>())
        {
            t.RegisterMember("src", &Thumb::src);
            t.RegisterMember("index", &Thumb::index);
            t.RegisterMember("sel", &Thumb::sel);
        }
        m.RegisterArray<std::vector<Thumb>>();
        if (auto b = m.RegisterStruct<Bar>())
            b.RegisterMember("h", &Bar::h);
        m.RegisterArray<std::vector<Bar>>();
        m.RegisterArray<std::vector<std::string>>();
        if (auto r = m.RegisterStruct<RailRow>())
        {
            r.RegisterMember("id", &RailRow::id);
            r.RegisterMember("track", &RailRow::track);
            r.RegisterMember("label", &RailRow::label);
            r.RegisterMember("value", &RailRow::value);
            r.RegisterMember("w", &RailRow::w);
            r.RegisterMember("kind", &RailRow::kind);
            r.RegisterMember("on", &RailRow::on);
            r.RegisterMember("enabled", &RailRow::enabled);
            r.RegisterMember("changed", &RailRow::changed);
            r.RegisterMember("buttons", &RailRow::buttons);
        }
        m.RegisterArray<std::vector<RailRow>>();
        if (auto sc = m.RegisterStruct<RailSec>())
        {
            sc.RegisterMember("title", &RailSec::title);
            sc.RegisterMember("open", &RailSec::open);
            sc.RegisterMember("setups", &RailSec::setups);
            sc.RegisterMember("rows", &RailSec::rows);
        }
        m.RegisterArray<std::vector<RailSec>>();
        m.Bind("presets", &v.presets);
        m.Bind("presetNone", &v.presetNone);
        m.Bind("presetSaveLabel", &v.presetSaveLabel);
        m.Bind("presetName", &v.presetName);
        m.Bind("deleteAsk", &v.deleteAsk);
        m.Bind("setupsTitle", &v.setupsTitle);
        m.Bind("tip", &v.tip);
        m.Bind("statuses", &v.statuses);
        m.Bind("lockMark", &v.lockMark);
        m.Bind("tipWhat", &v.tipWhat);
        m.Bind("tipLow", &v.tipLow);
        m.Bind("tipHigh", &v.tipHigh);
        m.Bind("tipClick", &v.tipClick);
        m.Bind("clickWord", &v.clickWord);
        m.Bind("drive", &v.drive);
        m.Bind("ims", &v.ims);
        m.BindEventCallback("presetload", [](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList& args)
                            {
                                const int i = args.empty() ? -1 : args[0].Get<int>();
                                if (i < 0 || i >= (int)gView.presets.size())
                                    return;
                                const std::string name = gView.presets[(size_t)i].label;
                                if (SLCameraPresets::load(name))
                                    SLCameraModel::toast(presetMessage("PresetLoaded", name));
                            });
        m.BindEventCallback("presetdel", [](Rml::DataModelHandle, Rml::Event& event, const Rml::VariantList& args)
                            {
                                event.StopPropagation(); // not a load of the row it sits on
                                const int i = args.empty() ? -1 : args[0].Get<int>();
                                if (i < 0 || i >= (int)gView.presets.size())
                                    return;
                                // The first click asks, the second deletes (a preset is a file).
                                const std::string name = gView.presets[(size_t)i].label;
                                if (gView.presetArmed == name)
                                {
                                    SLCameraPresets::remove(name);
                                    gView.presetArmed.clear();
                                }
                                else
                                    gView.presetArmed = name;
                                refreshPresets(false);
                            });
        m.BindEventCallback("presetsave", [](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&)
                            {
                                std::string name = gView.presetName;
                                LLStringUtil::trim(name);
                                if (name.empty())
                                    SLCameraModel::toast(str("PresetTypeName"));
                                else
                                    SLCameraModel::toast(presetMessage(SLCameraPresets::save(name) ? "PresetSaved" : "PresetNotSaved", name));
                                if (Rml::Element* focus = gView.context->GetFocusElement())
                                    focus->Blur();
                                refreshPresets(true);
                            });
        // <Prestige> setting up the shot: the chips, the light, renaming a setup, where the camera stands
        m.Bind("shotHuds", &gShot.huds);
        m.Bind("shotPoser", &gShot.poser);
        m.Bind("shotFrozen", &gShot.frozen);
        m.Bind("shotQuiet", &gShot.quiet);
        m.Bind("shotSkyPhoto", &gShot.skyPhoto);
        m.Bind("shotLight", &gShot.light);
        m.Bind("shotHudsLabel", &gShot.hudsLabel);
        m.Bind("shotPoserLabel", &gShot.poserLabel);
        m.Bind("shotFreezeLabel", &gShot.freezeLabel);
        m.Bind("shotQuietLabel", &gShot.quietLabel);
        m.Bind("shotSkyLabel", &gShot.skyLabel);
        m.Bind("shotLightLabel", &gShot.lightLabel);
        m.Bind("anchors", &gShot.anchors);
        m.Bind("anchorTitle", &gShot.anchorTitle);
        m.Bind("anchorNote", &gShot.anchorNote);
        m.Bind("presetRenaming", &gShot.renaming);
        m.Bind("presetRenameLabel", &gShot.renameLabel);
        m.BindEventCallback("shothuds", [](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&)
                            {
                                PrestigeCameraShot::setShowHUDs(!PrestigeCameraShot::showHUDs());
                                refreshShot();
                            });
        m.BindEventCallback("shotposer", [](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&)
                            {
                                PrestigeCameraShot::toggleTool(PrestigeCameraShot::Tool::Poser);
                                refreshShot();
                            });
        m.BindEventCallback("shotlight", [](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&)
                            {
                                PrestigeCameraShot::toggleTool(PrestigeCameraShot::Tool::Light);
                                refreshShot();
                            });
        m.BindEventCallback("shotfreeze", [](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&)
                            {
                                PrestigeCameraShot::setWorldFrozen(!PrestigeCameraShot::worldFrozen());
                                refreshShot();
                            });
        m.BindEventCallback("shotquiet", [](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&)
                            {
                                PrestigeCameraShot::setQuiet(!PrestigeCameraShot::quiet());
                                refreshShot();
                            });
        m.BindEventCallback("shotsky", [](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&)
                            {
                                PrestigeCameraShot::setEnvForPhotoOnly(!PrestigeCameraShot::envForPhotoOnly());
                                refreshShot();
                            });
        m.BindEventCallback("shotanchor", [](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList& args)
                            {
                                if (!args.empty())
                                    PrestigeCameraShot::setAnchor(args[0].Get<int>());
                                refreshShot();
                            });
        m.BindEventCallback("presetren", [](Rml::DataModelHandle, Rml::Event& event, const Rml::VariantList& args)
                            {
                                event.StopPropagation(); // not a load of the row it sits on
                                const int i = args.empty() ? -1 : args[0].Get<int>();
                                if (i < 0 || i >= (int)gView.presets.size())
                                    return;
                                // The name goes into the field, Save becomes Rename; again on it: never mind.
                                const std::string name = gView.presets[(size_t)i].label;
                                if (gShot.renaming == name)
                                {
                                    set(gShot.renaming, std::string(), "presetRenaming");
                                    refreshPresets(true);
                                }
                                else
                                {
                                    set(gShot.renaming, name, "presetRenaming");
                                    set(gView.presetName, name, "presetName");
                                }
                            });
        m.BindEventCallback("presetrengo", [](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&)
                            {
                                std::string name = gView.presetName;
                                LLStringUtil::trim(name);
                                if (name.empty())
                                    SLCameraModel::toast(str("PresetTypeName"));
                                else
                                    SLCameraModel::toast(presetMessage(SLCameraPresets::rename(gShot.renaming, name) ? "PresetRenamed"
                                                                                                                  : "PresetNotRenamed",
                                                                       name));
                                set(gShot.renaming, std::string(), "presetRenaming");
                                if (Rml::Element* focus = gView.context->GetFocusElement())
                                    focus->Blur();
                                refreshPresets(true);
                            });
        // </Prestige>
        m.Bind("rail", &v.rail);
        m.Bind("railRight", &v.railRight);
        m.Bind("railShort", &v.railShort);
        m.Bind("openStudioLabel", &v.openStudioLabel);
        m.BindEventCallback("railsec", [](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList& args)
                            {
                                if (!args.empty())
                                    SLCameraRail::toggleSection((size_t)args[0].Get<int>());
                                refreshRail();
                            });
        m.BindEventCallback("railturn", [](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList& args)
                            {
                                int sec = -1, row = -1;
                                if (args.size() >= 2 && railIds(args[0].Get<Rml::String>(), sec, row))
                                    SLCameraRail::turn((size_t)sec, (size_t)row, args[1].Get<int>());
                                refreshRail();
                            });
        m.BindEventCallback("railreset", [](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList& args)
                            {
                                int sec = -1, row = -1;
                                if (!args.empty() && railIds(args[0].Get<Rml::String>(), sec, row))
                                    SLCameraRail::resetRow((size_t)sec, (size_t)row);
                                refreshRail();
                            });
        m.BindEventCallback("railpress", [](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList& args)
                            {
                                int sec = -1, row = -1;
                                if (args.size() >= 2 && railIds(args[0].Get<Rml::String>(), sec, row))
                                    SLCameraRail::press((size_t)sec, (size_t)row, (size_t)args[1].Get<int>());
                                refreshRail();
                            });
        m.BindEventCallback("railside", [](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&)
                            {
                                SLCameraRail::setDockedRight(!SLCameraRail::dockedRight());
                                refreshRail();
                            });
        m.BindEventCallback("openstudio", [](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&)
                            { SLCameraRail::openPhotoStudio(); });
        m.Bind("thumbs", &v.thumbs);
        m.Bind("reviewKey", &v.reviewKey);
        m.Bind("dest", &v.dest);
        m.Bind("sendDest", &v.sendDest);
        m.Bind("sendOpen", &v.sendOpen);
        m.Bind("savedDisk", &v.savedDisk);
        m.Bind("sendLocation", &v.sendLocation);
        m.Bind("sendDontAsk", &v.sendDontAsk);
        for (auto [name, field] : { std::make_pair("destDisk", &View::destDisk), std::make_pair("destInventory", &View::destInventory),
                                    std::make_pair("destProfile", &View::destProfile), std::make_pair("destEmail", &View::destEmail),
                                    std::make_pair("inventoryNote", &View::inventoryNote), std::make_pair("profileNote", &View::profileNote),
                                    std::make_pair("emailNote", &View::emailNote), std::make_pair("sendTitle", &View::sendTitle),
                                    std::make_pair("sendText", &View::sendText), std::make_pair("sendError", &View::sendError),
                                    std::make_pair("sendCaption", &View::sendCaption), std::make_pair("sendTo", &View::sendTo),
                                    std::make_pair("sendSubject", &View::sendSubject), std::make_pair("sendMessage", &View::sendMessage),
                                    std::make_pair("sendGoLabel", &View::sendGoLabel), std::make_pair("captionLabel", &View::captionLabel),
                                    std::make_pair("locationLabel", &View::locationLabel), std::make_pair("toLabel", &View::toLabel),
                                    std::make_pair("subjectLabel", &View::subjectLabel), std::make_pair("messageLabel", &View::messageLabel),
                                    std::make_pair("dontAskLabel", &View::dontAskLabel), std::make_pair("cancelLabel", &View::cancelLabel),
                                    std::make_pair("histDark", &View::histDark), std::make_pair("histBright", &View::histBright) })
            m.Bind(name, &(v.*field));
        m.BindEventCallback("dest", [](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList& args)
                            {
                                if (!args.empty())
                                    SLCameraModel::setDestination((U32)args[0].Get<int>());
                                refreshDestination();
                                openPopup(std::string());
                            });
        m.BindEventCallback("sendloc", [](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&)
                            { set(gView.sendLocation, !gView.sendLocation, "sendLocation"); });
        m.BindEventCallback("sendask", [](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&)
                            { set(gView.sendDontAsk, !gView.sendDontAsk, "sendDontAsk"); });
        m.BindEventCallback("sendcancel", [](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) { closeSendCard(); });
        m.BindEventCallback("sendgo", [](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) { sendGo(); });
        m.Bind("narrow", &v.narrow);
        m.Bind("allShotsLabel", &v.allShotsLabel);
        m.Bind("bars", &v.bars);
        m.Bind("savedShown", &v.savedShown);
        m.Bind("cardOpen", &v.cardOpen);
        m.Bind("cardMore", &v.cardMore);
        for (auto [name, field] : { std::make_pair("moreShots", &View::moreShots), std::make_pair("cardSrc", &View::cardSrc),
                                    std::make_pair("cardH", &View::cardH), std::make_pair("cardTitle", &View::cardTitle),
                                    std::make_pair("cardMeta", &View::cardMeta), std::make_pair("cardLabel", &View::cardLabel),
                                    std::make_pair("savedLabel", &View::savedLabel), std::make_pair("folderLabel", &View::folderLabel),
                                    std::make_pair("deleteLabel", &View::deleteLabel) })
            m.Bind(name, &(v.*field));
        m.BindEventCallback("thumb", [](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList& args)
                            {
                                if (!args.empty())
                                    fillCard(args[0].Get<int>());
                                refreshReview();
                            });
        m.BindEventCallback("folder", [](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&)
                            {
                                const int i = gView.cardOpen ? shotIndex(gView.cardSerial) : (int)SLCameraReview::count() - 1;
                                fileThere(i);
                                if (gViewerWindow)
                                    gViewerWindow->openSnapshotFolder();
                            });
        m.BindEventCallback("cardmore", [](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&)
                            { set(gView.cardMore, !gView.cardMore, "cardMore"); });
        m.BindEventCallback("del", [](Rml::DataModelHandle, Rml::Event& ev, const Rml::VariantList&)
                            {
                                // Delete hides with its menu: let go of it, so Space never deletes the next shot.
                                if (Rml::Element* e = ev.GetCurrentElement())
                                    e->Blur();
                                const int i = shotIndex(gView.cardSerial);
                                if (i < 0)
                                    return;
                                if (!SLCameraReview::deleteFile((size_t)i))
                                    SLCameraModel::toast(LLTrans::getString("CameraModeFileGone"));
                                SLCameraReview::remove((size_t)i);
                                refreshReview();
                            });
        m.Bind("barTop", &v.barTop);
        m.Bind("barAway", &v.barAway);
        m.Bind("noticeShown", &v.noticeShown);
        m.Bind("readoutShown", &v.readoutShown);
        m.Bind("noticeTitle", &v.noticeTitle);
        m.Bind("noticeText", &v.noticeText);
        m.Bind("noticeEnd", &v.noticeEnd);
        m.Bind("toggleKey", &v.toggleKey);
        m.Bind("readoutName", &v.readoutName);
        m.Bind("readoutValue", &v.readoutValue);
        m.Bind("readoutHint", &v.readoutHint);
        if (auto tk = m.RegisterStruct<Tick>())
        {
            tk.RegisterMember("on", &Tick::on);
            tk.RegisterMember("major", &Tick::major);
        }
        m.RegisterArray<std::vector<Tick>>();
        m.Bind("readoutTicks", &v.readoutTicks);
        m.Bind("qcells", &v.qcells);
        m.Bind("qhints", &v.qhints);
        m.BindEventCallback("qnext", [](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList& args)
                            {
                                if (args.empty())
                                    return;
                                gView.qSel = args[0].Get<int>();
                                SLCameraModel::qTurn(gView.qSel, 1);
                                refreshQuick();
                            });
        m.BindEventCallback("qmenu", [](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&)
                            {
                                // From the strip's more cell: above it, right-aligned to it.
                                View& v = gView;
                                if (v.popup == "quick")
                                {
                                    openPopup(std::string());
                                    return;
                                }
                                Rml::Element* more = v.document->GetElementById("cellmore");
                                const float px = v.density * v.k;
                                if (!more || px <= 0.f)
                                {
                                    SLCameraView::toggleQMenu();
                                    return;
                                }
                                const float right = (more->GetAbsoluteLeft() + more->GetOffsetWidth()) / px;
                                const float top   = more->GetAbsoluteTop() / px;
                                showQuick(right - 480.f, v.barTop ? top + more->GetOffsetHeight() / px + 16.f : top - 150.f - 16.f);
                            });
        m.Bind("popup", &v.popup);
        m.Bind("diskNote", &v.diskNote);
        m.Bind("classicLabel", &v.classicLabel);
        m.Bind("aspectTitle", &v.aspectTitle);
        m.Bind("sizeTitle", &v.sizeTitle);
        m.Bind("typeTitle", &v.typeTitle);
        m.Bind("qualityTitle", &v.qualityTitle);
        m.Bind("postTitle", &v.postTitle);
        m.Bind("aspects", &v.aspects);
        m.Bind("sizes", &v.sizes);
        m.Bind("formats", &v.formats);
        m.Bind("customOn", &v.customOn);
        m.Bind("jpeg", &v.jpeg);
        m.Bind("post", &v.post);
        // The fields and the slider write through the model as they change.
        m.BindFunc("customW", [](Rml::Variant& out) { out = gView.customW; },
                   [](const Rml::Variant& in)
                   {
                       gView.customW = in.Get<int>();
                       if (gView.customW > 0 && gView.customH > 0)
                           SLCameraModel::setOutputBase(gView.customW, gView.customH);
                   });
        m.BindFunc("customH", [](Rml::Variant& out) { out = gView.customH; },
                   [](const Rml::Variant& in)
                   {
                       gView.customH = in.Get<int>();
                       if (gView.customW > 0 && gView.customH > 0)
                           SLCameraModel::setOutputBase(gView.customW, gView.customH);
                   });
        m.BindFunc("quality", [](Rml::Variant& out) { out = gView.quality; },
                   [](const Rml::Variant& in)
                   {
                       gView.quality = in.Get<int>();
                       SLCameraModel::setJpegQuality(gView.quality);
                   });
        m.BindEventCallback("pop", [](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList& args)
                            {
                                const std::string which = args.empty() ? std::string() : args[0].Get<Rml::String>();
                                openPopup(gView.popup == which ? std::string() : which);
                            });
        m.BindEventCallback("classic", [](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&)
                            { SLCameraMode::openClassic(); });
        m.BindEventCallback("aspect", [](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList& args)
                            {
                                if (!args.empty())
                                    SLCameraModel::setAspect((U32)args[0].Get<int>());
                                refreshSizePopover();
                            });
        m.BindEventCallback("pickSize", [](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList& args)
                            {
                                const int i = args.empty() ? -1 : args[0].Get<int>();
                                gView.customChosen = false;
                                if (i == 0)
                                    SLCameraModel::setOutputFollowsWindow();
                                else if (i >= 1 && i <= 3)
                                    SLCameraModel::setOutputBase(kSizes[i - 1][0], kSizes[i - 1][1]);
                                else if (i == 4)
                                    gView.customChosen = true; // the fields show; the size stays until typed
                                refreshSizePopover();
                                if (i == 4)
                                    set(gView.customOn, true, "customOn");
                            });
        m.BindEventCallback("format", [](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList& args)
                            {
                                if (!args.empty())
                                    SLCameraModel::setFileFormat((U32)args[0].Get<int>());
                                refreshSizePopover();
                            });
        m.BindEventCallback("post", [](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&)
                            {
                                SLCameraModel::setPostEffects(!SLCameraModel::postEffects());
                                refreshSizePopover();
                            });
        m.Bind("leaveLabel", &v.leaveLabel);
        m.Bind("saveLabel", &v.saveLabel);
        m.Bind("sizeLabel", &v.sizeLabel);
        m.Bind("controlsLabel", &v.controlsLabel);
        m.Bind("toast", &v.toast);
        m.Bind("helpTitle", &v.helpTitle);
        m.Bind("helpFooter", &v.helpFooter);
        m.Bind("changeKeysLabel", &v.changeKeysLabel);
        m.BindEventCallback("changekeys", [](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&)
                            {
                                // Preferences are part of the interface camera mode hides: leave first.
                                SLCameraMode::stop();
                                if (LLFloaterPreference* prefs = dynamic_cast<LLFloaterPreference*>(LLFloaterReg::showInstance("preferences")))
                                    prefs->selectPanel("controls");
                            });
        m.Bind("controlsOpen", &v.controlsOpen);
        m.Bind("hintShown", &v.hintShown);
        m.Bind("helpOpen", &v.helpOpen);
        m.Bind("shutterDown", &v.shutterDown);
        m.Bind("hints", &v.hints);
        m.Bind("helpMouse", &v.helpMouse);
        m.Bind("helpKeys", &v.helpKeys);
        m.BindEventCallback("leave", [](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) { SLCameraMode::stop(); });
        m.BindEventCallback("controls", [](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&)
                            { SLCameraView::setControlsOpen(!gView.controlsOpen); });
        m.BindEventCallback("help", [](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&)
                            { SLCameraView::showHelp(!gView.helpOpen); });
        // <Prestige> isolate your subject: the chip's menu (modes, picking) and its labels
        m.Bind("isoModes", &v.isoModes);
        m.Bind("isoMode", &v.isoMode);
        m.Bind("isoCount", &v.isoCount);
        m.Bind("isoPicking", &v.isoPicking);
        for (auto [name, field] : { std::make_pair("isoChip", &View::isoChip), std::make_pair("isoTitle", &View::isoTitle),
                                    std::make_pair("isoPickLabel", &View::isoPickLabel), std::make_pair("isoClearLabel", &View::isoClearLabel),
                                    std::make_pair("isoHelp", &View::isoHelp), std::make_pair("isoPickText", &View::isoPickText),
                                    std::make_pair("isoPngNote", &View::isoPngNote), std::make_pair("isoHelpLine", &View::isoHelpLine) })
            m.Bind(name, &(v.*field));
        m.BindEventCallback("isomode", [](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList& args)
                            {
                                if (args.empty())
                                    return;
                                const int mode = args[0].Get<int>();
                                PrestigeCameraIsolate::setMode(mode);
                                if (mode == PrestigeCameraIsolate::MODE_PICKED && PrestigeCameraIsolate::pickedCount() == 0)
                                {
                                    openPopup(std::string()); // nobody picked yet: straight to picking
                                    PrestigeCameraIsolate::setPicking(true);
                                }
                                refreshIsolate();
                            });
        m.BindEventCallback("isopick", [](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&)
                            {
                                const bool on = !PrestigeCameraIsolate::picking();
                                openPopup(std::string()); // the frame is clicked next
                                PrestigeCameraIsolate::setPicking(on);
                                refreshIsolate();
                            });
        m.BindEventCallback("isoclear", [](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&)
                            {
                                PrestigeCameraIsolate::clearPicks();
                                refreshIsolate();
                            });
        // </Prestige>
        v.model = m.GetModelHandle();
    }
    catch (const std::bad_alloc&)
    {
        context->RemoveDataModel("camera");
        return false;
    }
    v.document = loadDocument(context);
    if (!v.document)
    {
        context->RemoveDataModel("camera");
        v.model = Rml::DataModelHandle();
        return false;
    }
    return true;
}
} // namespace

bool SLCameraView::start(Rml::Context* context)
{
    if (!context)
        return false;
    if (gView.context != context)
        release();
    if (!gView.document && !createDocument(context))
    {
        gView = View();
        return false;
    }
    View& v = gView;
    v.leaveLabel    = str("Leave");
    v.controlsLabel = str("Controls");
    v.sizeLabel     = sizeText();
    v.controlsOpen  = gSavedSettings.getBOOL("SLCameraModeControlsOpen");
    v.helpOpen      = false;
    v.popup.clear();
    v.classicLabel  = str("OpenClassic");
    v.aspectTitle   = str("AspectTitle");
    v.sizeTitle     = str("SizeTitle");
    v.typeTitle     = str("TypeTitle");
    v.qualityTitle  = str("QualityTitle");
    v.postTitle     = str("PostEffects");
    v.levelLabel    = str("LevelChip");
    v.barHidden     = gSavedSettings.getBOOL("SLCameraModeBarHidden");
    v.barTop        = gSavedSettings.getBOOL("SLCameraModeBarTop");
    v.barAway       = v.barHidden;
    v.peeking = v.noticeShown = v.readoutShown = v.handleDown = false;
    v.hiddenAt      = -100.0;
    v.readoutAt     = -100.0;
    v.noticeTitle   = str("BarHidden");
    v.noticeText    = str(v.barTop ? "BarBackTop" : "BarBackBottom");
    v.noticeEnd     = str("BarBackEnd");
    v.toggleKey     = key("camera_toggle_bar");
    v.readoutHint   = str("ReadoutHint");
    v.savedLabel    = str("SavedToDisk");
    v.folderLabel   = str("ShowInFolder");
    v.deleteLabel   = str("Delete");
    v.destDisk      = str("SaveToDisk");
    v.destInventory = str("SaveToInventory");
    v.destProfile   = str("ShareToProfile");
    v.destEmail     = str("SendByEmail");
    v.profileNote   = str("ProfileNote");
    v.emailNote     = str("EmailNote");
    v.captionLabel  = str("CaptionLabel");
    v.locationLabel = str("LocationLabel");
    v.toLabel       = str("ToLabel");
    v.subjectLabel  = str("SubjectLabel");
    v.messageLabel  = str("MessageLabel");
    v.dontAskLabel  = str("DontAskLabel");
    v.cancelLabel   = str("CancelLabel");
    v.histDark      = str("HistDark");
    v.histBright    = str("HistBright");
    v.sendOpen      = false;
    refreshDestination();
    v.reviewKey     = key("camera_review");
    v.allShotsLabel = str("AllShots");
    v.openStudioLabel = str("OpenPhotoStudio");
    v.setupsTitle     = str("SetupsTitle");
    v.presetNone      = str("PresetNone");
    v.presetSaveLabel = str("PresetSave");
    v.deleteAsk       = str("PresetDeleteAsk");

    // <Prestige> isolate your subject
    v.isoTitle        = str("IsolateTitle");
    v.isoClearLabel   = str("IsolateClear");
    v.isoHelp         = str("IsolateHelp");
    v.isoPngNote      = str("IsolatePngNote");
    v.isoHelpLine     = str("IsolateHelpLine");
    v.isoMarkSignature.clear();
    refreshIsolate();
    // </Prestige>
    v.clickWord       = str("TipClickWord");
    v.lockMark        = str("LockMark");
    // <Prestige> AE-L
    v.aelLabel        = str("AELabel");
    // </Prestige>
    v.tipCell         = -1;
    v.presetArmed.clear();
    refreshPresets(true);
    // <Prestige> setting up the shot
    gShot.renaming.clear();
    refreshShot();
    // </Prestige>
    v.tip.clear();
    v.drive.clear();
    v.ims.clear();
    v.nextImPoll = 0.0;
    v.railSignature.clear();
    v.railDragSec = v.railDragRow = -1;
    v.cardOpen = v.cardMore = v.savedShown = false;
    LLRmlUiAtlas::instance().setExternalSource("llext:camshot/", &shotTexture);
    v.studioLabel   = str("PhotoStudio");
    v.tilting       = false;
    v.finder        = Finder();
    refreshControls();
    v.shutterDown   = false;
    v.pressedInDoc  = false;
    v.hintSince     = now();
    v.hintShown     = true;
    v.rootW = v.rootH = 0; // lay out at the first sync
    v.lastX = v.lastY = S32_MIN;
    v.lastHover       = nullptr;
    buildHints();
    buildHelp();
    if (v.model)
        v.model.DirtyAllVariables();
    v.document->Show(Rml::ModalFlag::None, Rml::FocusFlag::None);
    v.shown   = true;
    v.changed = true;
    return true;
}

// A field being typed in asks the window for text input, so AltGr, dead keys and input methods
// type their characters (without it the window passes the bare key: AltGr+2 typed "2"), and gives
// it back when typing ends.
void setTextInput(bool on)
{
    if (on == gView.textInput)
        return;
    gView.textInput = on;
    if (gViewerWindow && gViewerWindow->getWindow())
        gViewerWindow->getWindow()->allowLanguageTextInput(nullptr, on);
}

void SLCameraView::stop()
{
    setTextInput(false);
    if (gView.document && gView.shown)
        gView.document->Hide();
    if (gView.shutterDown)
        SLCameraModel::halfPress(false);
    gView.shown       = false;
    gView.shutterDown = false;
}

void SLCameraView::release()
{
    LLRmlUiAtlas::instance().setExternalSource("llext:camshot/", {});
    gView = View();
}

float SLCameraView::dpScale()
{
    return gView.shown ? gView.k : 1.f;
}

bool SLCameraView::active()
{
    return gView.shown;
}

bool SLCameraView::sync(float density)
{
    View& v = gView;
    if (!v.shown)
        return false;
    setTextInput(textEditing());
    if (density != v.density)
    {
        v.density = density;
        markChanged();
    }
    static LLCachedControl<U32> s_theme(gSavedSettings, "UIKitTheme", 0);
    if ((s_theme == 1 ? 1u : 0u) != v.theme)
        retheme();
    if (LLView* root = LLUI::getInstance()->getRootView())
    {
        const LLRect& r = root->getRect();
        static LLCachedControl<F32> s_size(gSavedSettings, "SLCameraModeOverlaySize", 1.f);
        // The kit's dp (the UI size), times the overlay's own size setting (live).
        const float k = llclamp((F32)s_size, 0.75f, 2.5f);
        if (r.getWidth() != v.rootW || r.getHeight() != v.rootH || k != v.k)
        {
            v.rootW = r.getWidth();
            v.rootH = r.getHeight();
            v.k     = k;
            v.W     = (float)v.rootW / k;
            v.H     = (float)v.rootH / k;
            v.finder = Finder();
            set(v.narrow, v.W < 1500.f, "narrow");
            layout();
        }
    }
    fitLabels();
    // The pointer, for :hover (camera mode gets no hover events of its own), and a tilt drag.
    if (gViewerWindow)
    {
        const LLCoordGL mouse = gViewerWindow->getCurrentMouse();
        toRml(mouse.mX, mouse.mY, MASK_NONE);
        if (v.tilting)
        {
            const LLRect f = SLCameraModel::frame();
            SLCameraModel::setTilt(atan2f((F32)(mouse.mY - f.getCenterY()), (F32)llmax(mouse.mX - f.getCenterX(), 1)));
        }
    }
    const F64 t = now();
    // The hidden bar: resting the pointer on its edge shows it for as long as the pointer stays on
    // the bar (no timers); the notice says how to bring it back; the wheel's readout fades.
    if (v.barHidden && gViewerWindow)
    {
        const LLCoordGL mouse = gViewerWindow->getCurrentMouse();
        const S32 fromEdge = v.barTop ? v.rootH - mouse.mY : mouse.mY; // UI units from the bar's edge
        const bool atEdge  = fromEdge >= 0 && (float)fromEdge <= 8.f * v.k;
        const bool onBar   = fromEdge >= 0 && (float)fromEdge <= (28.f + 58.f + 8.f) * v.k;
        // A popover opened from the shown bar keeps it while open.
        v.peeking = atEdge || (v.peeking && (onBar || !v.popup.empty()));
    }
    else
        v.peeking = false;
    set(v.barAway, v.barHidden && !v.peeking, "barAway");
    set(v.noticeShown, v.barHidden && slcambar::hintVisible(t, v.hiddenAt), "noticeShown");
    set(v.readoutShown, v.barAway && t - v.readoutAt >= 0.0 && t - v.readoutAt < 1.5, "readoutShown");
    if (v.readoutShown)
    {
        const SLCameraModel::Cell c = SLCameraModel::cell(v.readoutValue_);
        set(v.readoutName, c.name, "readoutName");
        set(v.readoutValue, c.unit.empty() ? c.value : c.value + " " + c.unit, "readoutValue");
        // The dial: where the value sits on its scale.
        const F32 pos  = SLCameraModel::position(v.readoutValue_);
        const int tick = pos < 0.f ? -1 : ll_round(pos * (kTicks - 1));
        if (tick != v.readoutTick)
        {
            v.readoutTick = tick;
            v.readoutTicks.clear();
            if (tick >= 0)
                for (int i = 0; i < kTicks; ++i)
                    v.readoutTicks.push_back({ i == tick, i % 3 == 0 });
            dirty("readoutTicks");
        }
    }
    // The strip cell under the pointer explains itself, after the pointer has rested 0.3 s.
    {
        SLCameraModel::Value value = SLCameraModel::Value::Mode;
        int cell = -1;
        bool shutter = false; // the on-screen shutter explains itself too
        bool tiltTip = false; // and the tilt arc
        bool flashChip = false; // and so does the flash chip
        int  qcell   = -1;    // and so do the quick settings
        const char* railBase = nullptr; // and the rail's rows (and their reset arrow)
        std::string railWhy, railLabel; // a greyed row: why, under its name
        Rml::Element* railRowEl = nullptr;
        if (gViewerWindow)
        {
            const LLCoordGL mouse = gViewerWindow->getCurrentMouse();
            const bool over = toRml(mouse.mX, mouse.mY, MASK_NONE);
            if (over && v.controlsOpen && v.studioOn)
                for (Rml::Element* e = v.context->GetHoverElement(); e; e = e->GetParentNode())
                {
                    int sec = -1, row = -1;
                    if (e->GetId().rfind("rrow-", 0) == 0 && railIds(e->GetId(), sec, row) && sec < (int)v.rail.size()
                        && row < (int)v.rail[(size_t)sec].rows.size())
                    {
                        railRowEl = e;
                        bool onReset = false;
                        for (Rml::Element* h = v.context->GetHoverElement(); h && h != e; h = h->GetParentNode())
                            onReset |= h->IsClassSet("rreset");
                        const RailRow& rr = v.rail[(size_t)sec].rows[(size_t)row];
                        railBase = onReset ? "TipRailReset" : railTip(rr);
                        if (!onReset && !rr.why.empty())
                        {
                            railWhy   = rr.why;
                            railLabel = rr.label;
                        }
                        if (*railBase || !railWhy.empty())
                            cell = 300 + sec * 64 + row + (onReset ? 10000 : 0);
                        break;
                    }
                }
            if (cell < 0 && over && v.popup == "quick")
                for (Rml::Element* e = v.context->GetHoverElement(); e; e = e->GetParentNode())
                    if (e->GetId().rfind("qcell-", 0) == 0)
                    {
                        qcell = std::atoi(e->GetId().c_str() + 6);
                        cell  = 200 + qcell;
                        break;
                    }
            if (cell < 0 && v.controlsOpen && SLCameraView::overAt(mouse.mX, mouse.mY, &value) == slcamin::Over::Cell)
                cell = (int)value;
            else if (cell < 0 && over && within(v.context->GetHoverElement(), "shutter") && !v.shutterDown)
            {
                shutter = true;
                cell    = 100;
            }
            else if (cell < 0 && over && !v.tilting
                     && (within(v.context->GetHoverElement(), "tilthit") || within(v.context->GetHoverElement(), "tiltknob")))
            {
                tiltTip = true;
                cell    = 400;
            }
            else if (cell < 0 && over && v.controlsOpen && within(v.context->GetHoverElement(), "flashchip"))
            {
                flashChip = true;
                cell      = 150;
            }
        }
        if (cell != v.tipCell)
        {
            v.tipCell  = cell;
            v.tipSince = t;
        }
        static const char* const qtips[] = { "", "TipISO", "TipWB", "TipDrive", "TipFocus", "TipLook", "TipAspect", "TipGrid" };
        const char* base = "";
        if (cell >= 0 && t - v.tipSince >= 0.3)
        {
            if (railBase && *railBase)
                base = railBase;
            else if (shutter)
                base = "TipShutterButton";
            else if (tiltTip)
                base = "TipTilt";
            else if (flashChip)
                base = "TipFlash";
            else if (qcell == 0)
                base = tipFor(SLCameraModel::Value::Mode);
            else if (qcell > 0 && qcell < 8)
                base = qtips[qcell];
            else
                base = tipFor(value);
        }
        set(v.tip, tipPart(base, "Title"), "tip");
        set(v.tipWhat, tipPart(base, "What"), "tipWhat");
        set(v.tipLow, tipPart(base, "Low"), "tipLow");
        set(v.tipHigh, tipPart(base, "High"), "tipHigh");
        set(v.tipClick, tipPart(base, "Click"), "tipClick");
        // A greyed rail row says why it is greyed (god rays under volumetric fog, ...).
        if (!railWhy.empty() && cell >= 0 && t - v.tipSince >= 0.3)
        {
            set(v.tip, railLabel, "tip");
            set(v.tipWhat, railWhy, "tipWhat");
            set(v.tipLow, std::string(), "tipLow");
            set(v.tipHigh, std::string(), "tipHigh");
            set(v.tipClick, std::string(), "tipClick");
        }
        if (cell >= 0 && v.tip.empty())
            markChanged(); // keep capturing until the delay has run
        // Over the thing it is about: centred on it, kept inside the window.
        if (!v.tip.empty())
        {
            // Rail rows: beside the rail, level with the row. The tilt arc: to its left, level with the knob.
            Rml::Element* railEl = railRowEl ? v.document->GetElementById("rail") : tiltTip ? v.document->GetElementById("tiltband") : nullptr;
            Rml::Element* rowEl  = railRowEl ? railRowEl : tiltTip ? v.document->GetElementById("tiltknob") : nullptr;
            const bool    leftOf = railRowEl ? v.railRight : true;
            Rml::Element* wrapEl = v.document->GetElementById("tipwrap");
            if (railEl && rowEl && wrapEl && v.density > 0.f)
            {
                const float px    = v.density * v.k;
                const float railL = railEl->GetAbsoluteLeft() / px, railR = railL + railEl->GetOffsetWidth() / px;
                const float left  = leftOf ? railL - 428.f : railR + 8.f;
                const float top   = llclamp(rowEl->GetAbsoluteTop() / px - 8.f, 8.f, llmax(8.f, v.H - 200.f));
                const std::string placed = llformat("r%.1f|%.1f", left, top);
                if (wrapEl->GetAttribute<Rml::String>("data-placed", "") != placed)
                {
                    wrapEl->SetAttribute("data-placed", placed);
                    wrapEl->SetProperty("left", llformat("%.1fdp", left));
                    wrapEl->SetProperty("top", llformat("%.1fdp", top));
                    wrapEl->SetProperty("bottom", "auto");
                    markChanged();
                }
            }
            else
            {
            // The anchor (what it explains) and the box it clears: the strip, the bar, or the whole
            // quick settings grid (two rows: the tip never covers the other one).
            std::string anchorId = "shutter", boxId = "bar";
            if (qcell >= 0)
            {
                anchorId = llformat("qcell-%d", qcell);
                boxId    = "quick";
            }
            else if (flashChip)
            {
                anchorId = "flashchip";
                boxId    = "chips"; // at the top: the tip goes under it
            }
            else if (!shutter)
            {
                boxId = "strip";
                for (size_t i = 0; i < kCellCount; ++i)
                    if (kCells[i] == value)
                        anchorId = llformat("cell-%d", (int)i);
            }
            Rml::Element* anchor = v.document->GetElementById(anchorId);
            Rml::Element* box    = v.document->GetElementById(boxId);
            Rml::Element* wrap   = v.document->GetElementById("tipwrap");
            if (anchor && box && wrap && v.density > 0.f)
            {
                const float px     = v.density * v.k;
                const float centre = (anchor->GetAbsoluteLeft() + anchor->GetOffsetWidth() * 0.5f) / px;
                const float left   = llclamp(centre - 210.f, 8.f, llmax(8.f, v.W - 428.f));
                const float boxTop = box->GetAbsoluteTop() / px, boxBottom = boxTop + box->GetOffsetHeight() / px;
                // Above the box, unless it is the bar or strip at the top edge, or there is no room.
                const bool below = (v.barTop && qcell < 0) || boxTop < 140.f;
                const std::string placed = llformat("%.1f|%.1f|%d", left, below ? boxBottom : boxTop, below ? 1 : 0);
                if (wrap->GetAttribute<Rml::String>("data-placed", "") != placed)
                {
                    wrap->SetAttribute("data-placed", placed);
                    wrap->SetProperty("left", llformat("%.1fdp", left));
                    if (below)
                    {
                        wrap->SetProperty("top", llformat("%.1fdp", boxBottom + 8.f));
                        wrap->SetProperty("bottom", "auto");
                    }
                    else
                    {
                        wrap->SetProperty("top", "auto");
                        wrap->SetProperty("bottom", llformat("%.1fdp", v.H - boxTop + 8.f));
                    }
                    markChanged();
                }
            }
            }
        }
    }
    set(v.drive, SLCameraModel::driveStatus(), "drive");
    if (Rml::Element* flash = v.document->GetElementById("flash"))
    {
        const F32 a = SLCameraModel::flash();
        if (a > 0.f)
        {
            const LLRect f = SLCameraModel::frame();
            place("flash", (float)f.mLeft / v.k, v.H - (float)f.mTop / v.k, (float)f.getWidth() / v.k, (float)f.getHeight() / v.k);
            flash->SetProperty("opacity", llformat("%.3f", a * 0.6f));
            markChanged();
        }
        else if (flash->GetProperty<Rml::Style::Display>("display") != Rml::Style::Display::None)
        {
            flash->SetProperty("display", "none");
            markChanged();
        }
    }
    // Unread conversations, quietly: they keep arriving while the interface is away.
    if (t >= v.nextImPoll)
    {
        v.nextImPoll = t + 1.0;
        const S32 unread = LLIMMgr::instanceExists() ? LLIMMgr::getInstance()->getNumberOfUnreadIM() : 0;
        LLStringUtil::format_map_t args;
        args["[COUNT]"] = llformat("%d", unread);
        set(v.ims, unread > 0 ? LLTrans::getString("CameraModeUnreadIMs", args) : std::string(), "ims");
    }
    {
        std::vector<KeyText> st;
        if (SLCameraModel::focusLocked())
            st.push_back({ str("StatusFocusLocked") });
        if (SLCameraModel::dofPreviewHeld())
            st.push_back({ str("StatusDepthPreview") });
        // <Prestige> AE-L, a group focus, the shutter key's half-press waiting for its second press
        if (SLCameraModel::exposureLocked())
            st.push_back({ str("StatusExposureLocked") });
        if (SLCameraModel::groupSize() > 0)
        {
            LLStringUtil::format_map_t args;
            args["[COUNT]"] = llformat("%d", (S32)SLCameraModel::groupSize());
            st.push_back({ LLTrans::getString("CameraModeStatusGroup", args) });
        }
        if (SLCameraModel::halfPressArmed())
        {
            LLStringUtil::format_map_t args;
            args["[KEY]"] = key("camera_shoot");
            st.push_back({ LLTrans::getString("CameraModeStatusHalfPress", args), "", true });
        }
        // </Prestige>
        if (SLCameraModel::driveMode() != 0)
            st.push_back({ SLCameraModel::driveLabel() });
        // <Prestige> isolate your subject: on (and PNG), easy to forget
        if (PrestigeCameraIsolate::enabled())
            st.push_back({ PrestigeCameraIsolate::statusLabel(), "", true, std::string() });
        // <Prestige> the world held still (Freeze World): easy to forget, in the accent colour
        if (PrestigeCameraShot::worldFrozen())
            st.push_back({ str("StatusFrozen"), "", true });
        // </Prestige>
        if (!v.ims.empty())
            st.push_back({ v.ims, "", true });
        bool same = st.size() == v.statuses.size();
        for (size_t i = 0; same && i < st.size(); ++i)
            same = st[i].key == v.statuses[i].key;
        if (!same)
        {
            v.statuses = std::move(st);
            dirty("statuses");
        }
    }
    set(v.savedShown, t - SLCameraModel::lastSavedAt() >= 0.0 && t - SLCameraModel::lastSavedAt() < 3.0, "savedShown");
    if (v.savedShown && SLCameraModel::lastSavedTo() != v.savedShownTo)
    {
        static const char* const saved[] = { "SavedToDisk", "SavedToInventory", "SharedToProfile", "SentByEmail" };
        v.savedShownTo = llmin(SLCameraModel::lastSavedTo(), 3u);
        set(v.savedLabel, str(saved[v.savedShownTo]), "savedLabel");
        set(v.savedDisk, v.savedShownTo == SLCameraModel::DEST_DISK, "savedDisk");
    }
    // A shot waiting for its confirmation or address: the card.
    {
        U32 dest = 0;
        S32 cost = 0;
        if (SLCameraModel::pendingShot(dest, cost) && !v.sendOpen)
            openSendCard(dest, cost);
    }
    refreshReview();
    previewGrid();
    if (v.popup == "grid" || v.popup == "iso")   // <Prestige> + isolate
        placeChipMenus();
    updateFinder();
    // <Prestige> isolate your subject: picks change from the frame's clicks
    refreshIsolate();
    updateIsolateMarks();
    // <Prestige> the AF point's confirmation, the group's markers
    updatePrestigeFocus();
    // </Prestige>
    if (v.controlsOpen)
        refreshControls(); // the dials answer the wheel at once
    if (v.popup == "quick")
        refreshQuick();
    if (v.controlsOpen && v.studioOn && !v.barAway)
    {
        if (v.railDragSec >= 0 && gViewerWindow)
        {
            railDragTo(gViewerWindow->getCurrentMouse().mX);
            refreshRail(); // a slider being dragged follows at once
        }
        else if (t >= v.railNext)
        {
            v.railNext = t + 0.25;
            refreshRail();
            fitRail();
        }
    }
    set(v.hintShown, slcambar::hintVisible(t, v.hintSince), "hintShown");
    set(v.toast, SLCameraModel::toastText(), "toast");
    refreshShot(); // <Prestige> the chips, the light and where the camera stands
    // Values that can change from elsewhere (settings, the old Photo Studio rail): 4 Hz.
    if (t >= v.nextRefresh)
    {
        v.nextRefresh = t + 0.25;
        const std::string size = sizeText();
        if (size != v.sizeLabel)
        {
            v.sizeLabel  = size;
            v.fitPending = true;
            dirty("sizeLabel");
        }
        if (v.popup == "size")
            refreshSizePopover();
        refreshDestination(); // the inventory's price follows the output size
        const bool open = gSavedSettings.getBOOL("SLCameraModeControlsOpen");
        if (open != v.controlsOpen)
        {
            v.controlsOpen = open;
            dirty("controlsOpen");
            buildHints();
        }
    }
    const bool changed = v.changed;
    v.changed          = false;
    return changed;
}

void SLCameraView::markLayer()
{
    const U32 frame = LLFrameTimer::getFrameCount();
    if (!gView.shown || gView.markedFrame == frame)
        return;
    gView.markedFrame = frame;
    gGL.flush();
    slUIMarkExternalLayer();
}

bool SLCameraView::handleMouse(S32 x, S32 y, MASK mask, EMouseClickType click, bool down)
{
    View& v = gView;
    if (!v.shown)
        return false;
    const bool over = toRml(x, y, mask);
    Rml::Element* hover = v.context->GetHoverElement();
    if (click != CLICK_LEFT && click != CLICK_DOUBLELEFT)
    {
        // Right does nothing over the document; its release always goes on (a right-drag orbit
        // that ends over the bar must still end). Middle is voice, as everywhere: never taken.
        return click == CLICK_RIGHT && down && over;
    }
    if (down)
    {
        if (!over)
        {
            if (v.sendOpen)
                return true; // the card holds a shot: Cancel or Esc lets it go, not a stray click
            if (popupOpen())
            {
                closePopup(); // a click outside closes the open popover or card and is not passed on
                return true;
            }
            return false;
        }
        v.pressedInDoc = true;
        if (within(hover, "handle"))
        {
            // A click toggles the bar; a drag past half the window moves it to that edge.
            v.handleDown  = true;
            v.handleDownY = y;
            return true;
        }
        for (Rml::Element* e = hover; e; e = e->GetParentNode())
            if (e->GetId().rfind("rtrack-", 0) == 0 && railIds(e->GetId(), v.railDragSec, v.railDragRow))
            {
                railDragTo(x); // a press on the track sets it there; a drag follows
                return true;
            }
        if (within(hover, "tiltknob") || within(hover, "tilthit"))
        {
            // A press: the knob jumps there and follows the drag; double-click: straight again.
            if (click == CLICK_DOUBLELEFT)
                SLCameraModel::setTilt(0.f);
            else
                v.tilting = true;
            return true; // not an RmlUi click
        }
        if (within(hover, "shutter"))
        {
            // Holding the shutter is the half-press (autofocus).
            set(v.shutterDown, true, "shutterDown");
            SLCameraModel::halfPress(true);
        }
        else if (v.helpOpen && !within(hover, "help") && !within(hover, "helpbtn"))
            showHelp(false);
        else if (!v.popup.empty() && !within(hover, "savepop") && !within(hover, "sizepop") && !within(hover, "gridpop")
                 && !within(hover, "quick") && !within(hover, "cellmore") && !within(hover, "review") && !within(hover, "sendcard")
                 && !within(hover, "rail") && !within(hover, "saveto") && !within(hover, "size")
                 && !within(hover, "gridchip")
                 && !within(hover, "isopop") && !within(hover, "isochip"))   // <Prestige> + isolate
            openPopup(std::string()); // another control: the popover closes, the click goes on
        v.context->ProcessMouseButtonDown(0, rmlModifiers(mask));
        markChanged();
        return true;
    }
    if (!v.pressedInDoc)
        return false;
    v.pressedInDoc = false;
    if (v.tilting)
    {
        v.tilting = false;
        return true;
    }
    if (v.railDragSec >= 0)
    {
        v.railDragSec = v.railDragRow = -1;
        return true;
    }
    if (v.handleDown)
    {
        v.handleDown = false;
        if (std::abs(y - v.handleDownY) <= 6)
            toggleBar();
        else
        {
            const bool top = y > v.rootH / 2;
            gSavedSettings.setBOOL("SLCameraModeBarTop", top);
            set(v.barTop, top, "barTop");
            v.noticeText = str(top ? "BarBackTop" : "BarBackBottom");
            dirty("noticeText");
        }
        return true;
    }
    v.context->ProcessMouseButtonUp(0, rmlModifiers(mask));
    if (v.shutterDown)
    {
        // Let go over it: the shot. Slid off: cancelled.
        set(v.shutterDown, false, "shutterDown");
        if (over && within(hover, "shutter"))
            SLCameraModel::pressShutter();
        else
            SLCameraModel::halfPress(false);
    }
    markChanged();
    return true;
}

bool SLCameraView::handleWheel(S32 x, S32 y, S32 clicks, MASK mask)
{
    if (!toRml(x, y, mask))
        return false;
    // A rail row under the pointer turns (wheel up: forward).
    for (Rml::Element* e = gView.context->GetHoverElement(); e; e = e->GetParentNode())
    {
        int sec = -1, row = -1;
        if (e->GetId().rfind("rrow-", 0) == 0 && railIds(e->GetId(), sec, row))
        {
            SLCameraRail::turn((size_t)sec, (size_t)row, clicks < 0 ? 1 : -1);
            refreshRail();
            markChanged();
            return true;
        }
    }
    // A quick setting under the pointer turns (wheel up: forward, as the Q menu always did).
    for (Rml::Element* e = gView.context->GetHoverElement(); e; e = e->GetParentNode())
        if (e->GetId().rfind("qcell-", 0) == 0)
        {
            gView.qSel = std::atoi(e->GetId().c_str() + 6);
            SLCameraModel::qTurn(gView.qSel, clicks < 0 ? 1 : -1);
            refreshQuick();
            markChanged();
            return true;
        }
    gView.context->ProcessMouseWheel(float(clicks), rmlModifiers(mask));
    markChanged();
    return true;
}

bool SLCameraView::overDocument(S32 x, S32 y)
{
    return toRml(x, y, MASK_NONE);
}

void SLCameraView::toggleBar()
{
    View& v = gView;
    if (!v.shown)
        return;
    v.barHidden = !v.barHidden;
    gSavedSettings.setBOOL("SLCameraModeBarHidden", v.barHidden);
    v.peeking  = false;
    v.hiddenAt = v.barHidden ? now() : -100.0;
    if (v.barHidden)
        openPopup(std::string()); // its popovers go with it
    set(v.barAway, v.barHidden, "barAway");
    markChanged();
}

bool SLCameraView::barHidden()
{
    return gView.shown && gView.barHidden;
}

void SLCameraView::showReadout(SLCameraModel::Value value)
{
    View& v = gView;
    if (!v.shown || !v.barAway)
        return;
    v.readoutValue_ = value;
    v.readoutAt     = now();
    markChanged();
}

void SLCameraView::openQuickSettings(S32 x, S32 y)
{
    if (!gView.shown)
        return;
    showQuick((float)x / gView.k + 8.f, gView.H - (float)y / gView.k + 8.f);
}

void SLCameraView::toggleQMenu()
{
    View& v = gView;
    if (!v.shown)
        return;
    if (v.popup == "quick")
    {
        openPopup(std::string());
        return;
    }
    showQuick((v.W - 480.f) * 0.5f, (v.H - 150.f) * 0.5f);
}

bool SLCameraView::handleQKey(KEY key, MASK mask)
{
    View& v = gView;
    if (!v.shown || v.popup != "quick" || mask != MASK_NONE)
        return false;
    const int n = (int)SLCameraModel::qItems().size();
    switch (key)
    {
        case KEY_LEFT: v.qSel = (v.qSel + n - 1) % n; break;
        case KEY_RIGHT: v.qSel = (v.qSel + 1) % n; break;
        case KEY_UP: SLCameraModel::qTurn(v.qSel, 1); break;
        case KEY_DOWN: SLCameraModel::qTurn(v.qSel, -1); break;
        default: return false;
    }
    refreshQuick();
    markChanged();
    return true;
}

slcamin::Over SLCameraView::overAt(S32 x, S32 y, SLCameraModel::Value* cell)
{
    if (!toRml(x, y, MASK_NONE))
        return slcamin::Over::World;
    Rml::Element* hover = gView.context->GetHoverElement();
    if (within(hover, "gridchip"))
        return slcamin::Over::GridChip;
    if (within(hover, "tiltknob") || within(hover, "tilthit"))
        return slcamin::Over::TiltKnob;
    for (Rml::Element* e = hover; e; e = e->GetParentNode())
    {
        const std::string& id = e->GetId();
        if (id.rfind("cell-", 0) == 0)
        {
            const int i = std::atoi(id.c_str() + 5);
            if (i >= 0 && i < (int)kCellCount)
            {
                if (cell)
                    *cell = kCells[i];
                return slcamin::Over::Cell;
            }
        }
    }
    return slcamin::Over::World;
}

bool SLCameraView::textEditing()
{
    if (!gView.shown || !gView.context)
        return false;
    Rml::Element* focus = gView.context->GetFocusElement();
    if (!focus || focus->GetOwnerDocument() != gView.document)
        return false;
    return focus->GetTagName() == "textarea" || (focus->GetTagName() == "input" && focus->GetAttribute<Rml::String>("type", "text") == "text");
}

bool SLCameraView::handleKey(KEY key, MASK mask)
{
    View& v = gView;
    if (!textEditing())
        return false;
    Rml::Input::KeyIdentifier ki = Rml::Input::KI_UNKNOWN;
    switch (key)
    {
        case KEY_ESCAPE:
        case KEY_RETURN:
            if (Rml::Element* focus = v.context->GetFocusElement())
                focus->Blur(); // done typing; the value is already written
            markChanged();
            return true;
        case KEY_BACKSPACE: ki = Rml::Input::KI_BACK; break;
        case KEY_DELETE: ki = Rml::Input::KI_DELETE; break;
        case KEY_LEFT: ki = Rml::Input::KI_LEFT; break;
        case KEY_RIGHT: ki = Rml::Input::KI_RIGHT; break;
        case KEY_HOME: ki = Rml::Input::KI_HOME; break;
        case KEY_END: ki = Rml::Input::KI_END; break;
        case KEY_TAB: ki = Rml::Input::KI_TAB; break;
        default: break;
    }
    // The standard editing keys: select all, copy, cut, paste (the viewer's clipboard, llrmluihost.cpp).
    static const std::pair<KEY, Rml::Input::KeyIdentifier> kEditKeys[] = {
        { 'A', Rml::Input::KI_A }, { 'C', Rml::Input::KI_C }, { 'X', Rml::Input::KI_X }, { 'V', Rml::Input::KI_V } };
    for (const auto& [letter, id] : kEditKeys)
        if (ki == Rml::Input::KI_UNKNOWN && (mask & MASK_CONTROL) && key == letter)
            ki = id;
    if (ki != Rml::Input::KI_UNKNOWN)
        v.context->ProcessKeyDown(ki, rmlModifiers(mask));
    markChanged();
    return true; // every other key is the field's while it has the keyboard
}

bool SLCameraView::handleNavKey(KEY key, MASK mask)
{
    View& v = gView;
    if (!v.shown || !v.context || textEditing())
        return false;
    // Tab / Shift+Tab: through the overlay's controls (spec 7.3).
    if (key == KEY_TAB && !(mask & (MASK_CONTROL | MASK_ALT)))
    {
        v.context->ProcessKeyDown(Rml::Input::KI_TAB, rmlModifiers(mask));
        markChanged();
        return true;
    }
    // The send card: Enter sends (a field being typed in let go of Enter already).
    if (v.sendOpen && key == KEY_RETURN && mask == MASK_NONE)
    {
        sendGo();
        markChanged();
        return true;
    }
    Rml::Element* focus = v.context->GetFocusElement();
    const bool ours = focus && focus != v.document && focus->GetOwnerDocument() == v.document;
    // Space activates the focused control; Enter does too while a popover, menu or card is open
    // (with nothing open, Enter is the shutter). A control hidden since it was clicked keeps
    // RmlUi's focus: it never fires again.
    if (mask == MASK_NONE && (key == ' ' || key == KEY_RETURN)
        && slcamin::navActivatesFocus(key == ' ' ? slcamin::NavKey::Space : slcamin::NavKey::Enter, ours,
                                      ours && focus->IsVisible(true), menuOpen()))
    {
        focus->Click();
        markChanged();
        return true;
    }
    return false;
}

bool SLCameraView::handleChar(llwchar uni_char)
{
    if (!textEditing() || uni_char < 0x20)
        return false;
    gView.context->ProcessTextInput(static_cast<Rml::Character>(uni_char));
    markChanged();
    return true;
}

bool SLCameraView::popupOpen()
{
    return gView.shown && (gView.helpOpen || !gView.popup.empty() || gView.cardOpen || gView.sendOpen);
}

bool SLCameraView::menuOpen()
{
    return gView.shown && (gView.helpOpen || !gView.popup.empty() || gView.cardMore || gView.sendOpen);
}

void SLCameraView::closePopup()
{
    View& v = gView;
    if (v.cardMore)
    {
        set(v.cardMore, false, "cardMore"); // the innermost first
        return;
    }
    if (v.sendOpen)
    {
        closeSendCard(); // the shot is not sent
        return;
    }
    showHelp(false);
    openPopup(std::string());
    set(v.cardOpen, false, "cardOpen");
}

void SLCameraView::toggleReview()
{
    View& v = gView;
    if (!v.shown)
        return;
    if (v.cardOpen)
        set(v.cardOpen, false, "cardOpen");
    else if (SLCameraReview::count())
        fillCard((int)SLCameraReview::count() - 1);
    refreshReview();
}

bool SLCameraView::reviewOpen()
{
    return gView.shown && gView.cardOpen;
}

bool SLCameraView::handleReviewKey(KEY key, MASK mask)
{
    View& v = gView;
    if (!v.shown || !v.cardOpen || mask != MASK_NONE || (key != KEY_LEFT && key != KEY_RIGHT) || v.cardMore)
        return false;
    const int i = shotIndex(v.cardSerial), n = (int)SLCameraReview::count();
    if (i >= 0 && n > 0)
        fillCard(llclamp(i + (key == KEY_LEFT ? -1 : 1), 0, n - 1));
    refreshReview();
    markChanged();
    return true;
}

bool SLCameraView::helpOpen()
{
    return gView.shown && gView.helpOpen;
}

void SLCameraView::forgetShots()
{
    View& v = gView;
    for (U32 serial : v.exposed)
        releaseShot(serial);
    v.exposed.clear();
    v.thumbs.clear();
    v.cardOpen = false;
    if (v.model)
    {
        v.model.DirtyVariable("thumbs");
        v.model.DirtyVariable("cardOpen");
    }
    markChanged();
}

void SLCameraView::showHelp(bool show)
{
    if (show)
    {
        buildHelp(); // the keys as they are bound now
        openPopup(std::string());
    }
    set(gView.helpOpen, show, "helpOpen");
}

void SLCameraView::setControlsOpen(bool open)
{
    gSavedSettings.setBOOL("SLCameraModeControlsOpen", open);
    View& v = gView;
    set(v.controlsOpen, open, "controlsOpen");
    if (open)
        v.hintSince = now(); // opening Controls teaches its wheel once more
    buildHints();
}
