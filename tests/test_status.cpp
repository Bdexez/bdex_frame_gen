#include "status.h"
#include "platform.h"
#include "test.h"

#include <string>

using namespace bdex;

TEST(status_format_lists_every_entry) {
    std::map<uint64_t, StatusEntry> m;
    StatusEntry dev;
    dev.kind = "device";
    dev.state = "active";
    dev.device = "Some GPU";
    StatusEntry sc;
    sc.kind = "swapchain";
    sc.state = "pass-through";
    sc.reason = "swapchain format 64 not supported";
    sc.width = 800;
    sc.height = 600;
    sc.passes = "hud_mask,subpixel_lk";
    sc.tune = "trying pixel_vectors=1 (2 of 5 decided)";
    m[1] = dev;
    m[2] = sc;
    const std::string s = statusFormat("game", 42, 1000, m);
    CHECK(s.find("process=game\npid=42\ntime=1000\n") == 0);
    CHECK(s.find("[device 1]\nstate=active\ndevice=Some GPU\n") != std::string::npos);
    CHECK(s.find("[swapchain 2]\nstate=pass-through\nreason=swapchain format 64 not supported\n") != std::string::npos);
    CHECK(s.find("size=800x600\n") != std::string::npos);
    CHECK(s.find("latency_ms=-1.0\n") != std::string::npos);
    CHECK(s.find("passes=hud_mask,subpixel_lk\n") != std::string::npos);
    CHECK(s.find("online_tune=trying pixel_vectors=1 (2 of 5 decided)\n") != std::string::npos);
    CHECK(s.find("shown_fps=") == std::string::npos);  // (unknown: not written)
    m[2].shownFps = 143.4;
    m[2].heldPerS = 4.2;
    const std::string c = statusFormat("game", 42, 1000, m);
    CHECK(c.find("shown_fps=143.4\nheld_per_s=4.2\n") != std::string::npos);
}

TEST(status_key_accepts_pointers_and_integers) {
    int x = 0;
    CHECK(statusKey(&x) != 0);
    CHECK(statusKey(uint64_t(7)) == 7);
}

TEST(status_parse_and_describe) {
    std::map<uint64_t, StatusEntry> m;
    StatusEntry c;
    c.kind = "capture";
    c.state = "generating";
    c.target = "skyrimse";
    c.mode = "extrapolate";
    c.multiplier = 2;
    c.gameFps = 60;
    c.outFps = 120;
    c.latencyMs = 21;
    c.passes = "online_tune,hud_mask";
    c.tune = "trying pixel_vectors=1 (2 of 5 decided)";
    StatusEntry p;
    p.kind = "swapchain";
    p.state = "pass-through";
    p.reason = "swapchain format 64 not supported";
    m[1] = c;
    m[2] = p;
    const StatusFile f = statusParse(statusFormat("bdex_capture", 7, 99, m));
    CHECK(f.process == "bdex_capture" && f.pid == 7 && f.time == 99 && f.sections.size() == 2);
    CHECK(f.sections[0].kind == "capture" && f.sections[0].values.at("target") == "skyrimse");
    const auto lines = statusDescribe(f);
    CHECK(lines.size() == 4);
    CHECK(lines[0] == "skyrimse : 60 → 120 i/s (×2, extrapolation) · latence 21 ms");
    CHECK(lines[1] == "   passes : online_tune, hud_mask");
    CHECK(lines[2] == "   réglage auto : trying pixel_vectors=1 (2 of 5 decided)");
    CHECK(lines[3] == "transparente — swapchain format 64 not supported");
    // Steam's framerate limit in gamescope is shown instead of the refresh rate.
    std::map<uint64_t, StatusEntry> lim;
    StatusEntry l;
    l.kind = "swapchain";
    l.state = "generating";
    l.mode = "interpolate";
    l.multiplier = 1;
    l.gameFps = l.outFps = 30;
    l.refreshHz = 30;
    l.limitHz = 30;
    lim[1] = l;
    const std::string limText = statusFormat("game", 1, 1, lim);
    CHECK(limText.find("limit_hz=30.0\n") != std::string::npos);
    CHECK(statusDescribe(statusParse(limText))[0] ==
          "30 → 30 i/s (×1, interpolation) · limite Steam 30 i/s (rien au-delà ne s'affiche)");
    l.limitHz = 0;
    lim[1] = l;
    CHECK(statusFormat("game", 1, 1, lim).find("limit_hz") == std::string::npos);
    // Windows line ends, stray lines.
    const StatusFile g = statusParse("process=x\r\npid=3\r\nnoise\r\n\r\n[device 1]\r\nstate=inactive\r\nreason=disabled\r\n");
    CHECK(g.pid == 3 && statusDescribe(g).size() == 1 && statusDescribe(g)[0] == "Couche inactive : disabled");
}

TEST(status_describe_in_english) {
    std::map<uint64_t, StatusEntry> m;
    StatusEntry c;
    c.kind = "swapchain";
    c.state = "paused";
    c.mode = "hybrid";
    c.multiplier = 3;
    c.gameFps = 40;
    c.outFps = 120;
    c.refreshHz = 120;
    m[1] = c;
    const StatusFile f = statusParse(statusFormat("game", 7, 99, m));
    const auto en = statusDescribe(f, true), fr = statusDescribe(f);
    CHECK(!en.empty() && en[0].find("Paused — 40 → 120 fps (×3, hybrid) · screen 120 Hz") == 0);
    CHECK(!fr.empty() && fr[0].find("En pause — 40 → 120 i/s (×3, hybride)") == 0);
}

TEST(status_screen_timing_hint) {
    // x3 and up without a measured refresh grid: the hint to cap the game,
    // gone once a cap is set, at x2, or with the grid known.
    std::map<uint64_t, StatusEntry> m;
    StatusEntry c;
    c.kind = "swapchain";
    c.state = "generating";
    c.mode = "interpolate";
    c.multiplier = 4;
    c.gameFps = 30;
    c.outFps = 120;
    c.refreshHz = 144;
    c.displayGrid = 0;
    m[1] = c;
    auto lines = [&]() { return statusDescribe(statusParse(statusFormat("game", 1, 1, m))); };
    CHECK(lines().size() == 2 && lines()[1].find("cadence de l'écran non mesurée") != std::string::npos);
    m[1].fpsLimit = -1.0;
    CHECK(statusFormat("game", 1, 1, m).find("fps_limit=auto") != std::string::npos && lines().size() == 1);
    m[1].fpsLimit = 0.0;
    m[1].displayGrid = 1;
    CHECK(lines().size() == 1);
    m[1].displayGrid = 0;
    m[1].multiplier = 2;
    CHECK(lines().size() == 1);
    m[1].displayGrid = -1;  // not known: no line at all in the file
    CHECK(statusFormat("game", 1, 1, m).find("display_grid") == std::string::npos);
    // fps_limit holding the present of a DXVK / VKD3D game: the queue hint;
    // not when the game's Reflex / Anti-Lag sleep holds it.
    m[1].fpsLimit = 40.0;
    m[1].capPoint = "present";
    m[1].engine = "vkd3d";
    CHECK(lines().size() == 2 && lines()[1].find("Direct3D") != std::string::npos);
    m[1].capPoint = "input";
    CHECK(lines().size() == 1);
    m[1].capPoint = "present";
    m[1].engine = "Unreal Engine";
    CHECK(lines().size() == 1);
}

TEST(status_anticheat_warning) {
    // The layer kept on beside an anti-cheat (ignore_anticheat, Rocket
    // League on Windows): the warning travels with the device entry.
    std::map<uint64_t, StatusEntry> m;
    StatusEntry d;
    d.kind = "device";
    d.state = "active";
    d.device = "GPU";
    d.antiCheat = "easyanticheat";
    m[1] = d;
    const std::string text = statusFormat("rocketleague", 5, 1, m);
    CHECK(text.find("anticheat=easyanticheat\n") != std::string::npos);
    const auto fr = statusDescribe(statusParse(text));
    CHECK(fr.size() == 1 && fr[0].find("Anti-triche détecté (easyanticheat)") != std::string::npos);
    d.antiCheat.clear();
    m[1] = d;
    CHECK(statusFormat("game", 5, 1, m).find("anticheat") == std::string::npos);
    // Only Windows keeps injection on in Rocket League.
#ifdef _WIN32
    CHECK(platformAntiCheatException("rocketleague"));
#else
    CHECK(!platformAntiCheatException("rocketleague"));
#endif
    CHECK(!platformAntiCheatException("cs2"));
}
