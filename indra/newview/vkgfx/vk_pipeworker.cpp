/**
 * @file vk_pipeworker.cpp
 * @brief See vk_pipeworker.h.
 */
#include "linden_common.h"

#include "vk_pipeworker.h"
#include "vk_device.h"
#include "vk_pipeline.h"
#include "vk_compile_governor.h"
#include "vk_pipecache.h"

#include "llfile.h"
#include "llmemory.h"
#include "llmemorygovernor.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>
#include <unordered_map>

#if LL_WINDOWS
#include "llwin32headers.h"
#include <psapi.h>
#else
#include <csignal>
#include <fcntl.h>
#include <spawn.h>
#include <sys/file.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>
#if LL_LINUX
#include <sys/prctl.h>
#endif
extern char** environ;
#endif

// ======================================================================= layout registry
namespace
{
struct SetLayoutRec
{
    uint32_t flags = 0;
    std::vector<VkDescriptorSetLayoutBinding> bindings;   // (pImmutableSamplers dropped)
    std::vector<VkDescriptorBindingFlags> bindingFlags;
    bool hasBindingFlags = false;                         // a VkDescriptorSetLayoutBindingFlagsCreateInfo was chained
    bool immutable = false;                               // has immutable samplers: not describable
};
struct PipeLayoutRec
{
    uint32_t flags = 0;
    std::vector<VkDescriptorSetLayout> sets;
    std::vector<VkPushConstantRange> push;
};
constexpr size_t kMaxLayouts = 4096;
std::mutex& layoutMutex() { static std::mutex m; return m; }
std::unordered_map<VkDescriptorSetLayout, SetLayoutRec>& setLayouts() { static std::unordered_map<VkDescriptorSetLayout, SetLayoutRec> m; return m; }
std::unordered_map<VkPipelineLayout, PipeLayoutRec>& pipeLayouts() { static std::unordered_map<VkPipelineLayout, PipeLayoutRec> m; return m; }
}

void vkPipelineNoteSetLayout(VkDescriptorSetLayout l, const VkDescriptorSetLayoutCreateInfo* ci)
{
    if (!l || !ci) return;
    SetLayoutRec r;
    r.flags = ci->flags;
    const VkDescriptorSetLayoutBindingFlagsCreateInfo* bfi = nullptr;
    for (const VkBaseInStructure* p = (const VkBaseInStructure*)ci->pNext; p; p = p->pNext)
        if (p->sType == VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO)
            bfi = (const VkDescriptorSetLayoutBindingFlagsCreateInfo*)p;
    r.hasBindingFlags = bfi != nullptr;
    for (uint32_t i = 0; i < ci->bindingCount; ++i)
    {
        VkDescriptorSetLayoutBinding b = ci->pBindings[i];
        if (b.pImmutableSamplers) r.immutable = true;
        b.pImmutableSamplers = nullptr;
        r.bindings.push_back(b);
        r.bindingFlags.push_back((bfi && i < bfi->bindingCount && bfi->pBindingFlags) ? bfi->pBindingFlags[i] : 0);
    }
    std::lock_guard<std::mutex> lk(layoutMutex());
    if (setLayouts().size() >= kMaxLayouts) setLayouts().clear();
    setLayouts()[l] = std::move(r);
}

void vkPipelineNotePipelineLayout(VkPipelineLayout l, const VkPipelineLayoutCreateInfo* ci)
{
    if (!l || !ci) return;
    PipeLayoutRec r;
    r.flags = ci->flags;
    r.sets.assign(ci->pSetLayouts, ci->pSetLayouts + ci->setLayoutCount);
    r.push.assign(ci->pPushConstantRanges, ci->pPushConstantRanges + ci->pushConstantRangeCount);
    std::lock_guard<std::mutex> lk(layoutMutex());
    if (pipeLayouts().size() >= kMaxLayouts) pipeLayouts().clear();
    pipeLayouts()[l] = std::move(r);
}

void vkPipelineForgetSetLayout(VkDescriptorSetLayout l)
{
    std::lock_guard<std::mutex> lk(layoutMutex());
    setLayouts().erase(l);
}

uint64_t vkPipelineLayoutContentHash(VkPipelineLayout layout)
{
    if (!layout) return 0;
    uint64_t h = 1469598103934665603ull;
    auto u = [&](uint64_t v) {
        for (int i = 0; i < 8; ++i) { h ^= (uint8_t)(v >> (8 * i)); h *= 1099511628211ull; }
    };
    std::lock_guard<std::mutex> lk(layoutMutex());
    auto it = pipeLayouts().find(layout);
    if (it == pipeLayouts().end()) return 0;
    u(it->second.flags);
    u(it->second.sets.size());
    for (VkDescriptorSetLayout sl : it->second.sets)
    {
        auto s = setLayouts().find(sl);
        if (!sl) { u(0xD0u); continue; }
        if (s == setLayouts().end()) return 0;   // (a set layout never registered: no content to hash)
        const SetLayoutRec& r = s->second;
        u(r.flags);
        u(r.hasBindingFlags);
        u(r.immutable);
        u(r.bindings.size());
        for (size_t k = 0; k < r.bindings.size(); ++k)
        {
            u(r.bindings[k].binding); u(r.bindings[k].descriptorType); u(r.bindings[k].descriptorCount);
            u(r.bindings[k].stageFlags); u(r.bindingFlags[k]);
        }
    }
    u(it->second.push.size());
    for (const VkPushConstantRange& p : it->second.push) { u(p.stageFlags); u(p.offset); u(p.size); }
    return h ? h : 1;
}

// ======================================================================= recipe I/O
namespace
{
constexpr char kRecipeMagic[4] = {'S', 'L', 'P', 'R'};
// 2 (#1411 reuse): + the viewer's device request / fingerprint, set layouts' binding-flags presence,
// pipeline layouts' flags, GraphicsPipelineDesc::oitMrt.
// 3: compute items' compFlags may carry the private vk_pipeline.h kStageFlagWave32 bit, which only a build that
// translates it may read -- an older worker would hand it to the driver as a stage flag, so it refuses version 3.
// 4 (#1517): + GraphicsPipelineDesc::premultipliedOver (an older worker would build that pipeline without it).
// 5: module names distinguish selected wire twins. Version 4 called wire SPIR-V "Frag" /
// "FragBlend", so respinning those names could select plain code. Regenerate the recipe once
// through the in-session worker; existing main/worker VkPipelineCache blobs remain valid.
// 6 (MBOIT): + GraphicsPipelineDesc::addMrt (an older worker would build the moments pipelines unblended).
constexpr uint32_t kRecipeVersion = 6;

struct Writer
{
    std::string s;
    void u32(uint32_t v) { s.append(reinterpret_cast<const char*>(&v), 4); }
    void i32(int32_t v) { s.append(reinterpret_cast<const char*>(&v), 4); }
    void f32(float v) { s.append(reinterpret_cast<const char*>(&v), 4); }
    void bytes(const void* p, size_t n) { s.append(static_cast<const char*>(p), n); }
    void str(const std::string& v) { u32((uint32_t)v.size()); s += v; }
};
struct Reader
{
    const std::string& s;
    size_t pos = 0;
    bool bad = false;
    explicit Reader(const std::string& d) : s(d) {}
    bool take(void* p, size_t n)
    {
        if (bad || pos + n > s.size()) { bad = true; std::memset(p, 0, n); return false; }
        std::memcpy(p, s.data() + pos, n);
        pos += n;
        return true;
    }
    uint32_t u32() { uint32_t v = 0; take(&v, 4); return v; }
    int32_t i32() { int32_t v = 0; take(&v, 4); return v; }
    float f32() { float v = 0; take(&v, 4); return v; }
    std::string str()
    {
        const uint32_t n = u32();
        if (bad || n > (64u << 20) || pos + n > s.size()) { bad = true; return std::string(); }
        std::string r = s.substr(pos, n);
        pos += n;
        return r;
    }
    // A count bounded by what is left (malformed input never allocates without bound).
    uint32_t count(size_t minEach)
    {
        const uint32_t n = u32();
        if (bad || (size_t)n * std::max<size_t>(minEach, 1) > s.size() - pos) { bad = true; return 0; }
        return n;
    }
};

// Every GraphicsPipelineDesc field that reaches the driver (the handles are indices elsewhere).
void writeDesc(Writer& w, const GraphicsPipelineDesc& d)
{
    w.u32(d.colorFormat); w.u32(d.depthFormat); w.u32(d.depthTest); w.u32(d.depthWrite); w.u32(d.blend);
    w.u32(d.additive); w.u32(d.glowFormat); w.u32(d.glowWrite); w.u32(d.reactFormat); w.u32(d.reactWriteMask);
    w.u32(d.mrtCount);
    for (uint32_t i = 0; i < GraphicsPipelineDesc::kMaxMrt; ++i) w.u32(d.mrtFormats[i]);
    w.u32(d.polygonMode); w.u32(d.depthCompare); w.u32(d.relaxedDepthCompare);
    w.f32(d.depthBiasConstant); w.f32(d.depthBiasSlope); w.f32(d.lineWidth);
    w.u32(d.cullMode); w.u32(d.frontFace); w.u32(d.noColor); w.u32(d.depthClamp); w.u32(d.customBlend);
    w.u32(d.srcColor); w.u32(d.dstColor); w.u32(d.partAccum); w.u32(d.glowAdditive); w.u32(d.alphaOne);
    w.u32(d.colorWrite); w.u32(d.dynamicDepthCull); w.u32(d.dynamicShadingRate); w.u32(d.fsrAttachment);
    w.u32(d.reverseZ); w.u32(d.views); w.u32(d.disableOptimization); w.u32(d.oitMrt); w.u32(d.premultipliedOver);
    w.u32(d.addMrt);
    w.u32((uint32_t)d.bindings.size());
    for (const auto& b : d.bindings) { w.u32(b.binding); w.u32(b.stride); w.u32(b.inputRate); }
    w.u32((uint32_t)d.attributes.size());
    for (const auto& a : d.attributes) { w.u32(a.location); w.u32(a.binding); w.u32(a.format); w.u32(a.offset); }
}
void readDesc(Reader& r, GraphicsPipelineDesc& d)
{
    d.colorFormat = (VkFormat)r.u32(); d.depthFormat = (VkFormat)r.u32(); d.depthTest = r.u32(); d.depthWrite = r.u32();
    d.blend = r.u32(); d.additive = r.u32(); d.glowFormat = (VkFormat)r.u32(); d.glowWrite = r.u32();
    d.reactFormat = (VkFormat)r.u32(); d.reactWriteMask = r.u32(); d.mrtCount = std::min(r.u32(), GraphicsPipelineDesc::kMaxMrt);
    for (uint32_t i = 0; i < GraphicsPipelineDesc::kMaxMrt; ++i) d.mrtFormats[i] = (VkFormat)r.u32();
    d.polygonMode = (VkPolygonMode)r.u32(); d.depthCompare = (VkCompareOp)r.u32(); d.relaxedDepthCompare = (VkCompareOp)r.u32();
    d.depthBiasConstant = r.f32(); d.depthBiasSlope = r.f32(); d.lineWidth = r.f32();
    d.cullMode = r.u32(); d.frontFace = (VkFrontFace)r.u32(); d.noColor = r.u32(); d.depthClamp = r.u32();
    d.customBlend = r.u32(); d.srcColor = (VkBlendFactor)r.u32(); d.dstColor = (VkBlendFactor)r.u32();
    d.partAccum = r.u32(); d.glowAdditive = r.u32(); d.alphaOne = r.u32(); d.colorWrite = r.u32();
    d.dynamicDepthCull = r.u32(); d.dynamicShadingRate = r.u32(); d.fsrAttachment = r.u32(); d.reverseZ = r.u32();
    d.views = std::max(1u, r.u32()); d.disableOptimization = r.u32(); d.oitMrt = r.u32(); d.premultipliedOver = r.u32();
    d.addMrt = r.u32();
    const uint32_t nb = r.count(12);
    d.bindings.resize(nb);
    for (auto& b : d.bindings) { b.binding = r.u32(); b.stride = r.u32(); b.inputRate = (VkVertexInputRate)r.u32(); }
    const uint32_t na = r.count(16);
    d.attributes.resize(na);
    for (auto& a : d.attributes) { a.location = r.u32(); a.binding = r.u32(); a.format = (VkFormat)r.u32(); a.offset = r.u32(); }
}

bool writeHeader(Writer& w, VkPhysicalDevice phys)
{
    // #1411: the device this process created (its request and what it was created with): the
    // worker creates the same one and checks it got it.
    std::string request, fpText;
    uint64_t fp = 0;
    if (!vkDeviceLastParity(request, fp, fpText)) return false;
    VkPhysicalDeviceProperties p{};
    vkGetPhysicalDeviceProperties(phys, &p);
    w.bytes(kRecipeMagic, 4);
    w.u32(kRecipeVersion);
    w.u32(p.vendorID); w.u32(p.deviceID); w.u32(p.driverVersion);
    w.bytes(p.pipelineCacheUUID, VK_UUID_SIZE);
    w.str(request);
    w.u32((uint32_t)fp); w.u32((uint32_t)(fp >> 32));
    w.str(fpText);
    return true;
}

// The lines of `a` missing from `b` (fingerprint texts: one item per line).
std::vector<std::string> linesNotIn(const std::string& a, const std::string& b)
{
    auto split = [](const std::string& s) {
        std::vector<std::string> v;
        size_t pos = 0;
        while (pos < s.size())
        {
            size_t eol = s.find('\n', pos);
            if (eol == std::string::npos) eol = s.size();
            if (eol > pos) v.push_back(s.substr(pos, eol - pos));
            pos = eol + 1;
        }
        return v;
    };
    const std::vector<std::string> va = split(a), vb = split(b);
    std::vector<std::string> out;
    for (const std::string& l : va)
        if (std::find(vb.begin(), vb.end(), l) == vb.end()) out.push_back(l);
    return out;
}

// Replace `path` with `tmp` atomically (Windows: MoveFileExW REPLACE_EXISTING, never remove-then-
// rename: a kill between the two lost the file). A reader holding the file open (no share-delete)
// refuses the replace for a moment: retried briefly.
bool replaceFile(const std::string& tmp, const std::string& path)
{
    for (int i = 0; i < 10; ++i)
    {
        if (LLFile::replace(tmp, path, i < 9 ? EACCES : 0) == 0) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return false;
}
}

std::string vkPipelineJobRecipe(const PipelineJob* job, VkPhysicalDevice phys)
{
    const size_t n = vkPipelineJobSize(job);
    if (!n || !phys) return std::string();
    std::vector<VkPipelineRecipeItem> items(n);
    for (size_t i = 0; i < n; ++i) vkPipelineJobItem(job, i, items[i]);
    std::vector<VkShaderModule> mods;
    std::vector<VkPipelineLayout> pls;
    std::vector<VkDescriptorSetLayout> sls;
    auto idxOf = [](auto& v, auto h) -> int32_t {
        if (!h) return -1;
        auto it = std::find(v.begin(), v.end(), h);
        if (it != v.end()) return (int32_t)(it - v.begin());
        v.push_back(h);
        return (int32_t)v.size() - 1;
    };
    Writer body;
    body.u32((uint32_t)n);
    std::vector<int32_t> plIdx(n);
    for (const VkPipelineRecipeItem& it : items)
    {
        body.str(it.name);
        body.u32(it.compute);
        body.i32(idxOf(mods, it.desc.vert)); body.i32(idxOf(mods, it.desc.frag)); body.i32(idxOf(mods, it.desc.task));
        body.i32(idxOf(mods, it.desc.mesh)); body.i32(idxOf(mods, it.comp));
        body.i32(idxOf(pls, it.desc.layout));
        body.u32(it.compFlags);
        writeDesc(body, it.desc);
        body.u32((uint32_t)it.specEntries.size());
        for (const auto& e : it.specEntries) { body.u32(e.constantID); body.u32(e.offset); body.u32((uint32_t)e.size); }
        body.u32((uint32_t)it.specData.size());
        body.bytes(it.specData.data(), it.specData.size());
    }
    Writer w;
    if (!writeHeader(w, phys)) return std::string();   // (no device created by this process)
    w.u32((uint32_t)mods.size());
    for (VkShaderModule m : mods)
    {
        const uint32_t* code = nullptr;
        uint32_t words = 0;
        std::string name;
        if (!vkPipelineModuleCode(m, code, words, name)) return std::string();   // (an unnoted module)
        w.u32(words);
        w.bytes(code, (size_t)words * 4);
        w.str(name);
    }
    std::vector<std::string> plBlobs;
    {
        std::lock_guard<std::mutex> lk(layoutMutex());
        for (VkPipelineLayout pl : pls)
        {
            auto it = pipeLayouts().find(pl);
            if (it == pipeLayouts().end()) return std::string();
            Writer pw;
            pw.u32(it->second.flags);
            pw.u32((uint32_t)it->second.sets.size());
            for (VkDescriptorSetLayout sl : it->second.sets) pw.i32(idxOf(sls, sl));
            pw.u32((uint32_t)it->second.push.size());
            for (const auto& r : it->second.push) { pw.u32(r.stageFlags); pw.u32(r.offset); pw.u32(r.size); }
            plBlobs.push_back(pw.s);
        }
        w.u32((uint32_t)sls.size());
        for (VkDescriptorSetLayout sl : sls)
        {
            auto it = setLayouts().find(sl);
            if (it == setLayouts().end() || it->second.immutable) return std::string();
            w.u32(it->second.flags);
            w.u32(it->second.hasBindingFlags ? 1u : 0u);
            w.u32((uint32_t)it->second.bindings.size());
            for (size_t k = 0; k < it->second.bindings.size(); ++k)
            {
                const VkDescriptorSetLayoutBinding& b = it->second.bindings[k];
                w.u32(b.binding); w.u32(b.descriptorType); w.u32(b.descriptorCount); w.u32(b.stageFlags);
                w.u32(it->second.bindingFlags[k]);
            }
        }
    }
    w.u32((uint32_t)plBlobs.size());
    for (const std::string& b : plBlobs) w.s += b;
    w.s += body.s;
    return w.s;
}

// ======================================================================= worker
namespace
{
#if LL_WINDOWS
constexpr DWORD kWorkerLockOffset = 4096;   // the locked byte (the pid text lives before it)
#endif
std::atomic<VkPipelineRespinFn> s_respin{nullptr};
std::atomic<bool> s_finishing{false};
std::atomic<int64_t> s_finishSince{0};
VkDeviceContext* s_workerDev = nullptr;
std::string s_workerCache;
int64_t nowMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
std::mutex& workerOutMutex();
void workerSay(const char* fmt, unsigned a, unsigned b)
{
    std::lock_guard<std::mutex> lk(workerOutMutex());   // (compile threads and the batch hook both print)
    std::printf(fmt, a, b);
    std::fflush(stdout);
}
void workerProgress()
{
    static int64_t last = 0;
    unsigned done = 0, queued = 0;
    double idle = 0;
    vkPipelineCompileProgress(done, queued, idle);
    workerSay("done %u %u\n", done, queued);
    const int64_t t = nowMs();
    // (a heavy completion forces this hook too; a save rewrites the whole file -- the viewer's seed
    // plus these compiles, up to vk_pipecache.h kMaxBytes -- so at most every 30 s; "worker done" saves the rest)
    if (s_workerDev && t - last >= 30000)
    {
        last = t;
        s_workerDev->savePipelineCache("worker progress");
        size_t n = 0;
        vkPipelineCacheGetDataGated(s_workerDev->device(), s_workerDev->pipelineCache(), &n, nullptr, "worker progress");
        workerSay("saved %u KB %u\n", (unsigned)(n >> 10), 0u);
    }
}
// A line for the viewer's log: stdout "log <text>" (the host relays it: on Windows the worker's
// stderr reaches nothing) and stderr. Bounded well under the host's 4 KB line buffer.
std::mutex& workerOutMutex() { static std::mutex m; return m; }
void workerLog(const char* fmt, ...)
{
    char buf[1536];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    for (char* c = buf; *c; ++c) if (*c == '\n' || *c == '\r') *c = ' ';
    std::lock_guard<std::mutex> lk(workerOutMutex());
    std::printf("log %s\n", buf);
    std::fflush(stdout);
    std::fprintf(stderr, "[pipeworker] %s\n", buf);
}
void workerEvent(bool warn, const char* line)
{
    std::fprintf(stderr, "[pipeworker]%s %s\n", warn ? " WARN" : "", line);
    if (warn) workerLog("WARN %s", line);   // (out-of-memory requeues, give-ups: the viewer's log too)
}
// #1411: per recipe index, the create-info hash + creation feedback + time ("ph" lines), and the
// batch's feedback totals for the summary.
std::atomic<unsigned> s_workerHits{0}, s_workerMisses{0}, s_workerNoFb{0};
void workerBatch(const VkPipelineBatchReport& r)
{
    {
        std::lock_guard<std::mutex> lk(workerOutMutex());
        for (size_t i = 0; i < r.items.size(); ++i)
        {
            const VkPipelineCompileStat& s = r.items[i];
            if (!s.ciHash) continue;
            std::printf("ph %zu %016llx %d %.0f\n", i, (unsigned long long)s.ciHash, s.appCacheHit, s.ms);
        }
        std::fflush(stdout);
    }
    s_workerHits += r.appHits;
    s_workerMisses += r.appMisses;
    s_workerNoFb += r.noFeedback;
    workerLog("batch: %zu pipelines (%zu compiled + %zu dedup twins), sum of compiles %.0f ms, wall %.0f ms; driver cache"
              " feedback: %u hits, %u misses, %u without", r.total, r.compiled, r.fromCache, r.sumMs, r.wallMs, r.appHits,
              r.appMisses, r.noFeedback);
}
// The driver's serialized size of a cache (what it kept of the data it was created from).
size_t cacheBytes(VkDevice d, VkPipelineCache c, const char* what)
{
    size_t n = 0;
    if (!c || vkPipelineCacheGetDataGated(d, c, &n, nullptr, what) != VK_SUCCESS) return 0;
    return n;
}
uint64_t workerRss()
{
#if LL_WINDOWS
    // Working set may plateau because Windows is paging, while private commit keeps
    // growing. Match the viewer's probe: observe the allocation, not residency.
    PROCESS_MEMORY_COUNTERS_EX pmem{};
    pmem.cb = sizeof(pmem);
    if (GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS*)&pmem, sizeof(pmem)))
        return (uint64_t)pmem.PrivateUsage;
    return 0;
#else
    return LLMemory::getCurrentRSS();
#endif
}
// The viewer's live budget (pipelineMemBudget, llvulkancontext.cpp), here where the compiles run:
// the smaller of the available physical memory and the available commit, less the reserve (2 GB or
// a tenth of the RAM) and AMD's 2 GB driver margin. Without it the worker admitted against no
// budget at all (only its in-flight cap, a quarter of the RAM), whatever the viewer and the rest of
// the machine held. AMD's Windows driver does not fail a compile that runs out of commit: it
// aborts the process inside amdvlk64.dll (0xC0000409), with no log line and no minidump.
std::atomic<uint64_t> s_workerDriverMargin{0};
uint64_t workerBudget()
{
    static std::mutex sMutex;
    static std::chrono::steady_clock::time_point sAt;
    static uint64_t sBudget = 0;
    static bool sHave = false;
    std::lock_guard<std::mutex> lk(sMutex);
    const auto now = std::chrono::steady_clock::now();
    if (sHave && now - sAt < std::chrono::milliseconds(50)) return sBudget;   // (every waiting thread asks)
    LLMemoryGovernor::SystemMemory sm;
    if (!LLMemoryGovernor::readSystemMemory(sm) || !sm.avail)
        sBudget = UINT64_MAX;   // can't tell: the in-flight cap and the thread count limit it
    else
    {
        uint64_t avail = sm.avail;
        if (sm.have_commit) avail = std::min<uint64_t>(avail, sm.commit_avail);
        const uint64_t reserve = std::max<uint64_t>(2048ull << 20, sm.total / 10)
                               + s_workerDriverMargin.load(std::memory_order_relaxed);
        sBudget = avail > reserve ? avail - reserve : 0;   // (0: one at a time, the progress guarantee)
        if (vkPipelineRdnaGovernor())
            vkSetPipelinePressureCap(sBudget == 0 ? 1u : (sBudget < reserve / 2 ? 2u : 0u), "worker live headroom");
    }
    sAt = now;
    sHave = true;
    return sBudget;
}
}

static int workerRun(int argc, char** argv);

int vkPipelineWorkerMain(int argc, char** argv)
{
    // The stdin reader thread sits in fgets holding stdin's lock: a normal exit() would block
    // forever in the stdio cleanup (measured: the worker hung after its last pipeline). Every
    // result is on disk / in stdout by now: flush and leave without the exit-time cleanup.
    const int rc = workerRun(argc, argv);
    std::fflush(stdout);
    std::fflush(stderr);
    std::_Exit(rc);
}

static int workerRun(int argc, char** argv)
{
    std::string recipePath, cacheFile, lockPath, costsPath, seedFile;
    bool forceRdna = false;
    unsigned threads = 2, memMB = 0, finishMax = 600, memCapMB = 0;
    bool rayTracing = true, respin = false;
    for (int i = 1; i < argc; ++i)
    {
        const std::string a = argv[i];
        auto next = [&]() -> const char* { return i + 1 < argc ? argv[++i] : ""; };
        if (a == "--recipe") recipePath = next();
        else if (a == "--cache") cacheFile = next();
        else if (a == "--lock") lockPath = next();
        else if (a == "--threads") threads = (unsigned)std::max(1, std::atoi(next()));
        else if (a == "--mem-mb") memMB = (unsigned)std::max(0, std::atoi(next()));
        else if (a == "--finish-max-sec") finishMax = (unsigned)std::max(0, std::atoi(next()));
        else if (a == "--ray-tracing") rayTracing = std::atoi(next()) != 0;
        else if (a == "--respin") respin = true;
        else if (a == "--mem-cap-mb") memCapMB = (unsigned)std::max(0, std::atoi(next()));
        else if (a == "--costs") costsPath = next();
        else if (a == "--rdna-governor") forceRdna = true;
        else if (a == "--seed") seedFile = next();
    }
    if (recipePath.empty() || cacheFile.empty()) { std::fprintf(stderr, "[pipeworker] usage: worker --recipe F --cache F\n"); return 2; }
#if !LL_WINDOWS
    // One worker per cache: the lock is held for the process lifetime (released by the kernel on exit).
    if (!lockPath.empty())
    {
        const int fd = ::open(lockPath.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
        if (fd < 0 || ::flock(fd, LOCK_EX | LOCK_NB) != 0) { std::fprintf(stderr, "[pipeworker] another worker holds %s\n", lockPath.c_str()); return 3; }
        char pid[32];
        const int n = std::snprintf(pid, sizeof(pid), "%d\n", (int)getpid());
        if (::ftruncate(fd, 0) == 0 && ::write(fd, pid, (size_t)n) != n) { /* best effort */ }
    }
    ::signal(SIGPIPE, SIG_IGN);              // a finishing worker outlives its reader: progress writes just fail
    // (No PR_SET_PDEATHSIG: it is a per-thread setting that a finishing worker could not drop for
    // the main thread from its stdin reader -- measured: the deliberate finish was killed with the
    // viewer. The stdin pipe's EOF is the parent-death signal instead, on every platform.)
    if (memMB)
    {
        // RLIMIT_DATA, never RLIMIT_AS: drivers reserve huge address ranges they never touch.
        rlimit rl{(rlim_t)memMB << 20, (rlim_t)memMB << 20};
        ::setrlimit(RLIMIT_DATA, &rl);
    }
    ::setpriority(PRIO_PROCESS, 0, 10);      // below the viewer (its render / asset threads win the cores)
#else
    // One worker per cache (#1411, as flock above): an exclusive byte-range lock past the pid text,
    // held for the process lifetime (the handle is never closed; Windows releases the lock when the
    // process ends, however it ends). The pid stays readable: a new viewer stops a stale worker by it.
    if (!lockPath.empty())
    {
        const HANDLE h = CreateFileW(ll_convert_string_to_wide(lockPath).c_str(), GENERIC_READ | GENERIC_WRITE,
                                     FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_ALWAYS,
                                     FILE_ATTRIBUTE_NORMAL, nullptr);
        OVERLAPPED ov{};
        ov.Offset = kWorkerLockOffset;
        if (h == INVALID_HANDLE_VALUE
            || !LockFileEx(h, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, 1, 0, &ov))
        {
            workerLog("another worker holds %s; exiting", lockPath.c_str());
            return 3;
        }
        char pid[32];
        const int n = std::snprintf(pid, sizeof(pid), "%-30lu\n", (unsigned long)GetCurrentProcessId());
        DWORD written = 0;
        SetFilePointer(h, 0, nullptr, FILE_BEGIN);
        WriteFile(h, pid, (DWORD)n, &written, nullptr);   // (best effort)
    }
    SetPriorityClass(GetCurrentProcess(), BELOW_NORMAL_PRIORITY_CLASS);
#endif
    // Commands on stdin. EOF without "finish" = the viewer is gone: stop at once (no cleanup:
    // the cache file on disk is always a complete, atomically written one).
    std::thread([finishMax]() {
        char line[64];
        while (std::fgets(line, sizeof(line), stdin))
        {
            if (std::strncmp(line, "finish", 6) == 0)
            {
                s_finishSince = nowMs();
                s_finishing = true;
                std::fputs("finishing\n", stdout);   // (the host waits for this before it lets the viewer exit)
                std::fflush(stdout);
            }
            else if (std::strncmp(line, "quit", 4) == 0) break;
        }
        std::fprintf(stderr, "[pipeworker] viewer gone (stdin closed)%s\n", s_finishing.load() ? "; finishing the recipe" : "; exiting");
        if (!s_finishing.load()) std::_Exit(0);
        // Finishing: bounded.
        while (nowMs() - s_finishSince.load() < (int64_t)finishMax * 1000) std::this_thread::sleep_for(std::chrono::milliseconds(500));
        std::fprintf(stderr, "[pipeworker] finish time limit reached; exiting\n");
        std::_Exit(0);
    }).detach();

    std::string recipe;
    if (LLFILE* f = LLFile::fopen(recipePath, "rb"))
    {
        char buf[1 << 16];
        size_t n;
        while ((n = fread(buf, 1, sizeof(buf), f)) > 0 && recipe.size() < (512u << 20)) recipe.append(buf, n);
        LLFile::close(f);
    }
    Reader r(recipe);
    char magic[4];
    r.take(magic, 4);
    const uint32_t ver = r.u32();
    const uint32_t vendor = r.u32(), device = r.u32(), driver = r.u32();
    uint8_t uuid[VK_UUID_SIZE];
    r.take(uuid, VK_UUID_SIZE);
    if (r.bad || std::memcmp(magic, kRecipeMagic, 4) != 0 || ver != kRecipeVersion)
    {
        workerLog("bad recipe %s (version %u, this build reads %u)", recipePath.c_str(), ver, kRecipeVersion);
        return 2;
    }
    // #1411: THE VIEWER'S DEVICE. A driver may key its cache entries on how the device was created
    // (AMD 26.10.1: a worker on a default headless device built 53 pipelines the viewer then missed,
    // 37 of 37): the same request -- features, extensions, swapchain, queues -- and then the check
    // that it produced the same device.
    const std::string request = r.str();
    const uint32_t fpLo = r.u32(), fpHi = r.u32();
    const uint64_t viewerFp = (uint64_t)fpLo | ((uint64_t)fpHi << 32);
    const std::string viewerFpText = r.str();
    if (r.bad) { workerLog("malformed recipe header %s", recipePath.c_str()); return 2; }
    VkDeviceContext dev;
    bool wantSwapchain = false;
    std::vector<std::string> instExtNames;
    std::string why;
    if (!dev.applyCreateRequest(request, wantSwapchain, instExtNames, why))
    {
        workerLog("recipe without a device request (%s)", why.c_str());
        return 2;
    }
    (void)rayTracing;   // (the request carries the viewer's ray-tracing wish)
    dev.setPipelineCacheFile(cacheFile, "worker");
    std::vector<const char*> instExts;
    for (const std::string& e : instExtNames) instExts.push_back(e.c_str());
    if (!dev.initForPresent(false, instExts, wantSwapchain))
    {
        workerLog("device init failed: %s", dev.requirementsReport().failure.c_str());
        return 2;
    }
    {
        VkPhysicalDeviceProperties p{};
        vkGetPhysicalDeviceProperties(dev.physicalDevice(), &p);
        if (p.vendorID != vendor || p.deviceID != device || p.driverVersion != driver
            || std::memcmp(p.pipelineCacheUUID, uuid, VK_UUID_SIZE) != 0)
        {
            workerLog("recipe is for another GPU / driver");
            dev.shutdown();
            return 2;
        }
    }
    if (dev.deviceFingerprint() != viewerFp)
    {
        // Not the viewer's device: whatever it compiled would be keyed differently. Say exactly how.
        const std::vector<std::string> onlyViewer = linesNotIn(viewerFpText, dev.deviceFingerprintText());
        const std::vector<std::string> onlyWorker = linesNotIn(dev.deviceFingerprintText(), viewerFpText);
        workerLog("device DIFFERS from the viewer's (fingerprint %016llx, viewer %016llx): %zu item(s) only in the"
                  " viewer's, %zu only in the worker's; not compiling (its results would not be reused)",
                  (unsigned long long)dev.deviceFingerprint(), (unsigned long long)viewerFp, onlyViewer.size(),
                  onlyWorker.size());
        for (size_t i = 0; i < onlyViewer.size() && i < 24; ++i) workerLog("  viewer only: %s", onlyViewer[i].c_str());
        for (size_t i = 0; i < onlyWorker.size() && i < 24; ++i) workerLog("  worker only: %s", onlyWorker[i].c_str());
        dev.shutdown();
        return 4;
    }
    workerLog("device matches the viewer's (fingerprint %016llx; swapchain %s, %zu instance extension(s))",
              (unsigned long long)viewerFp, wantSwapchain ? "yes" : "no", instExtNames.size());
    const VkDevice d = dev.device();
    // The worker's own cache (its file, loaded by the device) and the SEED: the viewer's main cache
    // file, so a pipeline the viewer (or an earlier worker, merged) already has is a hit here too.
    {
        const size_t own = cacheBytes(d, dev.pipelineCache(), "worker own cache");
        size_t seedBytes = 0, seedKept = 0;
        bool merged = false;
        std::vector<uint8_t> blob;
        if (!seedFile.empty() && dev.pipelineCache() && vkpipecache::load(dev.physicalDevice(), seedFile, "seed", blob, true)
            && !blob.empty())
        {
            seedBytes = blob.size();
            VkPipelineCacheCreateInfo ci{VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
            ci.initialDataSize = blob.size();
            ci.pInitialData = blob.data();
            VkPipelineCache sc = VK_NULL_HANDLE;
            if (vkPipelineCacheCreateGated(d, &ci, &sc, "worker seed") == VK_SUCCESS)
            {
                seedKept = cacheBytes(d, sc, "worker seed");
                // (no compile has started: the destination is not in use)
                merged = vkPipelineCacheMergeGated(d, dev.pipelineCache(), 1, &sc, "worker seed") == VK_SUCCESS;
                vkPipelineCacheDestroyGated(d, sc, "worker seed");
            }
        }
        workerLog("cache: own (%s) driver holds %zu KB; seed %s: %zu KB, driver kept %zu KB%s; now %zu KB",
                  dev.pipelineCacheWarm() ? "from its file" : "no file", own >> 10, seedFile.empty() ? "(none)" : seedFile.c_str(),
                  seedBytes >> 10, seedKept >> 10, merged ? ", merged" : (seedBytes ? ", NOT merged" : ""),
                  cacheBytes(d, dev.pipelineCache(), "worker own cache") >> 10);
    }
    // Modules (the code stays in `codes`, alive for the whole run: vkNoteShaderModule's rule).
    std::vector<std::vector<uint32_t>> codes;
    std::vector<VkShaderModule> mods;
    const uint32_t nm = r.count(8);
    unsigned respun = 0, respinChanged = 0, respinUnknown = 0;
    for (uint32_t i = 0; i < nm && !r.bad; ++i)
    {
        const uint32_t words = r.count(4);
        codes.emplace_back(words);
        r.take(codes.back().data(), (size_t)words * 4);
        const std::string name = r.str();
        if (respin)
        {
            // (a recipe of an earlier build: its heavy modules become this build's; one this build has
            // no module of keeps the recorded SPIR-V -- stale if that shader changed since)
            std::vector<uint32_t> fresh;
            VkPipelineRespinFn fn = s_respin.load();
            if (fn && fn(name, fresh) && !fresh.empty())
            {
                ++respun;
                if (fresh != codes.back()) ++respinChanged;
                codes.back().swap(fresh);
            }
            else ++respinUnknown;
        }
        VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        ci.codeSize = codes.back().size() * 4;
        ci.pCode = codes.back().data();
        VkShaderModule m = VK_NULL_HANDLE;
        if (r.bad || codes.back().empty() || vkCreateShaderModule(d, &ci, nullptr, &m) != VK_SUCCESS) m = VK_NULL_HANDLE;
        if (m) vkNoteShaderModule(m, ci.pCode, ci.codeSize, name.c_str());
        mods.push_back(m);
    }
    if (respin)
        workerLog("respin: %u of %u module(s) replaced by this build's SPIR-V (%u of them changed), %u kept as recorded",
                  respun, nm, respinChanged, respinUnknown);
    std::vector<VkDescriptorSetLayout> sls;
    const uint32_t ns = r.count(8);
    for (uint32_t i = 0; i < ns && !r.bad; ++i)
    {
        VkDescriptorSetLayoutCreateInfo ci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        ci.flags = r.u32();
        const bool hasBindingFlags = r.u32() != 0;   // (chained exactly when the viewer chained it)
        const uint32_t nb = r.count(20);
        std::vector<VkDescriptorSetLayoutBinding> bs(nb);
        std::vector<VkDescriptorBindingFlags> fl(nb);
        for (uint32_t k = 0; k < nb; ++k)
        {
            bs[k].binding = r.u32(); bs[k].descriptorType = (VkDescriptorType)r.u32(); bs[k].descriptorCount = r.u32();
            bs[k].stageFlags = r.u32(); fl[k] = r.u32();
        }
        VkDescriptorSetLayoutBindingFlagsCreateInfo bf{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO};
        bf.bindingCount = nb;
        bf.pBindingFlags = fl.data();
        ci.pNext = hasBindingFlags ? &bf : nullptr;
        ci.bindingCount = nb;
        ci.pBindings = bs.data();
        VkDescriptorSetLayout l = VK_NULL_HANDLE;
        if (r.bad || vkCreateDescriptorSetLayout(d, &ci, nullptr, &l) != VK_SUCCESS) l = VK_NULL_HANDLE;
        if (l) vkPipelineNoteSetLayout(l, &ci);   // (#1411: the create-info hash reads layout content)
        sls.push_back(l);
    }
    std::vector<VkPipelineLayout> pls;
    const uint32_t np = r.count(8);
    for (uint32_t i = 0; i < np && !r.bad; ++i)
    {
        const uint32_t plFlags = r.u32();
        const uint32_t nsets = r.count(4);
        std::vector<VkDescriptorSetLayout> sets(nsets);
        for (auto& s : sets) { const int32_t k = r.i32(); s = (k >= 0 && (size_t)k < sls.size()) ? sls[k] : VK_NULL_HANDLE; }
        const uint32_t npush = r.count(12);
        std::vector<VkPushConstantRange> push(npush);
        for (auto& p : push) { p.stageFlags = r.u32(); p.offset = r.u32(); p.size = r.u32(); }
        VkPipelineLayoutCreateInfo ci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        ci.flags = plFlags;
        ci.setLayoutCount = nsets; ci.pSetLayouts = sets.data();
        ci.pushConstantRangeCount = npush; ci.pPushConstantRanges = push.data();
        VkPipelineLayout l = VK_NULL_HANDLE;
        if (r.bad || vkCreatePipelineLayout(d, &ci, nullptr, &l) != VK_SUCCESS) l = VK_NULL_HANDLE;
        if (l) vkPipelineNotePipelineLayout(l, &ci);
        pls.push_back(l);
    }
    struct Item { GraphicsPipelineDesc desc; bool compute; VkShaderModule comp; uint32_t compFlags;
                  std::vector<VkSpecializationMapEntry> e; std::vector<uint8_t> data; VkSpecializationInfo si{}; std::string name;
                  VkPipeline out = VK_NULL_HANDLE; };
    std::vector<Item> items;
    const uint32_t npl = r.count(16);
    items.resize(npl);
    auto mod = [&](int32_t k) { return (k >= 0 && (size_t)k < mods.size()) ? mods[k] : VK_NULL_HANDLE; };
    for (Item& it : items)
    {
        if (r.bad) break;
        it.name = r.str();
        it.compute = r.u32() != 0;
        it.desc.vert = mod(r.i32()); it.desc.frag = mod(r.i32()); it.desc.task = mod(r.i32()); it.desc.mesh = mod(r.i32());
        it.comp = mod(r.i32());
        const int32_t li = r.i32();
        it.compFlags = r.u32();
        readDesc(r, it.desc);
        it.desc.layout = (li >= 0 && (size_t)li < pls.size()) ? pls[li] : VK_NULL_HANDLE;
        const uint32_t ne = r.count(12);
        it.e.resize(ne);
        for (auto& e : it.e) { e.constantID = r.u32(); e.offset = r.u32(); e.size = r.u32(); }
        const uint32_t nd = r.count(1);
        it.data.resize(nd);
        r.take(it.data.data(), nd);
        for (const auto& e : it.e) if ((size_t)e.offset + e.size > it.data.size()) r.bad = true;   // (malformed)
    }
    if (r.bad) { workerLog("malformed recipe"); dev.shutdown(); return 2; }

    s_workerDev = &dev;
    // The viewer's admission policy, here where the compiles now run (a child without it would
    // just move the #1411 paging into another process): the learned costs (read-only: the viewer
    // owns that file), AMD's 3 GB default per unknown pipeline and its heavy-lane start of 1-2,
    // the process's resident set as the memory probe, the live system headroom as the budget
    // (workerBudget), and an in-flight cap from the host.
    {
        VkPhysicalDeviceDriverProperties drv{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
        VkPhysicalDeviceProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
        p2.pNext = &drv;
        vkGetPhysicalDeviceProperties2(dev.physicalDevice(), &p2);
        const bool amd = drv.driverID == VK_DRIVER_ID_AMD_PROPRIETARY;
        const char* rdnaRule = vkcompilegovernor::rule(p2.properties.vendorID, p2.properties.deviceID,
                                                     vkcompilegovernor::kWindows, drv.driverInfo, forceRdna);
        vkSetPipelineRdnaGovernor(rdnaRule != nullptr);
        if (rdnaRule) workerLog("RDNA1/2 compile governor: heavy pipelines 8 GB each, 1 at a time until measured; rule: %s; Windows heavy calls serialized across viewer/worker processes", rdnaRule);
        const size_t learned = costsPath.empty() ? 0 : vkpipecache::loadCosts(dev.physicalDevice(), costsPath, true);
        vkSetPipelineDefaultCost(amd ? (3072ull << 20) : (512ull << 20));
        const unsigned cap = amd && !rdnaRule ? (learned ? 2u : 1u) : 0u;
        vkSetPipelineConcurrencyStart(cap);
        vkSetPipelineColdStartCap(cap);
        vkSetPipelineMemProbe(&workerRss);
        s_workerDriverMargin.store(amd ? (2048ull << 20) : 0, std::memory_order_relaxed);
        vkSetPipelineMemBudget(&workerBudget);
        if (memCapMB) vkSetPipelineMemCap((uint64_t)memCapMB << 20);
    }
    vkSetPipelineCompileThreads(threads);
    vkSetPipelineEventHook(&workerEvent);
    vkSetPipelineProgressHook(&workerProgress, 1);
    vkSetPipelineBatchHook(&workerBatch);   // #1411 "ph" lines: the viewer checks its creates against them
    vkSetPipelineCacheWarm(dev.pipelineCacheWarm());
    workerSay("start %u %u\n", (unsigned)items.size(), threads);
    {
        PipelineBatchScope scope(d);
        for (Item& it : items)
        {
            if (!it.e.empty()) it.si = VkSpecializationInfo{(uint32_t)it.e.size(), it.e.data(), it.data.size(), it.data.data()};
            const VkSpecializationInfo* spec = it.e.empty() ? nullptr : &it.si;
            if (it.compute) createComputePipelineInto(d, it.comp, it.desc.layout, it.compFlags, spec, &it.out, it.name.c_str());
            else
            {
                it.desc.fragSpec = spec;
                createGraphicsPipelineInto(d, it.desc, &it.out, it.name.c_str());
            }
        }
    }   // (the scope's flush compiles them, workers below normal priority -- the process is)
    unsigned made = 0;
    for (Item& it : items)
        if (it.out) { ++made; vkForgetPipeline(it.out); vkDestroyPipeline(d, it.out, nullptr); }
    vkSetPipelineProgressHook(nullptr, 0);
    vkSetPipelineBatchHook(nullptr);
    const bool savedOk = dev.savePipelineCache("worker done");
    {
        size_t n = 0;
        vkPipelineCacheGetDataGated(d, dev.pipelineCache(), &n, nullptr, "worker done");
        workerSay("saved %u KB %u\n", (unsigned)(n >> 10), 1u);
        workerLog("done: %u of %zu pipelines made; driver cache feedback %u hits, %u misses, %u without; cache %zu KB %s %s",
                  made, items.size(), s_workerHits.load(), s_workerMisses.load(), s_workerNoFb.load(), n >> 10,
                  savedOk ? "written to" : "NOT written to", cacheFile.c_str());
    }
    workerSay("all %u %u\n", made, (unsigned)items.size());
    s_workerDev = nullptr;
    for (VkPipelineLayout l : pls) if (l) vkDestroyPipelineLayout(d, l, nullptr);
    for (VkDescriptorSetLayout l : sls) if (l) vkDestroyDescriptorSetLayout(d, l, nullptr);
    for (VkShaderModule m : mods) if (m) vkDestroyShaderModule(d, m, nullptr);
    dev.shutdown();
    return made == items.size() ? 0 : 1;
}

// ======================================================================= host
std::string vkPipelineWorkerExePath()
{
#if LL_WINDOWS
    wchar_t buf[MAX_PATH * 2];
    const DWORD n = GetModuleFileNameW(nullptr, buf, (DWORD)(sizeof(buf) / sizeof(buf[0])));
    if (!n) return std::string();
    return ll_convert_wide_to_string(std::wstring(buf, n));
#elif LL_LINUX
    char buf[4096];
    const ssize_t n = ::readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    return n > 0 ? std::string(buf, (size_t)n) : std::string();
#else
    return std::string();
#endif
}

namespace
{
bool writeFileAtomic(const std::string& path, const std::string& data)
{
    const std::string tmp = path + ".tmp";
    LLFILE* f = LLFile::fopen(tmp, "wb");
    if (!f) return false;
    bool ok = fwrite(data.data(), 1, data.size(), f) == data.size();
    ok = (LLFile::close(f) == 0) && ok;
    ok = ok && replaceFile(tmp, path);   // (#1411: one atomic step on Windows too)
    if (!ok) LLFile::remove(tmp, ENOENT);
    return ok;
}

#if !LL_WINDOWS
// A worker of an older session still finishing on the same cache: stop it (only a process whose
// command line says it is one of our workers), so two never write one file.
void stopStaleWorker(const std::string& lockPath)
{
    const int fd = ::open(lockPath.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
    if (fd < 0) return;
    if (::flock(fd, LOCK_EX | LOCK_NB) == 0) { ::flock(fd, LOCK_UN); ::close(fd); return; }   // nobody holds it
    char buf[32] = {};
    const ssize_t n = ::pread(fd, buf, sizeof(buf) - 1, 0);
    const int pid = n > 0 ? std::atoi(buf) : 0;
#if LL_LINUX
    bool isWorker = false;
    if (pid > 1)
    {
        char path[64], cmd[512] = {};
        std::snprintf(path, sizeof(path), "/proc/%d/cmdline", pid);
        if (FILE* c = std::fopen(path, "rb"))
        {
            const size_t k = std::fread(cmd, 1, sizeof(cmd) - 1, c);
            std::fclose(c);
            for (size_t i = 0; i < k; ++i) if (!cmd[i]) cmd[i] = ' ';
            isWorker = std::strstr(cmd, " worker --recipe ") != nullptr;
        }
    }
    if (isWorker) ::kill(pid, SIGTERM);
#endif
    for (int i = 0; i < 40; ++i)   // up to 2 s for it to go (the kernel releases its lock)
    {
        if (::flock(fd, LOCK_EX | LOCK_NB) == 0) { ::flock(fd, LOCK_UN); break; }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    ::close(fd);
}
#else
// #1411 Windows: the same guard. The lock holder is alive (the OS drops a dead process's lock), so the
// pid it wrote is current; it is stopped only when its image is this executable (a worker is this
// binary in worker mode). Its cache file is replaced atomically (MoveFileExW), so stopping it at any
// moment leaves the previous complete file.
void stopStaleWorker(const std::string& lockPath, const std::string& exe)
{
    const HANDLE h = CreateFileW(ll_convert_string_to_wide(lockPath).c_str(), GENERIC_READ | GENERIC_WRITE,
                                 FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_ALWAYS,
                                 FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    auto lockFree = [&]() {
        OVERLAPPED ov{};
        ov.Offset = kWorkerLockOffset;
        if (!LockFileEx(h, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, 1, 0, &ov)) return false;
        UnlockFileEx(h, 0, 1, 0, &ov);
        return true;
    };
    if (lockFree()) { CloseHandle(h); return; }
    char buf[32] = {};
    DWORD n = 0;
    SetFilePointer(h, 0, nullptr, FILE_BEGIN);
    const DWORD pid = ReadFile(h, buf, sizeof(buf) - 1, &n, nullptr) ? (DWORD)std::strtoul(buf, nullptr, 10) : 0;
    bool stopped = false;
    if (pid && pid != GetCurrentProcessId())
    {
        if (HANDLE p = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid))
        {
            wchar_t img[MAX_PATH * 2];
            DWORD len = (DWORD)(sizeof(img) / sizeof(img[0]));
            const bool ours = QueryFullProcessImageNameW(p, 0, img, &len)
                              && _wcsicmp(img, ll_convert_string_to_wide(exe).c_str()) == 0;
            if (ours && TerminateProcess(p, 1))
            {
                WaitForSingleObject(p, 2000);
                stopped = true;
            }
            CloseHandle(p);
        }
    }
    for (int i = 0; i < 40 && !lockFree(); ++i) Sleep(50);   // up to 2 s for the lock to go
    LL_INFOS("VKPipe") << "#1411: a compile worker of an earlier session (pid " << pid << ") held the cache lock; "
                       << (stopped ? "stopped" : "not stopped (not this executable, or gone)") << LL_ENDL;
    CloseHandle(h);
}
#endif
}

bool VkPipelineWorkerHost::start(const std::string& exe, const std::string& recipe, const std::string& cacheFile,
                                 unsigned threads, unsigned memMB, bool rayTracing, bool respin)
{
    if (mPid) { mWhy = "already running"; return false; }
    if (exe.empty() || recipe.empty() || cacheFile.empty()) { mWhy = "no executable / recipe / cache path"; return false; }
    mCache = cacheFile;
    mRecipe = recipe;
    mHashes.clear();
    mRecipePath = cacheFile + ".recipe";
    if (!writeFileAtomic(mRecipePath, recipe)) { mWhy = "cannot write " + mRecipePath; return false; }
    const std::string lockPath = cacheFile + ".lock";
    const std::string th = std::to_string(std::max(1u, threads)), mem = std::to_string(memMB), rt = rayTracing ? "1" : "0";
    // In-flight compile cap: half the process limit (the viewer's own rule is a quarter of RAM; the
    // limit given is half of RAM). The learned costs: the viewer's file next to the cache, read-only.
    const std::string capS = std::to_string(memMB / 2);
    const size_t slash = cacheFile.find_last_of("/\\");
    const std::string costs = (slash == std::string::npos ? std::string() : cacheFile.substr(0, slash + 1)) + "vk_pipeline_costs.txt";
    // #1411 SEED: the viewer's main cache (the worker file's name less ".worker"), read-only.
    static const std::string kWorkerSuffix = ".worker";
    const std::string seed = cacheFile.size() > kWorkerSuffix.size()
                                     && cacheFile.compare(cacheFile.size() - kWorkerSuffix.size(), kWorkerSuffix.size(), kWorkerSuffix) == 0
                                 ? cacheFile.substr(0, cacheFile.size() - kWorkerSuffix.size())
                                 : std::string();
#if LL_WINDOWS
    stopStaleWorker(lockPath, exe);   // #1411 (POSIX: below)
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE inR = nullptr, inW = nullptr, outR = nullptr, outW = nullptr;
    // (stdout: a large buffer -- the early worker writes before the renderer adopts and reads it;
    // with the default few KB its progress / log lines would block it)
    if (!CreatePipe(&inR, &inW, &sa, 0) || !CreatePipe(&outR, &outW, &sa, 256u << 10)) { mWhy = "CreatePipe failed"; return false; }
    SetHandleInformation(inW, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(outR, HANDLE_FLAG_INHERIT, 0);
    std::wstring cmd = L"\"" + ll_convert_string_to_wide(exe) + L"\" worker --recipe \"" + ll_convert_string_to_wide(mRecipePath)
                     + L"\" --cache \"" + ll_convert_string_to_wide(cacheFile) + L"\" --lock \"" + ll_convert_string_to_wide(lockPath)
                     + L"\" --threads " + ll_convert_string_to_wide(th)
                     + L" --mem-mb " + ll_convert_string_to_wide(mem) + L" --ray-tracing " + ll_convert_string_to_wide(rt)
                     + (respin ? L" --respin" : L"")
                     + L" --mem-cap-mb " + ll_convert_string_to_wide(capS) + L" --costs \"" + ll_convert_string_to_wide(costs) + L"\"";
    if (vkPipelineRdnaGovernor()) cmd += L" --rdna-governor";
    if (!seed.empty()) cmd += L" --seed \"" + ll_convert_string_to_wide(seed) + L"\"";
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = inR;
    si.hStdOutput = outW;
    si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    PROCESS_INFORMATION pi{};
    std::vector<wchar_t> cl(cmd.begin(), cmd.end());
    cl.push_back(0);
    const BOOL ok = CreateProcessW(nullptr, cl.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW | BELOW_NORMAL_PRIORITY_CLASS | CREATE_SUSPENDED,
                                   nullptr, nullptr, &si, &pi);
    CloseHandle(inR);
    CloseHandle(outW);
    if (!ok) { CloseHandle(inW); CloseHandle(outR); mWhy = "CreateProcess failed"; return false; }
    // Memory bound (job object; NOT kill-on-close: a deliberate finish outlives the viewer -- the
    // stdin pipe's EOF ends it otherwise).
    if (memMB)
    {
        HANDLE job = CreateJobObjectW(nullptr, nullptr);
        if (job)
        {
            JOBOBJECT_EXTENDED_LIMIT_INFORMATION li{};
            li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_PROCESS_MEMORY;
            li.ProcessMemoryLimit = (SIZE_T)memMB << 20;
            SetInformationJobObject(job, JobObjectExtendedLimitInformation, &li, sizeof(li));
            AssignProcessToJobObject(job, pi.hProcess);
            mJob = (long long)(intptr_t)job;
        }
    }
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    mPid = (long long)(intptr_t)pi.hProcess;
    mInH = inW;
    mOutH = outR;
#else
    stopStaleWorker(lockPath);
    // Close-on-exec from creation: a process another thread spawns meanwhile must not inherit our
    // end of the worker's stdin (it would keep the pipe open past a viewer crash). adddup2 clears
    // the flag on the child's fds 0 / 1.
    int in[2], out[2];
#if LL_LINUX
    if (::pipe2(in, O_CLOEXEC) != 0) { mWhy = "pipe failed"; return false; }
    if (::pipe2(out, O_CLOEXEC) != 0) { ::close(in[0]); ::close(in[1]); mWhy = "pipe failed"; return false; }
#else
    if (::pipe(in) != 0) { mWhy = "pipe failed"; return false; }
    if (::pipe(out) != 0) { ::close(in[0]); ::close(in[1]); mWhy = "pipe failed"; return false; }
#endif
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, in[0], 0);
    posix_spawn_file_actions_adddup2(&fa, out[1], 1);
    posix_spawn_file_actions_addclose(&fa, in[1]);
    posix_spawn_file_actions_addclose(&fa, out[0]);
    std::vector<std::string> args = {exe, "worker", "--recipe", mRecipePath, "--cache", cacheFile, "--lock", lockPath,
                                     "--threads", th, "--mem-mb", mem, "--ray-tracing", rt, "--mem-cap-mb", capS, "--costs", costs};
    if (respin) args.push_back("--respin");
    if (vkPipelineRdnaGovernor()) args.push_back("--rdna-governor");
    if (!seed.empty()) { args.push_back("--seed"); args.push_back(seed); }
    std::vector<char*> av;
    for (std::string& a : args) av.push_back(&a[0]);
    av.push_back(nullptr);
    pid_t pid = 0;
    const int rc = posix_spawn(&pid, exe.c_str(), &fa, nullptr, av.data(), environ);
    posix_spawn_file_actions_destroy(&fa);
    ::close(in[0]);
    ::close(out[1]);
    if (rc != 0) { ::close(in[1]); ::close(out[0]); mWhy = std::string("posix_spawn: ") + std::strerror(rc); return false; }
    ::fcntl(in[1], F_SETFD, FD_CLOEXEC);
    ::fcntl(out[0], F_SETFD, FD_CLOEXEC);
    ::fcntl(out[0], F_SETFL, ::fcntl(out[0], F_GETFL) | O_NONBLOCK);
    ::signal(SIGPIPE, SIG_IGN);   // (a write to a dead worker's stdin must not end the viewer)
    mPid = pid;
    mIn = in[1];
    mOut = out[0];
#endif
    mExited = false;
    mExitCode = -1;
    mDone = 0;
    return true;
}

void VkPipelineWorkerHost::readOutput()
{
    char buf[4096];
    for (;;)
    {
#if LL_WINDOWS
        DWORD avail = 0, n = 0;
        if (!mOutH || !PeekNamedPipe((HANDLE)mOutH, nullptr, 0, nullptr, &avail, nullptr) || !avail) return;
        if (!ReadFile((HANDLE)mOutH, buf, std::min<DWORD>(avail, sizeof(buf)), &n, nullptr) || !n) return;
#else
        if (mOut < 0) return;
        const ssize_t n = ::read(mOut, buf, sizeof(buf));
        if (n <= 0) return;
#endif
        mLine.append(buf, (size_t)n);
        size_t eol;
        while ((eol = mLine.find('\n')) != std::string::npos)
        {
            unsigned a = 0, b = 0;
            unsigned long long hash = 0;
            if (std::sscanf(mLine.c_str(), "done %u %u", &a, &b) == 2) mDone = a;
            else if (mLine.compare(0, 9, "finishing") == 0) mFinishAck = true;
            else if (mLine.compare(0, 4, "log ") == 0)   // #1411: the worker's own lines, into the viewer's log
                LL_INFOS("VKPipe") << "[compile worker] " << mLine.substr(4, eol - 4) << LL_ENDL;
            else if (std::sscanf(mLine.c_str(), "ph %u %llx", &a, &hash) == 2 && a < (1u << 16))
            {
                if (mHashes.size() <= a) mHashes.resize(a + 1, 0);
                mHashes[a] = hash;
            }
            mLine.erase(0, eol + 1);
        }
        if (mLine.size() > 4096) mLine.clear();   // (bounded)
    }
}

bool VkPipelineWorkerHost::poll()
{
    if (!mPid) return mExited;
    readOutput();
    if (mExited) return true;
#if LL_WINDOWS
    DWORD code = STILL_ACTIVE;
    if (GetExitCodeProcess((HANDLE)(intptr_t)mPid, &code) && code != STILL_ACTIVE)
    {
        mExited = true;
        mExitCode = (int)code;
    }
#else
    int st = 0;
    if (::waitpid((pid_t)mPid, &st, WNOHANG) == (pid_t)mPid)
    {
        mExited = true;
        mExitCode = WIFEXITED(st) ? WEXITSTATUS(st) : 128 + (WIFSIGNALED(st) ? WTERMSIG(st) : 0);
    }
#endif
    if (mExited) readOutput();
    return mExited;
}

void VkPipelineWorkerHost::stop(bool finish)
{
    if (!mPid) return;
    poll();
    if (!mExited)
    {
#if LL_WINDOWS
        if (finish && mInH) { DWORD w = 0; WriteFile((HANDLE)mInH, "finish\n", 7, &w, nullptr); }
        if (mInH) { CloseHandle((HANDLE)mInH); mInH = nullptr; }   // (EOF: a non-finishing worker exits)
        if (!finish)
        {
            if (WaitForSingleObject((HANDLE)(intptr_t)mPid, 2000) == WAIT_TIMEOUT) TerminateProcess((HANDLE)(intptr_t)mPid, 1);
        }
#else
        if (finish && mIn >= 0 && ::write(mIn, "finish\n", 7) != 7) finish = false;
        // A finishing worker must have dropped its parent-death signal before we may exit: wait
        // (bounded) for its acknowledgement.
        for (int i = 0; finish && !mFinishAck && i < 100 && !mExited; ++i)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            poll();
        }
        if (mIn >= 0) { ::close(mIn); mIn = -1; }   // (EOF: a non-finishing worker exits)
        if (!finish)
        {
            for (int i = 0; i < 40 && !poll(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(50));
            if (!mExited) { ::kill((pid_t)mPid, SIGKILL); ::waitpid((pid_t)mPid, nullptr, 0); }
        }
#endif
    }
#if LL_WINDOWS
    if (mInH) CloseHandle((HANDLE)mInH);
    if (mOutH) CloseHandle((HANDLE)mOutH);
    CloseHandle((HANDLE)(intptr_t)mPid);
    if (mJob) CloseHandle((HANDLE)(intptr_t)mJob);
    mInH = mOutH = nullptr;
    mJob = 0;
#else
    if (mIn >= 0) ::close(mIn);
    if (mOut >= 0) ::close(mOut);
    mIn = mOut = -1;
#endif
    mPid = 0;
}

void VkPipelineWorkerHost::killHard()
{
    if (!mPid) return;
#if LL_WINDOWS
    TerminateProcess((HANDLE)(intptr_t)mPid, 9);
    WaitForSingleObject((HANDLE)(intptr_t)mPid, 5000);
#else
    ::kill((pid_t)mPid, SIGKILL);
    int st = 0;
    ::waitpid((pid_t)mPid, &st, 0);
#endif
    mExited = true;
    mExitCode = 137;
    stop(false);
}

VkPipelineWorkerHost::~VkPipelineWorkerHost() { stop(false); }

VkPipelineCache vkPipelineWorkerLoadCache(VkDevice dev, VkPhysicalDevice phys, const std::string& file)
{
    std::vector<uint8_t> blob;
    if (!vkpipecache::load(phys, file, "worker", blob) || blob.empty()) return VK_NULL_HANDLE;
    VkPipelineCacheCreateInfo ci{VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
    ci.initialDataSize = blob.size();
    ci.pInitialData = blob.data();
    VkPipelineCache c = VK_NULL_HANDLE;
    if (vkPipelineCacheCreateGated(dev, &ci, &c, "worker cache load") != VK_SUCCESS) c = VK_NULL_HANDLE;
    // #1411: a driver that does not accept the data still returns VK_SUCCESS with an empty cache; what
    // it serializes back says what it kept (a header-sized result = nothing).
    size_t kept = 0;
    if (c) vkPipelineCacheGetDataGated(dev, c, &kept, nullptr, "worker cache load");
    LL_INFOS("VKPipe") << "#1411: worker cache " << file << ": " << (blob.size() >> 10) << " KB in the file, the driver kept "
                       << (kept >> 10) << " KB" << (c && kept < blob.size() / 2 ? " (most of it REJECTED)" : "") << LL_ENDL;
    return c;
}

bool vkPipelineWorkerSaveCache(VkDevice dev, VkPhysicalDevice phys, VkPipelineCache cache, const std::string& file,
                               const char* why)
{
    size_t last = 0;
    return cache && vkpipecache::save(dev, phys, cache, file, "worker", last, why);
}

void vkPipelineWorkerDiscard(const std::string& file) { LLFile::remove(file, ENOENT); }

const char* VkPipelineWorkerHost::exitText(int code)
{
    switch (code)
    {
    case 0: return "every pipeline compiled";
    case 1: return "some pipelines failed";
    case 2: return "bad arguments / recipe / device";
    case 3: return "another worker holds the cache lock";
    case 4: return "its device differs from the viewer's (nothing compiled: it would not be reused)";
    case 137: return "killed";
    default: return "crashed or stopped";
    }
}

bool vkPipelineWorkerMerge(VkDevice dev, VkPhysicalDevice phys, VkPipelineCache cache, const std::string& file, bool consume)
{
    if (!cache) return false;
    VkPipelineCache wc = vkPipelineWorkerLoadCache(dev, phys, file);
    if (!wc) return false;
    const bool ok = vkPipelineCacheMergeGated(dev, cache, 1, &wc, "worker cache merge") == VK_SUCCESS;
    vkPipelineCacheDestroyGated(dev, wc, "worker cache merge");
    if (ok && consume) LLFile::remove(file, ENOENT);
    return ok;
}

// ======================================================================= P5
void vkPipelineWorkerSetRespin(VkPipelineRespinFn fn) { s_respin.store(fn); }

namespace
{
std::mutex s_earlyMutex;
VkPipelineWorkerHost* s_early = nullptr;
}

bool vkPipelineWorkerSaveRecipe(const std::string& cacheFile, const std::string& recipe)
{
    if (cacheFile.empty() || recipe.empty()) return true;
    const std::string path = cacheFile + ".recipe";
    if (LLFILE* f = LLFile::fopen(path, "rb"))
    {
        std::string old;
        char buf[1 << 16];
        size_t n;
        while ((n = fread(buf, 1, sizeof(buf), f)) > 0 && old.size() <= recipe.size()) old.append(buf, n);
        LLFile::close(f);
        if (old == recipe) return false;   // unchanged: no write
    }
    writeFileAtomic(path, recipe);
    return true;
}

void vkPipelineEarlyWorkerStart(const std::string& cacheFile, bool rayTracing)
{
    if (cacheFile.empty() || !LLFile::isfile(cacheFile + ".recipe")) return;
    std::string recipe;
    if (LLFILE* f = LLFile::fopen(cacheFile + ".recipe", "rb"))
    {
        char buf[1 << 16];
        size_t n;
        while ((n = fread(buf, 1, sizeof(buf), f)) > 0 && recipe.size() < (512u << 20)) recipe.append(buf, n);
        LLFile::close(f);
    }
    // #1411: a recipe of another format (an earlier release's) would only cost a process that exits
    // at once: the in-session worker writes this build's recipe.
    {
        uint32_t ver = 0;
        if (recipe.size() >= 8) std::memcpy(&ver, recipe.data() + 4, 4);
        if (recipe.size() < 8 || std::memcmp(recipe.data(), kRecipeMagic, 4) != 0 || ver != kRecipeVersion)
        {
            LL_INFOS("VKPipe") << "#1411: no early compile worker (the saved recipe is format " << ver << ", this build writes "
                               << kRecipeVersion << ")" << LL_ENDL;
            return;
        }
    }
    std::lock_guard<std::mutex> lk(s_earlyMutex);
    if (s_early || recipe.empty()) return;
    const unsigned hw = std::max(1u, std::thread::hardware_concurrency());
    // The same memory bound as the in-session worker (LLCleanRendererVK::startUpgradeWorker): half the
    // RAM as the process limit, a quarter as the compiles' in-flight cap (start() passes memMB / 2). With
    // 0 the early worker had neither, and no live budget either, so its heavy lane ramped to the thread
    // count beside the viewer's own login-screen compiles (AMD's compiler: ~3-4 GB per heavy pipeline).
    unsigned memMB = 0;
    {
        LLMemoryGovernor::SystemMemory sm;
        if (LLMemoryGovernor::readSystemMemory(sm) && sm.total) memMB = (unsigned)(sm.total / 2 >> 20);
    }
    if (!memMB) return;   // (RAM unknown: no early worker; the renderer starts the in-session one as before)
    s_early = new VkPipelineWorkerHost();
    // (the host re-writes the recipe file it was read from: same bytes)
    if (!s_early->start(vkPipelineWorkerExePath(), recipe, cacheFile + ".worker", std::max(1u, hw / 4), memMB, rayTracing,
                        /*respin=*/true))
    {
        LL_WARNS("VKPipe") << "#1411: early compile worker failed to start (" << s_early->why() << ")" << LL_ENDL;
        delete s_early;
        s_early = nullptr;
        return;
    }
    LL_INFOS("VKPipe") << "#1411: early compile worker started from the saved recipe (" << (recipe.size() >> 10)
                       << " KB, seeded from " << cacheFile << ")" << LL_ENDL;
}

VkPipelineWorkerHost* vkPipelineEarlyWorkerTake()
{
    std::lock_guard<std::mutex> lk(s_earlyMutex);
    VkPipelineWorkerHost* h = s_early;
    s_early = nullptr;
    return h;
}

// #1547 GPU-loss recovery (vk_device.cpp vkEpochBegin): the layout recipes are keyed by raw handles of
// the retired device; a new device may hand out the same values. Emptied before it exists.
void vkPipeworkerEpochReset()
{
    std::lock_guard<std::mutex> lk(layoutMutex());
    setLayouts().clear();
    pipeLayouts().clear();
}
