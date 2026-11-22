"""Source guard: fresh texture staging buffers are budgeted per frame (2026-11-08 micro-stutters:
vk_texUpload / vk_texResidency / vk_texResUp / vk_texStagePrepare 7-29 ms in one frame).

A texture-prep submit that no cached stage fits creates and maps a new host-visible buffer on
the main thread (~0.5-1 ms each: "main-stage-allocation-cpu-ns" / "stage-allocation-calls" in
the texture-prep-service interval line). The residency pass checks its time budget only
between upgrades, so a batch of admitted upgrades paid every allocation in the same frame.
  FRESH-CAP    LLVKTexturePrepService::submit refuses (Deferred) a fresh stage past the frame's
               kFreshStagesPerFrame before it calls createBufferBounded, and counts the ones
               it makes.
  FRAME-RESET  beginFrame resets the budget; LLVulkanContext::renderFrame calls it.
  SIZE-CLASS   fresh stages are allocated in size classes (stageClassBytes) so freed ones are
               reused by similar jobs.
"""
import re

NAME = "tex-stage-budget"
STAGE = "source"
DESCRIPTION = "fresh texture staging allocations are capped per frame (no batch of vkAllocateMemory in one frame)"
GUARDS = ("7-29 ms vk_texResUp / vk_texStagePrepare frames: a residency batch allocating every new "
          "staging buffer in one frame (2026-11-08).")

SVC = "indra/newview/vkgfx/llvktextureprepservice.cpp"
CTX = "indra/newview/vkgfx/llvulkancontext.cpp"


def _strip_comments(text):
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    return re.sub(r"//[^\n]*", "", text)


def _function(text, signature):
    at = text.find(signature)
    if at < 0:
        return None
    start = text.index("{", at)
    depth = 0
    for i in range(start, len(text)):
        if text[i] == "{":
            depth += 1
        elif text[i] == "}":
            depth -= 1
            if depth == 0:
                return text[start:i + 1]
    return None


def run(ctx, result):
    svc = _strip_comments(open(ctx.path(SVC), encoding="utf-8").read())
    vk = _strip_comments(open(ctx.path(CTX), encoding="utf-8").read())
    bad = []

    sub = _function(svc, "LLVKTexturePrep::QueueResult LLVKTexturePrepService::submit(")
    if not sub:
        bad.append("LLVKTexturePrepService::submit not found; update this check")
    else:
        alloc = sub.find("createBufferBounded(")
        gate = re.search(r"freshStagesThisFrame\s*>=\s*kFreshStagesPerFrame[^;]*?\)\s*\{[^{}]*?return QueueResult::Deferred;",
                         sub, flags=re.S)
        if alloc < 0:
            bad.append("submit no longer allocates stages with createBufferBounded; update this check")
        elif not gate or gate.start() > alloc:
            bad.append("FRESH-CAP: submit allocates a fresh stage without first refusing past kFreshStagesPerFrame")
        if not re.search(r"\+\+mImpl->freshStagesThisFrame", sub):
            bad.append("FRESH-CAP: submit does not count the fresh stages it allocates")
        if "stageClassBytes(" not in sub:
            bad.append("SIZE-CLASS: fresh stages are no longer allocated in size classes (stageClassBytes)")

    bf = _function(svc, "void LLVKTexturePrepService::beginFrame(")
    if not bf or "freshStagesThisFrame = 0" not in bf:
        bad.append("FRAME-RESET: LLVKTexturePrepService::beginFrame does not reset the fresh-stage budget")
    rf = _function(vk, "bool LLVulkanContext::renderFrame(")
    if not rf or "->beginFrame()" not in rf:
        bad.append("FRAME-RESET: LLVulkanContext::renderFrame does not start the texture-prep frame budget")

    for b in bad:
        result.fail(b)
    if not bad:
        result.summary = "fresh staging capped per frame (kFreshStagesPerFrame), size-classed, reset per frame"
