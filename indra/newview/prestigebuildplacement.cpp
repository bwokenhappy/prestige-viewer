/**
 * @file prestigebuildplacement.cpp
 * @brief Build helpers: Place on Surface (Build menu, P) with a see-through
 *        preview, and Snap to Objects for the Build tool's move arrows.
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

#include "prestigebuildplacement.h"

#include "llagent.h"
#include "llagentcamera.h"
#include "llbbox.h"
#include "llcallbacklist.h"
#include "llfocusmgr.h"
#include "llkeyboard.h"
#include "llmanip.h"
#include "llmenugl.h"
#include "llnotificationsutil.h"
#include "llprimitive.h"
#include "llrender.h"
#include "llselectmgr.h"
#include "lltool.h"
#include "lltoolmgr.h"
#include "llui.h"
#include "llviewercontrol.h"
#include "llviewerobject.h"
#include "llviewerobjectlist.h"
#include "llviewerwindow.h"
#include "llvolume.h"
#include "llvovolume.h"
#include "llmeshrepository.h"
#include "llworld.h"
#include "rlvhandler.h"
#include "rlvmodifiers.h"

#include <array>
#include <cmath>
#include <utility>
#include <vector>

namespace
{
    // How far along the mouse ray a surface is looked for (as the other picks).
    constexpr F32 PICK_DEPTH = 512.f;
    // The preview draws one box per prim up to this many, else one box round them all.
    constexpr size_t MAX_PREVIEW_BOXES = 256;
    // Snap to Objects: objects whose box is farther than this from the selection's
    // box when the drag starts are left out.
    constexpr F32 SNAP_REACH = 64.f;
    constexpr size_t SNAP_MAX_NEIGHBORS = 8192;

    // Prestige orange (#F28C28).
    const LLColor4 PREVIEW_COLOR(0.949f, 0.549f, 0.157f, 1.f);

    typedef std::array<LLVector3, 8> corners_t;

    void bbox_corners(const LLBBox& box, corners_t& out)
    {
        const LLVector3& lo = box.getMinLocal();
        const LLVector3& hi = box.getMaxLocal();
        for (S32 i = 0; i < 8; ++i)
        {
            const LLVector3 local((i & 1) ? hi.mV[VX] : lo.mV[VX],
                                  (i & 2) ? hi.mV[VY] : lo.mV[VY],
                                  (i & 4) ? hi.mV[VZ] : lo.mV[VZ]);
            out[i] = box.localToAgent(local);
        }
    }

    void grow(LLVector3& lo, LLVector3& hi, const LLVector3& p)
    {
        for (S32 k = 0; k < 3; ++k)
        {
            lo.mV[k] = llmin(lo.mV[k], p.mV[k]);
            hi.mV[k] = llmax(hi.mV[k], p.mV[k]);
        }
    }

    //------------------------------------------------------------------------
    // Place on Surface
    //------------------------------------------------------------------------

    enum ESession
    {
        SESSION_NONE = 0,
        SESSION_HOLD,   // P is held: the preview follows the mouse, release places
        SESSION_AIM     // from the menu: the preview follows the mouse, a click places
    };

    enum EFail
    {
        FAIL_NONE = 0,
        FAIL_SELECTION,     // nothing (movable) selected any more
        FAIL_NO_SURFACE,    // no surface under the mouse, or only the selection itself
        FAIL_TOO_FAR        // farther than the Build tool lets you drag in one go
    };

    struct Placement
    {
        bool mValid = false;
        bool mRotate = false;           // turn the selection (Align to surface)
        bool mAlignSkipped = false;     // Align to surface asked for, but parts are selected
        LLQuaternion mRotation;         // turn about mPivot first...
        LLVector3 mPivot;               // (agent)
        LLVector3 mMove;                // ...then move by this (agent)
        F32 mMaxDistance = 0.f;         // for the "too far" message
        LLUUID mFirstID;                // which selection this was worked out for
        S32 mObjectCount = 0;
        std::vector<corners_t> mBoxes;  // where the boxes will be (the preview)
    };

    ESession sSession = SESSION_NONE;
    KEY sHoldKey = KEY_NONE;
    Placement sPreview;
    bool sIdleRegistered = false;
    bool sAimDeselected = false;

    void on_idle(void*);

    // The click-to-place tool (Build > Place on Surface clicked): a transient tool,
    // so clicks on windows still go to the windows and only world clicks reach it.
    class PrestigeToolPlaceAim : public LLTool
    {
    public:
        PrestigeToolPlaceAim() : LLTool("PlaceOnSurface") {}

        bool handleMouseDown(S32 x, S32 y, MASK mask) override
        {
            // Hold the mouse until it is released, so the click's mouse-up does
            // not reach the Build tool (it would select what is under it).
            setMouseCapture(true);
            PrestigePlaceOnSurface::placeAt(x, y);
            return true;
        }
        bool handleMouseUp(S32 x, S32 y, MASK mask) override
        {
            setMouseCapture(false);
            return true;
        }
        bool handleDoubleClick(S32 x, S32 y, MASK mask) override { return true; }
        bool handleRightMouseDown(S32 x, S32 y, MASK mask) override
        {
            setMouseCapture(true);
            PrestigePlaceOnSurface::cancel();
            return true;
        }
        bool handleRightMouseUp(S32 x, S32 y, MASK mask) override
        {
            setMouseCapture(false);
            return true;
        }
        bool handleHover(S32 x, S32 y, MASK mask) override
        {
            gViewerWindow->setCursor(UI_CURSOR_CROSS);
            return true;
        }
        bool handleKey(KEY key, MASK mask) override
        {
            if (key == KEY_ESCAPE)
            {
                PrestigePlaceOnSurface::cancel();
                return true;
            }
            return false;
        }
        void handleDeselect() override
        {
            PrestigePlaceOnSurface::onAimToolDeselected();
        }
    };

    PrestigeToolPlaceAim* aim_tool()
    {
        // Made once and kept for the whole session (tools are never deleted while in use).
        static PrestigeToolPlaceAim* tool = new PrestigeToolPlaceAim();
        return tool;
    }

    bool aim_tool_is_current()
    {
        return LLToolMgr::instanceExists() && LLToolMgr::getInstance()->usingTransientTool()
            && LLToolMgr::getInstance()->getCurrentTool() == aim_tool();
    }

    void selection_signature(LLUUID& first_id, S32& count)
    {
        LLObjectSelectionHandle sel = LLSelectMgr::getInstance()->getSelection();
        LLViewerObject* first = sel->getFirstRootObject(true);
        first_id = first ? first->getID() : LLUUID::null;
        count = sel->getObjectCount();
    }

    // The surface under screen point (x, y): its point and its normal (agent space,
    // the normal turned towards the camera). Not the selection itself, avatars or
    // attachments.
    bool pick_surface(S32 x, S32 y, LLVector3& pos, LLVector3& normal)
    {
        LLVector4a hit_pos, hit_normal;
        hit_pos.clear();
        hit_normal.clear();
        S32 face = -1;
        LLViewerObject* hit = gViewerWindow->cursorIntersect(x, y, PICK_DEPTH, nullptr, -1,
                                                             false,   // pick_transparent
                                                             false,   // pick_rigged
                                                             true,    // pick_unselectable
                                                             false,   // pick_reflection_probe
                                                             &face, nullptr, nullptr, &hit_pos, nullptr, &hit_normal);
        if (hit)
        {
            if (hit->isSelected() || hit->isAvatar() || hit->isAttachment() || hit->isHUDAttachment())
            {
                return false;
            }
            pos.set(hit_pos.getF32ptr());
            normal.set(hit_normal.getF32ptr());
            if (hit->getPCode() == LLViewerObject::LL_VO_SURFACE_PATCH && normal.isExactlyZero())
            {
                normal = LLWorld::getInstance()->resolveLandNormalGlobal(gAgent.getPosGlobalFromAgent(pos));
            }
        }
        else
        {
            // Nothing drawn under the mouse answered (e.g. no recent answer yet): the ground.
            LLVector3d land_global;
            if (!gViewerWindow->mousePointOnLandGlobal(x, y, &land_global, false))
            {
                return false;
            }
            pos = gAgent.getPosAgentFromGlobal(land_global);
            normal = LLWorld::getInstance()->resolveLandNormalGlobal(land_global);
        }

        if (normal.normalize() < 0.001f)
        {
            normal = LLVector3::z_axis;
        }
        const LLVector3 ray_dir = gViewerWindow->mouseDirectionGlobal(x, y);
        if (normal * ray_dir > 0.f)
        {
            normal = -normal;   // the side of the surface the camera sees
        }
        return true;
    }

    // Align to surface turns whole linksets only: every object that moves must be
    // a linkset root picked as a whole (not Edit Linked parts).
    bool can_turn_selection()
    {
        LLObjectSelectionHandle sel = LLSelectMgr::getInstance()->getSelection();
        for (LLObjectSelection::iterator it = sel->begin(); it != sel->end(); ++it)
        {
            LLSelectNode* node = *it;
            LLViewerObject* obj = node ? node->getObject() : nullptr;
            if (!obj)
            {
                continue;
            }
            if (node->mIndividualSelection || obj->isAttachment())
            {
                return false;
            }
        }
        return true;
    }

    // Where the selection goes for screen point (x, y).
    bool compute_placement(S32 x, S32 y, Placement& out, EFail& why, bool want_boxes)
    {
        out = Placement();
        why = FAIL_NONE;

        LLSelectMgr* sm = LLSelectMgr::getInstance();
        LLObjectSelectionHandle sel = sm->getSelection();
        if (sel->isEmpty() || sel->getSelectType() != SELECT_TYPE_WORLD)
        {
            why = FAIL_SELECTION;
            return false;
        }

        // The selection's prims as boxes (agent space).
        std::vector<corners_t> boxes;
        LLVector3 lo(F32_MAX, F32_MAX, F32_MAX);
        LLVector3 hi(-F32_MAX, -F32_MAX, -F32_MAX);
        for (LLObjectSelection::iterator it = sel->begin(); it != sel->end(); ++it)
        {
            LLSelectNode* node = *it;
            LLViewerObject* obj = node ? node->getObject() : nullptr;
            if (!obj || obj->isDead())
            {
                continue;
            }
            if (obj->isAvatar() || node->mSelectedGLTFNode != -1)
            {
                why = FAIL_SELECTION;
                return false;
            }
            corners_t c;
            bbox_corners(obj->getBoundingBoxAgent(), c);
            for (const LLVector3& p : c)
            {
                grow(lo, hi, p);
            }
            boxes.push_back(c);
        }
        if (boxes.empty())
        {
            why = FAIL_SELECTION;
            return false;
        }

        LLVector3 surface_pos, n;
        if (!pick_surface(x, y, surface_pos, n))
        {
            why = FAIL_NO_SURFACE;
            return false;
        }

        // Align to surface: turn the chosen side of the selection to face the surface.
        out.mPivot = (lo + hi) * 0.5f;
        if (gSavedSettings.getBOOL("PrestigePlaceOnSurfaceAlign"))
        {
            if (can_turn_selection())
            {
                static const LLVector3 SIDES[6] =
                {
                    LLVector3(0.f, 0.f, -1.f),  // bottom
                    LLVector3(0.f, 0.f, 1.f),   // top
                    LLVector3(-1.f, 0.f, 0.f),  // back
                    LLVector3(1.f, 0.f, 0.f),   // front
                    LLVector3(0.f, -1.f, 0.f),  // right
                    LLVector3(0.f, 1.f, 0.f)    // left
                };
                const S32 side = llclamp(gSavedSettings.getS32("PrestigePlaceOnSurfaceSide"), 0, 5);
                // The selection's own axes: its box turns with the root prim.
                const LLVector3 side_agent = SIDES[side] * sm->getBBoxOfSelection().getRotation();
                out.mRotation.shortestArc(side_agent, -n);
                out.mRotate = true;
            }
            else
            {
                out.mAlignSkipped = true;
            }
        }

        if (out.mRotate)
        {
            lo.setVec(F32_MAX, F32_MAX, F32_MAX);
            hi.setVec(-F32_MAX, -F32_MAX, -F32_MAX);
            for (corners_t& c : boxes)
            {
                for (LLVector3& p : c)
                {
                    p = out.mPivot + (p - out.mPivot) * out.mRotation;
                    grow(lo, hi, p);
                }
            }
        }

        // The selection's lowest point as seen from the surface goes onto the surface's
        // plane, plus the offset; its middle goes over the point under the mouse.
        F32 lowest = F32_MAX;
        for (const corners_t& c : boxes)
        {
            for (const LLVector3& p : c)
            {
                lowest = llmin(lowest, p * n);
            }
        }
        const LLVector3 middle = (lo + hi) * 0.5f;
        const F32 height = (middle * n) - lowest + gSavedSettings.getF32("PrestigePlaceOnSurfaceOffset");
        out.mMove = surface_pos + n * height - middle;

        // As far as the Build tool's own move goes in one drag, when that is limited.
        if (gSavedSettings.getBOOL("LimitDragDistance"))
        {
            const F32 max_dist = gSavedSettings.getF32("MaxDragDistance");
            if (out.mMove.lengthSquared() > max_dist * max_dist)
            {
                out.mMaxDistance = max_dist;
                why = FAIL_TOO_FAR;
                return false;
            }
        }
// [RLVa:KB] - as the Create tool: no placing beyond the fartouch distance
        if (gRlvHandler.hasBehaviour(RLV_BHVR_FARTOUCH))
        {
            static RlvCachedBehaviourModifier<float> s_nFartouchDist(RLV_MODIFIER_FARTOUCHDIST);
            if (dist_vec_squared(gAgent.getPositionAgent(), surface_pos) > s_nFartouchDist * s_nFartouchDist)
            {
                why = FAIL_NO_SURFACE;
                return false;
            }
        }
// [/RLVa:KB]

        if (want_boxes)
        {
            if (boxes.size() <= MAX_PREVIEW_BOXES)
            {
                out.mBoxes.reserve(boxes.size());
                for (const corners_t& c : boxes)
                {
                    corners_t moved;
                    for (S32 i = 0; i < 8; ++i)
                    {
                        moved[i] = c[i] + out.mMove;
                    }
                    out.mBoxes.push_back(moved);
                }
            }
            else
            {
                // Too many prims to draw one by one: one box round them all.
                corners_t all;
                for (S32 i = 0; i < 8; ++i)
                {
                    all[i].setVec((i & 1) ? hi.mV[VX] : lo.mV[VX],
                                  (i & 2) ? hi.mV[VY] : lo.mV[VY],
                                  (i & 4) ? hi.mV[VZ] : lo.mV[VZ]);
                    all[i] += out.mMove;
                }
                out.mBoxes.push_back(all);
            }
        }

        selection_signature(out.mFirstID, out.mObjectCount);
        out.mValid = true;
        return true;
    }

    // Moves (and turns) the selection the way the Build tool's arrows do, then sends
    // one update for it, which the region keeps for Undo.
    void apply_placement(const Placement& p)
    {
        LLSelectMgr* sm = LLSelectMgr::getInstance();
        LLObjectSelectionHandle sel = sm->getSelection();
        if (sel->isEmpty())
        {
            return;
        }
        sm->saveSelectedObjectTransform(SELECT_ACTION_TYPE_MOVE);

        for (LLObjectSelection::iterator it = sel->begin(); it != sel->end(); ++it)
        {
            LLSelectNode* node = *it;
            LLViewerObject* obj = node ? node->getObject() : nullptr;
            if (!obj || obj->isDead() || node->mSelectedGLTFNode != -1)
            {
                continue;
            }
            // Only linkset roots and parts picked one by one move; the rest go along.
            if (!obj->isRootEdit() && !node->mIndividualSelection)
            {
                continue;
            }
            if (!obj->isRootEdit())
            {
                LLViewerObject* parent = (LLViewerObject*)obj->getParent();
                if (parent && parent->isSelected())
                {
                    continue;   // moved with its parent
                }
            }
            LLViewerObject* root = obj->getRootEdit();
            if (!obj->permMove() || obj->isPermanentEnforced() || (root && root->isPermanentEnforced()))
            {
                continue;
            }

            const LLVector3 old_agent = obj->getPositionAgent();
            LLVector3 new_agent = old_agent;
            if (p.mRotate && obj->isRootEdit())
            {
                new_agent = p.mPivot + (old_agent - p.mPivot) * p.mRotation;
                obj->setRotation(obj->getRotation() * p.mRotation);
            }
            new_agent += p.mMove;

            LLVector3d new_global = gAgent.getPosGlobalFromAgent(new_agent);
            // As the Build tool: not too far underground, not above the height limit.
            const F64 min_height = LLWorld::getInstance()->getMinAllowedZ(obj, obj->getPositionGlobal());
            if (new_global.mdV[VZ] < min_height)
            {
                new_global.mdV[VZ] = min_height;
            }
            if (new_global.mdV[VZ] > MAX_OBJECT_Z)
            {
                new_global.mdV[VZ] = MAX_OBJECT_Z;
            }
            if (obj->getPCode() == LL_PCODE_LEGACY_GRASS)
            {
                new_global.mdV[VZ] = LLWorld::getInstance()->resolveLandHeightGlobal(new_global) + 1.f;
            }
            if (obj->isRootEdit())
            {
                new_global = LLWorld::getInstance()->clipToVisibleRegions(obj->getPositionGlobal(), new_global);
            }
            new_agent = gAgent.getPosAgentFromGlobal(new_global);

            if (obj->isRootEdit())
            {
                obj->setPositionAgent(new_agent);
            }
            else if (root)
            {
                LLVector3 local = new_agent - root->getPositionAgent();
                local = local * ~root->getRotation();
                obj->setPositionParent(local, false);
            }
            LLManip::rebuild(obj);

            if (node->mIndividualSelection)
            {
                // A root moved on its own: its parts stay where they are.
                obj->resetChildrenPosition(old_agent - new_agent, true);
            }
            node->mLastPositionLocal = obj->getPosition();
        }

        sm->updateSelectionCenter();
        gAgentCamera.clearFocusObject();   // as the Build tool's move: the camera does not follow it
        U32 update = UPD_POSITION;
        if (p.mRotate)
        {
            update |= UPD_ROTATION;
        }
        sm->sendMultipleUpdate(update);
        sm->saveSelectedObjectTransform(SELECT_ACTION_TYPE_PICK);
        dialog_refresh_all();
    }

    void notify_failure(EFail why, F32 max_distance)
    {
        if (why == FAIL_NO_SURFACE)
        {
            LLNotificationsUtil::add("PrestigePlaceOnSurfaceNoSurface");
        }
        else if (why == FAIL_TOO_FAR)
        {
            LLSD args;
            args["DISTANCE"] = llformat("%.0f", max_distance);
            LLNotificationsUtil::add("PrestigePlaceOnSurfaceTooFar", args);
        }
    }

    void place_now(S32 x, S32 y, bool use_preview)
    {
        Placement p;
        EFail why = FAIL_NONE;
        bool ok = compute_placement(x, y, p, why, false);
        if (!ok && why == FAIL_NO_SURFACE && use_preview && sPreview.mValid)
        {
            // The mouse is over the selection itself (or nothing) now: use the spot
            // the preview showed last, if it was for this selection.
            LLUUID first_id;
            S32 count = 0;
            selection_signature(first_id, count);
            if (first_id == sPreview.mFirstID && count == sPreview.mObjectCount)
            {
                p = sPreview;
                ok = true;
            }
        }
        if (!ok)
        {
            notify_failure(why, p.mMaxDistance);
            return;
        }
        if (p.mAlignSkipped)
        {
            LLNotificationsUtil::add("PrestigePlaceOnSurfaceNoAlign");
        }
        apply_placement(p);
    }

    void start_session(ESession session, KEY key)
    {
        sSession = session;
        sHoldKey = key;
        sAimDeselected = false;
        sPreview = Placement();
        if (!sIdleRegistered)
        {
            gIdleCallbacks.addFunction(on_idle, nullptr);
            sIdleRegistered = true;
        }
        on_idle(nullptr);   // show the preview at once
    }

    void end_session()
    {
        const ESession was = sSession;
        sSession = SESSION_NONE;
        sHoldKey = KEY_NONE;
        sPreview = Placement();
        if (sIdleRegistered)
        {
            // (Safe from inside the callback: the list only marks the entry.)
            gIdleCallbacks.deleteFunction(on_idle, nullptr);
            sIdleRegistered = false;
        }
        if (was == SESSION_AIM && aim_tool_is_current())
        {
            LLToolMgr::getInstance()->clearTransientTool();
        }
    }

    void on_idle(void*)
    {
        if (sSession == SESSION_NONE)
        {
            end_session();
            return;
        }
        if (sAimDeselected || !PrestigePlaceOnSurface::canPlace()
            || (gKeyboard && gKeyboard->getKeyDown(KEY_ESCAPE)))
        {
            end_session();
            return;
        }
        if (sSession == SESSION_AIM && !aim_tool_is_current())
        {
            end_session();   // another tool took over (e.g. a drag from inventory)
            return;
        }

        const S32 x = gViewerWindow->getCurrentMouseX();
        const S32 y = gViewerWindow->getCurrentMouseY();

        if (sSession == SESSION_HOLD)
        {
            if (!gFocusMgr.getAppHasFocus())
            {
                end_session();   // the window lost focus with P down: never place by accident
                return;
            }
            if (!gKeyboard || !gKeyboard->getKeyDown(sHoldKey))
            {
                // P released: put the selection where the preview is.
                Placement keep = sPreview;
                end_session();
                sPreview = keep;
                place_now(x, y, true);
                sPreview = Placement();
                return;
            }
        }

        Placement p;
        EFail why = FAIL_NONE;
        if (compute_placement(x, y, p, why, true))
        {
            sPreview = p;
        }
        else if (why == FAIL_TOO_FAR)
        {
            sPreview = Placement();   // nothing to show: it can't go there
        }
        // FAIL_NO_SURFACE (e.g. the mouse crosses the selection): keep the last preview.
    }

    void draw_box_faces(const corners_t& c)
    {
        // The six sides, two triangles each (corner index bits: x = 1, y = 2, z = 4).
        static const S32 FACES[6][4] =
        {
            { 0, 2, 3, 1 },   // -Z
            { 4, 5, 7, 6 },   // +Z
            { 0, 4, 6, 2 },   // -X
            { 1, 3, 7, 5 },   // +X
            { 0, 1, 5, 4 },   // -Y
            { 2, 6, 7, 3 }    // +Y
        };
        gGL.begin(LLRender::TRIANGLES);
        for (const auto& f : FACES)
        {
            gGL.vertex3fv(c[f[0]].mV);
            gGL.vertex3fv(c[f[1]].mV);
            gGL.vertex3fv(c[f[2]].mV);
            gGL.vertex3fv(c[f[0]].mV);
            gGL.vertex3fv(c[f[2]].mV);
            gGL.vertex3fv(c[f[3]].mV);
        }
        gGL.end();
    }

    void draw_box_edges(const corners_t& c)
    {
        static const S32 EDGES[12][2] =
        {
            { 0, 1 }, { 2, 3 }, { 4, 5 }, { 6, 7 },   // along X
            { 0, 2 }, { 1, 3 }, { 4, 6 }, { 5, 7 },   // along Y
            { 0, 4 }, { 1, 5 }, { 2, 6 }, { 3, 7 }    // along Z
        };
        gGL.begin(LLRender::LINES);
        for (const auto& e : EDGES)
        {
            gGL.vertex3fv(c[e[0]].mV);
            gGL.vertex3fv(c[e[1]].mV);
        }
        gGL.end();
    }
}

//----------------------------------------------------------------------------
// PrestigePlaceOnSurface
//----------------------------------------------------------------------------

// static
bool PrestigePlaceOnSurface::canPlace()
{
    if (!LLToolMgr::instanceExists() || !LLSelectMgr::instanceExists() || !LLToolMgr::getInstance()->inBuildMode())
    {
        return false;
    }
    LLSelectMgr* sm = LLSelectMgr::getInstance();
    LLObjectSelectionHandle sel = sm->getSelection();
    if (sel->isEmpty() || sel->getSelectType() != SELECT_TYPE_WORLD)
    {
        return false;
    }
    bool can_move = false;
    bool can_modify = false;
    sm->selectGetEditMoveLinksetPermissions(can_move, can_modify);
    return can_move;
}

// static
void PrestigePlaceOnSurface::onPlaceCommand(LLUICtrl* ctrl)
{
    if (!canPlace())
    {
        return;
    }

    // From the shortcut (its key is down now) or a click on the menu?
    KEY key = KEY_NONE;
    if (LLMenuItemGL* item = dynamic_cast<LLMenuItemGL*>(ctrl))
    {
        key = item->getAcceleratorKey();
    }
    const bool from_key = key != KEY_NONE && gKeyboard && gKeyboard->getKeyDown(key);

    if (sSession == SESSION_AIM)
    {
        // P while waiting for a click: place it where the mouse is. (The menu
        // clicked again: the mouse is on the menu, keep waiting for the click.)
        if (from_key)
        {
            placeAt(gViewerWindow->getCurrentMouseX(), gViewerWindow->getCurrentMouseY());
        }
        return;
    }
    if (sSession == SESSION_HOLD)
    {
        return;   // still held
    }

    if (from_key)
    {
        if (gSavedSettings.getBOOL("PrestigePlaceOnSurfacePreview"))
        {
            start_session(SESSION_HOLD, key);   // places when the key is released
        }
        else
        {
            place_now(gViewerWindow->getCurrentMouseX(), gViewerWindow->getCurrentMouseY(), false);
        }
        return;
    }

    // Clicked in the menu: the mouse is on the menu, so wait for a click in the world.
    LLToolMgr::getInstance()->setTransientTool(aim_tool());
    start_session(SESSION_AIM, KEY_NONE);
    LLNotificationsUtil::add("PrestigePlaceOnSurfaceAim");
}

// static
void PrestigePlaceOnSurface::placeAt(S32 x, S32 y)
{
    const bool had_session = sSession != SESSION_NONE;
    Placement keep = sPreview;
    end_session();
    if (!canPlace())
    {
        return;
    }
    sPreview = keep;
    place_now(x, y, had_session);
    sPreview = Placement();
}

// static
void PrestigePlaceOnSurface::cancel()
{
    end_session();
}

// static
void PrestigePlaceOnSurface::onAimToolDeselected()
{
    // Called from inside the tool manager's tool switch: end it on the next idle.
    if (sSession == SESSION_AIM)
    {
        sAimDeselected = true;
    }
}

// static
void PrestigePlaceOnSurface::renderPreview()
{
    if (sSession == SESSION_NONE || !sPreview.mValid || sPreview.mBoxes.empty())
    {
        return;
    }

    gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
    LLRender::BlendScope blend(true);
    gGL.matrixMode(LLRender::MM_MODELVIEW);
    gGL.pushMatrix();
    {
        // See-through sides and bright edges where nothing is in front of them...
        LLRender::DepthScope depth(true, false);
        gGL.color4f(PREVIEW_COLOR.mV[VRED], PREVIEW_COLOR.mV[VGREEN], PREVIEW_COLOR.mV[VBLUE], 0.18f);
        for (const corners_t& c : sPreview.mBoxes)
        {
            draw_box_faces(c);
        }
        LLUI::setLineWidth(2.f);
        gGL.color4f(PREVIEW_COLOR.mV[VRED], PREVIEW_COLOR.mV[VGREEN], PREVIEW_COLOR.mV[VBLUE], 0.9f);
        for (const corners_t& c : sPreview.mBoxes)
        {
            draw_box_edges(c);
        }
    }
    {
        // ...and faint edges through walls, so it can be found when hidden.
        LLRender::DepthScope depth(false);
        LLUI::setLineWidth(1.f);
        gGL.color4f(PREVIEW_COLOR.mV[VRED], PREVIEW_COLOR.mV[VGREEN], PREVIEW_COLOR.mV[VBLUE], 0.3f);
        for (const corners_t& c : sPreview.mBoxes)
        {
            draw_box_edges(c);
        }
    }
    gGL.popMatrix();
    gGL.flush();
    gGL.color4f(1.f, 1.f, 1.f, 1.f);
}

//----------------------------------------------------------------------------
// PrestigeObjectSnap
//----------------------------------------------------------------------------

namespace
{
    struct AABox
    {
        LLVector3 mMin;
        LLVector3 mMax;
    };

    bool sSnapActive = false;
    AABox sSnapSelection;
    std::vector<AABox> sSnapNeighbors;
    // The object each box in sSnapNeighbors belongs to (same order).
    std::vector<LLPointer<LLViewerObject>> sSnapNeighborObjects;

    // The exact stops against an object's real shape (its triangles), not its box.
    // Worked out once per drag, the first time the drag moves along a region axis.
    struct ExactStops
    {
        bool mExact = false;    // false: use the box
        F64 mAhead = 0.0;       // nearest stop ahead (1.0e30: none)
        F64 mBehind = 0.0;      // nearest stop behind (-1.0e30: none)
    };
    std::vector<ExactStops> sSnapExact;
    S32 sSnapExactAxis = -1;    // which region axis (0..2) sSnapExact is for, -1: none yet
    F64 sSnapExactSign = 0.0;   // and which way along it

    // How much real shape is looked at per drag before falling back to boxes.
    constexpr S32 SNAP_EXACT_MAX_OBJECTS = 96;
    constexpr S64 SNAP_EXACT_MAX_TRIANGLES = 2000000;

    AABox aabox_of(const LLBBox& box)
    {
        corners_t c;
        bbox_corners(box, c);
        AABox out;
        out.mMin.setVec(F32_MAX, F32_MAX, F32_MAX);
        out.mMax.setVec(-F32_MAX, -F32_MAX, -F32_MAX);
        for (const LLVector3& p : c)
        {
            grow(out.mMin, out.mMax, p);
        }
        return out;
    }

    bool overlaps(const AABox& a, const AABox& b)
    {
        for (S32 k = 0; k < 3; ++k)
        {
            if (a.mMax.mV[k] < b.mMin.mV[k] || a.mMin.mV[k] > b.mMax.mV[k])
            {
                return false;
            }
        }
        return true;
    }

    // Clips a polygon to lo < p[i] < hi (one axis). Sutherland-Hodgman, small fixed arrays.
    S32 clip_axis(const LLVector3* in, S32 n, LLVector3* out, S32 i, F32 lo, F32 hi)
    {
        LLVector3 tmp[16];
        S32 m = 0;
        // lower side
        for (S32 a = 0; a < n && m < 15; ++a)
        {
            const LLVector3& p = in[a];
            const LLVector3& q = in[(a + 1) % n];
            const bool pin = p.mV[i] >= lo;
            const bool qin = q.mV[i] >= lo;
            if (pin)
            {
                tmp[m++] = p;
            }
            if (pin != qin)
            {
                const F32 t = (lo - p.mV[i]) / (q.mV[i] - p.mV[i]);
                tmp[m++] = p + (q - p) * t;
            }
        }
        S32 k = 0;
        // upper side
        for (S32 a = 0; a < m && k < 15; ++a)
        {
            const LLVector3& p = tmp[a];
            const LLVector3& q = tmp[(a + 1) % m];
            const bool pin = p.mV[i] <= hi;
            const bool qin = q.mV[i] <= hi;
            if (pin)
            {
                out[k++] = p;
            }
            if (pin != qin)
            {
                const F32 t = (hi - p.mV[i]) / (q.mV[i] - p.mV[i]);
                out[k++] = p + (q - p) * t;
            }
        }
        return k;
    }

    // The stops against obj's real shape for the selection box sliding along region
    // axis k (sign +1 or -1): the parts of its triangles inside the selection's
    // cross-section, nearest in front and nearest behind. False: no shape to use
    // (not a prim/mesh, rigged or animated, or its shape could not be read).
    bool exact_stops(LLViewerObject* obj, const AABox& sel, S32 k, F64 sign, S64& budget, ExactStops& out)
    {
        LLVOVolume* vobj = dynamic_cast<LLVOVolume*>(obj);
        if (!vobj || vobj->isDead() || vobj->isRiggedMesh() || vobj->isAnimatedObject())
        {
            return false;
        }
        LLVolume* volume = vobj->getVolume();
        if (!volume || volume->getNumVolumeFaces() <= 0)
        {
            return false;
        }
        if (volume->hasReleasedFaceGeometry() && !gMeshRepo.rehydrateReleasedVolume(volume))
        {
            return false;
        }

        const S32 i = (k + 1) % 3;
        const S32 j = (k + 2) % 3;
        constexpr F32 EDGE = 0.001f;   // touching side by side is not in the way
        const F32 i_lo = sel.mMin.mV[i] + EDGE, i_hi = sel.mMax.mV[i] - EDGE;
        const F32 j_lo = sel.mMin.mV[j] + EDGE, j_hi = sel.mMax.mV[j] - EDGE;
        if (i_lo >= i_hi || j_lo >= j_hi)
        {
            return false;
        }
        // Along the drag: the selection spans [trail, lead] (projected on sign * axis).
        const F64 lead = sign > 0.0 ? sel.mMax.mV[k] : -sel.mMin.mV[k];
        const F64 trail = sign > 0.0 ? sel.mMin.mV[k] : -sel.mMax.mV[k];
        const F64 EPS = 0.001;

        out.mExact = true;
        out.mAhead = 1.0e30;
        out.mBehind = -1.0e30;

        std::vector<LLVector3> agent;
        for (S32 f = 0; f < volume->getNumVolumeFaces(); ++f)
        {
            const LLVolumeFace& vf = volume->getVolumeFace(f);
            if (!vf.mPositions || !vf.mIndices || vf.mNumVertices < 3 || vf.mNumIndices < 3)
            {
                if (vf.mNumIndices >= 3)
                {
                    return false;   // part of the shape is missing: don't trust the rest
                }
                continue;
            }
            budget -= vf.mNumIndices / 3;
            if (budget < 0)
            {
                return false;
            }
            agent.resize(vf.mNumVertices);
            for (S32 v = 0; v < vf.mNumVertices; ++v)
            {
                agent[v] = vobj->volumePositionToAgent(LLVector3(vf.mPositions[v].getF32ptr()));
            }
            for (S32 t = 0; t + 2 < vf.mNumIndices; t += 3)
            {
                const LLVector3& a = agent[vf.mIndices[t]];
                const LLVector3& b = agent[vf.mIndices[t + 1]];
                const LLVector3& c = agent[vf.mIndices[t + 2]];
                // Quick reject: the triangle misses the cross-section.
                if (llmax(a.mV[i], b.mV[i], c.mV[i]) <= i_lo || llmin(a.mV[i], b.mV[i], c.mV[i]) >= i_hi
                    || llmax(a.mV[j], b.mV[j], c.mV[j]) <= j_lo || llmin(a.mV[j], b.mV[j], c.mV[j]) >= j_hi)
                {
                    continue;
                }
                LLVector3 poly[3] = { a, b, c };
                LLVector3 p1[16], p2[16];
                S32 n = clip_axis(poly, 3, p1, i, i_lo, i_hi);
                if (n < 1)
                {
                    continue;
                }
                n = clip_axis(p1, n, p2, j, j_lo, j_hi);
                for (S32 v = 0; v < n; ++v)
                {
                    const F64 q = sign * p2[v].mV[k];
                    if (q >= lead - EPS)
                    {
                        out.mAhead = llmin(out.mAhead, q - lead);
                    }
                    else if (q <= trail + EPS)
                    {
                        out.mBehind = llmax(out.mBehind, q - trail);
                    }
                    // else: already inside the selection's span: let it move out
                }
            }
        }
        return true;
    }

    // Works out sSnapExact for region axis k / sign, for the boxes the slide can meet.
    void compute_exact_stops(S32 k, F64 sign)
    {
        sSnapExactAxis = k;
        sSnapExactSign = sign;
        sSnapExact.assign(sSnapNeighbors.size(), ExactStops());
        const S32 i = (k + 1) % 3;
        const S32 j = (k + 2) % 3;
        S32 objects = 0;
        S64 budget = SNAP_EXACT_MAX_TRIANGLES;
        for (size_t n = 0; n < sSnapNeighbors.size() && objects < SNAP_EXACT_MAX_OBJECTS && budget > 0; ++n)
        {
            const AABox& b = sSnapNeighbors[n];
            // Only boxes across the selection's path (overlapping its cross-section).
            if (b.mMax.mV[i] <= sSnapSelection.mMin.mV[i] || b.mMin.mV[i] >= sSnapSelection.mMax.mV[i]
                || b.mMax.mV[j] <= sSnapSelection.mMin.mV[j] || b.mMin.mV[j] >= sSnapSelection.mMax.mV[j])
            {
                continue;
            }
            LLViewerObject* obj = n < sSnapNeighborObjects.size() ? sSnapNeighborObjects[n].get() : nullptr;
            if (!obj)
            {
                continue;
            }
            ++objects;
            ExactStops e;
            if (exact_stops(obj, sSnapSelection, k, sign, budget, e))
            {
                sSnapExact[n] = e;
            }
        }
    }
}

// static
void PrestigeObjectSnap::beginDrag()
{
    endDrag();

    static LLCachedControl<bool> snap_to_objects(gSavedSettings, "PrestigeSnapToObjects", false);
    if (!snap_to_objects)
    {
        return;
    }
    LLObjectSelectionHandle sel = LLSelectMgr::getInstance()->getSelection();
    if (sel->isEmpty() || sel->getSelectType() != SELECT_TYPE_WORLD)
    {
        return;
    }

    // The selection's box where the drag starts.
    AABox selection;
    selection.mMin.setVec(F32_MAX, F32_MAX, F32_MAX);
    selection.mMax.setVec(-F32_MAX, -F32_MAX, -F32_MAX);
    bool have_selection = false;
    for (LLObjectSelection::iterator it = sel->begin(); it != sel->end(); ++it)
    {
        LLSelectNode* node = *it;
        LLViewerObject* obj = node ? node->getObject() : nullptr;
        if (!obj || obj->isDead() || obj->isAvatar())
        {
            continue;
        }
        if (node->mSelectedGLTFNode != -1)
        {
            return;   // glTF nodes move inside their object: no snapping
        }
        const AABox b = aabox_of(obj->getBoundingBoxAgent());
        grow(selection.mMin, selection.mMax, b.mMin);
        grow(selection.mMin, selection.mMax, b.mMax);
        have_selection = true;
    }
    if (!have_selection)
    {
        return;
    }

    // The objects around it (prims, not avatars or attachments; nothing selected).
    AABox reach;
    reach.mMin = selection.mMin - LLVector3(SNAP_REACH, SNAP_REACH, SNAP_REACH);
    reach.mMax = selection.mMax + LLVector3(SNAP_REACH, SNAP_REACH, SNAP_REACH);
    const S32 count = gObjectList.getNumObjects();
    for (S32 i = 0; i < count && sSnapNeighbors.size() < SNAP_MAX_NEIGHBORS; ++i)
    {
        LLViewerObject* obj = gObjectList.getObject(i);
        if (!obj || obj->isSelected() || obj->getPCode() != LL_PCODE_VOLUME || obj->mDrawable.isNull()
            || obj->isAttachment() || obj->isHUDAttachment() || obj->isAvatar())
        {
            continue;
        }
        // Quick test on the prim's centre and size before working out its box.
        const LLVector3& pos = obj->getPositionAgent();
        const F32 radius = obj->getScale().length() * 0.5f;
        bool near_enough = true;
        for (S32 k = 0; k < 3 && near_enough; ++k)
        {
            near_enough = pos.mV[k] + radius >= reach.mMin.mV[k] && pos.mV[k] - radius <= reach.mMax.mV[k];
        }
        if (!near_enough)
        {
            continue;
        }
        const AABox b = aabox_of(obj->getBoundingBoxAgent());
        if (overlaps(b, reach))
        {
            sSnapNeighbors.push_back(b);
            sSnapNeighborObjects.push_back(obj);
        }
    }

    sSnapSelection = selection;
    sSnapActive = !sSnapNeighbors.empty();
}

// static
void PrestigeObjectSnap::endDrag()
{
    sSnapActive = false;
    sSnapNeighbors.clear();
    sSnapNeighborObjects.clear();
    sSnapExact.clear();
    sSnapExactAxis = -1;
    sSnapExactSign = 0.0;
}

// static
F64 PrestigeObjectSnap::adjust(const LLVector3& axis_in, F64 distance)
{
    if (!sSnapActive)
    {
        return distance;
    }
    LLVector3 axis = axis_in;
    if (axis.normalize() < 0.001f)
    {
        return distance;
    }

    static LLCachedControl<F32> snap_distance(gSavedSettings, "PrestigeSnapToObjectsDistance", 0.25f);
    const F64 threshold = llmax(0.0, (F64)snap_distance());
    const F64 EPS = 0.001;      // 1 mm: boxes this close count as touching
    const F64 NONE = 1.0e30;

    // For every box: the stretch of the drag (from its start) over which the
    // selection's box would be inside it. The nearest one ahead stops the drag at
    // its near side; the nearest one behind stops it going back.
    // Along a region axis (the World ruler): stop at the objects' real shape, not
    // their boxes (a floor mesh's box can stand well above the floor you see).
    S32 region_axis = -1;
    for (S32 k = 0; k < 3; ++k)
    {
        if (std::fabs(axis.mV[k]) > 0.9999f)
        {
            region_axis = k;
        }
    }
    const F64 axis_sign = region_axis >= 0 ? (axis.mV[region_axis] > 0.f ? 1.0 : -1.0) : 0.0;
    if (region_axis >= 0 && (region_axis != sSnapExactAxis || axis_sign != sSnapExactSign))
    {
        compute_exact_stops(region_axis, axis_sign);
    }

    F64 ahead = NONE;
    F64 behind = -NONE;
    for (size_t n = 0; n < sSnapNeighbors.size(); ++n)
    {
        if (region_axis >= 0 && n < sSnapExact.size() && sSnapExact[n].mExact)
        {
            ahead = llmin(ahead, sSnapExact[n].mAhead);
            behind = llmax(behind, sSnapExact[n].mBehind);
            continue;
        }
        const AABox& b = sSnapNeighbors[n];
        F64 enter = -NONE;
        F64 leave = NONE;
        bool meets = true;
        for (S32 k = 0; k < 3 && meets; ++k)
        {
            const F64 d = axis.mV[k];
            const F64 s_min = sSnapSelection.mMin.mV[k];
            const F64 s_max = sSnapSelection.mMax.mV[k];
            const F64 b_min = b.mMin.mV[k];
            const F64 b_max = b.mMax.mV[k];
            if (std::fabs(d) < 1.0e-6)
            {
                // Not moving this way: it only meets the box if they already overlap here.
                meets = s_max > b_min + EPS && s_min < b_max - EPS;
            }
            else
            {
                F64 t1 = (b_min - s_max) / d;
                F64 t2 = (b_max - s_min) / d;
                if (t1 > t2)
                {
                    std::swap(t1, t2);
                }
                enter = llmax(enter, t1);
                leave = llmin(leave, t2);
            }
        }
        if (!meets || enter >= leave)
        {
            continue;
        }
        if (enter < -EPS && leave > EPS)
        {
            continue;   // already inside it when the drag started: let it move out
        }
        if (enter >= -EPS)
        {
            ahead = llmin(ahead, enter);
        }
        else
        {
            behind = llmax(behind, leave);
        }
    }

    F64 t = distance;
    // Never into an object...
    if (t > ahead)
    {
        t = ahead;
    }
    if (t < behind)
    {
        t = behind;
    }
    // ...and flush against it when close.
    const F64 to_ahead = ahead < NONE ? ahead - t : NONE;
    const F64 to_behind = behind > -NONE ? t - behind : NONE;
    if (to_ahead <= to_behind && to_ahead <= threshold)
    {
        t = ahead;
    }
    else if (to_behind < to_ahead && to_behind <= threshold)
    {
        t = behind;
    }
    return t;
}
