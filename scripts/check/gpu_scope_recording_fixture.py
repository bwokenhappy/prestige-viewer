"""Extract the production RAII recorder against a value-only Vulkan query sink."""
from pathlib import Path


def fixture(root):
    header = (Path(root) / "indra/newview/vkgfx/llcleanrenderervk.h").read_text()
    cpp = (Path(root) / "indra/newview/vkgfx/llcleanrenderervk.cpp").read_text()
    start = header.index("    class GpuPassScope\n")
    stop = header.index("private:\n    void gpuSubMark", start)
    declarations = header[start:stop]
    start = cpp.index("LLCleanRendererVK::GpuPassScope::GpuPassScope(")
    stop = cpp.index("void LLCleanRendererVK::gpuSubMark", start)
    definitions = cpp[start:stop]
    return r'''
#include "vkgfx/vk_gpu_scope_math.h"
#include <cassert>
#include <cstdint>
#include <type_traits>
#include <vector>
using VkCommandBuffer = uint64_t;
using VkQueryPool = uint64_t;
constexpr uint64_t VK_NULL_HANDLE = 0;
constexpr uint64_t VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT = 1;
struct Write { uint64_t cmd, pool; uint32_t index; };
std::vector<Write> writes;
bool vkAsyncIsAsyncCmd(VkCommandBuffer cmd) { return cmd == 2; }
void vkCmdWriteTimestamp2(VkCommandBuffer cmd, uint64_t, VkQueryPool pool, uint32_t index)
{ writes.push_back({cmd, pool, index}); }
struct LLCleanRendererVK
{
    static constexpr uint32_t SUB_TS = 24, kFeTs = 12;
    static constexpr uint8_t kSubFiller = 4;
    bool mTsOn = true, mScopeRecording = true, mTsLean = false, mAsyncAvail = true, mAsyncTs = true;
    uint32_t mMvViews = 0, mTsSlot = 0, mSubCount = 0, mFeCount = 0;
    VkCommandBuffer mFeCmd = 3, mScopeFrameCmd = 1, mAsyncGfx = 0;
    VkQueryPool mSubPool = 10, mFePool = 20;
    uint32_t mScopeOverflowR[1]{};
    uint16_t mSubScopeEndR[1][SUB_TS]{}, mFeScopeEndR[1][kFeTs]{};
    uint8_t mFeKindR[1][kFeTs]{}, queue[SUB_TS]{};
    uint8_t* mSubQueue = queue;
    const char* mSubNames[SUB_TS]{}, *mFeNames[kFeTs]{};
''' + declarations + "\n};\n" + definitions + r'''
int main()
{
    LLCleanRendererVK r;
    r.mTsOn = false;
    { LLCleanRendererVK::GpuPassScope scope(r, 1, "off"); }
    assert(writes.empty() && r.mSubCount == 0);
    r.mTsOn = true;
    { LLCleanRendererVK::GpuPassScope scope(r, 4, "immediate"); }
    assert(writes.empty() && r.mSubCount == 0);
    r.mScopeRecording = false;
    { LLCleanRendererVK::GpuPassScope scope(r, 1, "outside"); }
    assert(writes.empty()); r.mScopeRecording = true;
    { LLCleanRendererVK::GpuPassScope a(r, 1, "a");
      { LLCleanRendererVK::GpuPassScope b(r, 1, "b"); } }
    assert(writes.size() == 4 && writes[0].index == 0 && writes[1].index == 2
        && writes[2].index == 3 && writes[3].index == 1);
    assert(r.mSubScopeEndR[0][0] == 1 && r.mSubScopeEndR[0][2] == 3);
    assert(!r.gpuPass(1, "empty", [] { return false; }));
    assert(writes.size() == 6 && r.queue[4] == 4 && r.queue[5] == 4);
    r.mMvViews = 2;
    { LLCleanRendererVK::GpuPassScope scope(r, 1, "stereo"); }
    assert(r.mSubCount == 10 && writes[6].index == 6 && writes[7].index == 8);
    { LLCleanRendererVK::GpuPassScope scope(r, 2, "async"); }
    assert(r.mSubCount == 12 && r.queue[10] == 7 && r.queue[11] == 8);
    { LLCleanRendererVK::GpuPassScope scope(r, 3, "frontend"); }
    assert(r.mFeCount == 2 && writes.back().pool == 20 && r.mFeKindR[0][0] == 1);
    r.mAsyncTs = false; const auto size = writes.size();
    { LLCleanRendererVK::GpuPassScope scope(r, 2, "no async clock"); }
    assert(writes.size() == size);
    r.mSubCount = 23;
    { LLCleanRendererVK::GpuPassScope scope(r, 1, "full"); }
    assert(r.mSubCount == 23 && writes.size() == size && r.mScopeOverflowR[0] == 1);
    r.mSubCount = 12;
    auto early = [&] { LLCleanRendererVK::GpuPassScope scope(r, 1, "early"); return; };
    early(); assert(writes.size() == size + 2);
}
'''
