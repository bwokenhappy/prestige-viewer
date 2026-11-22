// #703 The new interface's documents (RML), in one place: the viewer's controllers load them
// (llnewui.cpp, llnewuitools.cpp; the windows are in llnewuiwindowdocs.h) and the offscreen lab renders them with
// sample models (indra/rmluilab). Layout only: colours, type, radii and the controls come from
// the kit (llnewuikitstyle.cpp), through LLNewUIKit::themed. The newui-kit check holds them to
// that (no colour, font or type-size literals) and checks their markup like camera-rml.
// Camera mode's document stays with its view (slcameraview.cpp, kCameraRml).
#pragma once

namespace LLNewUIDocs
{
// The main bar: where you are and the command field, centred at the top (or bottom) edge. Data
// model "bar".
inline constexpr const char* kBarRml = R"rml(<rml><head><title>bar</title><style>
body { position: absolute; left: 0; top: 0; right: 0; bottom: 0; pointer-events: none; }
#barwrap { display: flex; justify-content: center; }
#bar { display: flex; flex-direction: row; align-items: center; width: 1000dp; max-width: 96%; height: auto; gap: 6dp; padding: 4dp; }
#bar .btn { display: flex; align-items: center; justify-content: center; flex: 0 0 auto; height: 28dp; line-height: 28dp; gap: 6dp; }
#actions { display: flex; flex-direction: row; flex: 0 0 auto; align-items: center; gap: 4dp; }
#actions .btn { padding: 0 10dp; }
#bar .navicon { width: 18dp; height: 18dp; image-color: $text; }
#bar .navsquare { width: 32dp; padding: 0; }
#location { flex: 1 1 220dp; min-width: 80dp; max-width: 300dp; margin: 0; overflow: hidden; text-overflow: ellipsis; }
#command { flex: 1 1 240dp; min-width: 100dp; max-width: 320dp; margin: 0; overflow: hidden; }
#bar #actionsCompact { display: none; }
@media (max-width: 900dp) { #actions .frequent { display: none; } }
@media (max-width: 650dp) { #actions { display: none; } #bar #actionsCompact { display: flex; } #bar .hk { display: none; } }
.nav-upload { decorator: llimage(Icon_File_Upload solid); }
.nav-home { decorator: llimage(Home_Off solid); }
.nav-favourite { decorator: llimage(Favorite_Star_Off solid); }
.nav-camera { decorator: llimage(Command_Snapshot_Icon solid); }
.nav-people { decorator: llimage(Command_People_Icon solid); }
#navscrim { pointer-events: auto; }
#navpop { position: absolute; left: 50%; margin-left: -220dp; top: 40dp; width: 440dp; max-height: 420dp; overflow-y: auto; pointer-events: auto; text-align: left; }
#navAddress { display: flex; flex-direction: row; gap: 6dp; }
#navAddress .field { flex: 1 1 0; height: 28dp; }
</style></head><body><div data-model="bar">
<div id="barwrap" class="barwrap"><div id="bar" class="bar floatbar">
  <div id="actions"><div class="btn" title="Upload image, sound, animation, model or bulk" data-event-click="navigation(3)"><div class="navicon nav-upload"></div><span>Upload</span></div><div class="btn frequent" title="Take a snapshot" data-event-click="quickAction(0)"><div class="navicon nav-camera"></div><span>Snapshot</span></div><div class="btn frequent" title="Open People" data-event-click="quickAction(1)"><div class="navicon nav-people"></div><span>People</span></div></div>
  <div id="actionsCompact" class="btn navsquare" title="Create and upload" data-event-click="navigation(3)"><ic name="more"/></div>
  <div class="btn navsquare" title="Teleport home" data-event-click="home()"><div class="navicon nav-home"></div></div>
  <div class="btn navsquare" title="Favourites" data-event-click="navigation(2)"><div class="navicon nav-favourite"></div></div>
  <div id="location" class="chip" data-event-click="navigation(1)" data-attr-title="location">{{location}}</div>
  <div id="command" class="cbtn cmd" data-event-click="open()"><ic name="find"/>{{hint}}<span class="hk" data-if="key != ''">{{key}}</span></div>
</div></div>
<div id="navscrim" class="scrim clear" data-if="navOpen" data-event-click="navClose()"></div>
<div id="navpop" class="popover menu" data-if="navOpen">
  <div id="navAddress" data-if="navMode == 1"><div class="field"><input id="address" class="bare" type="text" maxlength="512" data-value="address" placeholder="Region, SLURL, or x/y/z"/></div><div class="btn accent" data-event-click="navGo()"><span>Go</span></div></div>
  <div class="opt" data-for="n : navLabels" data-class-selected="it_index == navSelected" data-class-off="navEnabled[it_index] == 0" data-event-click="navPick(it_index)"><div class="label">{{n}}</div></div>
  <div class="btn" data-event-click="navClose()"><span>Close</span></div>
</div></div></body></rml>)rml";

// The command surface: search menus, windows and settings; the keys it takes are shown under the
// results. Data model "palette".
inline constexpr const char* kPaletteRml = R"rml(<rml><head><title>palette</title><style>
body { position: absolute; left: 0; top: 0; right: 0; bottom: 0; pointer-events: none; }
#scrim { pointer-events: auto; }
#surface { pointer-events: auto; left: 50%; margin-left: -300dp; width: 600dp; top: 96dp; }
#queryBox { height: 32dp; line-height: 32dp; } #query { font-size: $fontTitle; }
#results { margin-top: 8dp; max-height: 420dp; }
.empty { padding: 8dp; }
</style></head><body><div data-model="palette">
<div id="scrim" class="scrim" data-event-click="close()"></div>
<div id="surface" class="popover">
  <div id="queryBox" class="field"><input id="query" class="bare" type="text" data-value="query" placeholder="Search menus, windows and settings"/></div>
  <div id="results" class="list">
    <div class="opt" data-for="r : rows" data-class-selected="it_index == selected" data-class-off="r.off" data-event-click="run(it_index)">
      <div class="kind">{{r.kind}}</div><div class="mk"><div class="ckb" data-if="r.check" data-class-on="r.on"><div class="t"></div></div></div>
      <div class="label">{{r.label}}</div><div class="path">{{r.path}}</div>
      <div class="hk" data-if="r.shortcut != ''">{{r.shortcut}}</div>
    </div>
    <div class="empty caption" data-if="rows.size == 0">No matches</div>
  </div>
  <div class="keys"><span class="hk">Up</span><span class="hk">Down</span><span class="ht end">choose</span><span class="hk">Enter</span><span class="ht end">run</span><span class="hk">Esc</span><span class="ht">close</span></div>
</div></div></body></rml>)rml";

// The toolbar commands as an icon grid at the bottom centre: a pull-up tab flush with the bottom
// edge (or above the bar there) that opens the grid while hovered; the grid's grip locks it open.
// The tip card over the hovered button. Data model "tools".
inline constexpr const char* kToolsRml = R"rml(<rml><head><title>tools</title><style>
body { position: absolute; left: 0; top: 0; right: 0; bottom: 0; pointer-events: none; }
#toolwrap { bottom: 12dp; }
#tabwrap { bottom: 0; }
#tipwrap { position: absolute; width: 300dp; visibility: hidden; }
#tipwrap.shown { visibility: visible; }
</style></head><body><div data-model="tools">
<div id="toolwrap" class="barwrap">
  <div id="tools" class="tools floatbar" data-if="open" data-class-locked="locked" data-style-width="width">
    <div id="hide" class="handle" data-event-click="lock()"><div class="grip"></div></div>
    <div class="tool" data-for="t : tools" data-attr-id="t.id" data-class-on="t.on" data-class-off="t.off"
         data-event-mousedown="press(it_index)" data-event-click="run(it_index)"><div class="icon" data-style-decorator="t.icon"></div></div>
  </div>
</div>
<div id="tabwrap" class="barwrap">
  <div id="tab" class="toolstab" data-if="!open" data-event-click="lock()"><div class="tabface"><div class="grip"></div></div></div>
</div>
<div id="tipwrap" data-class-shown="tip != ''"><div class="tipcard"><div class="tt">{{tip}}</div><div class="tw" data-if="tipWhat != ''">{{tipWhat}}</div>
  <div class="tc" data-if="tipClick != ''"><span class="hk" data-if="tipKey != ''">{{tipKey}}</span>{{tipClick}}</div></div></div>
</div></body></rml>)rml";

} // namespace LLNewUIDocs
