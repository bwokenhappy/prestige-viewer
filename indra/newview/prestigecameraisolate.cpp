/**
 * @file prestigecameraisolate.cpp
 * @brief Camera mode's "Isolate your subject" (see prestigecameraisolate.h).
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Prestige Viewer Source Code
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;
 * version 2.1 of the License only.
 * $/LicenseInfo$
 */

#include "llviewerprecompiledheaders.h"

#include "prestigecameraisolate.h"

#include "slcameramodel.h"
#include "slsnapshotscope.h"
#include "slstraightalpha.h"

#include "llagent.h"
#include "llcharacter.h"
#include "llimage.h"
#include "llsnapshotavatars.h"
#include "llsnapshotmodel.h"
#include "lltrans.h"
#include "llviewercamera.h"
#include "llviewercontrol.h"
#include "llviewerobject.h"
#include "llviewerwindow.h"
#include "llvoavatar.h"
#include "llvoavatarself.h"

#include <cmath>
#include <set>

namespace PrestigeCameraIsolate
{
namespace
{
// The session's picks: you (when picked) and the other avatars / free-standing animesh (their
// avatar ids: an animesh's control avatar).
struct Picks
{
    bool picking = false;
    bool self = false;
    std::set<LLUUID> ids;
};
Picks sPicks;

std::string str(const char* name, const LLStringUtil::format_map_t& args = LLStringUtil::format_map_t())
{
    return LLTrans::getString(std::string("CameraModeIsolate") + name, args);
}

LLStringUtil::format_map_t countArgs()
{
    LLStringUtil::format_map_t args;
    args["[COUNT]"] = llformat("%d", (S32)pickedCount());
    return args;
}

// The subject a picked object stands for: an avatar, the wearer of an attachment (or of a worn
// animesh), or a free-standing animesh's control avatar. None for a plain object.
LLVOAvatar* subjectOf(LLViewerObject* obj)
{
    if (!obj || obj->isDead() || obj->isHUDAttachment())
        return nullptr;
    LLVOAvatar* av = obj->asAvatar();
    if (!av)
        av = obj->getAvatar();   // an animesh's control avatar, or an attachment's wearer
    if (!av)
        av = obj->getAvatarAncestor();
    if (av && av->isControlAvatar())
    {
        LLVOAvatar* wearer = av->getAttachedAvatar();
        if (wearer && !wearer->isDead())
            av = wearer;
    }
    if (!av || av->isDead() || av->isUIAvatar())
        return nullptr;
    return av;
}

LLVOAvatar* findAvatar(const LLUUID& id)
{
    for (LLCharacter* character : LLCharacter::sInstances)
    {
        LLVOAvatar* av = dynamic_cast<LLVOAvatar*>(character);
        if (av && !av->isDead() && av->getID() == id)
            return av;
    }
    return nullptr;
}

// For the length of one capture: a shot wider than the window keeps the viewfinder's framing.
// The live path crops the window's frame (full width, less height); the offscreen render keeps
// the vertical field of view and would show more at the sides. Restored on every path out.
class FrameHold
{
public:
    FrameHold(S32 w, S32 h)
    {
        LLViewerCamera* cam = LLViewerCamera::getInstance();
        const F32 window = cam->getAspect(), shot = (F32)w / (F32)llmax(h, 1);
        if (window <= 0.f || shot <= window * 1.001f)
            return;
        mSaved = cam->getView();
        cam->setViewNoBroadcast(2.f * atanf(tanf(mSaved * 0.5f) * window / shot));
        mOn = true;
    }
    ~FrameHold()
    {
        if (mOn)
            LLViewerCamera::getInstance()->setViewNoBroadcast(mSaved);
    }
    FrameHold(const FrameHold&) = delete;
    FrameHold& operator=(const FrameHold&) = delete;

private:
    F32  mSaved = 0.f;
    bool mOn = false;
};

// Anything of the subject in the picture (one alpha above zero).
bool anyCoverage(const LLImageRaw& raw)
{
    const U8* data = raw.getData();
    if (!data || raw.getComponents() != 4)
        return false;
    const size_t n = (size_t)raw.getWidth() * (size_t)raw.getHeight();
    for (size_t p = 0; p < n; ++p)
        if (data[p * 4 + 3] != 0)
            return true;
    return false;
}
} // namespace

S32 mode()
{
    static LLCachedControl<S32> s_mode(gSavedSettings, "PrestigeCameraIsolate", 0);
    return llclamp((S32)s_mode, (S32)MODE_OFF, (S32)MODE_PICKED);
}

void setMode(S32 m)
{
    gSavedSettings.setS32("PrestigeCameraIsolate", llclamp(m, (S32)MODE_OFF, (S32)MODE_PICKED));
    if (mode() != MODE_PICKED)
        sPicks.picking = false;
}

bool enabled()
{
    return mode() != MODE_OFF;
}

bool picking()
{
    return sPicks.picking && mode() == MODE_PICKED;
}

void setPicking(bool on)
{
    if (on && mode() != MODE_PICKED)
        setMode(MODE_PICKED);
    if (on == sPicks.picking)
        return;
    sPicks.picking = on;
    SLCameraModel::toast(on ? str("PickingOn") : str("PickingOff", countArgs()), on ? 4.0 : 2.0);
}

bool pickAt(S32 x, S32 y)
{
    if (!picking() || !gViewerWindow || !SLCameraModel::frame().pointInRect(x, y))
        return false;
    // Transparent and rigged surfaces count: hair, lace and mesh bodies are what is clicked.
    LLViewerObject* obj = gViewerWindow->cursorIntersect(x, y, 512.f, nullptr, -1, true, true, true, false);
    if (!obj)
    {
        SLCameraModel::toast(str("Missed"));
        return true;
    }
    LLVOAvatar* av = subjectOf(obj);
    if (!av)
    {
        SLCameraModel::toast(str("NotAvatar"), 4.0);
        return true;
    }
    bool now_in = false;
    if (av->isSelf())
    {
        sPicks.self = !sPicks.self;
        now_in = sPicks.self;
    }
    else if (sPicks.ids.erase(av->getID()) == 0)
    {
        sPicks.ids.insert(av->getID());
        now_in = true;
    }
    SLCameraModel::toast(str(now_in ? "Picked" : "Unpicked", countArgs()));
    return true;
}

size_t pickedCount()
{
    return sPicks.ids.size() + (sPicks.self ? 1u : 0u);
}

void clearPicks()
{
    sPicks.self = false;
    sPicks.ids.clear();
}

void markPoints(std::vector<std::pair<S32, S32>>& out)
{
    out.clear();
    if (mode() != MODE_PICKED)
        return;
    std::vector<LLVOAvatar*> subjects;
    if (sPicks.self && isAgentAvatarValid())
        subjects.push_back(gAgentAvatarp);
    for (const LLUUID& id : sPicks.ids)
        if (LLVOAvatar* av = findAvatar(id))
            subjects.push_back(av);
    const LLViewerCamera* cam = LLViewerCamera::getInstance();
    for (LLVOAvatar* av : subjects)
    {
        // Just over the head (the skeleton's head: animesh have one too).
        const LLVector3 at = av->mHeadp ? av->mHeadp->getWorldPosition() + LLVector3(0.f, 0.f, 0.35f)
                                        : av->getPositionAgent() + LLVector3(0.f, 0.f, 1.f);
        LLCoordGL screen;
        if (cam->projectPosAgentToScreen(at, screen, true))
            out.emplace_back(screen.mX, screen.mY);
    }
}

bool capture(LLImageRaw& raw, S32 w, S32 h)
{
    if (!gViewerWindow || w <= 0 || h <= 0)
        return false;
    const S32 m = mode();
    bool self = true;
    std::set<LLUUID> others;
    if (m == MODE_PICKED)
    {
        self = sPicks.self;
        for (const LLUUID& id : sPicks.ids)
            if (findAvatar(id))
                others.insert(id);   // (one that left the region is not asked for)
        if (!self && others.empty())
        {
            SLCameraModel::toast(str(pickedCount() ? "PickedGone" : "NothingPicked"), 4.0);
            return false;
        }
    }
    bool ok = false;
    {
        // Both holds end with this block, whatever the render does: the Snapshot floater's scope
        // and the camera's field of view are back before anything else runs.
        LLSnapshotScopeOverride scope(m == MODE_PICKED ? (U32)SNAPSHOT_AVATARS_SELECTED : (U32)SNAPSHOT_AVATARS_SELF, self,
                                      others, true);
        FrameHold frame(w, h);
        ok = gViewerWindow->rawSnapshot(&raw, w, h, false, false, false, false, false, false, false,
                                        LLSnapshotModel::SNAPSHOT_TYPE_AVATAR_TRANSPARENT, (S32)MAX_SNAPSHOT_IMAGE_SIZE);
    }
    if (!ok || raw.getComponents() != 4)
    {
        SLCameraModel::toast(str("Failed"), 4.0);
        return false;
    }
    if (!anyCoverage(raw))
    {
        SLCameraModel::toast(str("NotInFrame"), 4.0);
        return false;
    }
    LL_INFOS("CameraMode") << "isolated shot " << raw.getWidth() << "x" << raw.getHeight() << " (mode " << m << ", "
                           << (self ? "with" : "without") << " you, " << others.size() << " others)" << LL_ENDL;
    return true;
}

void prepareForTexture(LLImageRaw& raw)
{
    // (LLSnapshotLivePreview::uploadSnapshotTexture's limit: 2048 x 2048)
    constexpr S32 kMaxTextureSize = 2048;
    if (raw.getComponents() != 4 || !raw.getData())
        return;
    {
        LLImageDataLock lock(&raw);
        slPremultiplyRGBA(raw.getData(), (size_t)raw.getWidth() * (size_t)raw.getHeight());
    }
    raw.biasedScaleToPowerOfTwo(kMaxTextureSize);   // (takes the lock itself)
    LLImageDataLock lock(&raw);
    slUnpremultiplyRGBA(raw.getData(), (size_t)raw.getWidth() * (size_t)raw.getHeight());
}

std::string statusLabel()
{
    switch (mode())
    {
    case MODE_MY_AVATAR: return str("StatusMine");
    case MODE_PICKED:    return str(picking() ? "StatusPicking" : "StatusPicked", countArgs());
    default:             return std::string();
    }
}

void endSession()
{
    sPicks = Picks();
}
} // namespace PrestigeCameraIsolate
