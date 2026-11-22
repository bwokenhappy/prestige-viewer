// CPU fixture executes the production class accounting, draw limits and P-list calls.
#include "prepass_under_test.h"
#include <array>
#include <iostream>

struct Population
{
    std::vector<Instance> rows;
    std::vector<unsigned> buckets;
};

Population population(unsigned frame)
{
    // Deliberately unequal classes catch a P-row/counter-slot mixup (notably rows 2/3).
    const uint32_t flags[10] = {0,4,2,1,5,3,256,260,257,261};
    const unsigned counts[10] = {1100,37,500,300,90,400,173,23,77,31};
    Population p;
    for (unsigned b=0;b<10;++b)
        for (unsigned i=0;i<counts[b];++i)
        {
            unsigned klass=b;
            // Change hundreds of world rows between opaque/mask/sided classes.
            // Retain old populations below to model GPU rows behind the CPU mirror.
            if (b==0 && i<500) klass=std::array<unsigned,4>{0,1,6,7}[frame%4];
            Instance row;
            row.flags=flags[klass] | ((i%7==0) ? 2097152u : 0u);
            row.skin_id=(klass==3 || klass==4 || klass==5 || klass==8 || klass==9) ? 0u : 0xFFFFFFFFu;
            p.rows.push_back(row);
            p.buckets.push_back(klass);
        }
    return p;
}

void check(LLCleanRendererVK& renderer, const Population& gpu, bool tight)
{
    calls.clear();
    renderer.recordPLists();
    assert(calls.size()==4);
    const unsigned bucket[4]={0,1,6,7};
    for (unsigned phase=0;phase<2;++phase)
        for (unsigned row=0;row<4;++row)
        {
            unsigned count=0;
            for (unsigned id=0;id<gpu.rows.size();++id)
            {
                const bool inFrustum=id%7!=0;
                const bool visibleBefore=id%3!=0;
                const bool visibleNow=id%5!=0;
                const bool belongs=phase==0 ? visibleBefore : visibleNow && !visibleBefore;
                count += gpu.buckets[id]==bucket[row] && inFrustum && belongs;
            }
            const Call& c=calls[row];
            assert(c.offset==uint64_t(row)*renderer.mOccl2Cap*sizeof(DrawCmd));
            assert(c.countOffset==row*sizeof(uint32_t) && c.stride==sizeof(DrawCmd));
            assert(c.maximum<=renderer.mOccl2Cap && c.maximum>=count);
            // The actual indirect count/stream is unchanged: a clamp must retain every draw.
            assert(std::min(count,c.maximum)==count);
            if (tight) assert(c.maximum<renderer.mOccl2Cap/2);
        }
}

int main()
{
    LLCleanRendererVK renderer;
    std::vector<Population> history;
    for (unsigned frame=0;frame<40;++frame)
    {
        history.push_back(population(frame));
        renderer.uploadMirror(history.back().rows);
        const unsigned first=frame+1>renderer.kClassRing ? frame+1-renderer.kClassRing : 0;
        for (unsigned old=first;old<=frame;++old) check(renderer,history[old],true);
    }
    renderer.mInstFullUpload=true;
    check(renderer,history.front(),false);
    for (const auto& c:calls) assert(c.maximum==renderer.mdiMax());
    renderer.mInstFullUpload=false;
    boundsEnabled=false;
    check(renderer,history.back(),false);
    for (const auto& c:calls) assert(c.maximum==renderer.mdiMax());
    boundsEnabled=true;
    renderer.mInstLiveCount=0xFFFFFFFFu;
    check(renderer,history.back(),false);
    for (const auto& c:calls) assert(c.maximum==renderer.mOccl2Cap);
    std::cout << "P1/P2 subsets retained through class-history/upload/off/unknown cases\n";
}
