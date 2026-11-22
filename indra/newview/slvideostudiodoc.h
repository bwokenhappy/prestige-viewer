// Video Studio's overlay document (#1668): the RML, std only, so the offscreen lab (indra/rmluilab,
// --studio) renders exactly what the viewer shows (slvideostudioview.cpp binds its data model "vstudio").
#pragma once

// Layout only; every colour, radius and type size comes from the kit's tokens ($name).
inline const char* const kStudioRml = R"rml(<rml><head><title>videostudio</title><style>
body { position: absolute; left: 0; top: 0; right: 0; bottom: 0; pointer-events: none; }
#panel { position: absolute; right: 16dp; top: 16dp; bottom: 92dp; width: 340dp; box-sizing: border-box; pointer-events: auto; }
#panel .vhead { position: absolute; left: 12dp; right: 8dp; top: 8dp; height: 28dp; display: flex; flex-direction: row; align-items: center; }
#panel .vhead .title { flex: 1 1 0; }
#tabs { position: absolute; left: 10dp; right: 10dp; top: 42dp; }
#vbody { position: absolute; left: 10dp; right: 10dp; top: 74dp; bottom: 44dp; overflow-y: auto; }
/* the rows scroll when the panel is shorter than them (a large UI size): a thin bar, no arrows */
#vbody scrollbarvertical { width: 6dp; margin-left: 4dp; }
#vbody scrollbarvertical slidertrack { background-color: transparent; }
#vbody scrollbarvertical sliderbar { min-height: 24dp; border-radius: 3dp; background-color: $borderStrong; }
#vbody scrollbarvertical sliderarrowdec, #vbody scrollbarvertical sliderarrowinc { width: 0; height: 0; }
#vhint { position: absolute; left: 12dp; right: 12dp; bottom: 8dp; height: 30dp; }
.vsec { height: 22dp; line-height: 22dp; margin-top: 10dp; color: $accent; }
.vnote { margin: 4dp 0 6dp 0; }
.rrow .rl { width: 104dp; }
.rrow .rv { width: 70dp; }
.rrow.tg .rl { flex: 1 1 0; width: auto; } /* a toggle row: the label takes the room, the switch sits right */
.rrow .rname { width: 120dp; white-space: nowrap; overflow: hidden; text-overflow: ellipsis; }
.rrow.on .rname { color: $accent; }
#transport { position: absolute; left: 50%; margin-left: -320dp; width: 640dp; bottom: 22dp; height: 48dp; box-sizing: border-box;
             padding: 0 10dp; display: flex; flex-direction: row; align-items: center; gap: 8dp; pointer-events: auto; }
#transport .cbtn { flex: 0 0 auto; }
#transport .cbtn.rec { background-color: $error; border-color: $error; color: $onAccent; font-weight: bold; } /* filled: readable in both themes */
#transport .cbtn.rec.on { border-color: $text; }
#scrub { flex: 1 1 0; }
#time { flex: 0 0 auto; width: 92dp; text-align: right; white-space: nowrap; }
#recpill { position: absolute; left: 24dp; top: 24dp; pointer-events: none; }
#recpill .dot { display: inline-block; width: 10dp; height: 10dp; border-radius: 5dp; background-color: $error; margin-right: 6dp; }
</style></head>
<body data-model="vstudio">
<div id="recpill" class="capsule" data-if="recStatus != '' && !hidden"><span class="dot"></span>{{recStatus}}</div>
<div id="panel" class="panel" data-if="!hidden">
  <div class="vhead"><div class="title">{{title}}</div>
    <div class="btn" data-event-click="leave()"><ic name="x"/></div></div>
  <div id="tabs" class="segmented"><div class="seg" data-for="t : tabs" data-class-on="t.on" data-event-click="vtab(it_index)">{{t.label}}</div></div>
  <div id="vbody">
    <div data-for="r : rows">
      <div class="vsec" data-if="r.kind == 5">{{r.label}}</div>
      <div class="vnote caption" data-if="r.kind == 4">{{r.label}}</div>
      <div class="rrow" data-if="r.kind < 4" data-attr-id="r.id" data-class-dim="!r.enabled" data-class-on="r.on" data-class-tg="r.kind == 1">
        <div class="rl" data-if="r.kind != 3">{{r.label}}</div>
        <div class="rname" data-if="r.kind == 3 && r.label != ''">{{r.label}}</div>
        <div class="rtrack" data-if="r.kind == 0" data-attr-id="r.track"><div class="tr"><div class="fl" data-style-width="r.w"></div></div></div>
        <div class="toggle" data-if="r.kind == 1" data-class-on="r.on" data-event-click="vturn(it_index, 1)"><div class="knob"></div></div>
        <div class="rchoice" data-if="r.kind == 2"><div class="arr" data-event-click="vturn(it_index, -1)"><span class="chev left"></span></div><div class="cv" data-event-click="vturn(it_index, 1)">{{r.value}}</div>
          <div class="arr" data-event-click="vturn(it_index, 1)"><span class="chev right"></span></div></div>
        <div class="rbtns" data-if="r.kind == 3"><div class="btn" data-for="b : r.buttons" data-event-click="vpress(r.index, it_index)">{{b}}</div></div>
        <div class="rv" data-if="r.kind == 0">{{r.value}}</div>
      </div>
    </div>
  </div>
  <div id="vhint" class="caption">{{hint}}</div>
</div>
<div id="transport" class="panel" data-if="!hidden">
  <div class="cbtn" data-class-on="playing" data-event-click="play()">{{playLabel}}</div>
  <div id="scrub" class="rtrack"><div class="tr"><div class="fl" data-style-width="scrubW"></div></div></div>
  <div id="time">{{timeText}}</div>
  <div class="cbtn rec" data-class-on="recording" data-event-click="rec()">{{recLabel}}</div>
  <div class="cbtn" data-event-click="leave()">{{leaveLabel}}</div>
</div>
</body></rml>)rml";
