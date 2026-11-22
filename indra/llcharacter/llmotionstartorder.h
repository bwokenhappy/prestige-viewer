/**
 * @file llmotionstartorder.h
 * @brief Where an activating motion goes in LLMotionController's active list (#1518).
 *
 * The active list is ordered newest START first, and that order breaks blend ties between
 * equal priorities: updateMotionsByType skips a fully weighted motion whose joint
 * signature adds nothing to the motions before it, and LLJointStateBlender::addJointState
 * keeps earlier-added (newer) states ahead at equal priority -- at weight 1 the first one
 * wins outright. LL activates a motion whose data is cached synchronously, in start order.
 * Since the cached read moved to a worker (LLKeyframeMotion::readAssetAsync) a motion's
 * activation can come frames after its start request, and pushing it to the front then
 * made it "newer" than motions started after it: on load-in an avatar's animations ended
 * up in read-completion order (a sit lost its pelvis and legs to an equal-priority
 * animation until a stand / sit restarted it). The controller stamps each start request
 * and inserts the activating motion where its stamp belongs; a fresh stamp (a direct
 * start) is the largest, so that is LL's push_front.
 *
 * Header-only and free of LL types so scripts/check/test_motion_start_order.cpp can run it.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * $/LicenseInfo$
 */

#pragma once

#include <cstdint>

// First position in `list` (newest first, by seq_of descending) whose element is not newer
// than `seq`: inserting there keeps the order. `newer` (optional) counts the elements
// skipped -- motions started after this one that are already active.
template <class List, class SeqOf>
typename List::iterator llMotionStartOrderPos(List& list, std::uint64_t seq, SeqOf seq_of, std::uint32_t* newer = nullptr)
{
    typename List::iterator pos = list.begin();
    std::uint32_t n = 0;
    while (pos != list.end() && seq_of(*pos) > seq)
    {
        ++pos;
        ++n;
    }
    if (newer)
    {
        *newer = n;
    }
    return pos;
}
