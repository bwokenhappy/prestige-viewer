/**
 * @file slfloatervrbodymap.cpp
 * @brief #1592 VR body mapping window. See slfloatervrbodymap.h; the mapping is llvrbodymapping.h.
 */
#include "llviewerprecompiledheaders.h"

#include "slfloatervrbodymap.h"

#include "llavatarjoint.h"
#include "llbutton.h"
#include "llcheckboxctrl.h"
#include "llcombobox.h"
#include "llfilepicker.h"
#include "llfontgl.h"
#include "llnotificationsutil.h"
#include "llpanel.h"
#include "llrender2dutils.h"
#include "llsliderctrl.h"
#include "lltextbox.h"
#include "lluictrlfactory.h"
#include "llviewermenufile.h"   // LLFilePickerReplyThread
#include "llvoavatarself.h"
#include "llvrbodymapping.h"

#include <algorithm>

namespace sb = slbmap;

namespace
{
constexpr S32 ROW_H = 40;        // a row: big enough to hit with a laser pointer on the VR wall
constexpr S32 CTRL_H = 34;
constexpr S32 NAME_W = 200, JOINT_W = 250, STATE_W = 170, GAP = 8;

// The profile dropdown's entries (value: LLVRBodyMap::choose's).
struct Choice
{
    const char* value;
    const char* label;
};
const Choice kChoices[] = {
    {"auto", "Auto (detect)"},
    {"human", "Human"},
    {"pony_bipedal", "Open Pony: Bipedal"},
    {"pony_arms", "Open Pony: Arms only"},
    {"pony_head", "Open Pony: Head only"},
    {"custom", "Custom"},
};
const char* const kStateLabels[sb::S_COUNT] = {"Tracked", "Animation", "Off (hold)"};

// What a built-in does, in words (the status line under the profile): no bone names needed.
const char* presetHelp(const std::string& id)
{
    if (id == "pony_bipedal")
        return "The pony stands on its hind legs: your legs drive its hind legs, the controllers its forelegs as arms (the "
               "hooves in your hands), the headset its head and neck.";
    if (id == "pony_arms")
        return "The pony stays on four legs and its animations; the forelegs follow the controllers (the hooves in your "
               "hands, reaching from its shoulders), the head and neck the headset.";
    if (id == "pony_head") return "The body stays on its animations; only the head and neck follow the headset.";
    return "Your tracking drives the matching human joints.";
}

// The window's text size (XUI's SansSerifLarge): readable on the VR wall.
const LLFontGL* bigFont()
{
    static const LLFontGL* f = nullptr;
    if (!f) f = LLFontGL::getFontByName("SansSerifLarge");
    return f ? f : LLFontGL::getFontSansSerifBig();
}

LLSD bigItem(const std::string& value, const std::string& label)
{
    LLSD row;
    row["value"] = value;
    row["columns"][0]["value"] = label;
    row["columns"][0]["font"]["name"] = "SansSerifLarge";
    return row;
}

LLComboBox* makeCombo(const std::string& name, const LLRect& rect)
{
    LLComboBox::Params p = LLUICtrlFactory::getDefaultParams<LLComboBox>();
    p.name(name);
    p.rect(rect);
    p.follows.flags(FOLLOWS_LEFT | FOLLOWS_TOP);
    p.font(bigFont());
    p.drop_down_button.font(bigFont());
    p.combo_button.font(bigFont());
    p.max_chars(40);
    return LLUICtrlFactory::create<LLComboBox>(p);
}

const LLColor4 kTracked(0.35f, 0.95f, 0.45f, 1.f);
const LLColor4 kAnim(0.58f, 0.61f, 0.66f, 1.f);
const LLColor4 kOff(1.f, 0.70f, 0.20f, 1.f);
const LLColor4 kOther(0.30f, 0.32f, 0.36f, 1.f);
const LLColor4 kHighlight(1.f, 1.f, 1.f, 1.f);

LLColor4 stateColor(int role)
{
    const sb::Profile& p = LLVRBodyMap::profile();
    if (role < 0) return kOther;
    if (!p.enabled(role) && !sb::isFingers(role)) return kOther;
    switch (p.state[role])
    {
    case sb::S_TRACKED: return kTracked;
    case sb::S_OFF: return kOff;
    default: return kAnim;
    }
}
} // namespace

// ---- The preview: your avatar's skeleton as it is drawn now ------------------------------------
class SLVRBodyMapPreview : public LLView
{
public:
    SLVRBodyMapPreview(const LLView::Params& p, SLFloaterVRBodyMap* owner) : LLView(p), mOwner(owner) {}
    void draw() override;

private:
    struct Bone
    {
        LLJoint* joint;
        int role;
        bool own;
    };
    SLFloaterVRBodyMap* mOwner;
    std::vector<Bone> mBones;
    U32 mJointGen = 0;
    const LLVOAvatar* mFor = nullptr;
    void collect(LLVOAvatarSelf* av);
};

void SLVRBodyMapPreview::collect(LLVOAvatarSelf* av)
{
    mBones.clear();
    for (LLAvatarJoint* j : av->getSkeleton())
    {
        if (!j || !j->getParent()) continue;
        const std::string& n = j->getName();
        const std::string& pn = j->getParent()->getName();
        // Body bones only: the face's many small bones would hide the head's mapping.
        if (n.size() < 2 || n[0] != 'm' || pn.empty() || pn[0] != 'm' || n.compare(0, 5, "mFace") == 0 || pn == "mRoot") continue;
        bool own = false;
        const int role = LLVRBodyMap::roleOf(j, &own);
        mBones.push_back({j, role, own});
    }
    mJointGen = LLVRBodyMap::jointGeneration();
    mFor = av;
}

void SLVRBodyMapPreview::draw()
{
    const LLRect r = getLocalRect();
    gl_rect_2d(r, LLColor4(0.05f, 0.06f, 0.08f, 0.95f));
    const LLFontGL* font = bigFont();
    if (!isAgentAvatarValid())
    {
        font->renderUTF8(std::string("No avatar"), 0, r.getWidth() / 2, r.getHeight() / 2, kAnim, LLFontGL::HCENTER, LLFontGL::VCENTER);
        return;
    }
    LLVOAvatarSelf* av = gAgentAvatarp.get();
    if (av != mFor || mJointGen != LLVRBodyMap::jointGeneration()) collect(av);
    LLJoint* root = av->getRootJoint();
    if (!root || mBones.empty()) return;
    const LLVector3 rootP = root->getWorldPosition();
    const LLQuaternion inv = ~root->getWorldRotation();
    // Joint positions in the avatar's frame (X forward, Y left, Z up), and the bounds.
    std::vector<LLVector3> pos(mBones.size()), ppos(mBones.size());
    LLVector3 lo(1e9f, 1e9f, 1e9f), hi(-1e9f, -1e9f, -1e9f);
    for (size_t i = 0; i < mBones.size(); ++i)
    {
        pos[i] = (mBones[i].joint->getWorldPosition() - rootP) * inv;
        ppos[i] = (mBones[i].joint->getParent()->getWorldPosition() - rootP) * inv;
        for (const LLVector3& v : {pos[i], ppos[i]})
            for (int k = 0; k < 3; ++k)
            {
                lo.mV[k] = llmin(lo.mV[k], v.mV[k]);
                hi.mV[k] = llmax(hi.mV[k], v.mV[k]);
            }
    }
    const S32 top = r.getHeight() - 36, bottom = 44;
    const S32 half = (r.getWidth() - 30) / 2;
    const F32 spanH = llmax(llmax(hi.mV[VX] - lo.mV[VX], hi.mV[VY] - lo.mV[VY]), 0.1f);
    const F32 spanV = llmax(hi.mV[VZ] - lo.mV[VZ], 0.1f);
    const F32 scale = 0.92f * llmin((F32)half / spanH, (F32)(top - bottom) / spanV);
    const F32 midX = 0.5f * (lo.mV[VX] + hi.mV[VX]), midY = 0.5f * (lo.mV[VY] + hi.mV[VY]);
    const S32 frontC = 10 + half / 2, sideC = 20 + half + half / 2;
    // Front: facing you (the avatar's left on your right). Side: from its right (forward to the right).
    auto project = [&](bool sideView, const LLVector3& v, S32& x, S32& y) {
        x = sideView ? sideC + ll_round((v.mV[VX] - midX) * scale) : frontC + ll_round((v.mV[VY] - midY) * scale);
        y = bottom + ll_round((v.mV[VZ] - lo.mV[VZ]) * scale);
    };
    font->renderUTF8(std::string("Front"), 0, frontC, r.getHeight() - 8, kAnim, LLFontGL::HCENTER, LLFontGL::TOP);
    font->renderUTF8(std::string("Side"), 0, sideC, r.getHeight() - 8, kAnim, LLFontGL::HCENTER, LLFontGL::TOP);
    gl_line_2d(half + 15, bottom, half + 15, top, kOther);

    const int hl = mOwner ? mOwner->highlightRole() : -1;
    // Unmapped bones first, the highlighted role last (on top).
    for (int pass = 0; pass < 3; ++pass)
    {
        for (size_t i = 0; i < mBones.size(); ++i)
        {
            const Bone& b = mBones[i];
            const bool isHl = hl >= 0 && b.role == hl;
            const int want = isHl ? 2 : (b.role >= 0 ? 1 : 0);
            if (want != pass) continue;
            const LLColor4 c = isHl ? kHighlight : stateColor(b.role);
            const S32 thick = isHl ? 3 : (b.role >= 0 ? 2 : 1);
            for (int view = 0; view < 2; ++view)
            {
                S32 x1, y1, x2, y2;
                project(view != 0, ppos[i], x1, y1);
                project(view != 0, pos[i], x2, y2);
                for (S32 t = 0; t < thick; ++t)
                {
                    gl_line_2d(x1 + t, y1, x2 + t, y2, c);
                    gl_line_2d(x1, y1 + t, x2, y2 + t, c);
                }
                if (b.own && b.role >= 0)
                {
                    const S32 d = isHl ? 7 : 4;
                    gl_rect_2d(x2 - d, y2 + d, x2 + d, y2 - d, c, true);
                }
            }
        }
    }
    // The highlighted role's name and joint, and the legend.
    if (hl >= 0)
    {
        const sb::Profile& p = LLVRBodyMap::profile();
        const std::string joint = sb::isFingers(hl) ? std::string("Bento fingers") : (p.joint[hl].empty() ? std::string("(none)") : p.joint[hl]);
        font->renderUTF8(std::string(sb::roleInfo(hl).label) + ": " + joint, 0, r.getWidth() / 2, r.getHeight() - 30, kHighlight,
                         LLFontGL::HCENTER, LLFontGL::TOP);
    }
    S32 lx = 12;
    for (int s = 0; s < sb::S_COUNT; ++s)
    {
        const LLColor4 c = s == sb::S_TRACKED ? kTracked : (s == sb::S_OFF ? kOff : kAnim);
        gl_rect_2d(lx, 30, lx + 14, 16, c, true);
        lx += 20;
        F32 right = 0.f;
        font->renderUTF8(std::string(kStateLabels[s]), 0, (F32)lx, 23.f, c, LLFontGL::LEFT, LLFontGL::VCENTER, LLFontGL::NORMAL,
                         LLFontGL::NO_SHADOW, S32_MAX, S32_MAX, &right);
        lx = (S32)right + 18;
    }
}

// ---- The window ---------------------------------------------------------------------------------
SLFloaterVRBodyMap::SLFloaterVRBodyMap(const LLSD& key) : LLFloater(key) {}

bool SLFloaterVRBodyMap::postBuild()
{
    mProfile = getChild<LLComboBox>("profile");
    for (const Choice& c : kChoices) mProfile->addElement(bigItem(c.value, c.label));
    mProfile->setCommitCallback([this](LLUICtrl*, const LLSD&) { onProfile(); });
    mStatus = getChild<LLTextBox>("status");
    mUpright = getChild<LLCheckBoxCtrl>("upright");
    mUpright->setCommitCallback([this](LLUICtrl*, const LLSD&) { onUpright(); });
    mRolesPanel = getChild<LLPanel>("roles_panel");
    mRolesScroll = getChild<LLView>("roles_scroll");
    getChild<LLButton>("reset")->setCommitCallback([this](LLUICtrl*, const LLSD&) { onReset(); });
    getChild<LLButton>("import")->setCommitCallback([this](LLUICtrl*, const LLSD&) { onImport(); });
    getChild<LLButton>("export")->setCommitCallback([this](LLUICtrl*, const LLSD&) { onExport(); });

    // The preview fills its placeholder.
    LLView* area = getChild<LLView>("preview_area");
    LLView::Params vp;
    vp.name("preview");
    vp.rect(area->getLocalRect());
    vp.follows.flags(FOLLOWS_ALL);
    vp.mouse_opaque(false);
    mPreview = new SLVRBodyMapPreview(vp, this);
    area->addChild(mPreview);

    buildRows();
    return true;
}

void SLFloaterVRBodyMap::buildRows()
{
    const S32 rowsH = ROW_H * sb::R_COUNT + 8;
    LLRect pr = mRolesPanel->getRect();
    pr.mBottom = pr.mTop - rowsH;
    mRolesPanel->setRect(pr);
    for (int r = 0; r < sb::R_COUNT; ++r)
    {
        const S32 top = rowsH - 4 - r * ROW_H;
        S32 x = 4;
        LLButton::Params bp = LLUICtrlFactory::getDefaultParams<LLButton>();
        bp.name(std::string("role_") + sb::roleInfo(r).key);
        bp.label(sb::roleInfo(r).label);
        bp.font(bigFont());
        bp.rect(LLRect(x, top, x + NAME_W, top - CTRL_H));
        bp.follows.flags(FOLLOWS_LEFT | FOLLOWS_TOP);
        bp.font_halign(LLFontGL::LEFT);
        bp.pad_left(10);
        bp.tool_tip("Select this role: the preview highlights the bones it drives");
        mRows[r].name = LLUICtrlFactory::create<LLButton>(bp);
        mRows[r].name->setCommitCallback([this, r](LLUICtrl*, const LLSD&) { select(r); });
        mRolesPanel->addChild(mRows[r].name);
        x += NAME_W + GAP;

        mRows[r].joint = makeCombo(std::string("joint_") + sb::roleInfo(r).key, LLRect(x, top, x + JOINT_W, top - CTRL_H));
        mRows[r].joint->setToolTip(sb::isFingers(r) ? std::string("Finger tracking drives the Bento fingers of this hand, or nothing")
                                   : sb::isHoof(r)  ? std::string("The bone past the hand that ends the limb (the Open Pony's hoof, "
                                                                  "mHandRing1): it takes the controller's place and turn, "
                                                                  "as your hand at the wrist. None for a human hand")
                                                    : std::string("The avatar joint this role drives (none: the role is off)"));
        mRows[r].joint->setCommitCallback([this, r](LLUICtrl*, const LLSD&) { onRoleChanged(r); });
        mRolesPanel->addChild(mRows[r].joint);
        x += JOINT_W + GAP;

        mRows[r].state = makeCombo(std::string("state_") + sb::roleInfo(r).key, LLRect(x, top, x + STATE_W, top - CTRL_H));
        for (int s = 0; s < sb::S_COUNT; ++s) mRows[r].state->addElement(bigItem(sb::stateKey(s), kStateLabels[s]));
        mRows[r].state->setToolTip(LLStringExplicit("Tracked: your tracking drives it. Animation: the animation keeps it. Off: held "
                                                    "still in the profile's base pose (no tracking, no animation)"));
        mRows[r].state->setCommitCallback([this, r](LLUICtrl*, const LLSD&) { onRoleChanged(r); });
        mRolesPanel->addChild(mRows[r].state);
    }

    // Weights per part: three columns.
    LLPanel* wp = getChild<LLPanel>("weights_panel");
    const S32 colW = wp->getRect().getWidth() / 3;
    for (int p = 0; p < sb::P_COUNT; ++p)
    {
        const S32 col = p % 3, row = p / 3;
        const S32 top = wp->getRect().getHeight() - row * ROW_H;
        LLSliderCtrl::Params sp = LLUICtrlFactory::getDefaultParams<LLSliderCtrl>();
        sp.name(std::string("weight_") + sb::partKey(p));
        sp.label(sb::partLabel(p));
        sp.label_width(150);
        sp.text_width(50);
        sp.font(bigFont());
        sp.rect(LLRect(col * colW + 4, top, (col + 1) * colW - 8, top - 30));
        sp.follows.flags(FOLLOWS_LEFT | FOLLOWS_TOP);
        sp.min_value(0.f);
        sp.max_value(1.f);
        sp.increment(0.05f);
        sp.decimal_digits(2);
        sp.initial_value(1.f);
        sp.tool_tip("How much of this part the body layer drives (0: all animation)");
        mWeights[p] = LLUICtrlFactory::create<LLSliderCtrl>(sp);
        mWeights[p]->setCommitCallback([this, p](LLUICtrl*, const LLSD&) { onWeight(p); });
        wp->addChild(mWeights[p]);
    }
}

void SLFloaterVRBodyMap::fillJointLists()
{
    mJointNames.clear();
    if (isAgentAvatarValid())
    {
        for (LLAvatarJoint* j : gAgentAvatarp->getSkeleton())
        {
            if (!j) continue;
            const std::string& n = j->getName();
            if (n.size() < 2 || n[0] != 'm' || n == "mRoot" || n.compare(0, 5, "mFace") == 0) continue;
            mJointNames.push_back(n);
        }
    }
    // The profile's own joints even when this skeleton lacks them (an imported map).
    const sb::Profile& p = LLVRBodyMap::profile();
    for (int r = 0; r < sb::R_COUNT; ++r)
        if (!sb::isFingers(r) && !p.joint[r].empty() && std::find(mJointNames.begin(), mJointNames.end(), p.joint[r]) == mJointNames.end())
            mJointNames.push_back(p.joint[r]);
    for (int r = 0; r < sb::R_COUNT; ++r)
    {
        LLComboBox* c = mRows[r].joint;
        if (!c) continue;
        c->removeall();
        c->addElement(bigItem("-", "(none)"));
        if (sb::isFingers(r))
        {
            c->addElement(bigItem("fingers", "Bento fingers"));
            continue;
        }
        for (const std::string& n : mJointNames) c->addElement(bigItem(n, n));
    }
}

void SLFloaterVRBodyMap::onOpen(const LLSD&)
{
    LLVRBodyMap::redetect();
    fillJointLists();
    refresh();
}

void SLFloaterVRBodyMap::refresh()
{
    mRefreshing = true;
    const sb::Profile& p = LLVRBodyMap::profile();
    mProfile->setValue(LLVRBodyMap::choice());
    for (int r = 0; r < sb::R_COUNT; ++r)
    {
        if (!mRows[r].joint) continue;
        mRows[r].joint->setValue(p.joint[r].empty() ? std::string("-") : p.joint[r]);
        mRows[r].state->setValue(sb::stateKey(p.state[r]));
        mRows[r].state->setEnabled(p.enabled(r));
    }
    for (int q = 0; q < sb::P_COUNT; ++q)
        if (mWeights[q]) mWeights[q]->setValue(p.weight[q]);
    // The stance: offered for pony profiles and bodies not detected as human.
    mUpright->setValue(p.upright);
    mUpright->setEnabled(p.upright || sb::isPonyProfile(p) || LLVRBodyMap::detected() != sb::D_HUMAN);
    std::string from = p.id;
    for (const Choice& c : kChoices)
        if (p.id == c.value) from = c.label;
    std::string text = presetHelp(p.id);
    if (LLVRBodyMap::choice() == "auto") text = "Auto picked " + from + ": " + text;
    if (p.custom) text = "Custom (from " + from + "), edited below.";
    if (p.upright && p.id != "pony_bipedal") text += " Stands on its hind legs.";
    mStatus->setText(text + "  Detected: " + LLVRBodyMap::detectReason() + ".");
    mShownGen = LLVRBodyMap::generation();
    mRefreshing = false;
}

void SLFloaterVRBodyMap::draw()
{
    if (mShownGen != LLVRBodyMap::generation()) refresh();
    // The row under the pointer (laser or mouse) highlights its bones in the preview.
    mHover = -1;
    if (mRolesPanel && mRolesScroll && mRolesPanel->isInVisibleChain())
    {
        S32 x = 0, y = 0;
        LLUI::getInstance()->getMousePositionLocal(mRolesPanel, &x, &y);
        S32 sx = 0, sy = 0;
        LLUI::getInstance()->getMousePositionLocal(mRolesScroll, &sx, &sy);
        if (mRolesScroll->getLocalRect().pointInRect(sx, sy) && x >= 0 && x < mRolesPanel->getRect().getWidth())
        {
            const S32 r = (mRolesPanel->getRect().getHeight() - 4 - y) / ROW_H;
            if (y >= 0 && r >= 0 && r < sb::R_COUNT) mHover = r;
        }
    }
    LLFloater::draw();
}

void SLFloaterVRBodyMap::select(int role)
{
    mSelected = role;
    for (int r = 0; r < sb::R_COUNT; ++r)
        if (mRows[r].name) mRows[r].name->setToggleState(r == role);
}

void SLFloaterVRBodyMap::applyEdit(const sb::Profile& p)
{
    LLVRBodyMap::edit(p);
    refresh();
}

void SLFloaterVRBodyMap::onProfile()
{
    if (mRefreshing) return;
    LLVRBodyMap::choose(mProfile->getValue().asString());
    fillJointLists();
    refresh();
}

void SLFloaterVRBodyMap::onRoleChanged(int role)
{
    if (mRefreshing || role < 0 || role >= sb::R_COUNT) return;
    select(role);
    sb::Profile p = LLVRBodyMap::profile();
    const std::string j = mRows[role].joint->getValue().asString();
    p.joint[role] = (j == "-" || j.empty()) ? std::string() : j;
    const std::string s = mRows[role].state->getValue().asString();
    for (int k = 0; k < sb::S_COUNT; ++k)
        if (s == sb::stateKey(k)) p.state[role] = k;
    applyEdit(p);
}

void SLFloaterVRBodyMap::onWeight(int part)
{
    if (mRefreshing || part < 0 || part >= sb::P_COUNT) return;
    sb::Profile p = LLVRBodyMap::profile();
    p.weight[part] = llclamp((F32)mWeights[part]->getValue().asReal(), 0.f, 1.f);
    applyEdit(p);
}

void SLFloaterVRBodyMap::onUpright()
{
    if (mRefreshing) return;
    sb::Profile p = LLVRBodyMap::profile();
    if (mUpright->getValue().asBoolean()) p.upright = true;   // (the mapping drives the hips, spine and legs: settle)
    else sb::onFourLegs(p);
    applyEdit(p);
}

void SLFloaterVRBodyMap::onReset()
{
    // Back to the built-in the profile came from (Auto stays Auto).
    const std::string c = LLVRBodyMap::choice();
    LLVRBodyMap::choose(c == "custom" ? LLVRBodyMap::profile().id : c);
    fillJointLists();
    refresh();
}

void SLFloaterVRBodyMap::onImport()
{
    LLHandle<LLFloater> h = getHandle();
    LLFilePickerReplyThread::startPicker(
        [h](const std::vector<std::string>& files, LLFilePicker::ELoadFilter, LLFilePicker::ESaveFilter)
        {
            if (files.empty()) return;
            std::string err;
            const bool ok = LLVRBodyMap::importFile(files.front(), err);
            if (!ok || !err.empty())
            {
                LLSD args;
                args["MESSAGE"] = ok ? "Body mapping imported: " + err : "Body mapping not imported: " + err;
                LLNotificationsUtil::add("GenericAlert", args);
            }
            if (SLFloaterVRBodyMap* f = dynamic_cast<SLFloaterVRBodyMap*>(h.get()))
            {
                f->fillJointLists();
                f->refresh();
            }
        },
        LLFilePicker::FFLOAD_ALL, false);
}

void SLFloaterVRBodyMap::onExport()
{
    LLFilePickerReplyThread::startPicker(
        [](const std::vector<std::string>& files, LLFilePicker::ELoadFilter, LLFilePicker::ESaveFilter)
        {
            if (files.empty()) return;
            std::string err;
            if (!LLVRBodyMap::exportFile(files.front(), err))
            {
                LLSD args;
                args["MESSAGE"] = "Body mapping not exported: " + err;
                LLNotificationsUtil::add("GenericAlert", args);
            }
        },
        LLFilePicker::FFSAVE_ALL, "body_mapping.txt");
}
