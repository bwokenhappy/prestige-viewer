// Included twice: stubs before extracted production functions, then executable checks.
#ifndef WORKER_IDENTITY_STUBS
#define WORKER_IDENTITY_STUBS
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <vector>

using VkDevice = int;
using VkPhysicalDevice = int;
using VkShaderModule = int;
constexpr int VK_NULL_HANDLE=0, VK_SUCCESS=0, VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO=1, VK_UUID_SIZE=16;
struct VkShaderModuleCreateInfo { int sType; size_t codeSize=0; const uint32_t* pCode=nullptr; };
struct VkPhysicalDeviceProperties { uint32_t vendorID=1, deviceID=2, driverVersion=3; uint8_t pipelineCacheUUID[16]{}; };
struct ModuleInfo { uint64_t hash; std::string name; };
struct SpecModSrc { const uint32_t* code; size_t bytes; const char* name; };
static std::vector<uint32_t> created, recorded;
static std::string recordedName;
int vkCreateShaderModule(VkDevice, const VkShaderModuleCreateInfo* ci, void*, VkShaderModule* m)
{
    created.assign(ci->pCode, ci->pCode + ci->codeSize/4); *m=1; return VK_SUCCESS;
}
void vkNoteShaderModule(VkShaderModule, const uint32_t* code, size_t bytes, const char* name)
{
    recorded.assign(code, code+bytes/4); recordedName=name ? name : "";
}
#define VK_CHECK(call) assert((call)==VK_SUCCESS)
bool vkDeviceLastParity(std::string& request, uint64_t& fp, std::string& text)
{ request="device"; fp=42; text="parity"; return true; }
void vkGetPhysicalDeviceProperties(VkPhysicalDevice, VkPhysicalDeviceProperties*) {}
template<class... T> void workerLog(const char*, T...) {}
#define LL_INFOS(tag) std::ostringstream()
#define LL_ENDL std::endl
using LLFILE = FILE;
namespace LLFile { FILE* fopen(const std::string& path, const char* mode) { return std::fopen(path.c_str(),mode); } int close(FILE* f) { return std::fclose(f); } }
void writeFileAtomic(const std::string& path, const std::string& bytes)
{
    const std::string temp=path+".tmp";
    { std::ofstream out(temp, std::ios::binary); out.write(bytes.data(), bytes.size()); assert(out.good()); }
    assert(std::rename(temp.c_str(), path.c_str())==0);
}
std::string contents(const std::string& path)
{ std::ifstream in(path, std::ios::binary); return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()}; }
#else
void roundTrip(const std::vector<uint32_t>& expected)
{
    assert(created==expected && recorded==expected);
    std::vector<uint32_t> refreshed;
    assert(worldRespin(recordedName, refreshed));
    // The original bug fails here: same recorded name, different wire/plain bytes.
    assert(refreshed==recorded);
}
int main()
{
    struct Pair { const char* plain; const char* wire; const uint32_t* code; size_t bytes; };
    const Pair pairs[] = {
        {"Frag","FragWire",kWorldFragWireSpv,kWorldFragWireSpv_bytes},
        {"FragRt","FragRtWire",kWorldFragRtWireSpv,kWorldFragRtWireSpv_bytes},
        {"FragBlend","FragBlendWire",kWorldFragBlendWireSpv,kWorldFragBlendWireSpv_bytes},
        {"FragBlendRt","FragBlendRtWire",kWorldFragBlendRtWireSpv,kWorldFragBlendRtWireSpv_bytes},
        {"FragGbuf","FragGbufWire",kWorldFragGbufWireSpv,kWorldFragGbufWireSpv_bytes},
        {"FragGbufNt","FragGbufNtWire",kWorldFragGbufNtWireSpv,kWorldFragGbufNtWireSpv_bytes},
        {"FragGbufBlend","FragGbufBlendWire",kWorldFragGbufBlendWireSpv,kWorldFragGbufBlendWireSpv_bytes}
    };
    const uint32_t placeholder[]={7,8};
    for (const auto& p : pairs)
    {
        const std::vector<uint32_t> expected(p.code,p.code+p.bytes/4);
        const std::string symbol="kWorld"+std::string(p.plain)+"Spv";
        moduleFromSpv(1,placeholder,sizeof(placeholder),symbol.c_str(),true);
        roundTrip(expected); assert(recordedName==p.wire);
        assert(moduleLabel(1,{42,recordedName})==p.plain);
        specModule(1,{placeholder,sizeof(placeholder),p.plain},true);
        roundTrip(expected); assert(recordedName==p.wire);
        assert(moduleLabel(1,{42,recordedName})==p.plain);
        swapModule(placeholder,sizeof(placeholder),symbol.c_str(),true);
        roundTrip(expected); assert(recordedName==p.wire);
        assert(moduleLabel(1,{42,recordedName})==p.plain);
    }
    moduleFromSpv(1,kWorldFragSpv,kWorldFragSpv_bytes,"kWorldFragSpv",false);
    roundTrip({kWorldFragSpv,kWorldFragSpv+ kWorldFragSpv_bytes/4}); assert(recordedName=="Frag");
    specModule(1,{kWorldFragBlendSpv,kWorldFragBlendSpv_bytes,"FragBlend"},false);
    roundTrip({kWorldFragBlendSpv,kWorldFragBlendSpv+kWorldFragBlendSpv_bytes/4}); assert(recordedName=="FragBlend");
    std::vector<uint32_t> unchanged={9,10}; assert(!worldRespin("unknown",unchanged)); assert(unchanged==std::vector<uint32_t>({9,10}));
    const uint32_t* ptr=placeholder; size_t size=sizeof(placeholder);
    GMDXRezFx::wireModule(nullptr,ptr,size,true); assert(ptr==placeholder && size==sizeof(placeholder));
    GMDXRezFx::wireModule("unknown",ptr,size,true); assert(ptr==placeholder && size==sizeof(placeholder));

    Writer current; assert(writeHeader(current,1));
    uint32_t version=0; std::memcpy(&version,current.s.data()+4,4); assert(version==6);   // (6: + GraphicsPipelineDesc::addMrt, MBOIT)
    std::string corrupt=current.s; corrupt[0]='X'; assert(!earlyAccept(corrupt) && readAccept(corrupt)==2);
    std::string old=current.s; const uint32_t v4=4; std::memcpy(&old[4],&v4,4);
    assert(!earlyAccept(old) && readAccept(old)==2);
    assert(!earlyAccept(old.substr(0,7)) && readAccept(old.substr(0,7))==2);
    writeFileAtomic("cache","main cache seed"); writeFileAtomic("cache.worker","worker cache seed");
    writeFileAtomic("cache.recipe",old);
    // No accepted early recipe: the in-session writer supplies the new semantic format.
    assert(vkPipelineWorkerSaveRecipe("cache",current.s));
    assert(contents("cache.recipe")==current.s);
    assert(earlyAccept(contents("cache.recipe")) && readAccept(contents("cache.recipe"))==0);
    assert(!vkPipelineWorkerSaveRecipe("cache",current.s));
    assert(contents("cache")=="main cache seed" && contents("cache.worker")=="worker cache seed");
    std::puts("worker semantic identity and recipe regeneration PASS");
}
#endif
