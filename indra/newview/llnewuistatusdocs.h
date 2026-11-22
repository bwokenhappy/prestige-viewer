// #703 The new interface's status bar and its menus (RML): the viewer's controllers load them
// (llnewuistatus.cpp, llnewuimenus.cpp) and the offscreen lab renders them with sample models
// (indra/rmluilab). Layout only, as llnewuidocs.h: colours, type and the controls come from the
// kit (llnewuikitstyle.cpp) through LLNewUIKit::themed; the newui-kit check holds them to that.
// Standard C++ only (the lab includes it).
#pragma once

namespace LLNewUIStatusDocs
{
// The status bar: the parcel's restrictions, frame rate, network, time, L$, media, volume,
// presets, gesture sets and the "more" menu, in a small floating bar at the top right; the tip
// card under the hovered part. Data model "status".
inline constexpr const char* kStatusRml = R"rml(<rml><head><title>status</title><style>
body { position: absolute; left: 0; top: 0; right: 0; bottom: 0; pointer-events: none; }
#statwrap { position: absolute; right: 8dp; top: 4dp; pointer-events: none; }
#stats { pointer-events: auto; }
#st-fps .v { display: inline-block; min-width: 22dp; text-align: right; }
#st-time .v { font-weight: normal; }
#stipwrap { position: absolute; width: 280dp; visibility: hidden; }
#stipwrap.shown { visibility: visible; }
.p-voice { decorator: llimage(Parcel_VoiceNo_Dark solid); }
.p-fly { decorator: llimage(Parcel_FlyNo_Dark solid); }
.p-push { decorator: llimage(Parcel_PushNo_Dark solid); }
.p-build { decorator: llimage(Parcel_BuildNo_Dark solid); }
.p-scripts { decorator: llimage(Parcel_ScriptsNo_Dark solid); }
.p-seeavs { decorator: llimage(Parcel_SeeAVsOff_Light solid); }
.p-damage { decorator: llimage(Parcel_Health_Dark solid); }
.i-presets { decorator: llimage(Presets_Icon solid); }
.i-camera { decorator: llimage(Cam_FreeCam_Off solid); }
.i-gestures { decorator: llimage(Inv_Gesture solid); }
</style></head><body><div data-model="status">
<div id="statwrap"><div id="stats" class="stats floatbar">
  <div id="st-parcel" class="st" data-if="showParcel" data-event-click="click('parcel')">
    <span class="v muted">{{rating}}</span>
    <div class="sic p-voice warn" data-if="noVoice"></div><div class="sic p-fly warn" data-if="noFly"></div>
    <div class="sic p-push warn" data-if="noPush"></div><div class="sic p-build warn" data-if="noBuild"></div>
    <div class="sic p-scripts warn" data-if="noScripts"></div><div class="sic p-seeavs warn" data-if="noSeeAvs"></div>
    <div class="sic p-damage bad" data-if="damage"></div><span class="v bad" data-if="damage">{{health}}</span>
  </div>
  <div id="st-fps" class="st" data-if="showFps" data-event-click="click('fps')">
    <span class="v" data-class-good="fpsLevel == 1" data-class-warn="fpsLevel == 2" data-class-bad="fpsLevel == 3" data-class-fgen="fpsLevel == 4">{{fps}}</span><span class="u">fps</span>
  </div>
  <div id="st-net" class="st" data-if="showNet" data-event-click="click('net')">
    <span class="v" data-class-good="pingLevel == 1" data-class-warn="pingLevel == 2" data-class-bad="pingLevel == 3">{{ping}}</span><span class="u">ms</span>
    <span class="v" data-class-good="lossLevel == 1" data-class-warn="lossLevel == 2" data-class-bad="lossLevel == 3">{{loss}}</span><span class="u">% loss</span>
  </div>
  <div id="st-bw" class="st" data-if="showBandwidth" data-event-click="click('net')"><span class="v">{{bandwidth}}</span><span class="u">kbps</span></div>
  <div id="st-time" class="st info" data-if="showTime"><span class="v">{{time}}</span></div>
  <div id="st-money" class="st" data-if="showBalance" data-class-on="menuOn == 'balance'" data-event-click="click('balance')">
    <span class="u">L$</span><span class="v">{{balance}}</span>
  </div>
  <div id="st-media" class="st" data-if="showMedia" data-class-off="!mediaUsable" data-event-click="click('media')"><div class="sic" data-style-decorator="mediaIcon"></div></div>
  <div id="st-volume" class="st" data-if="showVolume" data-event-click="click('volume')"><div class="sic" data-class-bad="muted" data-style-decorator="volumeIcon"></div></div>
  <div id="st-presets" class="st" data-if="showPresets" data-event-click="click('presets')"><div class="sic i-presets"></div></div>
  <div id="st-camera" class="st" data-if="showPresets" data-event-click="click('camera')"><div class="sic i-camera"></div></div>
  <div id="st-gestures" class="st" data-if="showGestures" data-event-click="click('gestures')"><div class="sic i-gestures"></div></div>
  <div id="st-more" class="st" data-class-on="menuOn == 'more'" data-event-click="click('more')"><ic name="more"/></div>
</div></div>
<div id="stipwrap" data-class-shown="tip != ''"><div class="tipcard"><div class="tt">{{tip}}</div><div class="tw" data-if="tipWhat != ''">{{tipWhat}}</div>
  <div class="tc" data-if="tipClick != ''">{{tipClick}}</div></div></div>
</div></body></rml>)rml";

// A menu of rows under the status bar (the L$ actions, "more", and the classic menus to browse):
// a check box for toggles, the keys, a chevron for a submenu, a head with the way back. Data
// model "smenu".
inline constexpr const char* kStatusMenuRml = R"rml(<rml><head><title>smenu</title><style>
body { position: absolute; left: 0; top: 0; right: 0; bottom: 0; pointer-events: none; }
#mscrim { pointer-events: auto; }
#menu { pointer-events: auto; width: 300dp; top: 40dp; right: 8dp; }
</style></head><body><div data-model="smenu">
<div id="mscrim" class="scrim clear" data-event-click="close()"></div>
<div id="menu" class="popover menu" data-event-mousescroll="scroll(ev.wheel_delta_y)">
  <div class="mhead" data-if="title != ''">
    <div class="btn" data-if="canBack" data-event-click="back()"><span class="chev left"></span></div><div class="title">{{title}}</div>
  </div>
  <div data-for="r : rows">
    <div class="sep" data-if="r.sep"></div>
    <div class="opt" data-if="!r.sep" data-class-off="r.off" data-event-click="pick(it_index)">
      <div class="mk"><div class="ckb" data-if="r.check" data-class-on="r.on"><div class="t"></div></div></div>
      <div class="label">{{r.label}}</div><div class="hk" data-if="r.hint != ''">{{r.hint}}</div><span class="chev right" data-if="r.branch"></span>
    </div>
  </div>
  <div class="caption" data-if="more != ''">{{more}}</div>
</div></div></body></rml>)rml";
} // namespace LLNewUIStatusDocs
