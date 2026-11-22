/* Host test of the DLSS add-on's pure parts (no NGX): contrib/ngxshim/src/last_error.h (the failure
 * text slip_nr_last_error returns, per thread: the main thread's DLSS evaluate and the present thread's
 * frame generation evaluate may fail at once), and contrib/ngxshim/src/fg_caps.h (what slip_nr_fg_caps answers:
 * the reason frame generation cannot run, and a caller's older, shorter struct). The numbers here are made up. Built and run (also under
 * ThreadSanitizer where the compiler has it) by scripts/check/checks/framegen_addon.py. */
#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <cstdint>
#include <cstdlib>
#include <unistd.h>
#include "contrib/ngxshim/src/fg_caps.h"
#include "contrib/ngxshim/src/last_error.h"

int main()
{
    // ---- fg_caps.h: what slip_nr_fg_caps answers ----
    {
        slip_fg_query ok;   // (every read succeeded, the library is there, GPU scheduling is on)
        ok.availableRead = true; ok.available = true; ok.maxRead = true; ok.maxGenerated = 3;
        ok.library = SLIP_FG_STATE_YES; ok.gpuScheduling = SLIP_FG_STATE_YES;
        slip_fg_answer a = slip_fg_decide(ok);
        assert(a.available == 1 && a.maxGenerated == 3 && a.reason == SLIP_FG_REASON_NOT_STATED);
        // nothing known about the library or the scheduling (Linux has no such setting): no cause is claimed
        { slip_fg_query q = ok; q.library = SLIP_FG_STATE_UNKNOWN; q.gpuScheduling = SLIP_FG_STATE_UNKNOWN;
          a = slip_fg_decide(q); assert(a.available == 1 && a.reason == SLIP_FG_REASON_NOT_STATED); }
        // the read of "available" failed: not "this GPU cannot", but "no answer"
        { slip_fg_query q = ok; q.availableRead = false; q.available = false;
          a = slip_fg_decide(q); assert(a.available == 0 && a.maxGenerated == 0 && a.reason == SLIP_FG_REASON_QUERY_FAILED); }
        // not available, the library is in none of the feature folders: that is the cause (before a driver hint)
        { slip_fg_query q = ok; q.available = false; q.library = SLIP_FG_STATE_NO; q.needsNewerDriver = true;
          a = slip_fg_decide(q); assert(a.available == 0 && a.reason == SLIP_FG_REASON_LIBRARY_MISSING); }
        // not available, the library is there, the driver is too old: the driver fields say it, no other cause
        { slip_fg_query q = ok; q.available = false; q.needsNewerDriver = true; q.gpuScheduling = SLIP_FG_STATE_NO;
          a = slip_fg_decide(q); assert(a.available == 0 && a.reason == SLIP_FG_REASON_NOT_STATED); }
        // not available, Windows' GPU scheduling is off
        { slip_fg_query q = ok; q.available = false; q.gpuScheduling = SLIP_FG_STATE_NO;
          a = slip_fg_decide(q); assert(a.available == 0 && a.reason == SLIP_FG_REASON_GPU_SCHEDULING_OFF); }
        // the runtime says "available" with GPU scheduling off: it cannot run, and the answer says why
        { slip_fg_query q = ok; q.gpuScheduling = SLIP_FG_STATE_NO;
          a = slip_fg_decide(q); assert(a.available == 0 && a.maxGenerated == 0 && a.reason == SLIP_FG_REASON_GPU_SCHEDULING_OFF); }
        // not available and nothing else known: no cause claimed (the caller's general text)
        { slip_fg_query q = ok; q.available = false;
          a = slip_fg_decide(q); assert(a.available == 0 && a.reason == SLIP_FG_REASON_NOT_STATED); }
        // ... also when the folders could not be read (or none was given): "missing" is never a guess
        { slip_fg_query q = ok; q.available = false; q.library = SLIP_FG_STATE_UNKNOWN;
          a = slip_fg_decide(q); assert(a.available == 0 && a.reason == SLIP_FG_REASON_NOT_STATED); }
        // available, but the most generated frames could not be read (or reads 0): one generated frame (2x) is
        // what every GPU with frame generation runs
        { slip_fg_query q = ok; q.maxRead = false; q.maxGenerated = 0;
          a = slip_fg_decide(q); assert(a.available == 1 && a.maxGenerated == 1); }
        { slip_fg_query q = ok; q.maxGenerated = 0; a = slip_fg_decide(q); assert(a.available == 1 && a.maxGenerated == 1); }
        // a library missing while the runtime says "available" (it found one elsewhere): no cause
        { slip_fg_query q = ok; q.library = SLIP_FG_STATE_NO; a = slip_fg_decide(q); assert(a.available == 1 && a.reason == 0); }
    }
    // The caller's struct: the first release's 28 bytes get no reason written (and nothing after them is
    // touched); the 32 bytes of 1.3.2 do; a larger, newer one keeps what this library does not know.
    {
        struct Caps { uint32_t struct_size, available, max_generated, needs_newer_driver, min_driver_major, min_driver_minor,
                               application_id_set, unavailable_reason; };
        slip_fg_answer a; a.available = 0; a.maxGenerated = 0; a.reason = SLIP_FG_REASON_LIBRARY_MISSING;
        uint32_t mem[10];
        for (uint32_t& m : mem) m = 0xABABABABu;
        Caps* c = reinterpret_cast<Caps*>(mem);
        slip_fg_write_caps(c, 28u, a, true, 580u, 88u);
        // (application_id_set: always 0 since 1.4.0 -- the add-on reads no application id)
        assert(mem[0] == 28u && mem[1] == 0 && mem[2] == 0 && mem[3] == 1 && mem[4] == 580 && mem[5] == 88 && mem[6] == 0);
        assert(mem[7] == 0xABABABABu && mem[8] == 0xABABABABu);
        slip_fg_write_caps(c, 32u, a, false, 0u, 0u);
        assert(mem[0] == 32u && mem[3] == 0 && mem[6] == 0 && mem[7] == SLIP_FG_REASON_LIBRARY_MISSING && mem[8] == 0xABABABABu);
        slip_fg_write_caps(c, 40u, a, false, 0u, 0u);
        assert(mem[0] == 40u && mem[7] == SLIP_FG_REASON_LIBRARY_MISSING && mem[8] == 0xABABABABu && mem[9] == 0xABABABABu);
        assert(slip_fg_size_ok(28u, 28u) && slip_fg_size_ok(32u, 28u) && slip_fg_size_ok(4096u, 28u));
        assert(!slip_fg_size_ok(27u, 28u) && !slip_fg_size_ok(0u, 28u));
    }
    // NVIDIA's frame generation library by its file name, and whether a folder holds it
    assert(slip_fg_is_library_name("nvngx_dlssg.dll") && slip_fg_is_library_name("NVNGX_DLSSG.DLL"));
    assert(slip_fg_is_library_name("libnvidia-ngx-dlssg.so.310.9.16") && slip_fg_is_library_name("libnvidia-ngx-dlssg.so"));
    assert(!slip_fg_is_library_name("nvngx_dlss.dll") && !slip_fg_is_library_name("nvngx_dlssd.dll"));
    assert(!slip_fg_is_library_name("libnvidia-ngx-dlss.so.310.9.16") && !slip_fg_is_library_name("libnvidia-ngx-dlssd.so.310.9.16"));
    assert(!slip_fg_is_library_name("nvngx_dlssg.dll.bak") && !slip_fg_is_library_name("") && !slip_fg_is_library_name(nullptr));
    {
        const char* base = std::getenv("TMPDIR");
        std::string tmpl = std::string(base && *base ? base : "/tmp") + "/slip_fg_lib_XXXXXX";
        const char* dir = mkdtemp(&tmpl[0]);
        assert(dir);
        const std::string d(dir);
        auto touch = [&d](const char* name) { FILE* f = std::fopen((d + "/" + name).c_str(), "w"); assert(f); std::fclose(f); };
        assert(slip_fg_library_in(dir) == SLIP_FG_STATE_NO);                    // an empty folder
        touch("libnvidia-ngx-dlss.so.310.9.16"); touch("libnvidia-ngx-dlssd.so.310.9.16"); touch("libslipstream_ngx.so");
        assert(slip_fg_library_in(dir) == SLIP_FG_STATE_NO);                    // DLSS alone: installed before this feature
        touch("libnvidia-ngx-dlssg.so.310.9.16");
        assert(slip_fg_library_in(dir) == SLIP_FG_STATE_YES);
        assert(slip_fg_library_in((d + "/nowhere").c_str()) == SLIP_FG_STATE_UNKNOWN);   // unreadable: nothing claimed
        assert(slip_fg_library_in("") == SLIP_FG_STATE_UNKNOWN && slip_fg_library_in(nullptr) == SLIP_FG_STATE_UNKNOWN);
        // several feature folders: found in any = there; missing only when every folder was read and none has it
        assert(slip_fg_merge_library(SLIP_FG_STATE_UNKNOWN, SLIP_FG_STATE_NO) == SLIP_FG_STATE_NO);       // (the first folder)
        assert(slip_fg_merge_library(SLIP_FG_STATE_NO, SLIP_FG_STATE_YES) == SLIP_FG_STATE_YES);
        assert(slip_fg_merge_library(SLIP_FG_STATE_YES, SLIP_FG_STATE_NO) == SLIP_FG_STATE_YES);
        assert(slip_fg_merge_library(SLIP_FG_STATE_NO, SLIP_FG_STATE_NO) == SLIP_FG_STATE_NO);
        for (const char* n : {"libnvidia-ngx-dlss.so.310.9.16", "libnvidia-ngx-dlssd.so.310.9.16", "libslipstream_ngx.so",
                              "libnvidia-ngx-dlssg.so.310.9.16"})
            std::remove((d + "/" + n).c_str());
        rmdir(dir);
    }

    // The failure text: never NULL, "" before any failure; each thread reads its own last failure, and the
    // pointer it got stays readable (and unchanged) until that thread's next failure, whatever the other
    // thread does meanwhile.
    assert(slip_last_error_text() != nullptr && *slip_last_error_text() == '\0');
    slip_set_last_error("Super Resolution evaluate failed: the main thread's failure");
    const char* mine = slip_last_error_text();
    std::thread present([]
    {
        assert(*slip_last_error_text() == '\0');
        for (int i = 0; i < 20000; ++i)
        {
            slip_set_last_error(i % 2 ? "NGX_VK_EVALUATE_DLSSG failed: the present thread's failure, a longer text than the other one"
                                      : "NGX_VK_EVALUATE_DLSSG failed");
            assert(std::strncmp(slip_last_error_text(), "NGX_VK_EVALUATE_DLSSG failed", 28) == 0);
        }
    });
    for (int i = 0; i < 20000; ++i)
        assert(std::strcmp(mine, "Super Resolution evaluate failed: the main thread's failure") == 0);
    present.join();
    assert(slip_last_error_text() == mine);
    assert(std::strcmp(mine, "Super Resolution evaluate failed: the main thread's failure") == 0);
    std::puts("add-on tests pass");
    return 0;
}
