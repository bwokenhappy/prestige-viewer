// Test-only (prestige/tests/vkdevice): minimal stand-ins for the Firestorm llcommon symbols the
// ported vkgfx objects reference, so the device code links without building llcommon.
#include "linden_common.h"
#include "llerror.h"
#include "llfile.h"
#include "llapp.h"
#include "llmemory.h"
#include <cstdio>
#include <sstream>
#include <unistd.h>
namespace LLError {
CallSite::CallSite(ELevel level, const char* file, int line, const std::type_info& class_info, const char* function, bool print_once, const char** tags, size_t tag_count)
: mLevel(level), mFile(file), mLine(line), mClassInfo(class_info), mFunction(function), mTags(tags), mTagCount(tag_count), mPrintOnce(print_once), mLevelString(""), mCached(false), mShouldLog(false) {}
CallSite::~CallSite() {}
bool Log::shouldLog(CallSite&) { return true; }
void Log::flush(const std::ostringstream& out, const CallSite& site) { fprintf(stderr, "[%d] %s\n", (int)site.mLevel, out.str().c_str()); }
}
LLFILE* LLFile::fopen(const std::filesystem::path& p, const char* mode, int) { return ::fopen(p.string().c_str(), mode); }
int LLFile::close(LLFILE* f) { return f ? ::fclose(f) : 0; }
int LLFile::remove(const std::filesystem::path& p, int) { return ::remove(p.string().c_str()); }
int LLFile::rename(const std::filesystem::path& a, const std::filesystem::path& b, int) { return ::rename(a.string().c_str(), b.string().c_str()); }
std::filesystem::file_status LLFile::getStatus(const std::filesystem::path& p, bool, int) { std::error_code ec; return std::filesystem::status(p, ec); }
int LLApp::getPid() { return (int)getpid(); }
U64 LLMemory::getCurrentRSS() { return 0; }
