#include "injection.h"

#include "launcher.h"
#include "log.h"
#include "platform.h"

#include <windows.h>
#include <psapi.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace bdex {

namespace fs = std::filesystem;

namespace {

template <typename C> const C* T(const C* fr, const C* en) { return launcherFrench() ? fr : en; }

std::wstring wide(const std::string& s) {
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(static_cast<size_t>(std::max(n - 1, 0)), L'\0');
    if (n > 1) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
    return w;
}

// Lower case, no directory, no extension: the layer's normalizeName.
std::string normalised(const std::wstring& path) {
    std::wstring b = fs::path(path).filename().wstring();
    if (b.size() > 4 && wideLower(b.substr(b.size() - 4)) == L".exe") b.resize(b.size() - 4);
    return narrow(wideLower(b));
}

// The directory of bdex_capture.exe: the package, with dxvk\, vkd3d-proton\ and the layer.
fs::path packageDir() {
    wchar_t exe[MAX_PATH];
    const DWORD n = GetModuleFileNameW(nullptr, exe, MAX_PATH);
    return n ? fs::path(std::wstring(exe, n)).parent_path() : fs::path();
}

fs::path localDir(const wchar_t* sub) {
    wchar_t buf[MAX_PATH];
    const DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH);
    if (!n || n >= MAX_PATH) return {};
    return fs::path(std::wstring(buf, n)) / sub;
}

// x64 from the executable's PE header (a 32-bit game needs 32-bit DLLs).
bool exeIs64(const std::wstring& path, bool fallback) {
    std::ifstream in(fs::path(path), std::ios::binary);
    char mz[64];
    if (!in.read(mz, sizeof mz) || mz[0] != 'M' || mz[1] != 'Z') return fallback;
    uint32_t peOff = 0;
    std::memcpy(&peOff, mz + 0x3C, 4);
    char pe[6];
    in.seekg(peOff);
    if (!in.read(pe, sizeof pe) || pe[0] != 'P' || pe[1] != 'E') return fallback;
    uint16_t machine = 0;
    std::memcpy(&machine, pe + 4, 2);
    return machine == 0x8664 /* AMD64 */ || machine == 0xAA64 /* ARM64 */;
}

// The anti-cheat of a game: a competitive title (the layer's list), an
// anti-cheat's files beside it (Easy Anti-Cheat, BattlEye — the launcher of a
// protected game starts it with the game), one of its modules in the game,
// or an anti-cheat process running.
std::string findAntiCheat(const GameInfo& g, const std::vector<std::wstring>& modules) {
    if (platformIsCompetitiveGame(g.name)) return g.name + T(" (jeu compétitif)", " (competitive game)");
    for (const std::wstring& m : modules) {
        const std::wstring b = wideLower(fs::path(m).filename().wstring());
        if (b.rfind(L"easyanticheat", 0) == 0) return "Easy Anti-Cheat";
        if (b.rfind(L"beclient", 0) == 0) return "BattlEye";
    }
    if (!g.exePath.empty()) {
        std::error_code ec;
        fs::path dir = fs::path(g.exePath).parent_path();
        for (int up = 0; up < 4 && !dir.empty(); ++up, dir = dir.parent_path()) {
            if (fs::exists(dir / L"EasyAntiCheat", ec) || fs::exists(dir / L"EasyAntiCheat_EOS", ec) ||
                fs::exists(dir / L"start_protected_game.exe", ec) || fs::exists(dir / L"EasyAntiCheat_EOS_Setup.exe", ec))
                return "Easy Anti-Cheat";
            if (fs::exists(dir / L"BattlEye", ec)) return "BattlEye";
            if (dir == dir.root_path()) break;
        }
    }
    return platformAntiCheatRunning();
}

const wchar_t* kMarker = L"bdex-framegen-injection.txt";

fs::path injectionsList() { return localDir(L"bdex-framegen") / L"injections.txt"; }

void saveInjections(const std::vector<std::wstring>& games) {
    std::error_code ec;
    fs::create_directories(injectionsList().parent_path(), ec);
    std::ofstream out(injectionsList(), std::ios::trunc);
    for (const auto& g : games) out << narrow(g) << "\n";
}

} // namespace

const char* apiName(GameInfo::Api api) {
    switch (api) {
    case GameInfo::Vulkan: return "Vulkan";
    case GameInfo::D3D9: return "Direct3D 9";
    case GameInfo::D3D10: return "Direct3D 10";
    case GameInfo::D3D11: return "Direct3D 11";
    case GameInfo::D3D12: return "Direct3D 12";
    case GameInfo::OpenGL: return "OpenGL";
    default: return "?";
    }
}

GameInfo inspectGame(DWORD pid) {
    GameInfo g;
    g.pid = pid;
    std::vector<std::wstring> modules;
    if (HANDLE h = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid)) {
        wchar_t path[MAX_PATH * 2];
        DWORD n = MAX_PATH * 2;
        if (QueryFullProcessImageNameW(h, 0, path, &n)) g.exePath.assign(path, n);
        HMODULE mods[1024];
        DWORD bytes = 0;
        if (EnumProcessModulesEx(h, mods, sizeof mods, &bytes, LIST_MODULES_ALL)) {
            for (DWORD i = 0; i < std::min<DWORD>(bytes / sizeof(HMODULE), 1024); ++i) {
                wchar_t m[MAX_PATH];
                const DWORD len = GetModuleFileNameExW(h, mods[i], m, MAX_PATH);
                if (len) modules.emplace_back(m, len);
            }
        }
        CloseHandle(h);
    } else if (HANDLE l = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)) {
        // (A protected game refuses the module list: its path at least.)
        wchar_t path[MAX_PATH * 2];
        DWORD n = MAX_PATH * 2;
        if (QueryFullProcessImageNameW(l, 0, path, &n)) g.exePath.assign(path, n);
        CloseHandle(l);
    }
    g.name = g.exePath.empty() ? narrow(wideLower(processName(pid))) : normalised(g.exePath);
    if (g.name.size() > 4 && g.name.compare(g.name.size() - 4, 4, ".exe") == 0) g.name.resize(g.name.size() - 4);
#ifdef _WIN64
    g.is64 = g.exePath.empty() ? true : exeIs64(g.exePath, true);
#else
    g.is64 = g.exePath.empty() ? false : exeIs64(g.exePath, false);
#endif
    // The graphics API from the modules: D3D12 games load d3d11 / dxgi too,
    // and DXVK (ours or not) loads vulkan-1 under the D3D names.
    bool vk = false, gl = false, d9 = false, d10 = false, d11 = false, d12 = false;
    for (const std::wstring& m : modules) {
        const std::wstring b = wideLower(fs::path(m).filename().wstring());
        vk |= b == L"vulkan-1.dll";
        gl |= b == L"opengl32.dll";
        d9 |= b == L"d3d9.dll";
        d10 |= b == L"d3d10.dll" || b == L"d3d10_1.dll" || b == L"d3d10core.dll";
        d11 |= b == L"d3d11.dll";
        d12 |= b == L"d3d12.dll";
    }
    g.api = d12 ? GameInfo::D3D12 : d11 ? (d10 ? GameInfo::D3D10 : GameInfo::D3D11) : d10 ? GameInfo::D3D10
          : d9 ? GameInfo::D3D9 : vk ? GameInfo::Vulkan : gl ? GameInfo::OpenGL : GameInfo::Unknown;
    std::error_code ec;
    g.translated = !g.exePath.empty() && fs::exists(fs::path(g.exePath).parent_path() / kMarker, ec);
    g.antiCheat = findAntiCheat(g, modules);
    g.antiCheatException = !g.antiCheat.empty() && platformAntiCheatException(g.name);
    return g;
}

std::vector<std::wstring> translationFiles(GameInfo::Api api) {
    switch (api) {
    case GameInfo::D3D9: return {L"d3d9.dll"};
    case GameInfo::D3D10: return {L"d3d10core.dll", L"d3d11.dll", L"dxgi.dll"};
    case GameInfo::D3D11: return {L"d3d11.dll", L"dxgi.dll"};
    // VKD3D-Proton needs DXVK's dxgi (measured under Wine: the system dxgi crashes it).
    case GameInfo::D3D12: return {L"d3d12.dll", L"d3d12core.dll", L"dxgi.dll"};
    default: return {};
    }
}

// One of the files translationFiles names, as a bare file name.
static bool isTranslationFile(const std::wstring& name) {
    const std::wstring n = wideLower(name);
    for (const auto* f : {L"d3d9.dll", L"d3d10core.dll", L"d3d11.dll", L"dxgi.dll", L"d3d12.dll", L"d3d12core.dll"})
        if (n == f) return true;
    return false;
}

// Where the package keeps a file: d3d12*.dll from VKD3D-Proton, the rest from DXVK.
static fs::path translationSource(const std::wstring& file, bool is64) {
    const bool vkd3d = file.rfind(L"d3d12", 0) == 0;
    return packageDir() / (vkd3d ? L"vkd3d-proton" : L"dxvk") / (is64 ? L"x64" : vkd3d ? L"x86" : L"x32") / file;
}

bool translationAvailable(const GameInfo& g) {
    const auto files = translationFiles(g.api);
    if (files.empty()) return false;
    std::error_code ec;
    for (const auto& f : files)
        if (!fs::exists(translationSource(f, g.is64), ec)) return false;
    return true;
}

bool installTranslation(const GameInfo& g, std::string& why) {
    const auto files = translationFiles(g.api);
    if (files.empty() || g.exePath.empty()) {
        why = T("pas de couche de traduction pour ce jeu", "no translation layer for this game");
        return false;
    }
    if (!translationAvailable(g)) {
        why = T("DXVK / VKD3D-Proton absents du paquet (dossiers dxvk et vkd3d-proton)",
                "DXVK / VKD3D-Proton missing from the package (dxvk and vkd3d-proton folders)");
        return false;
    }
    const fs::path dir = fs::path(g.exePath).parent_path();
    std::error_code ec;
    for (const auto& f : files)
        if (fs::exists(dir / f, ec)) {
            why = narrow(f) + T(" existe déjà à côté du jeu (autre DXVK, ReShade… ?) : rien n'est remplacé",
                                " already exists next to the game (another DXVK, ReShade...?): nothing is replaced");
            return false;
        }
    std::vector<std::wstring> copied;
    for (const auto& f : files) {
        fs::copy_file(translationSource(f, g.is64), dir / f, ec);
        if (ec) {
            why = narrow(f) + T(" : copie impossible (", ": copy failed (") + ec.message() +
                  T(") — dossier protégé ? lancer en administrateur", ") — protected folder? run as administrator");
            for (const auto& c : copied) fs::remove(dir / c, ec);
            return false;
        }
        copied.push_back(f);
    }
    {
        std::ofstream marker(dir / kMarker, std::ios::trunc);
        marker << "# Copied by bdex-framegen (" << apiName(g.api) << " -> Vulkan): removed by the launcher's\n"
               << "# « Retirer les injections » button, or delete these files and this one.\n";
        for (const auto& f : copied) marker << narrow(f) << "\n";
    }
    auto list = injectedGames();
    if (std::find(list.begin(), list.end(), g.exePath) == list.end()) list.push_back(g.exePath);
    saveInjections(list);
    LOGI("injection prepared for %s (%s): %zu translation DLL(s) next to %s", g.name.c_str(), apiName(g.api),
         copied.size(), narrow(dir.wstring()).c_str());
    return true;
}

bool removeTranslation(const std::wstring& exePath, std::string& why) {
    const fs::path dir = fs::path(exePath).parent_path();
    std::error_code ec;
    std::ifstream marker(dir / kMarker);
    bool ok = true;
    if (marker) {
        std::string line;
        while (std::getline(marker, line)) {
            if (line.empty() || line[0] == '#') continue;
            if (!line.empty() && line.back() == '\r') line.pop_back();
            // Only the names installTranslation writes: the marker sits in the
            // game's folder, and an edited line (a full path, "..\\") would
            // otherwise delete any file.
            if (!isTranslationFile(wide(line))) {
                LOGW("injection marker in %s: \"%s\" is not a translation DLL, left alone",
                     narrow(dir.wstring()).c_str(), line.c_str());
                continue;
            }
            fs::remove(dir / wide(line), ec);
            if (ec) {
                why = line + T(" : suppression impossible (jeu encore ouvert ?)", ": cannot be removed (game still running?)");
                ok = false;
            }
        }
        marker.close();
        if (ok) fs::remove(dir / kMarker, ec);
    }
    if (ok) {
        auto list = injectedGames();
        list.erase(std::remove(list.begin(), list.end(), exePath), list.end());
        saveInjections(list);
    }
    return ok;
}

std::vector<std::wstring> injectedGames() {
    std::vector<std::wstring> out;
    std::ifstream in(injectionsList());
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!line.empty()) out.push_back(wide(line));
    }
    return out;
}

bool registerLayerForUser(std::string& why) {
    struct Arch {
        const wchar_t* src;     // in the package
        const wchar_t* dest;    // under %LOCALAPPDATA%
        const wchar_t* name;    // layer name (manifest)
        const wchar_t* key;     // HKCU registry key
    };
    const Arch arches[] = {
#ifdef _WIN64
        {L"VkLayer_bdex_framegen.dll", L"bdex-framegen", L"VK_LAYER_BDEX_framegen", L"SOFTWARE\\Khronos\\Vulkan\\ImplicitLayers"},
        {L"x86\\VkLayer_bdex_framegen.dll", L"bdex-framegen32", L"VK_LAYER_BDEX_framegen_32",
         L"SOFTWARE\\WOW6432Node\\Khronos\\Vulkan\\ImplicitLayers"},
#else
        {L"VkLayer_bdex_framegen.dll", L"bdex-framegen32", L"VK_LAYER_BDEX_framegen_32", L"SOFTWARE\\Khronos\\Vulkan\\ImplicitLayers"},
#endif
    };
    bool any = false;
    std::error_code ec;
    for (const Arch& a : arches) {
        const fs::path src = packageDir() / a.src;
        if (!fs::exists(src, ec)) continue;
        const fs::path dest = localDir(a.dest);
        if (dest.empty()) {
            why = "LOCALAPPDATA unset";
            return false;
        }
        fs::create_directories(dest, ec);
        const fs::path dll = dest / L"VkLayer_bdex_framegen.dll";
        // The package's DLL changed (an update): copy it again — not while a
        // game holds the old one loaded (it stays; the next start picks it up).
        const bool stale = !fs::exists(dll, ec) || fs::file_size(dll, ec) != fs::file_size(src, ec) ||
                           fs::last_write_time(dll, ec) < fs::last_write_time(src, ec);
        if (stale) {
            fs::copy_file(src, dll, fs::copy_options::overwrite_existing, ec);
            if (ec) LOGW("layer copy to %s failed (%s): a game still has it loaded?", narrow(dll.wstring()).c_str(),
                         ec.message().c_str());
        }
        const fs::path profiles = packageDir() / L"profiles.conf";
        if (fs::exists(profiles, ec)) fs::copy_file(profiles, dest / L"profiles.conf", fs::copy_options::overwrite_existing, ec);
        const fs::path json = dest / (std::wstring(a.name) + L".json");
        {
            std::ofstream m(json, std::ios::trunc);
            m << "{\n  \"file_format_version\": \"1.2.0\",\n  \"layer\": {\n    \"name\": \"" << narrow(a.name)
              << "\",\n    \"type\": \"GLOBAL\",\n    \"library_path\": \".\\\\VkLayer_bdex_framegen.dll\",\n"
                 "    \"api_version\": \"1.4.0\",\n    \"implementation_version\": \"1\",\n"
                 "    \"description\": \"BDEX frame generation: interpolates extra frames between the game's frames\",\n"
                 "    \"functions\": {\n      \"vkGetInstanceProcAddr\": \"vkGetInstanceProcAddr\",\n"
                 "      \"vkGetDeviceProcAddr\": \"vkGetDeviceProcAddr\",\n"
                 "      \"vkNegotiateLoaderLayerInterfaceVersion\": \"vkNegotiateLoaderLayerInterfaceVersion\"\n    },\n"
                 "    \"disable_environment\": { \"BDEX_FG_DISABLE\": \"1\" }\n  }\n}\n";
        }
        HKEY key = nullptr;
        if (RegCreateKeyExW(HKEY_CURRENT_USER, a.key, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS) {
            why = "registry: HKCU\\" + narrow(a.key);
            return false;
        }
        const DWORD zero = 0;
        const LONG r = RegSetValueExW(key, json.wstring().c_str(), 0, REG_DWORD, reinterpret_cast<const BYTE*>(&zero), sizeof zero);
        RegCloseKey(key);
        if (r != ERROR_SUCCESS) {
            why = "registry value " + narrow(json.wstring());
            return false;
        }
        any = true;
    }
    if (!any) why = T("VkLayer_bdex_framegen.dll absent du paquet", "VkLayer_bdex_framegen.dll missing from the package");
    return any;
}

std::wstring antiCheatWarning(const GameInfo& g, bool injection) {
    const std::wstring ac = wide(g.antiCheat), game = wide(g.name);
    if (injection)
        return std::wstring(T(L"Anti-triche détecté pour ", L"Anti-cheat detected for ")) + game + L" (" + ac + L").\n\n" +
               T(L"La génération d'images va être injectée DANS le jeu (couche Vulkan",
                 L"Frame generation is going to be injected INTO the game (Vulkan layer") +
               (g.api != GameInfo::Vulkan ? T(L", via DXVK copié à côté du jeu", L", through DXVK copied next to the game") : L"") +
               T(L"). L'anti-triche ne la détecte pas à ce jour, mais il peut changer à tout moment : "
                 L"un bannissement du compte est possible. Utilisation à tes risques.\n\n"
                 L"Le lancement continue.",
                 L"). The anti-cheat does not flag it today, but it can change at any time: the account "
                 L"could be banned. Use at your own risk.\n\nThe launch goes ahead.");
    return std::wstring(T(L"Anti-triche détecté pour ", L"Anti-cheat detected for ")) + game + L" (" + ac + L").\n\n" +
           T(L"bdex-framegen passe par la capture de la fenêtre : rien n'est chargé dans le jeu ni modifié. "
             L"Certains éditeurs interdisent tout de même les logiciels tiers : utilisation à tes risques.\n\n"
             L"Le lancement continue.",
             L"bdex-framegen captures the window: nothing is loaded into the game nor changed. Some "
             L"publishers still forbid third-party software: use at your own risk.\n\nThe launch goes ahead.");
}

} // namespace bdex
