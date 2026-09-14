#include "platform.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <tlhelp32.h>
extern char** _environ;  // MSVCRT/MinGW (also in <stdlib.h>); must stay at global scope
#else
#include <dirent.h>
#include <fstream>
#include <unistd.h>
extern char** environ;
#endif

namespace bdex {
namespace {

// Lower-cased basename, extension dropped: "BEService64.exe" -> "beservice64",
// "GameMon.des" -> "gamemon".
std::string normalizeName(std::string s) {
    size_t p = s.find_last_of("/\\");
    if (p != std::string::npos) s = s.substr(p + 1);
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    size_t dot = s.find('.');
    if (dot != std::string::npos) s.resize(dot);
    return s;
}

bool isDeniedAntiCheat(const std::string& name) {
    // User-mode processes of multiplayer anti-cheats. These only run while
    // their protected game runs, so matching them disables the layer just
    // for that game. Vanguard (vgc/vgk) is deliberately absent: it installs
    // as a system-wide service, so keying on it would disable the layer for
    // every game on any machine that has Valorant installed.
    static const char* const kDenied[] = {
        "easyanticheat",          // Easy Anti-Cheat
        "easyanticheat_launcher", // EAC launcher
        "start_protected_game",   // EAC-wrapped game stub
        "beservice",              // BattlEye service (32-bit)
        "beservice64",            // BattlEye service (64-bit)
        "bedaisy",                // BattlEye driver helper
        "gamemon",                // nProtect GameGuard
        "xigncode",               // XIGNCODE3
        "mhyprot2",               // miHoYo Protect
    };
    for (const char* d : kDenied)
        if (name == d) return true;
    return false;
}

#ifdef _WIN32
std::string narrow(const wchar_t* w) {
    if (!w) return "";
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return "";
    std::string s(static_cast<size_t>(n), '\0');  // n includes the terminator
    WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
    s.resize(static_cast<size_t>(n) - 1);
    return s;
}
#endif

} // namespace

std::vector<std::string> platformProcessNames() {
    std::vector<std::string> names;
    auto add = [&names](const std::string& raw) {
        std::string b = normalizeName(raw);
        if (!b.empty() && std::find(names.begin(), names.end(), b) == names.end()) names.push_back(b);
    };
#ifdef _WIN32
    wchar_t exe[MAX_PATH];
    if (GetModuleFileNameW(nullptr, exe, MAX_PATH)) add(narrow(exe));
    int argc = 0;
    if (LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc)) {
        for (int i = 0; i < argc; ++i) add(narrow(argv[i]));
        LocalFree(argv);
    }
#else
    std::ifstream in("/proc/self/cmdline", std::ios::binary);
    std::string arg;
    while (std::getline(in, arg, '\0')) {
        size_t a = arg.find_first_not_of(" \t\r\n");
        if (a == std::string::npos) continue;
        add(arg.substr(a));
    }
#endif
    return names;
}

char** platformEnviron() {
#ifdef _WIN32
    return _environ;  // declared at global scope above
#else
    return environ;  // declared at file scope above
#endif
}

std::string platformConfigPath() {
#ifdef _WIN32
    const char* appdata = std::getenv("APPDATA");
    if (appdata && *appdata) return std::string(appdata) + "\\bdex-framegen\\bdex-framegen.conf";
    return "";
#else
    if (const char* x = std::getenv("XDG_CONFIG_HOME")) return std::string(x) + "/bdex-framegen.conf";
    if (const char* h = std::getenv("HOME")) return std::string(h) + "/.config/bdex-framegen.conf";
    return "";
#endif
}

bool platformAntiCheatPresent(std::string* foundName) {
#ifdef _WIN32
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return false;
    bool found = false;
    PROCESSENTRY32W pe;
    pe.dwSize = sizeof(pe);
    if (Process32FirstW(snap, &pe)) {
        do {
            std::string n = normalizeName(narrow(pe.szExeFile));
            if (isDeniedAntiCheat(n)) {
                if (foundName) *foundName = n;
                found = true;
                break;
            }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return found;
#else
    // Also catches Wine/Proton, where the anti-cheat's .exe shows up as a
    // regular /proc entry whose exe link points into the prefix.
    DIR* proc = opendir("/proc");
    if (!proc) return false;
    bool found = false;
    while (dirent* e = readdir(proc)) {
        if (!std::isdigit(static_cast<unsigned char>(e->d_name[0]))) continue;
        char link[16 + 256], exe[512];
        snprintf(link, sizeof(link), "/proc/%s/exe", e->d_name);
        ssize_t n = readlink(link, exe, sizeof(exe) - 1);
        if (n <= 0) continue;
        exe[n] = '\0';
        std::string name = normalizeName(exe);
        if (isDeniedAntiCheat(name)) {
            if (foundName) *foundName = name;
            found = true;
            break;
        }
    }
    closedir(proc);
    return found;
#endif
}

} // namespace bdex
