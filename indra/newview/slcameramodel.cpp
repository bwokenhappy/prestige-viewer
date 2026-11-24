/**
 * @file slcameramodel.cpp
 * @brief Camera mode's camera: state and actions (see slcameramodel.h).
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

#include "llviewerprecompiledheaders.h"

#include "slcameramodel.h"

#include "slcameraexposure.h"
#include "slcameramode.h"
#include "slcamerareview.h"
#include "prestigecameraisolate.h"   // <Prestige> isolate your subject

#include "llagent.h"
#include "llagentcamera.h"
#include "llimagebmp.h"
#include "llimagejpeg.h"
#include "llimagepng.h"
#include "llpresetsmanager.h"
#include "llsnapshotlivepreview.h"
#include "llsnapshotmodel.h"
#include "llviewercamera.h"
#include "llviewercontrol.h"
#include "llviewerobject.h"
#include "llviewerobjectlist.h"  // gObjectList (AF-C follows its subject)
#include "llviewerwindow.h"
#include "lltrans.h"
#include "llagentbenefits.h"
#include "llagentui.h"
#include "llpostcard.h"
#include "llregex.h"
#include "llviewerassetupload.h"
#include "llviewerregion.h"
#include "llwebprofile.h"
#include "llstatusbar.h"     // can_afford_transaction
// <Prestige> faces, group focus, focus confirmation, AE-L
#include "prestigecamerafocus.h"
#include "llvoavatar.h"
// </Prestige>

#include <boost/regex.hpp>

#include <cmath>
#include <vector>

namespace SLCE = SLCameraExposure;

namespace SLCameraModel
{
namespace
{
// What a camera session keeps on top of the settings.
struct Model
{
    // Autofocus: the AF point (normalised in the frame, 0.5 / 0.5 = centre) and what it locked on.
    F32 afX = 0.5f, afY = 0.5f;
    LLVector3d focusGlobal;
    bool focusValid = false;
    // AF-C: the subject the AF point found, and the focus point in its own frame, so the focus
    // follows the subject without picking again.
    LLUUID focusObject;
    LLVector3 focusOffset;
    // The DoF preview turned depth of field on for as long as it is held.
    bool dofPreviewForced = false;
    bool dofHeld = false;          // the DoF preview is held (key or not)
    // AF-L: the held focus stays through half-presses until AF-L lets it go.
    bool afLocked = false;
    F32 programShift = 0.f;    // P: stops toward a wider aperture (a real body forgets it too)

    F64 flashUntil = 0.0;
    F64 savedAt = -100.0;      // the last shot reached its destination (or was handed to it)
    U32 savedTo = 0;           // ... which one (Destination)
    // Phase 2 (#1572): a shot waiting for its confirmation (inventory, profile) or its address
    // (e-mail), and what this session no longer asks.
    LLPointer<LLImageRaw> pending;
    U32 pendingTo = 0;
    LLVector3d pendingPos;
    bool skipAsk[4] = {};
    std::string lastEmailTo;
    bool lastAddLocation = false;
    std::string toast;
    F64 toastUntil = 0.0;
};
Model sModel;

F64 now() { return LLTimer::getTotalSeconds(); }
} // namespace

LLRect frame()
{
    if (!gViewerWindow)
        return LLRect();
    const S32 w = gViewerWindow->getWindowWidthScaled();
    const S32 h = gViewerWindow->getWindowHeightScaled();
    S32 ow = 0, oh = 0;
    outputSize(ow, oh);
    F32 cw = 0.f, ch = 0.f;
    SLCE::cropRect(w, h, ow, oh, cw, ch);
    const S32 l = ll_round(((F32)w - cw) * 0.5f), b = ll_round(((F32)h - ch) * 0.5f);
    return LLRect(l, b + ll_round(ch), l + ll_round(cw), b);
}

void toast(const std::string& text, F64 seconds)
{
    sModel.toast = text;
    sModel.toastUntil = now() + seconds;
}

// ---- settings (the existing renderer / Photo Studio settings are the source of truth) ----

U32 exposureModeSetting()
{
    static LLCachedControl<U32> s_mode(gSavedSettings, "SLCameraModeExposure", 0);
    return llmin((U32)s_mode, (U32)SLCE::MODE_MANUAL);
}

F32 currentShutter()
{
    static LLCachedControl<F32> s_shutter(gSavedSettings, "RenderVKMotionBlurShutter", 1.f / 60.f);
    return (F32)s_shutter > 0.f ? (F32)s_shutter : 1.f / 125.f;
}

F32 currentISO()
{
    static LLCachedControl<F32> s_iso(gSavedSettings, "SLCameraModeISO", 100.f);
    return llclamp((F32)s_iso, 100.f, 12800.f);
}

F32 currentExposureGain()
{
    static LLCachedControl<F32> s_exposure(gSavedSettings, "RenderExposure", 1.f);
    return llclamp((F32)s_exposure, 0.5f, 4.f);
}

SLCE::Settings currentSettings()
{
    static LLCachedControl<F32> s_fnumber(gSavedSettings, "CameraFNumber", 9.f);
    SLCE::Settings s;
    s.mode = exposureModeSetting();
    s.fnumber = s_fnumber;
    s.shutter = currentShutter();
    s.iso = currentISO();
    // P / Av / Tv: RenderExposure is the compensation (auto exposure on). M: it is the manual
    // exposure's own gain, not a compensation.
    s.evComp = s.mode == SLCE::MODE_MANUAL ? 0.f : std::log2(currentExposureGain());
    s.programShift = sModel.programShift;
    return s;
}

// The renderer's exposure range: RenderExposure is clamped to 0.5..4 where it is uploaded.
constexpr F32 EV_COMP_LO = -1.f;
constexpr F32 EV_COMP_HI = 2.f;

void applySettings(const SLCE::Settings& s)
{
    const SLCE::Result r = SLCE::solve(s);
    gSavedSettings.setU32("SLCameraModeExposure", s.mode);
    gSavedSettings.setF32("SLCameraModeISO", s.iso);
    gSavedSettings.setF32("CameraFNumber", r.fnumber);
    gSavedSettings.setF32("RenderVKMotionBlurShutter", r.shutter);
    if (gSavedSettings.getBOOL("RenderDynamicExposureEnabled") != r.autoExposure)
    {
        gSavedSettings.setBOOL("RenderDynamicExposureEnabled", r.autoExposure);
    }
    gSavedSettings.setF32("RenderExposure", llclamp(std::exp2(r.exposureStops), 0.5f, 4.f));
}

// White balance: the Photo Studio's grade, which has to be on for it to apply.
bool whiteBalanceOn()
{
    static LLCachedControl<bool> s_grade(gSavedSettings, "RenderColorGrade", false);
    return s_grade;
}

F32 currentKelvin()
{
    static LLCachedControl<F32> s_cct(gSavedSettings, "RenderColorGradeWhiteBalanceCCT", 0.f);
    return SLCE::kelvinForWhiteBalanceOffset(s_cct);
}

void setKelvin(F32 kelvin)
{
    gSavedSettings.setF32("RenderColorGradeWhiteBalanceCCT", SLCE::whiteBalanceOffsetForKelvin(kelvin));
    if (!whiteBalanceOn())
    {
        gSavedSettings.setBOOL("RenderColorGrade", true);
        toast("Colour grading turned on for white balance");
    }
}

// ---- the frame ----

void outputSize(S32& w, S32& h)
{
    static LLCachedControl<S32> s_w(gSavedSettings, "SnapshotLocalWidth", 0);
    static LLCachedControl<S32> s_h(gSavedSettings, "SnapshotLocalHeight", 0);
    static LLCachedControl<U32> s_aspect(gSavedSettings, "SLCameraModeAspect", 0);
    S32 base_w = s_w, base_h = s_h;
    if (base_w <= 0 || base_h <= 0)
    {
        base_w = gViewerWindow->getWindowWidthRaw();
        base_h = gViewerWindow->getWindowHeightRaw();
    }
    SLCE::outputSize(s_aspect, base_w, base_h, (S32)MAX_SNAPSHOT_IMAGE_SIZE, w, h);
}

// The 35 mm equivalent of the view as framed.
F32 currentFocalLength()
{
    const F32 view_h = (F32)gViewerWindow->getWindowHeightScaled();
    const LLRect fr = frame();
    return SLCE::focalLengthForFov(LLViewerCamera::getInstance()->getDefaultFOV(), view_h, (F32)fr.getWidth(), (F32)fr.getHeight());
}

void setFocalLength(F32 focal_mm)
{
    LLViewerCamera* cam = LLViewerCamera::getInstance();
    const F32 view_h = (F32)gViewerWindow->getWindowHeightScaled();
    const LLRect fr = frame();
    F32 fov = SLCE::fovForFocalLength(focal_mm, view_h, (F32)fr.getWidth(), (F32)fr.getHeight());
    fov = llclamp(fov, cam->getMinView(), cam->getMaxView());
    gSavedSettings.setF32("CameraAngle", fov);   // its commit signal sets the camera's FOV
}

// ---- focus (#1576): AF-S, AF-C, MF ----

enum FocusMode : U32
{
    FOCUS_AF_S = 0,   // focus once at the AF point and hold
    FOCUS_AF_C = 1,   // follow the subject the AF point found
    FOCUS_MF = 2,     // the focus ring: a distance in metres
    FOCUS_COUNT = 3
};

U32 focusMode()
{
    static LLCachedControl<U32> s_mode(gSavedSettings, "SLCameraModeFocus", 0);
    return llmin((U32)s_mode, (U32)FOCUS_MF);
}

F32 manualFocusDistance()
{
    static LLCachedControl<F32> s_dist(gSavedSettings, "SLCameraModeFocusDistance", 5.f);
    return llclamp((F32)s_dist, 0.1f, 1000.f);
}

const char* focusModeLabel(U32 mode)
{
    switch (mode)
    {
    case FOCUS_AF_C: return "AF-C";
    case FOCUS_MF:   return "MF";
    default:         return "AF-S";
    }
}

// Distance along the view axis from the camera to an agent-space point.
F32 focusDistanceTo(const LLVector3& p)
{
    const LLViewerCamera* cam = LLViewerCamera::getInstance();
    return cam->getAtAxis() * (p - cam->getOrigin());
}

// <Prestige> faces, group focus and the aperture that keeps a group sharp (prestigecamerafocus.h)
namespace PCF = PrestigeCameraFocus;
namespace
{
struct PrestigeFocus
{
    LLUUID person;              // the avatar whose eyes the focus is on (AF-C follows them)
    bool   halfArmed = false;   // Enter's first press (the half-press): the next one shoots
    // Group focus: the aperture settings it started from (put back when it ends, unless the
    // photographer turned the aperture since) and what it set last.
    bool           apertureActive = false;
    SLCE::Settings base;
    F32            setN = 0.f;
    U32            setMode = 0;
    bool           warnedDeep = false;    // "not everyone can be sharp", once per group
    F64            nextGroupCheck = 0.0;  // AF-C: the aperture follows the group at 4 Hz
};
PrestigeFocus sPrestige;

// The AF area around the AF point where a face is found (UI units: its 40 dp box and a margin).
constexpr F32 kAfAreaHalf = 24.f;
} // namespace

// The aperture settings of `from` on `onto`: the mode, f-number and program shift, and the
// shutter when it was Tv (which a group may have switched to Av).
void prestigeTakeAperture(SLCE::Settings& onto, const SLCE::Settings& from)
{
    onto.mode = from.mode;
    onto.fnumber = from.fnumber;
    onto.programShift = from.programShift;
    if (from.mode == SLCE::MODE_SHUTTER)
        onto.shutter = from.shutter;
}

// Group focus: the smallest aperture change that keeps every chosen person's eyes sharp -- the
// f-number at least fNumberFor(near, far), starting from the aperture the group started with.
void prestigeGroupAperture(F32 near_m, F32 far_m)
{
    const SLCE::Settings cur = currentSettings();
    const SLCE::Result have = SLCE::solve(cur);
    if (!sPrestige.apertureActive || std::fabs(have.fnumber - sPrestige.setN) > 0.01f || cur.mode != sPrestige.setMode)
    {
        // A new group, or the photographer turned the aperture (or the mode): that is the base now.
        sPrestige.apertureActive = true;
        sPrestige.base = cur;
        sPrestige.setN = have.fnumber;
        sPrestige.setMode = cur.mode;
    }
    const F32 need = PCF::fNumberFor(near_m, far_m);
    const std::vector<F32>& stops = SLCE::apertureStops();
    SLCE::Settings s = cur;
    prestigeTakeAperture(s, sPrestige.base);
    if (SLCE::solve(s).fnumber + 0.01f < need)
    {
        if (s.mode == SLCE::MODE_PROGRAM)
        {
            // P: the program shift toward a smaller aperture, a third of a stop at a time.
            while (SLCE::solve(s).fnumber + 0.01f < need && s.programShift > -8.f)
                s.programShift = llmax(s.programShift - 1.f / 3.f, -8.f);
        }
        else
        {
            // Av / M: the aperture itself. Tv (the aperture follows the shutter): Av, as turning
            // the aperture's own cell does.
            if (s.mode == SLCE::MODE_SHUTTER)
                s.mode = SLCE::MODE_APERTURE;
            F32 n = stops.back();
            for (F32 stop : stops)
            {
                if (stop + 0.01f >= need)
                {
                    n = stop;
                    break;
                }
            }
            s.fnumber = n;
        }
        if (need > stops.back() + 0.01f && !sPrestige.warnedDeep)
        {
            sPrestige.warnedDeep = true;
            toast(LLTrans::getString("PrestigeCameraGroupTooDeep"), 3.0);
        }
    }
    const SLCE::Result want = SLCE::solve(s);
    if (s.mode == cur.mode && std::fabs(want.fnumber - have.fnumber) < 0.01f && std::fabs(want.shutter - have.shutter) < 1e-5f
        && std::fabs(s.programShift - cur.programShift) < 0.01f)
    {
        return;   // already there
    }
    if (s.mode != cur.mode && s.mode == SLCE::MODE_APERTURE)
        toast(LLTrans::getString("CameraModeSwitchedAv"), 3.0);
    sModel.programShift = s.programShift;
    applySettings(s);
    sPrestige.setN = want.fnumber;
    sPrestige.setMode = s.mode;
}

// The group ends (a plain click, the last one taken out, camera mode closing): the aperture back
// to where it started, unless the photographer has turned it since.
void prestigeEndGroup()
{
    if (sPrestige.apertureActive)
    {
        const SLCE::Settings cur = currentSettings();
        if (std::fabs(SLCE::solve(cur).fnumber - sPrestige.setN) < 0.01f && cur.mode == sPrestige.setMode)
        {
            SLCE::Settings s = cur;
            prestigeTakeAperture(s, sPrestige.base);
            sModel.programShift = s.programShift;
            applySettings(s);
        }
    }
    sPrestige.apertureActive = false;
    sPrestige.warnedDeep = false;
    PCF::clearGroup();
}

// A group focus: the focus between its people's eyes and the aperture that covers them (instead
// of the AF point). False: no group.
bool prestigeAcquireGroup()
{
    if (PCF::group().empty())
        return false;
    F32 near_m = 0.f, far_m = 0.f;
    if (!PCF::groupDepths(near_m, far_m))
    {
        PCF::fail();   // nobody of the group in view: the focus stays where it was
        return true;
    }
    const LLViewerCamera* cam = LLViewerCamera::getInstance();
    const LLVector3 point = cam->getOrigin() + cam->getAtAxis() * PCF::groupFocusDistance(near_m, far_m);
    sModel.focusGlobal = gAgent.getPosGlobalFromAgent(point);
    sModel.focusValid = true;
    sModel.focusObject.setNull();
    sPrestige.person.setNull();
    prestigeGroupAperture(near_m, far_m);
    PCF::search();
    return true;
}

// After the AF point's pick: on a person (anywhere on the body, a rigged mesh body, something
// worn), or a face in the AF area in front of what the pick hit, the focus goes to their eyes.
// Otherwise the point stays; confirmed when the pick hit something, failed on the sky.
void prestigeFaceFocus(LLViewerObject* obj, S32 x, S32 y, const LLVector3& point)
{
    sPrestige.person.setNull();
    LLVOAvatar* av = PCF::personOf(obj);
    if (!av)
        av = PCF::faceNear(x, y, kAfAreaHalf, obj ? focusDistanceTo(point) : 512.f);
    LLVector3 eyes;
    if (av && PCF::eyesOf(av, eyes))
    {
        sModel.focusGlobal = gAgent.getPosGlobalFromAgent(eyes);
        sModel.focusObject.setNull();
        sPrestige.person = av->getID();
        PCF::search();
        return;
    }
    if (obj)
        PCF::search();
    else
        PCF::fail();
}

// AF-C: the focus follows the person's eyes, or the group (its focus distance), every frame.
// False: neither (or the group is out of view: the focus holds).
bool prestigeTrack(LLVector3& agent_pos)
{
    const LLViewerCamera* cam = LLViewerCamera::getInstance();
    if (!PCF::group().empty())
    {
        F32 near_m = 0.f, far_m = 0.f;
        if (!PCF::groupDepths(near_m, far_m))
            return false;
        agent_pos = cam->getOrigin() + cam->getAtAxis() * PCF::groupFocusDistance(near_m, far_m);
        sModel.focusGlobal = gAgent.getPosGlobalFromAgent(agent_pos);
        return true;
    }
    if (sPrestige.person.isNull())
        return false;
    if (PCF::eyesOf(sPrestige.person, agent_pos))
    {
        sModel.focusGlobal = gAgent.getPosGlobalFromAgent(agent_pos);
        return true;
    }
    sPrestige.person.setNull();   // gone: the focus holds where the eyes were last
    PCF::fail();
    return false;
}
// </Prestige>

// Focus at the AF point: the nearest surface on its ray (sky: far away). One pick, at a press.
// MF: the ring is set to that distance instead (focus there, then turn the ring).
void acquireFocus()
{
    const LLRect fr = frame();
    if (fr.isEmpty()) return;
    // <Prestige> a group focus: its people's eyes, not the AF point (MF: the ring, as before)
    if (focusMode() != FOCUS_MF && prestigeAcquireGroup()) return;
    // </Prestige>
    const S32 x = fr.mLeft + ll_round(sModel.afX * (F32)fr.getWidth());
    const S32 y = fr.mBottom + ll_round(sModel.afY * (F32)fr.getHeight());
    LLVector4a hit, start, end;
    hit.clear();
    start.clear();
    end.clear();
    LLViewerObject* obj = gViewerWindow->cursorIntersect(x, y, 512.f, nullptr, -1, false, true, true, false, nullptr,
                                                         nullptr, nullptr, &hit, nullptr, nullptr, nullptr, &start, &end);
    const LLVector3 point(obj ? hit.getF32ptr() : end.getF32ptr());
    if (focusMode() == FOCUS_MF)
    {
        gSavedSettings.setF32("SLCameraModeFocusDistance", llclamp(focusDistanceTo(point), 0.1f, 1000.f));
        return;
    }
    sModel.focusGlobal = gAgent.getPosGlobalFromAgent(point);
    sModel.focusValid = true;
    sModel.focusObject.setNull();
    if (obj && !obj->isHUDAttachment())
    {
        // AF-C keeps the point on the subject: in the subject's own frame.
        sModel.focusObject = obj->getID();
        sModel.focusOffset = (point - obj->getPositionAgent()) * ~obj->getRenderRotation();
    }
    // <Prestige> faces: a pick on a person (or a face in the AF area) focuses on their eyes
    prestigeFaceFocus(obj, x, y, point);
    // </Prestige>
}

// The shutter's half-press: autofocus at the AF point -- not in MF (the ring is the focus) and
// not while AF-L holds one. A click in the frame and AF-ON focus regardless.
void halfPressFocus()
{
    if (focusMode() == FOCUS_MF || sModel.afLocked) return;
    acquireFocus();
}

// The focus ring (MF): wheel steps of about 12 %, so near distances move finely.
void turnFocusRing(S32 clicks)
{
    const F32 d = manualFocusDistance() * std::exp2((F32)clicks / 6.f);
    gSavedSettings.setF32("SLCameraModeFocusDistance", llclamp(d, 0.1f, 1000.f));
}

void setFocusMode(U32 mode)
{
    gSavedSettings.setU32("SLCameraModeFocus", mode % FOCUS_COUNT);
    if (mode % FOCUS_COUNT == FOCUS_MF && sModel.focusValid)
    {
        // Into MF from a lock: the ring starts where the lock was.
        const F32 d = focusDistanceTo(gAgent.getPosAgentFromGlobal(sModel.focusGlobal));
        gSavedSettings.setF32("SLCameraModeFocusDistance", llclamp(d, 0.1f, 1000.f));
    }
}

// The DoF preview, held: depth of field on while held, back off on release if it was off.
void setDofPreview(bool held)
{
    sModel.dofHeld = held;
    if (held && !sModel.dofPreviewForced && !gSavedSettings.getBOOL("RenderDepthOfField"))
    {
        gSavedSettings.setBOOL("RenderDepthOfField", true);
        sModel.dofPreviewForced = true;
    }
    else if (!held && sModel.dofPreviewForced)
    {
        gSavedSettings.setBOOL("RenderDepthOfField", false);
        sModel.dofPreviewForced = false;
    }
}

// ---- the shot ----

// What the shot was taken at, for the review.
std::string shotLabel()
{
    static LLCachedControl<F32> s_fnumber(gSavedSettings, "CameraFNumber", 9.f);
    const F32 ev = exposureModeSetting() == SLCE::MODE_MANUAL ? SLCE::solve(currentSettings()).exposureStops
                                                       : std::log2(currentExposureGain());
    return llformat("%s  %s  %s  ISO %d  %s EV", SLCE::modeLabel(exposureModeSetting()), SLCE::shutterLabel(currentShutter()).c_str(),
                    SLCE::apertureLabel(s_fnumber).c_str(), ll_round(currentISO()), SLCE::evLabel(ev).c_str());
}

// The shot to a destination other than the disk (inventory, profile feed, e-mail).
void sendTo(U32 dest, LLImageRaw& raw, const LLVector3d& pos, const LLSD& params);

void capture(bool in_sequence)
{
    // A shot waiting on the card (inventory, profile, e-mail) is never replaced unseen: the card
    // says what it sends and what it costs. Send or cancel it first.
    if (!in_sequence && destination() != DEST_DISK && sModel.pending.notNull())
    {
        toast(LLTrans::getString("CameraModeShotWaiting"));
        return;
    }
    S32 w = 0, h = 0;
    outputSize(w, h);
    LLPointer<LLImageRaw> raw = new LLImageRaw;
    const bool no_post = gSavedSettings.getBOOL("RenderSnapshotNoPost");
    // <Prestige> isolate your subject: the subject alone on a transparent background (RGBA)
    const bool isolate = PrestigeCameraIsolate::enabled();
    if (isolate && !PrestigeCameraIsolate::capture(*raw, w, h))
        return;   // (it said why)
    // </Prestige>
    // Never the interface (the overlay is part of it), never the HUDs: camera mode hides both.
    // <Prestige> (!isolate: the cut-out above is the shot) </Prestige>
    if (!isolate && !gViewerWindow->rawSnapshot(raw, w, h, false, false, false, false, false, no_post, false,
                                    LLSnapshotModel::SNAPSHOT_TYPE_COLOR, (S32)MAX_SNAPSHOT_IMAGE_SIZE))
    {
        toast("The shot could not be taken");
        return;
    }
    const U32 serial = SLCameraReview::add(*raw, shotLabel());
    // Phase 2: other destinations. A burst or a bracket would cost L$ or post once per frame:
    // those save to disk (and say so).
    U32 dest = destination();
    if (dest != DEST_DISK && in_sequence)
    {
        toast(LLTrans::getString("CameraModeSequenceToDisk"));
        dest = DEST_DISK;
    }
    // <Prestige> isolate: the profile feed and e-mail take JPEG (no transparency): the cut-out
    // goes to disk. An inventory texture keeps its alpha (at the upload's power-of-two size).
    if (isolate && (dest == DEST_PROFILE || dest == DEST_EMAIL))
    {
        toast(LLTrans::getString("CameraModeIsolateToDisk"), 4.0);
        dest = DEST_DISK;
    }
    if (isolate && dest == DEST_INVENTORY)
    {
        PrestigeCameraIsolate::prepareForTexture(*raw);
        toast(LLTrans::getString("CameraModeIsolateInventory"), 4.0);
    }
    // </Prestige>
    if (dest != DEST_DISK)
    {
        gViewerWindow->playSnapshotAnimAndSound();
        sModel.flashUntil = now() + 0.12;
        const LLVector3d pos = gAgentCamera.getCameraPositionGlobal();
        if (dest == DEST_INVENTORY)
        {
            const S32 cost = LLSnapshotLivePreview::snapshotTextureCost(raw->getWidth(), raw->getHeight());
            if (!can_afford_transaction(cost))
            {
                LLStringUtil::format_map_t args;
                args["[COST]"] = llformat("%d", cost);
                toast(LLTrans::getString("CameraModeCannotAfford", args), 4.0);
                return;
            }
        }
        if (dest != DEST_EMAIL && sModel.skipAsk[dest])
        {
            sendTo(dest, *raw, pos, LLSD().with("location", sModel.lastAddLocation));
            return;
        }
        // Wait for the overlay's confirmation (or the address).
        sModel.pending    = raw;
        sModel.pendingTo  = dest;
        sModel.pendingPos = pos;
        return;
    }
    LLPointer<LLImageFormatted> formatted;
    // <Prestige> isolate: only PNG keeps the transparency
    switch (isolate ? LLSnapshotModel::SNAPSHOT_FORMAT_PNG : (LLSnapshotModel::ESnapshotFormat)gSavedSettings.getS32("SnapshotFormat"))
    // </Prestige>
    {
    case LLSnapshotModel::SNAPSHOT_FORMAT_JPEG:
        formatted = new LLImageJPEG(gSavedSettings.getS32("SnapshotQuality"));
        break;
    case LLSnapshotModel::SNAPSHOT_FORMAT_BMP:
        formatted = new LLImageBMP;
        break;
    default:
        formatted = new LLImagePNG;
        break;
    }
    formatted->enableOverSize();
    formatted->encode(raw, 0);
    formatted->disableOverSize();
    sModel.flashUntil = now() + 0.12; // the sound: saveImageLocal plays it once the file is written
    LLSnapshotLivePreview::saveLocal(
        formatted,
        [serial]()
        {
            if (gViewerWindow)
                SLCameraReview::setPath(serial, gViewerWindow->getLastSnapshotPath());
            sModel.savedAt = now();
            sModel.savedTo = DEST_DISK;
        },
        []() { toast(LLTrans::getString("CameraModeSaveFailed")); });
}

// ---- drive (#1577): single, self-timer, burst, exposure bracketing ----

enum Drive : U32
{
    DRIVE_SINGLE = 0,
    DRIVE_TIMER_2 = 1,
    DRIVE_TIMER_10 = 2,
    DRIVE_BURST = 3,
    DRIVE_BRACKET = 4,
    DRIVE_COUNT = 5
};

U32 driveMode()
{
    static LLCachedControl<U32> s_drive(gSavedSettings, "SLCameraModeDrive", 0);
    return llmin((U32)s_drive, (U32)DRIVE_BRACKET);
}

U32 burstCount()
{
    static LLCachedControl<U32> s_count(gSavedSettings, "SLCameraModeBurstCount", 5);
    return llclamp((U32)s_count, 2u, 20u);
}

U32 bracketShots()
{
    static LLCachedControl<U32> s_shots(gSavedSettings, "SLCameraModeBracketShots", 3);
    const U32 n = llclamp((U32)s_shots, 3u, 7u);
    return n | 1u;   // 3, 5 or 7
}

F32 bracketStep()
{
    static LLCachedControl<F32> s_step(gSavedSettings, "SLCameraModeBracketStep", 1.f);
    return llclamp(std::round((F32)s_step * 3.f) / 3.f, 1.f / 3.f, 2.f);
}

std::string driveLabel()
{
    switch (driveMode())
    {
    case DRIVE_TIMER_2:  return "2s timer";
    case DRIVE_TIMER_10: return "10s timer";
    case DRIVE_BURST:    return llformat("Burst %u", burstCount());
    case DRIVE_BRACKET:  return llformat("AEB %ux%s", bracketShots(), SLCE::evLabel(bracketStep()).substr(1).c_str());
    default:             return "Single";
    }
}

// A sequence in progress: a self-timer's wait, then one shot per idle tick (a bracketed shot's
// exposure is set one tick before it is taken, so the frame it captures has it).
struct DriveRun
{
    bool running = false;
    F64 startAt = 0.0;            // self-timer: the shot waits until then
    std::vector<F32> offsets;     // per shot: EV from the base exposure
    size_t next = 0;
    bool exposureSet = false;     // this shot's exposure is in (bracketing)
    bool bracketing = false;
    F32 baseExposure = 1.f;       // RenderExposure before bracketing, put back after
};
DriveRun sDrive;


void stopDrive()
{
    if (!sDrive.running) return;
    if (sDrive.bracketing)
    {
        gSavedSettings.setF32("RenderExposure", sDrive.baseExposure);
    }
    sDrive = DriveRun();
}

void driveTick()
{
    if (!sDrive.running || !SLCameraMode::active())
    {
        stopDrive();
        return;
    }
    if (now() < sDrive.startAt) return;
    if (sDrive.next >= sDrive.offsets.size())
    {
        stopDrive();
        return;
    }
    if (sDrive.bracketing && !sDrive.exposureSet)
    {
        const F32 want = sDrive.baseExposure * std::exp2(sDrive.offsets[sDrive.next]);
        gSavedSettings.setF32("RenderExposure", llclamp(want, 0.5f, 4.f));
        if (want < 0.499f || want > 4.001f)
        {
            toast("Bracketing: the renderer's exposure stops at -1 / +2 EV");
        }
        sDrive.exposureSet = true;
        return;   // next tick: a frame with this exposure
    }
    capture(sDrive.offsets.size() > 1);
    sDrive.exposureSet = false;
    ++sDrive.next;
}

// The shutter: what the drive mode does with a press. A press while a sequence runs cancels it.
void pressShutter()
{
    if (sDrive.running)
    {
        stopDrive();
        toast("Cancelled");
        return;
    }
    const U32 drive = driveMode();
    if (drive == DRIVE_SINGLE)
    {
        capture(false);
        return;
    }
    if ((drive == DRIVE_BURST || drive == DRIVE_BRACKET) && !gViewerWindow->isSnapshotLocSet())
    {
        // Without a snapshot folder every save asks for a file name, and a sequence would queue
        // dialogs over one another: the first single shot to disk picks the folder.
        toast(LLTrans::getString(destination() == DEST_DISK ? "CameraModeFolderFirst" : "CameraModeFolderFirstDisk"), 4.0);
        return;
    }
    sDrive = DriveRun();
    sDrive.running = true;
    sDrive.startAt = now() + (drive == DRIVE_TIMER_2 ? 2.0 : (drive == DRIVE_TIMER_10 ? 10.0 : 0.0));
    if (drive == DRIVE_BURST)
    {
        sDrive.offsets.assign(burstCount(), 0.f);
    }
    else if (drive == DRIVE_BRACKET)
    {
        // 0, -s, +s, -2s, +2s, ... as cameras order a bracket.
        const F32 s = bracketStep();
        sDrive.offsets.push_back(0.f);
        for (U32 i = 1; i <= bracketShots() / 2; ++i)
        {
            sDrive.offsets.push_back(-s * (F32)i);
            sDrive.offsets.push_back(s * (F32)i);
        }
        sDrive.bracketing = true;
        sDrive.baseExposure = currentExposureGain();
    }
    else
    {
        sDrive.offsets.assign(1, 0.f);
    }
}

void turnDrive(S32 clicks, MASK mask)
{
    const U32 drive = driveMode();
    if ((mask & MASK_SHIFT) && drive == DRIVE_BURST)
    {
        gSavedSettings.setU32("SLCameraModeBurstCount", (U32)llclamp((S32)burstCount() + clicks, 2, 20));
    }
    else if ((mask & MASK_SHIFT) && drive == DRIVE_BRACKET)
    {
        gSavedSettings.setU32("SLCameraModeBracketShots", (U32)llclamp((S32)bracketShots() + 2 * clicks, 3, 7));
    }
    else if ((mask & MASK_CONTROL) && drive == DRIVE_BRACKET)
    {
        gSavedSettings.setF32("SLCameraModeBracketStep", SLCE::stepThirds(bracketStep(), clicks, 1.f / 3.f, 2.f));
    }
    else
    {
        const S32 n = (S32)DRIVE_COUNT;
        gSavedSettings.setU32("SLCameraModeDrive", (U32)((((S32)drive + clicks) % n + n) % n));
    }
}

// ---- dials ----

void turnMode(S32 clicks)
{
    SLCE::Settings s = currentSettings();
    const U32 was = s.mode;
    s.mode = (U32)(((S32)s.mode + clicks % (S32)SLCE::MODE_COUNT + (S32)SLCE::MODE_COUNT) % (S32)SLCE::MODE_COUNT);
    if (was == SLCE::MODE_MANUAL && s.mode != SLCE::MODE_MANUAL)
    {
        s.evComp = 0.f;   // leaving M: its gain is not a compensation
    }
    sModel.programShift = 0.f;
    s.programShift = 0.f;
    applySettings(s);
}

void turn(Value ctl, S32 clicks)
{
    if (clicks == 0) return;
    SLCE::Settings s = currentSettings();
    switch (ctl)
    {
    case Value::Mode:
        turnMode(clicks);
        return;
    case Value::Shutter:
        if (s.mode == SLCE::MODE_PROGRAM)
        {
            // P: the shutter dial is the program shift (faster shutter, wider aperture).
            sModel.programShift = llclamp(sModel.programShift + (F32)clicks / 3.f, -8.f, 8.f);
            s.programShift = sModel.programShift;
        }
        else if (s.mode == SLCE::MODE_APERTURE)
        {
            toast("Av: the shutter follows the aperture");
            return;
        }
        else
        {
            s.shutter = SLCE::stepStop(SLCE::shutterStops(), s.shutter, clicks);
        }
        break;
    case Value::Aperture:
        if (s.mode == SLCE::MODE_PROGRAM)
        {
            sModel.programShift = llclamp(sModel.programShift - (F32)clicks / 3.f, -8.f, 8.f);
            s.programShift = sModel.programShift;
        }
        else if (s.mode == SLCE::MODE_SHUTTER)
        {
            toast("Tv: the aperture follows the shutter");
            return;
        }
        else
        {
            s.fnumber = SLCE::stepStop(SLCE::apertureStops(), s.fnumber, clicks);
        }
        break;
    case Value::EV:
        if (s.mode == SLCE::MODE_MANUAL)
        {
            toast("M: exposure comes from the aperture, shutter and ISO");
            return;
        }
        s.evComp = SLCE::stepThirds(s.evComp, clicks, EV_COMP_LO, EV_COMP_HI);
        break;
    case Value::ISO:
        s.iso = SLCE::stepStop(SLCE::isoStops(), s.iso, clicks);
        break;
    case Value::WB:
        setKelvin(SLCE::stepKelvin(currentKelvin(), clicks));
        return;
    case Value::Focal:
        setFocalLength(SLCE::stepFocalLength(currentFocalLength(), clicks));
        return;
    case Value::Tilt:
    {
        // Half a degree per click, for fine levelling.
        const F32 limit = 40.f * DEG_TO_RAD;
        gAgentCamera.setRollAngle(llclamp(gAgentCamera.getRollAngle() + (F32)clicks * 0.5f * DEG_TO_RAD, -limit, limit));
        return;
    }
    case Value::Drive:
        turnDrive(clicks, MASK_NONE);
        return;
    case Value::Focus:
        if (focusMode() == FOCUS_MF)
            turnFocusRing(clicks);
        else
            toast("The focus ring turns in MF (click the focus readout)");
        return;
    default:
        return;
    }
    applySettings(s);
}

// A value turned on purpose (its cell: the wheel over it, its arrows): the value you touch is the
// value you set. In P (both chosen by the camera) or the other priority mode, the shutter switches
// to Tv and the aperture to Av, keeping the exposure; the dials over the picture keep the mode.
void turnValue(Value v, S32 clicks)
{
    if (clicks == 0) return;
    const U32 mode = exposureModeSetting();
    U32 to = mode;
    if (v == Value::Shutter && (mode == SLCE::MODE_PROGRAM || mode == SLCE::MODE_APERTURE))
        to = SLCE::MODE_SHUTTER;
    else if (v == Value::Aperture && (mode == SLCE::MODE_PROGRAM || mode == SLCE::MODE_SHUTTER))
        to = SLCE::MODE_APERTURE;
    if (to != mode)
    {
        SLCE::Settings s = currentSettings();
        s.mode = to;
        sModel.programShift = 0.f;
        s.programShift = 0.f;
        applySettings(s);
        toast(LLTrans::getString(to == SLCE::MODE_SHUTTER ? "CameraModeSwitchedTv" : "CameraModeSwitchedAv"), 3.0);
    }
    turn(v, clicks);
}

// The second dial (Shift+wheel), by mode: exposure, or the aperture in M.
Value rearDial()
{
    return exposureModeSetting() == SLCE::MODE_MANUAL ? Value::Aperture : Value::EV;
}

// ---- the Q menu (#1579): a quick-settings grid ----

enum QItem : S32
{
    Q_MODE = 0,
    Q_ISO,
    Q_WB,
    Q_DRIVE,
    Q_FOCUS,
    Q_LOOK,
    Q_ASPECT,
    Q_GRID,
    Q_COUNT
};

// The Looks (Photo Studio's picture styles), stepped in order.
void stepLook(S32 dir)
{
    LLPresetsManager::preset_name_list_t names;
    LLPresetsManager::getInstance()->loadPresetNamesFromDir(PRESETS_LOOKS, names, DEFAULT_HIDE);
    if (names.empty()) return;
    std::vector<std::string> list(names.begin(), names.end());
    const std::string cur = gSavedSettings.getString("PresetLooksActive");
    S32 index = -1;
    for (size_t i = 0; i < list.size(); ++i)
        if (list[i] == cur) index = (S32)i;
    const S32 n = (S32)list.size();
    index = index < 0 ? (dir > 0 ? 0 : n - 1) : ((index + dir) % n + n) % n;
    LLPresetsManager::getInstance()->loadLooksPreset(list[(size_t)index]);
}

const std::vector<std::string>& qItems()
{
    static const std::vector<std::string> items = { "Mode", "ISO", "White balance", "Drive", "Focus", "Picture style", "Aspect", "Grid" };
    return items;
}

std::string qValue(S32 item)
{
    switch (item)
    {
    case Q_MODE:  return SLCE::modeLabel(exposureModeSetting());
    case Q_ISO:   return llformat("%d", ll_round(currentISO()));
    case Q_WB:
    {
        const F32 k = currentKelvin();
        const char* preset = SLCE::whiteBalancePresetName(k);
        return preset ? std::string(preset) : llformat("%d K", ll_round(k));
    }
    case Q_DRIVE: return driveLabel();
    case Q_FOCUS: return focusModeLabel(focusMode());
    case Q_LOOK:
    {
        const std::string look = gSavedSettings.getString("PresetLooksActive");
        return look.empty() ? std::string("(custom)") : look;
    }
    case Q_ASPECT: return SLCE::aspectLabel(gSavedSettings.getU32("SLCameraModeAspect"));
    default:       return SLCE::gridLabel(gSavedSettings.getU32("SLCameraModeGrid"));
    }
}

void qTurn(S32 item, S32 dir)
{
    if (dir == 0) return;
    switch (item)
    {
    case Q_MODE:  turnMode(dir); break;
    case Q_ISO:   turn(Value::ISO, dir); break;
    case Q_WB:    setKelvin(SLCE::nextWhiteBalancePreset(currentKelvin(), dir)); break;
    case Q_DRIVE: turnDrive(dir, MASK_NONE); break;
    case Q_FOCUS: setFocusMode((focusMode() + (dir > 0 ? 1u : FOCUS_COUNT - 1u)) % FOCUS_COUNT); break;
    case Q_LOOK:  stepLook(dir); break;
    case Q_ASPECT:
        gSavedSettings.setU32("SLCameraModeAspect",
                              (U32)((((S32)gSavedSettings.getU32("SLCameraModeAspect") + dir) % (S32)SLCE::ASPECT_COUNT
                                     + (S32)SLCE::ASPECT_COUNT) % (S32)SLCE::ASPECT_COUNT));
        break;
    default:
        gSavedSettings.setU32("SLCameraModeGrid",
                              (U32)((((S32)gSavedSettings.getU32("SLCameraModeGrid") + dir) % (S32)SLCE::GRID_COUNT
                                     + (S32)SLCE::GRID_COUNT) % (S32)SLCE::GRID_COUNT));
        break;
    }
}


namespace
{
LLPointer<LLImageFormatted> asJpeg(LLImageRaw& raw)
{
    LLPointer<LLImageFormatted> jpeg = new LLImageJPEG(llclamp(gSavedSettings.getS32("SnapshotQuality"), 10, 100));
    jpeg->enableOverSize();
    const bool ok = jpeg->encode(&raw, 0);
    jpeg->disableOverSize();
    return ok ? jpeg : LLPointer<LLImageFormatted>();
}

void noteSent(U32 dest)
{
    sModel.savedAt = now();
    sModel.savedTo = dest;
}
} // namespace

void sendTo(U32 dest, LLImageRaw& raw, const LLVector3d& pos, const LLSD& params)
{
    switch (dest)
    {
    case DEST_INVENTORY:
        // As the classic window's "Save to inventory": the Snapshots folder, priced by its size.
        if (LLSnapshotLivePreview::uploadSnapshotTexture(raw))
            noteSent(dest);
        else
            toast(LLTrans::getString("CameraModeSaveFailed"), 4.0);
        break;
    case DEST_PROFILE:
        if (LLPointer<LLImageFormatted> jpeg = asJpeg(raw))
        {
            LLWebProfile::uploadImage(jpeg, params["caption"].asString(), params["location"].asBoolean());
            noteSent(dest);
        }
        break;
    case DEST_EMAIL:
    {
        const std::string url = gAgent.getRegion() ? gAgent.getRegion()->getCapability("SendPostcard") : std::string();
        LLPointer<LLImageFormatted> jpeg = url.empty() ? LLPointer<LLImageFormatted>() : asJpeg(raw);
        if (!jpeg)
        {
            toast(LLTrans::getString("CameraModeEmailUnavailable"), 4.0);
            break;
        }
        std::string from;
        LLAgentUI::buildFullname(from);
        LLResourceUploadInfo::ptr_t info(std::make_shared<LLPostcardUploadInfo>(
            from, params["to"].asString(), params["subject"].asString(), params["message"].asString(), pos, jpeg,
            [](LLUUID, LLUUID, LLUUID, LLSD response) { LLPostCard::reportPostResult(response["state"].asString() == "complete"); }));
        LLViewerAssetUpload::EnqueueInventoryUpload(url, info);
        noteSent(dest);
        break;
    }
    default:
        break;
    }
}

// ---- the public face ----

U32 destination()
{
    return llmin(gSavedSettings.getU32("SLCameraModeDestination"), (U32)DEST_EMAIL);
}

void setDestination(U32 dest)
{
    gSavedSettings.setU32("SLCameraModeDestination", llmin(dest, (U32)DEST_EMAIL));
}

S32 inventoryCost()
{
    S32 w = 0, h = 0;
    outputSize(w, h);
    return LLSnapshotLivePreview::snapshotTextureCost(w, h);
}

bool pendingShot(U32& dest, S32& cost)
{
    if (sModel.pending.isNull())
        return false;
    dest = sModel.pendingTo;
    cost = dest == DEST_INVENTORY ? LLSnapshotLivePreview::snapshotTextureCost(sModel.pending->getWidth(), sModel.pending->getHeight()) : 0;
    return true;
}

const std::string& lastEmailTo()
{
    return sModel.lastEmailTo;
}

std::string commitPending(const LLSD& params)
{
    if (sModel.pending.isNull())
        return std::string();
    const U32 dest = sModel.pendingTo;
    if (dest == DEST_EMAIL)
    {
        // As the classic postcard: one or more addresses, and a subject.
        static const boost::regex email_format(
            "[A-Za-z0-9.%+-_]+@[A-Za-z0-9.-]+\\.[A-Za-z]{2,}(,[ \t]*[A-Za-z0-9.%+-_]+@[A-Za-z0-9.-]+\\.[A-Za-z]{2,})*");
        const std::string to = params["to"].asString();
        if (to.empty() || !ll_regex_match(to, email_format))
            return LLTrans::getString("CameraModeEmailBadAddress");
        if (params["subject"].asString().empty())
            return LLTrans::getString("CameraModeEmailNoSubject");
        sModel.lastEmailTo = to;
    }
    if (dest < 4 && params["dont_ask"].asBoolean())
        sModel.skipAsk[dest] = true;
    if (dest == DEST_PROFILE)
        sModel.lastAddLocation = params["location"].asBoolean();
    LLPointer<LLImageRaw> raw = sModel.pending;
    sModel.pending = nullptr;
    sendTo(dest, *raw, sModel.pendingPos, params);
    return std::string();
}

void discardPending()
{
    sModel.pending = nullptr;
}

U32 lastSavedTo()
{
    return sModel.savedTo;
}

void windowSize(S32& w, S32& h)
{
    w = gViewerWindow ? gViewerWindow->getWindowWidthRaw() : 0;
    h = gViewerWindow ? gViewerWindow->getWindowHeightRaw() : 0;
}

void outputBase(S32& w, S32& h)
{
    w = gSavedSettings.getS32("SnapshotLocalWidth");
    h = gSavedSettings.getS32("SnapshotLocalHeight");
    if (w <= 0 || h <= 0)
        windowSize(w, h);
}

void setOutputBase(S32 w, S32 h)
{
    gSavedSettings.setS32("SnapshotLocalWidth", llclamp(w, 1, (S32)MAX_SNAPSHOT_IMAGE_SIZE));
    gSavedSettings.setS32("SnapshotLocalHeight", llclamp(h, 1, (S32)MAX_SNAPSHOT_IMAGE_SIZE));
}

bool outputFollowsWindow()
{
    return gSavedSettings.getS32("SnapshotLocalWidth") <= 0 || gSavedSettings.getS32("SnapshotLocalHeight") <= 0;
}

void setOutputFollowsWindow()
{
    gSavedSettings.setS32("SnapshotLocalWidth", 0);
    gSavedSettings.setS32("SnapshotLocalHeight", 0);
}

U32 fileFormat()
{
    return (U32)llclamp(gSavedSettings.getS32("SnapshotFormat"), 0, 2);
}

void setFileFormat(U32 format)
{
    gSavedSettings.setS32("SnapshotFormat", (S32)llmin(format, 2u));
}

S32 jpegQuality()
{
    return llclamp(gSavedSettings.getS32("SnapshotQuality"), 0, 100);
}

void setJpegQuality(S32 quality)
{
    gSavedSettings.setS32("SnapshotQuality", llclamp(quality, 0, 100));
}

bool postEffects()
{
    return !gSavedSettings.getBOOL("RenderSnapshotNoPost");
}

void setPostEffects(bool on)
{
    gSavedSettings.setBOOL("RenderSnapshotNoPost", !on);
}

void begin()
{
    sModel.pending = nullptr;
    // "Don't ask again" holds for one camera-mode session: the next one asks before L$ or a post.
    for (bool& skip : sModel.skipAsk)
        skip = false;
    sModel.savedAt = -100.0;
    sModel.programShift = 0.f;
    // A new session focuses afresh (a held point may belong to another place by now).
    sModel.focusValid = false;
    sModel.focusObject.setNull();
    sModel.afLocked = false;
    sModel.toastUntil = 0.0;
    // <Prestige> no person, group, half-press, confirmation or exposure lock carried over
    sPrestige = PrestigeFocus();
    PCF::clearGroup();
    PCF::clearConfirm();
    PCF::setExposureLocked(false);
    // </Prestige>
}

void end()
{
    setDofPreview(false);
    // <Prestige> the group's aperture let go (the restore puts every dial back anyway), the
    // exposure lock released, and the depth of field as camera mode leaves it remembered for the
    // next session (on by default: PrestigeCameraDepthOfField)
    prestigeEndGroup();
    sPrestige = PrestigeFocus();
    PCF::clearConfirm();
    PCF::setExposureLocked(false);
    gSavedSettings.setBOOL("PrestigeCameraDepthOfField", gSavedSettings.getBOOL("RenderDepthOfField"));
    // </Prestige>
    stopDrive();
    sModel.pending = nullptr; // the card closed with camera mode: its full-size shot goes too
    PrestigeCameraIsolate::endSession();   // <Prestige> isolate: picking off, the picks forgotten
}

bool focusAt(S32 x, S32 y)
{
    const LLRect fr = frame();
    if (!fr.pointInRect(x, y))
        return false;
    // <Prestige> a plain click is one subject again (the group ends), and moves the AF point:
    // the shutter key's half-press is over
    if (!PCF::group().empty())
        prestigeEndGroup();
    sPrestige.halfArmed = false;
    // </Prestige>
    sModel.afX = llclamp((F32)(x - fr.mLeft) / (F32)llmax(fr.getWidth(), 1), 0.f, 1.f);
    sModel.afY = llclamp((F32)(y - fr.mBottom) / (F32)llmax(fr.getHeight(), 1), 0.f, 1.f);
    acquireFocus();
    return true;
}

void afPoint(F32& x, F32& y)
{
    x = sModel.afX;
    y = sModel.afY;
}

void autofocus()
{
    acquireFocus();
}

void halfPress(bool held)
{
    // <Prestige> another half-press (or its release) ends the shutter key's armed one
    sPrestige.halfArmed = false;
    // </Prestige>
    if (held)
        halfPressFocus();
}

void toggleFocusLock()
{
    // AF-L: lock the focus (focusing first when nothing is held), so half-presses keep it;
    // again: unlock and let it go (back to the depth of field's own focus).
    if (sModel.afLocked)
    {
        sModel.afLocked = false;
        sModel.focusValid = false;
        sModel.focusObject.setNull();
        toast("Focus unlocked");
    }
    else
    {
        if (!sModel.focusValid) acquireFocus();
        sModel.afLocked = sModel.focusValid;
        if (sModel.afLocked) toast("Focus locked");
    }
}

bool focusLocked()
{
    return sModel.afLocked;
}

bool dofPreviewHeld()
{
    return sModel.dofHeld;
}

bool moveAfPoint(S32 x, S32 y)
{
    const LLRect fr = frame();
    if (fr.isEmpty())
        return false;
    // <Prestige> moving the AF point ends the shutter key's half-press
    sPrestige.halfArmed = false;
    // </Prestige>
    sModel.afX = llclamp((F32)(x - fr.mLeft) / (F32)llmax(fr.getWidth(), 1), 0.f, 1.f);
    sModel.afY = llclamp((F32)(y - fr.mBottom) / (F32)llmax(fr.getHeight(), 1), 0.f, 1.f);
    return true;
}

bool focusHeld()
{
    return sModel.focusValid;
}

void tick()
{
    driveTick();
    // <Prestige> focus confirmation (green, the double beep); AF-C: the group's aperture follows
    // it (4 Hz); the exposure lock lets go in M (nothing automatic to hold)
    PCF::tick();
    if (!PCF::group().empty() && focusMode() == FOCUS_AF_C && sModel.focusValid && now() >= sPrestige.nextGroupCheck)
    {
        sPrestige.nextGroupCheck = now() + 0.25;
        F32 near_m = 0.f, far_m = 0.f;
        if (PCF::groupDepths(near_m, far_m))
            prestigeGroupAperture(near_m, far_m);
    }
    if (PCF::exposureLocked() && exposureModeSetting() == SLCE::MODE_MANUAL)
        PCF::setExposureLocked(false);
    // </Prestige>
}

bool driving()
{
    return sDrive.running;
}

std::string driveStatus()
{
    if (!sDrive.running) return std::string();
    const F64 wait = sDrive.startAt - now();
    if (wait > 0.0)
        return llformat("%d", (S32)std::ceil(wait));
    if (sDrive.offsets.size() > 1)
        return llformat("%d / %d", (S32)llmin(sDrive.next + 1, sDrive.offsets.size()), (S32)sDrive.offsets.size());
    return std::string();
}

void toggle(Value v)
{
    switch (v)
    {
    case Value::Shutter:
        gSavedSettings.setBOOL("RenderVKMotionBlur", !gSavedSettings.getBOOL("RenderVKMotionBlur"));
        break;
    case Value::Aperture:
        gSavedSettings.setBOOL("RenderDepthOfField", !gSavedSettings.getBOOL("RenderDepthOfField"));
        break;
    case Value::WB:
        gSavedSettings.setBOOL("RenderColorGrade", !whiteBalanceOn());
        break;
    default:
        break;
    }
}

void step(Value v, S32 dir)
{
    if (dir == 0) return;
    switch (v)
    {
    case Value::Mode:
        turnMode(dir);
        break;
    case Value::WB:
        setKelvin(SLCE::nextWhiteBalancePreset(currentKelvin(), dir));
        break;
    case Value::Focal:
    {
        // The next (or previous) prime, wrapping.
        const std::vector<F32>& primes = SLCE::primeFocalLengths();
        const F32 f = currentFocalLength();
        F32 next = dir > 0 ? primes.front() : primes.back();
        if (dir > 0)
        {
            for (F32 p : primes)
                if (p > f + 0.5f) { next = p; break; }
        }
        else
        {
            for (auto it = primes.rbegin(); it != primes.rend(); ++it)
                if (*it < f - 0.5f) { next = *it; break; }
        }
        setFocalLength(next);
        break;
    }
    case Value::Focus:
        setFocusMode((focusMode() + (dir > 0 ? 1u : FOCUS_COUNT - 1u)) % FOCUS_COUNT); // AF-S -> AF-C -> MF
        break;
    case Value::Drive:
        turnDrive(dir, MASK_NONE);
        break;
    default:
        turn(v, dir);
        break;
    }
}

void reset(Value v)
{
    if (v == Value::EV && exposureMode() != SLCE::MODE_MANUAL)
    {
        SLCE::Settings s = currentSettings();
        s.evComp = 0.f;
        applySettings(s);
    }
    else if (v == Value::ISO)
    {
        SLCE::Settings s = currentSettings();
        s.iso = 100.f;
        applySettings(s);
    }
}

U32 grid()
{
    static LLCachedControl<U32> s_grid(gSavedSettings, "SLCameraModeGrid", 1);
    return llmin((U32)s_grid, (U32)SLCE::GRID_COUNT - 1);
}

void setGrid(U32 g)
{
    gSavedSettings.setU32("SLCameraModeGrid", g % SLCE::GRID_COUNT);
}

void stepGrid(S32 dir)
{
    const S32 n = (S32)SLCE::GRID_COUNT;
    setGrid((U32)((((S32)grid() + dir) % n + n) % n));
}

U32 aspect()
{
    static LLCachedControl<U32> s_aspect(gSavedSettings, "SLCameraModeAspect", 0);
    return llmin((U32)s_aspect, (U32)SLCE::ASPECT_COUNT - 1);
}

void setAspect(U32 a)
{
    gSavedSettings.setU32("SLCameraModeAspect", a % SLCE::ASPECT_COUNT);
}

void stepAspect(S32 dir)
{
    const S32 n = (S32)SLCE::ASPECT_COUNT;
    setAspect((U32)((((S32)aspect() + dir) % n + n) % n));
}

bool level()
{
    static LLCachedControl<bool> s_level(gSavedSettings, "SLCameraModeLevel", true);
    return s_level;
}

void setLevel(bool on)
{
    gSavedSettings.setBOOL("SLCameraModeLevel", on);
}

F32 tiltLimit()
{
    return 40.f * DEG_TO_RAD;
}

void levelAngles(F32& roll, F32& pitch)
{
    const LLViewerCamera* cam = LLViewerCamera::getInstance();
    roll  = asinf(llclamp(cam->getLeftAxis().mV[VZ], -1.f, 1.f));
    pitch = asinf(llclamp(cam->getAtAxis().mV[VZ], -1.f, 1.f));
}

F32 levelTolerance()
{
    return 0.5f * DEG_TO_RAD;
}

F32 tilt()
{
    return gAgentCamera.getRollAngle();
}

void setTilt(F32 radians)
{
    gAgentCamera.setRollAngle(llclamp(radians, -tiltLimit(), tiltLimit()));
}

const std::string& toastText()
{
    static const std::string none;
    return now() < sModel.toastUntil ? sModel.toast : none;
}

F64 lastSavedAt()
{
    return sModel.savedAt;
}

bool flashOn()
{
    static LLCachedControl<bool> s_flash(gSavedSettings, "SLCameraModeFlash", true);
    return s_flash;
}

void setFlashOn(bool on)
{
    gSavedSettings.setBOOL("SLCameraModeFlash", on);
}

F32 flash()
{
    if (!flashOn())
        return 0.f;
    const F64 t = now();
    return t < sModel.flashUntil ? (F32)((sModel.flashUntil - t) / 0.12) : 0.f;
}

U32 exposureMode() { return exposureModeSetting(); }
F32 shutter() { return currentShutter(); }
F32 fnumber()
{
    static LLCachedControl<F32> s_fnumber(gSavedSettings, "CameraFNumber", 9.f);
    return s_fnumber;
}
F32 iso() { return currentISO(); }
F32 exposureStops()
{
    return exposureMode() == SLCE::MODE_MANUAL ? SLCE::solve(currentSettings()).exposureStops   // the manual meter
                                               : std::log2(currentExposureGain());              // the compensation
}
F32 kelvin() { return currentKelvin(); }
F32 focalLength() { return currentFocalLength(); }

namespace
{
std::string word(const char* name)
{
    return LLTrans::getString(std::string("CameraModeName") + name);
}
} // namespace

namespace
{
F32 placeIn(const std::vector<F32>& stops, F32 value)
{
    if (stops.size() < 2)
        return 0.5f;
    size_t best = 0;
    for (size_t i = 1; i < stops.size(); ++i)
        if (std::fabs(std::log(stops[i] / value)) < std::fabs(std::log(stops[best] / value)))
            best = i;
    return (F32)best / (F32)(stops.size() - 1);
}
} // namespace

F32 position(Value v)
{
    switch (v)
    {
    case Value::Shutter:  return placeIn(SLCE::shutterStops(), shutter());
    case Value::Aperture: return placeIn(SLCE::apertureStops(), fnumber());
    case Value::ISO:      return placeIn(SLCE::isoStops(), iso());
    case Value::EV:       return llclamp((exposureStops() - EV_COMP_LO) / (EV_COMP_HI - EV_COMP_LO), 0.f, 1.f);
    case Value::WB:       return llclamp((kelvin() - SLCE::WB_MIN_K) / (SLCE::WB_MAX_K - SLCE::WB_MIN_K), 0.f, 1.f);
    case Value::Focal:    return llclamp(std::log(llmax(focalLength(), 1.f) / 10.f) / std::log(30.f), 0.f, 1.f); // 10 .. 300 mm
    case Value::Tilt:     return llclamp(0.5f + tilt() / (2.f * tiltLimit()), 0.f, 1.f);
    case Value::Focus:    return focusMode() == 2 ? llclamp(std::log(manualFocusDistance() / 0.1f) / std::log(10000.f), 0.f, 1.f) : -1.f;
    default:              return -1.f;
    }
}

Cell cell(Value v)
{
    static LLCachedControl<bool> s_mb(gSavedSettings, "RenderVKMotionBlur", false);
    static LLCachedControl<bool> s_dof(gSavedSettings, "RenderDepthOfField", false);
    Cell c;
    switch (v)
    {
    case Value::Mode:
    {
        static const char* const names[] = { "Program", "AperturePriority", "ShutterPriority", "Manual" };
        const U32 mode = exposureMode();
        c.value = SLCE::modeLabel(mode);
        c.name  = word(names[llmin(mode, 3u)]);
        break;
    }
    case Value::Shutter:
        c.value = SLCE::shutterLabel(shutter());
        c.unit  = "s";
        c.name  = word(s_mb ? "MotionBlurOn" : "MotionBlurOff");
        c.off   = !s_mb;
        break;
    case Value::Aperture:
        c.value = SLCE::apertureLabel(fnumber());
        c.name  = word(s_dof ? "DepthOn" : "DepthOff");
        c.off   = !s_dof;
        break;
    case Value::EV:
    {
        const std::string ev = SLCE::evLabel(exposureStops());
        c.value = ev == "0.0" ? std::string("±0") : ev;
        c.unit  = "EV";
        c.name  = word(exposureMode() == SLCE::MODE_MANUAL ? "Meter" : "Exposure");
        break;
    }
    case Value::ISO:
        c.value = llformat("ISO %d", ll_round(iso()));
        c.name  = word("Sensitivity");
        break;
    case Value::WB:
        c.value = llformat("%d", ll_round(kelvin()));
        c.unit  = "K";
        c.name  = word(whiteBalanceOn() ? "WhiteBalance" : "WhiteBalanceOff");
        c.off   = !whiteBalanceOn();
        break;
    case Value::Focal:
        c.value = llformat("%d", ll_round(focalLength()));
        c.unit  = "mm";
        c.name  = word("Lens");
        break;
    case Value::Focus:
    {
        const U32 fmode = focusMode();
        bool held = fmode == FOCUS_MF;
        F32 d = manualFocusDistance();
        if (fmode != FOCUS_MF)
        {
            LLVector3 p;
            held = focusPoint(p);
            if (held) d = focusDistanceTo(p);
        }
        c.value = focusModeLabel(fmode);
        if (held)
        {
            LLStringUtil::format_map_t args;
            args["[DISTANCE]"] = d > 200.f ? LLTrans::getString("CameraModeFocusFar") : llformat(d < 10.f ? "%.1f m" : "%.0f m", d);
            c.name = LLTrans::getString(sModel.afLocked && fmode != FOCUS_MF ? "CameraModeNameFocusLocked" : "CameraModeNameFocusAt", args);
        }
        else
            c.name = word("FocusNone");
        break;
    }
    case Value::Drive:
        c.value = driveLabel();
        c.name  = word("Shooting");
        break;
    case Value::Tilt:
        c.value = llformat("%+.1f°", tilt() * RAD_TO_DEG);
        c.name  = word("Tilt");
        break;
    }
    c.marked = false; // (no dial: the wheel turns the value under the pointer)
    return c;
}

bool focusPoint(LLVector3& agent_pos)
{
    if (!SLCameraMode::active()) return false;
    const U32 mode = focusMode();
    if (mode == FOCUS_MF)
    {
        // The ring: a point at that distance straight ahead.
        const LLViewerCamera* cam = LLViewerCamera::getInstance();
        agent_pos = cam->getOrigin() + cam->getAtAxis() * manualFocusDistance();
        return true;
    }
    if (!sModel.focusValid) return false;
    // <Prestige> AF-C: a person's eyes, or the group, followed every frame
    if (mode == FOCUS_AF_C && prestigeTrack(agent_pos))
        return true;
    // </Prestige>
    if (mode == FOCUS_AF_C && sModel.focusObject.notNull())
    {
        // Follow the subject: its focus point, where the subject now is.
        if (LLViewerObject* obj = gObjectList.findObject(sModel.focusObject))
        {
            agent_pos = obj->getPositionAgent() + sModel.focusOffset * obj->getRenderRotation();
            sModel.focusGlobal = gAgent.getPosGlobalFromAgent(agent_pos);
            return true;
        }
        sModel.focusObject.setNull();   // gone: hold where it was last
    }
    agent_pos = gAgent.getPosAgentFromGlobal(sModel.focusGlobal);
    return true;
}

// <Prestige> focus and exposure: the public face (slcameramodel.h)
void applySessionDefaults()
{
    // Depth of field on in camera mode, unless it was turned off there last time.
    static LLCachedControl<bool> s_dof(gSavedSettings, "PrestigeCameraDepthOfField", true);
    if (s_dof && !gSavedSettings.getBOOL("RenderDepthOfField"))
        gSavedSettings.setBOOL("RenderDepthOfField", true);
}

void shutterKey()
{
    // The second press, or a press with nothing to focus (MF: the ring; AF-L: the held focus) or
    // that cancels a running sequence: the shutter, as before.
    if (sDrive.running || sPrestige.halfArmed || focusMode() == FOCUS_MF || sModel.afLocked)
    {
        sPrestige.halfArmed = false;
        pressShutter();
        return;
    }
    halfPress(true);   // focus (and the confirmation); the next press shoots
    sPrestige.halfArmed = true;
}

bool halfPressArmed()
{
    return sPrestige.halfArmed;
}

void cancelHalfPress()
{
    sPrestige.halfArmed = false;
}

bool groupFocusAt(S32 x, S32 y)
{
    const LLRect fr = frame();
    if (!fr.pointInRect(x, y))
        return false;
    sPrestige.halfArmed = false;
    if (focusMode() == FOCUS_MF)
    {
        toast(LLTrans::getString("PrestigeCameraGroupNeedsAF"), 3.0);
        return true;
    }
    LLVector4a hit;
    hit.clear();
    LLViewerObject* obj = gViewerWindow->cursorIntersect(x, y, 512.f, nullptr, -1, false, true, true, false, nullptr,
                                                         nullptr, nullptr, &hit);
    LLVOAvatar* av = PCF::personOf(obj);
    if (!av)
        av = PCF::faceNear(x, y, kAfAreaHalf, obj ? focusDistanceTo(LLVector3(hit.getF32ptr())) : 512.f);
    if (!av)
    {
        toast(LLTrans::getString("PrestigeCameraGroupNoPerson"), 3.0);
        return true;
    }
    // The person focused before the first Shift+click joins the group first.
    const LLUUID seed = sModel.focusValid ? sPrestige.person : LLUUID::null;
    const bool was_in = PCF::inGroup(av->getID());
    if (!PCF::toggle(av->getID(), seed) && !was_in)
    {
        LLStringUtil::format_map_t args;
        args["[COUNT]"] = llformat("%d", (S32)PCF::MAX_GROUP);
        toast(LLTrans::getString("PrestigeCameraGroupFull", args), 3.0);
        return true;
    }
    if (PCF::group().empty())
    {
        prestigeEndGroup();   // the last one taken out: the focus stays where it is
        toast(LLTrans::getString("PrestigeCameraGroupOff"), 2.0);
        return true;
    }
    acquireFocus();
    LLStringUtil::format_map_t args;
    args["[COUNT]"] = llformat("%d", (S32)PCF::group().size());
    toast(LLTrans::getString("PrestigeCameraGroupCount", args), 2.0);
    return true;
}

size_t groupSize()
{
    return PCF::group().size();
}

void groupMarkers(std::vector<GroupMarker>& out)
{
    out.clear();
    if (!SLCameraMode::active() || focusMode() == FOCUS_MF)
        return;
    for (const LLUUID& id : PCF::group())
    {
        LLVector3 eyes;
        GroupMarker m;
        if (PCF::eyesOf(id, eyes) && PCF::toScreen(eyes, m.x, m.y))
            out.push_back(m);
    }
}

U32 focusConfirm()
{
    if (focusMode() == FOCUS_MF)
        return PCF::CONFIRM_NONE;
    const U32 c = PCF::confirm();
    return (c == PCF::CONFIRM_FAILED || sModel.focusValid) ? c : (U32)PCF::CONFIRM_NONE;
}

void toggleExposureLock()
{
    if (PCF::exposureLocked())
    {
        PCF::setExposureLocked(false);
        toast(LLTrans::getString("PrestigeCameraExposureUnlocked"));
        return;
    }
    // M (and the renderer's auto exposure off): nothing automatic to hold.
    if (exposureModeSetting() == SLCE::MODE_MANUAL || !gSavedSettings.getBOOL("RenderDynamicExposureEnabled"))
    {
        toast(LLTrans::getString("PrestigeCameraExposureFixed"), 3.0);
        return;
    }
    PCF::setExposureLocked(true);
    toast(LLTrans::getString("PrestigeCameraExposureLocked"));
}

bool exposureLocked()
{
    return SLCameraMode::active() && PCF::exposureLocked();
}

bool focusPeaking()
{
    static LLCachedControl<bool> s_peaking(gSavedSettings, "PrestigeCameraFocusPeaking", true);
    return s_peaking && SLCameraMode::active() && focusMode() == FOCUS_MF;
}
// </Prestige>

} // namespace SLCameraModel
