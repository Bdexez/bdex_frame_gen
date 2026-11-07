#include "status.h"
#include "platform.h"

#include <cmath>
#include <chrono>
#include <cstdlib>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <locale>
#include <iomanip>
#include <mutex>
#include <sstream>

namespace bdex {

namespace {

struct Board {
    std::mutex mutex;
    std::map<uint64_t, StatusEntry> entries;
    std::chrono::steady_clock::time_point written{};
    bool dirAttempted = false;
    std::string path;  // "" = unavailable
};

// Never destroyed (like the layer's global tables): games may exit without
// tearing their devices down.
Board& board() {
    static Board* b = new Board();
    return *b;
}

void writeLocked(Board& b) {
    if (!b.dirAttempted) {
        b.dirAttempted = true;
        const std::string dir = platformStatusDir();
        std::error_code ec;
        if (!dir.empty() && (std::filesystem::create_directories(dir, ec) || std::filesystem::is_directory(dir, ec)))
            b.path = dir + "/" + std::to_string(platformPid()) + ".status";
    }
    if (b.path.empty()) return;
    std::error_code ec;
    if (b.entries.empty()) {
        std::filesystem::remove(b.path, ec);
        return;
    }
    const auto names = platformProcessNames();
    const long long now = std::chrono::duration_cast<std::chrono::seconds>(
                              std::chrono::system_clock::now().time_since_epoch()).count();
    const std::string tmp = b.path + ".tmp";
    {
        std::ofstream out(tmp, std::ios::trunc);
        if (!out) return;
        out << statusFormat(names.empty() ? std::string("?") : names.front(), platformPid(), now, b.entries);
    }
    std::filesystem::rename(tmp, b.path, ec);  // atomic for the panel's reader
    b.written = std::chrono::steady_clock::now();
}

} // namespace

// One decimal, always with a dot (snprintf follows the process's locale;
// the panel and the report parse these files). A stream in the classic
// locale: std::to_chars for floating point needs GCC 11 (see config.cpp).
static void fixed1(char* buf, size_t size, double v) {
    std::ostringstream out;
    out.imbue(std::locale::classic());
    out << std::fixed << std::setprecision(1) << v;
    snprintf(buf, size, "%s", out.str().c_str());
}

std::string statusFormat(const std::string& process, long pid, long long unixTime,
                         const std::map<uint64_t, StatusEntry>& entries) {
    std::ostringstream o;
    o << "process=" << process << "\npid=" << pid << "\ntime=" << unixTime << "\n";
    int n = 0;
    for (const auto& [key, e] : entries) {
        o << "\n[" << e.kind << " " << ++n << "]\n";
        o << "state=" << e.state << "\n";
        if (!e.reason.empty()) o << "reason=" << e.reason << "\n";
        if (!e.device.empty()) o << "device=" << e.device << "\n";
        if (!e.antiCheat.empty()) o << "anticheat=" << e.antiCheat << "\n";
        if (e.kind == "device") continue;
        if (!e.target.empty()) o << "target=" << e.target << "\n";
        char buf[64];
        o << "size=" << e.width << "x" << e.height << "\n";
        if (!e.mode.empty()) o << "mode=" << e.mode << "\n";
        o << "multiplier=" << e.multiplier << "\n";
        fixed1(buf, sizeof buf, e.gameFps);
        o << "game_fps=" << buf << "\n";
        fixed1(buf, sizeof buf, e.outFps);
        o << "output_fps=" << buf << "\n";
        fixed1(buf, sizeof buf, e.refreshHz);
        o << "refresh_hz=" << buf << "\n";
        if (e.limitHz > 0.0) {
            fixed1(buf, sizeof buf, e.limitHz);
            o << "limit_hz=" << buf << "\n";
        }
        if (e.displayGrid >= 0) o << "display_grid=" << e.displayGrid << "\n";
        if (e.fpsLimit != 0.0) {
            fixed1(buf, sizeof buf, e.fpsLimit);
            o << "fps_limit=" << (e.fpsLimit < 0.0 ? std::string("auto") : std::string(buf)) << "\n";
            if (!e.capPoint.empty()) o << "cap_point=" << e.capPoint << "\n";
        }
        if (e.autoCapFps > 0.0) {
            fixed1(buf, sizeof buf, e.autoCapFps);
            o << "auto_cap_fps=" << buf << "\n";
        }
        if (e.gpuBusy >= 0) o << "gpu_busy=" << e.gpuBusy << "\n";
        if (e.reflexSimToPresentMs >= 0.0) {
            fixed1(buf, sizeof buf, e.reflexSimToPresentMs);
            o << "reflex_sim_present_ms=" << buf << "\n";
        }
        if (e.reflexSimToGpuMs >= 0.0) {
            fixed1(buf, sizeof buf, e.reflexSimToGpuMs);
            o << "reflex_sim_gpu_ms=" << buf << "\n";
        }
        if (!e.engine.empty()) o << "engine=" << e.engine << "\n";
        fixed1(buf, sizeof buf, e.latencyMs);
        o << "latency_ms=" << buf << "\n";
        if (e.shownFps >= 0.0) {
            fixed1(buf, sizeof buf, e.shownFps);
            o << "shown_fps=" << buf << "\n";
            fixed1(buf, sizeof buf, e.heldPerS);
            o << "held_per_s=" << buf << "\n";
        }
        o << "gpu_budget_level=" << e.budgetLevel << "\n";
        if (e.uncertainPct >= 0.0) {
            fixed1(buf, sizeof buf, e.uncertainPct);
            o << "flow_uncertain_pct=" << buf << "\n";
        }
        if (!e.passes.empty()) o << "passes=" << e.passes << "\n";
        if (!e.tune.empty()) o << "online_tune=" << e.tune << "\n";
    }
    return o.str();
}

StatusFile statusParse(const std::string& text) {
    StatusFile f;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        if (line.front() == '[' && line.back() == ']') {
            StatusFile::Section s;
            s.kind = line.substr(1, line.find(' ') == std::string::npos ? line.size() - 2 : line.find(' ') - 1);
            f.sections.push_back(s);
            continue;
        }
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = line.substr(0, eq), value = line.substr(eq + 1);
        if (!f.sections.empty()) {
            f.sections.back().values[key] = value;
        } else if (key == "process") {
            f.process = value;
        } else if (key == "pid") {
            f.pid = std::strtol(value.c_str(), nullptr, 10);
        } else if (key == "time") {
            f.time = std::strtoll(value.c_str(), nullptr, 10);
        }
    }
    return f;
}

std::vector<std::string> statusDescribe(const StatusFile& file, bool english) {
    std::vector<std::string> out;
    auto T = [english](const char* fr, const char* en) { return english ? en : fr; };
    auto num = [](const std::map<std::string, std::string>& v, const char* k, double def) {
        auto it = v.find(k);
        if (it == v.end()) return def;
        std::istringstream in(it->second);
        in.imbue(std::locale::classic());
        double x = def;
        if (!(in >> x)) x = def;
        return x;
    };
    auto get = [](const std::map<std::string, std::string>& v, const char* k) {
        auto it = v.find(k);
        return it == v.end() ? std::string() : it->second;
    };
    for (const auto& s : file.sections) {
        const auto& v = s.values;
        const std::string state = get(v, "state");
        if (!get(v, "anticheat").empty())
            out.push_back(T("⚠ Anti-triche détecté (", "⚠ Anti-cheat detected (") + get(v, "anticheat") +
                          T(") : génération maintenue, risque de bannissement à tes risques",
                            "): generation kept on, ban risk at your own risk"));
        if (s.kind == "device") {
            if (state != "active")
                out.push_back(T("Couche inactive : ", "Layer inactive: ") + (v.count("reason") ? get(v, "reason") : state));
            continue;
        }
        const std::string who =
            s.kind == "capture" && !get(v, "target").empty() ? get(v, "target") + T(" : ", ": ") : "";
        if (state == "pass-through") {
            out.push_back(who + T("transparente — ", "pass-through — ") +
                          (v.count("reason") ? get(v, "reason") : T("raison inconnue", "unknown reason")));
            continue;
        }
        char buf[160];
        const std::string mode = get(v, "mode");
        snprintf(buf, sizeof buf, T("%.0f → %.0f i/s (×%s, %s)", "%.0f → %.0f fps (×%s, %s)"), num(v, "game_fps", 0),
                 num(v, "output_fps", 0), get(v, "multiplier").c_str(),
                 mode == "extrapolate" ? "extrapolation" : mode == "hybrid" ? T("hybride", "hybrid") : "interpolation");
        std::string line = (state == "paused" ? T("En pause — ", "Paused — ") : "") + who + buf;
        if (num(v, "auto_cap_fps", 0) > 0) {
            snprintf(buf, sizeof buf, T(" · GPU saturé : jeu tenu sous %.0f i/s pour garder la génération fluide",
                                        " · GPU saturated: game held under %.0f fps to keep the generation smooth"),
                     num(v, "auto_cap_fps", 0));
            line += buf;
        }
        if (num(v, "limit_hz", 0) > 0) {
            snprintf(buf, sizeof buf, T(" · limite Steam %.0f i/s (rien au-delà ne s'affiche)",
                                        " · Steam limit %.0f fps (nothing above it is shown)"), num(v, "limit_hz", 0));
            line += buf;
        } else if (num(v, "refresh_hz", 0) > 0 && num(v, "display_grid", -1) == 3) {  // x11_clock
            snprintf(buf, sizeof buf, T(" · écran %.0f Hz (cadence lue auprès du serveur X)", " · screen %.0f Hz (timing read from the X server)"),
                     num(v, "refresh_hz", 0));
            line += buf;
        } else if (num(v, "refresh_hz", 0) > 0) {
            snprintf(buf, sizeof buf, T(" · écran %.0f Hz", " · screen %.0f Hz"), num(v, "refresh_hz", 0));
            line += buf;
        }
        if (num(v, "latency_ms", -1) >= 0) {
            snprintf(buf, sizeof buf, T(" · latence %.0f ms", " · latency %.0f ms"), num(v, "latency_ms", -1));
            line += buf;
        }
        if (num(v, "shown_fps", -1) >= 0) {
            snprintf(buf, sizeof buf, T(" · affichées %.0f/s, tenues %.0f/s", " · shown %.0f/s, held %.0f/s"),
                     num(v, "shown_fps", -1), num(v, "held_per_s", 0));
            line += buf;
            // (fewer frames than refreshes: hz - output of them a second hold
            // a frame whatever the pacing — 30 fps x4 on 144 Hz, 24; as the panel)
            const double hz = num(v, "refresh_hz", 0), outp = num(v, "output_fps", 0);
            if (state == "generating" && hz > 0 && outp > 0.5 * hz && outp < 0.97 * hz && hz - outp >= 1.0) {
                snprintf(buf, sizeof buf, T(" · dont ~%.0f/s inévitables à %.0f i/s sur %.0f Hz", " · of which ~%.0f/s unavoidable at %.0f fps on %.0f Hz"),
                         hz - outp, outp, hz);
                line += buf;
                for (int even : {60, 72, 75, 90, 100, 120, 144, 165})
                    if (std::fabs(outp - even) <= 0.03 * even) {
                        snprintf(buf, sizeof buf, T(" · un écran réglé à %d Hz serait régulier", " · a screen set to %d Hz would be even"), even);
                        line += buf;
                        // (in interpolation the real frame waits m - 1 refreshes:
                        // longer ones at the lower rate — x4 on 120 instead of 144 Hz, ~4 ms)
                        const double game = num(v, "game_fps", 0);
                        const double extra = game > 0 ? (std::round(outp / game) - 1.0) * (1000.0 / even - 1000.0 / hz) : 0.0;
                        if (extra >= 1.0) {
                            snprintf(buf, sizeof buf, T(" (~%.0f ms de latence en plus)", " (~%.0f ms more latency)"), extra);
                            line += buf;
                        }
                        break;
                    }
            }
        }
        if (num(v, "gpu_budget_level", 0) > 0) {
            snprintf(buf, sizeof buf, T(" · budget GPU : niveau %.0f", " · GPU budget: level %.0f"),
                     num(v, "gpu_budget_level", 0));
            line += buf;
        }
        if (get(v, "reason") == "fast")
            line += T(" · génération inutile : le jeu tient déjà la fréquence de l'écran",
                      " · generation not needed: the game already holds the screen's refresh rate");
        if (num(v, "flow_uncertain_pct", -1) >= 0) {
            snprintf(buf, sizeof buf, T(" · flux incertain sur %.0f %% de l'image", " · flow uncertain on %.0f %% of the picture"),
                     num(v, "flow_uncertain_pct", -1));
            line += buf;
        }
        out.push_back(line);
        // x3 and up on a screen whose refresh grid the layer cannot see: the
        // generated frames are paced on the game's interval, not on the
        // refreshes, and land up to a refresh off the moment they depict
        // (simulated, 30 fps on 144 Hz: -2.5 dB and twice the flicker at
        // every multiplier). Capping the game at screen / multiplier puts one
        // frame on each refresh.
        if (state == "generating" && num(v, "multiplier", 0) >= 3 && v.count("display_grid") &&
            num(v, "display_grid", 1) < 1 && num(v, "limit_hz", 0) <= 0 && !v.count("fps_limit"))
            out.push_back(T("   cadence de l'écran non mesurée : hors VRR, en ×3 et plus les images tombent entre deux "
                            "rafraîchissements — « Limite d'images du jeu » sur Auto, ou un écran à un multiple exact "
                            "du jeu (30 i/s ×4 → 120 Hz)",
                            "   screen timing not measured: without VRR, at x3 and above frames fall between "
                            "refreshes — set « Game frame rate cap » to Auto, or the screen to an exact multiple "
                            "of the game (30 fps x4 -> 120 Hz)"));
        // fps_limit holding the present of a Direct3D game under Proton:
        // DXVK and VKD3D-Proton present from a thread of their own and let the
        // game queue frames ahead (its maximum frame latency, 1 to 3), so
        // holding that thread fills the queue and the game reads its input a
        // frame or two before the screen needs it. Its own limiter, Reflex or
        // Anti-Lag, or its present wait (fps_limit_wait: cap_point "wait"),
        // where the layer then holds the game, do not queue.
        {
            std::string eng = get(v, "engine");
            for (char& c : eng) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            // (Unreal, RE Engine: a game thread one frame ahead of the one that
            // presents — held there, it still reads its input a frame early;
            // Reflex's sleep is on the game thread. Measured on such a
            // pipeline at 40 fps: 46.9 ms at the wait, 26.2 at Reflex's
            // sleep, 27.3 with the game's own limiter.)
            if (get(v, "cap_point") == "wait")
                out.push_back(T("   limite d'images retenue dans l'attente du jeu, avant qu'il lise ses entrées : pas de latence ajoutée "
                                "(jeux Unreal, RE Engine : activer NVIDIA Reflex dans le jeu place la limite sur leur fil de "
                                "jeu, une image de latence en moins)",
                                "   frame limit held in the game's wait, before it reads its input: no latency added (Unreal, "
                                "RE Engine games: NVIDIA Reflex on in the game puts the limit on their game thread, a frame of "
                                "latency less)"));
            else if (get(v, "cap_point") == "input")
                out.push_back(T("   limite d'images retenue au sommeil Reflex / Anti-Lag du jeu, avant qu'il lise ses entrées",
                                "   frame limit held at the game's Reflex / Anti-Lag sleep, before it reads its input"));
            if (get(v, "cap_point") == "present" && (eng.find("dxvk") != std::string::npos || eng.find("vkd3d") != std::string::npos))
                out.push_back(T("   limite d'images sur un jeu Direct3D (DXVK / VKD3D) retenu à sa présentation (pilote sans "
                                "present_wait, ou fps_limit_wait coupé) : le jeu garde une à trois images d'avance — la limite "
                                "d'images du jeu (ou Reflex / Anti-Lag activé dans le jeu) est plus réactive",
                                "   frame rate cap on a Direct3D game (DXVK / VKD3D) held at its present (a driver without present "
                                "wait, or fps_limit_wait off): the game keeps one to three frames ahead — the game's own frame "
                                "cap (or Reflex / Anti-Lag on in the game) responds faster"));
        }
        if (!get(v, "passes").empty()) {
            std::string p = get(v, "passes");
            for (size_t i = 0; (i = p.find(',', i)) != std::string::npos; i += 2) p.replace(i, 1, ", ");
            out.push_back(T("   passes : ", "   passes: ") + p);
        }
        if (!get(v, "online_tune").empty()) out.push_back(T("   réglage auto : ", "   auto tuning: ") + get(v, "online_tune"));
    }
    return out;
}

void statusSet(uint64_t key, const StatusEntry& entry) {
    Board& b = board();
    std::lock_guard<std::mutex> lock(b.mutex);
    auto it = b.entries.find(key);
    const bool changed = it == b.entries.end() || it->second.state != entry.state || it->second.reason != entry.reason;
    b.entries[key] = entry;
    if (changed || std::chrono::steady_clock::now() - b.written >= std::chrono::seconds(1)) writeLocked(b);
}

void statusRemove(uint64_t key) {
    Board& b = board();
    std::lock_guard<std::mutex> lock(b.mutex);
    if (b.entries.erase(key)) writeLocked(b);
}

} // namespace bdex
