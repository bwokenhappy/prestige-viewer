"""Regression guard for the direct stage of media on prims (TVs, web pages): a ring of stage buffers per live media.

A media frame used to travel through the shared texture preparation tickets like a decoded texture: the main thread
snapshots the pixels, a worker prepares them, a later frame records the copy, and the ticket holds its media slot
until its descriptor bank retires. That is four full-frame CPU passes and at most one frame every few render frames.
The direct stage leases each live media a small ring of persistently mapped stage buffers from the prep service
(the service stays the only owner of staging memory); the main thread copies the changed texels into a free entry
and the same frame records the copy into the bound image. An entry is written again only after that frame's fence.
No free entry skips the frame; no ring (budget, resize, first frame, unusual format) uses the tickets as before.

  * the fixture (scripts/check/test_media_direct.cpp, includes indra/newview/vkgfx/llvkmediadirect.h): an entry in
    flight or pinned is never picked, a two-entry ring serves every frame with two frames in flight, the budget
    admits four HD TVs and not a fifth, a resize / first frame / pending ticket goes to the tickets, the copied
    rectangle is clamped to the image (an untrusted one becomes the whole frame);
  * source wiring: the prep service sizes and gates its rings through the header, the renderer decides through it,
    registers the ring entry with the frame's recording (the fence), returns the ring with the media slot, reports
    the three counters in the texture-world-stage interval line, and releases the service slot of a ready ticket it
    drops at record time (that drop leaked the slot and its stage bytes).
"""
import os
import re
import shutil
import subprocess
import tempfile

NAME = "media-direct"
STAGE = "source"
DESCRIPTION = ("Media on prims upload through a leased ring of stage buffers in the frame that has the pixels; "
               "ring entries are reused only after their frame's fence, tickets remain the fallback")
GUARDS = ("Choppy media on prims (5-10 fps on screen while the page paints 30+): media frames queued behind "
          "preparation tickets; a stage buffer rewritten while a frame in flight still copies from it; a prep "
          "service slot leaked by a ticket dropped at record time.")

HEADER = "indra/newview/vkgfx/llvkmediadirect.h"
SERVICE_H = "indra/newview/vkgfx/llvktextureprepservice.h"
SERVICE = "indra/newview/vkgfx/llvktextureprepservice.cpp"
RENDERER = "indra/newview/vkgfx/llcleanrenderervk.cpp"
FIXTURE = "scripts/check/test_media_direct.cpp"


def _function(code, signature):
    at = code.find(signature)
    if at < 0:
        return ""
    open_at = code.find("{", at)
    depth = 0
    for i in range(open_at, len(code)):
        if code[i] == "{":
            depth += 1
        elif code[i] == "}":
            depth -= 1
            if depth == 0:
                return code[open_at:i + 1]
    return ""


def _wiring(ctx):
    missing = []

    def read(rel):
        with open(ctx.path(rel), encoding="utf-8", errors="replace") as f:
            return f.read()

    service_h = read(SERVICE_H)
    if '#include "llvkmediadirect.h"' not in service_h:
        missing.append("llvktextureprepservice.h does not include llvkmediadirect.h")
    if not re.search(r"kDirectRingEntries\s*=\s*llvkmediadirect::kRingEntries\s*;", service_h) \
            or not re.search(r"kDirectStageLimit\s*=\s*llvkmediadirect::kBudgetBytes\s*;", service_h):
        missing.append("the prep service does not take its ring size and direct budget from llvkmediadirect.h")

    service = read(SERVICE)
    lease = _function(service, "bool LLVKTexturePrepService::leaseDirectStages(")
    if "llvkmediadirect::leaseFits(" not in lease:
        missing.append("leaseDirectStages does not admit a ring through llvkmediadirect::leaseFits (the byte budget)")
    state = _function(service, "llvkmediadirect::Entry LLVKTexturePrepService::directStage(")
    if not re.search(r"llvkmediadirect::entryState\(\s*slot->recordPending\s*,\s*slot->retirementSerial\s*,"
                     r"\s*slot->quarantined\s*\)", state):
        missing.append("directStage does not gate a ring entry through llvkmediadirect::entryState(recordPending, "
                       "retirementSerial, quarantined): an entry could be rewritten before its frame's fence")
    retire = _function(service, "void LLVKTexturePrepService::retireGpuUse(")
    if "slot.direct" not in retire:
        missing.append("retireGpuUse does not make a retired ring entry writable again")

    code = read(RENDERER)
    if '#include "llvkmediadirect.h"' not in code:
        missing.append("llcleanrenderervk.cpp does not include llvkmediadirect.h")
    direct = _function(code, "LLCleanRendererVK::MediaDirectResult LLCleanRendererVK::recordDirectMediaStage(")
    if not direct:
        missing.append("LLCleanRendererVK::recordDirectMediaStage is missing")
    else:
        for call, why in (("llvkmediadirect::route(", "direct or tickets"),
                          ("llvkmediadirect::region(", "the copied rectangle"),
                          ("llvkmediadirect::pickEntry(", "the ring entry"),
                          ("llvkmediadirect::ringDead(", "a ring that never comes back"),
                          ("llvkmediadirect::leaseDue(", "the lease retry")):
            if call not in direct:
                missing.append("recordDirectMediaStage does not decide %s through %s...)" % (why, call))
        if "leaseDirectStages(" not in direct:
            missing.append("recordDirectMediaStage does not lease its ring from the prep service")
        record_at = direct.find("recordTexturePrepTicket(")
        copy_at = direct.find("recordTexUpload(")
        if record_at < 0 or copy_at < 0 or record_at > copy_at:
            missing.append("recordDirectMediaStage records the copy without first registering the ring entry with "
                           "the frame's recording (recordTexturePrepTicket): no fence would guard its reuse")
        if "mTexFrameMediaGen[mTexFrameMediaGenCount++]" not in direct:
            missing.append("recordDirectMediaStage does not publish the generation at submit (mTexFrameMediaGen)")
        if re.search(r"vkWaitForFences|vkDeviceWaitIdle|vkQueueWaitIdle", direct):
            missing.append("recordDirectMediaStage waits on the GPU: a busy ring must skip the frame")
    updates = _function(code, "void LLCleanRendererVK::recordMediaUpdates(")
    if "recordDirectMediaStage(" not in updates or "startImmutableMediaStage(" not in updates:
        missing.append("recordMediaUpdates does not try the direct stage with the tickets as its fallback")
    if not re.search(r"checks = \(uint32_t\)std::min<size_t>\(kChecksPerFrame, mMediaSlots\.size\(\)\);", updates) \
            or "checked < checks;" not in updates:
        missing.append("recordMediaUpdates may visit a media slot twice in a frame: its generation is published "
                       "at submit, so the direct stage would copy the same frame into both ring entries")
    untrack = _function(code, "void LLCleanRendererVK::untrackMediaSlot(")
    if "returnMediaDirectLease(" not in untrack:
        missing.append("untrackMediaSlot does not return the media's ring (leaked until shutdown)")
    track = _function(code, "void LLCleanRendererVK::trackMediaSlot(")
    if "returnMediaDirectLease(" not in track:
        missing.append("trackMediaSlot keeps the previous media's ring when the slot changes its media")
    if '" media-direct/skipped/fallback "' not in code:
        missing.append("the texture-world-stage interval line does not report media-direct/skipped/fallback")

    ready = _function(code, "void LLCleanRendererVK::recordReadyTexStages(")
    drops = 0
    for m in re.finditer(r"discardTexStageCandidate\(ticket\);\s*"
                         r"if \(ticket\.life == TexStageTicket::Life::Quarantine\) continue;(.*?)"
                         r"ticket = TexStageTicket\{\};", ready, re.S):
        drops += 1
        if "releaseTexStageService(ticket, false);" not in m.group(1):
            missing.append("recordReadyTexStages drops a stale ready ticket without releaseTexStageService: its "
                           "prep service slot and stage bytes leak")
    if drops < 2:
        missing.append("recordReadyTexStages: the two stale-ticket drops (media, ordinary) were not found; update "
                       "this check")
    return missing


def run(ctx, result):
    if not os.path.isfile(ctx.path(HEADER)):
        result.fail(HEADER + " is missing: media on prims have no direct stage (every frame waits for a ticket)")
        return
    cxx = os.environ.get("CXX") or shutil.which("c++") or shutil.which("g++") or shutil.which("clang++")
    if cxx:
        with tempfile.TemporaryDirectory() as tmp:
            exe = os.path.join(tmp, "test_media_direct")
            c = subprocess.run([cxx, "-std=c++17", "-O2", "-Wall", "-I" + os.path.dirname(ctx.path(HEADER)),
                                ctx.path(FIXTURE), "-o", exe], capture_output=True, text=True)
            if c.returncode != 0:
                result.fail("test_media_direct.cpp did not compile:\n" + c.stderr[-2000:])
                return
            r = subprocess.run([exe], capture_output=True, text=True)
            if r.returncode != 0:
                result.fail("test_media_direct failed:\n" + r.stdout[-2000:])
                return
    missing = _wiring(ctx)
    if missing:
        result.fail("\n".join(missing))
        return
    if not cxx:
        result.warn("no C++ compiler found: the fixture not run (source wiring checked)")
        result.summary = "wiring only (no compiler)"
        return
    result.summary = "ring / budget / route / region rules tested, wiring holds"
