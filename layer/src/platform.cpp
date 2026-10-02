#include "platform.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN  // (bdex_capture defines it for the whole target)
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shellapi.h>
#include <tlhelp32.h>
// _environ comes from <stdlib.h> (the UCRT makes it a macro over
// __p__environ(): redeclaring it here was an inconsistent-linkage warning).
#else
#include <dirent.h>
#include <dlfcn.h>
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
        "easyanticheat_eos",      // EAC through Epic Online Services
        "start_protected_game",   // EAC-wrapped game stub
        "beservice",              // BattlEye service (32-bit)
        "beservice64",            // BattlEye service (64-bit)
        "bedaisy",                // BattlEye driver helper
        "gamemon",                // nProtect GameGuard
        "gamemon64",              // nProtect GameGuard (64-bit)
        "pnkbstra",               // PunkBuster services
        "pnkbstrb",
        "xigncode",               // XIGNCODE3
        "mhyprot2",               // miHoYo Protect
    };
    for (const char* d : kDenied)
        if (name == d) return true;
    return false;
}

} // namespace

bool platformIsCompetitiveGame(const std::string& name) {
    // Competitive multiplayer games, by executable (normalizeName: lower
    // case, no directory, no extension). Some anti-cheats run no process of
    // their own to detect — VAC lives in the game and Steam, Ricochet and
    // Vanguard in a driver or service — and EAC / BattlEye under Proton run
    // in the game's process. A layer loaded into these games is what gets
    // accounts banned (CS2 banned AMD Anti-Lag+ users in 2023 for hooking
    // the game), so the layer stays off in them, whatever the config says.
    static const char* const kGames[] = {
        // Valve Anti-Cheat
        "cs2", "csgo", "csgo_linux64",      // Counter-Strike 2 / CS:GO
        "dota2",                            // Dota 2
        "deadlock", "project8",             // Deadlock
        "tf_linux64", "tf_win64", "tf",     // Team Fortress 2 (64-bit)
        // Vanguard, Ricochet, Defense Matrix, NetEase
        "valorant", "valorant-win64-shipping",
        "league of legends",
        "cod",                              // Call of Duty (Black Ops 6, Warzone...)
        "overwatch",                        // Overwatch 2
        "marvel-win64-shipping",            // Marvel Rivals
        // EAC / BattlEye, whose processes Proton does not show
        "r5apex", "r5apex_dx12",            // Apex Legends
        "fortniteclient-win64-shipping",
        "rainbowsix", "rainbowsix_vulkan",  // Rainbow Six Siege
        "tslgame",                          // PUBG
        "rustclient",                       // Rust
        "deadbydaylight-win64-shipping",
        "haloinfinite",
        "destiny2",
        "escapefromtarkov",
        "rocketleague",
        "squadgame",                        // Squad
        "aces",                             // War Thunder
        "worldoftanks",
    };
    for (const char* g : kGames)
        if (name == g) return true;
    return false;
}

namespace {

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

std::string platformBundledProfilesPath() {
    if (const char* p = std::getenv("BDEX_FG_PROFILES")) return p;  // "" disables them
#ifdef _WIN32
    // Next to the module that contains this code (the layer DLL, or
    // bdex_capture.exe): the release zip ships profiles.conf beside both.
    HMODULE self = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&platformBundledProfilesPath), &self))
        return "";
    wchar_t buf[MAX_PATH];
    DWORD n = GetModuleFileNameW(self, buf, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return "";
    std::string path = narrow(buf);
    size_t slash = path.find_last_of("\\/");
    return slash == std::string::npos ? "" : path.substr(0, slash + 1) + "profiles.conf";
#else
    // Installed layout: <prefix>/lib[32]/libVkLayer_bdex_framegen.so and
    // <prefix>/share/bdex-framegen/profiles.conf.
    Dl_info info{};
    if (!dladdr(reinterpret_cast<void*>(&platformBundledProfilesPath), &info) || !info.dli_fname) return "";
    std::string lib = info.dli_fname;
    size_t slash = lib.rfind('/');
    if (slash == std::string::npos) return "";
    return lib.substr(0, slash) + "/../share/bdex-framegen/profiles.conf";
#endif
}

std::string platformExpandPath(const std::string& in) {
    if (in.empty()) return in;
#ifdef _WIN32
    // The OS resolves %VAR%; an undefined %VAR% is left verbatim. Two calls:
    // size probe (includes the terminator), then fill.
    DWORD n = ExpandEnvironmentStringsA(in.c_str(), nullptr, 0);
    if (n == 0) return in;
    std::string out(n, '\0');
    DWORD w = ExpandEnvironmentStringsA(in.c_str(), out.data(), n);
    if (w == 0 || w > n) return in;
    out.resize(w - 1);  // drop the terminator
    return out;
#else
    std::string out;
    out.reserve(in.size());
    for (size_t i = 0; i < in.size();) {
        char c = in[i];
        if (c == '~' && i == 0 && (in.size() == 1 || in[1] == '/')) {
            if (const char* h = std::getenv("HOME")) out += h;
            else out += '~';
            ++i;
        } else if (c == '$' && i + 1 < in.size()) {
            size_t j = i + 1;
            bool brace = in[j] == '{';
            if (brace) ++j;
            size_t start = j;
            while (j < in.size() && (std::isalnum(static_cast<unsigned char>(in[j])) || in[j] == '_')) ++j;
            std::string name = in.substr(start, j - start);
            if (brace) { if (j < in.size() && in[j] == '}') ++j; else name.clear(); }
            const char* v = name.empty() ? nullptr : std::getenv(name.c_str());
            if (v) { out += v; i = j; }
            else { out += c; ++i; }  // leave an unknown/malformed reference as-is
        } else {
            out += c;
            ++i;
        }
    }
    return out;
#endif
}

std::string platformStatusDir() {
    if (const char* p = std::getenv("BDEX_FG_STATUS_DIR")) return p;
#ifdef _WIN32
    const char* tmp = std::getenv("TEMP");
    if (tmp && *tmp) return std::string(tmp) + "\\bdex-framegen";
    return "";
#else
    if (const char* x = std::getenv("XDG_RUNTIME_DIR"); x && *x) return std::string(x) + "/bdex-framegen";
    return "/tmp/bdex-framegen-" + std::to_string(static_cast<long>(getuid()));
#endif
}

bool platformAppSessionActive() {
    const std::string dir = platformStatusDir();
    if (dir.empty()) return false;
#ifdef _WIN32
    std::ifstream in(dir + "\\app.session");
#else
    std::ifstream in(dir + "/app.session");
#endif
    long pid = 0;
    if (!(in >> pid) || pid <= 0) return false;
#ifdef _WIN32
    HANDLE h = OpenProcess(SYNCHRONIZE, FALSE, static_cast<DWORD>(pid));
    if (!h) return false;
    const bool alive = WaitForSingleObject(h, 0) == WAIT_TIMEOUT;
    CloseHandle(h);
    return alive;
#else
    // Alive and still the control panel (not a recycled process id).
    std::ifstream cmd("/proc/" + std::to_string(pid) + "/cmdline", std::ios::binary);
    const std::string line((std::istreambuf_iterator<char>(cmd)), std::istreambuf_iterator<char>());
    return line.find("bdex-framegen") != std::string::npos;
#endif
}

std::string platformCacheDir() {
    if (const char* p = std::getenv("BDEX_FG_CACHE_DIR")) return p;
#ifdef _WIN32
    const char* app = std::getenv("LOCALAPPDATA");
    if (app && *app) return std::string(app) + "\\bdex-framegen";
    return "";
#else
    if (const char* x = std::getenv("XDG_CACHE_HOME"); x && *x) return std::string(x) + "/bdex-framegen";
    if (const char* h = std::getenv("HOME"); h && *h) return std::string(h) + "/.cache/bdex-framegen";
    return "";
#endif
}

long platformPid() {
#ifdef _WIN32
    return static_cast<long>(GetCurrentProcessId());
#else
    return static_cast<long>(getpid());
#endif
}

bool platformAntiCheatPresent(std::string* foundName) {
    // This process is a competitive game (its executable or, under Proton,
    // the Windows .exe on its command line).
    for (const std::string& n : platformProcessNames())
        if (platformIsCompetitiveGame(n)) {
            if (foundName) *foundName = n + " (competitive game)";
            return true;
        }
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
