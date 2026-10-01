#pragma once
// The capture app's notification-area icon: its tooltip is the live status
// (captured game, fps, latency, online_tune), refreshed with the statistics;
// its menu toggles frame generation, shows the detailed status and quits —
// the same as the hotkeys, for a player who does not remember them. Menu
// choices come back to the thread's message loop as kMsgToggle / kMsgQuit.
#include <windows.h>

#include <string>

namespace bdex {

class Tray {
public:
    static constexpr UINT kMsgToggle = WM_APP + 10;
    static constexpr UINT kMsgQuit = WM_APP + 11;

    bool create();
    void destroy();
    // Tooltip (first lines; Windows keeps 127 characters) and the text of
    // "Statut détaillé…".
    void setStatus(const std::wstring& tip, const std::wstring& details);
    void setGeneration(bool on) { generation_ = on; }

private:
    static LRESULT CALLBACK proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    void add();
    void showMenu();

    HWND hwnd_ = nullptr;
    UINT taskbarCreated_ = 0;
    bool generation_ = true;
    std::wstring tip_ = L"bdex framegen", details_;
};

} // namespace bdex
