// CPU fixture for the motion START ORDER (#1518; indra/llcharacter/llmotionstartorder.h, check motion-start-order,
// scripts/check/checks/motion_start_order.py).
//
// LLMotionController keeps its active motions newest-start first, and at EQUAL priority the first one wins a joint:
// updateMotionsByType skips a fully weighted motion whose joint signature adds no bits (0xff >> (7 - priority) is the
// same byte for both), and LLJointStateBlender::addJointState keeps the earlier-added state in slot 0, which at
// weight 1 blends to itself (lerp factor sum / new_sum = 1). clean_anim.comp mirrors that order exactly.
//
// LL reference: a cached animation loads synchronously, so every start activates at once -> push_front in START order.
// Since the cached read went to a worker, a start parks the motion in the loading set and it activates when its read
// lands. The OLD rule pushed it to the front then (read-completion order: the bug -- a sit started after an
// equal-priority animation lost the pelvis / hind legs to it when its read landed first, until a stand / sit). The NEW
// rule stamps the start request and inserts the motion where the stamp belongs (llMotionStartOrderPos).
//
// Asserts, for every start order x read-completion order of 4 equal-priority motions (and with some of them already
// in memory, i.e. synchronous): NEW active order == LL reference order; the OLD rule differs from the reference in
// the #1518 case (fixture sanity). Plus: a direct start / restart after late loads is the newest; a start withdrawn
// (stopped) while loading and requested again takes the new request's place; a repeated request while loading keeps
// the first.
#include "llmotionstartorder.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <list>
#include <vector>

namespace
{
int g_fail = 0;
void expect(bool ok, const char* what)
{
    if (!ok) { std::printf("FAIL: %s\n", what); ++g_fail; }
}

struct Motion
{
    int id = 0;
    bool loading = false;       // in mLoadingMotions
    bool stopped = true;
    bool active = false;
    std::uint64_t active_seq = 0;
    std::uint64_t pending = 0;
};

// The controller's bookkeeping, term for term as llmotioncontroller.cpp (activateMotionInstance / stopMotionInstance
// / updateLoadingMotions), with the insertion rule switchable to the pre-#1518 push_front.
struct Controller
{
    bool new_rule = true;
    std::list<Motion*> active;
    std::uint64_t counter = 0;

    void activate(Motion* m)
    {
        if (m->loading)
        {
            if (m->stopped || m->pending == 0) m->pending = ++counter;
            m->stopped = false;
            return;
        }
        const std::uint64_t seq = m->pending ? m->pending : ++counter;
        m->pending = 0;
        m->active_seq = seq;
        if (m->active) active.remove(m);
        if (new_rule)
            active.insert(llMotionStartOrderPos(active, seq, [](const Motion* x) { return x->active_seq; }), m);
        else
            active.push_front(m);
        m->active = true;
        m->stopped = false;
    }
    void stop(Motion* m)
    {
        if (m->active && !m->stopped) { m->stopped = true; active.remove(m); m->active = false; }
        else if (m->loading) { m->stopped = true; m->pending = 0; }
    }
    void loaded(Motion* m)      // updateLoadingMotions: the read landed
    {
        m->loading = false;
        if (!m->stopped) activate(m);
    }
    std::vector<int> order() const
    {
        std::vector<int> o;
        for (const Motion* m : active) o.push_back(m->id);
        return o;
    }
    int winner() const { return active.empty() ? -1 : active.front()->id; }   // equal priority: first wins
};

// LL reference: synchronous loads, push_front at every start.
std::vector<int> llReference(const std::vector<int>& starts)
{
    std::list<int> l;
    for (int id : starts) { l.remove(id); l.push_front(id); }
    return std::vector<int>(l.begin(), l.end());
}

std::vector<int> run(bool new_rule, const std::vector<int>& starts, const std::vector<int>& completes,
                     const std::vector<bool>& cached)
{
    std::vector<Motion> ms(starts.size());
    Controller c;
    c.new_rule = new_rule;
    for (size_t i = 0; i < ms.size(); ++i) { ms[i].id = (int)i; ms[i].loading = !cached[i]; }
    for (int id : starts) c.activate(&ms[id]);
    for (int id : completes) if (ms[id].loading) c.loaded(&ms[id]);
    return c.order();
}
} // namespace

int main()
{
    // 1. The #1518 case: B (the sit) started after A (an equal-priority animation keying the same joints); A's read
    //    lands last. LL: B wins. Old rule: A wins (the broken sit). New rule: B wins.
    {
        const std::vector<int> starts = {0, 1}, completes = {1, 0};
        const std::vector<bool> cached = {false, false};
        expect(llReference(starts).front() == 1, "reference: the later start wins the tie");
        expect(run(false, starts, completes, cached).front() == 0,
               "fixture sanity: the old rule lets the earlier-started, later-loaded motion win (the #1518 bug)");
        expect(run(true, starts, completes, cached).front() == 1, "new rule: the later start wins the tie");
    }

    // 2. Exhaustive: 4 motions, every start order x every completion order x every cached (synchronous) subset.
    int cases = 0, old_wrong = 0;
    std::vector<int> starts = {0, 1, 2, 3};
    do
    {
        std::vector<int> completes = {0, 1, 2, 3};
        do
        {
            for (int mask = 0; mask < 16; ++mask)
            {
                std::vector<bool> cached(4);
                for (int i = 0; i < 4; ++i) cached[i] = (mask >> i) & 1;
                const std::vector<int> ref = llReference(starts);
                const std::vector<int> got = run(true, starts, completes, cached);
                if (got != ref)
                {
                    std::printf("FAIL: start %d%d%d%d complete %d%d%d%d cached 0x%x: order %d%d%d%d, LL %d%d%d%d\n",
                                starts[0], starts[1], starts[2], starts[3], completes[0], completes[1], completes[2],
                                completes[3], mask, got[0], got[1], got[2], got[3], ref[0], ref[1], ref[2], ref[3]);
                    ++g_fail;
                }
                if (run(false, starts, completes, cached) != ref) ++old_wrong;
                ++cases;
            }
        } while (std::next_permutation(completes.begin(), completes.end()));
    } while (std::next_permutation(starts.begin(), starts.end()));
    expect(old_wrong > 0, "fixture sanity: the old rule departs from LL's order for some load-completion orders");

    // 3. Later starts after late loads: a direct start (data in memory) and a restart of an active motion are the
    //    newest; a stop while loading withdraws the request; a repeated request while loading keeps the first.
    {
        std::vector<Motion> ms(4);
        for (int i = 0; i < 4; ++i) { ms[i].id = i; ms[i].loading = true; }
        Controller c;
        c.activate(&ms[0]);                 // seq 1
        c.activate(&ms[1]);                 // seq 2
        c.activate(&ms[0]);                 // repeated request while loading: keeps seq 1
        c.loaded(&ms[1]);
        c.loaded(&ms[0]);
        expect(c.order() == std::vector<int>({1, 0}), "repeated request while loading keeps its first place");
        ms[2].loading = false;
        c.activate(&ms[2]);                 // direct start: newest
        expect(c.winner() == 2, "a direct start after late loads is the newest");
        c.activate(&ms[0]);                 // restart of an active motion: newest (stand / sit cycle)
        expect(c.order() == std::vector<int>({0, 2, 1}), "a restart moves the motion to the front");
        c.activate(&ms[3]);                 // loading, seq 5
        c.stop(&ms[3]);                     // withdrawn
        c.activate(&ms[1]);                 // restart: seq 6, front
        c.activate(&ms[3]);                 // requested again: seq 7
        c.loaded(&ms[3]);
        expect(c.winner() == 3, "a start withdrawn while loading takes its new request's place");
        expect(c.order() == std::vector<int>({3, 1, 0, 2}), "order after withdraw / re-request");
    }

    if (g_fail)
    {
        std::printf("%d failure(s)\n", g_fail);
        return 1;
    }
    std::printf("ok: %d start x completion x cached cases match LL's start order (old rule wrong in %d)\n", cases,
                old_wrong);
    return 0;
}
