/**
 * @file llfloatervrsettings.cpp
 * @brief VR settings wall menu (#1593). See llfloatervrsettings.h.
 */
#include "llviewerprecompiledheaders.h"

#include "llfloatervrsettings.h"

#include "llbutton.h"
#include "llfloaterreg.h"
#include "llnotificationsutil.h"
#include "llpanel.h"
#include "lltabcontainer.h"
#include "lltextbox.h"
#include "llviewercontrol.h"
#include "llvrbody.h"
#include "vkgfx/llvrspacewarp.h"
#include "vkgfx/llxrsession.h"

#include <cmath>

namespace
{
// Layout, UI points. At the default VR UI scale (#1591: 720 points tall on a 110 degree wall)
// a point is ~0.083 degrees: a row is ~3.7 degrees tall, easy to hold the laser on.
constexpr S32 kRowH = 44;
constexpr S32 kRowGap = 10;
constexpr S32 kPad = 20;
constexpr S32 kLabelW = 380;
constexpr S32 kStepBtnW = 90;
constexpr S32 kStepValueW = 220;
constexpr S32 kToggleW = 220;
constexpr S32 kActionW = 340;

const LLFontGL* bigFont()
{
    return LLFontGL::getFontSansSerifBig();
}
} // namespace

LLFloaterVRSettings::LLFloaterVRSettings(const LLSD& key)
    : LLFloater(key)
{
}

std::string LLFloaterVRSettings::text(const std::string& key) const
{
    return hasString(key) ? getString(key) : key;
}

LLButton* LLFloaterVRSettings::makeButton(LLPanel* panel, const std::string& name, const std::string& label, const LLRect& r,
                                          bool toggle)
{
    LLButton::Params p;
    p.name(name);
    p.label(label);
    p.label_selected(label);
    p.font(bigFont());
    p.rect(r);
    p.is_toggle(toggle);
    p.follows.flags(FOLLOWS_LEFT | FOLLOWS_TOP);
    LLButton* b = LLUICtrlFactory::create<LLButton>(p);
    if (b) panel->addChild(b);
    return b;
}

S32 LLFloaterVRSettings::addLabel(LLPanel* panel, S32 top, const std::string& label_key)
{
    const S32 lineH = llmax(16, (S32)bigFont()->getLineHeight() + 4);
    const S32 y = top - (kRowH - lineH) / 2;
    LLTextBox::Params p;
    p.name(label_key);
    p.initial_value(text(label_key));
    p.font(bigFont());
    p.rect(LLRect(kPad, y, kPad + kLabelW, y - lineH));
    p.follows.flags(FOLLOWS_LEFT | FOLLOWS_TOP);
    if (LLTextBox* t = LLUICtrlFactory::create<LLTextBox>(p)) panel->addChild(t);
    return kPad + kLabelW + kPad;   // the controls' left edge
}

void LLFloaterVRSettings::addToggle(LLPanel* panel, S32& top, const char* setting, const std::string& label_key)
{
    const S32 x = addLabel(panel, top, label_key);
    Row row;
    row.kind = TOGGLE;
    row.control = gSavedSettings.getControl(setting).get();
    LLButton* b = makeButton(panel, std::string(setting), text("off"), LLRect(x, top, x + kToggleW, top - kRowH), true);
    if (b)
    {
        const size_t i = mRows.size();
        b->setClickedCallback([this, i](LLUICtrl*, const LLSD&) { onToggle(i); });
        row.buttons.push_back(b);
    }
    mRows.push_back(row);
    top -= kRowH + kRowGap;
}

void LLFloaterVRSettings::addChoice(LLPanel* panel, S32& top, const char* setting, const std::string& label_key,
                                    const std::vector<std::pair<std::string, std::string>>& values, std::function<bool()> enabled)
{
    const S32 x0 = addLabel(panel, top, label_key);
    Row row;
    row.kind = CHOICE;
    row.enabled = std::move(enabled);   // (greyed out while it cannot work, as the buttons of a setting this build lacks)
    row.control = gSavedSettings.getControl(setting).get();
    const S32 n = (S32)values.size();
    const S32 avail = panel->getRect().getWidth() - kPad - x0;
    const S32 w = n > 0 ? (avail - 8 * (n - 1)) / n : avail;
    const size_t i = mRows.size();
    for (S32 k = 0; k < n; ++k)
    {
        const S32 x = x0 + k * (w + 8);
        LLButton* b = makeButton(panel, std::string(setting) + "_" + values[k].first, text(values[k].second),
                                 LLRect(x, top, x + w, top - kRowH), true);
        if (!b) continue;
        const size_t which = (size_t)k;
        b->setClickedCallback([this, i, which](LLUICtrl*, const LLSD&) { onChoice(i, which); });
        row.buttons.push_back(b);
        row.values.push_back(values[k].first);
    }
    mRows.push_back(row);
    top -= kRowH + kRowGap;
}

void LLFloaterVRSettings::addStepper(LLPanel* panel, S32& top, const char* setting, const std::string& label_key, F32 step,
                                     F32 lo, F32 hi, Unit unit)
{
    const S32 x = addLabel(panel, top, label_key);
    Row row;
    row.kind = STEPPER;
    row.control = gSavedSettings.getControl(setting).get();
    row.step = step;
    row.lo = lo;
    row.hi = hi;
    row.unit = unit;
    const size_t i = mRows.size();
    LLButton* minus = makeButton(panel, std::string(setting) + "_minus", "\xE2\x88\x92", LLRect(x, top, x + kStepBtnW, top - kRowH), false);
    const S32 vx = x + kStepBtnW + 8;
    const S32 lineH = llmax(16, (S32)bigFont()->getLineHeight() + 4);
    const S32 vy = top - (kRowH - lineH) / 2;
    LLTextBox::Params tp;
    tp.name(std::string(setting) + "_value");
    tp.initial_value(std::string());
    tp.font(bigFont());
    tp.h_pad(0);
    tp.font_halign(LLFontGL::HCENTER);
    tp.rect(LLRect(vx, vy, vx + kStepValueW, vy - lineH));
    tp.follows.flags(FOLLOWS_LEFT | FOLLOWS_TOP);
    row.value = LLUICtrlFactory::create<LLTextBox>(tp);
    if (row.value) panel->addChild(row.value);
    const S32 px = vx + kStepValueW + 8;
    LLButton* plus = makeButton(panel, std::string(setting) + "_plus", "+", LLRect(px, top, px + kStepBtnW, top - kRowH), false);
    if (minus)
    {
        minus->setClickedCallback([this, i](LLUICtrl*, const LLSD&) { onStep(i, -1); });
        row.buttons.push_back(minus);
    }
    if (plus)
    {
        plus->setClickedCallback([this, i](LLUICtrl*, const LLSD&) { onStep(i, 1); });
        row.buttons.push_back(plus);
    }
    mRows.push_back(row);
    top -= kRowH + kRowGap;
}

void LLFloaterVRSettings::addAction(LLPanel* panel, S32& top, const std::string& label_key, const std::string& button_key,
                                    std::function<void()> run, std::function<bool()> enabled)
{
    const S32 x = addLabel(panel, top, label_key);
    Row row;
    row.kind = ACTION;
    row.enabled = std::move(enabled);
    LLButton* b = makeButton(panel, button_key, text(button_key), LLRect(x, top, x + kActionW, top - kRowH), false);
    if (b)
    {
        b->setClickedCallback([run](LLUICtrl*, const LLSD&) { run(); });
        row.buttons.push_back(b);
    }
    mRows.push_back(row);
    top -= kRowH + kRowGap;
}

// A choice's value as a numeric setting stores it: the value itself where it is a number
// ("0", "2"), else its position in the row (a String choice bound to a U32 setting).
S32 LLFloaterVRSettings::choiceNumber(const Row& row, size_t k)
{
    const std::string& s = row.values[k];
    S32 n = 0;
    return (!s.empty() && LLStringUtil::convertToS32(s, n)) ? n : (S32)k;
}

void LLFloaterVRSettings::addNote(LLPanel* panel, S32& top, const char* setting, const std::string& note_key)
{
    const S32 lineH = llmax(16, (S32)bigFont()->getLineHeight() + 4);
    Row row;
    row.kind = NOTE;
    row.control = gSavedSettings.getControl(setting).get();
    LLTextBox::Params p;
    p.name(note_key);
    p.initial_value(text(note_key));
    p.font(bigFont());
    p.wrap(true);
    p.rect(LLRect(kPad, top, panel->getRect().getWidth() - kPad, top - 2 * lineH));
    p.follows.flags(FOLLOWS_LEFT | FOLLOWS_TOP);
    row.value = LLUICtrlFactory::create<LLTextBox>(p);
    if (row.value) panel->addChild(row.value);
    mRows.push_back(row);
    top -= 2 * lineH + kRowGap;
}

// A live line of text under a row (#1594: what space warp is doing). Two lines tall, wrapped; the
// text follows its source while the floater is open (refreshRow).
void LLFloaterVRSettings::addStatus(LLPanel* panel, S32& top, std::function<std::string()> text)
{
    const S32 lineH = llmax(16, (S32)bigFont()->getLineHeight() + 4);
    Row row;
    row.kind = STATUS;
    row.status = std::move(text);
    LLTextBox::Params p;
    p.name("status");
    p.initial_value(row.status());
    p.font(bigFont());
    p.wrap(true);
    p.rect(LLRect(kPad, top, panel->getRect().getWidth() - kPad, top - 3 * lineH));
    p.follows.flags(FOLLOWS_LEFT | FOLLOWS_TOP);
    row.value = LLUICtrlFactory::create<LLTextBox>(p);
    if (row.value) panel->addChild(row.value);
    mRows.push_back(row);
    top -= 3 * lineH + kRowGap;
}

bool LLFloaterVRSettings::postBuild()
{
    LLTabContainer* tabs = getChild<LLTabContainer>("tabs");
    LLPanel* body = tabs->getChild<LLPanel>("body");
    LLPanel* hands = tabs->getChild<LLPanel>("hands");
    LLPanel* view = tabs->getChild<LLPanel>("view");
    LLPanel* interaction = tabs->getChild<LLPanel>("interaction");
    LLPanel* graphics = tabs->getChild<LLPanel>("graphics");

    // ---- Body
    S32 top = body->getRect().getHeight() - kPad;
    addChoice(body, top, "SlipstreamVRBodySource", "body_source",
              {{"Auto", "source_auto"}, {"OpenXR", "source_openxr"}, {"SlimeVR", "source_slimevr"},
               {"Headset", "source_headset"}, {"Estimate", "source_estimate"}});
    addToggle(body, top, "SlipstreamVRBodyEstimate", "body_estimate");
    addToggle(body, top, "SlipstreamVRShowBody", "show_body");                 // #1585
    addToggle(body, top, "SlipstreamVRBodyShare", "body_share");
    addToggle(body, top, "SlipstreamVRFlycamFreezePose", "flycam_freeze");
    addStepper(body, top, "SlipstreamVRBodyTurnLimit", "turn_limit", 5.f, 30.f, 150.f, DEG);   // (vr-arms-1584)
    addAction(body, top, "calibrate", "calibrate_button", [] { LLVRBody::requestCalibration(); },
              [] { return LLVRBody::available() && !LLVRBody::calibrating(); });
    // The bone mapping window (#1565 mapping; branch vr-bonemap): disabled until it is in the build.
    addAction(body, top, "bone_mapping", "bone_mapping_button",
              [] { LLFloaterReg::showInstance("vr_body_mapping", LLSD(), true); },
              [] { return LLFloaterReg::isRegistered("vr_body_mapping"); });

    // ---- Hands
    top = hands->getRect().getHeight() - kPad;
    addToggle(hands, top, "SlipstreamVRArmTracking", "arms");
    addToggle(hands, top, "SlipstreamVRFingerTracking", "fingers");
    addToggle(hands, top, "SlipstreamVRGrabEnabled", "grab");                  // #1590
    addStepper(hands, top, "SlipstreamVRBodyWeightArms", "arm_strength", 0.1f, 0.f, 1.f, PERCENT);

    // ---- View & comfort
    top = view->getRect().getHeight() - kPad;
    addToggle(view, top, "SlipstreamVRSafetyGovernor", "safety_governor");
    addToggle(view, top, "SlipstreamVRSmoothTurn", "smooth_turn");
    addStepper(view, top, "SlipstreamVRSnapTurnDegrees", "snap_turn", 15.f, 15.f, 90.f, DEG);
    addStepper(view, top, "SlipstreamVRSmoothTurnSpeed", "smooth_turn_speed", 20.f, 20.f, 360.f, DEG_PER_S);
    addStepper(view, top, "SlipstreamVRComfortVignette", "vignette", 0.1f, 0.f, 1.f, PERCENT);
    addToggle(view, top, "SlipstreamVRTeleportMove", "teleport_move");
    addStepper(view, top, "SlipstreamVRWorldTextScale", "world_text", 0.25f, 0.25f, 4.f, TIMES);

    // ---- Graphics (#1593 follow-up). SlipstreamVRStereo is an experiment read at session start
    // (not persisted): no switch here, only the note that point shadows and VR GI need it.
    top = graphics->getRect().getHeight() - kPad;
    addNote(graphics, top, "SlipstreamVRStereo", "stereo_note");
    addToggle(graphics, top, "SlipstreamVRGI", "vr_gi");
    // (#1608: 1 the desktop's ReSTIR DI per eye; 2 / 3 the RT shadow mask's point lights, the cheaper option)
    addChoice(graphics, top, "SlipstreamVRPointShadows", "point_shadows",
              {{"0", "level_off"}, {"1", "level_full"}, {"2", "level_fast"}, {"3", "level_fast_high"}});   // (vr-heavy)
    addChoice(graphics, top, "SlipstreamVRFoveation", "foveation",
              {{"0", "level_off"}, {"1", "level_low"}, {"2", "level_high"}});   // (vr-heavy)
    addChoice(graphics, top, "SlipstreamVRSpaceWarp", "space_warp",
              {{"1", "warp_auto"}, {"0", "warp_off"}, {"2", "warp_always"}},   // (vr-spacewarp: U32 0 off, 1 auto, 2 always)
              [] { return !LLXrSession::active() || LLXrSession::instance().stereo(); });   // (needs layered stereo; on the desktop: as set)
    // #1594: whether it is doing anything (on, the rendered / shown rates, standing by, or why not)
    addStatus(graphics, top, [] { return LLVRSpaceWarp::instance().statusText(); });

    // ---- Interaction
    top = interaction->getRect().getHeight() - kPad;
    addStepper(interaction, top, "SlipstreamVRWallAngle", "wall_size", 10.f, 60.f, 160.f, DEG);       // #1591
    addStepper(interaction, top, "SlipstreamVRUIScale", "wall_text", 0.1f, 0.5f, 2.f, PERCENT);
    addStepper(interaction, top, "SlipstreamVRWallDistance", "wall_distance", 0.25f, 1.f, 5.f, METRES);
    addToggle(interaction, top, "SlipstreamVRWallFreezesMovement", "wall_freezes");
    addStepper(interaction, top, "SlipstreamVRWristScale", "wrist_size", 0.1f, 0.5f, 2.f, PERCENT);

    if (LLButton* close = findChild<LLButton>("close_btn"))
        close->setClickedCallback([this](LLUICtrl*, const LLSD&) { closeFloater(); });

    for (Row& row : mRows) refreshRow(row, true);
    return true;
}

void LLFloaterVRSettings::onOpen(const LLSD& key)
{
    center();   // (on the wall in front of you: the wall is placed facing you when it comes up)
    for (Row& row : mRows) refreshRow(row, true);
}

void LLFloaterVRSettings::draw()
{
    // The rows follow their settings (a change on the wrist, in Preferences or by a feature).
    for (Row& row : mRows) refreshRow(row, false);
    LLFloater::draw();
}

void LLFloaterVRSettings::refreshRow(Row& row, bool force)
{
    if (row.kind == STATUS)
    {
        const std::string now = row.status ? row.status() : std::string();
        if (!force && row.shown.isString() && row.shown.asString() == now) return;
        row.shown = now;
        if (row.value) row.value->setText(now);
        return;
    }
    if (row.kind == CHOICE && row.enabled && row.control)
    {
        // (#1594: space warp's buttons cannot work in a mono session)
        const bool on = row.enabled();
        for (LLButton* b : row.buttons)
            if (b->getEnabled() != on) b->setEnabled(on);
    }
    if (row.kind == ACTION)
    {
        const bool on = !row.enabled || row.enabled();
        if (!force && row.shown.isBoolean() && row.shown.asBoolean() == on) return;
        row.shown = on;
        for (LLButton* b : row.buttons) b->setEnabled(on);
        return;
    }
    if (!row.control)
    {
        if (!force) return;
        if (row.kind == NOTE)
        {
            if (row.value) row.value->setVisible(false);
            return;
        }
        for (LLButton* b : row.buttons) b->setEnabled(false);
        if ((row.kind == TOGGLE || row.kind == CHOICE) && !row.buttons.empty()) row.buttons[0]->setLabel(text("not_in_build"));
        if (row.kind == CHOICE && row.buttons.size() > 1)
        {
            // One wide disabled button across the row.
            LLRect r = row.buttons[0]->getRect();
            r.mRight = row.buttons.back()->getRect().mRight;
            row.buttons[0]->setRect(r);
            for (size_t k = 1; k < row.buttons.size(); ++k) row.buttons[k]->setVisible(false);
        }
        if (row.value) row.value->setText(text("not_in_build"));
        return;
    }
    const LLSD v = row.control->getValue();
    if (!force && llsd_equals(v, row.shown)) return;
    row.shown = v;
    switch (row.kind)
    {
    case TOGGLE:
        if (!row.buttons.empty())
        {
            const bool on = v.asBoolean();
            row.buttons[0]->setToggleState(on);
            row.buttons[0]->setLabel(text(on ? "on" : "off"));
            row.buttons[0]->setLabelSelected(text(on ? "on" : "off"));
        }
        break;
    case CHOICE:
    {
        const bool str = row.control->type() == TYPE_STRING;
        const std::string cur = v.asString();
        bool any = false;
        for (size_t k = 0; k < row.buttons.size(); ++k)
        {
            const bool sel = str ? LLStringUtil::compareInsensitive(cur, row.values[k]) == 0 : v.asInteger() == choiceNumber(row, k);
            any |= sel;
            row.buttons[k]->setToggleState(sel);
        }
        if (str && !any && !row.buttons.empty()) row.buttons[0]->setToggleState(true);   // (unknown: Auto, as the readers do)
        break;
    }
    case NOTE:
        if (row.value) row.value->setVisible(!v.asBoolean());   // (shown while stereo is off)
        break;
    case STEPPER:
        if (row.value)
        {
            const F32 f = (F32)v.asReal();
            std::string s;
            switch (row.unit)
            {
            case DEG: s = llformat("%d\xC2\xB0", (S32)std::lround(f)); break;
            case DEG_PER_S: s = llformat("%d\xC2\xB0/s", (S32)std::lround(f)); break;
            case PERCENT: s = llformat("%d%%", (S32)std::lround(f * 100.f)); break;
            case METRES: s = llformat("%.2f m", f); break;
            case TIMES: s = llformat("%.2fx", f); break;
            }
            row.value->setText(s);
            if (row.buttons.size() == 2)
            {
                row.buttons[0]->setEnabled(f > row.lo + 1e-4f);
                row.buttons[1]->setEnabled(f < row.hi - 1e-4f);
            }
        }
        break;
    default:
        break;
    }
}

void LLFloaterVRSettings::onToggle(size_t i)
{
    if (i >= mRows.size() || !mRows[i].control) return;
    Row& row = mRows[i];
    const bool on = !row.control->getValue().asBoolean();
    row.control->setValue(LLSD(on));
    LL_INFOS("VR") << "VR settings: " << row.control->getName() << " " << (on ? "on" : "off") << LL_ENDL;
    // Low frame rate protection off: the same warning as Preferences > Safety.
    if (!on && row.control->getName() == "SlipstreamVRSafetyGovernor") LLNotificationsUtil::add("VRSafetyGovernorOff");
    refreshRow(row, true);
}

void LLFloaterVRSettings::onChoice(size_t i, size_t which)
{
    if (i >= mRows.size() || !mRows[i].control || which >= mRows[i].values.size()) return;
    Row& row = mRows[i];
    if (row.control->type() == TYPE_STRING) row.control->setValue(LLSD(row.values[which]));
    else row.control->setValue(LLSD(choiceNumber(row, which)));
    LL_INFOS("VR") << "VR settings: " << row.control->getName() << " = " << row.values[which] << LL_ENDL;
    refreshRow(row, true);
}

void LLFloaterVRSettings::onStep(size_t i, S32 dir)
{
    if (i >= mRows.size() || !mRows[i].control) return;
    Row& row = mRows[i];
    F32 v = (F32)row.control->getValue().asReal();
    if (!std::isfinite(v)) v = row.lo;
    // Rounded to the step (repeated presses never drift), within the setting's range.
    v = llclamp(std::round((v + row.step * (F32)dir) / row.step) * row.step, row.lo, row.hi);
    switch (row.control->type())
    {
    case TYPE_U32:
    case TYPE_S32:
        row.control->setValue(LLSD((S32)std::lround(v)));
        break;
    default:
        row.control->setValue(LLSD((F64)v));
        break;
    }
    LL_INFOS("VR") << "VR settings: " << row.control->getName() << " = " << v << LL_ENDL;
    refreshRow(row, true);
}
