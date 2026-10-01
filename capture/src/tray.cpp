#include "tray.h"

#include <shellapi.h>

#include <algorithm>
#include <cwchar>

namespace bdex {
namespace {

constexpr UINT kCallback = WM_APP + 1;
constexpr UINT kIconId = 1;
enum { kCmdToggle = 100, kCmdDetails, kCmdQuit };

} // namespace

bool Tray::create() {
    const HINSTANCE inst = GetModuleHandleW(nullptr);
    const wchar_t* cls = L"bdex_capture_tray";
    WNDCLASSEXW wc{sizeof wc};
    if (!GetClassInfoExW(inst, cls, &wc)) {
        wc.lpfnWndProc = proc;
        wc.hInstance = inst;
        wc.lpszClassName = cls;
        if (!RegisterClassExW(&wc)) return false;
    }
    // A hidden top-level window (message-only windows do not get the
    // TaskbarCreated broadcast, sent when Explorer restarts).
    hwnd_ = CreateWindowExW(0, cls, L"bdex framegen", 0, 0, 0, 0, 0, nullptr, nullptr, inst, this);
    if (!hwnd_) return false;
    taskbarCreated_ = RegisterWindowMessageW(L"TaskbarCreated");
    add();
    return true;
}

void Tray::add() {
    NOTIFYICONDATAW nid{sizeof nid};
    nid.hWnd = hwnd_;
    nid.uID = kIconId;
    nid.uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE;
    nid.uCallbackMessage = kCallback;
    nid.hIcon = LoadIconW(nullptr, MAKEINTRESOURCEW(32512));  // IDI_APPLICATION, as a wide resource id (no UNICODE here)
    wcsncpy_s(nid.szTip, tip_.c_str(), _TRUNCATE);
    Shell_NotifyIconW(NIM_ADD, &nid);
}

void Tray::destroy() {
    if (!hwnd_) return;
    NOTIFYICONDATAW nid{sizeof nid};
    nid.hWnd = hwnd_;
    nid.uID = kIconId;
    Shell_NotifyIconW(NIM_DELETE, &nid);
    DestroyWindow(hwnd_);
    hwnd_ = nullptr;
}

void Tray::setStatus(const std::wstring& tip, const std::wstring& details) {
    details_ = details;
    if (tip == tip_ || !hwnd_) return;
    tip_ = tip;
    NOTIFYICONDATAW nid{sizeof nid};
    nid.hWnd = hwnd_;
    nid.uID = kIconId;
    nid.uFlags = NIF_TIP;
    wcsncpy_s(nid.szTip, tip_.c_str(), _TRUNCATE);
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

void Tray::showMenu() {
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING | (generation_ ? MF_CHECKED : MF_UNCHECKED), kCmdToggle,
                L"Génération d'images activée\tCtrl+Alt+G");
    AppendMenuW(menu, MF_STRING, kCmdDetails, L"Statut détaillé…");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kCmdQuit, L"Quitter\tCtrl+Alt+Q");
    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(hwnd_);  // so that the menu closes when clicking elsewhere
    const UINT cmd = static_cast<UINT>(TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY, pt.x, pt.y, 0, hwnd_, nullptr));
    DestroyMenu(menu);
    if (cmd == kCmdToggle) PostThreadMessageW(GetCurrentThreadId(), kMsgToggle, 0, 0);
    if (cmd == kCmdQuit) PostThreadMessageW(GetCurrentThreadId(), kMsgQuit, 0, 0);
    if (cmd == kCmdDetails)
        MessageBoxW(nullptr, details_.empty() ? L"(pas encore de mesure)" : details_.c_str(), L"bdex framegen — en cours",
                    MB_OK | MB_ICONINFORMATION | MB_TOPMOST);
}

LRESULT CALLBACK Tray::proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
    }
    auto* self = reinterpret_cast<Tray*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (self && msg == self->taskbarCreated_ && msg != 0) {
        self->add();  // Explorer restarted: the icon is gone
        return 0;
    }
    if (self && msg == kCallback && (LOWORD(lp) == WM_RBUTTONUP || LOWORD(lp) == WM_CONTEXTMENU ||
                                     LOWORD(lp) == WM_LBUTTONUP)) {
        self->showMenu();
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

} // namespace bdex
