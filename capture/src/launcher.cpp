// bdex_capture's native launcher — see launcher.h. Win32 only, no toolkit:
// hand-placed controls in one window, IsDialogMessage for keyboard
// navigation. The options persist to the shared config file's [*] section
// with the same key names the Linux control panel
// (tools/bdex-framegen-gui) writes, so the two panels stay interchangeable.
#include "launcher.h"

#include "log.h"
#include "platform.h"

#include <windows.h>
#include <dwmapi.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <cwctype>
#include <string>
#include <utility>
#include <vector>

namespace bdex {

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

// Keys the launcher manages in the global scope; a stale one not present in
// the new set is removed from the file (preset / present_mode = "auto").
static bool managedKey(const std::string& key) {
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

// ------------------------------------------------------------ the window --

namespace {

enum {
    // 100+: Windows reserves 0..10 for IDOK/IDCANCEL/…
    ID_LB = 100,
    ID_REFRESH,
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
};

const wchar_t* const kPresets[] = {L"Auto", L"Qualité", L"Équilibré", L"Performance"};
const char* const kPresetVals[] = {"", "quality", "balanced", "performance"};
const wchar_t* const kFilters[] = {L"Lanczos (le plus net)", L"Bicubique", L"Bilinéaire"};
const int kFilterVals[] = {2, 1, 0};
const wchar_t* const kHud[] = {L"Désactivé", L"FPS", L"FPS + génération", L"+ 1% low",
                               L"+ 1% et 0.1% low"};

struct Ui {
    LaunchChoice* choice;
    std::vector<WindowInfo> windows;
    HWND lb = nullptr, gen = nullptr, preset = nullptr, mult = nullptr, mode = nullptr;
    HWND size = nullptr, scaleEd = nullptr, filter = nullptr, sharpEd = nullptr;
    HWND fifo = nullptr, hud = nullptr;
    HWND grad = nullptr, scLow = nullptr, scHigh = nullptr, smooth = nullptr;
    HWND zbias = nullptr, sfine = nullptr, fiter = nullptr;
};

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

void fillFromChoice(Ui& ui) {
    const LaunchChoice& c = *ui.choice;
    SendMessageW(ui.gen, BM_SETCHECK, c.multiplier > 1 ? BST_CHECKED : BST_UNCHECKED, 0);
    int p = 0;
    for (int i = 1; i <= 3; ++i)
        if (c.preset == kPresetVals[i]) p = i;
    SendMessageW(ui.preset, CB_SETCURSEL, p, 0);
    SendMessageW(ui.mult, CB_SETCURSEL, std::clamp(c.multiplier, 2, 4) - 2, 0);
    SendMessageW(ui.mode, CB_SETCURSEL, c.extrapolate ? 0 : 1, 0);
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
    setNum(ui.grad, c.gradWeight);
    setNum(ui.scLow, c.sceneCutLow);
    setNum(ui.scHigh, c.sceneCutHigh);
    setNum(ui.smooth, c.smoothness);
    setNum(ui.zbias, c.zeroBias);
    setNum(ui.sfine, c.searchFine);
    setNum(ui.fiter, c.flowIterations);
    updateEnabled(ui);
}

void persist(const LaunchChoice& c) {
    std::vector<std::pair<std::string, std::string>> kv;
    kv.emplace_back("multiplier", std::to_string(c.multiplier));
    kv.emplace_back("mode", c.extrapolate ? "extrapolate" : "interpolate");
    kv.emplace_back("upscale_filter",
                    kFilterVals[0] == c.filter ? "lanczos" : kFilterVals[1] == c.filter ? "bicubic" : "bilinear");
    char num[32];
    std::snprintf(num, sizeof num, "%g", static_cast<double>(c.sharpness));
    kv.emplace_back("sharpness", num);
    kv.emplace_back("overlay", std::to_string(c.hud));
    if (!c.preset.empty()) kv.emplace_back("preset", c.preset);
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
    if (!saveGlobalConfig(kv)) LOGW("could not write the config file");
}

void gather(Ui& ui) {
    LaunchChoice& c = *ui.choice;
    const int was = static_cast<int>(SendMessageW(ui.lb, LB_GETCURSEL, 0, 0));
    c.target = (was >= 0 && was < static_cast<int>(ui.windows.size())) ? ui.windows[was].hwnd
                                                                       : nullptr;
    const bool on = SendMessageW(ui.gen, BM_GETCHECK, 0, 0) == BST_CHECKED;
    c.multiplier = on ? sel(ui.mult) + 2 : 1;
    c.preset = kPresetVals[sel(ui.preset)];
    c.extrapolate = sel(ui.mode) == 0;
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
    c.gradWeight = std::clamp(readFloat(ui.grad, 0.f), 0.f, 8.f);
    c.sceneCutLow = std::clamp(readFloat(ui.scLow, 0.05f), 0.f, 1.f);
    c.sceneCutHigh = std::clamp(readFloat(ui.scHigh, 0.09f), 0.f, 1.f);
    c.smoothness = std::clamp(readFloat(ui.smooth, 0.004f), 0.f, 1.f);
    c.zeroBias = std::clamp(readFloat(ui.zbias, 0.002f), 0.f, 1.f);
    c.searchFine = std::clamp(readInt(ui.sfine, 2), 1, 4);
    c.flowIterations = std::clamp(readInt(ui.fiter, 1), 0, 3);
}

LRESULT CALLBACK launcherProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* ui = reinterpret_cast<Ui*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
    case WM_CREATE: {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        ui = reinterpret_cast<Ui*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(ui));
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

        group(L"Jeu à capturer", 12, 8, 472, 168);
        ui->lb = add(L"LISTBOX", nullptr,
                     LBS_NOTIFY | LBS_NOINTEGRALHEIGHT | WS_VSCROLL | WS_BORDER | WS_TABSTOP,
                     24, 26, 448, 108, ID_LB);
        add(L"BUTTON", L"Actualiser la liste", BS_PUSHBUTTON | WS_TABSTOP, 356, 140, 116, 23,
            ID_REFRESH);

        group(L"Génération d'images", 12, 184, 472, 136);
        ui->gen = add(L"BUTTON", L"Activer (plusieurs images par image du jeu)",
                      BS_AUTOCHECKBOX | WS_TABSTOP, 24, 202, 360, 20, ID_GEN);
        add(L"STATIC", L"Profil de qualité", SS_LEFT, 36, 231, 140, 16, 0);
        ui->preset = combo(180, 228, 292, ID_PRESET);
        add(L"STATIC", L"Images par image", SS_LEFT, 36, 257, 140, 16, 0);
        ui->mult = combo(180, 254, 292, ID_MULT);
        add(L"STATIC", L"Mode", SS_LEFT, 36, 283, 140, 16, 0);
        ui->mode = combo(180, 280, 292, ID_MODE);

        group(L"Affichage", 12, 328, 472, 134);
        add(L"STATIC", L"Taille de la fenêtre", SS_LEFT, 36, 346, 140, 16, 0);
        ui->size = combo(180, 343, 292, ID_SIZE);
        add(L"STATIC", L"Échelle", SS_LEFT, 36, 372, 140, 16, 0);
        ui->scaleEd = add(L"EDIT", nullptr, ES_NUMBER | WS_TABSTOP, 180, 369, 90, 23, ID_SCALE);
        add(L"STATIC", L"%", SS_LEFT, 276, 372, 20, 16, 0);
        add(L"STATIC", L"Filtre d'agrandissement", SS_LEFT, 36, 398, 140, 16, 0);
        ui->filter = combo(180, 395, 292, ID_FILTER);
        add(L"STATIC", L"Netteté (CAS)", SS_LEFT, 36, 424, 140, 16, 0);
        ui->sharpEd = add(L"EDIT", nullptr, ES_NUMBER | WS_TABSTOP, 180, 421, 90, 23, ID_SHARP);
        add(L"STATIC", L"%", SS_LEFT, 276, 424, 20, 16, 0);

        group(L"Présentation", 12, 470, 472, 76);
        ui->fifo = add(L"BUTTON", L"Synchroniser avec l'écran (FIFO / vsync)",
                       BS_AUTOCHECKBOX | WS_TABSTOP, 24, 488, 360, 20, ID_FIFO);
        add(L"STATIC", L"Compteur de FPS", SS_LEFT, 36, 515, 140, 16, 0);
        ui->hud = combo(180, 512, 292, ID_HUD);

        // Advanced optical-flow tuning. A plain EDIT (no ES_NUMBER) accepts a
        // decimal point; the two integer radii use ES_NUMBER.
        group(L"Réglages avancés", 12, 552, 472, 226);
        add(L"STATIC", L"Laisser par défaut convient à la plupart des jeux. 0 pour désactiver la netteté.",
            SS_LEFT, 24, 570, 452, 16, 0);
        auto advEdit = [&](const wchar_t* label, int y, int id, bool integer) -> HWND {
            add(L"STATIC", label, SS_LEFT, 36, y + 3, 214, 16, 0);
            const DWORD st = WS_TABSTOP | WS_BORDER | (integer ? ES_NUMBER : 0);
            return add(L"EDIT", nullptr, st, 258, y, 90, 23, id);
        };
        ui->grad   = advEdit(L"Netteté du mouvement rapide", 592, ID_GRAD, false);
        ui->scLow  = advEdit(L"Coupure de scène — début du fondu", 618, ID_SCLOW, false);
        ui->scHigh = advEdit(L"Coupure de scène — image réelle", 644, ID_SCHIGH, false);
        ui->smooth = advEdit(L"Lissage du mouvement", 670, ID_SMOOTH, false);
        ui->zbias  = advEdit(L"Stabilité de l'image fixe", 696, ID_ZBIAS, false);
        ui->sfine  = advEdit(L"Précision de recherche (1–4)", 722, ID_SFINE, true);
        ui->fiter  = advEdit(L"Itérations de synthèse (0–3)", 748, ID_FITER, true);

        add(L"BUTTON", L"Démarrer", BS_DEFPUSHBUTTON | WS_TABSTOP, 286, 790, 96, 26, IDOK);
        add(L"BUTTON", L"Quitter", BS_PUSHBUTTON | WS_TABSTOP, 390, 790, 94, 26, IDCANCEL);

        for (const wchar_t* s : kPresets)
            SendMessageW(ui->preset, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(s));
        for (const wchar_t* s : {L"x2", L"x3", L"x4"})
            SendMessageW(ui->mult, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(s));
        for (const wchar_t* s : {L"Extrapolation (latence minimale)", L"Interpolation (plus fluide)"})
            SendMessageW(ui->mode, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(s));
        for (const wchar_t* s : {L"Identique au jeu", L"Ajustée à l'écran", L"Échelle personnalisée"})
            SendMessageW(ui->size, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(s));
        for (const wchar_t* s : kFilters)
            SendMessageW(ui->filter, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(s));
        for (const wchar_t* s : kHud)
            SendMessageW(ui->hud, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(s));

        fillFromChoice(*ui);
        refreshList(*ui);
        return 0;
    }
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case ID_REFRESH:
            refreshList(*ui);
            return 0;
        case ID_GEN:
        case ID_SIZE:
            updateEnabled(*ui);
            return 0;
        case IDOK: {
            gather(*ui);
            if (!ui->choice->target) {
                MessageBoxW(hwnd, L"Sélectionne d'abord la fenêtre du jeu à capturer.",
                            L"bdex framegen", MB_OK | MB_ICONINFORMATION);
                return 0;
            }
            ui->choice->ok = true;
            persist(*ui->choice);
            DestroyWindow(hwnd);
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
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
}

} // namespace

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
    RECT client{0, 0, 496, 830};
    AdjustWindowRect(&client, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, FALSE);
    RECT wa{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
    const int w = client.right - client.left, h = client.bottom - client.top;
    const int x = wa.left + static_cast<int>(std::max<LONG>(0, (wa.right - wa.left) - w)) / 2;
    const int y = wa.top + static_cast<int>(std::max<LONG>(0, (wa.bottom - wa.top) - h)) / 2;
    Ui ui;
    ui.choice = &choice;
    HWND hwnd = CreateWindowExW(WS_EX_CONTROLPARENT, cls, L"bdex framegen — lanceur",
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
