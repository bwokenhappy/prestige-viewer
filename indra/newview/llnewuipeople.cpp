// #703 The new interface's People window on the window framework (llnewuiwindow.h): friends,
// nearby, groups, recent and blocked, bound to the viewer's models (LLAvatarTracker, LLWorld,
// gAgent's groups, LLRecentPeople, LLMuteList), with the classic actions run through the viewer's
// own functions (LLAvatarActions, LLGroupActions): IM, profile, teleport offer / request, pay, map,
// share, call, friendship, group invite, block. The parity list: doc/newui-windows.md.
#include "llviewerprecompiledheaders.h"

#include "llnewuiwindow.h"
#include "llnewuiwindowdocs.h"
#include "llnewuiwindowkinds.h"
#include "llnewuiwinparts.h"

#include "llagent.h"
#include "llnetmap.h"
#include "lluictrlfactory.h"
#include "llui.h"
#include "llviewerregion.h"
#include "llviewerobjectlist.h"
#include "llvoavatar.h"
#include "llvoiceclient.h"
#include "fsradar.h"
#include "rlvactions.h"
#include "llavataractions.h"
#include "slavataractions.h"
#include "fsradarmenu.h"
#include "fsfloateravataralign.h"
#include "lllogchat.h"
#include "llavatarnamecache.h"
#include "llcallingcard.h"
#include "llgroupactions.h"
#include "llmutelist.h"
#include "llrecentpeople.h"
#include "llviewercontrol.h"
#include "llworld.h"

#include <RmlUi/Core/DataModelHandle.h>
#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Event.h>
#include <algorithm>
#include <cctype>
#include <unordered_map>

namespace
{
constexpr int   kPool  = 48; // row slots: the most a tall window shows
constexpr float kRowDp = 24.f;

enum View
{
    Friends,
    Nearby,
    Groups,
    Recent,
    Blocked,
    ViewCount
};
const char* const kViewNames[ViewCount] = { "Friends", "Nearby", "Groups", "Recent", "Blocked" };

struct Row
{
    std::string name, sub, status, statusTip;
    bool typing = false, speaking = false, voice = false, friendFlag = false, muted = false, chatRange = false, regionFlag = false, parcelFlag = false, sitting = false, payment = false, notes = false;
    bool        online = false, used = false, selected = false, group = false;
};
struct Entry
{
    bool typing = false, speaking = false, voice = false, friendFlag = false, muted = false, chatRange = false, regionFlag = false, parcelFlag = false, sitting = false, payment = false, notes = false;
    LLUUID      id;
    std::string name, sub, folded, status, statusTip;
    bool        online = false, group = false;
    F32         dist   = 0.f;
    LLMute      mute{ LLUUID::null };
};

std::string fold(std::string s)
{
    for (char& c : s)
        c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string nameOf(const LLUUID& id)
{
    LLAvatarName name;
    return LLAvatarNameCache::get(id, &name) ? name.getCompleteName() : std::string("Loading...");
}

class PeopleWindow final : public LLNewUIWindow
{
public:
    PeopleWindow()
    {
        mViews.assign(kViewNames, kViewNames + ViewCount);
        mRows.resize(kPool);
    }
    const char* rml() const override { return LLNewUIWindowDocs::kPeopleRml; }
    const char* model() const override { return "people"; }
    LLView* classicOverlay() override
    {
        if (mView != Nearby || mMenuOpen) return nullptr;
        if (!mMap && !mMapFailed)
        {
            try
            {
                LLNetMap::Params params; params.name = "native_nearby_map";
                mMap.reset(LLUICtrlFactory::create<LLNetMap>(params)); // includes postBuild
                mMapFailed = !mMap;
            }
            catch (const std::bad_alloc&) { mMapFailed = true; }
        }
        return mMap.get();
    }
    void mapEvent(Rml::Event& event, int kind)
    {
        auto* map = static_cast<LLNetMap*>(classicOverlay());
        if (!map || !doc) return;
        S32 x=0,y=0;
        LLUI::getInstance()->getMousePositionLocal(map,&x,&y);
        MASK mask=MASK_NONE;
        if (event.GetParameter<int>("ctrl_key",0)) mask |= MASK_CONTROL;
        if (event.GetParameter<int>("shift_key",0)) mask |= MASK_SHIFT;
        if (event.GetParameter<int>("alt_key",0)) mask |= MASK_ALT;
        if (kind == 0) { if (buttonOf(event) == 1) map->handleRightMouseDown(x,y,mask); else map->handleMouseDown(x,y,mask); }
        else if (kind == 1) map->handleHover(x,y,mask);
        else if (kind == 2) map->handleDoubleClick(x,y,mask);
        else { const float delta=event.GetParameter<float>("wheel_delta_y",0.f);
               if (delta != 0.f) map->handleScrollWheel(x,y,delta>0.f ? 1 : -1); }

    }

    bool bind(Rml::DataModelConstructor& m) override
    {
        if (auto r = m.RegisterStruct<Row>())
        {
            r.RegisterMember("name", &Row::name);
            r.RegisterMember("sub", &Row::sub);
            r.RegisterMember("status", &Row::status);
            r.RegisterMember("typing", &Row::typing);
            r.RegisterMember("speaking", &Row::speaking);
            r.RegisterMember("voice", &Row::voice);
            r.RegisterMember("friendFlag", &Row::friendFlag);
            r.RegisterMember("muted", &Row::muted);
            r.RegisterMember("chatRange", &Row::chatRange);
            r.RegisterMember("regionFlag", &Row::regionFlag);
            r.RegisterMember("parcelFlag", &Row::parcelFlag);
            r.RegisterMember("sitting", &Row::sitting);
            r.RegisterMember("payment", &Row::payment);
            r.RegisterMember("notes", &Row::notes);

            r.RegisterMember("statusTip", &Row::statusTip);
            r.RegisterMember("online", &Row::online);
            r.RegisterMember("used", &Row::used);
            r.RegisterMember("selected", &Row::selected);
            r.RegisterMember("group", &Row::group);
        }
        m.RegisterArray<std::vector<Row>>();
        m.RegisterArray<std::vector<std::string>>();
        llnewuiRegisterItems(m);
        m.BindEventCallback("mapPress", [this](Rml::DataModelHandle, Rml::Event& e, const Rml::VariantList&) { mapEvent(e,0); });
        m.BindEventCallback("mapHover", [this](Rml::DataModelHandle, Rml::Event& e, const Rml::VariantList&) { mapEvent(e,1); });
        m.BindEventCallback("mapDouble", [this](Rml::DataModelHandle, Rml::Event& e, const Rml::VariantList&) { mapEvent(e,2); });
        m.BindEventCallback("mapScroll", [this](Rml::DataModelHandle, Rml::Event& e, const Rml::VariantList&) { mapEvent(e,3); });
        m.Bind("rangeLimited", &mRangeLimited);
        m.BindEventCallback("rangeLimit", [this](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&)
            { mRangeLimited = !mRangeLimited; gSavedSettings.setBOOL("LimitRadarByRange",mRangeLimited); dirty("rangeLimited"); mNext=0; });
        m.BindFunc("range", [this](Rml::Variant& out) { out=mRange; }, [this](const Rml::Variant& in)
            { mRange=std::clamp(in.Get<float>(),20.f,4096.f); gSavedSettings.setF32("NearMeRange",mRange); mNext=0; });
        m.Bind("nearby", &mNearby);
        m.BindEventCallback("rangeChanged", [this](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&)
            { mNext = 0; });
        m.Bind("views", &mViews);
        m.Bind("view", &mView);
        m.Bind("filter", &mFilter);
        m.Bind("filterHint", &mFilterHint);
        m.Bind("rows", &mRows);
        m.Bind("count", &mScroll.count);
        m.Bind("countText", &mCountText);
        m.Bind("emptyText", &mEmptyText);
        m.Bind("thumbTop", &mScroll.thumbTop);
        m.Bind("thumbH", &mScroll.thumbH);
        m.Bind("acts", &mActs);
        m.Bind("menu", &mMenu);
        m.Bind("menuOpen", &mMenuOpen);
        m.Bind("menuX", &mMenuX);
        m.Bind("menuY", &mMenuY);
        m.BindEventCallback("setView", [this](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList& a)
                            { if (!a.empty()) setView(a[0].Get<int>()); });
        m.BindEventCallback("scroll", [this](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList& a)
                            {
                                if (a.empty()) return;
                                mScroll.first += a[0].Get<float>() > 0 ? 3 : -3;
                                fill();
                            });
        m.BindEventCallback("press", [this](Rml::DataModelHandle, Rml::Event& e, const Rml::VariantList& a)
                            {
                                if (a.empty()) return;
                                select(mScroll.first + a[0].Get<int>());
                                if (buttonOf(e) == 1)
                                    openMenu(&e);
                            });
        m.BindEventCallback("activate", [this](Rml::DataModelHandle, Rml::Event& event, const Rml::VariantList& a)
                            {
                                if (a.empty()) return;
                                for (auto* target=event.GetTargetElement(); target; target=target->GetParentNode())
                                    if (target->IsClassSet("rowim")) return;
                                select(mScroll.first + a[0].Get<int>());
                                run(mView == Groups ? "chat" : mView == Nearby ? "zoom" : mView == Blocked ? "profile" : "im");
                            });
        m.BindEventCallback("rowIM", [this](Rml::DataModelHandle, Rml::Event& event, const Rml::VariantList& a)
                            {
                                event.StopPropagation();
                                if (a.empty()) return;
                                select(mScroll.first + a[0].Get<int>());
                                run("im");
                            });
        m.BindEventCallback("act", [this](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList& a)
                            {
                                const size_t i = a.empty() ? mActs.size() : size_t(a[0].Get<int>());
                                if (i < mActs.size() && mActs[i].on)
                                    run(mActs[i].id);
                            });
        m.BindEventCallback("more", [this](Rml::DataModelHandle, Rml::Event& e, const Rml::VariantList&) { openMenu(&e); });
        m.BindEventCallback("menuRun", [this](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList& a)
                            {
                                const size_t i = a.empty() ? mMenu.size() : size_t(a[0].Get<int>());
                                if (i < mMenu.size() && mMenu[i].on)
                                {
                                    const std::string id = mMenu[i].id;
                                    closeMenu();
                                    run(id);
                                }
                            });
        m.BindEventCallback("bodyDown", [this](Rml::DataModelHandle, Rml::Event& e, const Rml::VariantList&)
                            {
                                // A press outside the menu closes it (the right press that opened it excepted).
                                if (!mMenuOpen) return;
                                for (Rml::Element* t = e.GetTargetElement(); t; t = t->GetParentNode())
                                    if (t->GetId() == "menu" || (buttonOf(e) == 1 && t->IsClassSet("vrow")) || t->IsClassSet("btn"))
                                        return;
                                closeMenu();
                            });
        return true;
    }

    bool sync(F64 now) override
    {
        // The list follows its model at 1 Hz (names arriving, people coming and going), the
        // filter and the window's height at once.
        const bool limited = gSavedSettings.getBOOL("LimitRadarByRange");
        if (mRangeLimited != limited) { mRangeLimited=limited; dirty("rangeLimited"); mNext=0; }
        const F32 savedRange = std::clamp(gSavedSettings.getF32("NearMeRange"), 20.f, 4096.f);
        if (mRange != savedRange) { mRange = savedRange; dirty("range"); mNext = 0; }
        const bool nearby = mView == Nearby;
        if (nearby != mNearby) { mNearby = nearby; dirty("nearby"); }
        const int slots = visibleRows("list", kRowDp, kPool);
        bool      again = slots != mScroll.fit || mFilter != mLastFilter;
        if (now >= mNext)
        {
            mNext = now + 1.0;
            collect();
            again = true;
        }
        if (again)
        {
            if (mFilter != mLastFilter)
            {
                mLastFilter   = mFilter;
                mScroll.first = 0;
                applyFilter();
            }
            mScroll.fit = slots;
            fill();
        }
        return false;
    }

    bool onKey(KEY key, MASK mask) override
    {
        if (mask != MASK_NONE)
            return false;
        const int sel = selectedIndex();
        switch (key)
        {
            case KEY_UP: select(std::max(0, sel - 1)); return true;
            case KEY_DOWN: select(std::min(int(mShown.size()) - 1, sel + 1)); return true;
            case KEY_PAGE_UP: select(std::max(0, sel - mScroll.fit)); return true;
            case KEY_PAGE_DOWN: select(std::min(int(mShown.size()) - 1, sel + mScroll.fit)); return true;
            case KEY_RETURN:
                if (sel >= 0)
                    run(mView == Groups ? "chat" : "im");
                return sel >= 0;
            default: return false;
        }
    }
    bool onFieldKey(Rml::Element*, KEY key, MASK mask) override
    {
        if (key == KEY_DOWN || key == KEY_UP)
            return onKey(key, mask); // from the filter straight into the list
        return false;
    }
    bool onReturn(Rml::Element*, MASK) override
    {
        if (selectedIndex() < 0 && !mShown.empty())
            select(0);
        return onKey(KEY_RETURN, MASK_NONE);
    }

    bool onTest(const LLSD& args) override
    {
        if (args.has("view"))
            setView(args["view"].asInteger());
        if (args.has("filter"))
        {
            mFilter = args["filter"].asString();
            dirty("filter");
        }
        if (args.has("select"))
        {
            collect();
            applyFilter();
            select(args["select"].asInteger());
        }
        if (args["menu"].asBoolean())
            openMenu(nullptr);
        return true;
    }

private:
    void setView(int v)
    {
        v = std::clamp(v, 0, int(ViewCount) - 1);
        if (v == mView)
            return;
        mView         = v;
        mNearby = mView == Nearby; dirty("nearby");
        mScroll.first = 0;
        mSelected.setNull();
        closeMenu();
        dirty("view");
        static const char* const hints[ViewCount] = { "Filter friends", "Filter people nearby", "Filter groups", "Filter recent people",
                                                       "Filter blocked" };
        mFilterHint = hints[v];
        dirty("filterHint");
        collect();
        applyFilter();
        fill();
    }

    // The current view's list from its model.
    void collect()
    {
        std::vector<Entry> out;
        switch (mView)
        {
            case Friends:
            {
                LLAvatarTracker::buddy_map_t buddies;
                LLAvatarTracker::instance().copyBuddyList(buddies);
                for (const auto& [id, rel] : buddies)
                {
                    Entry e;
                    e.id     = id;
                    e.name   = nameOf(id);
                    e.online = rel && rel->isOnline();
                    e.sub    = e.online ? "online" : "";
                    out.push_back(std::move(e));
                }
                std::sort(out.begin(), out.end(), [](const Entry& a, const Entry& b)
                          { return a.online != b.online ? a.online : LLStringUtil::compareDict(a.name, b.name) < 0; });
                break;
            }
            case Nearby:
            {
                if (!RlvActions::canShowNearbyAgents()) break;
                std::vector<LLSD> radarRows; LLSD radarStats;
                FSRadar::instance().getCurrentData(radarRows, radarStats);
                std::unordered_map<LLUUID, LLSD> radar;
                for (const LLSD& row : radarRows) radar.emplace(row["entry"]["id"].asUUID(), row["entry"]);
                uuid_vec_t              ids;
                std::vector<LLVector3d> positions;
                const LLVector3d        me    = gAgent.getPositionGlobal();
                if (mRangeLimited) LLWorld::getInstance()->getAvatars(&ids, &positions, me, mRange);
                else LLWorld::getInstance()->getAvatars(&ids, &positions);
                for (size_t i = 0; i < ids.size() && out.size() < 1024; ++i)
                {
                    if (ids[i] == gAgentID)
                        continue;
                    Entry e;
                    e.id     = ids[i];
                    const bool showName=RlvActions::canShowName(RlvActions::SNC_DEFAULT, ids[i]);
                    e.name = showName ? nameOf(ids[i]) : std::string("Resident");
                    // The radar snapshot can precede a live name-privacy change.
                    if (showName)
                        if (const auto it = radar.find(e.id); it != radar.end()) e.name = it->second["name"].asString();
                    e.dist   = F32(dist_vec(positions[i], me));
                    e.sub    = llformat("%.1f m", e.dist);
                    const auto flag = [&](bool& slot, bool on, const char* description)
                        { slot = slot || on; if (on) { e.statusTip += description; e.statusTip += " / "; } };
                    const auto* object = gObjectList.findObject(e.id);
                    const auto* avatar = object && object->isAvatar() ? static_cast<const LLVOAvatar*>(object) : nullptr;
                    flag(e.typing, avatar && avatar->isTyping(), "Typing");
                    if (LLVoiceClient::instanceExists())
                    {
                        auto* voice = LLVoiceClient::getInstance();
                        const bool speaking = voice->getIsSpeaking(e.id);
                        flag(e.speaking, speaking, "Speaking");
                        flag(e.voice, !speaking && voice->getVoiceEnabled(e.id), "Voice available");
                    }
                    flag(e.friendFlag, LLAvatarTracker::instance().isBuddy(e.id), "Friend");
                    flag(e.muted, LLMuteList::getInstance()->isMuted(e.id), "Muted");
                    const F32 sayRange = gAgent.getRegion() ? F32(gAgent.getRegion()->getChatRange()) : 20.f;
                    flag(e.chatRange, e.dist <= sayRange, "In normal chat range");
                    if (const auto it = radar.find(e.id); it != radar.end())
                    {
                        const LLSD& data = it->second;
                        flag(e.regionFlag, data["in_region"].asBoolean(), "Same region");
                        flag(e.parcelFlag, data["on_parcel"].asBoolean(), "Same parcel");
                        flag(e.sitting, data["sitting"].asBoolean(), "Sitting");
                        flag(e.payment, data["flags"].asInteger()==FSRADAR_PAYMENT_INFO_USED, "Payment information used");
                        flag(e.payment, data["flags"].asInteger()==FSRADAR_PAYMENT_INFO_FILLED, "Payment information on file");
                        const bool showDetails=showName && RlvActions::canShowName(RlvActions::SNC_DEFAULT);
                        flag(e.notes, showDetails && !data["notes"].asString().empty(), "Personal notes available");
                        if (showDetails && !data["age"].asString().empty()) e.statusTip += "Age (days): " + data["age"].asString() + " / ";
                        if (!data["seen"].asString().empty()) e.statusTip += "Seen: " + data["seen"].asString();
                    }
                    e.online = true;
                    out.push_back(std::move(e));
                }
                std::sort(out.begin(), out.end(), [](const Entry& a, const Entry& b) { return a.dist < b.dist; });
                break;
            }
            case Groups:
                for (const LLGroupData& g : gAgent.mGroups)
                {
                    Entry e;
                    e.id    = g.mID;
                    e.name  = g.mName;
                    e.group = true;
                    e.sub   = g.mID == gAgent.getGroupID() ? "active" : "";
                    out.push_back(std::move(e));
                }
                std::sort(out.begin(), out.end(), [](const Entry& a, const Entry& b) { return LLStringUtil::compareDict(a.name, b.name) < 0; });
                break;
            case Recent:
            {
                uuid_vec_t ids;
                LLRecentPeople::instance().get(ids);
                for (const LLUUID& id : ids)
                {
                    Entry e;
                    e.id     = id;
                    e.name   = nameOf(id);
                    e.online = LLAvatarTracker::instance().isBuddyOnline(id);
                    out.push_back(std::move(e));
                }
                break;
            }
            case Blocked:
                for (const LLMute& mute : LLMuteList::getInstance()->getMutes())
                {
                    Entry e;
                    e.id   = mute.mID;
                    e.name = mute.mName;
                    e.sub  = mute.getDisplayType();
                    e.mute = mute;
                    out.push_back(std::move(e));
                }
                break;
            default: break;
        }
        for (Entry& e : out)
            e.folded = fold(e.name);
        mAll.swap(out);
        applyFilter();
    }

    void applyFilter()
    {
        const std::string f = fold(mFilter);
        mShown.clear();
        for (size_t i = 0; i < mAll.size(); ++i)
            if (f.empty() || mAll[i].folded.find(f) != std::string::npos)
                mShown.push_back(i);
    }

    int selectedIndex() const
    {
        for (size_t i = 0; i < mShown.size(); ++i)
            if (mAll[mShown[i]].id == mSelected && mSelected.notNull())
                return int(i);
        return -1;
    }

    const Entry* selectedEntry() const
    {
        const int i = selectedIndex();
        return i < 0 ? nullptr : &mAll[mShown[size_t(i)]];
    }

    void select(int i)
    {
        if (i < 0 || i >= int(mShown.size()))
            return;
        mSelected = mAll[mShown[size_t(i)]].id;
        mScroll.reveal(i);
        fill();
    }

    // The visible window of the filtered list into the row slots; the actions for the selection.
    void fill()
    {
        if (mScroll.update(int(mShown.size()), mScroll.fit))
        {
            dirty("thumbTop");
            dirty("thumbH");
        }
        for (int i = 0; i < kPool; ++i)
        {
            Row&       r   = mRows[size_t(i)];
            const int  idx = mScroll.first + i;
            const bool use = i < mScroll.fit && idx < mScroll.count;
            r              = Row();
            r.used         = use;
            if (!use)
                continue;
            const Entry& e = mAll[mShown[size_t(idx)]];
            r.name         = e.name;
            r.sub          = e.sub;
            r.status = e.status; r.statusTip = e.statusTip;
            r.typing = e.typing;
            r.speaking = e.speaking;
            r.voice = e.voice;
            r.friendFlag = e.friendFlag;
            r.muted = e.muted;
            r.chatRange = e.chatRange;
            r.regionFlag = e.regionFlag;
            r.parcelFlag = e.parcelFlag;
            r.sitting = e.sitting;
            r.payment = e.payment;
            r.notes = e.notes;

            r.online       = e.online;
            r.group        = e.group;
            r.selected     = e.id == mSelected && mSelected.notNull();
        }
        dirty("rows");
        dirty("count");
        static const char* const units[ViewCount] = { "friends", "nearby", "groups", "recent", "blocked" };
        mCountText = mFilter.empty() ? llformat("%d %s", mScroll.count, units[mView])
                                     : llformat("%d of %d %s", mScroll.count, int(mAll.size()), units[mView]);
        if (mView == Friends)
        {
            const int online = int(std::count_if(mAll.begin(), mAll.end(), [](const Entry& e) { return e.online; }));
            mCountText += llformat(", %d online", online);
        }
        dirty("countText");
        static const char* const empty[ViewCount] = { "No friends yet. Right-click someone nearby to add them.", "Nobody nearby.",
                                                      "You are in no groups.", "Nobody you met recently.", "Nobody is blocked." };
        mEmptyText = mFilter.empty() ? empty[mView] : "No matches.";
        dirty("emptyText");
        updateActions();
    }

    void updateActions()
    {
        const Entry* e   = selectedEntry();
        const bool   sel = e != nullptr;
        std::vector<LLNewUIItem> acts;
        if (mView == Groups)
            acts = { { "Chat", "chat", sel }, { "Info", "info", sel }, { "Activate", "activate", sel && e->id != gAgent.getGroupID() } };
        else if (mView == Blocked)
            acts = { { "Unblock", "unblock", sel }, { "Profile", "profile", sel && e->mute.mType == LLMute::AGENT } };
        else
            acts = { { "IM", "im", sel },
                     { "Profile", "profile", sel },
                     { mView == Nearby ? "Zoom in" : "Teleport", mView == Nearby ? "zoom" : "offer_tp",
                       sel && (mView == Nearby ? SLAvatarActions::canZoomIn(e->id) : LLAvatarActions::canOfferTeleport(e->id)) },
                     { "Pay", "pay", sel } };
        if (acts != mActs)
        {
            mActs = acts;
            dirty("acts");
        }
    }

    // The full set for the selection, as the classic People panel's context menu.
    std::vector<LLNewUIItem> menuItems() const
    {
        const Entry* e = selectedEntry();
        if (mView == Groups)
        {
            const bool sel = e != nullptr;
            return { { "Chat", "chat", sel },
                     { "Group info", "info", sel },
                     { "Call", "call", sel && LLAvatarActions::canCall() },
                     { "Activate", "activate", sel && e->id != gAgent.getGroupID() },
                     { "Leave group", "leave", sel },
                     { "Create a group", "create", true },
                     { "Search groups", "search", true } };
        }
        if (!e)
            return {};
        if (mView == Blocked)
            return { { "Unblock", "unblock", true }, { "Profile", "profile", e->mute.mType == LLMute::AGENT } };
        const bool friendOf = LLAvatarActions::isFriend(e->id);
        std::vector<LLNewUIItem> items = { { "IM", "im", true },
                 { "Profile", "profile", true },
                 { "Offer teleport", "offer_tp", LLAvatarActions::canOfferTeleport(e->id) },
                 { "Request teleport", "request_tp", true },
                 { "Call", "call", LLAvatarActions::canCall() },
                 { "Pay", "pay", true },
                 { "Show on map", "map", LLAvatarActions::isAgentMappable(e->id) },
                 { "Share", "share", true },
                 { "Invite to group", "invite", true },
                 { friendOf ? "Remove friend" : "Add friend", friendOf ? "unfriend" : "friend", true },
                 { LLAvatarActions::isBlocked(e->id) ? "Unblock" : "Block", "block", LLAvatarActions::canBlock(e->id) } };
        if (mView == Nearby)
        {
            items.insert(items.begin(), { "Zoom in", "zoom", SLAvatarActions::canZoomIn(e->id) });
            const bool named=RlvActions::canShowName(RlvActions::SNC_DEFAULT,e->id);
            const std::vector<LLNewUIItem> nearbyActions = {
                { "Track on map", "track", true },
                { "Face towards avatar", "face", FSAvatarAlignBase::canFaceAvatarID(e->id) },
                { "Teleport to", "teleport_to", SLAvatarActions::canTeleportTo(e->id) },
                { "View chat history", "history", LLLogChat::isTranscriptExist(e->id) },
                { "Add to contact set", "contact_set", named },
                { "Report abuse", "report", true },
                { "Copy full name", "copy_full", named },
                { "Copy display name", "copy_display", named },
                { "Copy agent name", "copy_username", named },
                { "Copy profile SLURL", "copy_slurl", true },
                { "Copy UUID", "copy_id", true },
                { "Render normally", "render_normal", true },
                { "Do not render", "render_never", true },
                { "Render fully", "render_always", true } };
            items.insert(items.end(),nearbyActions.begin(),nearbyActions.end());
            if (SLAvatarActions::canFreezeEject(e->id))
            {
                items.push_back({ "Freeze on parcel", "freeze", true });
                items.push_back({ "Eject from parcel", "eject", true });
            }
            if (SLAvatarActions::canManageAvatarsEstate(e->id))
            {
                items.push_back({ "Estate teleport home", "estate_home", true });
                items.push_back({ "Estate kick", "estate_kick", true });
                items.push_back({ "Estate ban", "estate_ban", true });
            }
        }
        return items;
    }

    void openMenu(const Rml::Event* event)
    {
        mMenu = menuItems();
        if (mMenu.empty())
            return;
        float x = 120, y = 100, w = 1000, h = 1000; // no pointer (tests): near the list's top
        if (event)
            pointerIn(*event, "pbody", x, y, w, h);
        llnewuiPlaceMenu(x, y, w, h, mMenu.size(), mMenuX, mMenuY);
        mMenuOpen = true;
        for (const char* v : { "menu", "menuOpen", "menuX", "menuY" })
            dirty(v);
    }
    void closeMenu()
    {
        if (!mMenuOpen)
            return;
        mMenuOpen = false;
        dirty("menuOpen");
    }

    void run(const std::string& id)
    {
        const Entry* e = selectedEntry();
        if (id == "create")
            LLGroupActions::createGroup();
        else if (id == "search")
            LLGroupActions::search();
        if (!e)
            return;
        const LLUUID who = e->id;
        if (mView == Groups)
        {
            if (id == "chat")
                LLGroupActions::startIM(who); // shows in the conversations window while it is on
            else if (id == "info")
                LLGroupActions::show(who);
            else if (id == "call")
                LLGroupActions::startCall(who);
            else if (id == "activate")
                LLGroupActions::activate(who);
            else if (id == "leave")
                LLGroupActions::leave(who); // asks first
            mNext = 0;
            return;
        }
        if (id == "unblock" && mView == Blocked)
        {
            LLMuteList::getInstance()->remove(e->mute);
            mNext = 0;
            return;
        }
        if (id == "im")
            LLAvatarActions::startIM(who); // shows in the conversations window while it is on
        else if (id == "profile")
            LLAvatarActions::showProfile(who);
        else if (id == "offer_tp")
            LLAvatarActions::offerTeleport(who);
        else if (id == "request_tp")
            LLAvatarActions::teleportRequest(who);
        else if (id == "call")
            LLAvatarActions::startCall(who);
        else if (id == "pay")
            LLAvatarActions::pay(who);
        else if (id == "map")
            LLAvatarActions::showOnMap(who);
        else if (id == "share")
            LLAvatarActions::share(who);
        else if (id == "invite")
            LLAvatarActions::inviteToGroup(who);
        else if (id == "friend")
            LLAvatarActions::requestFriendshipDialog(who);
        else if (id == "unfriend")
            LLAvatarActions::removeFriendDialog(who); // asks first
        else if (id == "block")
            LLAvatarActions::toggleBlock(who);
        else if (mView == Nearby)
        {
            if (id == "zoom" && SLAvatarActions::canZoomIn(who)) SLAvatarActions::zoomIn(who);
            else if (id == "track") FSRadar::getInstance()->startTracking(who);
            else if (id == "face" && FSAvatarAlignBase::canFaceAvatarID(who)) FSAvatarAlignBase::faceAvatarID(who);
            else if (id == "teleport_to" && SLAvatarActions::canTeleportTo(who)) SLAvatarActions::teleportTo(who);
            else if (id == "history" && LLLogChat::isTranscriptExist(who)) LLAvatarActions::viewChatHistory(who);
            else if (id == "contact_set" && RlvActions::canShowName(RlvActions::SNC_DEFAULT,who)) LLAvatarActions::addToContactSet(who);
            else if (id == "report") SLAvatarActions::reportAbuse(who);
            else if (id == "copy_full" && RlvActions::canShowName(RlvActions::SNC_DEFAULT,who)) SLAvatarActions::copyData(who,SLAvatarActions::E_DATA_COMPLETE_NAME);
            else if (id == "copy_display" && RlvActions::canShowName(RlvActions::SNC_DEFAULT,who)) SLAvatarActions::copyData(who,SLAvatarActions::E_DATA_DISPLAY_NAME);
            else if (id == "copy_username" && RlvActions::canShowName(RlvActions::SNC_DEFAULT,who)) SLAvatarActions::copyData(who,SLAvatarActions::E_DATA_ACCOUNT_NAME);
            else if (id == "copy_slurl") SLAvatarActions::copyData(who,SLAvatarActions::E_DATA_SLURL);
            else if (id == "copy_id") SLAvatarActions::copyData(who,SLAvatarActions::E_DATA_UUID);
            else if (id == "render_normal") FSFloaterRadarMenu::FSRadarMenu::setRenderMode({who},0);
            else if (id == "render_never") FSFloaterRadarMenu::FSRadarMenu::setRenderMode({who},1);
            else if (id == "render_always") FSFloaterRadarMenu::FSRadarMenu::setRenderMode({who},2);
            else if (id == "freeze" && SLAvatarActions::canFreezeEject(who)) SLAvatarActions::parcelFreeze(who);
            else if (id == "eject" && SLAvatarActions::canFreezeEject(who)) SLAvatarActions::parcelEject(who);
            else if (id == "estate_home" && SLAvatarActions::canManageAvatarsEstate(who)) SLAvatarActions::estateTeleportHome(who);
            else if (id == "estate_kick" && SLAvatarActions::canManageAvatarsEstate(who)) SLAvatarActions::estateKick(who);
            else if (id == "estate_ban" && SLAvatarActions::canManageAvatarsEstate(who)) SLAvatarActions::estateBan(who);
        }
        mNext = 0;
    }

    bool mMapFailed = false;
    std::unique_ptr<LLNetMap> mMap;
    std::vector<std::string> mViews;
    int                      mView = Friends;
    F32 mRange = 162.f;
    bool mNearby = false, mRangeLimited = false;
    std::string              mFilter, mLastFilter, mFilterHint = "Filter friends", mCountText, mEmptyText;
    std::vector<Row>         mRows;
    std::vector<Entry>       mAll;
    std::vector<size_t>      mShown;
    LLNewUIScroll            mScroll;
    LLUUID                   mSelected;
    std::vector<LLNewUIItem> mActs, mMenu;
    bool                     mMenuOpen = false;
    float                    mMenuX = 0, mMenuY = 0;
    F64                      mNext = 0;
};
} // namespace

void slNewUIRegisterPeople(LLNewUIWindows& windows)
{
    LLNewUIWindows::Kind k;
    k.kind  = "people";
    k.title = "People";
    k.place = { 12.f, 56.f, 380.f, 640.f }; // top left, under the bar
    k.minW  = 320.f;
    k.minH  = 460.f;
    k.make  = [](const std::string&) -> std::unique_ptr<LLNewUIWindow> { return std::make_unique<PeopleWindow>(); };
    windows.registerKind(k);
}
