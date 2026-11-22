"""Exercise production raw-frame storage and pin the original-frame harvest wiring."""
import csv
import io
import os
import shutil
import subprocess
import tempfile
import xml.etree.ElementTree as ET

NAME = "vk-perf-raw"
STAGE = "source"
DESCRIPTION = "raw every-frame capture joins late GPU values, preserves absence and allocates nothing in collection"
GUARDS = "#1625: raw timings must not allocate, silently zero missing GPU samples or label them with the harvest frame."


def run(ctx, result):
    cxx = os.environ.get("CXX") or shutil.which("c++")
    if not cxx:
        result.fail("C++ compiler required to verify the raw capture hot path")
        return
    with tempfile.TemporaryDirectory() as tmp:
        exe = os.path.join(tmp, "raw")
        proc = subprocess.run([cxx, "-std=c++17", "-O1", "-Wall", "-Wextra", "-Werror", "-pthread",
                               "-I" + ctx.path("indra/newview"), ctx.path("scripts/check/test_vk_perf_raw.cpp"),
                               "-o", exe], capture_output=True, text=True)
        if proc.returncode:
            result.fail(proc.stdout + proc.stderr)
            return
        proc = subprocess.run([exe], capture_output=True, text=True, timeout=30)
        if proc.returncode:
            result.fail("raw fixtures failed: " + proc.stdout + proc.stderr)
            return
    rows = list(csv.DictReader(io.StringIO("\n".join(line for line in proc.stdout.splitlines()
                                                     if not line.startswith("#")))))
    if len(rows) != 8 or [int(r["frame_id"]) for r in rows] != list(range(100, 108)):
        result.fail("CSV lost frame rows or original frame IDs")
        return
    statuses = ["complete", "dropped_not_ready", "absent_not_recorded", "not_ready_at_end",
                "partial_passes_unavailable", "complete"]
    if [r["gpu_status"] for r in rows[:6]] != statuses:
        result.fail("CSV no longer distinguishes absent/dropped/pending/partial GPU samples")
    names = rows[0].keys()
    defer = next(c for c in names if c.startswith("gpu_g_") and c.endswith("_g_defer_ms"))
    join = next(c for c in names if c.endswith("_g_join_gi_ms"))
    if rows[0][defer] != "3.25" or rows[0][join] != "0" or any(r[defer] for r in rows[1:]):
        result.fail("late GPU joins, duplicate accumulation or measured zero vs absent changed")
    if rows[1]["gpu_total_ms"] or rows[2]["gpu_total_ms"] or rows[1]["cpu_1_vk_paceWait_ms"]:
        result.fail("absent measurements must stay blank, never zero")

    def read(path):
        with open(ctx.path(path), encoding="utf-8") as f:
            return f.read()
    renderer = read("indra/newview/vkgfx/llcleanrenderervk.cpp")
    for needle in ("mTsRecordedFrameR[mTsSlot] = LLVKPerfCapture::recordingFrame()",
                   "LLVKBench::subSample(mTsRecordedFrameR[h], sample.name, sample.ms, sample.async)",
                   "LLVKBench::gpuFrame(mTsRecordedFrameR[h], f[0], rawPassesComplete)",
                   "LLVKPerfCapture::gpuDropped(mTsRecordedFrameR[h])"):
        if needle not in renderer:
            result.fail("original recording identity/miss wiring missing: " + needle)
    capture = read("indra/newview/llvkperfcapture.cpp")
    frame = capture[capture.index("void LLVKPerfCapture::frameTick()"):]
    if frame.index("rawTick(completed_frame, now, dt_ms)") > frame.index("mRecent.emplace_back"):
        result.fail("raw mode must bypass the legacy deque/vector allocation path")
    if "std::launch::async" not in capture or "LLFloaterReportBug::scrubText(headerText()" not in capture:
        result.fail("CSV must export asynchronously with allowlisted scrubbed metadata")
    root = ET.fromstring(read("indra/newview/app_settings/settings.xml"))
    entries = list(root.find("map"))
    index = next(i for i, node in enumerate(entries) if node.tag == "key" and node.text == "RenderVKPerfCaptureRawSeconds")
    setting = list(entries[index + 1])
    fields = {setting[i].text: setting[i + 1].text for i in range(0, len(setting), 2)}
    if fields["Persist"] != "0" or fields["Value"] != "0":
        result.fail("raw capture duration must be nonpersistent and off by default")
    result.summary = "zero collection allocations; late joins, missing/zero/partial, overflow, CSV and detach fixtures pass"
