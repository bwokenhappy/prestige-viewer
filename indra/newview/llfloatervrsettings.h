/**
 * @file llfloatervrsettings.h
 * @brief VR settings wall menu (#1593, epic #1072): the VR features' switches with big buttons,
 *        made to be used with the laser on the UI wall.
 *
 * Preferences is too small to work from the headset. This floater is the viewer's own XUI floater
 * system (it lives on the wall like every other floater, takes the pointer's clicks, and needs no
 * second hit-test path), sized for the wall's VR UI scale (#1591): rows 44 UI points tall
 * (~3.7 degrees on the default wall) in the Large font (~0.6 degrees of x-height).
 *
 * Five tabs (Body, Hands, View & comfort, Graphics, Interaction); every row is one setting, applied the
 * moment it is pressed (the features read their settings live). The rows are a table in
 * llfloatervrsettings.cpp; their texts are the floater's <string>s. A row whose setting this build
 * does not have (a feature on another branch) shows disabled, "(not in this build)". The rows
 * follow their settings while the floater is open (a change made elsewhere shows at once).
 *
 * Opened from the wrist panel (Settings > All VR settings, or the "vr_settings" action), which
 * brings up the wall with it. Desktop: never opened by anything (it works there too).
 */
#pragma once

#include "llfloater.h"

#include <functional>
#include <vector>

class LLButton;
class LLControlVariable;
class LLPanel;
class LLTextBox;

class LLFloaterVRSettings : public LLFloater
{
public:
    LLFloaterVRSettings(const LLSD& key);
    bool postBuild() override;
    void onOpen(const LLSD& key) override;
    void draw() override;

private:
    enum Kind { TOGGLE, CHOICE, STEPPER, ACTION, NOTE, STATUS };
    enum Unit { DEG, DEG_PER_S, PERCENT, METRES, TIMES };
    struct Row
    {
        Kind kind = TOGGLE;
        LLControlVariable* control = nullptr;   // null: not in this build (ACTION: never)
        std::vector<LLButton*> buttons;          // TOGGLE: one; CHOICE: one per value; STEPPER: minus, plus
        std::vector<std::string> values;         // CHOICE (String settings; numeric ones: choiceNumber)
        LLTextBox* value = nullptr;              // STEPPER: the value between the buttons; NOTE: the note
        F32 step = 0.f, lo = 0.f, hi = 0.f;
        Unit unit = DEG;
        std::function<bool()> enabled;           // ACTION: whether it can run now
        LLSD shown;                              // what the row shows (refreshed on change)
        std::function<std::string()> status;     // STATUS: the live text (value is its box)
    };

    S32 addLabel(LLPanel* panel, S32 top, const std::string& label_key);
    void addToggle(LLPanel* panel, S32& top, const char* setting, const std::string& label_key);
    void addChoice(LLPanel* panel, S32& top, const char* setting, const std::string& label_key,
                   const std::vector<std::pair<std::string, std::string>>& values, std::function<bool()> enabled = nullptr);
    void addStepper(LLPanel* panel, S32& top, const char* setting, const std::string& label_key, F32 step, F32 lo, F32 hi,
                    Unit unit);
    void addNote(LLPanel* panel, S32& top, const char* setting, const std::string& note_key);
    void addStatus(LLPanel* panel, S32& top, std::function<std::string()> text);
    static S32 choiceNumber(const Row& row, size_t k);
    void addAction(LLPanel* panel, S32& top, const std::string& label_key, const std::string& button_key,
                   std::function<void()> run, std::function<bool()> enabled);
    LLButton* makeButton(LLPanel* panel, const std::string& name, const std::string& label, const LLRect& r, bool toggle);
    void refreshRow(Row& row, bool force);
    void onToggle(size_t row);
    void onChoice(size_t row, size_t which);
    void onStep(size_t row, S32 dir);
    std::string text(const std::string& key) const;

    std::vector<Row> mRows;
};
