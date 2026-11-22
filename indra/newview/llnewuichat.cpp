// #703 The new interface's conversations window: nearby chat and the IM sessions as tabs, on the
// window framework (llnewuiwindow.h). Bound to the viewer's chat models directly:
//  - nearby chat: LLNearbyChatLog (fed where nearby chat arrives), sent through
//    LLFloaterIMNearbyChat::sendChatText (the chat bar's own path: channels, gestures, triggers,
//    OOC, chat commands, auto split; shout / whisper / OOC on their Enter keys);
//  - IM sessions: LLIMModel (messages, unread counts, the on-disk history tail), sent through
//    LLFloaterIMSession::composeIM / filterSendIM (the classic conversation's path: RLV, OOC,
//    poses, "/N" channels, auto split), queued until the session is initialised;
//  - torn off: a conversation in its own window (several at once), out of the main window's tabs.
// The parity list: doc/newui-windows.md.
#include "llviewerprecompiledheaders.h"

#include "llnewuiwindow.h"
#include "llnewuikitstyle.h"
#include "llnewuichatmarkup.h"
#include "llnewuihistoryscroll.h"
#include "llnewuiwindowdocs.h"
#include "llnewuiwindowkinds.h"

#include "llagent.h"
#include "llurlregistry.h"
#include "llurlaction.h"
#include "llcallingcard.h"
#include "llfloateravatarpicker.h"
#include "llavataractions.h"
#include "llavatarnamecache.h"
#include "llfloaterimnearbychat.h"
#include "llfloaterimsession.h"
#include "llgroupactions.h"
#include "llimview.h"
#include "lllogchat.h"
#include "llnearbychatlog.h"
#include "llviewercontrol.h"
#include "llworld.h"
#include "slkeycommands.h"

#include <RmlUi/Core/DataModelHandle.h>
#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ScrollTypes.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Event.h>
#include <RmlUi/Core/StringUtilities.h>
#include <algorithm>
#include <cctype>
#include <deque>
#include <boost/signals2/connection.hpp>

namespace
{
constexpr size_t kShown       = 80;  // messages materialised at once (the rest scroll in)
constexpr size_t kMaxTabs     = 64;
constexpr size_t kLineHistory = 32;  // sent lines Up / Down bring back
constexpr F64    kRefresh     = 0.25;

struct Tab
{
    std::string name;
    int         unread   = 0;
    bool        closable = false;
};
struct Line
{
    std::string from, text, time, rich, sender;
    LLUUID senderID;
    bool groupHead = true;
    bool        sys = false, hist = false, emote = false, self = false;
    bool sameMessage(const Line& other) const
    {
        return from==other.from && text==other.text && time==other.time && senderID==other.senderID
            && sys==other.sys && hist==other.hist && emote==other.emote && self==other.self;
    }
};
struct Action
{
    std::string label, id;
};

// What a conversation is: nearby chat (null session) or an IM session.
struct Conv
{
    LLUUID      session; // null: nearby
    std::string name;
};

class ChatWindow;
std::vector<ChatWindow*> gChats; // every open conversations window (torn-off ones included)

bool isTornOff(const LLUUID& session, const ChatWindow* except);

class ChatWindow final : public LLNewUIWindow, public LLIMSessionObserver
{
public:
    explicit ChatWindow(const std::string& a)
    {
        mSingle = !a.empty() && a != "main";
        if (mSingle)
        {
            Conv c;
            if (a != "nearby")
                c.session.set(a, false);
            mConvs.push_back(c);
        }
        mNearbyChanged=LLNearbyChatLog::instance().addChangedCallback([this]
            { if (current().isNull()) mMessagesPending=true; });
        mIMChanged=LLIMModel::instance().addNewMsgCallback([this](const LLSD& event)
            { if (event["session_id"].asUUID()==current()) mMessagesPending=true; });
        observeSessions();
        gChats.push_back(this);
    }
    ~ChatWindow() override
    {
        if (mObserving && LLIMMgr::instanceExists()) LLIMMgr::getInstance()->removeSessionObserver(this);
        mNearbyChanged.disconnect(); mIMChanged.disconnect(); mLifetime.reset();
        gChats.erase(std::remove(gChats.begin(), gChats.end(), this), gChats.end());
    }

    void observeSessions()
    {
        if (!mObserving && LLIMMgr::instanceExists())
        {
            LLIMMgr::getInstance()->addSessionObserver(this);
            mObserving=true;
        }
    }
    void sessionAdded(const LLUUID&, const std::string&, const LLUUID&, bool) override { mForce=true; }
    void sessionActivated(const LLUUID&, const std::string&, const LLUUID&) override {}
    void sessionVoiceOrIMStarted(const LLUUID&) override { mForce=true; }
    void sessionRemoved(const LLUUID&) override { mForce=true; }
    void sessionIDUpdated(const LLUUID& old_session_id, const LLUUID& new_session_id) override
    {
        bool rebound=false;
        for (auto& conversation:mConvs)
            if (conversation.session==old_session_id) { conversation.session=new_session_id; rebound=true; }
        for (auto& queued:mQueued)
            if (queued.first==old_session_id) { queued.first=new_session_id; rebound=true; }
        if (rebound) { mRevision=~uint64_t(0); mForce=true; mMessagesPending=true; }
    }

    const char* rml() const override { return LLNewUIWindowDocs::kChatRml; }
    const char* model() const override { return "chat"; }
    bool        single() const { return mSingle; }
    LLUUID      current() const { return mCurrent < mConvs.size() ? mConvs[mCurrent].session : LLUUID::null; }
    bool        shows(const LLUUID& s) const
    {
        return std::any_of(mConvs.begin(), mConvs.end(), [&](const Conv& c) { return c.session == s; });
    }

    bool bind(Rml::DataModelConstructor& m) override
    {
        if (auto t = m.RegisterStruct<Tab>())
        {
            t.RegisterMember("name", &Tab::name);
            t.RegisterMember("unread", &Tab::unread);
            t.RegisterMember("closable", &Tab::closable);
        }
        m.RegisterArray<std::vector<Tab>>();
        if (auto l = m.RegisterStruct<Line>())
        {
            l.RegisterMember("from", &Line::from);
            l.RegisterMember("text", &Line::text);
            l.RegisterMember("rich", &Line::rich);
            l.RegisterMember("sender", &Line::sender);
            l.RegisterMember("groupHead", &Line::groupHead);
            l.RegisterMember("time", &Line::time);
            l.RegisterMember("sys", &Line::sys);
            l.RegisterMember("hist", &Line::hist);
            l.RegisterMember("emote", &Line::emote);
            l.RegisterMember("self", &Line::self);
        }
        m.RegisterArray<std::vector<Line>>();
        if (auto a = m.RegisterStruct<Action>())
            a.RegisterMember("label", &Action::label);
        m.RegisterArray<std::vector<Action>>();
        m.RegisterArray<std::vector<std::string>>();
        m.Bind("grouped", &mGrouped);
        m.Bind("pickerOpen", &mPickerOpen);
        m.Bind("pickerQuery", &mPickerQuery);
        m.Bind("pickerLabels", &mPickerLabels);
        m.Bind("pickerStatuses", &mPickerStatuses);
        m.Bind("pickerFriendCount", &mPickerFriendCount);
        m.Bind("pickerSelected", &mPickerSelected);
        m.BindEventCallback("pickerWheel", [](Rml::DataModelHandle, Rml::Event& event, const Rml::VariantList&)
            { event.StopPropagation(); }); // retain normal popup scrolling, not chat-history scrolling
        m.BindEventCallback("pickerOutside", [this](Rml::DataModelHandle, Rml::Event& event, const Rml::VariantList&)
        {
            if (!mPickerOpen) return;
            for (auto* element = event.GetTargetElement(); element; element = element->GetParentNode())
                if (element->GetId() == "conversationPicker" || element->GetId() == "conversationAdd") return;
            mPickerOpen = false; dirty("pickerOpen"); focusCompose();
        });
        m.BindEventCallback("layout", [this](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList& a)
            { if (!a.empty()) { gSavedSettings.setU32("UINewChatLayout", a[0].Get<int>() == 0 ? 0u : 1u); mNext = 0; } });
        m.BindEventCallback("newConversation", [this](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&)
            { mPickerOpen = !mPickerOpen; if (mPickerOpen) refreshPicker(); else focusCompose(); dirty("pickerOpen"); mPickerNeedsFocus = mPickerOpen; mNext = 0; });
        m.BindEventCallback("chooseConversation", [this](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList& a)
            { if (!a.empty()) chooseConversation(size_t(a[0].Get<int>())); });
        m.BindEventCallback("searchResident", [this](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) { findResident(); });
        m.BindEventCallback("chatLink", [](Rml::DataModelHandle, Rml::Event& e, const Rml::VariantList&)
            { for (auto* t = e.GetTargetElement(); t; t = t->GetParentNode())
                { const auto url = t->GetAttribute<Rml::String>("chat-url", "");
                  if (!url.empty()) { LLUrlAction::clickAction(url, false); e.StopPropagation(); break; } } });
        m.Bind("tabs", &mTabs);
        m.Bind("current", &mCurrentInt);
        m.Bind("single", &mSingle);
        m.Bind("peer", &mPeer);
        m.Bind("acts", &mActs);
        m.Bind("msgs", &mLines);
        m.Bind("thumbTop", &mHistoryThumb.top);
        m.Bind("thumbH", &mHistoryThumb.height);
        m.Bind("older", &mOlder);
        m.Bind("olderText", &mOlderText);
        m.Bind("emptyText", &mEmptyText);
        m.Bind("stamps", &mStamps);
        m.Bind("typing", &mTyping);
        m.Bind("line", &mLine);
        m.Bind("hint", &mHint);
        m.Bind("suggest", &mSuggest);
        m.BindEventCallback("pick", [this](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList& a)
                            { if (!a.empty()) select(size_t(a[0].Get<int>())); });
        m.BindEventCallback("closeTab", [this](Rml::DataModelHandle, Rml::Event& e, const Rml::VariantList& a)
                            {
                                e.StopPropagation(); // not a pick of the tab under it
                                if (!a.empty()) closeConversation(size_t(a[0].Get<int>()));
                            });
        m.BindEventCallback("act", [this](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList& a)
                            { if (!a.empty() && size_t(a[0].Get<int>()) < mActs.size()) runAction(mActs[size_t(a[0].Get<int>())].id); });
        m.BindEventCallback("scroll", [this](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList& a)
                            {
                                if (a.empty()) return;
                                scrollBy(a[0].Get<float>() > 0 ? -3 : 3);
                            });
        m.BindEventCallback("historyPage", [this](Rml::DataModelHandle, Rml::Event& e, const Rml::VariantList&)
                            {
                                if (buttonOf(e) != 0 || e.GetTargetElement()->GetId() == "historyThumb") return;
                                float x, y, width, height;
                                if (pointerIn(e, "historyBar", x, y, width, height))
                                    scrollBy(y < height * mHistoryThumb.top / 100.f ? 10 : -10);
                            });
        m.BindEventCallback("send", [this](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) { sendLine(MASK_NONE); });
        m.BindEventCallback("complete", [this](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList& a)
                            { if (!a.empty()) complete(size_t(a[0].Get<int>())); });
        return true;
    }

    void reveal(const std::string& a) override
    {
        if (a.empty() || a == "main")
            return;
        LLUUID s;
        if (a != "nearby")
            s.set(a, false);
        rebuildTabs();
        for (size_t i = 0; i < mConvs.size(); ++i)
            if (mConvs[i].session == s)
                select(i);
    }

    bool sync(F64 now) override
    {
        // Framework deactivation covers clicks in other native/classic windows or the world.
        if (mPickerOpen && !active) { mPickerOpen = false; dirty("pickerOpen"); }
        observeSessions();
        // Initialization can finish without a new-message event. This queue is capped
        // at 32 and empty normally; release it at the next capture, not the poll.
        if (!mQueued.empty()) flushQueued();
        // Same model events as classic chat. Consume before the slower metadata poll,
        // so own echo and incoming lines reach the next UI capture without a timer.
        if (mMessagesPending)
        {
            mMessagesPending=false;
            refreshMessages(false);
        }
        updateHistoryThumb(); // layout/resize may change the visible message estimate

        if (mPickerNeedsFocus && mPickerOpen && doc)
            if (auto* field = doc->GetElementById("conversationSearch"))
            { if (field->IsVisible(true) && field->Focus()) mPickerNeedsFocus = false; }
        if (now < mNext && !mForce)
            return false;
        mNext  = now + kRefresh;
        mForce = false;
        static LLCachedControl<U32> layout(gSavedSettings, "UINewChatLayout", 0u);
        if (mGrouped != (U32(layout) != 0u))
        {
            mGrouped = U32(layout) != 0u;
            dirty("grouped");
            mRevision = ~uint64_t(0);
        }
        if (mPickerOpen && (mPickerQuery != mPickerPrevious || now >= mPickerNext))
        { refreshPicker(); mPickerNext = now + 1.0; }
        static LLCachedControl<bool> show_time(gSavedSettings, "IMShowTime", true);
        if (mStamps != bool(show_time))
        {
            mStamps = show_time;
            dirty("stamps");
        }
        if (!rebuildTabs() && mSingle && mConvs.empty())
        {
            LLNewUIWindows::instance().close(this); // its conversation ended
            return true;
        }
        refreshMessages(false);
        typingState();
        if (mSuggestStale)
            suggestNames(false);
        return false; // dirty() marks changes
    }

    bool wantsDrag(Rml::Element* hover) override
    {
        for (Rml::Element* e = hover; e && e != doc; e = e->GetParentNode())
            if (e->GetId() == "historyThumb")
            {
                mHistoryDragBack = mBack;
                mHistoryDragThumb = mHistoryThumb;
                Rml::Element* bar = doc->GetElementById("historyBar");
                mHistoryDragHeight = bar ? bar->GetClientHeight() / doc->GetContext()->GetDensityIndependentPixelRatio() : 0.f;
                return mHistoryDragHeight > 0.f;
            }
        return false;
    }

    void dragMove(S32, S32 dy) override
    {
        const size_t back = slwin::historyDragBack(mHistoryDragBack, dy, mHistoryDragHeight, mHistoryDragThumb, mHistoryTotal);
        if (back != mBack)
        {
            mBack = back;
            refreshMessages(true);
        }
    }

    bool onReturn(Rml::Element* field, MASK mask) override
    {
        if (field && field->GetId() == "conversationSearch") { chooseConversation(size_t(mPickerSelected)); return true; }
        sendLine(mask);
        return true;
    }

    bool onFieldKey(Rml::Element* field, KEY key, MASK mask) override
    {
        if (field && field->GetId() == "conversationSearch")
        {
            if (key == KEY_UP || key == KEY_DOWN)
            {
                const int count = int(mPickerLabels.size());
                if (count > 0) mPickerSelected = (mPickerSelected + (key == KEY_DOWN ? 1 : count - 1)) % count;
                dirty("pickerSelected");
                try
                {
                    if (doc)
                        if (auto* row = doc->GetElementById("conversationOption-" + std::to_string(mPickerSelected)))
                            row->ScrollIntoView({ Rml::ScrollAlignment::Nearest, Rml::ScrollAlignment::Nearest,
                                                  Rml::ScrollBehavior::Instant, Rml::ScrollParentage::Closest });
                }
                catch (const std::bad_alloc&) { /* Selection/Enter still work if scrolling cannot allocate. */ }
                return true;
            }
            if (key == KEY_ESCAPE) { mPickerOpen=false; dirty("pickerOpen"); focusCompose(); return true; }
            return false;
        }
        if (key == KEY_TAB && mask == MASK_NONE)
        {
            if (!mSuggest.empty())
            {
                complete(0);
                return true;
            }
            const std::string before = mLine;
            suggestNames(true);
            // Nothing to complete: Tab moves the focus on, as in any field.
            return mLine != before || !mSuggest.empty();
        }
        if (key == KEY_ESCAPE && !mSuggest.empty())
        {
            setSuggest({});
            return true;
        }
        if ((key == KEY_UP || key == KEY_DOWN) && mask == MASK_NONE && !mSent.empty())
        {
            // The lines sent before (as the chat bar's history).
            if (key == KEY_UP)
                mSentAt = mSentAt <= 0 ? int(mSent.size()) - 1 : mSentAt - 1;
            else
                mSentAt = mSentAt < 0 || mSentAt >= int(mSent.size()) - 1 ? -1 : mSentAt + 1;
            setLine(mSentAt < 0 ? std::string() : mSent[size_t(mSentAt)]);
            return true;
        }
        if ((key == KEY_PAGE_UP || key == KEY_PAGE_DOWN) && mask == MASK_NONE)
        {
            scrollBy(key == KEY_PAGE_UP ? 10 : -10);
            return true;
        }
        if (mask == MASK_ALT && (key == KEY_LEFT || key == KEY_RIGHT) && !mConvs.empty())
        {
            // Alt+Left / Right: the previous / next conversation (the classic container's keys).
            const size_t n = mConvs.size();
            select(key == KEY_LEFT ? (mCurrent + n - 1) % n : (mCurrent + 1) % n);
            return true;
        }
        if (!mSuggest.empty())
            mSuggestStale = true; // typing on: the candidates follow (next refresh)
        return false;
    }

    bool onKey(KEY key, MASK mask) override
    {
        if ((key == KEY_PAGE_UP || key == KEY_PAGE_DOWN) && mask == MASK_NONE)
        {
            scrollBy(key == KEY_PAGE_UP ? 10 : -10);
            return true;
        }
        return false;
    }

    bool onTest(const LLSD& args) override
    {
        rebuildTabs();
        if (args.has("tab"))
            select(size_t(std::max(0, args["tab"].asInteger())));
        if (args.has("line"))
        {
            setLine(args["line"].asString());
            suggestNames(false);
        }
        if (args["send"].asBoolean())
            sendLine(MASK_NONE);
        mForce = true;
        return true;
    }

    void onClosed() override
    {
        if (mTypingSent)
            setTyping(false);
    }

private:
    // ---- conversations ----
    // The tabs from the models: nearby first, then the IM sessions (in the model's order); torn-off
    // ones live in their own windows. False when this (torn-off) window's conversation is gone.
    bool rebuildTabs()
    {
        std::vector<Conv> convs;
        if (mSingle)
        {
            if (mConvs.empty())
                return false;
            Conv c = mConvs[0];
            if (c.session.notNull())
            {
                LLIMModel::LLIMSession* s = LLIMModel::instance().findIMSession(c.session);
                if (!s)
                {
                    mConvs.clear();
                    return false;
                }
                c.name = s->mName;
            }
            else
                c.name = "Nearby chat";
            convs.push_back(c);
        }
        else
        {
            if (!isTornOff(LLUUID::null, this))
                convs.push_back({ LLUUID::null, "Nearby chat" });
            for (const auto& [id, session] : LLIMModel::instance().mId2SessionMap)
                if (session && convs.size() < kMaxTabs && !isTornOff(id, this))
                    convs.push_back({ id, session->mName });
        }
        // Keep the current conversation selected across changes.
        const LLUUID cur   = current();
        const bool   had   = !mConvs.empty();
        bool         same  = convs.size() == mConvs.size();
        for (size_t i = 0; same && i < convs.size(); ++i)
            same = convs[i].session == mConvs[i].session && convs[i].name == mConvs[i].name;
        if (!same)
        {
            mConvs.swap(convs);
            size_t pick = 0;
            for (size_t i = 0; i < mConvs.size(); ++i)
                if (mConvs[i].session == cur)
                    pick = i;
            if (mCurrent != pick || !had)
            {
                mCurrent  = pick;
                mRevision = ~0ull;
                mBack     = 0;
            }
            mCurrentInt = int(mCurrent);
            dirty("current");
            updateHeader();
        }
        // Tabs and their unread counts (the current one is read while shown).
        std::vector<Tab> tabs;
        for (size_t i = 0; i < mConvs.size(); ++i)
        {
            Tab t;
            t.name     = mConvs[i].name;
            t.closable = mConvs[i].session.notNull();
            if (i != mCurrent)
            {
                if (mConvs[i].session.isNull())
                    t.unread = int(std::min<uint64_t>(99, LLNearbyChatLog::instance().revision() - mNearbySeen));
                else if (LLIMModel::LLIMSession* s = LLIMModel::instance().findIMSession(mConvs[i].session))
                    t.unread = std::min(99, s->mNumUnread);
            }
            tabs.push_back(t);
        }
        if (mConvs.empty() || mConvs[mCurrent].session.isNull())
            mNearbySeen = LLNearbyChatLog::instance().revision();
        bool tabsSame = tabs.size() == mTabs.size();
        for (size_t i = 0; tabsSame && i < tabs.size(); ++i)
            tabsSame = tabs[i].name == mTabs[i].name && tabs[i].unread == mTabs[i].unread && tabs[i].closable == mTabs[i].closable;
        if (!tabsSame)
        {
            mTabs.swap(tabs);
            dirty("tabs");
        }
        return true;
    }

    void select(size_t i)
    {
        if (i >= mConvs.size())
            return;
        if (i != mCurrent)
        {
            mCurrent    = i;
            mCurrentInt = int(i);
            mRevision   = ~0ull;
            mBack       = 0;
            dirty("current");
            setSuggest({});
            setLine(std::string());
        }
        updateHeader();
        mForce = true;
        if (doc)
            if (Rml::Element* line = doc->GetElementById("chatLine"))
                line->Focus();
    }

    void updateHeader()
    {
        const LLUUID s = current();
        std::vector<Action> acts;
        std::string         peer, hint;
        if (s.isNull())
        {
            peer = "Nearby chat";
            hint = "Say something nearby";
        }
        else if (LLIMModel::LLIMSession* session = LLIMModel::instance().findIMSession(s))
        {
            peer = session->mName;
            hint = "Message " + session->mName;
            if (session->isP2PSessionType())
            {
                acts.push_back({ "Profile", "profile" });
                acts.push_back({ "Offer teleport", "teleport" });
                acts.push_back({ "Pay", "pay" });
            }
            else if (session->isGroupSessionType())
                acts.push_back({ "Group info", "group" });
        }
        if (!mSingle)
            acts.push_back({ "Pop out", "popout" });
        else if (!s.isNull() || !isTornOff(LLUUID::null, this))
            acts.push_back({ "Dock", "dock" });
        if (peer != mPeer)
        {
            mPeer = peer;
            dirty("peer");
        }
        if (hint != mHint)
        {
            mHint = hint;
            dirty("hint");
        }
        mActs = acts;
        dirty("acts");
        if (mSingle)
            setTitle(peer);
    }

    void runAction(const std::string& id)
    {
        const LLUUID s = current();
        LLIMModel::LLIMSession* session = s.notNull() ? LLIMModel::instance().findIMSession(s) : nullptr;
        if (id == "profile" && session)
            LLAvatarActions::showProfile(session->mOtherParticipantID);
        else if (id == "teleport" && session)
            LLAvatarActions::offerTeleport(session->mOtherParticipantID);
        else if (id == "pay" && session)
            LLAvatarActions::pay(session->mOtherParticipantID);
        else if (id == "group" && session)
            LLGroupActions::show(session->mOtherParticipantID);
        else if (id == "popout")
            LLNewUIWindows::instance().open("chat", s.isNull() ? std::string("nearby") : s.asString(), true);
        else if (id == "dock")
        {
            // Back into the main window's tabs.
            const std::string a = s.isNull() ? std::string("nearby") : s.asString();
            mConvs.clear(); // no longer torn off: the main window takes it at its next refresh
            LLNewUIWindows::instance().close(this);
            LLNewUIWindows::instance().open("chat", a);
        }
    }

    void closeConversation(size_t i)
    {
        if (i >= mConvs.size() || mConvs[i].session.isNull())
            return;
        // Ends the session as closing its classic tab does (group snooze and all).
        if (gIMMgr)
            gIMMgr->leaveSession(mConvs[i].session);
        mForce = true;
    }

    // ---- messages ----
    void refreshMessages(bool force)
    {
        const LLUUID s = current();
        std::vector<Line> out;
        uint64_t          revision = 0;
        size_t            total    = 0;
        if (s.isNull())
        {
            const LLNearbyChatLog& log = LLNearbyChatLog::instance();
            revision                   = log.revision() * 1000 + mBack;
            if (!force && revision == mRevision)
                return;
            total             = log.size();
            const size_t end  = total - std::min(mBack, total);
            const size_t from = end > kShown ? end - kShown : 0;
            for (size_t i = from; i < end; ++i)
            {
                const LLNearbyChatLog::Line& l = log.at(i);
                Line m;
                m.time = l.time;
                m.sys  = l.source == CHAT_SOURCE_SYSTEM;
                m.self = l.fromID == gAgentID;
                m.senderID = l.fromID;
                setText(m, l.from, l.text);
                out.push_back(std::move(m));
            }
            mNearbySeen = log.revision();
        }
        else
        {
            LLIMModel::LLIMSession* session = LLIMModel::instance().findIMSession(s);
            if (!session)
                return;
            session->requestLogTail(); // the previous sessions' last messages (read off the main thread)
            const auto& tail = session->getLogTail().mMessages;
            const bool  tailIn = session->isLogTailLoaded();
            revision = (uint64_t(session->mMsgs.size()) * 2 + (tailIn ? 1 : 0)) * 1000 + mBack;
            if (!force && revision == mRevision)
            {
                markRead(session);
                return;
            }
            // The log tail, then this session's messages: one list, oldest first.
            const size_t tailN = tailIn ? tail.size() : 0;
            total              = tailN + session->mMsgs.size();
            const size_t end   = total - std::min(mBack, total);
            const size_t from  = end > kShown ? end - kShown : 0;
            for (size_t i = from; i < end; ++i)
            {
                Line m;
                if (i < tailN)
                {
                    const LLChatLogEntry& e = tail[i];
                    m.time = e.mTime;
                    m.hist = true;
                    m.sys  = !e.mHasFromID && e.mFrom.empty();
                    m.self = e.mFromID == gAgentID;
                    m.senderID = e.mFromID;
                    setText(m, e.mFrom, e.mText);
                }
                else
                {
                    const LLIMModel::LLIMMessage& e = session->mMsgs[i - tailN];
                    m.time = e.mTime;
                    m.hist = e.mIsHistory;
                    m.sys  = e.mFromID.isNull() || e.mIsRegionMsg;
                    m.self = e.mFromID == gAgentID;
                    m.senderID = e.mFromID;
                    setText(m, e.mFrom, e.mText);
                }
                out.push_back(std::move(m));
            }
            markRead(session);
        }
        const bool rebuildRich=force || mRevision==~uint64_t(0);
        mHistoryTotal = total;
        mRevision = revision;
        mBack     = std::min(mBack, total ? total - 1 : 0);
        size_t reuse=0;
        for (size_t i = 0; i < out.size(); ++i)
        {
            Line& line = out[i];
            // Appending and a rolling bounded tail preserve existing rich strings.
            // RmlUi's data-for retains row elements; unchanged data-rml retains its
            // children. Only new/changed lines need parsing or DOM replacement.
            while (!rebuildRich && reuse<mLines.size() && !line.sameMessage(mLines[reuse])) ++reuse;
            if (!rebuildRich && reuse<mLines.size()) line.rich=mLines[reuse++].rich;
            else line.rich=richText(line.text);
            line.groupHead = i == 0 || (line.senderID.notNull() && out[i-1].senderID.notNull()
                             ? line.senderID != out[i-1].senderID : line.sender != out[i-1].sender) || line.sys || line.emote
                             || out[i-1].sys || out[i-1].emote || line.hist != out[i-1].hist;
        }
        mLines.swap(out);
        dirty("msgs");
        updateHistoryThumb();
        const bool older = total > mLines.size() + mBack;
        const std::string olderText =
            mBack ? llformat("%u newer below - scroll down", unsigned(mBack)) : llformat("%u earlier - scroll up", unsigned(total - mLines.size()));
        if (older != mOlder || olderText != mOlderText)
        {
            mOlder     = older || mBack > 0;
            mOlderText = olderText;
            dirty("older");
            dirty("olderText");
        }
        const std::string empty = s.isNull() ? "Nobody has said anything nearby yet." : "No messages yet.";
        if (empty != mEmptyText)
        {
            mEmptyText = empty;
            dirty("emptyText");
        }
    }

    std::string richText(const std::string& text)
    {
        const std::weak_ptr<char> life = mLifetime;
        return slnewui::chatMarkup(text, [this, life](const std::string&, const std::string&, const std::string&)
            { if (life.lock()) { mRevision = ~uint64_t(0); mForce = true; } });
    }

    void focusCompose()
    {
        if (active && doc)
            if (auto* field = doc->GetElementById("chatLine"); field && field->IsVisible(true)) field->Focus();
    }

    void refreshPicker()
    {
        try
        {
            // Publish all parallel arrays together. An OOM retains the previous usable list.
            std::string previous = mPickerQuery, query = mPickerQuery;
            LLStringUtil::toLower(query);
            std::vector<std::string> labels, statuses;
            uuid_vec_t ids;
            std::vector<bool> groups;
            labels.reserve(32); statuses.reserve(32); ids.reserve(32); groups.reserve(32);
            size_t friendCount = 0;
            const auto add = [&](const LLUUID& id, const std::string& name, bool group, const std::string& status)
            {
                std::string lower = name; LLStringUtil::toLower(lower);
                const size_t count = group ? ids.size() - friendCount : ids.size();
                if (count < 16 && (query.empty() || lower.find(query) != std::string::npos))
                { labels.push_back(name); statuses.push_back(status); ids.push_back(id); groups.push_back(group); }
            };
            LLAvatarTracker::buddy_map_t friends;
            LLAvatarTracker::instance().copyBuddyList(friends);
            for (const auto& pair : friends)
            {
                LLAvatarName name;
                if (LLAvatarNameCache::get(pair.first, &name))
                    add(pair.first, name.getCompleteName(), false, pair.second->isOnline() ? "Online" : "Offline");
            }
            friendCount = labels.size();
            for (const auto& group : gAgent.mGroups) add(group.mID, group.mName, true, "Group");
            mPickerPrevious.swap(previous);
            mPickerLabels.swap(labels); mPickerStatuses.swap(statuses);
            mPickerIds.swap(ids); mPickerGroups.swap(groups);
            mPickerFriendCount = int(friendCount); mPickerSelected = 0;
            if (doc) if (auto* picker = doc->GetElementById("conversationPicker")) picker->SetScrollTop(0);
            dirty("pickerFriendCount"); dirty("pickerSelected"); dirty("pickerStatuses"); dirty("pickerLabels");
        }
        catch (const std::bad_alloc&) { /* Retain the last complete list; retry next refresh. */ }
    }
    void findResident()
    {
        if (auto* picker = LLFloaterAvatarPicker::show([](const uuid_vec_t& ids, const std::vector<LLAvatarName>&)
                { if (!ids.empty()) LLAvatarActions::startIM(ids.front()); }, false, true))
            picker->searchFor(mPickerQuery);
        mPickerOpen=false; dirty("pickerOpen");
    }
    void chooseConversation(size_t index)
    {
        if (index >= mPickerIds.size()) return;
        if (mPickerGroups[index]) LLGroupActions::startIM(mPickerIds[index]);
        else LLAvatarActions::startIM(mPickerIds[index]);
        mPickerOpen = false; mNext = 0; dirty("pickerOpen"); focusCompose();
    }

    // "/me waves" as an emote: the name, then the rest (as the classic history shows it).
    static void setText(Line& m, const std::string& from, const std::string& text)
    {
        m.sender = from;
        if (text.rfind("/me ", 0) == 0 || text.rfind("/me'", 0) == 0)
        {
            m.emote = true;
            m.from  = from;
            m.text  = text.substr(3);
        }
        else
        {
            m.from = from.empty() ? std::string() : from + ": ";
            m.text = text;
        }
    }

    void markRead(LLIMModel::LLIMSession* session)
    {
        // Read while it is the shown tab of a window that is not minimised (the classic rule).
        if (session && (session->mNumUnread || session->mParticipantUnreadMessageCount) && doc && doc->IsVisible() &&
            !doc->IsClassSet("min"))
            LLIMModel::instance().sendNoUnreadMessages(session->mSessionID);
    }

    void updateHistoryThumb()
    {
        // Estimate visible messages from this bounded batch's actual wrapped height.
        // Scrolling itself is in whole messages; no unbounded history DOM is built.
        float visible = float(visibleRows("history", 20.f, int(kShown)));
        Rml::Element* history = doc ? doc->GetElementById("history") : nullptr;
        Rml::Element* messages = doc ? doc->GetElementById("msgs") : nullptr;
        if (history && messages && !mLines.empty() && messages->GetClientHeight() > 0.f)
            visible = std::clamp(history->GetClientHeight() * float(mLines.size()) / messages->GetClientHeight(), 1.f, float(kShown));
        const auto thumb = slwin::historyThumb(mHistoryTotal, mBack, visible);
        if (thumb.top != mHistoryThumb.top || thumb.height != mHistoryThumb.height)
        {
            mHistoryThumb = thumb;
            dirty("thumbTop");
            dirty("thumbH");
        }
    }

    void scrollBy(int lines)
    {
        const double limit = mHistoryTotal ? double(mHistoryTotal - 1) : 0.0;
        const size_t back = size_t(std::clamp(double(mBack) + lines, 0.0, limit));
        if (back != mBack)
        {
            mBack = back;
            refreshMessages(true);
        }
    }

    // ---- the line ----
    void setLine(const std::string& text)
    {
        mLine = text;
        dirty("line");
    }

    void sendLine(MASK mask)
    {
        if (mLine.empty())
            return;
        const std::string text = mLine;
        const LLUUID      s    = current();
        if (s.isNull())
        {
            // The chat bar's Enter keys: shout, whisper, out of character (each optional).
            static LLCachedControl<bool> use_ctrl_shout(gSavedSettings, "FSUseCtrlShout", true);
            static LLCachedControl<bool> use_shift_whisper(gSavedSettings, "FSUseShiftWhisper", true);
            static LLCachedControl<bool> use_alt_ooc(gSavedSettings, "FSUseAltOOC", false);
            SLKeyCommands& keys = SLKeyCommands::instance();
            EChatType      type = CHAT_TYPE_NORMAL;
            bool           ooc  = false;
            if (mask != MASK_NONE && keys.matches("chat_shout", KEY_RETURN, mask))
                type = use_ctrl_shout ? CHAT_TYPE_SHOUT : CHAT_TYPE_NORMAL;
            else if (mask != MASK_NONE && keys.matches("chat_whisper", KEY_RETURN, mask))
                type = use_shift_whisper ? CHAT_TYPE_WHISPER : CHAT_TYPE_NORMAL;
            else if (mask != MASK_NONE && use_alt_ooc && keys.matches("chat_ooc", KEY_RETURN, mask))
                ooc = true;
            LLFloaterIMNearbyChat::sendChatText(utf8str_to_wstring(text), type, ooc);
            gAgent.stopTyping();
        }
        else
        {
            static LLCachedControl<bool> use_alt_ooc(gSavedSettings, "FSUseAltOOC", false);
            const bool ooc = mask != MASK_NONE && use_alt_ooc && SLKeyCommands::instance().matches("chat_ooc", KEY_RETURN, mask);
            if (LLIMModel::LLIMSession* session = LLIMModel::instance().findIMSession(s))
                for (const std::string& piece : LLFloaterIMSession::composeIM(utf8str_to_wstring(text), ooc, nullptr))
                {
                    const std::string filtered = LLFloaterIMSession::filterSendIM(s, session->mOtherParticipantID, piece);
                    if (session->mSessionInitialized)
                        LLIMModel::sendMessage(filtered, s, session->mOtherParticipantID, session->mType);
                    else if (mQueued.size() < 32)
                        mQueued.push_back({ s, filtered }); // sent once the session is up
                }
            setTyping(false);
        }
        mSent.push_back(text);
        if (mSent.size() > kLineHistory)
            mSent.pop_front();
        mSentAt = -1;
        mBack   = 0;
        setLine(std::string());
        setSuggest({});
        mForce = true;
    }

    void flushQueued()
    {
        for (auto it = mQueued.begin(); it != mQueued.end();)
        {
            LLIMModel::LLIMSession* session = LLIMModel::instance().findIMSession(it->first);
            if (!session)
                it = mQueued.erase(it);
            else if (session->mSessionInitialized)
            {
                LLIMModel::sendMessage(it->second, it->first, session->mOtherParticipantID, session->mType);
                it = mQueued.erase(it);
            }
            else
                ++it;
        }
    }

    // Typing: the typing animation nearby (as the chat bar), the typing notice in a one-to-one IM.
    void typingState()
    {
        const bool typing = !mLine.empty() && mLine[0] != '/';
        if (typing == mTypingSent)
            return;
        const LLUUID s = current();
        if (s.isNull())
        {
            static LLCachedControl<bool> anim(gSavedSettings, "PlayTypingAnim", true);
            if (typing && anim)
                gAgent.startTyping();
            else if (!typing)
                gAgent.stopTyping();
            mTypingSent = typing;
        }
        else
            setTyping(typing);
    }
    void setTyping(bool typing)
    {
        mTypingSent = typing;
        const LLUUID s = current();
        LLIMModel::LLIMSession* session = s.notNull() ? LLIMModel::instance().findIMSession(s) : nullptr;
        if (session && session->isP2PSessionType())
            LLIMModel::sendTypingState(s, session->mOtherParticipantID, typing);
    }

    // ---- name completion (Tab): avatars nearby, and the people in this conversation ----
    void setSuggest(const std::vector<std::string>& s)
    {
        if (s == mSuggest)
            return;
        mSuggest      = s;
        mSuggestStale = false;
        dirty("suggest");
    }

    static std::string fold(std::string s)
    {
        for (char& c : s)
            c = char(std::tolower(static_cast<unsigned char>(c)));
        return s;
    }

    std::string lastWord() const
    {
        const size_t at = mLine.find_last_of(' ');
        return at == std::string::npos ? mLine : mLine.substr(at + 1);
    }

    void suggestNames(bool completeSingle)
    {
        const std::string word = fold(lastWord());
        if (word.size() < 2)
        {
            setSuggest({});
            return;
        }
        uuid_vec_t ids;
        LLWorld::getInstance()->getAvatars(&ids, nullptr, gAgent.getPositionGlobal(), 96.f);
        if (LLIMModel::LLIMSession* session = LLIMModel::instance().findIMSession(current()))
            ids.push_back(session->mOtherParticipantID);
        std::vector<std::string> out;
        for (const LLUUID& id : ids)
        {
            LLAvatarName name;
            if (id == gAgentID || !LLAvatarNameCache::get(id, &name))
                continue;
            for (const std::string& n : { name.getDisplayName(), name.getUserName() })
                if (fold(n).rfind(word, 0) == 0 && std::find(out.begin(), out.end(), n) == out.end() && out.size() < 6)
                {
                    out.push_back(n);
                    break;
                }
        }
        if (completeSingle && out.size() == 1)
        {
            mSuggest = out;
            complete(0);
            return;
        }
        setSuggest(out);
    }

    void complete(size_t i)
    {
        if (i >= mSuggest.size())
            return;
        const size_t at = mLine.find_last_of(' ');
        setLine((at == std::string::npos ? std::string() : mLine.substr(0, at + 1)) + mSuggest[i] + " ");
        setSuggest({});
        if (doc)
            if (Rml::Element* line = doc->GetElementById("chatLine"))
                line->Focus();
    }

    bool                     mSingle = false;
    std::vector<Conv>        mConvs;
    size_t                   mCurrent = 0;
    int                      mCurrentInt = 0;
    std::vector<Tab>         mTabs;
    std::shared_ptr<char> mLifetime = std::make_shared<char>();
    boost::signals2::scoped_connection mNearbyChanged, mIMChanged;
    bool mMessagesPending=false, mObserving=false;
    bool mGrouped = false, mPickerOpen = false;
    std::string mPickerQuery, mPickerPrevious;
    std::vector<std::string> mPickerLabels, mPickerStatuses;
    int mPickerFriendCount = 0, mPickerSelected = 0;
    bool mPickerNeedsFocus = false;
    F64 mPickerNext = 0;
    uuid_vec_t mPickerIds;
    std::vector<bool> mPickerGroups;
    std::vector<Line>        mLines;
    std::vector<Action>      mActs;
    std::vector<std::string> mSuggest;
    bool                     mSuggestStale = false;
    std::string              mPeer, mOlderText, mEmptyText, mTyping, mLine, mHint;
    bool                     mOlder = false, mStamps = true, mForce = true, mTypingSent = false;
    slwin::HistoryThumb      mHistoryThumb, mHistoryDragThumb;
    size_t                   mHistoryTotal = 0, mHistoryDragBack = 0;
    float                    mHistoryDragHeight = 0.f;
    size_t                   mBack     = 0; // messages hidden below (scrolled up by this many)
    uint64_t                 mRevision = ~0ull, mNearbySeen = 0;
    F64                      mNext     = 0;
    std::deque<std::string>  mSent;
    int                      mSentAt = -1;
    std::vector<std::pair<LLUUID, std::string>> mQueued;
};

bool isTornOff(const LLUUID& session, const ChatWindow* except)
{
    return std::any_of(gChats.begin(), gChats.end(), [&](const ChatWindow* c) { return c != except && c->single() && c->shows(session); });
}

// Conversations started anywhere (People's IM, a profile's IM button, ...) show here while the
// native windows are on (llimview.h).
bool showConversationHere(const LLUUID& session)
{
    if (!LLNewUIWindows::instance().active())
        return false;
    for (ChatWindow* c : gChats)
        if (c->single() && c->shows(session))
        {
            LLNewUIWindows::instance().open("chat", session.asString());
            return true;
        }
    return LLNewUIWindows::instance().open("chat", session.isNull() ? std::string("nearby") : session.asString()) != nullptr;
}
} // namespace

void slNewUIRegisterChat(LLNewUIWindows& windows)
{
    LLNewUIWindows::Kind k;
    k.kind    = "chat";
    k.title   = "Conversations";
    k.place   = { 12.f, -12.f, 460.f, 420.f }; // bottom left
    k.minW    = 300.f;
    k.minH    = 220.f;
    k.multi   = true; // torn-off conversations
    k.restore = true;
    k.make    = [](const std::string& arg) -> std::unique_ptr<LLNewUIWindow> { return std::make_unique<ChatWindow>(arg); };
    windows.registerKind(k);
    gSLShowConversationHook = &showConversationHere;
}
