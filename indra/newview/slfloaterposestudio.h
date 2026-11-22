/**
 * @file slfloaterposestudio.h
 * @brief Pose Studio (#500): the poser floater -- figure bone picker,
 *        swing-twist dials, side tools, pose library and sequence.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Slipstream Viewer Source Code
 * Copyright (C) 2026, Slipstream Viewer contributors
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;
 * version 2.1 of the License only.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 * $/LicenseInfo$
 */

#ifndef SL_FLOATERPOSESTUDIO_H
#define SL_FLOATERPOSESTUDIO_H

#include <string>
#include <utility>
#include <vector>

#include <boost/signals2.hpp>

#include "llfloater.h"
#include "slposestudio.h"

class LLButton;
class LLCheckBoxCtrl;
class LLComboBox;
class LLLineEditor;
class LLScrollListCtrl;
class LLSpinCtrl;
class LLTextBox;
class LLVOAvatar;
class SLPoseDialCtrl;
class SLPoseFigureCtrl;

class SLFloaterPoseStudio : public LLFloater
{
    LOG_CLASS(SLFloaterPoseStudio);
public:
    SLFloaterPoseStudio(const LLSD& key);
    ~SLFloaterPoseStudio() override;

    bool postBuild() override;
    void onOpen(const LLSD& key) override;
    void onClose(bool app_quitting) override;
    // #1566: pick this bone in the figure (an FK bone clicked in the world).
    void selectBone(const std::string& joint_name);
    void draw() override;

private:
    // The OP Pose HUD's three dials (see SLBoneModel).
    enum EDial { DIAL_X = 0, DIAL_Y, DIAL_Z, DIAL_COUNT };

    LLVOAvatar* getTarget() const;
    const SLPoseStudio::joint_t* getSelectedJoint() const;
    // The bone the dials / spinners currently show (set when they are refreshed): their
    // edits go here, so a commit arriving between a figure click and the refresh cannot
    // land the previous bone's values on the newly selected one.
    const SLPoseStudio::joint_t* getControlsJoint() const;

    // UI -> engine
    void onTargetSelected();
    void onPoseToggle();
    void onBoneSelected();
    void onViewSelected();
    void onDialChanged(EDial dial);
    void onDialReleased(EDial dial);
    void onDialPressed(EDial dial);
    void onSpinnerChanged(EDial dial);
    void onPositionChanged();
    void onScaleChanged();
    void onSnapChanged();
    void applyAxis(EDial dial, F32 dial_degrees, bool committed);
    // The dial values a turn of this bone starts from: an edited bone's remembered values,
    // an unedited one's live pose decoded (SLPoseStudio::getShownBoneAngles), else zero.
    void seedAngles(LLVOAvatar* avatar, const SLPoseStudio::joint_t& joint, SLPoseAngles& out) const;
    void applyDialSettings();
    void onFigureSelected();
    void refreshFigureList();

    void onResetBone();
    void onResetAll();
    void onUndo();
    void onRedo();
    void onRelease();
    void onExport();
    void onShareToggled();
    void onShowOthersToggled();
    void onPoseOther();
    void onPoseOtherPicked(const LLUUID& target);
    void onStopBeingPosed();
    void onIgnoreToggled();
    void refreshSync();
    // Look-at sync (#902)
    void onLookAtMapSelected();
    void onLookAtMuteToggled();
    void refreshLookAt();
    void refreshLookAtStatus();

    void onSavePose();
    void doSavePose(const std::string& name);
    void onLoadPose();
    void onDeletePose();
    void onAddToSequence();
    void onSequenceRemove();
    void onSequenceMove(S32 delta);
    void onSequenceHoldChanged();
    void onSequenceSelected();
    void onPoseSelected();
    // #1566
    std::vector<S32> selectedSequenceIndices() const;
    // Dial frame (#1566 item 11): 0 the HUD's model, 1 local, 2 parent, 3 world axes.
    static S32 dialFrame();
    void applyFrameAxis(EDial dial, F32 dial_degrees, bool committed);
    std::string mFrameJoint;        // the bone the frame turn below belongs to
    LLQuaternion mFrameBase;        // its local rotation where the current turn began
    LLQuaternion mFrameLast;        // what the dials last set
    SLPoseAngles mFrameAngles;      // the dial values at mFrameBase
    std::string mUploadStatus;      // Upload and play's last outcome (kept on the status line)
    void onSetPoseEase();
    void onUploadAndPlay();

    // engine -> UI
    void refreshTargets();
    void refreshPosingState();
    void refreshBoneControls();
    void refreshPoseList();
    void refreshSequence();
    void refreshFigureViews();

    SLPoseFigureCtrl* mFigure = nullptr;
    LLComboBox* mViewCombo = nullptr;
    LLComboBox* mTargetCombo = nullptr;
    LLButton* mPoseBtn = nullptr;
    LLTextBox* mBoneName = nullptr;
    LLTextBox* mStatusText = nullptr;
    SLPoseDialCtrl* mDials[DIAL_COUNT] = { nullptr, nullptr, nullptr };
    LLSpinCtrl* mDialSpinners[DIAL_COUNT] = { nullptr, nullptr, nullptr };
    LLComboBox* mSnapCombo = nullptr;
    LLCheckBoxCtrl* mMirrorCheck = nullptr;
    LLCheckBoxCtrl* mFreezeCheck = nullptr;
    LLCheckBoxCtrl* mShareCheck = nullptr;
    LLCheckBoxCtrl* mShowOthersCheck = nullptr;
    LLComboBox* mSyncedCombo = nullptr;
    LLButton* mIgnoreBtn = nullptr;
    bool mSyncDirty = true;
    boost::signals2::scoped_connection mSyncConnection;
    LLComboBox* mLookAtMapCombo = nullptr;
    LLComboBox* mLookAtPeopleCombo = nullptr;
    LLButton* mLookAtMuteBtn = nullptr;
    LLTextBox* mLookAtStatusText = nullptr;
    LLCheckBoxCtrl* mLookAtSendCheck = nullptr;
    S32 mLookAtStatusCountdown = 0;
    bool mLookAtDirty = true;
    boost::signals2::scoped_connection mLookAtConnection;
    LLSpinCtrl* mPosSpinners[3] = { nullptr, nullptr, nullptr };
    LLSpinCtrl* mScaleSpinners[3] = { nullptr, nullptr, nullptr };

    LLScrollListCtrl* mPoseList = nullptr;
    LLLineEditor* mPoseNameEditor = nullptr;
    LLScrollListCtrl* mSequenceList = nullptr;
    LLSpinCtrl* mHoldSpinner = nullptr;
    LLButton* mPlayBtn = nullptr;
    LLButton* mStopBtn = nullptr;
    LLCheckBoxCtrl* mLoopCheck = nullptr;

    // See getControlsJoint. The dial values themselves live in SLPoseStudio, per bone.
    std::string mControlsJoint;
    LLComboBox* mFigureCombo = nullptr;
    bool mRefreshPending = true;
    bool mPoseListDirty = true;
    bool mSequenceDirty = true;
    bool mApplyingDials = false;
    // The decode a dial press put on the dials (unedited bone): the drag's first step applies
    // on top of exactly these values, not a later frame (#1459).
    bool mHasPressSeed = false;
    std::string mPressSeedJoint;
    SLPoseAngles mPressSeed;
    S32 mTargetRefreshCountdown = 0;
    std::vector<std::pair<LLUUID, std::string>> mTargetEntries;

    boost::signals2::scoped_connection mChangedConnection;
    boost::signals2::scoped_connection mEditedConnection;
};

#endif // SL_FLOATERPOSESTUDIO_H
