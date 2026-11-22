/**
 * @file slfloatervrbodymap.h
 * @brief #1592 VR body mapping window ("vr_body_mapping"): the profile (Auto / Human / Open Pony
 *        Bipedal / Arms only / Head only / Custom; what the chosen one does, and whether the body
 *        stands on its hind legs), per role the joint it drives and tracked /
 *        animation / off, weights per part, import / export, and a live skeleton preview of your
 *        avatar as it is drawn now (front and side), the hovered or selected role highlighted.
 *        Sized for the VR UI wall and a laser pointer: large text, 40 px rows, large dropdowns.
 *        Every change applies at once (LLVRBodyMap::edit: the profile becomes Custom, saved for
 *        this avatar). Opened from the VR wrist ("Mapping"), Preferences > Safety, and the VR
 *        settings wall's Body page.
 */
#pragma once

#include "llfloater.h"
#include "llvrbodymapmath.h"

class LLButton;
class LLCheckBoxCtrl;
class LLComboBox;
class LLPanel;
class LLSliderCtrl;
class LLTextBox;
class SLVRBodyMapPreview;

class SLFloaterVRBodyMap final : public LLFloater
{
public:
    explicit SLFloaterVRBodyMap(const LLSD& key);
    bool postBuild() override;
    void onOpen(const LLSD& key) override;
    void draw() override;

    // The role the preview highlights: the row under the pointer, else the selected one (-1).
    int highlightRole() const { return mHover >= 0 ? mHover : mSelected; }

private:
    ~SLFloaterVRBodyMap() override = default;
    void buildRows();
    void fillJointLists();
    void refresh();                 // the controls from LLVRBodyMap's profile
    void onProfile();
    void onRoleChanged(int role);
    void onWeight(int part);
    void onUpright();
    void onReset();
    void onImport();
    void onExport();
    void select(int role);
    void applyEdit(const slbmap::Profile& p);

    struct Row
    {
        LLButton* name = nullptr;
        LLComboBox* joint = nullptr;
        LLComboBox* state = nullptr;
    };
    Row mRows[slbmap::R_COUNT];
    LLSliderCtrl* mWeights[slbmap::P_COUNT] = {};
    LLComboBox* mProfile = nullptr;
    LLTextBox* mStatus = nullptr;
    LLCheckBoxCtrl* mUpright = nullptr;   // the stance (Open Pony / quadruped profiles)
    LLPanel* mRolesPanel = nullptr;
    LLView* mRolesScroll = nullptr;
    SLVRBodyMapPreview* mPreview = nullptr;
    int mSelected = -1;
    int mHover = -1;
    U32 mShownGen = 0;
    bool mRefreshing = false;
    std::vector<std::string> mJointNames;   // the avatar's joints offered in the dropdowns
};
