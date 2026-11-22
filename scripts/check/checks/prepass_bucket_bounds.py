"""Execute production indirect-draw limits against both phases and lagged class changes."""
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

NAME = 'prepass-bucket-bounds'
STAGE = 'source'
DESCRIPTION = 'prepass indirect limits never clip P1/P2; class history and upload fallback'
GUARDS = 'Global-capacity prepass scans, or clipped opaque/mask draws after class changes and lagged uploads.'
CPP = 'indra/newview/vkgfx/llcleanrenderervk.cpp'


def body(text, signature):
    start = text.index(signature)
    brace = text.index('{', start)
    depth = 1
    end = brace + 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start:end]


def run(ctx, result):
    cxx = os.environ.get('CXX') or shutil.which('c++') or shutil.which('clang++')
    if not cxx:
        result.fail('C++ compiler required for the no-clipping fixture')
        return
    source = Path(ctx.path(CPP)).read_text()
    header = Path(ctx.path('indra/newview/vkgfx/llcleanrenderervk.h')).read_text()
    ring = int(re.search(r'kClassRing\s*=\s*(\d+)', header)[1])
    rows = int(re.search(r'kOccl2PRows\s*=\s*(\d+)', source)[1])
    methods = '\n'.join(body(source, token) for token in (
        'uint32_t LLCleanRendererVK::mdiMax() const',
        'void LLCleanRendererVK::updateInstClassCounts(size_t run_from)',
        'uint32_t LLCleanRendererVK::mdiMaxBucket(uint32_t b, bool dyn) const'))
    draw = body(source, 'auto drawPList =') + ';'
    stub = r'''
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <utility>
#include <vector>
using VkCommandBuffer = int;
using VkPipeline = int;
using VkDeviceSize = uint64_t;
constexpr uint32_t BUCKET_OPAQUE=0, BUCKET_MASK=1;
constexpr uint32_t kOccl2PRows=ROWS;
struct DrawCmd { uint32_t indexCount, instanceCount, firstIndex; int32_t vertexOffset; uint32_t firstInstance; };
struct Instance { uint32_t lod_group_id=0, flags=0, skin_id=0xFFFFFFFFu; };
struct Call { VkDeviceSize offset, countOffset; uint32_t maximum, stride; };
std::vector<Call> calls;
bool boundsEnabled=true;
int gSavedSettings=0;
template<class T> struct LLCachedControl {
    LLCachedControl(int, const char*, T) {}
    operator T() const { return boundsEnabled; }
};
#define LL_HAS_CLEAN_PIPELINE 1
void vkCmdDrawIndexedIndirectCount(int, int buffer, VkDeviceSize off, int countBuffer,
                                  VkDeviceSize countOff, uint32_t maximum, uint32_t stride) {
    assert(buffer==11 && countBuffer==12);
    calls.push_back({off,countOff,maximum,stride});
}
struct LLCleanRendererVK {
    // This fixture executes the unchanged draw limits; the separate scope fixture
    // extracts and exercises the real query recorder, so it is inert here.
    struct GpuPassScope { template<class... T> GpuPassScope(T&&...) {} };
    static constexpr uint32_t NUM_BUCKETS=10, kClassRing=RING;
    uint32_t mBucketCap=8192, mOccl2Cap=8192, mInstLiveCount=0, mClassRingPos=0;
    uint32_t mAvDepthFaceCount=0;   // (tolerated: the extracted accounting may count avatar-depth faces)
    bool mInstFullUpload=false;
    struct Buffer { int buffer; } mOccl2List{11}, mOccl2Count{12};
    uint32_t mClassCount[NUM_BUCKETS]{}, mClassDynCount[NUM_BUCKETS]{};
    uint32_t mClassRing[kClassRing][2][NUM_BUCKETS]{};
    std::vector<char> mCpuInstances;
    std::vector<uint8_t> mInstClass;
    std::vector<std::pair<uint32_t,uint32_t>> mInstRuns;
    uint32_t mdiMax() const;
    uint32_t mdiMaxBucket(uint32_t b, bool dyn=false) const;
    void updateInstClassCounts(size_t run_from);
    void bindGraphicsPipeline(int, int) {}
    void recordPLists();
    void uploadMirror(const std::vector<Instance>& rows) {
        mCpuInstances.resize(rows.size()*sizeof(Instance));
        std::memcpy(mCpuInstances.data(),rows.data(),mCpuInstances.size());
        mInstLiveCount=uint32_t(rows.size());
        mInstRuns={{0,uint32_t(rows.size())}};
        updateInstClassCounts(0);
    }
};
'''.replace('ROWS', str(rows)).replace('RING', str(ring))
    draw_method = 'void LLCleanRendererVK::recordPLists() { VkCommandBuffer cmd=1;\n' + draw + '\nfor (uint32_t r=0;r<kOccl2PRows;++r) drawPList(1,r);\n}\n'
    with tempfile.TemporaryDirectory() as tmp:
        Path(tmp, 'prepass_under_test.h').write_text(stub + methods + draw_method)
        exe = str(Path(tmp, 'fixture'))
        compiled = subprocess.run([cxx, '-std=c++17', '-O1', '-Wall', '-I'+tmp,
                                   ctx.path('scripts/check/test_prepass_bucket_bounds.cpp'), '-o', exe],
                                  capture_output=True, text=True)
        if compiled.returncode:
            result.fail('compile failed:\n'+compiled.stdout+compiled.stderr)
            return
        checked = subprocess.run([exe], capture_output=True, text=True)
        if checked.returncode:
            result.fail(checked.stdout+checked.stderr)
            return
    result.summary = 'P1/P2 draw sets intact; unequal buckets; 4-frame class changes; full-upload/unknown/off fallbacks'
