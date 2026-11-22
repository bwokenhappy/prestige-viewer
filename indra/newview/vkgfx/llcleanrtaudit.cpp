/**
 * @file llcleanrtaudit.cpp
 * @brief #874 RT SCENE AUDIT: on DEVICE_LOST (and on demand, RenderVKRTAuditNow), check every
 *        TLAS instance reference of the last frames, the CPU tables they came from and the
 *        fault addresses against the GPU address registry (vk_device.cpp).
 *
 * WHY: a GPU page fault inside a ray query reports one address and nothing else. The likeliest
 * cause of such a fault (#874: an RX 9070 XT read 5.6 GB above every live allocation in the first
 * sun-shadow trace) is a TLAS record whose BLAS reference is garbage, unbuilt or long freed; the
 * traversal then reads BVH nodes from wherever that points. After the loss device-local memory
 * cannot be read, so the evidence has to be on the host already:
 *  - GPU side: LLCleanRTVK::recordBuild copies the records pass A / pass B wrote
 *    (VkAccelerationStructureInstanceKHR, 64 B each) and the rt_geom rows the consumers
 *    dereference (16 B each) into a HOST_VISIBLE | HOST_COHERENT (HOST_CACHED preferred) ring,
 *    one buffer per frame slot, right after the TLAS build; the GPU then writes the frame's
 *    token into word 0 (a fill after a copy -> clear barrier), so a slot whose copy never ran
 *    reads as "not executed". The memory stays mapped after a loss and is plain host memory:
 *    the same as the AMD breadcrumb buffer (vk_device.cpp sCrumbBuf), whose post-loss reads
 *    already appear in #874's report. The audit reads it only; no Vulkan call is made.
 *  - CPU side: the slot-address table (mSlotAddrCpu, the source of pass A's slot_addr), the
 *    per-slot / terrain-morph / rigged-group / OMM BLAS the CPU believes live, the rig entries
 *    uploaded with each frame (still in that frame slot's staging buffer) and the dummy BLAS.
 *  - The registry: vkAddrClassify / vkAddrNeighbourAS (live and recently freed AS notes and
 *    buffers, with their created / freed registry frames).
 * Each audited frame records the registry frame it was recorded in, so a reference into an AS
 * freed BEFORE that (a stale CPU table) is told apart from one freed AFTER it (a retire-ring
 * race: the GPU still ran the frame).
 *
 * COST in normal frames: one vkCmdCopyBuffer of instance_count x 80 B (device -> host memory),
 * a 4-byte fill and two barriers, after the TLAS build; three host buffers of that size. No CPU
 * work per instance; no host read. RenderVKRTLostAudit 0 turns the copy off.
 */

#include "linden_common.h"

#include "llcleanrtvk.h"
#include "llcleanrenderervk.h"
#include "../llcleanqvertex.h"   // LLCleanQV::kStride (static VBO vertex size)
#include "../llvkhitch.h"        // #874 read guard counter
#include "llcleanclusterrtvk.h"  // #874 read guard: the cluster buffers (fault reach)

#include "llerror.h"

#if defined(LL_HAS_CLEAN_PIPELINE)
#  include "llviewercontrol.h"
#  include "llcleaninstancestore.h"   // keyForId: the offending records' object + face
extern U32 gFrameCount;
#endif

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <thread>
#include <vector>

namespace
{
LLCleanRTVK* sAuditRT = nullptr;   // the RT foundation the hooks audit (one per process)

constexpr uint32_t kRecBytes = 64;   // VkAccelerationStructureInstanceKHR
constexpr uint32_t kGeomBytes = 16;  // rt_geom row (uvec4)
constexpr uint32_t kRigGroupBit = 0x800000u;   // clean_rt_instances.comp RT_RIG_GROUP
constexpr uint32_t kRigEntryWords = 8;         // RigEntryCPU / clean_rt_instances.comp RigEntry
constexpr size_t   kInstBytes = 224, kInstGroup = 176, kInstFlags = 184, kInstSkin = 188;
constexpr uint32_t kMaxBad = 20;               // offending records printed (all slots together)
constexpr uint32_t kMaxBadTables = 12;         // offending CPU-table rows printed

uint64_t threadKey()
{
    return (uint64_t)std::hash<std::thread::id>()(std::this_thread::get_id()) | 1ull;
}

inline uint32_t rd32(const uint8_t* p) { uint32_t v; std::memcpy(&v, p, 4); return v; }
inline float rdf(const uint8_t* p) { float v; std::memcpy(&v, p, 4); return v; }

// Output: the DEVICE_LOST block (vkFaultLog: stderr + the log + the report file) or, on
// demand, the log.
struct AuditOut
{
    bool lost = true;
    void operator()(const char* fmt, ...) const
    {
        char buf[1024];
        va_list ap;
        va_start(ap, fmt);
        std::vsnprintf(buf, sizeof(buf), fmt, ap);
        va_end(ap);
        if (lost) vkFaultLogLine(buf);
        else LL_INFOS("VKRT") << buf << LL_ENDL;
    }
};

// What a record's reference is.
enum RefCat : uint8_t
{
    RC_OK = 0,          // the start of a live BLAS
    RC_DUMMY,           // the refit placeholder BLAS (mask 0 by design)
    RC_INTERIOR,        // inside a live AS, not its start
    RC_WRONG_KIND,      // the start of a live TLAS
    RC_FREED_BEFORE,    // a freed AS, freed before the frame was recorded (stale table)
    RC_FREED_AFTER,     // a freed AS, freed after the frame was recorded (retire-ring race)
    RC_ARENA,           // AS storage no live AS owns
    RC_OTHER_BUF,       // another live buffer
    RC_FREED_BUF,       // a recently freed buffer
    RC_UNKNOWN,         // nothing the registry knows
    RC_CLUSTER,         // a cluster row inside cluster AS storage (CLAS / cluster BLAS: GPU-placed, never noted)
    RC_BUSY,            // the registry was busy (not classified)
    RC_COUNT
};
const char* const kCatName[RC_COUNT] = {"ok", "placeholder", "inside-live-AS", "TLAS-as-BLAS", "freed-before-build",
                                        "freed-after-build", "AS-arena-no-AS", "other-buffer", "freed-buffer",
                                        "unknown", "cluster-AS-storage (unverifiable)", "registry-busy"};
static_assert(RC_COUNT == 12, "kCatName");

bool isArenaName(const char* n)
{
    return std::strcmp(n, "rt.asChunk") == 0 || std::strcmp(n, "rt.asDedicated") == 0;
}
// The cluster RT's AS storage (llcleanclusterrtvk.cpp): CLAS / cluster BLAS placed by the GPU's
// cluster build (their addresses are written by the device, never vkAddrNote'd).
bool isClusterAsName(const char* n)
{
    return std::strcmp(n, "rt.cl.clasChunk") == 0 || std::strcmp(n, "rt.cl.blasArena") == 0
        || std::strcmp(n, "rt.cl.sbHeap") == 0;
}

RefCat categorize(const VkAddrHit& h, VkDeviceAddress ref, VkDeviceAddress dummy, uint32_t built_reg,
                  bool cluster_row = false)
{
    if (ref == dummy && dummy && h.cls == VKADDR_AS_LIVE && h.exact) return RC_DUMMY;
    switch (h.cls)
    {
    case VKADDR_AS_LIVE:
        if (!h.exact) return RC_INTERIOR;
        return std::strcmp(h.name, "tlas") == 0 ? RC_WRONG_KIND : RC_OK;
    case VKADDR_AS_FREED:  return h.freedFrame <= built_reg ? RC_FREED_BEFORE : RC_FREED_AFTER;
    case VKADDR_BUF_LIVE:
        if (cluster_row && isClusterAsName(h.name)) return RC_CLUSTER;
        return isArenaName(h.name) ? RC_ARENA : RC_OTHER_BUF;
    case VKADDR_BUF_FREED: return RC_FREED_BUF;
    default:               return RC_UNKNOWN;
    }
}

std::string describeHit(const VkAddrHit& h)
{
    char b[200];
    switch (h.cls)
    {
    case VKADDR_AS_LIVE:
    case VKADDR_AS_FREED:
        std::snprintf(b, sizeof(b), "%s AS '%s' id=%u [0x%llx, +%llu)%s created reg frame %u%s",
                      h.cls == VKADDR_AS_LIVE ? "live" : "previously freed", h.name, h.id, (unsigned long long)h.lo,
                      (unsigned long long)h.size, h.exact ? "" : " (interior)", h.frame,
                      h.freedFrame ? (" freed reg frame " + std::to_string(h.freedFrame)).c_str() : "");
        break;
    case VKADDR_BUF_LIVE:
    case VKADDR_BUF_FREED:
        std::snprintf(b, sizeof(b), "%s buffer '%s' usage=0x%x [0x%llx, +%llu) created reg frame %u%s",
                      h.cls == VKADDR_BUF_LIVE ? "live" : "previously freed", h.name[0] ? h.name : "-", h.id,
                      (unsigned long long)h.lo, (unsigned long long)h.size, h.frame,
                      h.freedFrame ? (" freed reg frame " + std::to_string(h.freedFrame)).c_str() : "");
        break;
    default:
        std::snprintf(b, sizeof(b), "not in the registry");
    }
    std::string o = b;
    if (h.prevKind)   // #973: the freed range this live object reuses (driver VA reuse)
        o += std::string(" (reuses a previously freed ") + (h.prevKind == 1 ? "AS" : "buffer") + " range '" + h.prevName
           + "' freed reg frame " + std::to_string(h.prevFreedFrame) + ")";
    return o;
}

// Batch classification with a sorted unique address list (records share BLAS).
struct Classifier
{
    std::vector<VkDeviceAddress> addrs;
    std::vector<VkAddrHit> hits;
    bool ok = false;
    void add(VkDeviceAddress a) { if (a) addrs.push_back(a); }
    void run()
    {
        std::sort(addrs.begin(), addrs.end());
        addrs.erase(std::unique(addrs.begin(), addrs.end()), addrs.end());
        hits.assign(addrs.size(), VkAddrHit{});
        ok = vkAddrClassify(addrs.data(), addrs.size(), hits.data());
    }
    const VkAddrHit* find(VkDeviceAddress a) const
    {
        auto it = std::lower_bound(addrs.begin(), addrs.end(), a);
        return ok && it != addrs.end() && *it == a ? &hits[it - addrs.begin()] : nullptr;
    }
};
} // namespace

// ===========================================================================
// hooks
// ===========================================================================
void LLCleanRTVK::auditRegister(bool on)
{
    if (on)
    {
        sAuditRT = this;
        gVkRtLostAuditHook = &LLCleanRTVK::auditLostHook;
        gVkRtLostAuditMainLoop = &LLCleanRTVK::auditLostMainLoop;
    }
    else if (sAuditRT == this)
    {
        sAuditRT = nullptr;
        gVkRtLostAuditHook = nullptr;
        gVkRtLostAuditMainLoop = nullptr;
    }
}

void LLCleanRTVK::auditInvalidate()
{
    for (AuditMeta& m : mAuditMeta) m = AuditMeta{};
}

void LLCleanRTVK::auditLostHook(const VkDeviceAddress* faults, uint32_t n)
{
    LLCleanRTVK* rt = sAuditRT;
    if (!rt) { vkFaultLogLine("[vktest] RT scene audit: not available (no RT foundation)"); return; }
    // The CPU tables are mutated by the thread that records the RT build: read them only there
    // (or later from the main loop, auditLostMainLoop). The GPU readback ring is safe anywhere.
    rt->inputLostReport(); // atomic counters: safe even when the recorder is another thread
    const bool same_thread = rt->mAuditThread == 0 || rt->mAuditThread == threadKey();
    rt->runAudit(faults, n, true, -1, same_thread, true);
    {
        // #973 rig copy input check (clean_rt_instances.comp rigCopy): rigged faces collapsed for
        // non-finite / out-of-reach skinned positions (the last readback; plain counters).
        char line[240];
        std::snprintf(line, sizeof(line), "[vktest] RT rig copy input check: last readback %u non-finite, %u out-of-reach "
                      "(> 1 km) rigged faces collapsed; %u in %u frames this session, %u groups marked for rebuild",
                      rt->mRigBadNonFinite, rt->mRigBadFar, rt->mRigBadTotal, rt->mRigBadFrames, rt->mRigBadRebuilds);
        vkFaultLogLine(line);
    }
    if (same_thread) rt->arenaGuardLostReport();   // #973 (its tables belong to the RT thread too)
    if (!same_thread) rt->mAuditCpuDeferred = true;
}

void LLCleanRTVK::auditLostMainLoop()
{
    LLCleanRTVK* rt = sAuditRT;
    if (!rt || !rt->mAuditCpuDeferred) return;
    rt->mAuditCpuDeferred = false;
    if (rt->mAuditThread != 0 && rt->mAuditThread != threadKey())
    {
        vkFaultLogLine("[vktest] RT scene audit (main loop): the RT build records on another thread -- CPU tables skipped");
        return;
    }
    VkDeviceAddress faults[16];
    const uint32_t n = vkLastFaultAddresses(faults, 16);
    vkFaultLogLine("[vktest] --- RT scene audit, CPU tables (main loop; the loss was raised on another thread) ---");
    rt->runAudit(faults, n, true, -1, true, false);
    rt->arenaGuardLostReport();   // #973
}

// ===========================================================================
// per frame
// ===========================================================================
void LLCleanRTVK::auditBeforeFrame()
{
    mAuditThread = threadKey();
#if defined(LL_HAS_CLEAN_PIPELINE)
    static LLCachedControl<bool> s_now(gSavedSettings, "RenderVKRTAuditNow", false);
    if (s_now)
    {
        gSavedSettings.setBOOL("RenderVKRTAuditNow", FALSE);   // one shot
        mAuditNowPending = true;
        LL_INFOS("VKRT") << "RT scene audit (RenderVKRTAuditNow): runs when the next recorded frame slot comes round"
                         << LL_ENDL;
    }
#endif
    if (!mAuditNowPending) return;
#if defined(LL_HAS_CLEAN_PIPELINE)
    static LLCachedControl<bool> s_on(gSavedSettings, "RenderVKRTLostAudit", true);
    if (!s_on)
    {
        // No readback to wait for: the CPU tables only (the verdict says why).
        mAuditNowPending = false;
        runAudit(nullptr, 0, false, -1, true, true);
        return;
    }
#endif
    // This frame slot's fence was waited (recordBuild runs after it): its readback is complete,
    // and nothing it referenced is freed until beginFrame's destroyRetired, which runs next.
    const uint32_t s = mR.mFrameSlot % 3u;
    if (!mAuditMeta[s].valid) return;   // nothing recorded in this slot yet: try the next one
    mAuditNowPending = false;
    runAudit(nullptr, 0, false, (int)s, true, true);
}

void LLCleanRTVK::auditRecordCopy(VkCommandBuffer cmd, uint32_t inst_n, uint32_t slot_n, bool tlas_update, bool ptlas,
                                  VkDeviceSize rig_off, bool rig_staged)
{
    const uint32_t s = mSlotIdx;
    mAuditMeta[s].valid = false;
    bool on = true;
#if defined(LL_HAS_CLEAN_PIPELINE)
    static LLCachedControl<bool> s_on(gSavedSettings, "RenderVKRTLostAudit", true);
    on = s_on;
#endif
    if (!on || !inst_n || !mTlasInst.buffer || !mRtGeom.buffer) return;
    const VkDeviceSize rec_bytes = (VkDeviceSize)inst_n * kRecBytes, geom_bytes = (VkDeviceSize)inst_n * kGeomBytes;
    if (!ensureBuf(mAuditRb[s], kAuditHdr + rec_bytes + geom_bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true)
        || !mAuditRb[s].mapped)
        return;
    // Stamp cleared by the host (the slot's previous GPU use is done), set by the GPU after the copy.
    std::memset(mAuditRb[s].mapped, 0, (size_t)kAuditHdr);
    auto barrier = [cmd](VkPipelineStageFlags2 ss, VkAccessFlags2 sa, VkPipelineStageFlags2 ds, VkAccessFlags2 da)
    {
        VkMemoryBarrier2 b{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
        b.srcStageMask = ss; b.srcAccessMask = sa; b.dstStageMask = ds; b.dstAccessMask = da;
        VkDependencyInfo d{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        d.memoryBarrierCount = 1; d.pMemoryBarriers = &b;
        vkCmdPipelineBarrier2(cmd, &d);
    };
    // pass A / pass B (compute) wrote the records and rt_geom -> the copy reads them.
    barrier(VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
            VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
    const VkBufferCopy c0{0, kAuditHdr, rec_bytes};
    vkCmdCopyBuffer(cmd, mTlasInst.buffer, mAuditRb[s].buffer, 1, &c0);
    const VkBufferCopy c1{0, kAuditHdr + rec_bytes, geom_bytes};
    vkCmdCopyBuffer(cmd, mRtGeom.buffer, mAuditRb[s].buffer, 1, &c1);
    // The stamp only after both copies finished (the fill must not overtake them).
    barrier(VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
            VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    const uint32_t token = mFrame ? mFrame : 1u;
    vkCmdFillBuffer(cmd, mAuditRb[s].buffer, 0, 4, token);
    barrier(VK_PIPELINE_STAGE_2_COPY_BIT | VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
            VK_PIPELINE_STAGE_2_HOST_BIT, VK_ACCESS_2_HOST_READ_BIT);

    AuditMeta& m = mAuditMeta[s];
    m.token = token;
    m.regFrame = vkAddrCurrentFrame();
    m.rtFrame = mFrame;
#if defined(LL_HAS_CLEAN_PIPELINE)
    m.gFrame = (uint32_t)gFrameCount;
#endif
    m.instN = inst_n;
    m.tlasCap = mTlasCap;
    m.rigN = (uint32_t)mRigEntries.size();
    m.rigOff = rig_off;
    m.rigStaged = rig_staged && m.rigN != 0;
    m.slotN = slot_n;
    m.idxValid = mR.mIdxValid;
    m.rigPosVerts = (uint32_t)std::min<VkDeviceSize>(mRigPos.size / 16u, 0xFFFFFFFFu);   // kRigPosStride
    m.vboVerts = (uint32_t)std::min<VkDeviceSize>(mR.mVbo.size / LLCleanQV::kStride, 0xFFFFFFFFu);
    m.tlasUpdate = tlas_update;
    m.ptlas = ptlas;
    m.tlasBuildFrame = mTlasBuildFrame;   // (this frame's when it built: the TLAS build is recorded before this)
    m.blasBuilt = mBuiltThisFrame;
    m.blasRefits = mRefits;
    m.rigBuilds = mRigBuilds;
    m.compactions = mCompactions;
    m.retiredAs = (uint32_t)mPend.as.size();
    m.retiredAllocs = (uint32_t)mPend.allocs.size();
    m.dummy = mDummyAddr;
    m.tlasInstAddr = mTlasInst.address;
    m.rtGeomAddr = mRtGeom.address;
    m.valid = true;
}

// ===========================================================================
// the audit
// ===========================================================================
void LLCleanRTVK::runAudit(const VkDeviceAddress* faults, uint32_t nfaults, bool lost, int only_slot, bool cpu_tables,
                           bool gpu_part)
{
    AuditOut out;
    out.lost = lost;
    const uint32_t reg_now = vkAddrCurrentFrame();

    // ---- which slots: valid meta + the GPU stamp, newest first ----
    struct Slot { uint32_t s; AuditMeta m; const uint8_t* base; bool stamped; };
    std::vector<Slot> slots;
    for (uint32_t s = 0; s < 3; ++s)
    {
        if (only_slot >= 0 && (int)s != only_slot) continue;
        const AuditMeta m = mAuditMeta[s];   // (by value: a worker-thread loss may race the recorder)
        const GpuBuffer rb = mAuditRb[s];
        if (!m.valid || !rb.mapped || rb.size < kAuditHdr + (VkDeviceSize)m.instN * (kRecBytes + kGeomBytes)) continue;
        const uint8_t* base = static_cast<const uint8_t*>(rb.mapped);
        slots.push_back({s, m, base, rd32(base) == m.token});
    }
    std::sort(slots.begin(), slots.end(), [](const Slot& a, const Slot& b) { return a.m.rtFrame > b.m.rtFrame; });
    // The newest slot the GPU stamped: the frame in flight at the loss (or the one just audited).
    uint32_t newest_exec = 0;
    for (const Slot& sl : slots) if (sl.stamped) { newest_exec = sl.m.rtFrame; break; }

    bool on = true;
#if defined(LL_HAS_CLEAN_PIPELINE)
    static LLCachedControl<bool> s_on(gSavedSettings, "RenderVKRTLostAudit", true);
    on = s_on;
#endif
    if (gpu_part)
        out("[vktest] RT scene audit (#874): registry frame %u, %zu readback slot(s)%s; CPU tables %s",
            reg_now, slots.size(), on ? "" : " (RenderVKRTLostAudit is off: no readback)",
            cpu_tables ? "checked" : "skipped (raised on another thread than the RT build: from the main loop)");

    // ---- classify every active reference of every stamped slot (one registry lock) ----
    Classifier cls;
    if (gpu_part)
        for (const Slot& sl : slots)
        {
            if (!sl.stamped) continue;
            const uint8_t* rec = sl.base + kAuditHdr;
            for (uint32_t i = 0; i < sl.m.instN; ++i)
            {
                const uint8_t* r = rec + (size_t)i * kRecBytes;
                cls.add((VkDeviceAddress)rd32(r + 56) | ((VkDeviceAddress)rd32(r + 60) << 32));
            }
        }
    // CPU tables' addresses (same lock).
    std::vector<VkDeviceAddress> tblSlot, tblOmm, tblRig, tblEntry;
    if (cpu_tables)
    {
        for (size_t sl = 0; sl * 4 + 3 < mSlotAddrCpu.size(); ++sl)
        {
            const VkDeviceAddress a = (VkDeviceAddress)mSlotAddrCpu[sl * 4] | ((VkDeviceAddress)mSlotAddrCpu[sl * 4 + 1] << 32);
            tblSlot.push_back(a);
            cls.add(a);
        }
        for (size_t id = 0; id * 4 + 1 < mOmmAddrCpu.size(); ++id)
        {
            const VkDeviceAddress a = (VkDeviceAddress)mOmmAddrCpu[id * 4] | ((VkDeviceAddress)mOmmAddrCpu[id * 4 + 1] << 32);
            tblOmm.push_back(a);
            cls.add(a);
        }
        for (const auto& kv : mRig) { tblRig.push_back(kv.second.addr); cls.add(kv.second.addr); }
        for (const RigEntryCPU& e : mRigEntries)
        {
            const VkDeviceAddress a = (VkDeviceAddress)e.blas_lo | ((VkDeviceAddress)e.blas_hi << 32);
            tblEntry.push_back(a);
            cls.add(a);
        }
        cls.add(mDummyAddr);
    }
    cls.run();
    if (!cls.ok) out("[vktest]   (address registry busy: references not classified)");

    // Where an address sits in the CPU tables (offender lines only).
    auto cpuOwner = [&](VkDeviceAddress a) -> std::string
    {
        if (!cpu_tables || !a) return std::string();
        std::string o;
        for (size_t sl = 0; sl < tblSlot.size(); ++sl)
            if (tblSlot[sl] == a) { o += " slot-table[" + std::to_string(sl) + "]"; break; }
        for (size_t sl = 0; sl < mSlot.size(); ++sl)
            if (mSlot[sl].addr == a) { o += " slot-blas[" + std::to_string(sl) + "]"; break; }
        for (const auto& kv : mTerrMorph)
            if (kv.second.addr == a) { o += " terrain-morph[" + std::to_string(kv.first) + "]"; break; }
        for (size_t id = 0; id < tblOmm.size(); ++id)
            if (tblOmm[id] == a) { o += " omm[" + std::to_string(id) + "]"; break; }
        for (const auto& kv : mRig)
            if (kv.second.addr == a)
            {
                char b[96];
                std::snprintf(b, sizeof(b), " rig-group(owner %llu, class %u, %zu geoms, built rt frame %u)",
                              (unsigned long long)(kv.first >> 8), (unsigned)(kv.first & 0xFF),
                              kv.second.sig.size() / 5, kv.second.builtFrame);
                o += b;
                break;
            }
        if (a == mDummyAddr) o += " dummy";
        return o.empty() ? std::string(" (no CPU table holds it now)") : o;
    };
    auto instText = [&](uint32_t id) -> std::string
    {
        if (!cpu_tables) return std::string();
        std::string t;
        const size_t rows = mR.mCpuInstances.size() / kInstBytes;
        if (id < rows)
        {
            const uint8_t* in = mR.mCpuInstances.data() + (size_t)id * kInstBytes;
            char b[96];
            std::snprintf(b, sizeof(b), " row: flags 0x%x lod group %u skin %d", rd32(in + kInstFlags),
                          rd32(in + kInstGroup), (int)rd32(in + kInstSkin));
            t += b;
        }
#if defined(LL_HAS_CLEAN_PIPELINE)
        LLUUID obj;
        S32 face = -1;
        if (LLCleanInstanceStore::instance().keyForId(id, obj, face))
            t += " object " + obj.asString() + " face " + std::to_string(face);
        else
            t += " (no live object)";
#endif
        return t;
    };

    // ---- per slot ----
    uint32_t printed_bad = 0;
    uint32_t bad_total = 0, active_total = 0, audited = 0;
    struct RefSeen { VkDeviceAddress ref; uint32_t inst, rtFrame; };
    std::vector<RefSeen> active_refs;   // (fault proximity)
    for (const Slot& sl : (gpu_part ? slots : std::vector<Slot>()))
    {
        const AuditMeta& m = sl.m;
        const bool newest = m.rtFrame == newest_exec;
        // Could the GPU still have been running this frame? On demand it is the frame audited
        // before anything it referenced may be freed (beginFrame's destroyRetired runs after it),
        // so nothing it references may be freed. On a loss: only the newest executed frame, and
        // only when RT did not go idle since and it was recorded in the last few viewer frames.
        bool in_flight = !lost;
        if (lost && newest && !mIdleSinceBuild)
        {
            in_flight = true;
#if defined(LL_HAS_CLEAN_PIPELINE)
            in_flight = (uint32_t)gFrameCount - m.gFrame <= 3u;
#endif
        }
        out("[vktest]   slot %u: RT frame %u (registry frame %u, viewer frame %u) TLAS %s%s, %u records (cap %u), "
            "%u rig entries%s -- %s",
            sl.s, m.rtFrame, m.regFrame, m.gFrame, m.tlasUpdate ? "REFIT" : "BUILD", m.ptlas ? " (partitioned)" : "",
            m.instN, m.tlasCap, m.rigN, m.rigStaged ? "" : " (not staged)",
            sl.stamped ? (newest ? "executed (newest executed frame)" : "executed")
                       : "NOT executed (no GPU stamp: recorded, never ran or still in flight)");
        // (#1508) the AS work recorded ahead of this frame's TLAS, executed or not (a hang inside a traversal of a
        // TLAS / BLAS this frame refit, rebuilt, compacted or retired around shows here)
        out("[vktest]     AS work: last full TLAS build RT frame %u (%u frame(s) before); static BLAS built %u, "
            "compacted %u; rigged groups refit %u, rebuilt %u; retired so far: %u AS, %u arena block(s)",
            m.tlasBuildFrame, m.rtFrame - m.tlasBuildFrame, m.blasBuilt, m.compactions, m.blasRefits, m.rigBuilds,
            m.retiredAs, m.retiredAllocs);
        if (!sl.stamped) continue;
        ++audited;
        const uint8_t* rec = sl.base + kAuditHdr;
        const uint8_t* geo = rec + (size_t)m.instN * kRecBytes;
        // Rig entries of that frame (its staging copy), when still there.
        const uint8_t* rig = nullptr;
        {
            const GpuBuffer st = mStage[sl.s];
            if (m.rigStaged && st.mapped && st.size >= m.rigOff + (VkDeviceSize)m.rigN * kRigEntryWords * 4)
                rig = static_cast<const uint8_t*>(st.mapped) + m.rigOff;
        }
        auto rigW = [rig](uint32_t e, uint32_t w) { return rd32(rig + ((size_t)e * kRigEntryWords + w) * 4); };

        uint32_t cat[RC_COUNT] = {};
        uint32_t active = 0, inactive = 0, groups = 0, cluster_rows = 0, nonfinite = 0;
        uint32_t custom_ne = 0, geom_oob = 0, row_mismatch = 0, group_oob = 0, member_oob = 0, rigpos_oob = 0,
                 rig_idx_oob = 0, rigmap_ne = 0, head_ne = 0, gcount_unknown = 0, mask0_active = 0;
        for (uint32_t i = 0; i < m.instN; ++i)
        {
            const uint8_t* r = rec + (size_t)i * kRecBytes;
            const VkDeviceAddress ref = (VkDeviceAddress)rd32(r + 56) | ((VkDeviceAddress)rd32(r + 60) << 32);
            if (!ref) { ++inactive; continue; }
            ++active;
            const uint32_t cm = rd32(r + 48), sf = rd32(r + 52);
            const uint32_t custom = cm & 0xFFFFFFu, mask = cm >> 24, flags = sf >> 24;
            const bool group = (custom & kRigGroupBit) != 0u;
            const uint8_t* g = geo + (size_t)i * kGeomBytes;
            const uint32_t gx = rd32(g), gy = rd32(g + 4), gz = rd32(g + 8), gw = rd32(g + 12);
            active_refs.push_back({ref, i, m.rtFrame});
            const VkAddrHit* h = cls.find(ref);
            const bool cluster_row = (gz & 0x40000000u) != 0u;
            RefCat c = h ? categorize(*h, ref, m.dummy, m.regFrame, cluster_row) : RC_BUSY;
            // An AS freed after the frame was recorded is only suspicious while the frame may
            // still have been running (the retire ring frees it once that frame's fence was waited).
            const bool bad_ref = !(c == RC_OK || c == RC_DUMMY || c == RC_BUSY || c == RC_CLUSTER
                                   || (c == RC_FREED_AFTER && !in_flight));
            ++cat[c];
            if (!mask && c != RC_DUMMY) ++mask0_active;
            bool finite = true;
            for (int k = 0; k < 12 && finite; ++k) finite = std::isfinite(rdf(r + 4 * k));
            if (!finite) ++nonfinite;

            // ---- index chain (what the consumers dereference from this record) ----
            const char* chain = nullptr;
            if (!group)
            {
                if (custom != (i & 0xFFFFFFu)) { ++custom_ne; chain = "custom index != instance id"; }
                if ((gz & 0x40000000u) != 0u) ++cluster_rows;   // cluster rows: pool indices (unchecked)
                else if (c == RC_DUMMY || mask == 0u) {}       // placeholder: rt_geom zeroed by design
                else if ((gz & 0x80000000u) != 0u) { ++row_mismatch; chain = "static record over a rigged rt_geom row"; }
                else if (gw == 0u || (uint64_t)gx + gw > m.idxValid || (int32_t)gy < 0 || gy >= m.vboVerts)
                    { ++geom_oob; chain = "rt_geom row out of the IBO / VBO"; }
            }
            else
            {
                ++groups;
                const uint32_t e = custom & 0x7FFFFFu;
                if (e >= m.rigN) { ++group_oob; chain = "group head >= rig entries (rt_rigmap OOB)"; }
                else if (rig)
                {
                    if (rigW(e, 0) != i) { ++head_ne; chain = "rt_rigmap[head].inst != record id"; }
                    const VkDeviceAddress eb = (VkDeviceAddress)rigW(e, 6) | ((VkDeviceAddress)rigW(e, 7) << 32);
                    if (eb != ref && c != RC_DUMMY) { ++head_ne; chain = "record ref != rig entry BLAS"; }
                    // Geometry count of the group BLAS (CPU: the group holding that address now).
                    uint32_t gcount = 0;
                    if (cpu_tables)
                        for (const auto& kv : mRig)
                            if (kv.second.addr == ref) { gcount = (uint32_t)(kv.second.sig.size() / 5); break; }
                    if (!gcount) { ++gcount_unknown; gcount = 1; }
                    if ((uint64_t)e + gcount > m.rigN) { ++member_oob; chain = "group geometries past the rig entries"; }
                    for (uint32_t k = 0; k < gcount && e + k < m.rigN; ++k)
                    {
                        const uint32_t me = e + k;
                        const uint32_t inst = rigW(me, 0), dst = rigW(me, 1), vc = rigW(me, 2);
                        const uint32_t fi = rigW(me, 3), ic = rigW(me, 4);
                        if (inst >= m.instN) { ++member_oob; chain = "member inst >= records (rt_geom OOB)"; continue; }
                        if ((uint64_t)dst + vc > m.rigPosVerts) { ++rigpos_oob; chain = "member vertices past rt_rigpos"; }
                        if ((uint64_t)fi + ic > m.idxValid) { ++rig_idx_oob; chain = "member indices past the IBO"; }
                        const uint8_t* mg = geo + (size_t)inst * kGeomBytes;
                        const uint32_t mz = rd32(mg + 8);
                        if ((mz & 0x40000000u) == 0u && (mz & 0x80000000u) != 0u && (mz & 0x7FFFFFFFu) != dst)
                            { ++rigmap_ne; chain = "member rt_geom row != its rig entry"; }
                    }
                }
            }
            if ((bad_ref || chain || !finite) && printed_bad < kMaxBad)
            {
                ++printed_bad;
                out("[vktest]     BAD inst %u: custom 0x%06x mask 0x%02x flags 0x%02x sbt %u %s ref 0x%llx: %s -- %s; "
                    "rt_geom (%u, %u, 0x%x, 0x%x)%s%s%s",
                    i, custom, mask, flags, sf & 0xFFFFFFu,
                    group ? "rigged-group" : ((gz & 0x40000000u) ? "cluster" : ((gz & 0x80000000u) ? "rigged" : "static")),
                    (unsigned long long)ref, kCatName[c], h ? describeHit(*h).c_str() : "-", gx, gy, gz, gw,
                    chain ? "; chain: " : "", chain ? chain : "", finite ? "" : "; NON-FINITE transform");
                const std::string own = cpuOwner(ref);
                const std::string it = instText(group && rig && (custom & 0x7FFFFFu) < m.rigN ? rigW(custom & 0x7FFFFFu, 0) : i);
                if (!own.empty() || !it.empty()) out("[vktest]       CPU now:%s;%s", own.c_str(), it.c_str());
            }
            if (bad_ref || chain || !finite) ++bad_total;
        }
        active_total += active;
        std::string cats;
        for (int c = 0; c < RC_COUNT; ++c)
            if (cat[c] || c == RC_OK) cats += std::string(cats.empty() ? "" : ", ") + kCatName[c] + " " + std::to_string(cat[c]);
        out("[vktest]     refs: %u active (%u inactive): %s%s", active, inactive, cats.c_str(),
            (cat[RC_FREED_AFTER] && !in_flight) ? " (freed-after-build in a finished frame: expected)" : "");
        out("[vktest]     index chain: custom!=id %u, rt_geom OOB %u, row/record mismatch %u, cluster rows %u (unchecked), "
            "rigged groups %u: head OOB %u, head mismatch %u, members OOB %u, rig_pos OOB %u, rig IBO OOB %u, "
            "member row mismatch %u, geometry count unknown %u%s; mask-0 active %u, non-finite transforms %u",
            custom_ne, geom_oob, row_mismatch, cluster_rows, groups, group_oob, head_ne, member_oob, rigpos_oob,
            rig_idx_oob, rigmap_ne, gcount_unknown, rig ? "" : " (rig entries not staged: heads only)", mask0_active,
            nonfinite);
        const uint32_t bad_refs = cat[RC_INTERIOR] + cat[RC_WRONG_KIND] + cat[RC_FREED_BEFORE]
                                + (in_flight ? cat[RC_FREED_AFTER] : 0u) + cat[RC_ARENA] + cat[RC_OTHER_BUF]
                                + cat[RC_FREED_BUF] + cat[RC_UNKNOWN];
        const uint32_t bad_chain = custom_ne + geom_oob + row_mismatch + group_oob + head_ne + member_oob + rigpos_oob
                                 + rig_idx_oob + rigmap_ne;
        if (!cls.ok)
            out("[vktest]     (references not classified: registry busy)");
        else if (bad_refs == 0 && bad_chain == 0 && nonfinite == 0)
            out("[vktest]     ALL OK: every active record references the start of a live BLAS (or the placeholder) and "
                "its index chain is in bounds -- a bad TLAS->BLAS reference is ruled out for this frame");
        else
            out("[vktest]     BAD: %u bad reference(s), %u index-chain fault(s), %u non-finite transform(s)", bad_refs,
                bad_chain, nonfinite);
    }
    if (gpu_part && printed_bad >= kMaxBad && bad_total > printed_bad)
        out("[vktest]   ... %u more bad record(s) not listed", bad_total - printed_bad);

    // ---- the CPU tables (now) ----
    if (cpu_tables)
    {
        uint32_t printed = 0;
        auto tableLine = [&](const char* what, const std::vector<VkDeviceAddress>& t,
                             const std::function<bool(size_t, VkDeviceAddress)>& expect_ok)
        {
            uint32_t cat[RC_COUNT] = {}, set = 0, mismatch = 0;
            for (size_t k = 0; k < t.size(); ++k)
            {
                if (!t[k]) continue;
                ++set;
                const VkAddrHit* h = cls.find(t[k]);
                // (now: "freed before" = freed already, the table still names it)
                const RefCat c = h ? categorize(*h, t[k], mDummyAddr, reg_now) : RC_BUSY;
                ++cat[c];
                const bool owner_ok = expect_ok ? expect_ok(k, t[k]) : true;
                if (!owner_ok) ++mismatch;
                if ((c != RC_OK && c != RC_DUMMY && c != RC_BUSY) || !owner_ok)
                    if (printed < kMaxBadTables)
                    {
                        ++printed;
                        out("[vktest]     BAD %s[%zu] = 0x%llx: %s -- %s%s", what, k, (unsigned long long)t[k], kCatName[c],
                            h ? describeHit(*h).c_str() : "-", owner_ok ? "" : "; its owner holds another BLAS");
                    }
            }
            std::string cats;
            for (int c = 0; c < RC_COUNT; ++c)
                if (cat[c]) cats += std::string(cats.empty() ? "" : ", ") + kCatName[c] + " " + std::to_string(cat[c]);
            out("[vktest]   CPU %s: %zu rows, %u set: %s; owner mismatch %u", what, t.size(), set,
                cats.empty() ? "-" : cats.c_str(), mismatch);
        };
        out("[vktest]   CPU tables now (registry frame %u, RT frame %u; the GPU's copy lags by the dirty-range upload):",
            reg_now, mFrame);
        tableLine("slot-addr", tblSlot, [&](size_t sl, VkDeviceAddress a)
        {
            if (sl < mTerrOwned.size() && mTerrOwned[sl])
            {
                auto it = mTerrMorph.find((uint32_t)sl);
                return it != mTerrMorph.end() && it->second.addr == a;
            }
            return sl < mSlot.size() && mSlot[sl].addr == a;
        });
        // The reverse: a slot whose BLAS exists but the table does not name it (no BLAS in RT).
        {
            uint32_t missing = 0;
            for (size_t sl = 0; sl < mSlot.size(); ++sl)
                if (mSlot[sl].as && mSlot[sl].addr && !(sl < mTerrOwned.size() && mTerrOwned[sl])
                    && (sl >= tblSlot.size() || tblSlot[sl] != mSlot[sl].addr))
                    ++missing;
            if (missing) out("[vktest]     %u slot BLAS not in the slot-address table", missing);
        }
        tableLine("omm-addr", tblOmm, [&](size_t id, VkDeviceAddress a)
        {
            return id < mOmm.size() && mOmm[id].addr == a;
        });
        tableLine("rig-group", tblRig, nullptr);
        tableLine("rig-entry", tblEntry, nullptr);
        if (mDummyAddr)
        {
            const VkAddrHit* h = cls.find(mDummyAddr);
            out("[vktest]   dummy BLAS 0x%llx: %s", (unsigned long long)mDummyAddr, h ? describeHit(*h).c_str() : "-");
        }
    }

    // ---- the fault addresses (with the readback part only: the main-loop pass has no records) ----
    for (uint32_t f = 0; gpu_part && f < nfaults && faults; ++f)
    {
        const VkDeviceAddress fa = faults[f];
        std::string inside;
        uint32_t inside_n = 0;
        const RefSeen* below = nullptr;
        const RefSeen* above = nullptr;
        for (const RefSeen& r : active_refs)
        {
            const VkAddrHit* h = cls.find(r.ref);
            if (h && h->cls != VKADDR_UNKNOWN && h->lo <= fa && fa < h->lo + h->size && r.ref >= h->lo)
            {
                if (inside_n++ < 4)
                    inside += " inst " + std::to_string(r.inst) + " (RT frame " + std::to_string(r.rtFrame) + ", "
                            + describeHit(*h) + ")";
            }
            if (r.ref <= fa && (!below || r.ref > below->ref)) below = &r;
            if (r.ref > fa && (!above || r.ref < above->ref)) above = &r;
        }
        out("[vktest]   fault 0x%llx: %u audited record(s) reference an AS / buffer containing it%s", (unsigned long long)fa,
            inside_n, inside.c_str());
        if (below)
            out("[vktest]     nearest reference below: 0x%llx (inst %u, RT frame %u), %.2f MB below",
                (unsigned long long)below->ref, below->inst, below->rtFrame, (double)(fa - below->ref) / (1024.0 * 1024.0));
        if (above)
            out("[vktest]     nearest reference above: 0x%llx (inst %u, RT frame %u), %.2f MB above",
                (unsigned long long)above->ref, above->inst, above->rtFrame, (double)(above->ref - fa) / (1024.0 * 1024.0));
        if (!below && !above) out("[vktest]     no audited reference to compare (no stamped readback)");
        VkAddrNeighbours nb;
        if (vkAddrNeighbourAS(fa, nb))
        {
            auto nbLine = [&](const char* tag, const VkAddrHit& h)
            {
                if (h.cls == VKADDR_UNKNOWN) { out("[vktest]     %s: none", tag); return; }
                const double mb = (double)(h.lo > fa ? h.lo - fa : fa - h.lo) / (1024.0 * 1024.0);
                out("[vktest]     %s: %s, start %.2f MB %s", tag, describeHit(h).c_str(), mb, h.lo > fa ? "above" : "below");
            };
            nbLine("nearest live AS below", nb.asBelow);
            nbLine("nearest live AS above", nb.asAbove);
            nbLine("nearest freed AS below", nb.freedAsBelow);
            nbLine("nearest freed AS above", nb.freedAsAbove);
            if (nb.liveTop && fa >= nb.liveTop)
                out("[vktest]     the fault is %.1f MB above the end of every live buffer / AS (0x%llx)",
                    (double)(fa - nb.liveTop) / (1024.0 * 1024.0), (unsigned long long)nb.liveTop);
        }
        else
            out("[vktest]     (address registry busy: no neighbours)");
    }

    // ---- #874 / #889 READ GUARD: what the RT shadow mask's guard caught (host memory: readable
    //      after the loss) and which RT-set buffer a 32-bit index could reach the fault from ----
    if (gpu_part)
    {
        guardReport(lost, cpu_tables);
        guardFaultReach(faults, faults ? nfaults : 0, lost);
    }

    if (!gpu_part) return;
    // ---- verdict ----
    if (!on)
        out("[vktest] RT scene audit: no readback (RenderVKRTLostAudit 0)%s", cpu_tables ? "; CPU tables above" : "");
    else if (audited == 0)
        out("[vktest] RT scene audit: no executed frame in the readback ring (nothing recorded yet, or none ran)");
    else if (bad_total == 0 && cls.ok)
        out("[vktest] RT scene audit: ALL OK -- %u active record(s) in %u executed frame(s), every reference the start of a "
            "live BLAS, every index chain in bounds: candidate 1 (TLAS instance -> bad BLAS) RULED OUT", active_total,
            audited);
    else
        out("[vktest] RT scene audit: %u BAD record(s) in %u executed frame(s) (%u active) -- see the BAD lines above",
            bad_total, audited, active_total);
}


// ===========================================================================
// #874 / #889 RT READ GUARD (clean_rt_common.glsl RTG_*)
// ===========================================================================
namespace
{
constexpr uint32_t kGSites = 16, kGCounts = 20;   // = LLCleanRTVK::kGuardSites / kGuardCounts (RTG_SITES / RTG_COUNTS)
const char* const kGuardSiteName[kGSites] = {
    "-", "instance-id", "rig-map-entry", "prim>=tri-count", "ibo-word", "index>=vert-count", "vbo-word",
    "rig-pos-word", "material-id", "texture-index", "cluster-cut", "cluster-pool", "cluster-rig-arena",
    "proj-shadow-record", "empty-geom-row", "light-table"};

std::string guardSites(const uint32_t* w)
{
    std::string sites;
    for (uint32_t k = 1; k < kGSites; ++k)
        if (w[kGCounts + k])
            sites += std::string(" ") + kGuardSiteName[k] + "=" + std::to_string(w[kGCounts + k]);
    return sites;
}
} // namespace

std::string LLCleanRTVK::guardDescribe(const uint32_t* w, bool cpu_tables) const
{
    const uint32_t* r = w + kGuardRec;
    const uint32_t site = r[0], custom = r[1], geom = r[2], prim = r[3], id = r[4], value = r[5], bound = r[6],
                   fl = r[7], vc = r[12];
    char b[512];
    std::snprintf(b, sizeof(b),
                  "site %u %s: value %u (0x%x) bound %u | hit custom 0x%x%s geom %u prim %u -> id %d%s%s | rt_geom "
                  "{first %u, base 0x%x, z 0x%x, w 0x%x} vert count %d",
                  site, site < kGuardSites ? kGuardSiteName[site] : "?", value, value, bound, custom,
                  (fl & 4u) ? " (rig GROUP)" : "", geom, prim, id == 0xFFFFFFFFu ? -1 : (int)id,
                  (fl & 1u) ? " rigged" : "", (fl & 2u) ? " cluster" : "", r[8], r[9], r[10], r[11],
                  vc == 0xFFFFFFFFu ? -1 : (int)vc);
    std::string o = b;
    if (!cpu_tables) return o;
    // CPU context NOW (the GPU record is up to 3 frames old: the tables may have moved on).
    const size_t rows = mR.mCpuInstances.size() / kInstBytes;
    if (id < rows)
    {
        const uint8_t* in = mR.mCpuInstances.data() + (size_t)id * kInstBytes;
        std::snprintf(b, sizeof(b), " | row now: flags 0x%x lod group %u skin %d", rd32(in + kInstFlags),
                      rd32(in + kInstGroup), (int)rd32(in + kInstSkin));
        o += b;
    }
#if defined(LL_HAS_CLEAN_PIPELINE)
    {
        LLUUID obj;
        S32 face = -1;
        if (id != 0xFFFFFFFFu && LLCleanInstanceStore::instance().keyForId(id, obj, face))
            o += " object " + obj.asString() + " face " + std::to_string(face);
        else
            o += " (no live object)";
    }
#endif
    if (custom & kRigGroupBit)
    {
        // The rig group: its first entry, the member the geometry index names, the group's
        // member count (now) and its BLAS (geometry count, built frame, registry creation frame).
        const uint32_t e0 = custom & 0x7FFFFFu, e = e0 + geom;
        uint32_t members = 0;
        if (e0 < mRigCands.size())
            for (size_t i = e0; i < mRigCands.size() && mRigCands[i].key == mRigCands[e0].key; ++i) ++members;
        std::snprintf(b, sizeof(b), " | rig entries now %zu: group head %u members %u, entry %u%s", mRigEntries.size(),
                      e0, members, e, (members && geom >= members) ? " PAST THE GROUP" : "");
        o += b;
        if (e < mRigEntries.size())
        {
            const RigEntryCPU& re = mRigEntries[e];
            std::snprintf(b, sizeof(b), " {inst %u dst %u verts %u first %u idx %u uv %u}", re.inst, re.dst_base,
                          re.vert_count, re.first_index, re.index_count, re.uv_base);
            o += b;
        }
        if (e0 < mRigCands.size())
        {
            const auto it = mRig.find(mRigCands[e0].key);
            if (it != mRig.end())
            {
                std::snprintf(b, sizeof(b), " | group BLAS 0x%llx owner %llu geometries %zu built RT frame %u last refit %u",
                              (unsigned long long)it->second.addr, (unsigned long long)(it->first >> 8),
                              it->second.sig.size() / 5, it->second.builtFrame, it->second.lastRefit);
                o += b;
                VkAddrHit h;
                const VkDeviceAddress a = it->second.addr;
                if (a && vkAddrClassify(&a, 1, &h) && h.cls != VKADDR_UNKNOWN)
                {
                    std::snprintf(b, sizeof(b), " (registry: id %u created reg frame %u%s)", h.id, h.frame,
                                  h.cls == VKADDR_AS_LIVE ? "" : " NOT LIVE");
                    o += b;
                }
            }
        }
    }
    return o;
}

void LLCleanRTVK::guardPrepare(uint32_t s)
{
    if (s >= 3 || !mGuard[s].mapped) return;
    guardHarvest(s);
    bool on = true;
#if defined(LL_HAS_CLEAN_PIPELINE)
    static LLCachedControl<bool> s_on(gSavedSettings, "RenderVKRTGuard", true);
    on = s_on;
#endif
    // Written bindless slots (renderer writeTextureDescriptors: min(table size, array cap)).
    const uint32_t tex = (uint32_t)std::min<size_t>(mR.mTextures.size(), mR.mMaxBindless);
    uint32_t* w = static_cast<uint32_t*>(mGuard[s].mapped);
    std::memset(w, 0, kRtdBase * 4);   // (the guard words; the candidate diagnostic's: rtdPrepare)
    w[kRtdBase] = rtdPrepare(w);
    mRtdArmed[s] = w[kRtdBase] != 0u;
    mRtdShift[s] = w[kRtdBase] ? w[kRtdBase] - 1u : 0u;
    mRtpArmed[s] = rtpPrepare(w);   // (RenderVKMoverProbePixel)
    w[0] = on ? 1u : 0u;
    w[1] = mFrame;
    w[2] = tex;
    GuardMeta& m = mGuardMeta[s];
    m.valid = true;
    m.rtFrame = mFrame;
    m.regFrame = vkAddrCurrentFrame();
#if defined(LL_HAS_CLEAN_PIPELINE)
    m.gFrame = (uint32_t)gFrameCount;
#endif
    m.texCount = tex;
}

// MOVER PROBE (RenderVKMoverProbePixel, Persist 0; clean_rt_mover_probe.glsl): arm slot s's region for the
// screen-centre pixel -- the render pixel and the mask texel that stands for it -- or leave it zeroed (off).
bool LLCleanRTVK::rtpPrepare(uint32_t* w)
{
    std::memset(w + kRtpBase, 0, (size_t)(kRtpEnd - kRtpBase) * 4);
    U32 on = 0;
#if defined(LL_HAS_CLEAN_PIPELINE)
    static LLCachedControl<U32> s_probe(gSavedSettings, "RenderVKMoverProbePixel", 0U);
    on = s_probe;
#endif
    const uint32_t rw = mR.mRenderW, rh = mR.mRenderH, mw = mR.mRtMaskW, mh = mR.mRtMaskH;
    if (on == 0u || rw < 2 || rh < 2 || mw < 2 || mh < 2) return false;
    const uint32_t px = rw / 2, py = rh / 2;
    const uint32_t mx = std::min<uint32_t>((uint32_t)((uint64_t)px * mw / rw), mw - 1);
    const uint32_t my = std::min<uint32_t>((uint32_t)((uint64_t)py * mh / rh), mh - 1);
    w[kRtpBase] = 1u;
    w[kRtpBase + 1] = px | (py << 16);
    w[kRtpBase + 2] = mx | (my << 16);
    w[kRtpBase + 3] = mFrame;
    return true;
}

// The slot's record, once a second (the slot's fence was waited: the words are this slot's frame's).
void LLCleanRTVK::rtpHarvest(uint32_t s, const uint32_t* w)
{
    (void)s;
    const double now = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    if (now < mRtpNextLog) return;
    mRtpNextLog = now + 1.0;
    auto f = [](uint32_t v) { float x; std::memcpy(&x, &v, 4); return x; };
    const uint32_t* m = w + kRtpMask;
    char b[512];
    std::string o;
    std::snprintf(b, sizeof(b), "mover probe (RenderVKMoverProbePixel) RT frame %u, render px (%u,%u) mask texel (%u,%u):",
                  w[kRtpBase + 3], w[kRtpBase + 1] & 0xFFFFu, w[kRtpBase + 1] >> 16, w[kRtpBase + 2] & 0xFFFFu,
                  w[kRtpBase + 2] >> 16);
    o += b;
    const uint32_t fl = m[0];
    if (fl == 0u)
    {
        o += " the mask did not reach the texel this frame (not dispatched / reused block)";
    }
    else
    {
        std::snprintf(b, sizeof(b), " flags 0x%x [%s%s%s%s%s%s%s%s%s%s%s%s] rp (%u,%u) P (%.3f %.3f %.3f) Ng (%.3f %.3f %.3f)",
                      fl, fl & 1u ? "traced " : "", fl & 2u ? "route " : "", fl & 4u ? "fold " : "", fl & 8u ? "restOn " : "",
                      fl & 16u ? "moverBlocked " : "", fl & 32u ? "localBlock " : "", fl & 64u ? "sunMoverOnly " : "",
                      fl & 128u ? "noMoverLight(SUBGROUP CULL: mover block skipped) " : "", fl & 256u ? "carried " : "",
                      fl & 512u ? "sky " : "", fl & 1024u ? "clusterOverflow " : "",
                      fl & 2048u ? "RT-PROJECTOR-LOOP(mover block skipped)" : "",
                      m[2] & 0xFFFFu, m[2] >> 16, f(m[3]), f(m[4]), f(m[5]), f(m[6]), f(m[7]), f(m[8]));
        o += b;
        std::snprintf(b, sizeof(b), "\n  eligible lights in range %u, with a mover on the segment %u, nloc %u%s, top [%u %u %u %u] (index + 1)",
                      m[9], m[10], m[11] & 0x7FFFFFFFu, (m[11] & 0x80000000u) ? " (all)" : "", m[12], m[13], m[14], m[15]);
        o += b;
        const uint32_t* r = m + 27;
        std::snprintf(b, sizeof(b), "\n  rest: N %.4f D %.4f W %.4f (lum) | slots %u: ids [%u %u %u %u] values [%.4f %.4f %.4f %.4f] | rays %u",
                      f(r[0]), f(r[1]), f(r[2]), r[3], r[4], r[5], r[6], r[7], f(r[8]), f(r[9]), f(r[10]), f(r[11]), r[12]);
        o += b;
        if (fl & 4u)
        {
            // (radiance packing) THE EXACT PIXEL FOLD's plan (clean_rt_mover_probe.glsl RTP_M_FOLD); the rest line's
            // N / D / W are then the U sums over every traced light (w Vs Vm, w Vs, w)
            const uint32_t fp = m[16];
            std::snprintf(b, sizeof(b), "\n  exact fold: %s explicit %u, Vs-only %u%s%s | blocked + statically lit lights "
                          "seen %u, other shadowed %u",
                          (fp & 0x800u) ? "slots," : "no slot (no blocked light static light reaches),", fp & 15u,
                          (fp >> 4) & 15u, (fp & 256u) ? ", N share (blocked lights past the slots)" : "",
                          (fp & 512u) ? ", D share" : "", (fp >> 12) & 255u, (fp >> 20) & 255u);
            o += b;
            // (RTP_M_FOLDL) which lights the fold kept as explicit-slot (C) / Vs-slot (Q) candidates, the unblocked hit
            // lights (H), and the write path -- a light blocked (Vm < 1) that is in NONE of C / Q reached the pixel
            // only through the N / D shares, or not at all (path 2: factor 1 at the pixel)
            const uint32_t* fq = m + 17;
            const uint32_t fc = fq[8];
            std::snprintf(b, sizeof(b), "\n  fold lists (light index + 1): C [%u %u %u %u] (n %u, cBits 0x%x, cBlk 0x%x%s) "
                          "Q [%u %u %u %u] (n %u) H [%u %u %u %u] (n %u) | path %s",
                          fq[0], fq[1], fq[2], fq[3], fc & 15u, (fc >> 16) & 15u, (fc >> 20) & 15u,
                          (fc & 4096u) ? ", a blocked C light past the 4 kept" : "", fq[4], fq[5], fq[6], fq[7], (fc >> 4) & 15u,
                          fq[9] & 255u, (fq[9] >> 8) & 255u, (fq[9] >> 16) & 255u, fq[9] >> 24, (fc >> 8) & 15u,
                          (fc >> 28) == 1u ? "mvFoldWrite (explicit / N / D slots)"
                          : ((fc >> 28) == 2u ? "mvFoldWriteHit (NO blocked light a static light reaches: Vm 1 slots only)"
                                              : "none recorded"));
            o += b;
        }
        const uint32_t ne = m[47];
        const uint32_t cap = (kRtpMaskN - 48) / kRtpEvWords;
        for (uint32_t e = 0; e < std::min(ne, cap); ++e)
        {
            const uint32_t* v = m + 48 + e * kRtpEvWords;
            const uint32_t ty = v[0] & 15u;
            if (ty == 1u)
            {
                const uint32_t ph = (v[0] >> 4) & 15u, bits = v[0] >> 8;
                static const char* const kPh[6] = {"scan", "mover rays", "static ray", "(sun; 1 = mover-only)",
                                                   "GATED OUT (no slot, no ray; vis = smooth N.l)",
                                                   "PASS 3 SKIPPED its static ray"};
                // word [8]: the light's identity (clean_rt_shadow_mask.comp mvLightInfo)
                const uint32_t li = v[8];
                std::snprintf(b, sizeof(b), "\n  light %d %s: w %.4f vis %.4f blocker t %.4f id %d rays %u %s %.4f%s%s%s%s | %s%s",
                              (int)v[1], ph < 6 ? kPh[ph] : "?", f(v[2]), f(v[3]), f(v[4]) > 1e29f ? -1.f : f(v[4]), (int)v[5], v[6],
                              ph == 0 ? "disc r" : (ph == 2 ? "its Vm" : "prad"), f(v[7]),
                              (ph == 0 && (bits & 1u)) ? " segHit" : "", (ph == 1 && (bits & 1u)) ? " own slot" : "",
                              (ph == 1 && (bits & 2u)) ? " traced"
                              : (ph == 5 ? ((bits & 1u) ? " (walled rule)" : " (contribution cut: Vs taken as 1/2)") : ""),
                              (ph == 4 && (bits & 1u)) ? " turned" : ((ph == 4 && (bits & 2u)) ? " no reach (attenuation / frustum / black gobo)" : ""),
                              !(li & 1u) ? "?" : ((li & 2u) ? ((li & 4u) ? "projector (RT record)" : "projector") : "point"),
                              (li & 8u) ? " near an avatar" : "");
                o += b;
                if ((li & 1u) && !(li & 2u))
                {
                    std::snprintf(b, sizeof(b), " own prim r %.3f m", (li >> 16) * 0.001f);
                    o += b;
                }
                b[0] = 0;
            }
            else if (ty == 2u)
            {
                const uint32_t info = v[4];
                const uint32_t kind = (v[0] >> 12) & 15u;
                // (a disc ray of the RT shadow cores -- clean_shadow_rt.glsl shadowDiscRay: the blocker, its flags -- bit 0
                //  rigged, 0x2000 the agent's own avatar --, skin, index count and the side it was met on)
                static const char* const kSide[4] = {"side ?", "met on its FRONT", "met from BEHIND", "side ?"};
                std::snprintf(b, sizeof(b), "\n    ray (%s): vis %.3f t %.4f blocker id %d%s%s flags 0x%x skin %d rt_geom.w %u%s | %s",
                              kind == 2u ? "movers" : (kind == 1u ? "static" : "all"), f(v[1]),
                              f(v[2]) > 1e29f ? -1.f : f(v[2]), (int)v[3], (info & 1u) ? " rigged" : "",
                              ((info >> 8) & 8192u) ? " own-avatar" : "", info >> 8, (int)v[5],
                              v[6] & 0x7FFFFFFFu, (v[6] & 0x80000000u) ? " cluster-drawn" : "", kSide[(v[0] >> 24) & 3u]);
            }
            else if (ty == 3u)
            {
                // (clean_rt_shadow_mask.comp mvProbeRays) the light's ray geometry as the core builds it
                const uint32_t bits = v[0] >> 8;
                const float ox = f(v[2]), oy = f(v[3]), oz = f(v[4]);
                std::snprintf(b, sizeof(b), "\n    geometry light %d: %s%s%s%s%s%s%s origin offset (%.5f %.5f %.5f) |%.5f| "
                              "elevation over the visible facet %.4f tmax %.4f light dist %.4f lift %.5f",
                              (int)v[1], (bits & 1u) ? "point" : "projector",
                              (bits & 2u) ? "" : " BACK-FACE SELF-SHADOWED (the core traced NO ray: Vm = Vs = 0)",
                              (bits & 4u) ? " lifted" : "", (bits & 8u) ? " bfs-rule" : "", (bits & 16u) ? " terminator-band" : "",
                              (bits & 64u) ? " escape-CANCELLED-lift" : ((bits & 128u) ? " escape-shortened-lift" : ""),
                              (bits & 32u) ? " split" : "", ox, oy, oz, std::sqrt(ox * ox + oy * oy + oz * oz), f(v[5]), f(v[6]),
                              f(v[7]), f(v[8]));
            }
            else if (ty == 4u)
            {
                // the two diagnostic hard centre rays from that origin: the mask's mover set vs pass 15's all-caster set
                auto who = [](uint32_t x) -> std::string
                {
                    if (x == 0xFFFFFFFFu) return "none";
                    char t[96];
                    std::snprintf(t, sizeof(t), "id %u%s%s%s", x & 0xFFFFFFu, (x & (1u << 29)) ? " rigged" : "",
                                  (x & (1u << 30)) ? " worn" : "", (x & (1u << 31)) ? " OWN-AVATAR" : "");
                    return t;
                };
                std::snprintf(b, sizeof(b), "\n    diag rays light %d%s: MOVER set vis %.3f t %.4f [%s] | PASS-15 ALL-CASTER set "
                              "vis %.3f t %.4f [%s] | core Vm %.3f%s",
                              (int)v[1], ((v[0] >> 8) & 1u) ? "" : " (tmax <= 0: not traced)", f(v[2]),
                              f(v[3]) > 1e29f ? -1.f : f(v[3]), who(v[4]).c_str(), f(v[5]), f(v[6]) > 1e29f ? -1.f : f(v[6]),
                              who(v[7]).c_str(), f(v[8]),
                              (f(v[5]) < 0.5f && f(v[8]) > 0.5f) ? "  <-- PASS 15 BLOCKED, MASK MOVER LIT" : "");
            }
            else std::snprintf(b, sizeof(b), "\n  event %u: type %u", e, ty);
            o += b;
        }
        if (ne > cap) { std::snprintf(b, sizeof(b), "\n  (%u more events dropped)", ne - cap); o += b; }
    }
    const uint32_t* sp = w + kRtpSp;
    for (uint32_t k = 0; k < 4; ++k)
    {
        const uint32_t* q = sp + k * 56;
        if (!(q[0] & 1u)) continue;
        std::snprintf(b, sizeof(b), "\n  spatial iter %u/%u slot %u%s%s%s: in %.4f out %.4f radius %.2f wsum %.4f hist %.1f | taps (w:lum)",
                      (q[0] >> 4) & 15u, (q[0] >> 8) & 255u, (q[0] >> 20) & 15u, (q[0] & 65536u) ? " on" : " off",
                      (q[0] & 131072u) ? " young" : "", (q[0] & 262144u) ? " scr" : "", f(q[1]), f(q[2]), f(q[3]), f(q[4]), f(q[5]));
        o += b;
        for (uint32_t t = 0; t < 25; ++t)
        {
            if (t % 5 == 0) o += "\n    ";
            if (f(q[6 + t]) < 0.f) std::snprintf(b, sizeof(b), "   --:--    ");
            else std::snprintf(b, sizeof(b), " %.3f:%.3f", f(q[6 + t]), f(q[31 + t]));
            o += b;
        }
    }
    // (RenderVKShadowMoverMask 2) PASS 15's MOVER-EXACT RECORD (clean_rt_mover_probe.glsl RTP_X): whether the probed
    // texel is exact and why (a light's segment meets a mover -- which light, which mover -- or the countdown), then per
    // light its terms t / s, its visibility V, the nearest blocker and whether the ray met a moving caster, and the sum
    const uint32_t* x = w + kRtpX;
    if (x[0] & 1u)
    {
        const uint32_t xf = x[0];
        const uint32_t nl = (xf >> 16) & 511u;
        std::snprintf(b, sizeof(b), "\n  mover exact (mode 2): texel (%u,%u) %s%s%s%s%s%s | countdown last %u now %u | lights %u, "
                      "first mover-crossed light %u (index + 1, 0 none) | tile candidates %u%s | reach %.2f m | job %u",
                      x[1] & 0xFFFFu, x[1] >> 16, (xf & 2u) ? "EXACT" : "stochastic",
                      (xf & 4u) ? " [a light segment meets a mover]" : "", (xf & 8u) ? " [countdown]" : "",
                      (xf & 16u) ? " [LIST FULL: stochastic this frame]" : "", (xf & 32u) ? " [tile overflow: every mover]" : "",
                      (xf & 64u) ? " [bounds overflow: every texel]" : "", (xf >> 8) & 15u, (xf >> 12) & 15u, nl, x[2],
                      x[3] & 0x7FFFFFFFu, (x[3] & 0x80000000u) ? " (all)" : "", f(x[4]), x[5]);
        o += b;
        if (xf & 2u)
        {
            std::snprintf(b, sizeof(b), "\n    sum t V: diffuse %.5f specular %.5f (lum) -> written (demodulated) %.5f",
                          f(x[6]), f(x[7]), f(x[8]));
            o += b;
        }
        const uint32_t shown = std::min<uint32_t>(nl, kRtpXLights);
        for (uint32_t q = 0; q < shown; ++q)
        {
            const uint32_t* l = x + kRtpXL + kRtpXLW * q;
            if (!l[0]) break;
            const uint32_t mi = l[0] >> 17;
            std::snprintf(b, sizeof(b), "\n    light %u: segment %s", (l[0] & 0xFFFFu) - 1u,
                          (l[0] & 0x10000u) ? "meets a mover" : "clear");
            o += b;
            if ((l[0] & 0x10000u) && mi != 0x7FFFu) { std::snprintf(b, sizeof(b), " (mover #%u of the frame's list)", mi); o += b; }
            if ((xf & 2u) && (l[5] >> 8) == (l[0] & 0xFFFFu))
            {
                std::snprintf(b, sizeof(b), " | t %.5f s %.5f V %.4f blocker %s%.3f m%s%s%s", f(l[1]), f(l[2]), f(l[3]),
                              f(l[4]) >= 64999.f ? "none " : "", f(l[4]) >= 64999.f ? 0.f : f(l[4]),
                              (l[5] & 2u) ? " MOVING caster" : "", (l[5] & 4u) ? " (coloured)" : "",
                              (l[5] & 1u) ? "" : " (no light here: not summed)");
                o += b;
            }
        }
        if (nl > shown) { std::snprintf(b, sizeof(b), "\n    (%u more lights not listed)", nl - shown); o += b; }
    }
    LL_INFOS("VKMoverProbe") << o << LL_ENDL;
}

void LLCleanRTVK::guardHarvest(uint32_t s)
{
    GuardMeta& m = mGuardMeta[s];
    const bool armed = m.valid && mGuard[s].mapped;
    if (armed && mRtdArmed[s]) rtdHarvest(s, static_cast<const uint32_t*>(mGuard[s].mapped));
    mRtdArmed[s] = false;
    if (armed && mRtpArmed[s]) rtpHarvest(s, static_cast<const uint32_t*>(mGuard[s].mapped));
    mRtpArmed[s] = false;
    m.valid = false;
    const double now = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    if (armed)
    {
        const uint32_t* w = static_cast<const uint32_t*>(mGuard[s].mapped);
        const uint32_t total = w[5];
        if (total)
        {
            mGuardTotal += total;
            mGuardWinTotal += total;
            ++mGuardFramesHit;
            for (uint32_t k = 0; k < kGuardSites; ++k) mGuardSiteTotal[k] += w[kGuardCounts + k];
            LLVKHitch::count("rt_guardViolations", total);
            bool verbose = false;
#if defined(LL_HAS_CLEAN_PIPELINE)
            static LLCachedControl<bool> s_verbose(gSavedSettings, "RenderVKRTGuardVerbose", false);
            verbose = s_verbose;
#endif
            // Logged once per distinct (site, instance id) -- a bounded set (kGuardSeenCap) --
            // or every harvested frame with RenderVKRTGuardVerbose.
            const uint64_t key = ((uint64_t)w[kGuardRec] << 32) | w[kGuardRec + 4];
            auto it = std::lower_bound(mGuardSeen.begin(), mGuardSeen.end(), key);
            const bool fresh = (it == mGuardSeen.end() || *it != key) && mGuardSeen.size() < kGuardSeenCap;
            if (fresh) mGuardSeen.insert(it, key);
            if (fresh || verbose)
                LL_WARNS("VKRT") << "RT read guard (#874): RT frame " << m.rtFrame << " (viewer frame " << m.gFrame
                                 << "): " << total << " out-of-range read(s) skipped in the RT shadow mask ["
                                 << guardSites(w) << " ] first: " << guardDescribe(w, true)
                                 << (fresh ? "" : " (verbose)") << LL_ENDL;
        }
    }
    if (mGuardWinTotal && now >= mGuardNextSummary)
    {
        std::string sites;
        for (uint32_t k = 1; k < kGuardSites; ++k)
            if (mGuardSiteTotal[k]) sites += std::string(" ") + kGuardSiteName[k] + "=" + std::to_string(mGuardSiteTotal[k]);
        LL_WARNS("VKRT") << "RT read guard (#874) summary: " << mGuardWinTotal << " skipped read(s) since the last summary; session "
                         << mGuardTotal << " in " << mGuardFramesHit << " frame(s), distinct offenders logged "
                         << mGuardSeen.size() << "/" << kGuardSeenCap << ", session by site:" << sites << LL_ENDL;
        mGuardWinTotal = 0;
        mGuardNextSummary = now + 10.0;
    }
}

void LLCleanRTVK::guardReport(bool lost, bool cpu_tables)
{
    AuditOut out;
    out.lost = lost;
    bool on = true;
#if defined(LL_HAS_CLEAN_PIPELINE)
    static LLCachedControl<bool> s_on(gSavedSettings, "RenderVKRTGuard", true);
    on = s_on;
#endif
    out("[vktest] RT read guard (#874 / #889): %s; session %llu skipped read(s) in %u frame(s) (harvested)",
        on ? "on" : "OFF (RenderVKRTGuard 0: reads unchecked)", (unsigned long long)mGuardTotal, mGuardFramesHit);
    for (uint32_t s = 0; s < 3; ++s)
    {
        const GuardMeta m = mGuardMeta[s];
        if (!mGuard[s].mapped) { out("[vktest]   guard slot %u: no buffer", s); continue; }
        if (!m.valid) { out("[vktest]   guard slot %u: not armed (harvested / no RT set this slot)", s); continue; }
        const uint32_t* w = static_cast<const uint32_t*>(mGuard[s].mapped);
        const std::string sites = guardSites(w);
        out("[vktest]   guard slot %u: RT frame %u (registry frame %u, viewer frame %u, %u bindless slots): %u skipped read(s)%s%s",
            s, m.rtFrame, m.regFrame, m.gFrame, m.texCount, w[5], sites.empty() ? "" : " --", sites.c_str());
        if (w[4]) out("[vktest]     first: %s", guardDescribe(w, cpu_tables).c_str());
    }
}

void LLCleanRTVK::guardFaultReach(const VkDeviceAddress* faults, uint32_t n, bool lost)
{
    if (!n || !faults) return;
    AuditOut out;
    out.lost = lost;
    struct B { const char* name; VkBuffer buf; uint32_t stride; };
    const B bufs[] = {
        {"rt_instances", mR.mInstanceSSBO.buffer, 224}, {"rt_materials", mR.mMaterialSSBO.buffer, 96},
        {"rt_geom", mRtGeom.buffer, 16}, {"rt_geom_vc", mRtGeomVc.buffer, 32},
        {"rt_vbo (vertex)", mR.mVbo.buffer, (uint32_t)LLCleanQV::kStride}, {"rt_ibo (index)", mR.mIbo.buffer, 2},
        {"rt_rigpos (vertex)", mRigPos.buffer, 16}, {"rt_rigmap (entry)", mRigEntry.buffer, 32},
        {"rt_cl_pool", mClusterRT ? mClusterRT->poolBuffer() : VK_NULL_HANDLE, 4},
        {"rt_cl_cut", mClusterRT ? mClusterRT->hitCutBuffer() : VK_NULL_HANDLE, 16},
        {"rt_cl_rig", mClusterRT ? mClusterRT->rigBuffer() : VK_NULL_HANDLE, 4},
        {"rt_cl_skin", mClusterRT ? mClusterRT->rigSkinBuffer() : VK_NULL_HANDLE, 4},
    };
    for (uint32_t f = 0; f < n; ++f)
    {
        const VkDeviceAddress fa = faults[f];
        uint32_t hits = 0;
        for (const B& b : bufs)
        {
            VkDeviceAddress lo = 0;
            VkDeviceSize size = 0;
            if (!b.buf || !vkAddrOfBuffer(b.buf, lo, size) || fa < lo) continue;
            const uint64_t off = fa - lo;
            const uint64_t elem = off / b.stride, word = off / 4;
            if (word >= (1ull << 32) && elem >= (1ull << 32)) continue;   // no 32-bit index reaches it
            ++hits;
            out("[vktest]   fault 0x%llx reachable from RT-set buffer %s [0x%llx, +%llu): offset %.1f MB = element %llu "
                "(stride %u; the buffer holds %llu) / word %llu%s",
                (unsigned long long)fa, b.name, (unsigned long long)lo, (unsigned long long)size,
                (double)off / (1024.0 * 1024.0), (unsigned long long)elem, b.stride,
                (unsigned long long)(size / b.stride), (unsigned long long)word, off < size ? " (INSIDE it)" : "");
        }
        if (!hits)
            out("[vktest]   fault 0x%llx: no RT-set buffer with a known address reaches it with a 32-bit index "
                "(a descriptor / BVH pointer, not an SSBO index)", (unsigned long long)fa);
    }
}
