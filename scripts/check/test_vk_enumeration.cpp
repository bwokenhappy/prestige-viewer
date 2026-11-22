// The production snapshot helper, with changing count/fill answers; no Vulkan device.
#include "vk_enumerate.h"
#include <cassert>
#include <cstdio>
#include <string>

std::vector<std::string> logLines;
void receiveLog(const char* line) { logLines.emplace_back(line); }
bool refuseAllocation=false;
template<class T> struct CheckedAllocator {
    using value_type=T;
    CheckedAllocator()=default;
    template<class U> CheckedAllocator(const CheckedAllocator<U>&) {}
    T* allocate(size_t n) { if (refuseAllocation) throw std::bad_alloc(); return std::allocator<T>{}.allocate(n); }
    void deallocate(T* p,size_t n) { std::allocator<T>{}.deallocate(p,n); }
    template<class U> bool operator==(const CheckedAllocator<U>&) const { return true; }
};
int main()
{
    vkenum::setLogSink(&receiveLog);
    std::vector<uint32_t> values{99};
    unsigned counts = 0, fills = 0;
    auto growing = [&](uint32_t* n, uint32_t* p) {
        if (!p) { *n = ++counts == 1 ? 1 : 3; return VK_SUCCESS; }
        ++fills;
        if (*n == 1) { p[0] = 1; return VK_INCOMPLETE; }
        p[0] = 7; p[1] = 8; *n = 2; return VK_SUCCESS;
    };
    assert(vkenum::collect(values, growing, "test grow/shrink") == VK_SUCCESS);
    assert(counts == 2 && fills == 2 && (values == std::vector<uint32_t>{7,8}));
    unsigned tries = 0;
    auto incomplete = [&](uint32_t* n, uint32_t* p) {
        *n = 1;
        if (p) { ++tries; p[0] = 0; return VK_INCOMPLETE; }
        return VK_SUCCESS;
    };
    assert(vkenum::collect(values, incomplete, "test bounded") == VK_INCOMPLETE);
    assert(tries == vkenum::kAttempts && values[0] == 7 && values.size() == 2);
    unsigned zeroCounts = 0;
    auto zero = [&](uint32_t* n, uint32_t*) { ++zeroCounts; *n = 0; return VK_INCOMPLETE; };
    assert(vkenum::collect(values, zero, "test zero") == VK_INCOMPLETE);
    assert(zeroCounts == vkenum::kAttempts && values[0] == 7);
    auto excessive = [](uint32_t* n, uint32_t*) { *n = 5000; return VK_SUCCESS; };
    assert(vkenum::collect(values, excessive, "test cap") == VK_INCOMPLETE);
    auto failure = [](uint32_t*, uint32_t*) { return VK_ERROR_OUT_OF_HOST_MEMORY; };
    assert(vkenum::collect(values, failure, "test error") == VK_ERROR_OUT_OF_HOST_MEMORY);
    auto empty = [](uint32_t* n, uint32_t*) { *n = 0; return VK_SUCCESS; };
    assert(vkenum::collect(values, empty, "test empty") == VK_SUCCESS && values.empty());
    std::vector<uint32_t,CheckedAllocator<uint32_t>> allocated{99};
    auto one=[](uint32_t* n,uint32_t* p) { *n=1; if(p) p[0]=7; return VK_SUCCESS; };
    refuseAllocation=true;
    assert(vkenum::collect(allocated,one,"test allocation refusal")==VK_ERROR_OUT_OF_HOST_MEMORY);
    assert(allocated.size()==1 && allocated[0]==99);
    refuseAllocation=false;
    assert(vkenum::collect(allocated,one,"test allocation recovery")==VK_SUCCESS && allocated[0]==7);
    assert(!logLines.empty() && logLines[0].find("test grow/shrink")!=std::string::npos);
    assert(logLines[0].find("retry 1/4")!=std::string::npos);
    vkenum::setLogSink(nullptr);
    std::puts("Vulkan bounded enumeration: PASS");
}
