"""Guards for the DLSS RR sets' target identity (2026-11-21 DEVICE_LOST, RTX 5090, DLSS RR DLAA at 3840x2088).

LLCleanDlssRRVK keeps its descriptor sets across frames and rewrites them only when ready() sees a different Want
(Targets::operator==). A view handle is not an identity: once the view is destroyed, the driver may give a new view
the same handle value. The renderer's screen targets ride targetGen and the RT reflection targets rtEpoch, but the two
denoiser MOT images the RR guide pass binds (23: the GI's, 24: the projector denoiser's) were compared by handle alone.
The projector denoiser re-created its images 1920x1044 -> 3840x2088 in pdnPrestep (device idle, correct), the new
MOT's 2D view came back with the freed view's handle, ready() said yes, and rrGuides read the freed 1920x1044 MOT
(VK_EXT_device_fault READ_INVALID inside it). Correct only while:
  FIELDS     every VkImageView / VkBuffer / generation field of Targets is compared by operator==.
  SERIALS    giMot / pdnMot carry their owner's MOT serial (giMotSerial / pdnMotSerial), compared, and rrWant fills
             them from LLCleanDenoiserVK::motSerial().
  OWNER      the denoiser gives MOT a new process-wide serial each time it creates it (createImage), and the
             history-only re-create (THE LEAN HISTORY) never re-creates MOT (its serial stays: no RR reset per flip).
  FRAME      the fault report's frame clock ticks on the frame-generation path too (beginFrameWithoutImage): without it
             every image of a frame-generation session read "created and freed in the frame of the loss".
"""
import re

NAME = "rr-target-identity"
STAGE = "source"
DESCRIPTION = "the DLSS RR sets are rewritten when a denoiser re-creates the MOT they bind (never keyed on a handle)"
GUARDS = ("A guide set left naming a freed denoiser MOT because the re-created view got the freed handle back "
          "(GPU READ_INVALID / DEVICE_LOST), and fault reports whose frame numbers never advance.")

RR_H = "indra/newview/vkgfx/llcleandlssrrvk.h"
RR = "indra/newview/vkgfx/llcleandlssrrvk.cpp"
REN = "indra/newview/vkgfx/llcleanrenderervk.cpp"
DN = "indra/newview/vkgfx/llcleandenoisevk.cpp"
DN_H = "indra/newview/vkgfx/llcleandenoisevk.h"
SWAP = "indra/newview/vkgfx/vk_swapchain.cpp"


def _strip(text):
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    return re.sub(r"//[^\n]*", "", text)


def _fn_body(src, head):
    at = src.find(head)
    if at < 0:
        return None
    end = src.find("\n}", at)
    return src[at:end if end >= 0 else len(src)]


def run(ctx, result):
    def read(rel):
        with open(ctx.path(rel), encoding="utf-8", errors="replace") as f:
            return _strip(f.read())

    bad = []
    rrh = read(RR_H)
    m = re.search(r"struct Targets\s*\{(.*?)\n    \};", rrh, flags=re.S)
    if not m:
        result.fail("FIELDS: LLCleanDlssRRVK::Targets not found in " + RR_H)
        return
    body = m.group(1)
    fields = []
    for decl in re.finditer(r"(VkImageView|VkBuffer|VkSampler|uint64_t)\s+([^;(]+);", body):
        for part in decl.group(2).split(","):
            name = part.split("=")[0].strip()
            name = re.sub(r"\[.*\]", "", name).strip()
            if name:
                fields.append(name)
    rr = read(RR)
    eq = _fn_body(rr, "bool LLCleanDlssRRVK::Targets::operator==")
    if eq is None:
        bad.append("FIELDS: Targets::operator== not found")
    else:
        for name in fields:
            if not re.search(r"\b%s\b" % re.escape(name), eq):
                bad.append("FIELDS: Targets::%s is not compared by operator== (ready() would keep a set naming "
                           "an old value)" % name)

    # SERIALS
    for mot, serial in (("giMot", "giMotSerial"), ("pdnMot", "pdnMotSerial")):
        if serial not in fields:
            bad.append("SERIALS: Targets::%s has no identity field %s (a recycled view handle compares equal)"
                       % (mot, serial))
    ren = read(REN)
    want = _fn_body(ren, "LLCleanDlssRRVK::Want LLCleanRendererVK::rrWant(")
    if want is None:
        bad.append("SERIALS: LLCleanRendererVK::rrWant not found")
    else:
        if not re.search(r"t\.giMotSerial\s*=\s*t\.giMot\s*\?\s*mGiDn->motSerial\(\)", want):
            bad.append("SERIALS: rrWant does not fill giMotSerial from the GI denoiser's motSerial()")
        if not re.search(r"t\.pdnMotSerial\s*=\s*t\.pdnMot\s*\?\s*mPdn->motSerial\(\)", want):
            bad.append("SERIALS: rrWant does not fill pdnMotSerial from the projector denoiser's motSerial()")

    # OWNER
    dnh = read(DN_H)
    if "uint64_t motSerial() const { return mMotSerial; }" not in dnh:
        bad.append("OWNER: LLCleanDenoiserVK::motSerial() is gone")
    dn = read(DN)
    create = _fn_body(dn, "bool LLCleanDenoiserVK::createImage(")
    if create is None or not re.search(r"if \(i == kDnMotIdx\)\s*\{\s*static std::atomic<uint64_t> (\w+)\{0\};\s*"
                                       r"mMotSerial = \+\+\1;", create):
        bad.append("OWNER: createImage does not give a (re-)created MOT a new process-wide serial")
    if "for (uint32_t i : {kDnHistIdx, kDnSunHistIdx})" not in dn:
        bad.append("OWNER: the history-only re-create no longer limits itself to HIST / SUNHIST (MOT's serial, and "
                   "with it the RR history, would change on every lean flip) -- review and update this check")

    # FRAME
    swap = read(SWAP)
    bfwi = _fn_body(swap, "bool VkSwapchain::beginFrameWithoutImage(")
    if bfwi is None or "vkAddrFrameTick();" not in bfwi:
        bad.append("FRAME: beginFrameWithoutImage does not tick the fault report's frame clock (frame generation)")

    for b in bad:
        result.fail(b)
    if not bad:
        result.summary = "%d Targets fields compared; denoiser MOTs keyed on their serials; frame clock ticks" % len(fields)
