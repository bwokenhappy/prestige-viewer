"""Regression guard for the frame generation block ("slip_fg") of the DLSS add-on's ABI header
(contrib/ngxshim/include/slipstream_ngx.h).

The viewer and the separately built add-on share this header verbatim; a struct whose struct_size
moved, a PFN_ typedef that no longer matches its prototype, or a header that stops compiling as
plain C99 breaks one side silently at run time. scripts/check/test_framegen_abi.c compiles it as
C99, as C++17 and as C99 with SLIP_NR_NO_PROTOTYPES (warnings are errors); everything it tests is
checked at compile time, so it is compiled only (-c), never linked: the entry points live in the
add-on.
"""
import os
import shutil
import subprocess
import tempfile

NAME = "framegen-abi"
STAGE = "source"
DESCRIPTION = "slip_fg ABI header: C99 + C++17, struct_size first, PFN typedefs match"
GUARDS = ("The viewer and the frame generation add-on disagreeing on the slip_fg ABI (a moved "
          "struct_size, a changed entry point) without a build error on either side.")

HEADER = "contrib/ngxshim/include/slipstream_ngx.h"
TEST = "scripts/check/test_framegen_abi.c"


def vulkan_include(ctx):
    for d in (os.environ.get("VULKAN_INCLUDE"), ctx.path("build-windows-x86_64/vk/include"), "/usr/include"):
        if d and os.path.exists(os.path.join(d, "vulkan", "vulkan.h")):
            return d
    return None


def run(ctx, result):
    if not os.path.exists(ctx.path(HEADER)):
        result.fail("%s is missing" % HEADER)
        result.summary = "header missing"
        return
    cc = os.environ.get("CC") or shutil.which("cc") or shutil.which("clang")
    cxx = os.environ.get("CXX") or shutil.which("c++") or shutil.which("clang++")
    vk = vulkan_include(ctx)
    if not cc or not cxx or not vk:
        result.warn("no C/C++ compiler or Vulkan headers: the slip_fg ABI was not compiled")
        result.summary = "not compiled"
        return
    incs = ["-I" + ctx.path("contrib/ngxshim/include"), "-I" + vk]
    ran = []
    with tempfile.TemporaryDirectory() as tmp:
        for tool, lang in ((cc, ["-std=c99", "-x", "c"]), (cxx, ["-std=c++17", "-x", "c++"]),
                           (cc, ["-std=c99", "-x", "c", "-DSLIP_NR_NO_PROTOTYPES"])):
            obj = os.path.join(tmp, "abi_%d.o" % len(ran))
            c = subprocess.run([tool] + lang + ["-c", "-Wall", "-Wextra", "-Werror"] + incs
                               + [ctx.path(TEST), "-o", obj], capture_output=True, text=True)
            if c.returncode:
                result.fail("%s (%s) did not compile:\n%s" % (TEST, lang[0], c.stdout + c.stderr))
            else:
                ran.append(" ".join(x for x in lang if x.startswith("-std") or x.startswith("-D")))
    result.summary = ("compiled as " + ", ".join(ran)) if ran else "failed"
