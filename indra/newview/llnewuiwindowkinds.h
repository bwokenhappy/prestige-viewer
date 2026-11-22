// #703 The native windows the framework knows (llnewuiwindow.h): each window's file registers its
// kind. Doc: doc/newui-windows.md (parity per window).
#pragma once

class LLNewUIWindows;

void slNewUIRegisterChat(LLNewUIWindows& windows);      // llnewuichat.cpp: "chat" (conversations)
void slNewUIRegisterPeople(LLNewUIWindows& windows);    // llnewuipeople.cpp: "people"
void slNewUIRegisterInventory(LLNewUIWindows& windows); // llnewuiinventory.cpp: "inventory" (several)

inline void slNewUIRegisterWindows(LLNewUIWindows& windows)
{
    slNewUIRegisterChat(windows);
    slNewUIRegisterPeople(windows);
    slNewUIRegisterInventory(windows);
}
