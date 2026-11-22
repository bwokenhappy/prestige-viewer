// Host-test stand-in for indra/llcommon/llerror.h: LL_INFOS / LL_WARNS / LL_WARNS_ONCE ... LL_ENDL
// collect the line and hand it to llstub::logLine (defined by the test, any thread).
#pragma once
#include <sstream>
#include <string>

namespace llstub
{
void logLine(const char* level, const std::string& line);
}

#define LLSTUB_LOG(level) do { const char* llstub_level = level; std::ostringstream llstub_line; llstub_line
#define LL_INFOS(tag) LLSTUB_LOG("INFO")
#define LL_WARNS(tag) LLSTUB_LOG("WARN")
#define LL_WARNS_ONCE(tag) LLSTUB_LOG("WARN")
#define LL_DEBUGS(tag) LLSTUB_LOG("DEBUG")
#define LL_ENDL ""; llstub::logLine(llstub_level, llstub_line.str()); } while (0)
