/**
 * @file llcleanrigcor.h
 * @brief #1581 centre-of-rotation skinning (RenderVKSkinningCoR): the per-avatar centre store.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Slipstream Viewer Source Code
 * Copyright (C) 2026, Slipstream contributors
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;
 * version 2.1 of the License only.
 * $/LicenseInfo$
 *
 * Optimised centre-of-rotation skinning (Le & Hodgins 2016; maths: llcleancormath.h) needs one
 * centre per skinned vertex: a weight-similarity-weighted integral over a surface.
 *
 * ONE DOMAIN PER AVATAR. The paper assumes one mesh. An SL avatar is many rigged meshes -- body,
 * head, hands, clothes, buttons, feathers, hair -- and a centre integrated over each mesh on its
 * own depends on which mesh a vertex belongs to: a shirt button (a tiny separate mesh whose
 * vertices all carry nearly the shirt's weights there) got its own centroid as centre, the shirt
 * under it a point on the torso axis -- same weights, different rotation centres, and the
 * button left the shirt as the torso twisted; likewise the head and body meshes at the neck seam
 * and separate feather cards over a wing. So the centres of an avatar are integrated over the
 * UNION of its rigged meshes, in bind (skeleton) space -- bind_shape x the model position --
 * with weights keyed by AVATAR joint (each mesh's joint map, LLCleanSkinStore::SkinMeta
 * joint_nums): equal weights give an equal centre whatever mesh carries them.
 *   domain = per (skin, face): the finest resident creator LOD of the face (one copy of each
 *            surface; a LOD switch does not change the domain)
 *   targets = every slot the avatar draws (the LOD in use and its #561 decimated steps)
 * A changed domain (a mesh worn / taken off / a finer LOD arrived) recomputes the avatar after
 * it has been stable for kSettleFrames; the old centres stay in use meanwhile.
 *
 * GPU: centres (3 fp32 per target vertex, bind space) in the renderer's arena; an
 * open-addressed table the CoR pre-skin variant (clean_preskin.comp PRESKIN_COR) reads per work
 * item: key = (the slot's weight-arena base, the item's skin id), entry = 2 x uvec4
 *   {weight base (kEmpty / kTomb), skin id, source vertex base, vertex count}, {first word, 0, 0, 0}
 * The pre-skin uses an entry only when all four match its item, so a moved slot, a morphed system
 * body, a recycled skin id or a slot not computed yet is skinned by LBS -- never with another
 * mesh's or another avatar's centres.
 *
 * Lifetime (free on liveness): entries not wanted for kAgeFrames (~30 s) are freed; a full arena
 * evicts the least recently used entry not used this frame; a slot the store rewrote or freed
 * (noteDirty) or a skin whose meta changed (noteSkinDirty) loses its entries. Nothing here runs,
 * and the renderer holds no buffer for it, while the option is off.
 *
 * Threading: everything on the render thread except the integration, which runs on one worker
 * from raw copies of the mirrors (the mirror accessors are render-thread only).
 */

#pragma once

#include "stdtypes.h"
#include "llcleancormath.h"
#include "llcleanqvertex.h"

#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

class LLCleanRigCoR
{
public:
    static LLCleanRigCoR& instance();

    static constexpr U32 kTableCap     = 32768;   // = clean_preskin.comp COR_TAB_CAP (power of two)
    static constexpr U32 kEntryWords   = 8;       // = 2 uvec4 (clean_preskin.comp corLookup)
    static constexpr U32 kProbes       = 16;      // = clean_preskin.comp COR_PROBES
    static constexpr U32 kEmpty        = 0xFFFFFFFFu;
    static constexpr U32 kTomb         = 0xFFFFFFFEu;
    static constexpr U32 kWordsPerVert = 3;       // fp32 xyz, bind space
    // The centre arena the renderer allocates while the option is on (VRAM, LRU inside):
    // 64 MiB = 5.6 M vertices.
    static constexpr U64 kArenaBytes   = 64ull << 20;
    // LLCleanSkinStore::SkinMeta, in 32-bit words (= clean_skin_palette.comp SkinMeta).
    static constexpr U32 kMetaAvatar     = 0;
    static constexpr U32 kMetaJointCount = 1;
    static constexpr U32 kMetaJointNums  = 4;
    static constexpr U32 kMetaBindShape  = 1876;
    static constexpr U32 kMetaMaxJoints  = 110;
    static constexpr U32 kAvatarJoints   = 216;

    // The renderer's CPU slot rows (= its GPU SlotInfo, 32 B).
    struct SlotRow { U32 first_index, index_count; S32 base_vertex; U32 has_weights, vert_count, p0, p1, p2; };
    static_assert(sizeof(SlotRow) == 32, "SlotRow must match the renderer's SlotInfo (32 B)");
    // The renderer's CPU skin meta mirror (LLCleanSkinStore::SkinMeta rows).
    struct Meta { const U32* words = nullptr; U32 rows = 0; U32 row_words = 0; };

    // ---- render thread ----
    // An instance draws `slot` skinned with `skin`; `domain` = its face's finest resident slot
    // (kEmpty: target only). The skin's avatar (meta) owns both.
    void want(U32 skin, U32 slot, U32 domain, const SlotRow* rows, size_t nrows, const Meta& meta, U32 frame);
    // Slots whose mirror was rewritten / freed; skins whose meta changed (re-resolved, recycled id).
    void noteDirty(const std::vector<U32>& slots);
    void noteSkinDirty(U32 lo, U32 hi);
    // Once per frame: re-key moved slots, drop stale / aged entries and domain members, commit
    // finished avatars (allocate, queue uploads, publish table entries), launch settled avatars,
    // log. capacity_words = the renderer's centre arena (0: off -- everything is dropped).
    void pump(U32 frame, const SlotRow* rows, size_t nrows, const Meta& meta, U32 capacity_words);
    const U32* tableWords() const { return mTable.data(); }
    bool takeTableDirty(std::vector<U32>& entries, bool& full);
    // Centre uploads queued by pump (word offset into the arena + data), recorded BEFORE the
    // table entries that name them.
    struct Upload { U32 first; std::vector<F32> data; };
    std::vector<Upload>& uploads() { return mUploads; }
    void reset();
    void stop();
    U32  liveEntries() const { return (U32)mLive.size(); }
    U64  usedBytes() const { return mUsedWords * 4u; }
    std::string statsLine() const;

private:
    LLCleanRigCoR();
    ~LLCleanRigCoR();

    struct Sig
    {
        U32 vc = 0, ic = 0;
        bool operator==(const Sig& o) const { return vc == o.vc && ic == o.ic; }
    };
    static U64 itemKey(U32 skin, U32 slot) { return ((U64)skin << 32) | slot; }

    // One slot of a job, copied raw from the mirror (decoded on the worker).
    struct SlotCopy
    {
        U32 skin = 0, slot = 0;
        Sig sig;
        bool domain = false, target = false;
        std::vector<LLCleanQV::QVertex> verts;   // header-prefixed
        std::vector<LLCleanQV::QWeight> wts;
        std::vector<U16> idx;                    // domain slots only
        F32 bind_shape[16];
        U32 joint_count = 0;
        S32 joints[kMetaMaxJoints];
        std::vector<F32> out;                    // targets: centres (vc * 3)
    };
    struct Job
    {
        U32 avatar = 0, id = 0, launched = 0;
        U64 sig = 0;
        std::vector<SlotCopy> slots;
        LLCleanCoR::Stats st;
        U32 domain_meshes = 0, domain_verts = 0, target_verts = 0;
        F32 ms = 0.f;
        bool ok = false;
    };
    struct Item { Sig sig; U32 seen = 0; };
    struct Avatar
    {
        std::map<U64, Item> domain;    // (skin, slot) -> member
        std::map<U64, Item> targets;   // (skin, slot) -> wanted
        bool sig_dirty = true;
        U64  sig = 0;                  // hash of the members + targets
        U64  built = 0;                // the sig the published centres were computed from (0: none)
        U32  changed = 0;              // frame the sig last changed
        U32  job = 0;                  // running job (0: none)
        U32  retry = 0;                // failed / no room: not relaunched before this frame
        bool missing = false;          // a target has no entry (new, aged, evicted, rewritten)
        U32  tchanged = 0;             // frame a target was last added
    };
    struct Entry
    {
        U32 skin = 0, slot = 0, avatar = 0;
        Sig sig;
        U32 key = kEmpty, vbase = 0;   // as published
        U32 first = 0, words = 0;
        U32 table = kEmpty;
        U32 last_use = 0;
    };

    static void compute(Job& job);
    void workerMain();
    bool alloc(U32 words, U32& first);
    void freeRange(U32 first, U32 words);
    void freeEntry(Entry& e);
    void dropEntry(U64 key);
    U32  tableInsert(const Entry& e);
    void tableRemove(U32 t);
    void tableRebuild();
    void writeEntryWords(U32 t, const Entry* e);
    bool evictLru(U32 frame);
    std::unique_ptr<Job> makeJob(U32 avatar, Avatar& a, const SlotRow* rows, size_t nrows, const Meta& meta, U64& copied);

    std::unordered_map<U32, Avatar> mAvatars;   // the skin meta's avatar slot -> state
    std::unordered_map<U64, Entry>  mLive;      // (skin, slot) -> published centres
    std::vector<U32>                mTable;
    std::vector<U32>                mDirty;
    bool                            mTableFull = true;
    U32                             mTombs = 0;
    std::vector<Upload>             mUploads;
    std::map<U32, U32>              mFree;      // first word -> words
    U32                             mCapWords = 0;
    U64                             mUsedWords = 0, mPeakWords = 0;
    U32                             mJobSeq = 0;
    // stats (the summary line, ~once a minute while anything happens)
    U64 mStatBuilt = 0, mStatFailed = 0, mStatEvicted = 0, mStatNoRoom = 0, mStatVerts = 0;
    F64 mStatMsSum = 0.0;
    F32 mStatMsMax = 0.f;
    U32 mStatMaxVerts = 0;
    F64 mLogTime = 0.0;
    U64 mLogBuilt = 0, mLogFailed = 0, mLogEvicted = 0, mLogNoRoom = 0;

    std::mutex                        mMutex;
    std::condition_variable           mCv;
    std::vector<std::unique_ptr<Job>> mJobsIn, mJobsOut;
    U32                               mInFlight = 0;
    std::thread                       mThread;
    bool                              mStopped = false;
};
