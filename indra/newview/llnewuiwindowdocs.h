// #703 The new interface's native windows (RML), one document per window kind: the framework
// (llnewuiwindow.h) loads them for each instance, the offscreen lab renders them over sample data
// (indra/rmluilab). Layout only, on the kit (llnewuikitstyle.cpp): <whead/> and <wgrip/> are the
// frame (title bar, minimise, close; resize grip), the surface and the controls are the kit's.
// The newui-kit check holds them to that (no colour, font or type-size literals; balanced markup;
// every variable and callback bound by its controller).
#pragma once

namespace LLNewUIWindowDocs
{
// Conversations: nearby chat and every IM session as tabs (one session, no tabs, when torn off),
// the history, the line. Data model "chat" (llnewuichat.cpp).
inline constexpr const char* kChatRml = R"rml(<rml><head><title>chat</title><style>
.wbody { padding: 0; gap: 0; }
.tabs { padding: 0 6dp; }
.tab .tabx { display: inline-block; margin-left: 6dp; width: 14dp; height: 14dp; line-height: 14dp; border-radius: 7dp; }
.tabx .ic-x div { background-color: $muted; } .tabx:hover .ic-x div { background-color: $text; }
.tabx .ic-x { width: 8dp; height: 8dp; margin: 0 0 0 3dp; }
.tabx .ic-x div { left: -1dp; top: 3.25dp; width: 10dp; }
.tabx:hover { background-color: $hover; }
#chead { display: flex; flex-direction: row; align-items: center; gap: 4dp; flex: 0 0 auto; padding: 4dp 8dp; border-bottom: 1dp $border; }
#chead .title { flex: 1 1 0; white-space: nowrap; overflow: hidden; text-overflow: ellipsis; }
#chead .btn { border: 1dp $border; }
#history { padding: 6dp 10dp; }
#msgs { position: absolute; left: 10dp; right: 20dp; bottom: 6dp; }
#historyBar { width: 12dp; cursor: pointer; }
#historyThumb { width: 6dp; right: 2dp; }
.msg { padding: 1dp 0; }
.msg.sys .text, .msg.hist .text, .msg.hist .from { color: $muted; }
.msg.emote .from { font-weight: bold; }
.msg.self .from { color: $text; }
.more { text-align: center; padding: 2dp 0 6dp 0; }
#typing { flex: 0 0 auto; padding: 0 12dp; height: 16dp; }
#compose { display: flex; flex-direction: row; align-items: center; gap: 6dp; flex: 0 0 auto; padding: 8dp 24dp 8dp 8dp; border-top: 1dp $border; }
#lineBox { flex: 1 1 0; height: 28dp; line-height: 28dp; }
#conversationPicker { position: absolute; left: 8dp; top: 0; width: 340dp; max-width: 92%; max-height: 92%; overflow-y: auto; z-index: 10; padding: 10dp; box-sizing: border-box; }
#conversationSearchBox { display: block; width: 100%; height: 32dp; line-height: 32dp; box-sizing: border-box; margin-bottom: 8dp; }
#conversationSearch { display: block; width: 100%; height: 100%; }
#conversationPicker .opt { display: flex; flex-direction: row; align-items: center; gap: 8dp; min-height: 30dp; }
#conversationPicker .picker-section { padding: 6dp 4dp; }
#conversationPicker .picker-status { flex: 0 0 auto; }
#conversationPicker .picker-icon { flex: 0 0 18dp; width: 18dp; height: 18dp; image-color: $muted; }
.picker-friend { decorator: llimage(Command_People_Icon solid); }
.picker-group { decorator: llimage(TabIcon_People_Off solid); }
.grouphead { margin-top: 8dp; margin-bottom: 2dp; }
.grouphead .time { margin-left: 6dp; }
#suggest { left: 8dp; bottom: 48dp; min-width: 200dp; }
</style></head><body><div data-model="chat" class="wframe" data-event-mousedown="pickerOutside()">
<whead/>
<div id="cbody" class="wbody">
  <div class="tabs" data-if="!single">
    <div id="conversationAdd" class="btn" title="New conversation" data-event-click="newConversation()">+</div>
    <div class="tab" data-for="t : tabs" data-class-on="it_index == current" data-event-click="pick(it_index)">{{t.name}}<span class="badge" data-if="t.unread > 0">{{t.unread}}</span><span class="tabx" data-if="t.closable" data-event-click="closeTab(it_index)"><ic name="x"/></span></div>
  </div>
  <div id="chead">
    <div class="title">{{peer}}</div>
    <div class="btn" data-class-on="!grouped" data-event-click="layout(0)">IRC</div>
    <div class="btn" data-class-on="grouped" data-event-click="layout(1)">Grouped</div>
    <div class="btn" data-for="a : acts" data-event-click="act(it_index)">{{a.label}}</div>
  </div>
  <div id="history" class="viewport" data-event-click="chatLink()" data-event-mousescroll="scroll(ev.wheel_delta_y)">
    <div id="msgs">
      <div class="more caption" data-if="older">{{olderText}}</div>
      <div class="msg" data-for="m : msgs" data-class-sys="m.sys" data-class-hist="m.hist" data-class-emote="m.emote" data-class-self="m.self"><div class="grouphead" data-if="grouped &amp;&amp; m.groupHead"><span class="from">{{m.sender}}</span> <span class="time caption" data-if="stamps">{{m.time}}</span></div><span class="time caption" data-if="stamps &amp;&amp; !grouped">{{m.time}} </span><span class="from" data-if="!grouped &amp;&amp; m.from != ''">{{m.from}}</span><span class="text" data-rml="m.rich"></span></div>
      <div class="caption" data-if="msgs.size == 0">{{emptyText}}</div>
    </div>
    <div id="conversationPicker" class="menu popover" data-if="pickerOpen" data-event-mousescroll="pickerWheel()">
      <div id="conversationSearchBox" class="field"><input id="conversationSearch" class="bare" type="text" maxlength="128" data-value="pickerQuery" placeholder="Search friends and groups"/></div>
      <div class="picker-section caption" data-if="pickerFriendCount > 0">Friends</div>
      <div data-for="p : pickerLabels">
        <div class="picker-section caption" data-if="it_index == pickerFriendCount">Groups</div>
        <div class="opt" data-attr-id="'conversationOption-' + it_index" data-class-selected="it_index == pickerSelected" data-event-click="chooseConversation(it_index)"><div class="picker-icon picker-friend" data-if="it_index &lt; pickerFriendCount"></div><div class="picker-icon picker-group" data-if="it_index &gt;= pickerFriendCount"></div><div class="label">{{p}}</div><div class="caption picker-status">{{pickerStatuses[it_index]}}</div></div>
      </div>
      <div class="caption" data-if="pickerLabels.size == 0">No matching friends or groups</div>
      <div class="btn" data-event-click="searchResident()">Search all residents...</div>
    </div>
    <div id="historyBar" class="vbar" data-if="thumbH > 0" data-event-mousedown="historyPage()"><div id="historyThumb" class="vthumb" data-style-top="thumbTop + '%'" data-style-height="thumbH + '%'"></div></div>
  </div>
  <div id="typing" class="caption">{{typing}}</div>
  <div id="compose">
    <div id="lineBox" class="field"><input id="chatLine" class="bare" type="text" maxlength="1022" data-value="line" data-attr-placeholder="hint"/></div>
    <div class="btn accent" data-event-click="send()">Send</div>
  </div>
  <div id="suggest" class="menu popover" data-if="suggest.size > 0">
    <div class="opt" data-for="s : suggest" data-class-selected="it_index == 0" data-event-click="complete(it_index)"><div class="label">{{s}}</div></div>
  </div>
</div>
<wgrip/>
</div></body></rml>)rml";

// People: friends, nearby, groups, recent, blocked, with the classic actions (IM, profile,
// teleport offer / request, pay, map, friendship, block ...). Data model "people" (llnewuipeople.cpp).
inline constexpr const char* kPeopleRml = R"rml(<rml><head><title>people</title><style>
.tabs .tab { flex: 1 1 0; text-align: center; padding: 0 4dp; }
#filterBox { flex: 0 0 26dp; height: 26dp; line-height: 26dp; }
.vrow { height: 24dp; line-height: 24dp; }
.vrow .icon { width: 16dp; height: 16dp; flex: 0 0 16dp; image-color: $muted; }
.empty { padding: 8dp; }
.foot { display: flex; flex-direction: row; flex: 0 0 auto; }
.foot .caption { flex: 1 1 0; }
#nativeMap { flex: 0 1 120dp; min-height: 80dp; border: 1dp $border; }
.rangefoot { display: flex; flex-direction: row; align-items: center; gap: 8dp; flex: 0 0 24dp; }
.rangefoot input { flex: 1 1 0; }
#menu { max-height: 90%; overflow-y: auto; }
.rowim { flex: 0 0 24dp; width: 24dp; height: 20dp; margin: 2dp 4dp; padding: 0; }
.rowim .readout { margin: 2dp 4dp; }
.readout-typing { decorator: llimage(Inv_Notecard solid); }
.readout-speaking { decorator: llimage(Command_Voice_Icon solid); }
.readout-voice { decorator: llimage(Parcel_Voice_Dark solid); }
.readout-friendFlag { decorator: llimage(Favorite_Star_Off solid); }
.readout-muted { decorator: llimage(AudioMute_Off solid); }
.readout-chatRange { decorator: llimage(Nearby_chat_icon solid); }
.readout-regionFlag { decorator: llimage(Home_Off solid); }
.readout-parcelFlag { decorator: llimage(Parcel_Build_Dark solid); }
.readout-sitting { decorator: llimage(Command_Avatar_Icon solid); }
.readout-payment { decorator: llimage(Parcel_ForSale_Light solid); }
.readout-notes { decorator: llimage(Inv_Notecard solid); }
.readout-im { decorator: llimage(Command_Chat_Icon solid); }
.radarflags { display: flex; flex-direction: row; flex: 0 0 154dp; width: 154dp; }
.radarslot { flex: 0 0 14dp; width: 14dp; height: 24dp; }
.readout { width: 14dp; height: 14dp; margin-top: 4dp; image-color: $muted; }
.vrow .distance { flex: 0 0 66dp; width: 66dp; text-align: right; padding-right: 8dp; }
.vrow .label { flex: 1 1 0; min-width: 40dp; }

</style></head><body><div data-model="people" class="wframe">
<whead/>
<div id="pbody" class="wbody" data-event-mousedown="bodyDown()">
  <div class="tabs">
    <div class="tab" data-for="v : views" data-class-on="it_index == view" data-event-click="setView(it_index)">{{v}}</div>
  </div>
  <div id="nativeMap" data-if="nearby" data-event-mousedown="mapPress()" data-event-mousemove="mapHover()" data-event-dblclick="mapDouble()" data-event-mousescroll="mapScroll()"></div>
  <div id="filterBox" class="field"><input id="filter" class="bare" type="text" data-value="filter" data-attr-placeholder="filterHint"/></div>
  <div id="list" class="viewport" data-event-mousescroll="scroll(ev.wheel_delta_y)">
    <div class="vrow" data-for="r : rows" data-if="r.used" data-class-selected="r.selected" data-style-top="it_index * 24 + 'dp'"
         data-event-mousedown="press(it_index)" data-event-dblclick="activate(it_index)">
      <div class="dot" data-class-on="r.online" data-if="!r.group"></div><div class="label" data-attr-title="r.name">{{r.name}}</div><div class="btn rowim" data-if="nearby" data-event-click="rowIM(it_index)" title="Instant message"><div class="readout readout-im"></div></div><div class="radarflags" data-if="nearby"><div class="radarslot"><div class="readout readout-typing" data-if="r.typing" title="Typing"></div></div><div class="radarslot"><div class="readout readout-speaking" data-if="r.speaking" title="Speaking"></div></div><div class="radarslot"><div class="readout readout-voice" data-if="r.voice" title="Voice available"></div></div><div class="radarslot"><div class="readout readout-friendFlag" data-if="r.friendFlag" title="Friend"></div></div><div class="radarslot"><div class="readout readout-muted" data-if="r.muted" title="Muted"></div></div><div class="radarslot"><div class="readout readout-chatRange" data-if="r.chatRange" title="In chat range"></div></div><div class="radarslot"><div class="readout readout-regionFlag" data-if="r.regionFlag" title="Same region"></div></div><div class="radarslot"><div class="readout readout-parcelFlag" data-if="r.parcelFlag" title="Same parcel"></div></div><div class="radarslot"><div class="readout readout-sitting" data-if="r.sitting" title="Sitting"></div></div><div class="radarslot"><div class="readout readout-payment" data-if="r.payment" title="Payment information"></div></div><div class="radarslot"><div class="readout readout-notes" data-if="r.notes" title="Personal notes"></div></div></div><div class="distance numeric" data-attr-title="r.statusTip">{{r.sub}}</div>
    </div>
    <div class="vbar" data-if="thumbH > 0"><div class="vthumb" data-style-top="thumbTop + '%'" data-style-height="thumbH + '%'"></div></div>
    <div class="empty caption" data-if="count == 0">{{emptyText}}</div>
  </div>
  <div class="foot"><div class="caption">{{countText}}</div></div>
  <div id="radarRange" class="rangefoot" data-if="nearby"><div class="btn" data-class-on="rangeLimited" data-event-click="rangeLimit()">Limit range</div><span class="caption">{{range}} m</span><input class="range" type="range" min="20" max="4096" step="2" data-attr-disabled="!rangeLimited" data-value="range" data-event-change="rangeChanged()"/></div>
  <div class="actions">
    <div class="btn" data-for="a : acts" data-class-off="!a.on" data-event-click="act(it_index)">{{a.label}}</div>
    <div class="btn" data-event-click="more()"><ic name="more"/></div>
  </div>
  <div id="menu" class="menu popover" data-if="menuOpen" data-style-left="menuX + 'dp'" data-style-top="menuY + 'dp'">
    <div class="opt" data-for="o : menu" data-class-off="!o.on" data-event-click="menuRun(it_index)"><div class="label">{{o.label}}</div></div>
  </div>
</div>
<wgrip/>
</div></body></rml>)rml";

// Inventory: the folder tree over the real inventory (All), recent items, worn items; search per
// tab; open / wear / add / take off / delete and the rest through the inventory's own actions;
// drag items onto the world, an avatar or a classic window. Data model "inv" (llnewuiinventory.cpp).
inline constexpr const char* kInventoryRml = R"rml(<rml><head><title>inventory</title><style>
.tabs .tab { flex: 1 1 0; text-align: center; }
#frow { display: flex; flex-direction: row; gap: 6dp; flex: 0 0 26dp; }
#filterBox { flex: 1 1 0; height: 26dp; line-height: 26dp; }
#frow .btn { height: 26dp; line-height: 26dp; border: 1dp $border; }
.vrow { gap: 4dp; }
.vrow .worn { color: $accent; font-size: $fontCaption; }
.empty { padding: 8dp; }
</style></head><body><div data-model="inv" class="wframe">
<whead/>
<div id="ibody" class="wbody" data-event-mousedown="bodyDown()">
  <div class="tabs">
    <div class="tab" data-for="v : views" data-class-on="it_index == view" data-event-click="setView(it_index)">{{v}}</div>
  </div>
  <div id="frow">
    <div id="filterBox" class="field"><input id="filter" class="bare" type="text" data-value="filter" placeholder="Search inventory"/></div>
    <div class="btn" data-event-click="newWindow()">New window</div>
  </div>
  <div id="list" class="viewport" data-event-mousescroll="scroll(ev.wheel_delta_y)">
    <div class="vrow" data-for="r : rows" data-if="r.used" data-class-selected="r.selected" data-style-top="it_index * 22 + 'dp'"
         data-event-mousedown="press(it_index)" data-event-dblclick="activate(it_index)">
      <div data-style-flex="'0 0 ' + r.indent + 'dp'"></div>
      <div class="twist" data-event-click="twist(it_index)"><span class="chev" data-if="r.twist != ''" data-class-down="r.twist == 'open'" data-class-right="r.twist == 'closed'"></span></div>
      <div class="ico" data-style-decorator="r.deco"></div>
      <div class="label">{{r.label}}</div>
      <div class="worn" data-if="r.worn">worn</div>
    </div>
    <div class="vbar" data-if="thumbH > 0"><div class="vthumb" data-style-top="thumbTop + '%'" data-style-height="thumbH + '%'"></div></div>
    <div class="empty caption" data-if="count == 0">{{emptyText}}</div>
  </div>
  <div class="caption">{{status}}</div>
  <div class="actions">
    <div class="btn" data-for="a : acts" data-class-off="!a.on" data-event-click="act(it_index)">{{a.label}}</div>
    <div class="btn" data-event-click="more()"><ic name="more"/></div>
  </div>
  <div id="menu" class="menu popover" data-if="menuOpen" data-style-left="menuX + 'dp'" data-style-top="menuY + 'dp'">
    <div class="opt" data-for="o : menu" data-class-off="!o.on" data-event-click="menuRun(it_index)"><div class="label">{{o.label}}</div></div>
  </div>
</div>
<wgrip/>
</div></body></rml>)rml";
} // namespace LLNewUIWindowDocs
