/**
 * @file prestigechatlinehistory.cpp
 * @brief Ctrl+Up / Ctrl+Down in a chat input bring back the lines sent in that
 *        conversation, for the rest of the viewer session.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Prestige Viewer Source Code
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;
 * version 2.1 of the License only.
 * $/LicenseInfo$
 */

#include "llviewerprecompiledheaders.h"

#include "prestigechatlinehistory.h"

#include "llchatentry.h"
#include "lluuid.h"

#include <map>
#include <string>
#include <vector>

namespace
{
    // Lines kept per conversation (the oldest go first).
    constexpr size_t MAX_LINES = 100;

    typedef std::vector<std::string> lines_t;

    std::map<LLUUID, lines_t>& store()
    {
        static std::map<LLUUID, lines_t> sLines;
        return sLines;
    }
}

namespace PrestigeChatLineHistory
{
    void remember(const LLUUID& session_id, LLChatEntry* editor)
    {
        if (!editor)
        {
            return;
        }
        // (sending by Enter has already added it through LLChatEntry::onCommit;
        // updateHistory skips a line that is the same as the last one)
        editor->updateHistory();

        lines_t lines = editor->getLineHistory();
        if (lines.size() > MAX_LINES)
        {
            lines.erase(lines.begin(), lines.begin() + static_cast<std::ptrdiff_t>(lines.size() - MAX_LINES));
            editor->setLineHistory(lines);
        }
        store()[session_id] = std::move(lines);
    }

    void restore(const LLUUID& session_id, LLChatEntry* editor)
    {
        if (!editor)
        {
            return;
        }
        const std::map<LLUUID, lines_t>& lines = store();
        const auto it = lines.find(session_id);
        if (it != lines.end())
        {
            editor->setLineHistory(it->second);
        }
        else
        {
            editor->setLineHistory(lines_t());
        }
    }
}
