/**
 * @file prestigechatlinehistory.h
 * @brief Ctrl+Up / Ctrl+Down in a chat input bring back the lines sent in that
 *        conversation, for the rest of the viewer session.
 *
 * Each conversation keeps its own lines: nearby chat (the null session id, shared
 * by the nearby chat window and the chat bar) and every IM, group or conference
 * conversation (its session id). The lines outlive the conversation's window, so
 * closing an IM and opening it again keeps them. The browsing itself is
 * LLChatEntry's line history; this only gives each input its conversation's lines.
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

#ifndef PRESTIGE_CHATLINEHISTORY_H
#define PRESTIGE_CHATLINEHISTORY_H

class LLChatEntry;
class LLUUID;

namespace PrestigeChatLineHistory
{
    // A line is being sent from editor in the conversation session_id (null: nearby
    // chat): add it to the editor's lines and keep them for the conversation.
    void remember(const LLUUID& session_id, LLChatEntry* editor);

    // Give editor the conversation's lines (nothing changes when it has them already).
    void restore(const LLUUID& session_id, LLChatEntry* editor);
}

#endif // PRESTIGE_CHATLINEHISTORY_H
