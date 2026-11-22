"""Regression guard for the 360 snapshot preview (World > 360 capture, 2026-11-09).

Two failures, both after the six faces were captured correctly:
  MEDIA-STARVE  Vulkan UI textures go through the async prep service. A candidate in flight
                was dropped whenever the image's generation moved on, and publication
                required candidateGeneration == the current generation. Browser media
                changes every frame (the 360 preview's WebGL view auto-rotates), so no
                candidate ever published and the control kept its first upload: black.
                The candidate must survive a generation change of the same image and size,
                and publish as "latest completed".
  FILE-URL      The page and the cube-map script were addressed as "file:///" + a raw OS
                path. On Windows that is "file:///C:\\Program Files\\..." for the page and a
                bare "C:/Users/..." for the image folder; a space, '#', '%', apostrophe or
                non-ASCII name breaks it (and an apostrophe is a JavaScript syntax error in
                the init() call). Both go through file_url_for_path() (percent-encoded), and
                the quality label through js_single_quoted().
The helpers are extracted from llfloater360capture.cpp, compiled and run on fixed inputs.
"""
import os
import re
import shutil
import subprocess
import tempfile

NAME = "capture-360"
STAGE = "source"
DESCRIPTION = "360 preview: UI media textures publish while changing; file URLs are encoded"
GUARDS = ("Black 360 preview on Vulkan (UI media candidate dropped on every generation change); "
          "'file not found' / broken init() from raw OS paths in file:// URLs (2026-11-09).")

FLOATER = "indra/newview/llfloater360capture.cpp"
BACKEND = "indra/newview/vkgfx/llrendervkbackend.cpp"
PAGE = "indra/newview/skins/default/html/common/equirectangular/eqr_gen.html"


def read(ctx, rel):
    with open(ctx.path(rel), encoding="utf-8", errors="replace") as f:
        return f.read()


def column0_function(text, signature):
    """A function defined at column 0, from its signature to the closing brace at column 0."""
    i = text.find("\n" + signature)
    if i < 0:
        return None
    j = text.find("\n}\n", i)
    return text[i + 1:j + 3] if j >= 0 else None


FIXTURE = r"""
#include <algorithm>
#include <cstdio>
#include <string>
HELPERS
static int fails = 0;
static void expect(const std::string& got, const std::string& want)
{
    if (got != want) { std::printf("got  %s\nwant %s\n", got.c_str(), want.c_str()); ++fails; }
}
int main()
{
    expect(file_url_for_path("/home/u/.slipstreamviewer/a_resident/eqrimg"),
           "file:///home/u/.slipstreamviewer/a_resident/eqrimg");
    expect(file_url_for_path("C:\\Program Files\\Slipstream\\skins\\default\\html\\common\\equirectangular\\eqr_gen.html"),
           "file:///C:/Program%20Files/Slipstream/skins/default/html/common/equirectangular/eqr_gen.html");
    expect(file_url_for_path("C:\\Users\\O'Brien #1 100%\\eqrimg"),
           "file:///C:/Users/O%27Brien%20%231%20100%25/eqrimg");
    expect(file_url_for_path("C:\\Users\\J\xC3\xB6rg\\eqrimg"), "file:///C:/Users/J%C3%B6rg/eqrimg");
    expect(js_single_quoted("Qualit\xC3\xA9 d'aper\xC3\xA7u\\</script>"),
           "Qualit\xC3\xA9 d\\'aper\xC3\xA7u\\\\\\x3C/script>");
    return fails ? 1 : 0;
}
"""


def run_fixture(ctx, result, helpers):
    cxx = os.environ.get("CXX") or shutil.which("c++") or shutil.which("g++") or shutil.which("clang++")
    if not cxx:
        result.warn("no C++ compiler found: file URL fixture not run")
        return False
    with tempfile.TemporaryDirectory() as tmp:
        src = os.path.join(tmp, "test_capture_360.cpp")
        exe = os.path.join(tmp, "test_capture_360")
        with open(src, "w", encoding="utf-8") as f:
            f.write(FIXTURE.replace("HELPERS", helpers))
        c = subprocess.run([cxx, "-std=c++17", "-O0", "-Wall", "-Werror", src, "-o", exe],
                           capture_output=True, text=True)
        if c.returncode:
            result.fail("FILE-URL: fixture compile failed:\n" + c.stdout + c.stderr)
            return False
        r = subprocess.run([exe], capture_output=True, text=True)
        if r.returncode:
            result.fail("FILE-URL: file_url_for_path / js_single_quoted gave:\n" + r.stdout + r.stderr)
            return False
    return True


def check_floater(ctx, result):
    src = read(ctx, FLOATER)
    url = column0_function(src, "std::string file_url_for_path(")
    quote = column0_function(src, "std::string js_single_quoted(")
    if not url or not quote:
        result.fail("FILE-URL: %s has no file_url_for_path() / js_single_quoted()" % FLOATER)
        return False
    for n, line in enumerate(src.splitlines(), 1):
        if re.search(r'"file:///?"\s*(<<|\+)', line):
            result.fail("FILE-URL: %s:%d builds a file URL from a raw path; use file_url_for_path()"
                        % (FLOATER, n))
    if src.count("file_url_for_path(getHTMLBaseFolder()") < 2:
        result.fail("FILE-URL: %s: both pages (default.html, eqr_gen.html) must be loaded through "
                    "file_url_for_path()" % FLOATER)
    if not re.search(r"<<\s*image_dir_url\s*<<", src) or "file_url_for_path(mImageSaveDir)" not in src:
        result.fail("FILE-URL: %s: init() must get the image folder as file_url_for_path(mImageSaveDir)"
                    % FLOATER)
    if "js_single_quoted(getSelectedQualityTooltip())" not in src:
        result.fail("FILE-URL: %s: the overlay label must be escaped with js_single_quoted()" % FLOATER)
    page = read(ctx, PAGE)
    if "img_path + '/cubemap_img.js'" not in page:
        result.fail("FILE-URL: %s no longer loads img_path + '/cubemap_img.js'; update this check "
                    "with the new contract" % PAGE)
    return run_fixture(ctx, result, url + "\n" + quote)


def check_backend(ctx, result):
    src = read(ctx, BACKEND)
    start = src.find("uint32_t LLRenderVKBackend::resolveTexture(")
    end = src.find("\n}\n", start)
    body = src[start:end] if start >= 0 and end > start else ""
    if not body:
        result.fail("MEDIA-STARVE: %s: no LLRenderVKBackend::resolveTexture()" % BACKEND)
        return
    if re.search(r"candidateGeneration\s*!=\s*gen", body):
        result.fail("MEDIA-STARVE: %s: resolveTexture() drops an in-flight candidate when the "
                    "generation moves on; media that changes every frame never publishes" % BACKEND)
    if re.search(r"candidateGeneration\s*==\s*gen", body):
        result.fail("MEDIA-STARVE: %s: resolveTexture() publishes only a candidate of the current "
                    "generation; publish the latest completed one" % BACKEND)
    if "cached.generation = publishedGen" not in body or "noteVKUiUploaded(publishedGen)" not in body:
        result.fail("MEDIA-STARVE: %s: a published candidate must record its own generation "
                    "(cached.generation / noteVKUiUploaded(publishedGen))" % BACKEND)


def run(ctx, result):
    for rel in (FLOATER, BACKEND, PAGE):
        if not os.path.exists(ctx.path(rel)):
            result.fail("%s is missing" % rel)
            result.summary = "360 capture sources missing"
            return
    ran = check_floater(ctx, result)
    check_backend(ctx, result)
    result.summary = ("file URL fixture + UI media publication rules" if ran
                      else "UI media publication / file URL source rules (no compiler)")
