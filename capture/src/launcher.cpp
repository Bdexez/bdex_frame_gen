// bdex_capture's native launcher — see launcher.h. Win32 only, no toolkit:
// hand-placed controls in one window, IsDialogMessage for keyboard
// navigation. The options persist to the shared config file's [*] section
// with the same key names the Linux control panel
// (tools/bdex-framegen-gui) writes, so the two panels stay interchangeable.
#include "launcher.h"
#include "injection.h"
#include "report.h"

#include "log.h"
#include "platform.h"
#include "options_ui.h"
#include "options_ui_conf.h"
#include "options_ui_en_conf.h"
#include "status.h"

#include <windows.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <vulkan/vulkan.h>

#include <algorithm>
#include <cctype>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <cstdio>
#include <memory>
#include <cstdlib>
#include <cwchar>
#include <cwctype>
#include <cstring>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

namespace bdex {

// The launcher's language: French when Windows' display language is
// (GetUserDefaultUILanguage), English otherwise; BDEX_LANG=fr|en chooses.
bool launcherFrench() {
    static const bool french = [] {
        if (const char* v = std::getenv("BDEX_LANG"); v && *v) return (v[0] == 'f' || v[0] == 'F');
        return PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_FRENCH;
    }();
    return french;
}
template <typename C> static const C* T(const C* fr, const C* en) { return launcherFrench() ? fr : en; }

// ------------------------------------------------------- window picking --

std::wstring wideLower(std::wstring s) {
    for (auto& c : s) c = static_cast<wchar_t>(towlower(c));
    return s;
}

std::string narrow(const std::wstring& w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return {};
    std::string s(static_cast<size_t>(n - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, s.data(), n, nullptr, nullptr);
    return s;
}

static std::wstring wideFromAcp(const char* s) {
    int n = MultiByteToWideChar(CP_ACP, 0, s, -1, nullptr, 0);
    std::wstring w(static_cast<size_t>(std::max(n - 1, 0)), L'\0');
    if (n > 1) MultiByteToWideChar(CP_ACP, 0, s, -1, w.data(), n);
    return w;
}

std::wstring processName(DWORD pid) {
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return L"?";
    wchar_t buf[MAX_PATH];
    DWORD n = MAX_PATH;
    std::wstring name = L"?";
    if (QueryFullProcessImageNameW(h, 0, buf, &n)) {
        name = buf;
        size_t p = name.find_last_of(L"\\/");
        if (p != std::wstring::npos) name = name.substr(p + 1);
    }
    CloseHandle(h);
    return name;
}

std::vector<WindowInfo> capturableWindows() {
    std::vector<WindowInfo> out;
    EnumWindows(
        [](HWND h, LPARAM lp) -> BOOL {
            auto* v = reinterpret_cast<std::vector<WindowInfo>*>(lp);
            if (!IsWindowVisible(h) || GetWindowTextLengthW(h) == 0) return TRUE;
            if (GetAncestor(h, GA_ROOT) != h) return TRUE;
            DWORD cloaked = 0;
            DwmGetWindowAttribute(h, DWMWA_CLOAKED, &cloaked, sizeof cloaked);
            if (cloaked) return TRUE;
            wchar_t title[256];
            GetWindowTextW(h, title, 256);
            DWORD pid = 0;
            GetWindowThreadProcessId(h, &pid);
            if (pid == GetCurrentProcessId()) return TRUE;
            v->push_back({h, pid, title});
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&out));
    return out;
}

// ---------------------------------------------------- config file access --
// The layer's config file is a small INI: `key = value` lines, a [name]
// section restricts the keys that follow to that game, [*] (or the preamble)
// is global. The launcher owns a handful of global keys; everything else —
// hand-written tuning, game sections — must survive a save.

static std::wstring configPathW() {
    if (const char* p = getenv("BDEX_FG_CONFIG")) return wideFromAcp(p);
    return wideFromAcp(platformConfigPath().c_str());
}

static std::string trim(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return s.substr(b, e - b);
}

static std::string lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// "  Key = value  " -> true and key set (lower-cased), when it is a key line.
static bool keyOf(const std::string& raw, std::string& key) {
    const std::string t = trim(raw);
    const size_t eq = t.find('=');
    if (t.empty() || t[0] == '#' || t[0] == ';' || eq == std::string::npos) return false;
    key = lower(trim(t.substr(0, eq)));
    return !key.empty();
}

// "[ name ]" -> true; section receives the trimmed inner name ("" for [*]).
static bool headerOf(const std::string& raw, std::string& section) {
    const std::string t = trim(raw);
    if (t.size() < 2 || t.front() != '[' || t.back() != ']') return false;
    section = trim(t.substr(1, t.size() - 2));
    if (section == "*") section.clear();
    return true;
}

static bool readLines(const std::wstring& path, std::vector<std::string>& lines) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD size = GetFileSize(f, nullptr);
    std::string data;
    if (size != INVALID_FILE_SIZE) {
        data.resize(size);
        DWORD got = 0;
        if (size && !ReadFile(f, data.data(), size, &got, nullptr)) got = 0;
        data.resize(got);
    }
    CloseHandle(f);
    lines.clear();
    size_t pos = 0;
    while (pos <= data.size()) {
        size_t end = data.find('\n', pos);
        if (end == std::string::npos) end = data.size();
        std::string line = data.substr(pos, end - pos);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        lines.push_back(line);
        pos = end + 1;
    }
    return true;
}

// Atomic replace so a crash mid-save cannot leave a truncated config.
static bool writeLines(const std::wstring& path, const std::vector<std::string>& lines) {
    const size_t slash = path.find_last_of(L"\\/");
    if (slash != std::wstring::npos)
        CreateDirectoryW(path.substr(0, slash).c_str(), nullptr);  // exists: fine
    const std::wstring tmp = path + L".tmp";
    HANDLE f = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    std::string data;
    for (const auto& l : lines) {
        data += l;
        data += '\n';
    }
    DWORD put = 0;
    const bool ok = data.empty() ||
                    (WriteFile(f, data.data(), static_cast<DWORD>(data.size()), &put, nullptr) &&
                     put == data.size());
    CloseHandle(f);
    return ok && MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
}

std::string globalConfigValue(const std::string& want) {
    const std::wstring path = configPathW();
    if (path.empty()) return "";
    std::vector<std::string> lines;
    if (!readLines(path, lines)) return "";
    bool global = true;  // preamble before any header is global too
    for (const auto& raw : lines) {
        std::string section;
        if (headerOf(raw, section)) {
            global = section.empty();
            continue;
        }
        std::string key;
        if (global && keyOf(raw, key) && key == lower(trim(want)))
            return lower(trim(raw.substr(raw.find('=') + 1)));  // keyword values, case-insensitive
    }
    return "";
}

// Canonical form of a section name for matching: lower-cased, no `.exe`
// (same reduction as Config::sectionMatches, so a [Game] / [game.exe] header
// and a target executable "Game.exe" all resolve to the same key).
static std::string sectionKey(std::string s) {
    s = lower(trim(s));
    if (s.size() > 4 && s.compare(s.size() - 4, 4, ".exe") == 0) s.resize(s.size() - 4);
    return s;
}

// Section header to write for a game window: the target's executable name
// without the .exe suffix, original case kept for readability.
static std::string sectionForPid(DWORD pid) {
    std::string s = narrow(processName(pid));
    const size_t p = s.find_last_of("\\/");
    if (p != std::string::npos) s = s.substr(p + 1);
    if (s.size() > 4 && lower(s.substr(s.size() - 4)) == ".exe") s.resize(s.size() - 4);
    return s;
}

// Every key = value pair in one scope of the config file: `section` empty is
// the global scope (preamble + [*]), otherwise the matching [section]. Keys
// come back lower-cased, values keep their case; later duplicates win.
static std::vector<std::pair<std::string, std::string>> readScope(const std::string& want) {
    std::vector<std::pair<std::string, std::string>> out;
    const std::wstring path = configPathW();
    if (path.empty()) return out;
    std::vector<std::string> lines;
    if (!readLines(path, lines)) return out;
    const std::string target = sectionKey(want);
    bool active = target.empty();  // preamble before any header is global
    for (const auto& raw : lines) {
        std::string section;
        if (headerOf(raw, section)) {
            active = target.empty() ? section.empty() : sectionKey(section) == target;
            continue;
        }
        std::string key;
        if (active && keyOf(raw, key))
            out.emplace_back(key, trim(raw.substr(raw.find('=') + 1)));
    }
    return out;
}

// Keys the launcher manages in the global scope; a stale one not present in
// the new set is removed from the file (preset / present_mode = "auto").
// The launcher's "Options avancées" column: every engine option that has an
// effect in the capture app (layer-only ones — game fps limit, dynamic
// multiplier, pause, present-extension compatibility — do not apply to a
// captured window). Values are passed to Config::apply through the config
// file, so the engine reads them exactly as on Linux.
enum ExtraKind { kCheck, kNumber, kChoice };
struct ExtraOpt {
    std::string key;            // empty: a section header
    std::wstring label;
    ExtraKind kind = kCheck;
    std::string def;            // config-string default (Config's, or options_ui.conf's)
    std::vector<std::wstring> choiceLabels;
    std::vector<std::string> choiceValues;
    int choices = 0;
};

// Options with a control of their own in the left column.
const char* const kLeftColumn[] = {"grad_weight", "scene_cut_low", "scene_cut_high", "smoothness",
                                   "zero_bias",   "search_fine",   "flow_iterations"};

static std::wstring wideUtf8(const std::string& s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(static_cast<size_t>(std::max(n - 1, 0)), L'\0');
    if (n > 1) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
    return w;
}

// The column, built once from layer/options_ui.conf (embedded at build time,
// the same file the GTK panel is built from) and the engine's option
// registry: the "launcher" sections and options, in order.
const std::vector<ExtraOpt>& extraOptions();

// Window layout, from the column's content (rows of 22 px, headers 20 px).
struct Layout {
    int columnGroupH;   // the "Options avancées" group (the method and GPU rows at its top)
    int buttonsY;
    int clientH;
};
Layout launcherLayout() {
    int h = 48 + 56 + 30 + 30 + 66;  // (the method and GPU rows, the injections and report buttons, the calibration)
    for (const ExtraOpt& o : extraOptions()) h += o.key.empty() ? 20 : 22;
    Layout l;
    l.columnGroupH = std::max(770, h + 4);
    // The left column (and "En cours" under it) ends at 910.
    l.buttonsY = std::max(8 + l.columnGroupH, 910) + 10;
    l.clientH = l.buttonsY + 26 + 12;
    return l;
}

const std::vector<ExtraOpt>& extraOptions() {
    static const std::vector<ExtraOpt> opts = [] {
        std::vector<ExtraOpt> out;
        std::vector<std::string> errors;
        auto sections = parseOptionsUi(reinterpret_cast<const char*>(kOptionsUiConf), errors);
        applyOptionsUiEnglish(reinterpret_cast<const char*>(kOptionsUiEnConf), sections, errors);
        localizeOptionsUi(sections, !launcherFrench());
        for (const auto& e : errors) LOGW("%s", e.c_str());
        for (const auto& sec : sections) {
            if (!sec.launcher) continue;
            bool header = false;
            for (const auto& o : sec.options) {
                if (!o.launcher || o.kind == "text") continue;
                if (std::find_if(std::begin(kLeftColumn), std::end(kLeftColumn),
                                 [&](const char* k) { return o.key == k; }) != std::end(kLeftColumn))
                    continue;
                if (!header) {
                    ExtraOpt h;
                    h.label = wideUtf8(sec.title);
                    out.push_back(h);
                    header = true;
                }
                ExtraOpt e;
                e.key = o.key;
                e.def = o.def;
                e.label = wideUtf8(o.label);
                if (o.kind == "switch") {
                    e.kind = kCheck;
                } else if (o.kind == "choice") {
                    e.kind = kChoice;
                    for (const auto& [value, label] : o.choices) {
                        e.choiceValues.push_back(value);
                        e.choiceLabels.push_back(wideUtf8(label));
                    }
                    e.choices = static_cast<int>(e.choiceValues.size());
                } else {
                    e.kind = kNumber;
                    char range[64];  // the accepted range, French decimal comma
                    std::snprintf(range, sizeof range, " (%g–%g)", o.lo, o.hi);
                    std::string r = range;
                    for (auto& ch : r)
                        if (ch == '.' && launcherFrench()) ch = ',';
                    e.label += wideUtf8(r);
                }
                out.push_back(e);
            }
        }
        return out;
    }();
    return opts;
}

// Canonical config string of an extra option (numbers without trailing zeros,
// booleans as 0/1, choices lower-case); the default when unparsable.
static std::string normalizeExtra(const ExtraOpt& o, std::string v) {
    v = trim(v);
    for (auto& ch : v) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    if (o.kind == kCheck) return (v == "1" || v == "true" || v == "on" || v == "yes") ? "1" : "0";
    if (o.kind == kChoice) {
        for (int i = 0; i < o.choices; ++i)
            if (v == o.choiceValues[size_t(i)]) return v;
        return o.def;
    }
    char* end = nullptr;
    const double d = std::strtod(v.c_str(), &end);
    if (!end || end == v.c_str()) return o.def;
    char buf[32];
    std::snprintf(buf, sizeof buf, "%g", d);
    return buf;
}

static bool managedKey(const std::string& key) {
    for (const ExtraOpt& o : extraOptions())
        if (!o.key.empty() && key == o.key) return true;
    static const char* kManaged[] = {"preset",        "multiplier",  "mode",      "present_mode",
                                     "upscale_filter", "sharpness",  "overlay",   "hud",
                                     "grad_weight",   "smoothness",  "zero_bias", "search_fine",
                                     "flow_iterations", "scene_cut_low", "scene_cut_high"};
    for (const char* m : kManaged)
        if (key == m) return true;
    return false;
}

// Rewrites the global scope ([*] block, or the preamble) with `pairs`,
// preserving every other line: comments, unknown keys and all [game]
// sections, including their own copies of the managed keys.
static bool saveGlobalConfig(const std::vector<std::pair<std::string, std::string>>& pairs) {
    const std::wstring path = configPathW();
    if (path.empty()) return false;
    std::vector<std::string> old;
    readLines(path, old);  // a missing file is fine — we create it

    std::vector<std::string> out;
    bool inGlobal = true;   // preamble before the first header
    bool inserted = false;  // our pairs went in
    auto insertPairs = [&] {
        for (const auto& kv : pairs) out.push_back(kv.first + " = " + kv.second);
        inserted = true;
    };
    for (const auto& raw : old) {
        std::string section;
        if (headerOf(raw, section)) {
            if (section.empty()) {  // the [*] block: drop old values, add ours
                out.push_back(raw);
                if (!inserted) insertPairs();
                inGlobal = true;
                continue;
            }
            if (!inserted) {  // first game section and no [*] yet: create one
                out.push_back("[*]");
                insertPairs();
                out.emplace_back();
            }
            inGlobal = false;
            out.push_back(raw);
            continue;
        }
        std::string key;
        if (inGlobal && keyOf(raw, key) && managedKey(key)) continue;  // stale value
        out.push_back(raw);
    }
    if (!inserted) {
        if (out.empty())
            out.push_back("# bdex-framegen — réglages bdex_capture (lanceur Windows)");
        out.push_back("[*]");
        insertPairs();
    }
    return writeLines(path, out);
}

// Writes `pairs` into the [section] scope (a per-game profile), rewriting only
// that section's managed keys and preserving everything else — the global
// scope, comments, other games' sections, and any hand-written keys in this
// section that the launcher does not manage. Creates the section at the end of
// the file when it does not exist yet.
static bool savePerGameConfig(const std::string& section,
                              const std::vector<std::pair<std::string, std::string>>& pairs) {
    const std::wstring path = configPathW();
    if (path.empty() || sectionKey(section).empty()) return false;
    std::vector<std::string> old;
    readLines(path, old);  // a missing file is fine — we create it

    const std::string want = sectionKey(section);
    std::vector<std::string> out;
    bool inTarget = false;
    bool inserted = false;
    auto insertPairs = [&] {
        for (const auto& kv : pairs) out.push_back(kv.first + " = " + kv.second);
        inserted = true;
    };
    for (const auto& raw : old) {
        std::string sec;
        if (headerOf(raw, sec)) {
            inTarget = sectionKey(sec) == want;
            out.push_back(raw);
            if (inTarget && !inserted) insertPairs();  // our values first in the section
            continue;
        }
        std::string key;
        if (inTarget && keyOf(raw, key) && managedKey(key)) continue;  // drop stale managed value
        out.push_back(raw);
    }
    if (!inserted) {  // no such section yet: append it
        if (!out.empty() && !trim(out.back()).empty()) out.emplace_back();
        out.push_back("[" + section + "]");
        insertPairs();
    }
    return writeLines(path, out);
}

// ------------------------------------------------------------ the window --

namespace {

enum {
    // 100+: Windows reserves 0..10 for IDOK/IDCANCEL/…
    ID_LB = 100,
    ID_REFRESH,
    ID_PERGAME,
    ID_GEN,
    ID_PRESET,
    ID_MULT,
    ID_MODE,
    ID_SIZE,
    ID_SCALE,
    ID_FILTER,
    ID_SHARP,
    ID_FIFO,
    ID_HUD,
    ID_GRAD,
    ID_SCLOW,
    ID_SCHIGH,
    ID_SMOOTH,
    ID_ZBIAS,
    ID_SFINE,
    ID_FITER,
    ID_RUNNING,
    ID_METHOD,
    ID_GPU,
    ID_UNINJECT,
    ID_REPORT,
    ID_CAL_RUN,
    ID_CAL_APPLY,
    ID_TIMER_RUNNING = 1,
};

const wchar_t* const kPresets[] = {L"Auto", T(L"Qualité", L"Quality"), T(L"Équilibré", L"Balanced"), T(L"Amélioré (options mesurées)", L"Enhanced (measured options)"), L"Performance"};
const char* const kPresetVals[] = {"", "quality", "balanced", "enhanced", "performance"};
const wchar_t* const kFilters[] = {T(L"Lanczos (le plus net)", L"Lanczos (sharpest)"), T(L"Bicubique", L"Bicubic"), T(L"Bilinéaire", L"Bilinear")};
const int kFilterVals[] = {2, 1, 0};
const wchar_t* const kHud[] = {T(L"Désactivé", L"Off"), L"FPS", T(L"FPS + génération", L"FPS + generation"), L"+ 1% low",
                               T(L"+ 1% et 0.1% low", L"+ 1% and 0.1% low")};

struct Ui {
    LaunchChoice* choice;
    LaunchChoice baseline;   // the global options, restored when a game with no profile is picked
    std::vector<WindowInfo> windows;
    HWND lb = nullptr, perGame = nullptr, gen = nullptr, preset = nullptr, mult = nullptr, mode = nullptr;
    HWND size = nullptr, scaleEd = nullptr, filter = nullptr, sharpEd = nullptr;
    HWND fifo = nullptr, hud = nullptr;
    HWND method = nullptr, gpu = nullptr;
    HWND calLabel = nullptr, calRun = nullptr, calApply = nullptr;
    std::vector<std::string> gpuNames;  // the GPU combo's entries after « Auto »
    HWND grad = nullptr, scLow = nullptr, scHigh = nullptr, smooth = nullptr;
    HWND zbias = nullptr, sfine = nullptr, fiter = nullptr;
    std::vector<HWND> extra;  // one control per extraOptions() entry (nullptr for headers)
    HWND running = nullptr;   // "En cours": the live status files
    std::wstring runningText;
};

static std::wstring wideFromUtf8(const std::string& s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(static_cast<size_t>(std::max(n - 1, 0)), L'\0');
    if (n > 1) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
    return w;
}

// A process that still runs (a status file outlives a crashed game).
static bool processAlive(long pid) {
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(pid));
    if (!h) return false;
    DWORD code = 0;
    const bool alive = GetExitCodeProcess(h, &code) && code == STILL_ACTIVE;
    CloseHandle(h);
    return alive;
}

// "En cours": what the layer (Vulkan games) and other bdex_capture instances
// publish (layer/src/status.h), refreshed every second while the launcher is
// open — the same files as the GTK panel's view on Linux.
void refreshRunning(Ui& ui) {
    std::string text;
    std::error_code ec;
    const std::string dir = platformStatusDir();
    const long long now = static_cast<long long>(std::time(nullptr));
    if (!dir.empty())
        for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
            if (entry.path().extension() != ".status") continue;
            std::ifstream in(entry.path());
            std::stringstream buf;
            buf << in.rdbuf();
            const StatusFile f = statusParse(buf.str());
            if (f.pid <= 0 || !processAlive(f.pid) || now - f.time > 30) continue;
            text += f.process + " (" + std::to_string(f.pid) + ")\r\n";
            const auto lines = statusDescribe(f, !launcherFrench());
            if (lines.empty()) text += T("   couche chargée\r\n", "   layer loaded\r\n");
            for (const auto& l : lines) text += "   " + l + "\r\n";
        }
    if (text.empty()) text = T("Aucun jeu en cours avec bdex-framegen (couche Vulkan ou capture).", "No game running with bdex-framegen (Vulkan layer or capture).");
    const std::wstring w = wideFromUtf8(text);
    if (w != ui.runningText) {  // no flicker nor lost scroll position when nothing changed
        ui.runningText = w;
        SetWindowTextW(ui.running, w.c_str());
    }
}

int sel(HWND combo) {
    const int i = static_cast<int>(SendMessageW(combo, CB_GETCURSEL, 0, 0));
    return std::max(0, i);
}

int readInt(HWND edit, int fallback) {
    wchar_t buf[32] = {};
    GetWindowTextW(edit, buf, 32);
    wchar_t* end = nullptr;
    const long v = wcstol(buf, &end, 10);
    return end && end != buf ? static_cast<int>(v) : fallback;
}

float readFloat(HWND edit, float fallback) {
    wchar_t buf[32] = {};
    GetWindowTextW(edit, buf, 32);
    wchar_t* end = nullptr;
    const float v = wcstof(buf, &end);
    return end && end != buf ? v : fallback;
}

// Formats a number the way the config file expects it (no trailing zeros).
void setNum(HWND edit, double value) {
    wchar_t buf[32];
    swprintf(buf, 32, L"%g", value);
    SetWindowTextW(edit, buf);
}

void updateEnabled(Ui& ui) {
    const bool on = SendMessageW(ui.gen, BM_GETCHECK, 0, 0) == BST_CHECKED;
    EnableWindow(ui.preset, on);
    EnableWindow(ui.mult, on);
    EnableWindow(ui.mode, on);
    const bool custom = sel(ui.size) == 2;
    EnableWindow(ui.scaleEd, custom);
}

void refreshList(Ui& ui) {
    HWND keep = nullptr;
    const int was = static_cast<int>(SendMessageW(ui.lb, LB_GETCURSEL, 0, 0));
    if (was >= 0 && was < static_cast<int>(ui.windows.size())) keep = ui.windows[was].hwnd;
    ui.windows = capturableWindows();
    SendMessageW(ui.lb, LB_RESETCONTENT, 0, 0);
    for (const auto& w : ui.windows) {
        std::wstring item = w.title;
        if (item.size() > 70) item = item.substr(0, 69) + L"…";
        item += L" — " + processName(w.pid);
        SendMessageW(ui.lb, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(item.c_str()));
    }
    if (keep)
        for (size_t i = 0; i < ui.windows.size(); ++i)
            if (ui.windows[i].hwnd == keep) SendMessageW(ui.lb, LB_SETCURSEL, i, 0);
}

void fillFromChoice(Ui& ui);  // defined below; onGameSelected refills the panel

// ------------------------------------------------------------ calibration --
// bdex-calibrate.exe (next to this program): what generation costs on this
// GPU at the main screen's size and refresh — measured by itself at the
// first start (in a thread: the window stays usable), again with « Mesurer »;
// « Appliquer » takes its preset and multiplier.
constexpr UINT kMsgCalibrated = WM_APP + 30;

std::wstring calibrationPath() {
    wchar_t buf[MAX_PATH];
    const DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH);
    if (!n || n >= MAX_PATH) return L"";
    return std::wstring(buf, n) + L"\\bdex-framegen\\calibration.json";
}

std::wstring calibrateExe() {
    wchar_t exe[MAX_PATH];
    const DWORD n = GetModuleFileNameW(nullptr, exe, MAX_PATH);
    if (!n) return L"";
    std::wstring dir(exe, n);
    dir = dir.substr(0, dir.find_last_of(L"\\/") + 1);
    const std::wstring p = dir + L"bdex-calibrate.exe";
    return GetFileAttributesW(p.c_str()) == INVALID_FILE_ATTRIBUTES ? L"" : p;
}

struct Calibration {
    bool ok = false;
    std::string gpu, preset;
    int multiplier = 2, w = 0, h = 0;
    double refresh = 0, costMs = 0;
};

// The few fields the launcher needs, from the tool's small JSON.
Calibration readCalibration() {
    Calibration c;
    std::ifstream in{std::filesystem::path(calibrationPath())};
    const std::string j((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    auto str = [&](const std::string& key, size_t from) {
        const size_t k = j.find("\"" + key + "\"", from);
        if (k == std::string::npos) return std::string();
        const size_t a = j.find('"', j.find(':', k) + 1), b = a == std::string::npos ? a : j.find('"', a + 1);
        return a == std::string::npos || b == std::string::npos ? std::string() : j.substr(a + 1, b - a - 1);
    };
    auto num = [&](const std::string& key, size_t from) {
        const size_t k = j.find("\"" + key + "\"", from);
        return k == std::string::npos ? 0.0 : std::atof(j.c_str() + j.find(':', k) + 1);
    };
    const size_t rec = j.find("\"recommended\"");
    if (rec == std::string::npos) return c;
    c.gpu = str("gpu", 0);
    c.preset = str("preset", rec);
    c.multiplier = static_cast<int>(num("multiplier", rec));
    c.costMs = num("cost_ms", rec);
    c.refresh = num("refresh", 0);
    const size_t size = j.find("\"size\"");
    if (size != std::string::npos) std::sscanf(j.c_str() + j.find('[', size), "[%d, %d]", &c.w, &c.h);
    c.ok = !c.preset.empty() && c.multiplier >= 2;
    return c;
}

std::wstring calibrationText(const Calibration& c) {
    if (!c.ok) return T(L"Calibration du GPU : pas encore mesurée.", L"GPU calibration: not measured yet.");
    wchar_t buf[512];
    swprintf(buf, 512, T(L"%ls à %dx%d, %.0f Hz : profil %ls, jusqu'à x%d (%.1f ms de GPU par image du jeu).",
                         L"%ls at %dx%d, %.0f Hz: %ls preset, up to x%d (%.1f ms of GPU a game frame)."),
             wideUtf8(c.gpu).c_str(), c.w, c.h, c.refresh, wideUtf8(c.preset).c_str(), c.multiplier, c.costMs);
    return buf;
}

// Runs the tool in a thread; the window gets kMsgCalibrated when it is done.
void startCalibration(HWND hwnd) {
    const std::wstring exe = calibrateExe(), out = calibrationPath();
    if (exe.empty() || out.empty()) return;
    DEVMODEW dm{};
    dm.dmSize = sizeof dm;
    EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &dm);
    wchar_t args[512];
    swprintf(args, 512, L"\"%ls\" --size %lux%lu --refresh %lu --json \"%ls\"", exe.c_str(),
             dm.dmPelsWidth ? dm.dmPelsWidth : 1920ul, dm.dmPelsHeight ? dm.dmPelsHeight : 1080ul,
             dm.dmDisplayFrequency > 1 ? dm.dmDisplayFrequency : 60ul, out.c_str());
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(out).parent_path(), ec);
    struct Job { HWND hwnd; std::wstring cmd; };
    auto* job = new Job{hwnd, args};
    HANDLE th = CreateThread(nullptr, 0, [](LPVOID p) -> DWORD {
        std::unique_ptr<Job> j(static_cast<Job*>(p));
        STARTUPINFOW si{sizeof si};
        PROCESS_INFORMATION pi{};
        BOOL ok = CreateProcessW(nullptr, j->cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
        DWORD code = 1;
        if (ok) {
            WaitForSingleObject(pi.hProcess, 300000);
            GetExitCodeProcess(pi.hProcess, &code);
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
        }
        if (IsWindow(j->hwnd)) PostMessageW(j->hwnd, kMsgCalibrated, code == 0, 0);
        return 0;
    }, job, 0, nullptr);
    if (th) CloseHandle(th);
    else delete job;
}

// Overlays a config scope's managed keys onto a LaunchChoice (per-game
// prefill); keys absent from the scope keep their baseline value. Mirrors the
// key names persist() writes and Config::apply() understands.
void applyScopeToChoice(const std::vector<std::pair<std::string, std::string>>& kv, LaunchChoice& c) {
    auto toF = [](const std::string& s, float d) {
        char* e = nullptr;
        const float v = std::strtof(s.c_str(), &e);
        return e && e != s.c_str() ? v : d;
    };
    auto toI = [](const std::string& s, int d) {
        char* e = nullptr;
        const long v = std::strtol(s.c_str(), &e, 10);
        return e && e != s.c_str() ? static_cast<int>(v) : d;
    };
    for (const auto& p : kv) {
        const std::string& k = p.first;
        const std::string v = lower(trim(p.second));
        if (k == "multiplier" || k == "mult") c.multiplier = std::clamp(toI(v, c.multiplier), 1, 8);
        else if (k == "adaptive") c.adaptive = v == "1" || v == "true" || v == "on" || v == "yes";
        else if (k == "mode") {
            c.hybrid = v == "hybrid";
            c.extrapolate = !c.hybrid && v.rfind("interp", 0) != 0;  // else extrapolate
        }
        else if (k == "preset") {
            if (v == "quality" || v == "balanced" || v == "performance") c.preset = v;
            else if (v == "auto" || v.empty()) c.preset.clear();
        } else if (k == "upscale_filter") {
            if (v == "bilinear" || v == "0") c.filter = 0;
            else if (v == "bicubic" || v == "catmull" || v == "catmull-rom" || v == "1") c.filter = 1;
            else c.filter = 2;
        } else if (k == "sharpness" || k == "sharpen") c.sharpness = std::clamp(toF(v, c.sharpness), 0.f, 1.f);
        else if (k == "overlay" || k == "hud") c.hud = std::clamp(toI(v, c.hud), 0, 4);
        else if (k == "present_mode") c.fifo = (v == "fifo" || v == "vsync");
        else if (k == "gpu") c.gpu = (v == "auto" || v == "game") ? std::string() : trim(p.second);
        else if (k == "method")
            c.method = (v == "layer" || v == "injection" || v == "1") ? 1 : (v == "capture" || v == "2") ? 2 : 0;
        else if (k == "grad_weight") c.gradWeight = std::clamp(toF(v, c.gradWeight), 0.f, 8.f);
        else if (k == "smoothness") c.smoothness = std::clamp(toF(v, c.smoothness), 0.f, 1.f);
        else if (k == "zero_bias") c.zeroBias = std::clamp(toF(v, c.zeroBias), 0.f, 1.f);
        else if (k == "search_fine") c.searchFine = std::clamp(toI(v, c.searchFine), 1, 4);
        else if (k == "flow_iterations") c.flowIterations = std::clamp(toI(v, c.flowIterations), 0, 3);
        else if (k == "scene_cut_low") c.sceneCutLow = std::clamp(toF(v, c.sceneCutLow), 0.f, 1.f);
        else if (k == "scene_cut_high") c.sceneCutHigh = std::clamp(toF(v, c.sceneCutHigh), 0.f, 1.f);
        else
            for (const ExtraOpt& o : extraOptions())
                if (!o.key.empty() && k == o.key) c.extra[k] = normalizeExtra(o, v);
    }
}

// A game was selected: show its saved [game] profile if one exists (checking
// the per-game box), otherwise fall back to the global options.
void onGameSelected(Ui& ui) {
    const int i = static_cast<int>(SendMessageW(ui.lb, LB_GETCURSEL, 0, 0));
    if (i < 0 || i >= static_cast<int>(ui.windows.size())) return;
    const std::string section = sectionForPid(ui.windows[i].pid);
    const auto scope = readScope(section);
    LaunchChoice merged = ui.baseline;
    applyScopeToChoice(scope, merged);
    *ui.choice = merged;
    fillFromChoice(ui);
    SendMessageW(ui.perGame, BM_SETCHECK, scope.empty() ? BST_UNCHECKED : BST_CHECKED, 0);
}

void fillFromChoice(Ui& ui) {
    const LaunchChoice& c = *ui.choice;
    SendMessageW(ui.gen, BM_SETCHECK, c.multiplier > 1 ? BST_CHECKED : BST_UNCHECKED, 0);
    int p = 0;
    for (int i = 1; i < static_cast<int>(std::size(kPresetVals)); ++i)
        if (c.preset == kPresetVals[i]) p = i;
    SendMessageW(ui.preset, CB_SETCURSEL, p, 0);
    SendMessageW(ui.mult, CB_SETCURSEL, c.adaptive ? 7 : std::clamp(c.multiplier, 2, 8) - 2, 0);
    SendMessageW(ui.mode, CB_SETCURSEL, c.extrapolate ? 0 : c.hybrid ? 2 : 1, 0);
    SendMessageW(ui.size, CB_SETCURSEL, c.fit ? 1 : (c.scale != 1.f ? 2 : 0), 0);
    wchar_t buf[32];
    swprintf(buf, 32, L"%g", static_cast<double>(c.scale));
    SetWindowTextW(ui.scaleEd, buf);
    swprintf(buf, 32, L"%d", static_cast<int>(c.sharpness * 100.f + 0.5f));
    SetWindowTextW(ui.sharpEd, buf);
    int f = 0;
    for (int i = 0; i < 3; ++i)
        if (c.filter == kFilterVals[i]) f = i;
    SendMessageW(ui.filter, CB_SETCURSEL, f, 0);
    SendMessageW(ui.fifo, BM_SETCHECK, c.fifo ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(ui.hud, CB_SETCURSEL, std::clamp(c.hud, 0, 4), 0);
    SendMessageW(ui.method, CB_SETCURSEL, std::clamp(c.method, 0, 2), 0);
    int g = 0;
    for (size_t i = 0; i < ui.gpuNames.size() && !c.gpu.empty(); ++i)
        if (wideLower(wideUtf8(ui.gpuNames[i])).find(wideLower(wideUtf8(c.gpu))) != std::wstring::npos) g = int(i) + 1;
    SendMessageW(ui.gpu, CB_SETCURSEL, g, 0);
    setNum(ui.grad, c.gradWeight);
    setNum(ui.scLow, c.sceneCutLow);
    setNum(ui.scHigh, c.sceneCutHigh);
    setNum(ui.smooth, c.smoothness);
    setNum(ui.zbias, c.zeroBias);
    setNum(ui.sfine, c.searchFine);
    setNum(ui.fiter, c.flowIterations);
    const auto& extra = extraOptions();
    for (size_t i = 0; i < extra.size(); ++i) {
        const ExtraOpt& o = extra[i];
        if (o.key.empty() || !ui.extra[i]) continue;
        const auto it = c.extra.find(o.key);
        const std::string v = it != c.extra.end() ? it->second : o.def;
        if (o.kind == kCheck) {
            SendMessageW(ui.extra[i], BM_SETCHECK, v == "1" ? BST_CHECKED : BST_UNCHECKED, 0);
        } else if (o.kind == kChoice) {
            int s = 0;
            for (int k = 0; k < o.choices; ++k)
                if (v == o.choiceValues[size_t(k)]) s = k;
            SendMessageW(ui.extra[i], CB_SETCURSEL, s, 0);
        } else {
            SetWindowTextW(ui.extra[i], wideFromAcp(v.c_str()).c_str());
        }
    }
    updateEnabled(ui);
}

void persist(const LaunchChoice& c) {
    std::vector<std::pair<std::string, std::string>> kv;
    kv.emplace_back("multiplier", std::to_string(c.multiplier));
    kv.emplace_back("adaptive", c.adaptive ? "1" : "0");
    kv.emplace_back("mode", c.extrapolate ? "extrapolate" : c.hybrid ? "hybrid" : "interpolate");
    kv.emplace_back("upscale_filter",
                    kFilterVals[0] == c.filter ? "lanczos" : kFilterVals[1] == c.filter ? "bicubic" : "bilinear");
    char num[32];
    std::snprintf(num, sizeof num, "%g", static_cast<double>(c.sharpness));
    kv.emplace_back("sharpness", num);
    kv.emplace_back("overlay", std::to_string(c.hud));
    if (!c.preset.empty()) kv.emplace_back("preset", c.preset);
    kv.emplace_back("gpu", c.gpu.empty() ? "auto" : c.gpu);
    kv.emplace_back("method", c.method == 1 ? "layer" : c.method == 2 ? "capture" : "auto");
    if (c.fifo) kv.emplace_back("present_mode", "fifo");
    // Advanced flow keys: written only when they differ from the default, so an
    // untouched panel keeps the file minimal and the engine on its own defaults
    // (same behaviour as the Linux control panel).
    auto addNum = [&](const char* key, double value, double def) {
        char a[32], b[32];
        std::snprintf(a, sizeof a, "%g", value);
        std::snprintf(b, sizeof b, "%g", def);
        if (std::string(a) != std::string(b)) kv.emplace_back(key, a);
    };
    addNum("grad_weight", c.gradWeight, 0.0);
    addNum("smoothness", c.smoothness, 0.004);
    addNum("zero_bias", c.zeroBias, 0.002);
    addNum("search_fine", c.searchFine, 2);
    addNum("flow_iterations", c.flowIterations, 1);
    addNum("scene_cut_low", c.sceneCutLow, 0.05);
    addNum("scene_cut_high", c.sceneCutHigh, 0.09);
    for (const ExtraOpt& o : extraOptions()) {
        if (o.key.empty()) continue;
        const auto it = c.extra.find(o.key);
        if (it != c.extra.end() && it->second != normalizeExtra(o, o.def)) kv.emplace_back(o.key, it->second);
    }
    // A per-game profile writes into the target's [game] section (Config::load
    // applies it over the globals when bdex_capture runs on that title);
    // otherwise the global [*] scope, as before.
    if (c.perGame && c.target) {
        DWORD pid = 0;
        GetWindowThreadProcessId(c.target, &pid);
        const std::string section = sectionForPid(pid);
        if (!sectionKey(section).empty()) {
            if (!savePerGameConfig(section, kv)) LOGW("could not write the config file");
            return;
        }
    }
    if (!saveGlobalConfig(kv)) LOGW("could not write the config file");
}

// The layer registered: Vulkan games load it, armed (virtual swapchain,
// frames passed through) until the launcher targets one (require_app, as
// the Linux control panel does) — so that injection is there to choose.
// (A configuration that already says, e.g. require_app = 0 to generate in
// every Vulkan game without the launcher, is kept.)
void armLayer() {
    if (!layerRegistered()) return;
    std::vector<std::pair<std::string, std::string>> kv;
    if (globalConfigValue("enabled").empty()) kv.emplace_back("enabled", "1");
    if (globalConfigValue("require_app").empty()) kv.emplace_back("require_app", "1");
    if (!kv.empty() && !saveGlobalConfig(kv)) LOGW("could not write the config file");
}

void gather(Ui& ui) {
    LaunchChoice& c = *ui.choice;
    const int was = static_cast<int>(SendMessageW(ui.lb, LB_GETCURSEL, 0, 0));
    c.target = (was >= 0 && was < static_cast<int>(ui.windows.size())) ? ui.windows[was].hwnd
                                                                       : nullptr;
    const bool on = SendMessageW(ui.gen, BM_GETCHECK, 0, 0) == BST_CHECKED;
    // « Adaptatif » (last entry): filling the monitor's refresh rate, up to a
    // ceiling main() sets from it (x4 up to 165 Hz, x6 up to 250, x8 above).
    c.adaptive = on && sel(ui.mult) == 7;
    c.multiplier = on ? (c.adaptive ? 4 : sel(ui.mult) + 2) : 1;
    c.preset = kPresetVals[sel(ui.preset)];
    c.extrapolate = sel(ui.mode) == 0;
    c.hybrid = sel(ui.mode) == 2;
    const int sizeSel = sel(ui.size);
    c.fit = sizeSel == 1;
    if (sizeSel == 2) {
        const int pct = std::clamp(readInt(ui.scaleEd, 100), 25, 400);
        c.scale = pct / 100.f;
    } else {
        c.scale = 1.f;
    }
    c.filter = kFilterVals[sel(ui.filter)];
    c.sharpness = std::clamp(readInt(ui.sharpEd, 0), 0, 100) / 100.f;
    c.fifo = SendMessageW(ui.fifo, BM_GETCHECK, 0, 0) == BST_CHECKED;
    c.hud = sel(ui.hud);
    c.method = std::clamp(sel(ui.method), 0, 2);
    const int g = sel(ui.gpu);
    c.gpu = g > 0 && g <= static_cast<int>(ui.gpuNames.size()) ? ui.gpuNames[size_t(g - 1)] : std::string();
    c.gradWeight = std::clamp(readFloat(ui.grad, 0.f), 0.f, 8.f);
    c.sceneCutLow = std::clamp(readFloat(ui.scLow, 0.05f), 0.f, 1.f);
    c.sceneCutHigh = std::clamp(readFloat(ui.scHigh, 0.09f), 0.f, 1.f);
    c.smoothness = std::clamp(readFloat(ui.smooth, 0.004f), 0.f, 1.f);
    c.zeroBias = std::clamp(readFloat(ui.zbias, 0.002f), 0.f, 1.f);
    c.searchFine = std::clamp(readInt(ui.sfine, 2), 1, 4);
    c.flowIterations = std::clamp(readInt(ui.fiter, 1), 0, 3);
    const auto& extra = extraOptions();
    for (size_t i = 0; i < extra.size(); ++i) {
        const ExtraOpt& o = extra[i];
        if (o.key.empty() || !ui.extra[i]) continue;
        std::string v;
        if (o.kind == kCheck) {
            v = SendMessageW(ui.extra[i], BM_GETCHECK, 0, 0) == BST_CHECKED ? "1" : "0";
        } else if (o.kind == kChoice) {
            v = o.choiceValues[std::clamp(sel(ui.extra[i]), 0, o.choices - 1)];
        } else {
            wchar_t buf[32] = {};
            GetWindowTextW(ui.extra[i], buf, 32);
            v = narrow(buf);
            for (auto& ch : v)
                if (ch == ',') ch = '.';  // accept a French decimal comma
        }
        c.extra[o.key] = normalizeExtra(o, v);
    }
    c.perGame = SendMessageW(ui.perGame, BM_GETCHECK, 0, 0) == BST_CHECKED;
}

LRESULT CALLBACK launcherProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* ui = reinterpret_cast<Ui*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
    case WM_CREATE: {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        ui = reinterpret_cast<Ui*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(ui));
        ui->baseline = *ui->choice;  // the global options, before any per-game merge
        const HINSTANCE inst = cs->hInstance;
        const HFONT font = static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
        auto add = [&](const wchar_t* cls, const wchar_t* text, DWORD style, int x, int y,
                       int w, int h, int id) -> HWND {
            HWND c = CreateWindowExW(0, cls, text, style | WS_CHILD | WS_VISIBLE, x, y, w, h,
                                     hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                                     inst, nullptr);
            if (c) SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
            return c;
        };
        auto group = [&](const wchar_t* t, int x, int y, int w, int h) {
            add(L"BUTTON", t, BS_GROUPBOX, x, y, w, h, 0);
        };
        auto combo = [&](int x, int y, int w, int id) {
            return add(L"COMBOBOX", nullptr, CBS_DROPDOWNLIST | CBS_HASSTRINGS | WS_VSCROLL,
                       x, y, w, 150, id);
        };

        group(T(L"Jeu à capturer", L"Game to capture"), 12, 8, 472, 168);
        ui->lb = add(L"LISTBOX", nullptr,
                     LBS_NOTIFY | LBS_NOINTEGRALHEIGHT | WS_VSCROLL | WS_BORDER | WS_TABSTOP,
                     24, 26, 448, 108, ID_LB);
        add(L"BUTTON", T(L"Actualiser la liste", L"Refresh the list"), BS_PUSHBUTTON | WS_TABSTOP, 356, 140, 116, 23,
            ID_REFRESH);
        ui->perGame = add(L"BUTTON", T(L"Profil pour ce jeu (sinon réglages globaux)", L"Profile for this game (else global settings)"),
                          BS_AUTOCHECKBOX | WS_TABSTOP, 24, 143, 324, 20, ID_PERGAME);

        group(T(L"Génération d'images", L"Frame generation"), 12, 184, 472, 136);
        ui->gen = add(L"BUTTON", T(L"Activer (plusieurs images par image du jeu)", L"Enable (several frames per game frame)"),
                      BS_AUTOCHECKBOX | WS_TABSTOP, 24, 202, 360, 20, ID_GEN);
        add(L"STATIC", T(L"Profil de qualité", L"Quality preset"), SS_LEFT, 36, 231, 140, 16, 0);
        ui->preset = combo(180, 228, 292, ID_PRESET);
        add(L"STATIC", T(L"Images par image", L"Frames per frame"), SS_LEFT, 36, 257, 140, 16, 0);
        ui->mult = combo(180, 254, 292, ID_MULT);
        add(L"STATIC", L"Mode", SS_LEFT, 36, 283, 140, 16, 0);
        ui->mode = combo(180, 280, 292, ID_MODE);

        group(T(L"Affichage", L"Display"), 12, 328, 472, 134);
        add(L"STATIC", T(L"Taille de la fenêtre", L"Window size"), SS_LEFT, 36, 346, 140, 16, 0);
        ui->size = combo(180, 343, 292, ID_SIZE);
        add(L"STATIC", T(L"Échelle", L"Scale"), SS_LEFT, 36, 372, 140, 16, 0);
        ui->scaleEd = add(L"EDIT", nullptr, ES_NUMBER | WS_TABSTOP, 180, 369, 90, 23, ID_SCALE);
        add(L"STATIC", L"%", SS_LEFT, 276, 372, 20, 16, 0);
        add(L"STATIC", T(L"Filtre d'agrandissement", L"Upscaling filter"), SS_LEFT, 36, 398, 140, 16, 0);
        ui->filter = combo(180, 395, 292, ID_FILTER);
        add(L"STATIC", T(L"Netteté (CAS)", L"Sharpness (CAS)"), SS_LEFT, 36, 424, 140, 16, 0);
        ui->sharpEd = add(L"EDIT", nullptr, ES_NUMBER | WS_TABSTOP, 180, 421, 90, 23, ID_SHARP);
        add(L"STATIC", L"%", SS_LEFT, 276, 424, 20, 16, 0);

        group(T(L"Présentation", L"Presentation"), 12, 470, 472, 76);
        ui->fifo = add(L"BUTTON", T(L"Synchroniser avec l'écran (FIFO / vsync)", L"Sync with the screen (FIFO / vsync)"),
                       BS_AUTOCHECKBOX | WS_TABSTOP, 24, 488, 360, 20, ID_FIFO);
        add(L"STATIC", T(L"Compteur de FPS", L"FPS counter"), SS_LEFT, 36, 515, 140, 16, 0);
        ui->hud = combo(180, 512, 292, ID_HUD);

        // Advanced optical-flow tuning. A plain EDIT (no ES_NUMBER) accepts a
        // decimal point; the two integer radii use ES_NUMBER.
        group(T(L"Réglages avancés", L"Advanced settings"), 12, 552, 472, 226);
        add(L"STATIC", T(L"Laisser par défaut convient à la plupart des jeux. 0 pour désactiver la netteté.", L"The defaults suit most games. 0 turns sharpening off."),
            SS_LEFT, 24, 570, 452, 16, 0);
        auto advEdit = [&](const wchar_t* label, int y, int id, bool integer) -> HWND {
            add(L"STATIC", label, SS_LEFT, 36, y + 3, 214, 16, 0);
            const DWORD st = WS_TABSTOP | WS_BORDER | (integer ? ES_NUMBER : 0);
            return add(L"EDIT", nullptr, st, 258, y, 90, 23, id);
        };
        ui->grad   = advEdit(T(L"Netteté du mouvement rapide", L"Sharp fast motion"), 592, ID_GRAD, false);
        ui->scLow  = advEdit(T(L"Coupure de scène — début du fondu", L"Scene cut — blend start"), 618, ID_SCLOW, false);
        ui->scHigh = advEdit(T(L"Coupure de scène — image réelle", L"Scene cut — real frame"), 644, ID_SCHIGH, false);
        ui->smooth = advEdit(T(L"Lissage du mouvement", L"Motion smoothing"), 670, ID_SMOOTH, false);
        ui->zbias  = advEdit(T(L"Stabilité de l'image fixe", L"Still-picture stability"), 696, ID_ZBIAS, false);
        ui->sfine  = advEdit(T(L"Précision de recherche (1–4)", L"Search precision (1–4)"), 722, ID_SFINE, true);
        ui->fiter  = advEdit(T(L"Itérations de synthèse (0–3)", L"Synthesis iterations (0–3)"), 748, ID_FITER, true);

        // Second column: the engine options of layer/options_ui.conf's
        // launcher sections (extraOptions), section by section.
        const Layout layout = launcherLayout();
        const auto& extra = extraOptions();
        group(T(L"Options avancées", L"Advanced options"), 496, 8, 472, layout.columnGroupH);
        add(L"STATIC", T(L"Seules les valeurs modifiées sont enregistrées ; par défaut convient à la plupart des jeux.", L"Only the values changed are saved; the defaults suit most games."),
            SS_LEFT, 508, 26, 452, 16, 0);
        ui->extra.assign(extra.size(), nullptr);
        int y = 48;
        // How and where: injection by the Vulkan layer (inside the game's
        // presentation: the lowest latency) where it has the game, else
        // capture; the GPU that generates (another one than the game's:
        // capture on it — the two-GPU setup).
        add(L"STATIC", T(L"Méthode", L"Method"), SS_LEFT, 508, y + 3, 186, 16, 0);
        ui->method = combo(700, y, 260, ID_METHOD);
        for (const wchar_t* s : {T(L"Auto — injection si possible, sinon capture", L"Auto — injection if possible, else capture"),
                                 T(L"Injection (couche Vulkan) seulement", L"Injection (Vulkan layer) only"),
                                 T(L"Capture seulement", L"Capture only")})
            SendMessageW(ui->method, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(s));
        y += 26;
        add(L"STATIC", T(L"GPU de génération", L"Generation GPU"), SS_LEFT, 508, y + 3, 190, 16, 0);
        ui->gpu = combo(700, y, 260, ID_GPU);
        SendMessageW(ui->gpu, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(T(L"Auto — celui du jeu", L"Auto — the game's")));
        ui->gpuNames = vulkanGpuNames();
        for (const auto& n : ui->gpuNames)
            SendMessageW(ui->gpu, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(wideUtf8(n).c_str()));
        y += 28;
        // Direct3D games get DXVK / VKD3D-Proton next to their executable for
        // the injection (injection.h): this takes them all away again.
        add(L"BUTTON", T(L"Retirer les injections (DXVK) des jeux…", L"Remove the injections (DXVK) from the games..."),
            BS_PUSHBUTTON | WS_TABSTOP, 508, y, 452, 23, ID_UNINJECT);
        y += 30;
        // A problem: the diagnostic report (written, copied) and the issue form.
        add(L"BUTTON", T(L"Signaler un problème… (rapport de diagnostic)", L"Report a problem... (diagnostic report)"),
            BS_PUSHBUTTON | WS_TABSTOP, 508, y, 452, 23, ID_REPORT);
        y += 30;
        // The GPU's calibration (bdex-calibrate.exe).
        {
            const Calibration cal = readCalibration();
            ui->calLabel = add(L"STATIC", calibrationText(cal).c_str(), SS_LEFT, 508, y + 2, 288, 32, 0);
            ui->calRun = add(L"BUTTON", T(L"Mesurer", L"Measure"), BS_PUSHBUTTON | WS_TABSTOP, 800, y, 76, 23, ID_CAL_RUN);
            ui->calApply = add(L"BUTTON", T(L"Appliquer", L"Apply"), BS_PUSHBUTTON | WS_TABSTOP, 884, y, 76, 23, ID_CAL_APPLY);
            EnableWindow(ui->calRun, !calibrateExe().empty());
            EnableWindow(ui->calApply, cal.ok);
            if (!cal.ok && !calibrateExe().empty()) {
                SetWindowTextW(ui->calLabel, T(L"Calibration du GPU : mesure en cours…", L"GPU calibration: measuring..."));
                EnableWindow(ui->calRun, FALSE);
                startCalibration(hwnd);
            }
        }
        y += 38;
        for (size_t i = 0; i < extra.size(); ++i) {
            const ExtraOpt& o = extra[i];
            const int id = 300 + static_cast<int>(i);
            if (o.key.empty()) {  // section header
                y += 3;
                add(L"STATIC", o.label.c_str(), SS_LEFT, 508, y, 452, 16, 0);
                y += 17;
                continue;
            }
            if (o.kind == kCheck) {
                ui->extra[i] = add(L"BUTTON", o.label.c_str(), BS_AUTOCHECKBOX | WS_TABSTOP, 520, y, 440, 20, id);
            } else if (o.kind == kChoice) {
                add(L"STATIC", o.label.c_str(), SS_LEFT, 520, y + 3, 250, 16, 0);
                ui->extra[i] = combo(776, y, 184, id);
                for (int k = 0; k < o.choices; ++k)
                    SendMessageW(ui->extra[i], CB_ADDSTRING, 0,
                                 reinterpret_cast<LPARAM>(o.choiceLabels[size_t(k)].c_str()));
            } else {
                add(L"STATIC", o.label.c_str(), SS_LEFT, 520, y + 3, 330, 16, 0);
                ui->extra[i] = add(L"EDIT", nullptr, WS_TABSTOP | WS_BORDER, 870, y, 90, 21, id);
            }
            y += 22;
        }

        // Under the left column.
        group(T(L"En cours", L"Running"), 12, 786, 472, 124);
        ui->running = add(L"EDIT", nullptr, ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL | WS_BORDER,
                          24, 804, 448, 98, ID_RUNNING);
        SetTimer(hwnd, ID_TIMER_RUNNING, 1000, nullptr);

        add(L"BUTTON", T(L"Démarrer", L"Start"), BS_DEFPUSHBUTTON | WS_TABSTOP, 770, layout.buttonsY, 96, 26, IDOK);
        add(L"BUTTON", T(L"Quitter", L"Quit"), BS_PUSHBUTTON | WS_TABSTOP, 874, layout.buttonsY, 94, 26, IDCANCEL);

        for (const wchar_t* s : kPresets)
            SendMessageW(ui->preset, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(s));
        for (const wchar_t* s : {L"x2", L"x3", L"x4", L"x5", L"x6", L"x7", L"x8", T(L"Adaptatif (fréquence de l'écran)", L"Adaptive (the screen's refresh rate)")})
            SendMessageW(ui->mult, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(s));
        for (const wchar_t* s : {T(L"Extrapolation (latence minimale)", L"Extrapolation (lowest latency)"), T(L"Interpolation (défaut, moins d'artefacts)", L"Interpolation (default, fewer artifacts)"),
                                 T(L"Hybride (×3/×4 : latence divisée par 2 à 3)", L"Hybrid (×3/×4: latency divided by 2 to 3)")})
            SendMessageW(ui->mode, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(s));
        for (const wchar_t* s : {T(L"Identique au jeu", L"Same as the game"), T(L"Ajustée à l'écran", L"Fitted to the screen"), T(L"Échelle personnalisée", L"Custom scale")})
            SendMessageW(ui->size, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(s));
        for (const wchar_t* s : kFilters)
            SendMessageW(ui->filter, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(s));
        for (const wchar_t* s : kHud)
            SendMessageW(ui->hud, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(s));

        fillFromChoice(*ui);
        refreshList(*ui);
        refreshRunning(*ui);
        return 0;
    }
    case kMsgCalibrated:
        if (ui) {
            const Calibration cal = readCalibration();
            SetWindowTextW(ui->calLabel, wp && cal.ok ? calibrationText(cal).c_str()
                                                      : T(L"Calibration du GPU : la mesure a échoué.", L"GPU calibration: the measurement failed."));
            EnableWindow(ui->calRun, TRUE);
            EnableWindow(ui->calApply, cal.ok);
        }
        return 0;
    case WM_TIMER:
        if (wp == ID_TIMER_RUNNING && ui) refreshRunning(*ui);
        return 0;
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case ID_REFRESH:
            refreshList(*ui);
            return 0;
        case ID_LB:
            if (HIWORD(wp) == LBN_SELCHANGE) onGameSelected(*ui);
            return 0;
        case ID_GEN:
        case ID_SIZE:
            updateEnabled(*ui);
            return 0;
        case IDOK: {
            gather(*ui);
            if (!ui->choice->target) {
                MessageBoxW(hwnd, T(L"Sélectionne d'abord la fenêtre du jeu à capturer.", L"Select the game's window to capture first."),
                            L"bdex framegen", MB_OK | MB_ICONINFORMATION);
                return 0;
            }
            ui->choice->ok = true;
            persist(*ui->choice);
            armLayer();
            DestroyWindow(hwnd);
            return 0;
        }
        case ID_CAL_RUN:
            SetWindowTextW(ui->calLabel, T(L"Calibration du GPU : mesure en cours…", L"GPU calibration: measuring..."));
            EnableWindow(ui->calRun, FALSE);
            startCalibration(hwnd);
            return 0;
        case ID_CAL_APPLY: {
            const Calibration cal = readCalibration();
            if (!cal.ok) return 0;
            gather(*ui);
            ui->choice->preset = cal.preset == "auto" ? std::string() : cal.preset;
            if (!ui->choice->adaptive) ui->choice->multiplier = cal.multiplier;
            fillFromChoice(*ui);
            updateEnabled(*ui);
            return 0;
        }
        case ID_REPORT: {
            std::string url;
            const std::wstring path = writeReport(url);
            if (path.empty()) {
                MessageBoxW(hwnd, T(L"Rapport impossible (LOCALAPPDATA ?).", L"Report impossible (LOCALAPPDATA?)."),
                            L"bdex framegen", MB_OK | MB_ICONWARNING);
                return 0;
            }
            // The report in the clipboard (to paste into the form), its file
            // shown in Explorer, then the pre-filled form.
            std::ifstream in(std::filesystem::path(path), std::ios::binary);
            const std::wstring text = wideUtf8(std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>()));
            if (OpenClipboard(hwnd)) {
                EmptyClipboard();
                if (HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, (text.size() + 1) * sizeof(wchar_t))) {
                    std::memcpy(GlobalLock(mem), text.c_str(), (text.size() + 1) * sizeof(wchar_t));
                    GlobalUnlock(mem);
                    if (!SetClipboardData(CF_UNICODETEXT, mem)) GlobalFree(mem);
                }
                CloseClipboard();
            }
            ShellExecuteW(nullptr, L"open", L"explorer.exe", (L"/select,\"" + path + L"\"").c_str(), nullptr, SW_SHOWNORMAL);
            MessageBoxW(hwnd,
                        (std::wstring(T(L"Rapport écrit (et copié dans le presse-papiers) :\n", L"Report written (and copied to the clipboard):\n")) +
                         path + T(L"\n\nLe formulaire pré-rempli va s'ouvrir : joins-y ce fichier (ou colle le rapport).",
                                  L"\n\nThe pre-filled form opens next: attach this file (or paste the report)."))
                            .c_str(),
                        L"bdex framegen", MB_OK | MB_ICONINFORMATION);
            ShellExecuteW(nullptr, L"open", wideUtf8(url).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            return 0;
        }
        case ID_UNINJECT: {
            const auto games = injectedGames();
            if (games.empty()) {
                MessageBoxW(hwnd, T(L"Aucun jeu n'a d'injection installée.", L"No game has an injection installed."),
                            L"bdex framegen", MB_OK | MB_ICONINFORMATION);
                return 0;
            }
            std::wstring done, failed;
            for (const auto& exe : games) {
                std::string why;
                if (removeTranslation(exe, why)) done += L"\n  " + exe;
                else failed += L"\n  " + exe + L" — " + wideUtf8(why);
            }
            std::wstring msg;
            if (!done.empty()) msg += std::wstring(T(L"Injection retirée :", L"Injection removed:")) + done;
            if (!failed.empty())
                msg += std::wstring(msg.empty() ? L"" : L"\n\n") + T(L"Non retirée (ferme le jeu et réessaie) :",
                                                                      L"Not removed (close the game and retry):") + failed;
            MessageBoxW(hwnd, msg.c_str(), L"bdex framegen", MB_OK | (failed.empty() ? MB_ICONINFORMATION : MB_ICONWARNING));
            return 0;
        }
        case IDCANCEL:
            DestroyWindow(hwnd);
            return 0;
        default:
            return 0;
        }
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        KillTimer(hwnd, ID_TIMER_RUNNING);
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
}

} // namespace

std::vector<std::string> vulkanGpuNames() {
    std::vector<std::string> out;
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "bdex_capture";
    app.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &app;
    VkInstance inst = VK_NULL_HANDLE;
    if (vkCreateInstance(&ici, nullptr, &inst) != VK_SUCCESS) return out;
    uint32_t n = 0;
    vkEnumeratePhysicalDevices(inst, &n, nullptr);
    std::vector<VkPhysicalDevice> pds(n);
    vkEnumeratePhysicalDevices(inst, &n, pds.data());
    for (VkPhysicalDevice pd : pds) {
        VkPhysicalDeviceProperties p;
        vkGetPhysicalDeviceProperties(pd, &p);
        if (p.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU) continue;
        if (std::find(out.begin(), out.end(), std::string(p.deviceName)) == out.end()) out.push_back(p.deviceName);
    }
    vkDestroyInstance(inst, nullptr);
    return out;
}

bool layerRegistered() {
    for (HKEY root : {HKEY_LOCAL_MACHINE, HKEY_CURRENT_USER}) {
        HKEY key = nullptr;
        if (RegOpenKeyExW(root, L"SOFTWARE\\Khronos\\Vulkan\\ImplicitLayers", 0, KEY_READ, &key) != ERROR_SUCCESS) continue;
        bool found = false;
        wchar_t name[1024];
        for (DWORD i = 0; !found; ++i) {
            DWORD len = static_cast<DWORD>(std::size(name));
            if (RegEnumValueW(key, i, name, &len, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
            found = wideLower(name).find(L"vk_layer_bdex_framegen") != std::wstring::npos;
        }
        RegCloseKey(key);
        if (found) return true;
    }
    return false;
}

bool runLauncher(LaunchChoice& choice) {
    choice.ok = false;  // stays false when the window is closed
    const HINSTANCE inst = GetModuleHandleW(nullptr);
    const wchar_t* cls = L"bdex_capture_launcher";
    WNDCLASSEXW wc{sizeof wc};
    if (!GetClassInfoExW(inst, cls, &wc)) {
        wc.lpfnWndProc = launcherProc;
        wc.hInstance = inst;
        wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
        wc.lpszClassName = cls;
        if (!RegisterClassExW(&wc)) return false;
    }
    // Prefill the advanced column from the global config scope (what the
    // capture would load), unless the caller already did.
    for (const ExtraOpt& o : extraOptions())
        if (!o.key.empty() && !choice.extra.count(o.key)) {
            const std::string v = globalConfigValue(o.key);
            if (!v.empty()) choice.extra[o.key] = normalizeExtra(o, v);
        }
    RECT client{0, 0, 980, launcherLayout().clientH};
    AdjustWindowRect(&client, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, FALSE);
    RECT wa{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
    const int w = client.right - client.left, h = client.bottom - client.top;
    const int x = wa.left + static_cast<int>(std::max<LONG>(0, (wa.right - wa.left) - w)) / 2;
    const int y = wa.top + static_cast<int>(std::max<LONG>(0, (wa.bottom - wa.top) - h)) / 2;
    Ui ui;
    ui.choice = &choice;
    HWND hwnd = CreateWindowExW(WS_EX_CONTROLPARENT, cls, T(L"bdex framegen — lanceur", L"bdex framegen — launcher"),
                                WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, x, y,
                                w, h, nullptr, nullptr, inst, &ui);
    if (!hwnd) return false;
    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);
    SetFocus(ui.lb);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0)
        if (!IsDialogMessageW(hwnd, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    return choice.ok;
}

} // namespace bdex
