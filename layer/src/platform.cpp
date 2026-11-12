#include "platform.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <chrono>
#include <thread>
#include <fstream>
#include <mutex>
#include <cstring>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN  // (bdex_capture defines it for the whole target)
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shellapi.h>
#include <tlhelp32.h>
#else
#include <cerrno>
#include <csignal>
#include <dirent.h>
#include <dlfcn.h>
#include <fstream>
#include <sys/stat.h>
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

namespace {
struct EnvCopy {
    std::vector<std::string> entries;  // "KEY=VALUE"
    std::vector<char*> list;           // the entries, then nullptr
};
std::mutex g_envMutex;
EnvCopy* g_env = nullptr;         // (never freed: its strings are handed out)
std::vector<EnvCopy*> g_envOld;   // platformEnvForget: earlier copies, for the pointers given from them

const EnvCopy& envCopy() {
    std::lock_guard<std::mutex> lock(g_envMutex);
    if (!g_env) {
        auto* copy = new EnvCopy;
#ifdef _WIN32
        // The process's own block, not the CRT's narrow copy (_environ): in a
        // program with a wide entry point the CRT builds that copy at the
        // first getenv only, and this DLL may be the first to ask. In the
        // ANSI code page, as getenv gave it ("=C:=..." entries: the shell's
        // per-drive directories, not variables).
        if (wchar_t* block = GetEnvironmentStringsW()) {
            for (const wchar_t* w = block; *w; w += wcslen(w) + 1) {
                if (*w == L'=') continue;
                const int n = WideCharToMultiByte(CP_ACP, 0, w, -1, nullptr, 0, nullptr, nullptr);
                if (n <= 1) continue;
                std::string entry(size_t(n), '\0');
                WideCharToMultiByte(CP_ACP, 0, w, -1, entry.data(), n, nullptr, nullptr);
                entry.resize(size_t(n) - 1);
                copy->entries.push_back(std::move(entry));
            }
            FreeEnvironmentStringsW(block);
        }
#else
        for (char** e = environ; e && *e; ++e) copy->entries.emplace_back(*e);  // (declared at file scope above)
#endif
        for (std::string& entry : copy->entries) copy->list.push_back(entry.data());
        copy->list.push_back(nullptr);
        g_env = copy;
    }
    return *g_env;
}
} // namespace

const char* platformEnv(const char* name) {
    const size_t n = strlen(name);
    for (const std::string& entry : envCopy().entries) {
#ifdef _WIN32
        // (Windows' variable names are not case-sensitive)
        const bool same = entry.size() > n && entry[n] == '=' && _strnicmp(entry.c_str(), name, n) == 0;
#else
        const bool same = entry.size() > n && entry[n] == '=' && entry.compare(0, n, name) == 0;
#endif
        if (same) return entry.c_str() + n + 1;
    }
    return nullptr;
}

char** platformEnviron() {
    // (the copy lives as long as the process: see g_env)
    return const_cast<char**>(envCopy().list.data());
}

void platformEnvForget() {
    std::lock_guard<std::mutex> lock(g_envMutex);
    if (g_env) g_envOld.push_back(g_env);
    g_env = nullptr;
}

std::string platformConfigPath() {
#ifdef _WIN32
    const char* appdata = platformEnv("APPDATA");
    if (appdata && *appdata) return std::string(appdata) + "\\bdex-framegen\\bdex-framegen.conf";
    return "";
#else
    if (const char* x = platformEnv("XDG_CONFIG_HOME")) return std::string(x) + "/bdex-framegen.conf";
    if (const char* h = platformEnv("HOME")) return std::string(h) + "/.config/bdex-framegen.conf";
    return "";
#endif
}

std::string platformBundledProfilesPath() {
    if (const char* p = platformEnv("BDEX_FG_PROFILES")) return p;  // "" disables them
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
    // <prefix>/share/bdex-framegen/profiles.conf — or one level deeper for a
    // distribution's library dir (Debian: lib/x86_64-linux-gnu).
    Dl_info info{};
    if (!dladdr(reinterpret_cast<void*>(&platformBundledProfilesPath), &info) || !info.dli_fname) return "";
    std::string lib = info.dli_fname;
    size_t slash = lib.rfind('/');
    if (slash == std::string::npos) return "";
    const std::string dir = lib.substr(0, slash);
    const std::string near = dir + "/../share/bdex-framegen/profiles.conf";
    const std::string deep = dir + "/../../share/bdex-framegen/profiles.conf";
    if (access(near.c_str(), R_OK) != 0 && access(deep.c_str(), R_OK) == 0) return deep;
    return near;
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
            if (const char* h = platformEnv("HOME")) out += h;
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
            const char* v = name.empty() ? nullptr : platformEnv(name.c_str());
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
    if (const char* p = platformEnv("BDEX_FG_STATUS_DIR")) return p;
#ifdef _WIN32
    const char* tmp = platformEnv("TEMP");
    if (tmp && *tmp) return std::string(tmp) + "\\bdex-framegen";
    return "";
#else
    // In the home, not $XDG_RUNTIME_DIR: Steam's container (pressure-vessel,
    // every Proton game) shares the home but only a few sockets of the
    // runtime directory, and the control panel must see the games' status
    // files and they its session file.
    if (const char* h = platformEnv("HOME"); h && *h) return std::string(h) + "/.cache/bdex-framegen/run";
    // No home: /tmp, where any user can create this name first and plant a
    // session file, or symbolic links our writes would follow. Ours alone,
    // or no status files.
    const std::string dir = "/tmp/bdex-framegen-" + std::to_string(static_cast<long>(getuid()));
    mkdir(dir.c_str(), 0700);
    struct stat st;
    if (lstat(dir.c_str(), &st) != 0 || !S_ISDIR(st.st_mode) || st.st_uid != getuid() || (st.st_mode & 077)) return "";
    return dir;
#endif
}

AppSession platformAppSession() {
    AppSession s;
    const std::string dir = platformStatusDir();
    if (dir.empty()) return s;
#ifdef _WIN32
    std::ifstream in(dir + "\\app.session");
#else
    std::ifstream in(dir + "/app.session");
#endif
    long panel = 0, target = 0;
    if (!(in >> panel) || panel <= 0) return s;
    if (!(in >> target)) target = 0;
    double hz = 0.0;
    if (!(in >> hz) || !(hz > 0.0 && hz < 1000.0)) hz = 0.0;
    // The panel rewrites the file every second: a stale one means it is gone
    // (crashed or killed). A heartbeat rather than a process lookup, which a
    // game in Steam's container could not do on the panel's process.
    std::error_code ec;
#ifdef _WIN32
    const auto written = std::filesystem::last_write_time(dir + "\\app.session", ec);
#else
    const auto written = std::filesystem::last_write_time(dir + "/app.session", ec);
#endif
    if (ec) return s;
    s.open = std::filesystem::file_time_type::clock::now() - written < std::chrono::seconds(5);
    if (s.open) {
        s.target = target;
        s.displayHz = hz;
    }
    return s;
}

std::string platformExeName() {
#ifdef _WIN32
    wchar_t exe[MAX_PATH];
    return GetModuleFileNameW(nullptr, exe, MAX_PATH) ? normalizeName(narrow(exe)) : std::string();
#else
    char exe[512];
    const ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (n <= 0) return "";
    exe[n] = '\0';
    return normalizeName(exe);
#endif
}

bool platformIsDesktopApp() {
#ifdef _WIN32
    return false;
#else
    // GTK 4 draws its windows with Vulkan: every GTK 4 application loads the
    // layer, and no game links it.
    std::ifstream in("/proc/self/maps");
    std::string line;
    while (std::getline(in, line))
        if (line.find("libgtk-4.so") != std::string::npos) return true;
    return false;
#endif
}

bool platformIsNonGame(const std::string& name) {
    // Keep in sync with NOT_GAMES in tools/bdex-framegen-gui.
    static const char* const kNonGames[] = {
        "steam", "steamwebhelper", "gamescope", "gamescopereaper", "gamescope-wl", "mangoapp",
        "steam-runtime-check-requirements", "chrome", "chromium",
        "firefox", "librewolf", "brave", "zen", "electron", "discord", "vesktop", "code",
        "obs", "vulkaninfo", "vkcube", "bdex-framegen-gui", "bdex-capture", "bdex_capture",
        "zed-editor", "zeditor",  // the Zed editor draws with Vulkan (wgpu)
        "explorer", "xalia",  // Wine's desktop and accessibility helpers (Proton: next to every game)
        // Steam's shader pre-caching: it replays a game's pipelines into the
        // driver's cache, a device without a window, run by the hundred
        // (555 status files in six days, 10/11, never removed: Steam kills
        // them). What the layer adds to a device (a queue, present_id /
        // present_wait, a queue priority) does not change how a pipeline is
        // compiled, so the cache it fills still matches the game's.
        "fossilize_replay",
    };
    for (const char* n : kNonGames)
        if (name == n) return true;
    return false;
}

void platformFineTimer(bool on) {
#ifdef _WIN32
    using Period = UINT(WINAPI*)(UINT);
    static const HMODULE winmm = LoadLibraryW(L"winmm.dll");
    if (!winmm) return;
    auto fn = reinterpret_cast<Period>(
        reinterpret_cast<void*>(GetProcAddress(winmm, on ? "timeBeginPeriod" : "timeEndPeriod")));
    if (fn) fn(1);
#else
    (void)on;
#endif
}

#ifdef _WIN32
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION  // (Windows 10 1803 SDKs and later; older MinGW headers)
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif
namespace {
// One timer per thread (a waitable timer is a kernel object: never shared
// between two sleeping threads), closed when the thread ends.
struct ThreadTimer {
    HANDLE handle = nullptr;
    ThreadTimer() {
        handle = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
        // (Before Windows 10 1803 the flag is refused: a plain timer, as
        // precise as the timer resolution — platformFineTimer raises it.)
        if (!handle) handle = CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);
    }
    ~ThreadTimer() {
        if (handle) CloseHandle(handle);
    }
    ThreadTimer(const ThreadTimer&) = delete;
    ThreadTimer& operator=(const ThreadTimer&) = delete;
};
} // namespace
#endif

void platformSleepUntil(std::chrono::steady_clock::time_point until) {
#ifdef _WIN32
    using namespace std::chrono;
    thread_local ThreadTimer timer;
    for (;;) {
        const auto left = until - steady_clock::now();
        if (left <= steady_clock::duration::zero()) return;
        LARGE_INTEGER due;
        due.QuadPart = -std::max<LONGLONG>(1, LONGLONG(duration_cast<nanoseconds>(left).count() / 100));  // 100 ns units, relative
        if (timer.handle && SetWaitableTimer(timer.handle, &due, 0, nullptr, nullptr, FALSE)) {
            WaitForSingleObject(timer.handle, INFINITE);
        } else {
            const auto ms = duration_cast<milliseconds>(left).count();
            if (ms > 0) Sleep(DWORD(ms));
            else SwitchToThread();
        }
    }
#else
    std::this_thread::sleep_until(until);
#endif
}

std::string platformCacheDir() {
    if (const char* p = platformEnv("BDEX_FG_CACHE_DIR")) return p;
#ifdef _WIN32
    const char* app = platformEnv("LOCALAPPDATA");
    if (app && *app) return std::string(app) + "\\bdex-framegen";
    return "";
#else
    if (const char* x = platformEnv("XDG_CACHE_HOME"); x && *x) return std::string(x) + "/bdex-framegen";
    if (const char* h = platformEnv("HOME"); h && *h) return std::string(h) + "/.cache/bdex-framegen";
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

bool platformProcessAlive(long pid) {
    if (pid <= 0) return false;
#ifdef _WIN32
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(pid));
    if (!h) return GetLastError() != ERROR_INVALID_PARAMETER;  // (no such process; access denied: alive)
    DWORD code = 0;
    const bool alive = !GetExitCodeProcess(h, &code) || code == STILL_ACTIVE;
    CloseHandle(h);
    return alive;
#else
    return kill(static_cast<pid_t>(pid), 0) == 0 || errno != ESRCH;
#endif
}

bool platformAntiCheatException(const std::string& name) {
#ifdef _WIN32
    return name == "rocketleague";
#else
    (void)name;
    return false;
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
    const std::string running = platformAntiCheatRunning();
    if (!running.empty() && foundName) *foundName = running;
    return !running.empty();
}

int platformOnBattery() {
    // (Tests: BDEX_POWER=battery|mains.)
    if (const char* v = platformEnv("BDEX_POWER"); v && *v) return v[0] == 'b' ? 1 : 0;
#ifdef _WIN32
    SYSTEM_POWER_STATUS st;
    if (!GetSystemPowerStatus(&st)) return -1;
    if (st.BatteryFlag == 128 /* no system battery */ || st.ACLineStatus == 255) return -1;
    return st.ACLineStatus == 0 ? 1 : 0;
#else
    // On mains when any "Mains" / "USB" supply is online; on battery when a
    // battery reports Discharging and nothing is online.
    std::error_code ec;
    bool battery = false, mains = false, sawMains = false;
    for (const auto& e : std::filesystem::directory_iterator("/sys/class/power_supply", ec)) {
        auto read = [&](const char* f) {
            std::ifstream in(e.path() / f);
            std::string v;
            std::getline(in, v);
            return v;
        };
        const std::string type = read("type");
        if (type == "Mains" || type == "USB") {
            sawMains = true;
            if (read("online") == "1") mains = true;
        } else if (type == "Battery" && read("scope") != "Device") {  // (not a mouse's battery)
            if (read("status") == "Discharging") battery = true;
        }
    }
    if (mains) return 0;
    if (battery) return 1;
    return sawMains ? 0 : -1;
#endif
}

double platformPerformanceFrequency() {
#ifdef _WIN32
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    return double(f.QuadPart);
#else
    return 1e9;
#endif
}

bool platformIsSteamDeck() {
#ifdef _WIN32
    return false;
#else
    std::ifstream in("/sys/class/dmi/id/board_name");
    std::string v;
    std::getline(in, v);
    return v == "Jupiter" || v == "Galileo";
#endif
}

std::string platformAntiCheatRunning() {
#ifdef _WIN32
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return "";
    std::string found;
    PROCESSENTRY32W pe;
    pe.dwSize = sizeof(pe);
    if (Process32FirstW(snap, &pe)) {
        do {
            std::string n = normalizeName(narrow(pe.szExeFile));
            if (isDeniedAntiCheat(n)) {
                found = n;
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
    if (!proc) return "";
    std::string found;
    while (dirent* e = readdir(proc)) {
        if (!std::isdigit(static_cast<unsigned char>(e->d_name[0]))) continue;
        char link[16 + 256], exe[512];
        snprintf(link, sizeof(link), "/proc/%s/exe", e->d_name);
        ssize_t n = readlink(link, exe, sizeof(exe) - 1);
        if (n <= 0) continue;
        exe[n] = '\0';
        std::string name = normalizeName(exe);
        if (isDeniedAntiCheat(name)) {
            found = name;
            break;
        }
    }
    closedir(proc);
    return found;
#endif
}

} // namespace bdex
