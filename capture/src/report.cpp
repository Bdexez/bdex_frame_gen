#include "report.h"

#include "config.h"
#include "injection.h"
#include "launcher.h"
#include "platform.h"
#include "status.h"

#include <windows.h>

#include <cctype>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <vector>

#ifndef BDEX_VERSION_STR
#define BDEX_VERSION_STR "?"
#endif

namespace bdex {

namespace fs = std::filesystem;

namespace {

std::string readFile(const fs::path& p, size_t tail = 0) {
    std::ifstream in(p, std::ios::binary);
    std::string s((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (tail && s.size() > tail) s = "[...]\n" + s.substr(s.size() - tail);
    return s;
}

// RtlGetVersion: the real version (GetVersionEx lies to unmanifested programs).
std::string windowsVersion() {
    using Fn = LONG(WINAPI*)(OSVERSIONINFOW*);
    OSVERSIONINFOW v{};
    v.dwOSVersionInfoSize = sizeof v;
    if (HMODULE nt = GetModuleHandleW(L"ntdll.dll"))
        if (auto fn = reinterpret_cast<Fn>(reinterpret_cast<void*>(GetProcAddress(nt, "RtlGetVersion")))) fn(&v);
    char buf[64];
    std::snprintf(buf, sizeof buf, "Windows %lu.%lu build %lu", v.dwMajorVersion, v.dwMinorVersion, v.dwBuildNumber);
    return buf;
}

std::string urlencode(const std::string& s) {
    std::string out;
    char buf[4];
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') out += static_cast<char>(c);
        else {
            std::snprintf(buf, sizeof buf, "%%%02X", c);
            out += buf;
        }
    }
    return out;
}

// The lines of a report section ("== name" up to the next "== ").
std::string section(const std::string& report, const std::string& name) {
    const size_t at = report.find("== " + name);
    if (at == std::string::npos) return "";
    const size_t from = report.find('\n', at);
    if (from == std::string::npos) return "";
    const size_t to = report.find("\n== ", from);
    return report.substr(from + 1, to == std::string::npos ? std::string::npos : to - from - 1);
}

std::string firstLine(const std::string& s) { return s.substr(0, s.find('\n')); }

// The report goes to a public issue: the profile's path (it names the user)
// becomes %USERPROFILE%, like ~ in the Linux report. Case-insensitive, as
// Windows paths are.
std::string hideProfile(std::string s) {
    wchar_t buf[MAX_PATH];
    const DWORD n = GetEnvironmentVariableW(L"USERPROFILE", buf, MAX_PATH);
    if (!n || n >= MAX_PATH) return s;
    std::string h = narrow(std::wstring(buf, n));
    while (!h.empty() && (h.back() == '\\' || h.back() == '/')) h.pop_back();
    if (h.size() < 4) return s;
    auto lower = [](std::string t) {
        for (char& c : t) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return t;
    };
    const std::string lh = lower(h), ls = lower(s);
    std::string out;
    size_t from = 0;
    for (size_t i = ls.find(lh); i != std::string::npos; i = ls.find(lh, from)) {
        out += s.substr(from, i - from) + "%USERPROFILE%";
        from = i + h.size();
    }
    return out + s.substr(from);
}

} // namespace

std::string buildReport() {
    std::ostringstream o;
    o << "bdex-framegen report (Windows launcher)\nVK_LAYER_BDEX_framegen v" << BDEX_VERSION_STR << "\n\n";
    o << "== lecture rapide (what runs now)\n";
    const std::string dir = platformStatusDir();
    std::error_code ec;
    int running = 0;
    if (!dir.empty())
        for (const auto& e : fs::directory_iterator(dir, ec)) {
            if (e.path().extension() != ".status") continue;
            const StatusFile f = statusParse(readFile(e.path()));
            o << f.process << " (pid " << f.pid << "):\n";
            for (const auto& line : statusDescribe(f, !launcherFrench())) o << "  " << line << "\n";
            ++running;
        }
    if (!running) o << "(no game with the layer, no capture)\n";
    o << "\n== system\n" << windowsVersion() << "\n";
    o << "\n== gpus\n";
    for (const auto& g : vulkanGpuNames()) o << "deviceName = " << g << "\n";
    o << "\n== layer\n" << (layerRegistered() ? "registered (HKCU/HKLM ImplicitLayers)" : "not registered") << "\n";
    for (const auto& g : injectedGames()) o << "injection (DXVK / VKD3D-Proton) next to: " << narrow(g) << "\n";
    o << "\n== settings (" << platformConfigPath() << ")\n" << readFile(platformConfigPath(), 8000) << "\n";
    const Config c = Config::load("");
    if (!c.logFile.empty()) o << "\n== log (" << c.logFile << ", end)\n" << readFile(platformExpandPath(c.logFile), 12000) << "\n";
    return hideProfile(o.str());
}

std::string issueUrl(const std::string& r) {
    const std::string system = firstLine(section(r, "system"));
    std::string gpu = firstLine(section(r, "gpus"));
    if (gpu.rfind("deviceName = ", 0) == 0) gpu = gpu.substr(13);
    std::string quick = section(r, "lecture rapide").substr(0, 1500);
    const std::string install = firstLine(section(r, "layer"));
    return "https://github.com/Bdexez/bdex_frame_gen/issues/new?template=bug_report.yml&title=" +
           urlencode("[" + system + "] ") + "&version=" + urlencode(BDEX_VERSION_STR) + "&distro=" + urlencode(system) +
           "&gpu=" + urlencode(gpu) + "&session=" + urlencode("Windows launcher (bdex_capture.exe)") +
           "&install=" + urlencode(install) + "&quick=" + urlencode(quick);
}

std::wstring writeReport(std::string& url) {
    wchar_t buf[MAX_PATH];
    const DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH);
    if (!n || n >= MAX_PATH) return L"";
    const fs::path dir = fs::path(std::wstring(buf, n)) / L"bdex-framegen";
    std::error_code ec;
    fs::create_directories(dir, ec);
    const fs::path path = dir / L"bdex-framegen-report.txt";
    const std::string text = buildReport();
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        if (!(out << text)) return L"";
    }
    url = issueUrl(text);
    return path.wstring();
}

} // namespace bdex
