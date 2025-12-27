#pragma once

#include <windows.h>

// Shows (or focuses) the help window.
void HelpWindow_Show(HWND owner);
void HelpWindow_ShowTopic(HWND owner, const wchar_t *topicHtml);
