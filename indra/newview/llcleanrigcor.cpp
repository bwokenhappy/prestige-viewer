/**
 * @file llcleanrigcor.cpp
 * @brief #1581 centre-of-rotation skinning: the per-avatar centre store. See the header.
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
 */

#include "llviewerprecompiledheaders.h"

#include "llcleanrigcor.h"

#include "llcleanmeshasset.h"
#include "llframetimer.h"
#include "llthread.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <sstream>

namespace
{
constexpr U32 kMaxLive      = LLCleanRigCoR::kTableCap / 2;   // load factor <= 0.5
constexpr U32 kAgeFrames    = 1800;     // an entry not wanted this long (~30 s): freed
constexpr U32 kItemAge      = 240;      // a domain member / target not seen this long (~4 s): left the avatar
constexpr U32 kSettleFrames = 30;       // the avatar's domain unchanged this long before a (re)compute
constexpr U32 kFirstSettle  = 10;       //   (first build of an avatar)
constexpr U32 kRetryFrames  = 3600;     // failed / no room: not relaunched for ~1 min (same content)
constexpr U64 kLaunchVerts  = 524288;   // mirror vertices copied per frame (raw; at least one avatar)
constexpr U32 kMaxInFlight  = 2;        // jobs queued or running (bounds the copies held)
constexpr U64 kUploadWords  = 1u << 19; // centre words committed per frame (at least one avatar)
constexpr U64 kMaxWork      = 1500000000ull;   // similarity terms per avatar (llcleancormath work cap)
constexpr F64 kLogSeconds   = 60.0;     // summary cadence (user logs: ~once a minute)
constexpr F32 kSlowMs       = 250.f;    // an avatar this slow is logged on its own (others: debug)

U32 hashKey(U32 wbase, U32 skin)
{
    static_assert((LLCleanRigCoR::kTableCap & (LLCleanRigCoR::kTableCap - 1u)) == 0u, "table capacity must be a power of two");
    U32 bits = 0;
    while ((1u << bits) < LLCleanRigCoR::kTableCap) ++bits;
    return ((wbase ^ (skin * 0x9E3779B9u)) * 2654435761u) >> (32u - bits);   // = clean_preskin.comp corLookup
}

// The row's weight-arena base (the table key) or kEmpty (no weights / not resident).
U32 rowKey(const LLCleanRigCoR::SlotRow& r)
{
    return (r.has_weights != 0u && r.index_count != 0u && r.base_vertex >= 0) ? r.has_weights - 1u : LLCleanRigCoR::kEmpty;
}

U64 mixHash(U64 h, U64 x)
{
    h ^= x + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
    return h;
}
} // namespace

LLCleanRigCoR& LLCleanRigCoR::instance()
{
    static LLCleanRigCoR s;
    return s;
}

LLCleanRigCoR::LLCleanRigCoR()
{
    mTable.assign((size_t)kTableCap * kEntryWords, 0u);
    for (U32 e = 0; e < kTableCap; ++e) mTable[(size_t)e * kEntryWords] = kEmpty;
}

LLCleanRigCoR::~LLCleanRigCoR()
{
    stop();
}

void LLCleanRigCoR::stop()
{
    {
        std::lock_guard<std::mutex> lk(mMutex);
        mStopped = true;
    }
    mCv.notify_all();
    if (mThread.joinable()) mThread.join();
}

void LLCleanRigCoR::reset()
{
    {
        std::lock_guard<std::mutex> lk(mMutex);
        mJobsIn.clear();
        mJobsOut.clear();   // (a running job's result is discarded: no avatar waits for it)
    }
    mInFlight = 0;
    mAvatars.clear();
    mLive.clear();
    mUploads.clear();
    for (U32 e = 0; e < kTableCap; ++e)
    {
        std::fill_n(mTable.begin() + (size_t)e * kEntryWords, kEntryWords, 0u);
        mTable[(size_t)e * kEntryWords] = kEmpty;
    }
    mTombs = 0;
    mTableFull = true;
    mDirty.clear();
    mFree.clear();
    if (mCapWords) mFree[0] = mCapWords;
    mUsedWords = 0;
}

// ---------------------------------------------------------------------------
// Worker: the avatar's union domain in bind space, avatar-joint weights, every target.
// ---------------------------------------------------------------------------
void LLCleanRigCoR::compute(Job& job)
{
    const auto t0 = std::chrono::steady_clock::now();
    auto mapInfluence = [](const SlotCopy& s, const LLCleanQV::QWeight& w) {
        LLCleanCoR::Influence in = LLCleanCoR::decodeRecord(reinterpret_cast<const uint8_t*>(&w));
        for (U32& j : in.j)
        {
            // = clean_skin_palette.comp: lanes past the joint count replicate the last joint
            const U32 src = s.joint_count ? std::min(j, s.joint_count - 1u) : 0u;
            j = (U32)llclamp(s.joints[src], 0, (S32)kAvatarJoints - 1);
        }
        return in;
    };
    auto toBind = [](const SlotCopy& s, const LLCleanQV::QHeader& h, const LLCleanQV::QVertex& v, F32* out) {
        F32 p[3];
        LLCleanQV::decodePos(h, v, p);
        const F32* m = s.bind_shape;   // column-major
        for (int r = 0; r < 3; ++r) out[r] = m[r] * p[0] + m[4 + r] * p[1] + m[8 + r] * p[2] + m[12 + r];
    };
    std::vector<F32> gp, ep;
    std::vector<LLCleanCoR::Influence> gi, ei;
    std::vector<U32> gx;
    for (SlotCopy& s : job.slots)
    {
        if (s.verts.size() < LLCleanQV::kHeaderVerts + s.sig.vc || s.wts.size() < s.sig.vc) continue;
        LLCleanQV::QHeader h;
        LLCleanQV::readHeader(s.verts.data(), h);
        const LLCleanQV::QVertex* sv = s.verts.data() + LLCleanQV::kHeaderVerts;
        if (s.domain)
        {
            const U32 base = (U32)gi.size();
            for (U32 v = 0; v < s.sig.vc; ++v)
            {
                F32 p[3];
                toBind(s, h, sv[v], p);
                gp.insert(gp.end(), p, p + 3);
                gi.push_back(mapInfluence(s, s.wts[v]));
            }
            for (U16 i : s.idx) gx.push_back(base + i);
            ++job.domain_meshes;
            job.domain_verts += s.sig.vc;
        }
        if (s.target)
        {
            for (U32 v = 0; v < s.sig.vc; ++v)
            {
                F32 p[3];
                toBind(s, h, sv[v], p);
                ep.insert(ep.end(), p, p + 3);
                ei.push_back(mapInfluence(s, s.wts[v]));
            }
            job.target_verts += s.sig.vc;
        }
        std::vector<LLCleanQV::QVertex>().swap(s.verts);
        std::vector<LLCleanQV::QWeight>().swap(s.wts);
        std::vector<U16>().swap(s.idx);
    }
    std::vector<F32> out(ep.size());
    job.ok = !ei.empty() && !gx.empty()
          && LLCleanCoR::computeCoR(gp.data(), gi.data(), (U32)gi.size(), gx.data(), (U32)gx.size(), ei.data(), ep.data(),
                                    (U32)ei.size(), out.data(), job.st, kMaxWork);
    if (job.ok)
    {
        size_t at = 0;
        for (SlotCopy& s : job.slots)
        {
            if (!s.target) continue;
            const size_t n = (size_t)s.sig.vc * 3u;
            if (at + n > out.size()) { job.ok = false; break; }
            s.out.assign(out.begin() + at, out.begin() + at + n);
            at += n;
        }
    }
    job.ms = std::chrono::duration<F32, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

void LLCleanRigCoR::workerMain()
{
    ll_set_current_thread_name("CleanRigCoR");
    for (;;)
    {
        std::unique_ptr<Job> job;
        {
            std::unique_lock<std::mutex> lk(mMutex);
            mCv.wait(lk, [&] { return mStopped || !mJobsIn.empty(); });
            if (mStopped) return;
            job = std::move(mJobsIn.front());
            mJobsIn.erase(mJobsIn.begin());
        }
        compute(*job);
        std::lock_guard<std::mutex> lk(mMutex);
        mJobsOut.push_back(std::move(job));
    }
}

// ---------------------------------------------------------------------------
// Render thread
// ---------------------------------------------------------------------------
void LLCleanRigCoR::want(U32 skin, U32 slot, U32 domain, const SlotRow* rows, size_t nrows, const Meta& meta, U32 frame)
{
    if (mStopped || mCapWords == 0 || slot >= nrows || skin >= meta.rows || !meta.words) return;
    const U32* m = meta.words + (size_t)skin * meta.row_words;
    if (m[kMetaJointCount] == 0u) return;   // unresolved skin: drawn in its bind pose, no palette
    const U32 avatar = m[kMetaAvatar] & 0x7FFFFFFFu;
    const SlotRow& r = rows[slot];
    if (rowKey(r) == kEmpty || r.vert_count == 0 || r.vert_count > LLCleanQV::kMaxSlotVerts) return;
    Avatar& a = mAvatars[avatar];
    const Sig sig{r.vert_count, r.index_count};
    {
        // (a new target -- another LOD or step drawn -- is computed against the current domain:
        // it does not change the domain, so the other targets' centres stand)
        Item& t = a.targets[itemKey(skin, slot)];
        if (!(t.sig == sig)) { t.sig = sig; a.missing = true; a.tchanged = frame; }
        t.seen = frame;
    }
    if (domain < nrows && rowKey(rows[domain]) != kEmpty && rows[domain].vert_count <= LLCleanQV::kMaxSlotVerts)
    {
        const Sig ds{rows[domain].vert_count, rows[domain].index_count};
        Item& d = a.domain[itemKey(skin, domain)];
        if (!(d.sig == ds)) { d.sig = ds; a.sig_dirty = true; }
        d.seen = frame;
    }
    auto lit = mLive.find(itemKey(skin, slot));
    if (lit != mLive.end() && lit->second.sig == sig) lit->second.last_use = frame;
}

void LLCleanRigCoR::noteDirty(const std::vector<U32>& slots)
{
    if (slots.empty() || (mLive.empty() && mAvatars.empty())) return;
    std::vector<U32> dirty(slots);
    std::sort(dirty.begin(), dirty.end());
    auto isDirty = [&](U32 s) { return std::binary_search(dirty.begin(), dirty.end(), s); };
    for (auto it = mLive.begin(); it != mLive.end();)
    {
        if (isDirty(it->second.slot))
        {
            auto ai = mAvatars.find(it->second.avatar);
            if (ai != mAvatars.end()) ai->second.missing = true;
            freeEntry(it->second);
            it = mLive.erase(it);
        }
        else ++it;
    }
    for (auto& kv : mAvatars)
        for (auto* items : {&kv.second.domain, &kv.second.targets})
            for (auto it = items->begin(); it != items->end();)
            {
                if (isDirty((U32)it->first)) { it = items->erase(it); kv.second.sig_dirty = true; }
                else ++it;
            }
}

void LLCleanRigCoR::noteSkinDirty(U32 lo, U32 hi)
{
    if (lo >= hi) return;
    for (auto it = mLive.begin(); it != mLive.end();)
    {
        if (it->second.skin >= lo && it->second.skin < hi)
        {
            auto ai = mAvatars.find(it->second.avatar);
            if (ai != mAvatars.end()) ai->second.missing = true;
            freeEntry(it->second);
            it = mLive.erase(it);
        }
        else ++it;
    }
    for (auto& kv : mAvatars)
        for (auto* items : {&kv.second.domain, &kv.second.targets})
            for (auto it = items->begin(); it != items->end();)
            {
                const U32 skin = (U32)(it->first >> 32);
                if (skin >= lo && skin < hi) { it = items->erase(it); kv.second.sig_dirty = true; }
                else ++it;
            }
}

bool LLCleanRigCoR::alloc(U32 words, U32& first)
{
    for (auto it = mFree.begin(); it != mFree.end(); ++it)
    {
        if (it->second < words) continue;
        first = it->first;
        const U32 rest = it->second - words;
        mFree.erase(it);
        if (rest) mFree[first + words] = rest;
        mUsedWords += words;
        mPeakWords = std::max(mPeakWords, mUsedWords);
        return true;
    }
    return false;
}

void LLCleanRigCoR::freeRange(U32 first, U32 words)
{
    if (!words) return;
    mUsedWords -= std::min<U64>(mUsedWords, words);
    auto next = mFree.lower_bound(first);
    if (next != mFree.end() && first + words == next->first)
    {
        words += next->second;
        next = mFree.erase(next);
    }
    if (next != mFree.begin())
    {
        auto prev = std::prev(next);
        if (prev->first + prev->second == first) { prev->second += words; return; }
    }
    mFree[first] = words;
}

void LLCleanRigCoR::freeEntry(Entry& e)
{
    if (e.table != kEmpty) tableRemove(e.table);
    e.table = kEmpty;
    freeRange(e.first, e.words);
    e.words = 0;
}

void LLCleanRigCoR::dropEntry(U64 key)
{
    auto it = mLive.find(key);
    if (it == mLive.end()) return;
    freeEntry(it->second);
    mLive.erase(it);
}

bool LLCleanRigCoR::evictLru(U32 frame)
{
    auto lru = mLive.end();
    for (auto it = mLive.begin(); it != mLive.end(); ++it)
        if (it->second.last_use != frame && (lru == mLive.end() || it->second.last_use < lru->second.last_use)) lru = it;
    if (lru == mLive.end()) return false;
    auto ai = mAvatars.find(lru->second.avatar);
    if (ai != mAvatars.end()) ai->second.missing = true;
    freeEntry(lru->second);
    mLive.erase(lru);
    ++mStatEvicted;
    return true;
}

void LLCleanRigCoR::writeEntryWords(U32 t, const Entry* e)
{
    U32* w = &mTable[(size_t)t * kEntryWords];
    std::fill_n(w, kEntryWords, 0u);
    if (!e) w[0] = kTomb;
    else
    {
        w[0] = e->key; w[1] = e->skin; w[2] = e->vbase; w[3] = e->sig.vc;
        w[4] = e->first;
    }
    if (!mTableFull) mDirty.push_back(t);
}

U32 LLCleanRigCoR::tableInsert(const Entry& e)
{
    if (e.key == kEmpty || e.key == kTomb) return kEmpty;
    const U32 h = hashKey(e.key, e.skin);
    for (U32 k = 0; k < kProbes; ++k)
    {
        const U32 t = (h + k) & (kTableCap - 1u);
        const U32 key = mTable[(size_t)t * kEntryWords];
        if (key == kEmpty || key == kTomb)
        {
            if (key == kTomb && mTombs) --mTombs;
            writeEntryWords(t, &e);
            return t;
        }
    }
    return kEmpty;   // probe chain full: this slot stays LBS (the pre-skin finds nothing)
}

void LLCleanRigCoR::tableRemove(U32 t)
{
    if (t >= kTableCap) return;
    writeEntryWords(t, nullptr);
    ++mTombs;
}

void LLCleanRigCoR::tableRebuild()
{
    for (U32 e = 0; e < kTableCap; ++e)
    {
        std::fill_n(mTable.begin() + (size_t)e * kEntryWords, kEntryWords, 0u);
        mTable[(size_t)e * kEntryWords] = kEmpty;
    }
    mTombs = 0;
    mTableFull = true;
    mDirty.clear();
    for (auto& kv : mLive) kv.second.table = tableInsert(kv.second);
}

bool LLCleanRigCoR::takeTableDirty(std::vector<U32>& entries, bool& full)
{
    full = mTableFull;
    entries.clear();
    if (mTableFull)
    {
        mTableFull = false;
        mDirty.clear();
        return true;
    }
    if (mDirty.empty()) return false;
    std::sort(mDirty.begin(), mDirty.end());
    mDirty.erase(std::unique(mDirty.begin(), mDirty.end()), mDirty.end());
    entries.swap(mDirty);
    mDirty.clear();
    return true;
}

std::unique_ptr<LLCleanRigCoR::Job> LLCleanRigCoR::makeJob(U32 avatar, Avatar& a, const SlotRow* rows, size_t nrows,
                                                           const Meta& meta, U64& copied)
{
    LLCleanMeshAsset& ms = LLCleanMeshAsset::instance();
    auto job = std::make_unique<Job>();
    job->avatar = avatar;
    job->id = ++mJobSeq;
    job->sig = a.sig;
    std::map<U64, size_t> at;   // (skin, slot) -> index in job->slots
    auto add = [&](U64 key, const Sig& sig, bool domain) -> bool
    {
        const U32 skin = (U32)(key >> 32), slot = (U32)key;
        if (slot >= nrows || skin >= meta.rows || !(Sig{rows[slot].vert_count, rows[slot].index_count} == sig)) return false;
        auto f = at.find(key);
        if (f != at.end())
        {
            (domain ? job->slots[f->second].domain : job->slots[f->second].target) = true;
            return true;
        }
        U32 nv = 0, nw = 0, ni = 0;
        const LLCleanMeshAsset::QVertex* sq = ms.cpuSlotPacked(slot, nv);   // header-prefixed (#573)
        const LLCleanMeshAsset::MirrorWeight* sw = ms.cpuSlotWeights(slot, nw);
        const U16* si = ms.cpuSlotIndices(slot, ni);
        if (!sq || !sw || !si || nv != sig.vc || nw != nv || ni != sig.ic) return false;
        SlotCopy s;
        s.skin = skin; s.slot = slot; s.sig = sig;
        (domain ? s.domain : s.target) = true;
        s.verts.assign(sq, sq + LLCleanMeshAsset::QV_HEADER + nv);
        s.wts.assign(sw, sw + nw);
        if (domain) s.idx.assign(si, si + ni);   // (domain slots are added first: a target-only slot needs none)
        const U32* m = meta.words + (size_t)skin * meta.row_words;
        std::memcpy(s.bind_shape, m + kMetaBindShape, sizeof(s.bind_shape));
        s.joint_count = std::min(m[kMetaJointCount], kMetaMaxJoints);
        std::memcpy(s.joints, m + kMetaJointNums, sizeof(s.joints));
        copied += nv;
        at.emplace(key, job->slots.size());
        job->slots.push_back(std::move(s));
        return true;
    };
    for (const auto& kv : a.domain) add(kv.first, kv.second.sig, true);
    for (const auto& kv : a.targets) add(kv.first, kv.second.sig, false);
    bool any_target = false, any_domain = false;
    for (const SlotCopy& s : job->slots) { any_target |= s.target; any_domain |= s.domain; }
    if (!any_target || !any_domain) return nullptr;
    return job;
}

void LLCleanRigCoR::pump(U32 frame, const SlotRow* rows, size_t nrows, const Meta& meta, U32 capacity_words)
{
    if (mStopped) return;
    if (capacity_words != mCapWords)
    {
        mCapWords = capacity_words;
        reset();   // (the arena was (re)created or freed: every range is gone)
    }
    if (mCapWords == 0) return;
    auto sigOk = [&](U32 slot, const Sig& s) {
        return slot < nrows && rows[slot].vert_count == s.vc && rows[slot].index_count == s.ic && rowKey(rows[slot]) != kEmpty;
    };

    // 1. Avatars: members / targets gone, rewritten or not seen for a while leave; the content
    // hash follows. An avatar with nothing left (and no job) goes.
    for (auto ait = mAvatars.begin(); ait != mAvatars.end();)
    {
        Avatar& a = ait->second;
        for (auto it = a.domain.begin(); it != a.domain.end();)
        {
            if (frame - it->second.seen > kItemAge || !sigOk((U32)it->first, it->second.sig))
            {
                it = a.domain.erase(it);
                a.sig_dirty = true;
            }
            else ++it;
        }
        for (auto it = a.targets.begin(); it != a.targets.end();)   // (their entries age on their own)
        {
            if (frame - it->second.seen > kAgeFrames || !sigOk((U32)it->first, it->second.sig)) it = a.targets.erase(it);
            else ++it;
        }
        if (a.sig_dirty)
        {
            // the DOMAIN's content: a change recomputes every target of the avatar
            a.sig_dirty = false;
            U64 h = 1469598103934665603ull;
            for (const auto& kv : a.domain) h = mixHash(mixHash(mixHash(h, kv.first), kv.second.sig.vc), kv.second.sig.ic);
            h |= 1u;   // (0 = "none")
            if (h != a.sig) { a.sig = h; a.changed = frame; }
        }
        if (a.domain.empty() && a.targets.empty() && a.job == 0) ait = mAvatars.erase(ait);
        else ++ait;
    }

    // 2. Live entries: other content / gone -> drop; not wanted -> age out; MOVED (compaction,
    // a repack) -> re-keyed by the slot's new weight base / vertex base.
    std::vector<Entry*> moved;
    for (auto it = mLive.begin(); it != mLive.end();)
    {
        Entry& e = it->second;
        const bool gone = !sigOk(e.slot, e.sig);
        if (gone || frame - e.last_use > kAgeFrames)
        {
            freeEntry(e);
            it = mLive.erase(it);
            if (!gone) ++mStatEvicted;
            continue;
        }
        const U32 key = rowKey(rows[e.slot]), vbase = (U32)rows[e.slot].base_vertex;
        if (key != e.key || vbase != e.vbase)
        {
            if (e.table != kEmpty) tableRemove(e.table);
            e.table = kEmpty;
            e.key = key;
            e.vbase = vbase;
            moved.push_back(&e);
        }
        ++it;
    }
    for (Entry* e : moved) e->table = tableInsert(*e);   // (unordered_map nodes are stable)

    // 3. Finished avatars -> centre ranges + uploads + table entries (all of an avatar at once:
    // its targets switch to the new domain's centres together).
    std::vector<std::unique_ptr<Job>> done;
    {
        std::lock_guard<std::mutex> lk(mMutex);
        done.swap(mJobsOut);
    }
    U64 committed = 0;
    for (size_t di = 0; di < done.size(); ++di)
    {
        if (committed >= kUploadWords)
        {
            std::lock_guard<std::mutex> lk(mMutex);   // the rest next frame
            for (size_t k = di; k < done.size(); ++k) mJobsOut.push_back(std::move(done[k]));
            break;
        }
        Job& job = *done[di];
        if (mInFlight) --mInFlight;
        auto ait = mAvatars.find(job.avatar);
        if (ait == mAvatars.end() || ait->second.job != job.id) continue;
        Avatar& a = ait->second;
        a.job = 0;
        mStatMsSum += job.ms;
        mStatMsMax = std::max(mStatMsMax, job.ms);
        if (!job.ok)
        {
            LL_INFOS("VKCoR") << "CoR precompute gave up on an avatar (" << job.domain_meshes << " meshes, "
                              << job.domain_verts << " vertices) after " << job.ms << " ms (" << job.st.clusters
                              << " clusters, work cap " << (job.st.aborted ? "hit" : "not hit")
                              << "): it stays on linear blend skinning" << LL_ENDL;
            a.retry = frame + kRetryFrames;
            a.built = job.sig;   // (not relaunched for the same content)
            ++mStatFailed;
            continue;
        }
        if (job.ms >= kSlowMs)
            LL_INFOS("VKCoR") << "CoR precompute: avatar domain " << job.domain_meshes << " meshes / " << job.domain_verts
                              << " vertices (" << job.st.tris << " triangles -> " << job.st.sub_tris << " subdivided, "
                              << job.st.clusters << " clusters), " << job.target_verts << " target vertices ("
                              << job.st.unique << " distinct weights) in " << job.ms << " ms" << LL_ENDL;
        else
            LL_DEBUGS("VKCoR") << "CoR precompute: avatar domain " << job.domain_meshes << " meshes / " << job.domain_verts
                               << " vertices (" << job.st.tris << " triangles -> " << job.st.sub_tris << " subdivided, "
                               << job.st.clusters << " clusters), " << job.target_verts << " target vertices ("
                               << job.st.unique << " distinct weights) in " << job.ms << " ms" << LL_ENDL;
        a.built = job.sig;
        a.missing = false;
        bool no_room = false;
        for (SlotCopy& s : job.slots)
        {
            if (!s.target || s.out.empty()) continue;
            const U64 key = itemKey(s.skin, s.slot);
            if (!a.targets.count(key) || !sigOk(s.slot, s.sig)) continue;   // left meanwhile
            dropEntry(key);   // (the previous domain's centres)
            if (mLive.size() >= kMaxLive && !evictLru(frame)) { no_room = true; continue; }
            const U32 words = s.sig.vc * kWordsPerVert;
            U32 first = 0;
            bool ok = alloc(words, first);
            while (!ok && evictLru(frame)) ok = alloc(words, first);
            if (!ok) { no_room = true; continue; }   // over the budget with every entry in use: LBS (#alloc policy)
            Entry e;
            e.skin = s.skin; e.slot = s.slot; e.avatar = job.avatar; e.sig = s.sig;
            e.key = rowKey(rows[s.slot]); e.vbase = (U32)rows[s.slot].base_vertex;
            e.first = first; e.words = words;
            e.last_use = frame;
            Upload up;
            up.first = first;
            up.data = std::move(s.out);
            mUploads.push_back(std::move(up));
            committed += words;
            e.table = tableInsert(e);
            mLive[key] = e;
            mStatVerts += s.sig.vc;
        }
        if (no_room) ++mStatNoRoom;
        // Targets still without centres: added after this job was launched (computed by the next
        // one), or not computable / no room (retried after kRetryFrames, never in a loop).
        for (const auto& kv : a.targets)
            if (!mLive.count(kv.first)) { a.missing = true; break; }
        if (a.missing && (no_room || (int32_t)(a.tchanged - job.launched) <= 0)) a.retry = frame + kRetryFrames;
        ++mStatBuilt;
        mStatMaxVerts = std::max(mStatMaxVerts, job.target_verts);
    }
    if (mTombs > kTableCap / 4) tableRebuild();

    // 4. Launch avatars whose content changed (once settled) or that lost entries.
    U64 copied = 0;
    for (auto& kv : mAvatars)
    {
        if (mInFlight >= kMaxInFlight || (copied >= kLaunchVerts && copied != 0)) break;
        Avatar& a = kv.second;
        if (a.job != 0 || a.domain.empty() || a.targets.empty()) continue;
        const bool changed = a.sig != a.built;
        if (!changed && !a.missing) continue;
        if (!changed && ((int32_t)(frame - a.retry) < 0 || frame - a.tchanged < kFirstSettle)) continue;
        if (changed && frame - a.changed < (a.built ? kSettleFrames : kFirstSettle)) continue;
        std::unique_ptr<Job> job = makeJob(kv.first, a, rows, nrows, meta, copied);
        if (!job) { a.built = a.sig; a.missing = false; continue; }   // (nothing copyable: asked again on change)
        a.job = job->id;
        a.missing = false;
        job->launched = frame;
        ++mInFlight;
        if (!mThread.joinable()) mThread = std::thread([this] { workerMain(); });
        {
            std::lock_guard<std::mutex> lk(mMutex);
            mJobsIn.push_back(std::move(job));
        }
        mCv.notify_one();
    }

    // 5. The summary line: about once a minute, while something changed.
    const F64 now = LLFrameTimer::getElapsedSeconds();
    if (now - mLogTime >= kLogSeconds
        && (mStatBuilt != mLogBuilt || mStatFailed != mLogFailed || mStatEvicted != mLogEvicted || mStatNoRoom != mLogNoRoom))
    {
        mLogTime = now;
        mLogBuilt = mStatBuilt; mLogFailed = mStatFailed; mLogEvicted = mStatEvicted; mLogNoRoom = mStatNoRoom;
        LL_INFOS("VKCoR") << statsLine() << LL_ENDL;
    }
}

std::string LLCleanRigCoR::statsLine() const
{
    std::ostringstream o;
    U64 live_verts = 0;
    for (const auto& kv : mLive) live_verts += kv.second.sig.vc;
    o << "CoR skinning: " << mAvatars.size() << " avatars, " << mLive.size() << " slots / " << live_verts
      << " vertices, centres " << (mUsedWords * 4u) / 1024u << " KiB (peak " << (mPeakWords * 4u) / 1024u << ") of "
      << (((U64)mCapWords * 4u) >> 20) << " MiB + table " << (kTableCap * kEntryWords * 4u) / 1024u << " KiB VRAM; built "
      << mStatBuilt << " avatar domains (" << mStatVerts << " vertices, avg "
      << ((mStatBuilt + mStatFailed) ? mStatMsSum / (F64)(mStatBuilt + mStatFailed) : 0.0) << " ms, max " << mStatMsMax
      << " ms, largest " << mStatMaxVerts << " target vertices), failed " << mStatFailed << ", no room " << mStatNoRoom
      << ", evicted " << mStatEvicted << ", in flight " << mInFlight;
    return o.str();
}
